
#pragma once

#include <obs-module.h>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include <thread>

struct beep_boop_badge_style {
    std::string glyph;
    uint32_t color;
};

// Badge glyphs use broadly supported Unicode symbols so OBS fonts can render them reliably.
inline beep_boop_badge_style beep_boop_badge_style_for(const std::string &badge) {
    std::string key;
    key.reserve(badge.size());
    for (unsigned char c : badge) {
        if (std::isalnum(c))
            key.push_back(static_cast<char>(std::tolower(c)));
    }

    constexpr uint32_t yellow = 0xFF00D7FF; // RGB: 255, 215, 0
    constexpr uint32_t blue = 0xFFFF0000;   // RGB: 0, 0, 255
    constexpr uint32_t green = 0xFF00FF00;  // RGB: 0, 255, 0
    constexpr uint32_t red = 0xFF0000FF;    // RGB: 255, 0, 0
    constexpr uint32_t white = 0xFFFFFFFF;

    if (key.find("admin") != std::string::npos) return {"★", yellow};
    if (key.find("moderator") != std::string::npos || key.find("mod") == 0)
        return {"★", blue};
    if (key.find("supporter") != std::string::npos) return {"▶", green};
    if (key.find("bluewhale") != std::string::npos || key.find("whaleblue") != std::string::npos)
        return {"⛃", blue};
    if (key.find("yellowwhale") != std::string::npos || key.find("whaleyellow") != std::string::npos)
        return {"⛃", yellow};
    if (key.find("subscriber") != std::string::npos || key == "sub")
        return {"♜", red};
    if (key.find("premium") != std::string::npos) return {"▶", green};
    return {badge, white};
}

enum beep_boop_alert_type {
    ALERT_FOLLOWER = 0,
    ALERT_SUBSCRIBER,
    ALERT_GIFTED_SUBS,
    ALERT_RAID,
    ALERT_RANT,
    ALERT_COUNT
};

struct beep_boop_goals_data;
struct beep_boop_chatterbox_data;

struct beep_boop_alerts_data {
    obs_source_t *parent = nullptr;
    obs_source_t *media_sources[ALERT_COUNT] = {};
    obs_source_t *label_source = nullptr;
    obs_source_t *prefix_source = nullptr;
    std::vector<obs_source_t *> badge_sources;
    obs_source_t *username_source = nullptr;
    obs_source_t *suffix_source = nullptr;

    bool styled_label_active = false;
    std::string active_label;
    std::mutex label_mutex;
    std::atomic<int> active_alert{-1};

    std::chrono::steady_clock::time_point alert_start_time{};
    int alert_cooldown = 60;
    std::atomic<int> alert_max_duration{60};
    bool loop_enabled = false;
    int loop_count = 3;
    std::atomic<int> current_loop_iteration{1};
    std::atomic<bool> loop_audio_muted{false};
    std::atomic<bool> media_started{false};

    std::string media_paths[ALERT_COUNT];
    uint32_t media_audio_mixers[ALERT_COUNT] = {};
    bool alerts_enabled = true;
    int video_x_percent = 0;
    int video_y_percent = 0;
    int video_width = 512;
    int video_height = 512;
    int alert_font_size = 32;
    std::string alert_font_face = "Times New Roman";

    std::mutex cooldown_mutex;
    std::chrono::steady_clock::time_point last_alert_times[ALERT_COUNT] = {};
};

enum beep_boop_goal_metric {
    GOAL_FOLLOWERS = 0,
    GOAL_SUBSCRIBERS = 1
};

enum beep_boop_goal_period {
    GOAL_TOTAL = 0,
    GOAL_MONTHLY,
    GOAL_WEEKLY,
    GOAL_DAILY
};

struct beep_boop_goals_data {
    obs_source_t *parent = nullptr;
    obs_source_t *label_source = nullptr;
    obs_source_t *count_source = nullptr;
    obs_source_t *completed_source = nullptr;
    obs_source_t *remaining_source = nullptr;

    std::mutex mutex;
    uint64_t api_follower_total = 0;
    uint64_t api_subscriber_total = 0;
    bool api_snapshot_initialized[2] = {false, false};
    std::vector<int64_t> api_follower_times;
    std::vector<std::pair<std::string, int64_t>> api_follower_events;
    std::vector<int64_t> api_subscriber_times;
    uint32_t label_color = 0xFFFFFFFF;
    uint32_t completed_color = 0xFF0000FF;
    uint32_t remaining_color = 0xFFFFFFFF;

    bool enabled = false;
    bool enable_text = true;
    int metric = GOAL_FOLLOWERS;
    int period = GOAL_TOTAL;
    int target = 25;
    std::string complete_scene;
    bool completion_scene_triggered = false;
    int font_size = 16;
    std::string font_face = "Times New Roman";
    int x = 0;
    int y = 108;
    int label_font_size = 24;
    std::string last_label;
    std::string last_count_label;
    std::string last_completed;
    std::string last_remaining;
};

struct chatterbox_voice_info {
    std::string name;
    std::string voice_id;
};

struct beep_boop_chatterbox_data {
    obs_source_t *parent = nullptr;
    std::mutex url_mutex;

    // OBS media source used to play generated WAV audio.
    obs_source_t *media_source = nullptr;
    std::mutex source_mutex;

    // Owned workers are joined during destroy to prevent use-after-free.
    std::mutex workers_mutex;
    std::vector<std::thread> workers;
    std::atomic<bool> shutting_down{false};

    // Serialize requests because they share one temporary WAV file/source.
    std::mutex request_mutex;
    // TTS message queue
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::pair<std::string, std::string>> tts_queue;
    int tts_buffer = 3;
    std::string url = "http://localhost:8000";
    bool enable_chat_vo = false;
    bool enable_rant_vo = false;
    std::string language = "en";
    float temperature = 0.8f;
    float exaggeration = 0.5f;
    std::atomic<float> volume{1.0f};
    std::string chat_voice = "default";
    std::string rant_voice = "default";

    std::atomic<bool> connected{false};

    std::mutex voices_mutex;
    std::vector<chatterbox_voice_info> available_voices;
};

// Alerts declarations
void alerts_init(beep_boop_alerts_data *alerts, obs_source_t *parent, obs_data_t *settings);
void alerts_update(beep_boop_alerts_data *alerts, obs_data_t *settings);
void alerts_destroy(beep_boop_alerts_data *alerts);
void alerts_add_properties(obs_properties_t *props, beep_boop_alerts_data *alerts);
void alerts_enum_sources(beep_boop_alerts_data *alerts, obs_source_enum_proc_t cb, void *param);
bool alerts_play(beep_boop_alerts_data *alerts, beep_boop_alert_type type);
void alerts_set_label(beep_boop_alerts_data *alerts, const std::string &label);
void alerts_set_user_label(beep_boop_alerts_data *alerts, const std::string &prefix,
                           const std::string &username, const std::string &suffix,
                           uint32_t username_color, const std::vector<std::string> &badges);
void alerts_video_render(beep_boop_alerts_data *alerts);
bool alerts_audio_render(beep_boop_alerts_data *alerts, uint64_t *ts_out,
                         obs_source_audio_mix *audio_output,
                         uint32_t mixers, size_t channels, size_t sample_rate);

// Goals declarations
void goals_init(beep_boop_goals_data *goals, obs_source_t *parent,
                obs_data_t *settings, int font_size, const std::string &font_face);
void goals_update(beep_boop_goals_data *goals, obs_data_t *settings,
                  int font_size, const std::string &font_face);
uint64_t goals_sync_api_snapshot(beep_boop_goals_data *goals, beep_boop_goal_metric metric,
                                 uint64_t api_total, const std::vector<int64_t> &event_times,
                                 const std::vector<std::pair<std::string, int64_t>> &follower_events = {});
void goals_render(beep_boop_goals_data *goals);
void goals_enum_sources(beep_boop_goals_data *goals, obs_source_enum_proc_t cb, void *param);
void goals_destroy(beep_boop_goals_data *goals);
void goals_add_properties(obs_properties_t *props, obs_source_t *source);

// Chatterbox declarations
void chatterbox_init(beep_boop_chatterbox_data *cb, obs_source_t *parent, obs_data_t *settings);
void chatterbox_update(beep_boop_chatterbox_data *cb, obs_data_t *settings);
void chatterbox_get_defaults(obs_data_t *settings);
void chatterbox_destroy(beep_boop_chatterbox_data *cb);
void chatterbox_add_properties(obs_properties_t *props, beep_boop_chatterbox_data *cb);
void chatterbox_speak_chat(beep_boop_chatterbox_data *cb, const std::string &username, const std::string &text);
void chatterbox_speak_rant(beep_boop_chatterbox_data *cb, const std::string &username, const std::string &text);
void chatterbox_test_speech(beep_boop_chatterbox_data *cb);
bool chatterbox_audio_render(beep_boop_chatterbox_data *cb, uint64_t *ts_out,
                             obs_source_audio_mix *audio_output,
                             uint32_t mixers, size_t channels, size_t sample_rate);
