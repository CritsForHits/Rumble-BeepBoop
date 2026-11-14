#include <obs-module.h>
#include <graphics/graphics.h>
#include <string>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <random>
#include "beepboop.h"

static obs_source_t *create_alert_text_source(const char *name, const std::string &text,
                                               uint32_t color, const std::string &face,
                                               int size)
{
    obs_data_t *settings = obs_data_create();
    obs_data_t *font = obs_data_create();
    obs_data_set_string(font, "face", face.c_str());
    obs_data_set_int(font, "size", size);
    obs_data_set_obj(settings, "font", font);
    obs_data_set_string(settings, "text", text.c_str());
    obs_data_set_string(settings, "align", "left");
    obs_data_set_int(settings, "color1", color);
    obs_data_set_int(settings, "color2", color);
    obs_data_set_bool(settings, "outline", true);
    obs_source_t *source = obs_source_create_private("text_ft2_source", name, settings);
    obs_data_release(font);
    obs_data_release(settings);
    return source;
}

static void update_alert_text_source(obs_source_t *source, const std::string &text,
                                     uint32_t color, const std::string &face, int size)
{
    if (!source) return;
    obs_data_t *settings = obs_source_get_settings(source);
    obs_data_t *font = obs_data_create();
    obs_data_set_string(font, "face", face.c_str());
    obs_data_set_int(font, "size", size);
    obs_data_set_obj(settings, "font", font);
    obs_data_set_string(settings, "text", text.c_str());
    obs_data_set_int(settings, "color1", color);
    obs_data_set_int(settings, "color2", color);
    obs_source_update(source, settings);
    obs_data_release(font);
    obs_data_release(settings);
}

// Order matches enum beep_boop_alert_type in beepboop.h
static const char *media_keys[ALERT_COUNT] = {
    "follower_media", "subscriber_media", "gifted_subs_media", "raid_media", "rant_media"
};
static const char *media_labels[ALERT_COUNT] = {
    "New Follower Alert", "New Subscriber Alert", "Gifted Subs Alert", "Raid Alert", "Rant Alert"
};

static obs_data_t *create_media_settings(const std::string &local_file)
{
    obs_data_t *media = obs_data_create();
    obs_data_set_bool(media, "is_local_file", true);
    obs_data_set_string(media, "local_file", local_file.c_str());
    obs_data_set_bool(media, "looping", false);
    obs_data_set_bool(media, "restart_on_activate", false);
    obs_data_set_bool(media, "close_when_inactive", false);
    obs_data_set_bool(media, "hw_decode", false);
    return media;
}

static bool is_audio_only_media(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "wav" || ext == "ogg" || ext == "mp3" || ext == "flac" ||
           ext == "aac" || ext == "m4a" || ext == "opus";
}

void alerts_stop(beep_boop_alerts_data *alerts)
{
    if (!alerts) return;
    alerts->active_alert.store(-1);
    alerts->loop_audio_muted.store(false);
    alerts->current_loop_iteration.store(1);
    alerts->media_started.store(false);
    alerts_set_label(alerts, "");
    for (int i = 0; i < ALERT_COUNT; ++i) {
        if (alerts->media_sources[i]) {
            obs_source_media_stop(alerts->media_sources[i]);
            obs_source_set_monitoring_type(alerts->media_sources[i], OBS_MONITORING_TYPE_NONE);
        }
    }
}

void alerts_set_label(beep_boop_alerts_data *alerts, const std::string &label)
{
    if (!alerts) return;
    {
        std::lock_guard<std::mutex> lock(alerts->label_mutex);
        alerts->active_label = label;
        alerts->styled_label_active = false;
    }
    if (alerts->label_source)
        update_alert_text_source(alerts->label_source, label, 0xFFFFFFFF,
                                 alerts->alert_font_face, alerts->alert_font_size);
}

void alerts_set_user_label(beep_boop_alerts_data *alerts, const std::string &prefix,
                           const std::string &username, const std::string &suffix,
                           uint32_t username_color, const std::vector<std::string> &badges)
{
    if (!alerts) return;

    std::string badge_text;
    for (const auto &badge : badges) {
        const auto style = beep_boop_badge_style_for(badge);
        if (!style.glyph.empty()) badge_text += style.glyph + " ";
    }

    std::lock_guard<std::mutex> lock(alerts->label_mutex);
    alerts->active_label = prefix + badge_text + username + suffix;
    alerts->styled_label_active = true;
    update_alert_text_source(alerts->prefix_source, prefix, 0xFFFFFFFF,
                             alerts->alert_font_face, alerts->alert_font_size);
    for (size_t i = 0; i < alerts->badge_sources.size(); ++i) {
        const auto *badge = i < badges.size() ? &badges[i] : nullptr;
        const auto style = badge ? beep_boop_badge_style_for(*badge)
                                 : beep_boop_badge_style{ "", 0xFFFFFFFF };
        update_alert_text_source(alerts->badge_sources[i],
                                 style.glyph.empty() ? "" : style.glyph + " ", style.color,
                                 alerts->alert_font_face, alerts->alert_font_size);
    }
    update_alert_text_source(alerts->username_source, username, username_color,
                             alerts->alert_font_face, alerts->alert_font_size);
    update_alert_text_source(alerts->suffix_source, suffix, 0xFFFFFFFF,
                             alerts->alert_font_face, alerts->alert_font_size);
}

static void read_video_settings(beep_boop_alerts_data *alerts, obs_data_t *settings)
{
    alerts->alerts_enabled = obs_data_get_bool(settings, "video_enabled");
    for (int i = 0; i < ALERT_COUNT; ++i)
        alerts->media_paths[i] = obs_data_get_string(settings, media_keys[i]);
    if (alerts->media_paths[ALERT_RANT].empty())
        alerts->media_paths[ALERT_RANT] = obs_data_get_string(settings, "donation_media");
    {
        std::lock_guard<std::mutex> lock(alerts->cooldown_mutex);
        alerts->alert_cooldown = std::clamp((int)obs_data_get_int(settings, "alert_cooldown"), 0, 1000);
        alerts->alert_max_duration = std::clamp((int)obs_data_get_int(settings, "alert_max_duration"), 10, 500);
        alerts->loop_enabled = obs_data_get_bool(settings, "alert_loop_enabled");
        alerts->loop_count = std::clamp((int)obs_data_get_int(settings, "alert_loop_count"), 2, 10);
    }
    alerts->video_x_percent = std::clamp((int)obs_data_get_int(settings, "video_x_percent"), 0, 100);
    alerts->video_y_percent = std::clamp((int)obs_data_get_int(settings, "video_y_percent"), 0, 100);
    alerts->video_width = std::clamp((int)obs_data_get_int(settings, "video_width"), 8, 4096);
    alerts->video_height = std::clamp((int)obs_data_get_int(settings, "video_height"), 8, 4096);
    alerts->alert_font_size = std::clamp((int)obs_data_get_int(settings, "alert_font_size"), 8, 256);
    alerts->alert_font_face = obs_data_get_string(settings, "alert_font_face");
    if (alerts->alert_font_face.empty()) alerts->alert_font_face = "DejaVu Sans";
}

bool alerts_play(beep_boop_alerts_data *alerts, beep_boop_alert_type type)
{
    if (!alerts || !alerts->alerts_enabled || type < 0 || type >= ALERT_COUNT ||
        !alerts->media_sources[type])
        return false;

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(alerts->cooldown_mutex);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - alerts->last_alert_times[type]).count();
        if (alerts->alert_cooldown > 0 && elapsed < alerts->alert_cooldown)
            return false;
        alerts->last_alert_times[type] = now;
    }

    for (int i = 0; i < ALERT_COUNT; ++i) {
        if (i != type && alerts->media_sources[i])
            obs_source_media_stop(alerts->media_sources[i]);
    }

    obs_source_t *source = alerts->media_sources[type];
    alerts->current_loop_iteration.store(1);
    alerts->loop_audio_muted.store(false);
    alerts->media_started.store(false);
    alerts->active_alert.store(type);
    alerts->alert_start_time = now;

    // Keep looping under our control so each completed play can be counted.
    obs_data_t *media = create_media_settings(alerts->media_paths[type]);
    obs_source_update(source, media);
    obs_data_release(media);
    obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
    obs_source_media_restart(source);
    return true;
}

static const std::vector<std::string> test_badges = {
    "admin", "moderator", "supporter", "whale-blue", "whale-yellow", "subscriber"
};

static uint32_t random_username_color()
{
    std::random_device random;
    std::mt19937 generator(random());
    std::uniform_int_distribution<uint32_t> color(0, 0xFFFFFF);
    return 0xFF000000 | color(generator);
}

static bool test_alert(beep_boop_alerts_data *alerts, beep_boop_alert_type type,
                       const std::string &prefix, const std::string &suffix)
{
    if (!alerts) return true;
    alerts_set_user_label(alerts, prefix, "UserA", suffix,
                          random_username_color(), test_badges);
    alerts_play(alerts, type);
    return true;
}

static bool test_follower(obs_properties_t *, obs_property_t *, void *data)
{ return test_alert(static_cast<beep_boop_alerts_data *>(data), ALERT_FOLLOWER, "", " has Followed!"); }
static bool test_subscriber(obs_properties_t *, obs_property_t *, void *data)
{ return test_alert(static_cast<beep_boop_alerts_data *>(data), ALERT_SUBSCRIBER, "", " has Subbed!"); }
static bool test_gifted_subs(obs_properties_t *, obs_property_t *, void *data)
{ return test_alert(static_cast<beep_boop_alerts_data *>(data), ALERT_GIFTED_SUBS, "", " gifted 5 Subs!"); }
static bool test_raid(obs_properties_t *, obs_property_t *, void *data)
{ return test_alert(static_cast<beep_boop_alerts_data *>(data), ALERT_RAID, "Incoming Raid from ", "!"); }
static bool test_rant(obs_properties_t *, obs_property_t *, void *data)
{ return test_alert(static_cast<beep_boop_alerts_data *>(data), ALERT_RANT, "", " Rants: \"Hello World\" on testing"); }

void alerts_init(beep_boop_alerts_data *alerts, obs_source_t *parent, obs_data_t *settings)
{
    if (!alerts || !parent) return;
    alerts->parent = parent;
    read_video_settings(alerts, settings);
    alerts->label_source = create_alert_text_source("BeepBoop Alert Name", "", 0xFFFFFFFF, alerts->alert_font_face, alerts->alert_font_size);
    alerts->prefix_source = create_alert_text_source("BeepBoop Alert Prefix", "", 0xFFFFFFFF, alerts->alert_font_face, alerts->alert_font_size);
    alerts->badge_sources.reserve(6);
    for (int i = 0; i < 6; ++i) {
        const std::string name = "BeepBoop Alert Badge " + std::to_string(i + 1);
        alerts->badge_sources.push_back(create_alert_text_source(name.c_str(), "", 0xFFFFFFFF,
            alerts->alert_font_face, alerts->alert_font_size));
    }
    alerts->username_source = create_alert_text_source("BeepBoop Alert Username", "", 0xFFFFFFFF, alerts->alert_font_face, alerts->alert_font_size);
    alerts->suffix_source = create_alert_text_source("BeepBoop Alert Suffix", "", 0xFFFFFFFF, alerts->alert_font_face, alerts->alert_font_size);

    if (alerts->label_source) obs_source_add_active_child(parent, alerts->label_source);
    if (alerts->prefix_source) obs_source_add_active_child(parent, alerts->prefix_source);
    for (auto *badge : alerts->badge_sources)
        if (badge) obs_source_add_active_child(parent, badge);
    if (alerts->username_source) obs_source_add_active_child(parent, alerts->username_source);
    if (alerts->suffix_source) obs_source_add_active_child(parent, alerts->suffix_source);

    for (int i = 0; i < ALERT_COUNT; ++i) {
        obs_data_t *media = create_media_settings("");
        alerts->media_sources[i] = obs_source_create_private("ffmpeg_source", media_labels[i], media);
        obs_data_release(media);
        if (alerts->media_sources[i]) {
            obs_source_add_active_child(parent, alerts->media_sources[i]);
            obs_source_set_monitoring_type(alerts->media_sources[i], OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
            obs_source_media_stop(alerts->media_sources[i]);
        }
    }
}

void alerts_update(beep_boop_alerts_data *alerts, obs_data_t *settings)
{
    if (!alerts) return;
    read_video_settings(alerts, settings);
    alerts_stop(alerts);
}

void alerts_destroy(beep_boop_alerts_data *alerts)
{
    if (!alerts) return;
    for (auto &source : alerts->media_sources) {
        if (source) {
            obs_source_remove_active_child(alerts->parent, source);
            obs_source_release(source);
            source = nullptr;
        }
    }
    alerts->active_alert.store(-1);
    obs_source_t **label_sources[] = {&alerts->label_source, &alerts->prefix_source,
                                      &alerts->username_source, &alerts->suffix_source};
    for (obs_source_t **source : label_sources) {
        if (*source) {
            obs_source_remove_active_child(alerts->parent, *source);
            obs_source_release(*source);
            *source = nullptr;
        }
    }
    for (auto *&badge : alerts->badge_sources) {
        if (badge) {
            obs_source_remove_active_child(alerts->parent, badge);
            obs_source_release(badge);
            badge = nullptr;
        }
    }
    alerts->badge_sources.clear();
    alerts->parent = nullptr;
}

void alerts_add_properties(obs_properties_t *props, beep_boop_alerts_data *alerts)
{
    obs_property_t *p_cd = obs_properties_add_int_slider(props, "alert_cooldown", "Alert Cooldown", 0, 1000, 1);
    obs_property_int_set_suffix(p_cd, " s");
    obs_property_t *p_duration = obs_properties_add_int_slider(props, "alert_max_duration", "Alert Max Duration", 10, 500, 1);
    obs_property_int_set_suffix(p_duration, " s");

    for (int i = 0; i < ALERT_COUNT; ++i) {
        obs_properties_add_path(props, media_keys[i], media_labels[i], OBS_PATH_FILE,
            "Media Files (*.mp3 *.wav *.ogg *.flac *.mp4 *.mkv *.webm);;All Files (*.*)", nullptr);
        
        auto test_cb = i == 0 ? test_follower :
                       i == 1 ? test_subscriber :
                       i == 2 ? test_gifted_subs :
                       i == 3 ? test_raid : test_rant;

        obs_properties_add_button2(props, (std::string("test_") + media_keys[i]).c_str(),
            (std::string("Test ") + media_labels[i]).c_str(), test_cb, alerts);
    }

    obs_property_t *alert_font = obs_properties_add_list(props, "alert_font_face", "Alerts Font Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(alert_font, "DejaVu Sans", "DejaVu Sans");
    obs_property_list_add_string(alert_font, "Liberation Sans", "Liberation Sans");
    obs_property_list_add_string(alert_font, "Liberation Serif", "Liberation Serif");
    obs_property_list_add_string(alert_font, "DejaVu Sans Mono", "DejaVu Sans Mono");
    obs_property_t *alert_size = obs_properties_add_int_slider(props, "alert_font_size", "Alert Font Size", 8, 256, 1);
    obs_property_int_set_suffix(alert_size, " px");
    obs_property_t *p = obs_properties_add_int_slider(props, "video_x_percent", "Alert X", 0, 100, 1);
    obs_property_int_set_suffix(p, " %");
    p = obs_properties_add_int_slider(props, "video_y_percent", "Alert Y", 0, 100, 1);
    obs_property_int_set_suffix(p, " %");
    p = obs_properties_add_int_slider(props, "video_width", "Alert Width (px)", 8, 4096, 1);
    obs_property_int_set_suffix(p, " px");
    p = obs_properties_add_int_slider(props, "video_height", "Alert Height (px)", 8, 4096, 1);
    obs_property_int_set_suffix(p, " px");
}

void alerts_enum_sources(beep_boop_alerts_data *alerts, obs_source_enum_proc_t cb, void *param)
{
    if (!alerts || !alerts->parent) return;
    if (alerts->label_source) cb(alerts->parent, alerts->label_source, param);
    if (alerts->prefix_source) cb(alerts->parent, alerts->prefix_source, param);
    {
        std::lock_guard<std::mutex> lock(alerts->label_mutex);
        for (auto *badge : alerts->badge_sources)
            if (badge) cb(alerts->parent, badge, param);
    }
    if (alerts->username_source) cb(alerts->parent, alerts->username_source, param);
    if (alerts->suffix_source) cb(alerts->parent, alerts->suffix_source, param);
    for (auto *source : alerts->media_sources)
        if (source) cb(alerts->parent, source, param);
}

void alerts_video_render(beep_boop_alerts_data *alerts)
{
    if (!alerts || !alerts->alerts_enabled) return;
    int index = alerts->active_alert.load();
    if (index < 0 || index >= ALERT_COUNT) return;
    
    obs_source_t *source = alerts->media_sources[index];
    if (!source) {
        alerts->active_alert.store(-1);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - alerts->alert_start_time).count();
    if (elapsed >= alerts->alert_max_duration) {
        alerts_stop(alerts);
        return;
    }

    bool loop_enabled = false;
    int max_loops = 1;
    {
        std::lock_guard<std::mutex> lock(alerts->cooldown_mutex);
        loop_enabled = alerts->loop_enabled;
        max_loops = alerts->loop_count;
    }
    const bool repeat_video = loop_enabled && !is_audio_only_media(alerts->media_paths[index]);
    const auto media_state = obs_source_media_get_state(source);
    if (media_state == OBS_MEDIA_STATE_PLAYING || media_state == OBS_MEDIA_STATE_PAUSED)
        alerts->media_started.store(true);

    const bool playback_ended =
        (media_state == OBS_MEDIA_STATE_ENDED || media_state == OBS_MEDIA_STATE_STOPPED) &&
        alerts->media_started.exchange(false);
    if (playback_ended) {
        const int current = alerts->current_loop_iteration.load();
        if (repeat_video && current < max_loops) {
            alerts->current_loop_iteration.store(current + 1);
            alerts->loop_audio_muted.store(true);
            alerts->media_started.store(false);
            obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_NONE);
            obs_source_media_restart(source);
            return;
        }
        alerts_stop(alerts);
        return;
    }
    if (media_state == OBS_MEDIA_STATE_ERROR) {
        alerts_stop(alerts);
        return;
    }

    uint32_t sw = obs_source_get_width(source), sh = obs_source_get_height(source);
    if (sw == 0 || sh == 0) {
        sw = alerts->video_width;
        sh = alerts->video_height;
    }
    if (alerts->video_width <= 0 || alerts->video_height <= 0) return;
    
    obs_video_info vi = {};
    if (!obs_get_video_info(&vi) || vi.base_width == 0 || vi.base_height == 0) return;
    
    float x = (float)(alerts->video_x_percent * vi.base_width) / 100.0f;
    float y = (float)(alerts->video_y_percent * vi.base_height) / 100.0f;

    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    gs_matrix_scale3f((float)alerts->video_width / sw, (float)alerts->video_height / sh, 1.0f);
    obs_source_video_render(source);
    gs_matrix_pop();

    std::string active_label;
    bool styled_label = false;
    {
        std::lock_guard<std::mutex> lock(alerts->label_mutex);
        active_label = alerts->active_label;
        styled_label = alerts->styled_label_active;
    }
    if (!active_label.empty()) {
        const bool audio_only = is_audio_only_media(alerts->media_paths[index]);
        float label_width = 0.0f;
        if (styled_label) {
            label_width = static_cast<float>(obs_source_get_width(alerts->prefix_source)) +
                          static_cast<float>(obs_source_get_width(alerts->username_source)) +
                          static_cast<float>(obs_source_get_width(alerts->suffix_source));
            for (auto *badge : alerts->badge_sources)
                if (badge) label_width += static_cast<float>(obs_source_get_width(badge));
        } else if (alerts->label_source) {
            label_width = static_cast<float>(obs_source_get_width(alerts->label_source));
        }
        const float label_x = audio_only ? x : x + std::max(0.0f, (static_cast<float>(alerts->video_width) - label_width) * 0.5f);
        const float label_y = audio_only ? y : y + static_cast<float>(alerts->video_height) + 8.0f;
        float draw_x = label_x;
        auto draw_label_part = [&](obs_source_t *part) {
            if (!part) return;
            gs_matrix_push();
            gs_matrix_translate3f(draw_x, label_y, 0.0f);
            obs_source_video_render(part);
            gs_matrix_pop();
            draw_x += static_cast<float>(obs_source_get_width(part));
        };
        if (styled_label) {
            draw_label_part(alerts->prefix_source);
            for (auto *badge : alerts->badge_sources) draw_label_part(badge);
            draw_label_part(alerts->username_source);
            draw_label_part(alerts->suffix_source);
        } else draw_label_part(alerts->label_source);
    }
}

bool alerts_audio_render(beep_boop_alerts_data *alerts, uint64_t *ts_out,
                         obs_source_audio_mix *audio_output, uint32_t mixers,
                         size_t channels, size_t sample_rate)
{
    (void)sample_rate;
    if (!alerts || !alerts->alerts_enabled) return false;
    int index = alerts->active_alert.load();
    if (index < 0 || index >= ALERT_COUNT) return false;

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - alerts->alert_start_time).count();
    if (elapsed >= alerts->alert_max_duration) {
        alerts->active_alert.store(-1);
        alerts->loop_audio_muted.store(false);
        alerts->media_started.store(false);
        obs_source_t *expired_source = alerts->media_sources[index];
        if (expired_source) {
            obs_source_set_monitoring_type(expired_source, OBS_MONITORING_TYPE_NONE);
            obs_source_media_stop(expired_source);
        }
        return false;
    }
    if (alerts->loop_audio_muted.load())
        return false;

    obs_source_t *source = alerts->media_sources[index];
    if (!source) return false;
    uint64_t timestamp = obs_source_get_audio_timestamp(source);
    if (!timestamp) return false;
    obs_source_audio_mix child = {};
    obs_source_get_audio_mix(source, &child);
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