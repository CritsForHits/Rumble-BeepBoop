#include <obs-module.h>
#include <obs-frontend-api.h>
#include "beepboop.h"
#include <graphics/graphics.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <cstdio>
#include <deque>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("BeepBoop", "en-US")

using json = nlohmann::json;

static constexpr uint32_t SOURCE_WIDTH = 1920;
static constexpr uint32_t SOURCE_HEIGHT = 1080;
static constexpr uint32_t MESSAGE_HEIGHT = 36;
static constexpr int SSE_TIMEOUT_SECONDS = 25;
static constexpr int SSE_RETRY_SECONDS = 5;

struct chat_message {
    std::string display;
    std::chrono::steady_clock::time_point received_at;
    std::vector<obs_source_t *> badge_sources;
    obs_source_t *username_source = nullptr;
    obs_source_t *text_source = nullptr;
};

struct pending_message {
    std::string username;
    std::string text;
    std::string color;
    std::vector<std::string> badges;
};

struct beep_boop_data {
    obs_source_t *source = nullptr;
    obs_source_t *status_source = nullptr;
    beep_boop_alerts_data alerts;
    beep_boop_goals_data goals;
    beep_boop_chatterbox_data chatterbox;
    std::string api_url;
    std::string stream_id;
    bool rant_snapshot_initialized = false;
    std::string last_rant_key;
    // Per-stream aggregate like totals; there is no per-user like event data.
    std::unordered_map<std::string, uint64_t> like_counts;
    bool follower_history_seeded = false;
    std::unordered_set<std::string> seen_follower_events;
    std::atomic<bool> repeat_follower_protection{true};

    std::mutex messages_mutex;
    std::mutex queue_mutex;
    std::queue<pending_message> pending_messages;

    std::atomic<bool> active{true};
    std::atomic<int> max_messages{4};
    std::atomic<int> message_timeout{30};
    std::atomic<bool> chat_enabled{true};
    std::atomic<bool> reverse_stack{false};
    std::atomic<bool> chat_placeholder{true};
    std::atomic<bool> reconnect_requested{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> api_connected{false};
    std::atomic<bool> status_refresh_requested{false};
    std::mutex api_refresh_mutex;
    std::mutex api_snapshot_mutex;
    std::chrono::steady_clock::time_point last_api_refresh{};
    int chat_x_percent = 0;
    int chat_y_percent = 95;
    int font_size = 32;
    std::string font_face = "Times New Roman";
    std::thread worker;
    std::thread api_poll_worker;
    std::condition_variable stop_cv;
    std::mutex stop_mutex;

    struct user_info {
        std::string username = "Unknown";
        std::string color = "#FFFFFF";
        std::vector<std::string> badges;
    };
    std::mutex users_mutex;
    std::unordered_map<std::string, user_info> users;
    std::unordered_set<std::string> seen_messages;
    std::vector<chat_message> messages;
};

static void refresh_api_snapshot_from_sse(beep_boop_data *bb);

struct HttpResponse {
    std::string body;
};

struct SseContext {
    beep_boop_data *bb = nullptr;
    std::string buffer;
    bool got_event = false;
    bool got_init = false;
};

static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *user_data) {
    auto *res = static_cast<HttpResponse *>(user_data);
    res->body.append(ptr, size * nmemb);
    return size * nmemb;
}

static uint32_t parse_color(const std::string &color) {
    if (color.size() != 7 || color[0] != '#') return 0xFFFFFFFF;
    try {
        uint32_t r = std::stoul(color.substr(1, 2), nullptr, 16);
        uint32_t g = std::stoul(color.substr(3, 2), nullptr, 16);
        uint32_t b = std::stoul(color.substr(5, 2), nullptr, 16);
        return 0xFF000000 | (b << 16) | (g << 8) | r;
    } catch (...) { return 0xFFFFFFFF; }
}

static obs_source_t *create_text_source(const char *name, const std::string &text, uint32_t color, int font_size = 32, const std::string &font_face = "Times New Roman") {
    obs_data_t *settings = obs_data_create();
    obs_data_t *font = obs_data_create();
    obs_data_set_string(font, "face", font_face.c_str());
    obs_data_set_int(font, "size", font_size);
    obs_data_set_obj(settings, "font", font);
    obs_data_release(font);
    obs_data_set_string(settings, "text", text.c_str());
    obs_data_set_string(settings, "align", "left");
    obs_data_set_int(settings, "color1", color);
    obs_data_set_int(settings, "color2", color);
    obs_data_set_bool(settings, "outline", true);
    obs_source_t *src = obs_source_create_private("text_gdiplus", name, settings);
    obs_data_release(settings);
    return src;
}

static void set_text_font(obs_source_t *src, const std::string &face, int size) {
    if (!src) return;
    obs_data_t *settings = obs_source_get_settings(src);
    obs_data_t *font = obs_data_get_obj(settings, "font");
    if (font) {
        obs_data_set_string(font, "face", face.c_str());
        obs_data_set_int(font, "size", size);
        obs_data_set_obj(settings, "font", font);
        obs_source_update(src, settings);
        obs_data_release(font);
    }
    obs_data_release(settings);
}

static void set_text_opacity(obs_source_t *src, uint8_t alpha) {
    if (!src) return;
    obs_data_t *settings = obs_source_get_settings(src);
    for (const char *key : {"color1", "color2"}) {
        uint32_t color = (uint32_t)obs_data_get_int(settings, key);
        color = (color & 0x00FFFFFFu) | ((uint32_t)alpha << 24);
        obs_data_set_int(settings, key, color);
    }
    obs_source_update(src, settings);
    obs_data_release(settings);
}

static void apply_message_opacity(beep_boop_data *bb) {
    float opacity = 255.0f;
    for (auto &m : bb->messages) {
        uint8_t alpha = (uint8_t)std::lround(opacity);
        for (auto *badge : m.badge_sources) set_text_opacity(badge, alpha);
        set_text_opacity(m.username_source, alpha);
        set_text_opacity(m.text_source, alpha);
        opacity *= 0.8f;
    }
}

static void release_message(chat_message &msg) {
    for (auto *badge : msg.badge_sources)
        if (badge) obs_source_release(badge);
    if (msg.username_source) obs_source_release(msg.username_source);
    if (msg.text_source) obs_source_release(msg.text_source);
    msg.badge_sources.clear();
    msg.username_source = msg.text_source = nullptr;
}

static void prune_messages(beep_boop_data *bb) {
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(bb->message_timeout.load());
    size_t limit = static_cast<size_t>(bb->max_messages.load());
    bool changed = false;
    for (auto it = bb->messages.begin(); it != bb->messages.end();) {
        if (now - it->received_at >= timeout || static_cast<size_t>(it - bb->messages.begin()) >= limit) {
            release_message(*it);
            it = bb->messages.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) apply_message_opacity(bb);
}

static std::string format_badges(const std::vector<std::string> &badges) {
    std::string result;
    for (const auto &badge : badges) {
        const auto style = beep_boop_badge_style_for(badge);
        if (!style.glyph.empty()) result += style.glyph + " ";
    }
    return result;
}

static std::string wrap_message(const std::string &text, size_t limit = 50) {
    std::istringstream words(text);
    std::string word, line, result;
    while (words >> word) {
        if (line.empty()) {
            line = word;
        } else if (line.size() + 1 + word.size() <= limit) {
            line += " " + word;
        } else {
            if (!result.empty()) result += '\n';
            result += line;
            line = word;
        }
    }
    if (!line.empty()) {
        if (!result.empty()) result += '\n';
        result += line;
    }
    return result;
}

static void add_message(beep_boop_data *bb, const std::string &user, const std::string &text, const std::string &color, const std::vector<std::string> &badges) {
    if (!bb || text.empty() || !bb->chat_enabled.load()) return;
    std::string safe_user = user.empty() ? "Unknown" : user;

    chatterbox_speak_chat(&bb->chatterbox, safe_user, text);

    std::lock_guard<std::mutex> lock(bb->queue_mutex);
    bb->pending_messages.push({safe_user, text, color, badges});
}

static bool fetch_api(const std::string &url, json &result) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    HttpResponse res;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status < 200 || status >= 300) return false;
    try {
        result = json::parse(res.body);
        return true;
    } catch (...) { return false; }
}

static void dispatch_alert_event(beep_boop_data *bb, const json &event) {
    if (!bb) return;
    std::vector<std::string> candidates;
    auto add_type = [&](const json &obj, const char *key) {
        if (obj.contains(key) && obj[key].is_string())
            candidates.push_back(obj[key].get<std::string>());
    };
    add_type(event, "type");
    add_type(event, "_sse_event");
    if (event.contains("data") && event["data"].is_object()) {
        const auto &data = event["data"];
        add_type(data, "type");
        add_type(data, "event_type");
        add_type(data, "event");
    }
    for (auto candidate : candidates) {
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (candidate.find("init") != std::string::npos) continue;
        if (candidate.find("raid") != std::string::npos) {
            alerts_play(&bb->alerts, ALERT_RAID);
            return;
        }
    }
}

static void process_sse_event(beep_boop_data *bb, const json &event) {
    dispatch_alert_event(bb, event);
    if (!event.contains("data") || !event["data"].is_object()) return;
    const auto &data = event["data"];
    if (data.contains("users") && data["users"].is_array()) {
        for (const auto &u : data["users"]) {
            try {
                std::string uid = u.contains("id") ? (u["id"].is_string() ? u["id"].get<std::string>() : u["id"].dump()) : "";
                if (uid.empty()) continue;

                beep_boop_data::user_info info;
                info.username = u.value("username", std::string("Unknown"));
                info.color = u.value("color", std::string("#FFFFFF"));
                if (u.contains("badges") && u["badges"].is_array()) {
                    for (const auto &badge : u["badges"]) {
                        if (badge.is_string()) info.badges.push_back(badge.get<std::string>());
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(bb->users_mutex);
                    bb->users[uid] = std::move(info);
                }
            } catch (...) {}
        }
    }
    if (!data.contains("messages") || !data["messages"].is_array()) return;
    const bool parent_is_init = event.value("type", std::string()) == "init" ||
        event.value("_sse_event", std::string()) == "init";
    for (const auto &item : data["messages"]) {
        if (!parent_is_init) dispatch_alert_event(bb, item);
        std::string id = item.value("id", "");
        std::string uid = item.contains("user_id") && !item["user_id"].is_null()
            ? (item["user_id"].is_string() ? item["user_id"].get<std::string>() : item["user_id"].dump())
            : "";
        std::string text = item.value("text", "");
        if (text.empty()) continue;
        std::string key = id.empty() ? uid + ":" + text : id;
        if (parent_is_init) {
            bb->seen_messages.insert(key);
            continue;
        }
        if (!bb->seen_messages.insert(key).second) continue;

        beep_boop_data::user_info info;
        {
            std::lock_guard<std::mutex> lock(bb->users_mutex);
            if (const auto it = bb->users.find(uid); it != bb->users.end()) info = it->second;
        }
        add_message(bb, info.username, text, info.color, info.badges);
    }
}

static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *user_data) {
    auto *ctx = static_cast<SseContext *>(user_data);
    ctx->buffer.append(ptr, size * nmemb);
    size_t pos;
    while ((pos = ctx->buffer.find("\n\n")) != std::string::npos) {
        std::string block = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 2);
        size_t dpos = block.find("data:");
        if (dpos != std::string::npos) {
            try {
                std::string raw_json = block.substr(dpos + 5);
                json ev = json::parse(raw_json);
                size_t epos = block.find("event:");
                if (epos != std::string::npos) {
                    size_t begin = epos + 6;
                    while (begin < block.size() && block[begin] == ' ') ++begin;
                    size_t finish = block.find('\n', begin);
                    ev["_sse_event"] = block.substr(begin, finish == std::string::npos ? finish : finish - begin);
                }
                process_sse_event(ctx->bb, ev);
                refresh_api_snapshot_from_sse(ctx->bb);
                if (ev.value("type", "") == "init") ctx->got_init = true;
                else ctx->got_event = true;
            } catch (...) {}
        }
    }
    return size * nmemb;
}

static size_t sse_header_cb(char *ptr, size_t size, size_t nmemb, void *user_data) {
    const size_t bytes = size * nmemb;
    auto *bb = static_cast<beep_boop_data *>(user_data);
    if (bb && bytes >= 12 && std::string(ptr, bytes).rfind("HTTP/", 0) == 0) {
        const std::string status_line(ptr, bytes);
        bb->connected.store(status_line.find(" 200") != std::string::npos);
    }
    return bytes;
}

static bool connect_sse(beep_boop_data *bb, const std::string &stream_id) {
    try {
        uint64_t cid = std::stoull(stream_id, nullptr, 36);
        std::string url = "https://web7.rumble.com/chat/api/chat/" + std::to_string(cid) + "/stream";
        CURL *curl = curl_easy_init();
        if (!curl) return false;

        SseContext ctx{bb, "", false, false};
        struct curl_slist *headers = curl_slist_append(nullptr, "Accept: text/event-stream");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sse_header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, bb);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)SSE_TIMEOUT_SECONDS);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            auto *c = static_cast<SseContext *>(clientp);
            return (c && c->bb && c->bb->active.load() && !c->bb->reconnect_requested.load()) ? 0 : 1;
        });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_perform(curl);
        bb->connected.store(false);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return ctx.got_init || ctx.got_event;
    } catch (...) { return false; }
}

static uint64_t json_count(const json &obj, const char *key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) return 0;
    try {
        if (obj[key].is_number_unsigned()) return obj[key].get<uint64_t>();
        if (obj[key].is_number_integer()) return static_cast<uint64_t>(std::max<int64_t>(0, obj[key].get<int64_t>()));
        if (obj[key].is_number()) return static_cast<uint64_t>(std::max(0.0, obj[key].get<double>()));
    } catch (...) {}
    return 0;
}

static int64_t parse_iso8601(const std::string &value) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char zone = 0;
    int offset_hour = 0, offset_minute = 0;
    if (std::sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c%2d:%2d",
                    &year, &month, &day, &hour, &minute, &second,
                    &zone, &offset_hour, &offset_minute) < 6) return 0;
    std::tm tm{};
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second;
#ifdef _WIN32
    std::time_t epoch = _mkgmtime(&tm);
#else
    std::time_t epoch = timegm(&tm);
#endif
    if (zone == '+' || zone == '-') {
        const int offset = offset_hour * 3600 + offset_minute * 60;
        epoch += (zone == '+' ? -offset : offset);
    }
    return static_cast<int64_t>(epoch);
}

static std::vector<int64_t> api_event_times(const json &obj, const char *array_key, const char *time_key) {
    std::vector<int64_t> times;
    if (!obj.is_object() || !obj.contains(array_key) || !obj[array_key].is_array()) return times;
    for (const auto &entry : obj[array_key]) {
        if (!entry.is_object() || !entry.contains(time_key) || !entry[time_key].is_string()) continue;
        const int64_t stamp = parse_iso8601(entry[time_key].get<std::string>());
        if (stamp > 0) times.push_back(stamp);
    }
    return times;
}

static std::vector<std::pair<std::string, int64_t>> api_follower_events(const json &obj) {
    std::vector<std::pair<std::string, int64_t>> events;
    if (!obj.is_object() || !obj.contains("recent_followers") || !obj["recent_followers"].is_array()) return events;
    for (const auto &entry : obj["recent_followers"]) {
        if (!entry.is_object()) continue;
        const std::string username = entry.value("username", std::string());
        const std::string followed_on = entry.value("followed_on", std::string());
        const int64_t stamp = parse_iso8601(followed_on);
        if (!username.empty() && stamp > 0) events.emplace_back(username, stamp);
    }
    return events;
}

static void set_api_connected(beep_boop_data *bb, bool connected, bool force_refresh = false) {
    if (!bb) return;
    const bool previous = bb->api_connected.exchange(connected);
    if ((previous != connected || force_refresh) && bb->source)
        obs_source_update_properties(bb->source);
}

static uint32_t api_user_color(const std::string &color) {
    if (color.size() != 7 || color[0] != '#') return 0xFFFFFFFF;
    try {
        const uint32_t r = std::stoul(color.substr(1, 2), nullptr, 16);
        const uint32_t g = std::stoul(color.substr(3, 2), nullptr, 16);
        const uint32_t b = std::stoul(color.substr(5, 2), nullptr, 16);
        return 0xFF000000 | (b << 16) | (g << 8) | r;
    } catch (...) { return 0xFFFFFFFF; }
}

static void set_alert_user_label(beep_boop_data *bb, const json &event_user, const std::string &prefix, const std::string &suffix) {
    if (!bb) return;
    std::string username = event_user.is_object() ? event_user.value("username", std::string("Unknown")) : "Unknown";
    std::string color = event_user.is_object() ? event_user.value("color", std::string()) : std::string();
    std::vector<std::string> badges;
    if (event_user.is_object() && event_user.contains("badges") && event_user["badges"].is_array()) {
        for (const auto &badge : event_user["badges"])
            if (badge.is_string()) badges.push_back(badge.get<std::string>());
    }
    alerts_set_user_label(&bb->alerts, prefix, username, suffix, api_user_color(color.empty() ? "#FFFFFF" : color), badges);
}

static void sync_api_goal_totals(beep_boop_data *bb, const json &root) {
    if (!bb || !root.is_object()) return;
    std::lock_guard<std::mutex> snapshot_lock(bb->api_snapshot_mutex);
    uint64_t delta = 0;
    if (root.contains("followers") && root["followers"].is_object()) {
        const auto &followers = root["followers"];
        const uint64_t total = json_count(followers, "num_followers");
        const auto times = api_event_times(followers, "recent_followers", "followed_on");
        const auto recent_events = api_follower_events(followers);
        if (followers.contains("num_followers")) {
            delta = goals_sync_api_snapshot(&bb->goals, GOAL_FOLLOWERS, total, times, recent_events);
            if (!bb->follower_history_seeded) {
                for (const auto &event : recent_events)
                    bb->seen_follower_events.insert(event.first + "|" + std::to_string(event.second));
                bb->follower_history_seeded = true;
            } else if (delta > 0 && alerts_play(&bb->alerts, ALERT_FOLLOWER)) {
                const json latest = (followers.contains("latest_follower") && followers["latest_follower"].is_object())
                    ? followers["latest_follower"] : json::object();
                set_alert_user_label(bb, latest, "", " has Followed!");
            }
        }
    }
    if (root.contains("subscribers") && root["subscribers"].is_object()) {
        const auto &subs = root["subscribers"];
        const uint64_t total = json_count(subs, "num_subscribers");
        const auto times = api_event_times(subs, "recent_subscribers", "subscribed_on");
        if (subs.contains("num_subscribers")) {
            delta = goals_sync_api_snapshot(&bb->goals, GOAL_SUBSCRIBERS, total, times);
            if (delta > 0 && alerts_play(&bb->alerts, ALERT_SUBSCRIBER)) {
                const json latest = (subs.contains("latest_subscriber") && subs["latest_subscriber"].is_object())
                    ? subs["latest_subscriber"] : json::object();
                set_alert_user_label(bb, latest, "", " has Subbed!");
            }
        }
    }

    // Likes VFX: compare aggregate like totals for each stream. The first
    // snapshot establishes a baseline; later increases trigger one alert.
    if (root.contains("livestreams") && root["livestreams"].is_array()) {
        for (const auto &stream : root["livestreams"]) {
            if (!stream.is_object() || !stream.contains("likes")) continue;

            std::string stream_key = "default";
            if (stream.contains("stream_key") && !stream["stream_key"].is_null()) {
                stream_key = stream["stream_key"].is_string()
                    ? stream["stream_key"].get<std::string>()
                    : stream["stream_key"].dump();
            } else if (stream.contains("id") && !stream["id"].is_null()) {
                stream_key = stream["id"].is_string()
                    ? stream["id"].get<std::string>()
                    : stream["id"].dump();
            }
            if (stream_key.empty()) stream_key = "default";

            const uint64_t new_likes = json_count(stream, "likes");
            const auto result = bb->like_counts.emplace(stream_key, new_likes);
            if (result.second) continue; // Initial value: do not alert.

            const uint64_t old_likes = result.first->second;
            result.first->second = new_likes;
            if (new_likes > old_likes) {
                alerts_play(&bb->alerts, ALERT_LIKES);
            }
        }
    }

    // Rant Alert Handling matching exact required format: UserA Rants: "Hello World" on testing
    if (root.contains("livestreams") && root["livestreams"].is_array()) {
        for (const auto &stream : root["livestreams"]) {
            if (!stream.is_object() || !stream.contains("chat") || !stream["chat"].is_object()) continue;
            const auto &chat = stream["chat"];
            if (!chat.contains("latest_rant") || !chat["latest_rant"].is_object()) continue;
            const auto &rant = chat["latest_rant"];
            const std::string created = rant.value("created_on", std::string());
            const std::string username = rant.value("username", std::string("Unknown"));
            const std::string rant_text = rant.value("text", std::string());
            const std::string key = created + "|" + username + "|" + rant_text;
            if (!key.empty()) {
                if (bb->rant_snapshot_initialized && key != bb->last_rant_key) {
                    if (alerts_play(&bb->alerts, ALERT_RANT)) {
                        std::string formatted_rant = username + " Rants: \"" + rant_text + "\" on testing";
                        alerts_set_label(&bb->alerts, formatted_rant);
                        
                        chatterbox_speak_rant(&bb->chatterbox, username, rant_text);
                    }
                }
                bb->last_rant_key = key;
                bb->rant_snapshot_initialized = true;
            }
            break;
        }
    }
}

static void refresh_api_snapshot_from_sse(beep_boop_data *bb) {
    if (!bb || bb->api_url.empty()) return;
    {
        std::lock_guard<std::mutex> lock(bb->api_refresh_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (bb->last_api_refresh.time_since_epoch().count() != 0 &&
            now - bb->last_api_refresh < std::chrono::seconds(1)) return;
        bb->last_api_refresh = now;
    }
    json root;
    const bool ok = fetch_api(bb->api_url, root);
    set_api_connected(bb, ok, bb->status_refresh_requested.exchange(false));
    if (ok) sync_api_goal_totals(bb, root);
}

static void api_poll_worker_thread(beep_boop_data *bb) {
    constexpr int API_POLL_INTERVAL_SECONDS = 15;
    while (bb->active.load()) {
        for (int elapsed = 0; elapsed < API_POLL_INTERVAL_SECONDS && bb->active.load(); ++elapsed)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!bb->active.load()) break;
        refresh_api_snapshot_from_sse(bb);
    }
}

static void worker_thread(beep_boop_data *bb) {
    while (bb->active.load()) {
        if (bb->reconnect_requested.exchange(false)) {
            bb->connected.store(false);
            bb->stream_id.clear();
        }
        if (bb->stream_id.empty()) {
            json root;
            if (fetch_api(bb->api_url, root)) {
                set_api_connected(bb, true, bb->status_refresh_requested.exchange(false));
                sync_api_goal_totals(bb, root);
            } else {
                set_api_connected(bb, false, bb->status_refresh_requested.exchange(false));
            }
            if (root.contains("livestreams") && !root["livestreams"].empty()) {
                bb->stream_id = root["livestreams"][0].value("id", "");
            } else {
                bb->connected.store(false);
            }
        }
        if (!bb->stream_id.empty()) {
            if (!connect_sse(bb, bb->stream_id)) {
                if (bb->reconnect_requested.exchange(false)) {
                    bb->stream_id.clear();
                    continue;
                }
                std::unique_lock<std::mutex> lk(bb->stop_mutex);
                if (bb->stop_cv.wait_for(lk, std::chrono::seconds(SSE_RETRY_SECONDS), [bb]{return !bb->active.load() || bb->reconnect_requested.load();})) {
                    if (!bb->active.load()) break;
                    bb->reconnect_requested.store(false);
                    bb->stream_id.clear();
                }
            }
        } else {
            std::unique_lock<std::mutex> lk(bb->stop_mutex);
            if (bb->stop_cv.wait_for(lk, std::chrono::seconds(2), [bb]{return !bb->active.load() || bb->reconnect_requested.load();})) {
                if (!bb->active.load()) break;
                bb->reconnect_requested.store(false);
            }
        }
    }
}

static void render_source_at(obs_source_t *source, float x, float y) {
    if (!source) return;
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    obs_source_video_render(source);
    gs_matrix_pop();
}

static void beep_boop_render(void *data, gs_effect_t *) {
    auto *bb = static_cast<beep_boop_data *>(data);
    if (!bb) return;

    {
        std::lock_guard<std::mutex> qlock(bb->queue_mutex);
        std::lock_guard<std::mutex> mlock(bb->messages_mutex);
        while (!bb->pending_messages.empty()) {
            auto pm = std::move(bb->pending_messages.front());
            bb->pending_messages.pop();

            std::string badge_text = format_badges(pm.badges);
            std::string username_text = pm.username + ": ";
            std::string display = badge_text + username_text + pm.text;

            if (!bb->messages.empty() && bb->messages.front().display == display)
                continue;

            prune_messages(bb);
            chat_message entry;
            entry.display = display;
            entry.received_at = std::chrono::steady_clock::now();

            for (size_t i = 0; i < pm.badges.size(); ++i) {
                const auto style = beep_boop_badge_style_for(pm.badges[i]);
                if (style.glyph.empty()) continue;
                const std::string source_name = "BeepBoop Badge " + std::to_string(i + 1);
                if (auto *badge = create_text_source(source_name.c_str(), style.glyph + " ",
                                                     style.color, bb->font_size, bb->font_face))
                    entry.badge_sources.push_back(badge);
            }
            entry.username_source = create_text_source("BeepBoop Username", username_text, parse_color(pm.color), bb->font_size, bb->font_face);
            entry.text_source = create_text_source("BeepBoop Text", wrap_message(pm.text), 0xFFFFFFFF, bb->font_size, bb->font_face);

            if (!entry.username_source || !entry.text_source) {
                release_message(entry);
                continue;
            }
            bb->messages.insert(bb->messages.begin(), std::move(entry));
            prune_messages(bb);
            apply_message_opacity(bb);
        }
    }

    alerts_video_render(&bb->alerts);
    goals_render(&bb->goals);

    if (!bb->chat_enabled.load()) return;

    obs_video_info video_info = {};
    uint32_t canvas_width = SOURCE_WIDTH;
    uint32_t canvas_height = SOURCE_HEIGHT;
    if (obs_get_video_info(&video_info)) {
        canvas_width = video_info.base_width;
        canvas_height = video_info.base_height;
    }

    const float chat_x = bb->chat_x_percent * canvas_width / 100.0f;
    const float chat_y = bb->chat_y_percent * canvas_height / 100.0f;

    std::lock_guard<std::mutex> lock(bb->messages_mutex);
    prune_messages(bb);

    if (bb->messages.empty()) {
        if (bb->chat_placeholder.load())
            render_source_at(bb->status_source, chat_x, chat_y);
        return;
    }

    const float direction = bb->reverse_stack.load() ? 1.0f : -1.0f;
    float y = chat_y;
    for (auto it = bb->messages.begin(); it != bb->messages.end(); ++it) {
        float x = chat_x;
        for (auto *badge : it->badge_sources) {
            render_source_at(badge, x, y);
            x += static_cast<float>(obs_source_get_width(badge));
        }
        if (it->username_source) {
            render_source_at(it->username_source, x, y);
            x += static_cast<float>(obs_source_get_width(it->username_source));
        }
        if (it->text_source) render_source_at(it->text_source, x, y);
        y += direction * std::max(MESSAGE_HEIGHT,
            it->text_source ? obs_source_get_height(it->text_source) : MESSAGE_HEIGHT);
    }
}

static void *beep_boop_create(obs_data_t *settings, obs_source_t *source) {
    auto *bb = new beep_boop_data();
    bb->source = source;
    bb->api_url = obs_data_get_string(settings, "api_url");
    bb->max_messages = std::clamp((int)obs_data_get_int(settings, "max_messages"), 1, 10);
    bb->message_timeout = std::clamp((int)obs_data_get_int(settings, "message_timeout"), 5, 120);
    bb->chat_enabled = obs_data_get_bool(settings, "enable_chat");
    bb->reverse_stack = obs_data_get_bool(settings, "reverse_stack");
    bb->chat_placeholder = obs_data_get_bool(settings, "chat_placeholder");
    bb->chat_x_percent = std::clamp((int)obs_data_get_int(settings, "chat_x_percent"), 0, 100);
    bb->chat_y_percent = std::clamp((int)obs_data_get_int(settings, "chat_y_percent"), 0, 100);
    bb->repeat_follower_protection.store(obs_data_get_bool(settings, "repeat_follower_protection"));
    bb->font_size = std::clamp((int)obs_data_get_int(settings, "font_size"), 8, 256);
    bb->font_face = obs_data_get_string(settings, "font_face");
    
    alerts_init(&bb->alerts, source, settings);
    goals_init(&bb->goals, source, settings, bb->font_size, bb->font_face);
    chatterbox_init(&bb->chatterbox, source, settings);

    bb->status_source = create_text_source("BeepBoop Status", "No Recent Chat Messages...", 0xFFFFFFFF, bb->font_size, bb->font_face);
    bb->worker = std::thread(worker_thread, bb);
    bb->api_poll_worker = std::thread(api_poll_worker_thread, bb);
    return bb;
}

static void beep_boop_destroy(void *data) {
    auto *bb = static_cast<beep_boop_data *>(data);
    if (!bb) return;
    bb->active.store(false);
    bb->stop_cv.notify_all();
    if (bb->worker.joinable()) bb->worker.join();
    if (bb->api_poll_worker.joinable()) bb->api_poll_worker.join();

    alerts_destroy(&bb->alerts);
    goals_destroy(&bb->goals);
    chatterbox_destroy(&bb->chatterbox);

    {
        std::lock_guard<std::mutex> lock(bb->messages_mutex);
        for (auto &m : bb->messages) release_message(m);
    }
    if (bb->status_source) obs_source_release(bb->status_source);
    delete bb;
}

static uint32_t beep_boop_width(void *) {
    obs_video_info video_info = {};
    return obs_get_video_info(&video_info) ? video_info.base_width : SOURCE_WIDTH;
}

static uint32_t beep_boop_height(void *) {
    obs_video_info video_info = {};
    return obs_get_video_info(&video_info) ? video_info.base_height : SOURCE_HEIGHT;
}

static void beep_boop_enum_sources(void *data, obs_source_enum_proc_t cb, void *param) {
    auto *bb = static_cast<beep_boop_data *>(data);
    if (!bb) return;
    std::lock_guard<std::mutex> lock(bb->messages_mutex);
    if (bb->status_source) cb(bb->source, bb->status_source, param);
    for (const auto &m : bb->messages) {
        for (auto *badge : m.badge_sources)
            if (badge) cb(bb->source, badge, param);
        if (m.username_source) cb(bb->source, m.username_source, param);
        if (m.text_source) cb(bb->source, m.text_source, param);
    }
    alerts_enum_sources(&bb->alerts, cb, param);
    goals_enum_sources(&bb->goals, cb, param);
}

static bool beep_boop_audio_render(void *data, uint64_t *ts_out, obs_source_audio_mix *audio_output, uint32_t mixers, size_t channels, size_t sample_rate) {
    auto *bb = static_cast<beep_boop_data *>(data);
    if (!bb) return false;
    
    bool alerts_rendered = alerts_audio_render(&bb->alerts, ts_out, audio_output, mixers, channels, sample_rate);
    bool chatterbox_rendered = chatterbox_audio_render(&bb->chatterbox, ts_out, audio_output, mixers, channels, sample_rate);
    
    return alerts_rendered || chatterbox_rendered;
}

bool obs_module_load(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    obs_source_info info = {};
    info.id = "beep_boop_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE;
    info.icon_type = OBS_ICON_TYPE_SLIDESHOW;
    info.get_name = [](void*) { return "Rumble-BB"; };
    info.create = beep_boop_create;
    info.destroy = beep_boop_destroy;
    info.update = [](void* data, obs_data_t* s) {
        auto *bb = static_cast<beep_boop_data *>(data);
        bb->api_url = obs_data_get_string(s, "api_url");
        bb->max_messages = std::clamp((int)obs_data_get_int(s, "max_messages"), 1, 10);
        bb->message_timeout = std::clamp((int)obs_data_get_int(s, "message_timeout"), 5, 120);
        bb->chat_enabled = obs_data_get_bool(s, "enable_chat");
        bb->reverse_stack = obs_data_get_bool(s, "reverse_stack");
        bb->chat_placeholder = obs_data_get_bool(s, "chat_placeholder");
        bb->chat_x_percent = std::clamp((int)obs_data_get_int(s, "chat_x_percent"), 0, 100);
        bb->chat_y_percent = std::clamp((int)obs_data_get_int(s, "chat_y_percent"), 0, 100);
        bb->repeat_follower_protection.store(obs_data_get_bool(s, "repeat_follower_protection"));
        bb->font_size = std::clamp((int)obs_data_get_int(s, "font_size"), 8, 256);
        bb->font_face = obs_data_get_string(s, "font_face");
        set_text_font(bb->status_source, bb->font_face, bb->font_size);
        {
            std::lock_guard<std::mutex> lock(bb->messages_mutex);
            for (auto &m : bb->messages) {
                for (auto *badge : m.badge_sources)
                    set_text_font(badge, bb->font_face, bb->font_size);
                set_text_font(m.username_source, bb->font_face, bb->font_size);
                set_text_font(m.text_source, bb->font_face, bb->font_size);
            }
        }
        alerts_update(&bb->alerts, s);
        goals_update(&bb->goals, s, bb->font_size, bb->font_face);
        chatterbox_update(&bb->chatterbox, s);
    };
    info.get_defaults = [](obs_data_t *s) {
        obs_data_set_default_string(s, "api_url", "");
        obs_data_set_default_int(s, "max_messages", 4);
        obs_data_set_default_int(s, "message_timeout", 30);
        obs_data_set_default_bool(s, "enable_chat", true);
        obs_data_set_default_bool(s, "reverse_stack", false);
        obs_data_set_default_bool(s, "chat_placeholder", true);
        obs_data_set_default_int(s, "chat_x_percent", 60);
        obs_data_set_default_int(s, "chat_y_percent", 95);
        obs_data_set_default_int(s, "font_size", 32);
        obs_data_set_default_string(s, "font_face", "Times New Roman");
        obs_data_set_default_bool(s, "video_enabled", true);
        obs_data_set_default_int(s, "alert_cooldown", 60);
        obs_data_set_default_int(s, "alert_max_duration", 60);
        obs_data_set_default_bool(s, "alert_loop_enabled", false);
        obs_data_set_default_int(s, "alert_loop_count", 3);
        obs_data_set_default_int(s, "alert_font_size", 32);
        obs_data_set_default_string(s, "alert_font_face", "Times New Roman");
        obs_data_set_default_string(s, "follower_media", "");
        obs_data_set_default_string(s, "subscriber_media", "");
        obs_data_set_default_string(s, "raid_media", "");
        obs_data_set_default_string(s, "rant_media", "");
        obs_data_set_default_string(s, "likes_media", "");
        obs_data_set_default_string(s, "donation_media", "");
        obs_data_set_default_int(s, "video_x_percent", 60);
        obs_data_set_default_int(s, "video_y_percent", 30);
        obs_data_set_default_int(s, "likes_x_percent", 40);
        obs_data_set_default_int(s, "likes_y_percent", 95);
        obs_data_set_default_int(s, "video_width", 512);
        obs_data_set_default_int(s, "video_height", 512);
        obs_data_set_default_int(s, "goal_x", 75);
        obs_data_set_default_int(s, "goal_y", 15);
        obs_data_set_default_int(s, "goal_font_size", 40);
        obs_data_set_default_string(s, "goal_font_face", "Times New Roman");
        obs_data_set_default_bool(s, "goal_enabled", false);
        obs_data_set_default_bool(s, "enable_goal_text", true);
        obs_data_set_default_int(s, "goal_metric", GOAL_FOLLOWERS);
        obs_data_set_default_int(s, "goal_period", GOAL_MONTHLY);
        obs_data_set_default_int(s, "goal_target", 25);
    
        obs_source_t *current_scene = obs_frontend_get_current_scene();
        if (current_scene) {
            obs_data_set_default_string(s, "goal_complete_scene", obs_source_get_name(current_scene));
            obs_source_release(current_scene);
        } else {
            obs_data_set_default_string(s, "goal_complete_scene", "");
        }
    
        obs_data_set_default_int(s, "goal_font_color", 0xFFFFFFFF);
        obs_data_set_default_int(s, "goal_completed_color", 0xFF0000FF);
        obs_data_set_default_int(s, "goal_remaining_color", 0xFFFFFFFF);
        obs_data_set_default_bool(s, "repeat_follower_protection", true);

        chatterbox_get_defaults(s);
    };
    info.get_properties = [](void *data) {
        auto *bb = static_cast<beep_boop_data *>(data);
        obs_properties_t *p = obs_properties_create();
        obs_properties_add_text(p, "api_url", "Rumble API URL", OBS_TEXT_PASSWORD);
        const char *connection_status = bb && bb->api_connected.load() ? "🟢 🔗 Connected" : "🔴 ⛓ Disconnected";
        obs_properties_add_text(p, "connection_status", connection_status, OBS_TEXT_INFO);
        obs_properties_add_button2(p, "refresh_connection", "Refresh Connection", [](obs_properties_t *props, obs_property_t *, void *data) {
            auto *source_data = static_cast<beep_boop_data *>(data);
            if (source_data) {
                source_data->status_refresh_requested.store(true);
                source_data->reconnect_requested.store(true);
                source_data->stop_cv.notify_all();
            }
            obs_property_t *status = obs_properties_get(props, "connection_status");
            if (status) obs_property_set_description(status, "🟡 Reconnecting…");
            return true;
        }, bb);
        obs_properties_add_bool(p, "enable_chat", "Enable Chat");
        obs_properties_add_bool(p, "reverse_stack", "Reverse Chat Stack");
        obs_properties_add_bool(p, "chat_placeholder", "Chat Placeholder");
        obs_properties_add_bool(p, "video_enabled", "Enable Alerts");
        obs_properties_add_bool(p, "repeat_follower_protection", "Repeat Follower Protection (Debug Option)");
        obs_properties_add_bool(p, "alert_loop_enabled", "Alert Video Loop (w/o audio)");
        obs_property_t *p_loop_count = obs_properties_add_int_slider(p, "alert_loop_count", "Alert Loop Count", 2, 10, 1);
        obs_property_int_set_suffix(p_loop_count, " times");
        obs_properties_add_bool(p, "goal_enabled", "Enable Goal");
        obs_properties_add_bool(p, "enable_goal_text", "Enable Goal Text");
        obs_properties_add_text(p, "sep_goals_chat", "────────────────────────", OBS_TEXT_INFO);
        obs_properties_add_int_slider(p, "max_messages", "Max Chat Messages", 1, 10, 1);
        obs_property_t *duration = obs_properties_add_int_slider(p, "message_timeout", "Chat Duration", 5, 120, 1);
        obs_property_int_set_suffix(duration, " s");
        obs_property_t *chat_font_size = obs_properties_add_int_slider(p, "font_size", "Chat Font Size", 8, 256, 1);
        obs_property_int_set_suffix(chat_font_size, " px");
        obs_property_t *font = obs_properties_add_list(p, "font_face", "Chat Font Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(font, "Times New Roman", "Times New Roman");
        obs_property_list_add_string(font, "Arial", "Arial");
        obs_property_list_add_string(font, "Verdana", "Verdana");
        obs_property_list_add_string(font, "Georgia", "Georgia");
        obs_property_list_add_string(font, "Courier New", "Courier New");
        obs_property_t *chat_x = obs_properties_add_int_slider(p, "chat_x_percent", "Chat X", 0, 100, 1);
        obs_property_int_set_suffix(chat_x, " %");
        obs_property_t *chat_y = obs_properties_add_int_slider(p, "chat_y_percent", "Chat Y", 0, 100, 1);
        obs_property_int_set_suffix(chat_y, " %");
        obs_properties_add_text(p, "sep_chat_alerts", "────────────────────────", OBS_TEXT_INFO);
        if (bb) alerts_add_properties(p, &bb->alerts);
        obs_properties_add_text(p, "sep_alert_goals", "────────────────────────", OBS_TEXT_INFO);
        goals_add_properties(p, bb ? bb->source : nullptr);
        obs_properties_add_text(p, "sep_goals_chatterbox", "────────────────────────", OBS_TEXT_INFO);
        chatterbox_add_properties(p, bb ? &bb->chatterbox : nullptr);
        return p;
    };
    info.video_render = beep_boop_render;
    info.audio_render = beep_boop_audio_render;
    info.get_width = beep_boop_width;
    info.get_height = beep_boop_height;
    info.enum_active_sources = beep_boop_enum_sources;
    info.enum_all_sources = beep_boop_enum_sources;
    obs_register_source(&info);
    return true;
}

void obs_module_unload(void) {
    curl_global_cleanup();
}