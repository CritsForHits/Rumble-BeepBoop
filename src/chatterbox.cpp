#include "beepboop.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <util/platform.h>
#include <thread>
#include <algorithm>
#include <cctype>
#include <fstream>

using json = nlohmann::json;

// Worker threads are owned by the source and joined during destruction.
template<typename Function, typename... Args>
static void chatterbox_start_worker(beep_boop_chatterbox_data *cb,
                                    Function &&function, Args&&... args)
{
    if (!cb)
        return;

    std::lock_guard<std::mutex> lock(cb->workers_mutex);
    if (cb->shutting_down.load())
        return;

    cb->workers.emplace_back(std::forward<Function>(function),
                             std::forward<Args>(args)...);
}

struct ChatHttpResponse {
    std::string body;
};

static size_t chatterbox_write_callback(char *ptr, size_t size, size_t nmemb, void *user_data) {
    auto *res = static_cast<ChatHttpResponse *>(user_data);
    res->body.append(ptr, size * nmemb);
    return size * nmemb;
}

static void set_chatterbox_connected(beep_boop_chatterbox_data *cb, bool connected,
                                     bool refresh_properties = false) {
    if (!cb) return;
    (void)refresh_properties;
    cb->connected.store(connected);
}

static bool chatterbox_http_get(const std::string &url, long timeout_seconds,
                                ChatHttpResponse &response)
{
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chatterbox_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && status >= 200 && status < 300;
}

static std::string chatterbox_get_url(beep_boop_chatterbox_data *cb)
{
    std::lock_guard<std::mutex> lock(cb->url_mutex);
    return cb->url;
}

static std::string normalize_chatterbox_url(std::string url)
{
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        url = "http://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

static std::string chatterbox_voice_display_name(const std::string &filename) {
    std::string name = filename;
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.erase(dot);
    std::replace(name.begin(), name.end(), '_', ' ');

    bool capitalize = true;
    for (char &ch : name) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (capitalize && std::isalpha(uch))
            ch = static_cast<char>(std::toupper(uch));
        else if (!capitalize && std::isalpha(uch))
            ch = static_cast<char>(std::tolower(uch));
        capitalize = (ch == ' ' || ch == '-' || ch == '_');
    }
    return name;
}

static void fetch_chatterbox_voices(beep_boop_chatterbox_data *cb) {
    if (!cb) return;
    const std::string configured_url = chatterbox_get_url(cb);
    if (configured_url.empty()) {
        {
            std::lock_guard<std::mutex> lock(cb->voices_mutex);
            cb->available_voices.clear();
        }
        set_chatterbox_connected(cb, false, false);
        return;
    }

    const std::string base_url = normalize_chatterbox_url(configured_url);

    ChatHttpResponse response;
    const bool connected = chatterbox_http_get(base_url + "/get_predefined_voices", 5, response);
    std::vector<chatterbox_voice_info> fetched;
    if (connected && !response.body.empty()) {
        try {
            const json root = json::parse(response.body);
            const json voices = root.is_array() ? root : root.value("voices", json::array());
            if (voices.is_array()) {
                for (const auto &voice : voices) {
                    if (!voice.is_object()) continue;
                    const std::string name = voice.value("display_name", voice.value("name", "Voice"));
                    const std::string id = voice.value("voice_id", voice.value("filename", "default"));
                    fetched.push_back({name, id});
                }
            }
        } catch (...) {}
    }

    if (connected) {
        ChatHttpResponse references_response;
        if (chatterbox_http_get(base_url + "/get_reference_files", 3, references_response)) {
            try {
                const json references = json::parse(references_response.body);
                if (references.is_array()) {
                    for (const auto &filename : references) {
                        if (!filename.is_string()) continue;
                        const std::string file = filename.get<std::string>();
                        fetched.push_back({chatterbox_voice_display_name(file), "ref_" + file});
                    }
                }
            } catch (...) {}
        }
    }

    if (configured_url != chatterbox_get_url(cb)) return;
    {
        std::lock_guard<std::mutex> lock(cb->voices_mutex);
        cb->available_voices = std::move(fetched);
    }
    set_chatterbox_connected(cb, connected, false);
}

static void chatterbox_poll_worker_thread(beep_boop_chatterbox_data *cb)
{
    constexpr int POLL_INTERVAL_SECONDS = 15;
    while (!cb->shutting_down.load()) {
        for (int elapsed = 0; elapsed < POLL_INTERVAL_SECONDS && !cb->shutting_down.load(); ++elapsed)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (cb->shutting_down.load()) break;

        const std::string configured_url = chatterbox_get_url(cb);
        if (configured_url.empty()) {
            set_chatterbox_connected(cb, false);
            continue;
        }
        const std::string base_url = normalize_chatterbox_url(configured_url);
        ChatHttpResponse response;
        const bool connected = chatterbox_http_get(base_url + "/get_predefined_voices", 5, response);
        if (configured_url == chatterbox_get_url(cb))
            set_chatterbox_connected(cb, connected);
    }
}

static obs_data_t *create_media_settings(const std::string &local_file) {
    obs_data_t *media = obs_data_create();
    obs_data_set_bool(media, "is_local_file", true);
    obs_data_set_string(media, "local_file", local_file.c_str());
    obs_data_set_bool(media, "looping", false);
    obs_data_set_bool(media, "restart_on_activate", false);
    obs_data_set_bool(media, "close_when_inactive", true);
    obs_data_set_bool(media, "hw_decode", false);
    return media;
}

static void fetch_and_play_tts(beep_boop_chatterbox_data *cb, const std::string &text, const std::string &voice) {
    if (!cb || text.empty() || cb->shutting_down.load())
        return;
    const std::string configured_url = chatterbox_get_url(cb);
    if (configured_url.empty()) {
        set_chatterbox_connected(cb, false);
        return;
    }

    // Prevent concurrent requests from overwriting the same WAV file or
    // replacing the OBS media source while another request is doing so.
    std::lock_guard<std::mutex> request_lock(cb->request_mutex);
    if (cb->shutting_down.load())
        return;

    const std::string base_url = normalize_chatterbox_url(configured_url);
    std::string endpoint = base_url + "/tts";

    bool is_ref = (voice.rfind("ref_", 0) == 0);
    std::string actual_voice = is_ref ? voice.substr(4) : voice;

    json payload;
    payload["text"] = text;
    payload["voice_mode"] = is_ref ? "clone" : "predefined";
    
    if (is_ref) {
        payload["reference_audio_filename"] = actual_voice;
    } else {
        payload["predefined_voice_id"] = actual_voice.empty() ? "default" : actual_voice;
    }

    payload["split_text"] = false;
    payload["output_format"] = "wav";

    std::string payload_str = payload.dump();

    CURL *curl = curl_easy_init();
    if (!curl) {
        set_chatterbox_connected(cb, false);
        return;
    }

    ChatHttpResponse res;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload_str.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chatterbox_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    const bool request_ok = code == CURLE_OK && status >= 200 && status < 300;
    if (configured_url == chatterbox_get_url(cb))
        set_chatterbox_connected(cb, request_ok);
    bool playback_started = false;
    if (request_ok && !res.body.empty()) {
        // Save generated audio in OBS's user configuration directory instead
        // of relying on the process working directory.
        char temp_path[512] = {};
        int path_result = os_get_config_path(
            temp_path, sizeof(temp_path), "chatterbox_temp.wav");

        if (path_result <= 0 || temp_path[0] == '\0') {
            return;
        }


        // Reject error pages or unexpected response formats before saving.
        if (res.body.size() < 12 ||
            res.body.compare(0, 4, "RIFF") != 0 ||
            res.body.compare(8, 4, "WAVE") != 0) {
            return;
        }

        std::ofstream outfile(temp_path, std::ios::binary | std::ios::trunc);
        if (!outfile.is_open()) {
            return;
        }

        outfile.write(res.body.data(), static_cast<std::streamsize>(res.body.size()));
        outfile.close();
        if (!outfile) {
            return;
        }


        std::lock_guard<std::mutex> lock(cb->source_mutex);
        if (cb->media_source) {
            obs_source_remove_active_child(cb->parent, cb->media_source);
            obs_source_release(cb->media_source);
            cb->media_source = nullptr;
        }

        obs_data_t *media_settings = create_media_settings(temp_path);
        cb->media_source = obs_source_create_private("ffmpeg_source", "Chatterbox TTS Stream", media_settings);
        obs_data_release(media_settings);

        if (cb->media_source) {
            obs_source_add_active_child(cb->parent, cb->media_source);
            // Use OBS's native source-volume control on the generated WAV playback.
            obs_source_set_volume(cb->media_source, cb->volume.load());
            obs_source_set_monitoring_type(cb->media_source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
            obs_source_media_restart(cb->media_source);
            playback_started = true;
        }
    }
    // Wait until OBS has actually started and finished playing the audio.
    if (playback_started) {
        bool saw_playing = false;

        const auto timeout = std::chrono::steady_clock::now() +
                         std::chrono::minutes(2);

        while (!cb->shutting_down.load() &&
           std::chrono::steady_clock::now() < timeout) {

            enum obs_media_state state;

            {
                std::lock_guard<std::mutex> lock(cb->source_mutex);

                if (!cb->media_source)
                    break;

                state = obs_source_media_get_state(cb->media_source);
            }

            if (state == OBS_MEDIA_STATE_PLAYING)
                saw_playing = true;

            if (saw_playing && state == OBS_MEDIA_STATE_ENDED)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

static void chatterbox_queue_worker(beep_boop_chatterbox_data *cb) {
    if (!cb)
        return;

    while (!cb->shutting_down.load()) {

        std::pair<std::string, std::string> message;

        {
            std::unique_lock<std::mutex> lock(cb->queue_mutex);

            cb->queue_cv.wait(lock, [cb] {
                return cb->shutting_down.load() ||
                       !cb->tts_queue.empty();
            });

            if (cb->shutting_down.load())
                break;

            message = std::move(cb->tts_queue.front());
            cb->tts_queue.pop_front();
        }

        fetch_and_play_tts(cb, message.first, message.second);
    }
}

static void chatterbox_enqueue_tts(
    beep_boop_chatterbox_data *cb,
    const std::string &text,
    const std::string &voice)
{
    if (!cb || text.empty() || cb->shutting_down.load())
        return;

    {
        std::lock_guard<std::mutex> lock(cb->queue_mutex);

        // Maximum number of waiting messages.
        if (cb->tts_queue.size() >=
            static_cast<size_t>(cb->tts_buffer)) {
            blog(LOG_WARNING,
                 "[BeepBoop] TTS queue full. Dropping message.");
            return;
        }

        cb->tts_queue.emplace_back(text, voice);
    }

    cb->queue_cv.notify_one();
}

void chatterbox_init(beep_boop_chatterbox_data *cb, obs_source_t *parent, obs_data_t *settings) {
    if (!cb)
        return;

    cb->parent = parent;
    cb->media_source = nullptr;
    cb->shutting_down.store(false);
    // Seed the URL before update so initialization performs only one voice fetch.
    {
        std::lock_guard<std::mutex> lock(cb->url_mutex);
        cb->url = obs_data_get_string(settings, "chatterbox_url");
    }
    chatterbox_update(cb, settings);
    chatterbox_start_worker(cb, fetch_chatterbox_voices, cb);
    chatterbox_start_worker(cb, chatterbox_poll_worker_thread, cb);
    chatterbox_start_worker(cb, chatterbox_queue_worker, cb);
}

void chatterbox_update(beep_boop_chatterbox_data *cb, obs_data_t *settings) {
    if (!cb || !settings || cb->shutting_down.load())
        return;

    const std::string new_url = obs_data_get_string(settings, "chatterbox_url");
    bool url_changed = false;
    {
        std::lock_guard<std::mutex> lock(cb->url_mutex);
        url_changed = new_url != cb->url;
        cb->url = new_url;
    }
    if (url_changed) {
        set_chatterbox_connected(cb, false);
        {
            std::lock_guard<std::mutex> lock(cb->voices_mutex);
            cb->available_voices.clear();
        }
        if (cb->parent) obs_source_update_properties(cb->parent);
    }

    cb->enable_chat_vo = obs_data_get_bool(settings, "enable_chat_vo");
    cb->enable_rant_vo = obs_data_get_bool(settings, "enable_rant_vo");

    const float volume = std::clamp((float)obs_data_get_double(settings, "chatterbox_volume"), 0.1f, 3.0f);
    cb->volume.store(volume);

    {
        std::lock_guard<std::mutex> lock(cb->queue_mutex);
        cb->tts_buffer = std::clamp(
            (int)obs_data_get_int(settings, "tts_buffer"),
            1, 10
        );
    }

    {
        std::lock_guard<std::mutex> lock(cb->source_mutex);
        if (cb->media_source)
            obs_source_set_volume(cb->media_source, volume);
    }

    cb->chat_voice = obs_data_get_string(settings, "chat_voice");
    cb->rant_voice = obs_data_get_string(settings, "rant_voice");

    if (url_changed)
        chatterbox_start_worker(cb, fetch_chatterbox_voices, cb);
}

void chatterbox_get_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings, "chatterbox_url", "http://localhost:8000");
    obs_data_set_default_bool(settings, "enable_chat_vo", false);
    obs_data_set_default_bool(settings, "enable_rant_vo", false);
    obs_data_set_default_int(settings, "tts_buffer", 3);
    obs_data_set_default_double(settings, "chatterbox_volume", 1.0);
    obs_data_set_default_string(settings, "chat_voice", "default");
    obs_data_set_default_string(settings, "rant_voice", "default");
}

void chatterbox_destroy(beep_boop_chatterbox_data *cb) {
    if (!cb)
        return;

    // Block new work, then wait for in-flight workers before releasing OBS
    // resources or allowing the owning source data to be destroyed.
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(cb->workers_mutex);
        cb->shutting_down.store(true);
        workers.swap(cb->workers);
        cb->queue_cv.notify_all();
    }

    for (auto &worker : workers) {
        if (worker.joinable())
            worker.join();
    }

    std::lock_guard<std::mutex> lock(cb->source_mutex);
    if (cb->media_source) {
        if (cb->parent)
            obs_source_remove_active_child(cb->parent, cb->media_source);
        obs_source_release(cb->media_source);
        cb->media_source = nullptr;
    }
    cb->parent = nullptr;
}

void chatterbox_add_properties(obs_properties_t *props, beep_boop_chatterbox_data *cb) {
    obs_properties_add_text(props, "chatterbox_url", "Chatterbox Connect URL", OBS_TEXT_DEFAULT);
    const char *status = cb && cb->connected.load()
        ? "🟢 Chatterbox Connected" : "🔴 Chatterbox Disconnected";
    obs_properties_add_text(props, "chatterbox_status", status, OBS_TEXT_INFO);

    obs_properties_add_button2(
        props,
        "refresh_chatterbox",
        "Refresh Connection",
        [](obs_properties_t *, obs_property_t *, void *data) {

            auto *cbox = static_cast<beep_boop_chatterbox_data *>(data);

            if (cbox && !cbox->shutting_down.load()) {

                set_chatterbox_connected(cbox, false, false);

                fetch_chatterbox_voices(cbox);
            }

            return true;

        },
        cb
    );

    obs_properties_add_float_slider(props, "chatterbox_volume", "TTS Volume", 0.1, 3.0, 0.1);

    obs_properties_add_int_slider(
        props,
        "tts_buffer",
        "TTS Buffer",
        1,
        10,
        1
    );

    obs_properties_add_bool(props, "enable_chat_vo", "Enable Chat VO");
    obs_properties_add_bool(props, "enable_rant_vo", "Enable Rant VO");

    std::vector<chatterbox_voice_info> voices;
    if (cb) {
        std::lock_guard<std::mutex> lock(cb->voices_mutex);
        voices = cb->available_voices;
    }

    obs_property_t *chat_voice_list = obs_properties_add_list(props, "chat_voice", "Chat Voice", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_t *rant_voice_list = obs_properties_add_list(props, "rant_voice", "Rant Voice", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(chat_voice_list, "Default", "default");
    obs_property_list_add_string(rant_voice_list, "Default", "default");

    for (const auto &v : voices) {
        obs_property_list_add_string(chat_voice_list, v.name.c_str(), v.voice_id.c_str());
        obs_property_list_add_string(rant_voice_list, v.name.c_str(), v.voice_id.c_str());
    }

    obs_properties_add_button2(props, "test_chatterbox_speech", "Test Speech", [](obs_properties_t *, obs_property_t *, void *data) {
        auto *cbox = static_cast<beep_boop_chatterbox_data *>(data);
        if (cbox) {
            chatterbox_test_speech(cbox);
        }
        return true;
    }, cb);
}

void chatterbox_speak_chat(beep_boop_chatterbox_data *cb,
                           const std::string &username,
                           const std::string &text) {
    if (!cb || !cb->enable_chat_vo || cb->shutting_down.load())
        return;

    std::string speech = username + " says: " + text;
    chatterbox_enqueue_tts(cb, speech, cb->chat_voice);
}

void chatterbox_speak_rant(beep_boop_chatterbox_data *cb,
                           const std::string &username,
                           const std::string &text) {
    if (!cb || !cb->enable_rant_vo || cb->shutting_down.load())
        return;

    std::string speech = username + " rants: " + text;
    chatterbox_enqueue_tts(cb, speech, cb->rant_voice);
}

void chatterbox_test_speech(beep_boop_chatterbox_data *cb) {
    if (!cb || cb->shutting_down.load())
        return;

    chatterbox_enqueue_tts(
        cb,
        "This is a test of your text to speech system.",
        cb->chat_voice
    );
}

bool chatterbox_audio_render(beep_boop_chatterbox_data *cb, uint64_t *ts_out,
                             obs_source_audio_mix *audio_output,
                             uint32_t mixers, size_t channels, size_t sample_rate) {
    (void)sample_rate;
    if (!cb || !audio_output || !ts_out)
        return false;

    std::lock_guard<std::mutex> lock(cb->source_mutex);
    if (!cb->media_source) return false;

    // Check if playback ended
    if (obs_source_media_get_state(cb->media_source) == OBS_MEDIA_STATE_ENDED) {
        return false;
    }

    uint64_t timestamp = obs_source_get_audio_timestamp(cb->media_source);
    if (!timestamp) return false;

    obs_source_audio_mix child = {};
    obs_source_get_audio_mix(cb->media_source, &child);
    for (size_t mix = 0; mix < MAX_AUDIO_MIXES; ++mix) {
        if (!(mixers & (1U << mix))) continue;
        for (size_t ch = 0; ch < channels; ++ch) {
            float *out = audio_output->output[mix].data[ch];
            float *in = child.output[mix].data[ch];
            if (!out || !in) continue;
            for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; ++i) out[i] += in[i];
        }
    }
    *ts_out = timestamp;
    return true;
}