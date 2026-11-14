#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <cstring>
#include <graphics/graphics.h>
#include "beepboop.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <unordered_set>

namespace {
constexpr int BAR_SEGMENTS = 40;

int64_t period_start(std::time_t now, int period)
{
    std::tm local{};
    bool local_ok = false;
#ifdef _WIN32
    local_ok = localtime_s(&local, &now) == 0;
    if (!local_ok) gmtime_s(&local, &now);
#else
    local_ok = localtime_r(&now, &local) != nullptr;
    if (!local_ok) gmtime_r(&now, &local);
#endif
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = local_ok ? -1 : 0;
    if (period == GOAL_WEEKLY)
        local.tm_mday -= (local.tm_wday + 6) % 7;
    else if (period == GOAL_MONTHLY)
        local.tm_mday = 1;
#ifdef _WIN32
    const std::time_t start = local_ok ? std::mktime(&local) : _mkgmtime(&local);
#else
    const std::time_t start = local_ok ? std::mktime(&local) : timegm(&local);
#endif
    return static_cast<int64_t>(start);
}

uint64_t count_since(const std::vector<int64_t> &times, int64_t start)
{
    return static_cast<uint64_t>(std::count_if(times.begin(), times.end(),
        [start](int64_t timestamp) { return timestamp >= start; }));
}

obs_source_t *create_text(const char *name, const std::string &text,
                          uint32_t color, int size, const std::string &face)
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

void update_text(obs_source_t *source, const std::string &text,
                 uint32_t color, int size, const std::string &face)
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

void render_at(obs_source_t *source, float x, float y)
{
    if (!source) return;
    gs_matrix_push();
    gs_matrix_translate3f(x, y, 0.0f);
    obs_source_video_render(source);
    gs_matrix_pop();
}

bool goal_period_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
    const int period = static_cast<int>(obs_data_get_int(settings, "goal_period"));
    obs_property_t *target = obs_properties_get(props, "goal_target");
    if (target) obs_property_int_set_limits(target, 0, period == GOAL_TOTAL ? 9000 : 50, 1);
    return true;
}

struct scene_switch_request { std::string name; };

void switch_to_goal_scene(void *param)
{
    auto *request = static_cast<scene_switch_request *>(param);
    if (request && !request->name.empty()) {
        obs_frontend_source_list scenes = {};
        obs_frontend_get_scenes(&scenes);
        for (size_t i = 0; i < scenes.sources.num; ++i) {
            obs_source_t *scene = scenes.sources.array[i];
            if (scene && request->name == obs_source_get_name(scene)) {
                obs_frontend_set_current_scene(scene);
                break;
            }
        }
        obs_frontend_source_list_free(&scenes);
    }
    delete request;
}
} // namespace

void goals_init(beep_boop_goals_data *goals, obs_source_t *parent,
                obs_data_t *settings, int font_size, const std::string &font_face)
{
    if (!goals || !parent) return;
    goals->parent = parent;
    goals_update(goals, settings, font_size, font_face);
    goals->label_source = create_text("BeepBoop Goal Label", "", goals->label_color,
                                     goals->label_font_size, goals->font_face);
    goals->count_source = create_text("BeepBoop Goal Count", "", goals->label_color,
                                     goals->label_font_size, goals->font_face);
    goals->completed_source = create_text("BeepBoop Goal Complete", "", 0xFFFFFFFF,
                                          goals->font_size, goals->font_face);
    goals->remaining_source = create_text("BeepBoop Goal Remaining", "", 0xFFFFFFFF,
                                           goals->font_size, goals->font_face);
    if (goals->label_source) obs_source_add_active_child(parent, goals->label_source);
    if (goals->count_source) obs_source_add_active_child(parent, goals->count_source);
    if (goals->completed_source) obs_source_add_active_child(parent, goals->completed_source);
    if (goals->remaining_source) obs_source_add_active_child(parent, goals->remaining_source);
}

void goals_update(beep_boop_goals_data *goals, obs_data_t *settings,
                  int /*font_size*/, const std::string & /*font_face*/)
{
    if (!goals || !settings) return;
    std::lock_guard<std::mutex> lock(goals->mutex);
    const int new_metric = static_cast<int>(obs_data_get_int(settings, "goal_metric"));
    goals->enabled = obs_data_get_bool(settings, "goal_enabled");
    
    // Check if property exists, otherwise default true
    if (obs_data_has_user_value(settings, "enable_goal_text")) {
        goals->enable_text = obs_data_get_bool(settings, "enable_goal_text");
    } else {
        goals->enable_text = true;
    }

    goals->metric = (new_metric == GOAL_SUBSCRIBERS) ? GOAL_SUBSCRIBERS : GOAL_FOLLOWERS;
    goals->period = static_cast<int>(obs_data_get_int(settings, "goal_period"));
    if (goals->period < GOAL_TOTAL || goals->period > GOAL_DAILY) goals->period = GOAL_TOTAL;
    goals->target = std::clamp(static_cast<int>(obs_data_get_int(settings, "goal_target")), 0, goals->period == GOAL_TOTAL ? 9000 : 50);
    goals->complete_scene = obs_data_get_string(settings, "goal_complete_scene");
    goals->label_color = static_cast<uint32_t>(obs_data_get_int(settings, "goal_font_color"));
    goals->completed_color = static_cast<uint32_t>(obs_data_get_int(settings, "goal_completed_color"));
    goals->remaining_color = static_cast<uint32_t>(obs_data_get_int(settings, "goal_remaining_color"));
    goals->label_font_size = std::clamp(static_cast<int>(obs_data_get_int(settings, "goal_font_size")), 8, 256);
    goals->font_size = std::max(1, static_cast<int>(std::lround(goals->label_font_size * 0.4)));
    goals->font_face = obs_data_get_string(settings, "goal_font_face");
    if (goals->font_face.empty()) goals->font_face = "DejaVu Sans";
    int goal_x = static_cast<int>(obs_data_get_int(settings, "goal_x"));
    int goal_y = static_cast<int>(obs_data_get_int(settings, "goal_y"));
    if (goal_x > 100) goal_x = static_cast<int>(std::lround(goal_x * 100.0 / 1920.0));
    if (goal_y > 100) goal_y = static_cast<int>(std::lround(goal_y * 100.0 / 1080.0));
    goals->x = std::clamp(goal_x, 0, 100);
    goals->y = std::clamp(goal_y, 0, 100);
    goals->last_label.clear();
    goals->last_count_label.clear();
    goals->last_completed.clear();
    goals->last_remaining.clear();
}

uint64_t goals_sync_api_snapshot(beep_boop_goals_data *goals, beep_boop_goal_metric metric,
                                 uint64_t api_total, const std::vector<int64_t> &event_times,
                                 const std::vector<std::pair<std::string, int64_t>> &follower_events)
{
    if (!goals || metric < GOAL_FOLLOWERS || metric > GOAL_SUBSCRIBERS) return 0;
    std::lock_guard<std::mutex> lock(goals->mutex);
    const int index = static_cast<int>(metric);
    uint64_t delta = 0;
    uint64_t &previous = metric == GOAL_SUBSCRIBERS
        ? goals->api_subscriber_total : goals->api_follower_total;
    if (goals->api_snapshot_initialized[index] && api_total > previous)
        delta = api_total - previous;
    previous = api_total;
    goals->api_snapshot_initialized[index] = true;
    if (metric == GOAL_SUBSCRIBERS)
        goals->api_subscriber_times = event_times;
    else {
        goals->api_follower_times = event_times;
        goals->api_follower_events = follower_events;
    }
    return delta;
}

void goals_render(beep_boop_goals_data *goals)
{
    if (!goals) return;
    int metric, period, target, font_size, x_offset, y_offset;
    uint32_t label_color, completed_color, remaining_color;
    uint64_t current = 0;
    std::string face, label, count_label, completed, remaining, complete_scene;
    bool queue_scene_switch = false;
    bool show_text = true;
    {
        std::lock_guard<std::mutex> lock(goals->mutex);
        if (!goals->enabled) return;
        show_text = goals->enable_text;
        metric = goals->metric;
        period = goals->period;
        target = goals->target;
        font_size = goals->font_size;
        x_offset = goals->x;
        y_offset = goals->y;
        face = goals->font_face;
        label_color = goals->label_color;
        completed_color = goals->completed_color;
        remaining_color = goals->remaining_color;
        complete_scene = goals->complete_scene;

        const bool subscribers = metric == GOAL_SUBSCRIBERS;
        const auto &times = subscribers ? goals->api_subscriber_times : goals->api_follower_times;
        const auto &follower_events = goals->api_follower_events;
        const uint64_t api_total = subscribers ? goals->api_subscriber_total : goals->api_follower_total;
        std::time_t now = std::time(nullptr);
        if (now == static_cast<std::time_t>(-1))
            now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        if (period == GOAL_TOTAL) current = api_total;
        else if (subscribers) current = count_since(times, period_start(now, period));
        else {
            const int64_t start = period_start(now, period);
            std::unordered_set<std::string> unique_followers;
            for (const auto &event : follower_events) {
                if (event.second >= start && !event.first.empty())
                    unique_followers.insert(event.first);
            }
            current = unique_followers.size();
        }
        
        const bool is_complete = target > 0 && current >= static_cast<uint64_t>(target);
        if (is_complete) {
            goals->enabled = false;
            blog(LOG_INFO, "BeepBoop Goal Completed! 'Enable Goals' automatically turned OFF.");
        }

        if (is_complete && !goals->completion_scene_triggered && !complete_scene.empty()) {
            goals->completion_scene_triggered = true;
            queue_scene_switch = true;
        } else if (!is_complete) {
            goals->completion_scene_triggered = false;
        }

        const int filled = target <= 0 || is_complete ? BAR_SEGMENTS :
            static_cast<int>((current * BAR_SEGMENTS) / static_cast<uint64_t>(target));
        const uint64_t remaining_count = current >= static_cast<uint64_t>(std::max(0, target))
            ? 0 : static_cast<uint64_t>(target) - current;
        label = std::to_string(remaining_count) +
            (metric == GOAL_SUBSCRIBERS ? " Subs Remaining!" : " Followers Remaining!");
        count_label.clear();
        for (int i = 0; i < filled; ++i) completed += "█";
        for (int i = filled; i < BAR_SEGMENTS; ++i) remaining += "█";

        // Always update bar progress strings, optionally update text labels
        if (show_text && label != goals->last_label) {
            goals->last_label = label;
            update_text(goals->label_source, label, label_color, goals->label_font_size, face);
        }
        if (completed != goals->last_completed || remaining != goals->last_remaining) {
            goals->last_completed = completed;
            goals->last_remaining = remaining;
            update_text(goals->completed_source, completed, completed_color, font_size, face);
            update_text(goals->remaining_source, remaining, remaining_color, font_size, face);
        }
    }

    if (queue_scene_switch) {
        auto *request = new scene_switch_request{complete_scene};
        obs_queue_task(OBS_TASK_UI, switch_to_goal_scene, request, false);
    }

    const float label_height = goals->label_source
        ? static_cast<float>(obs_source_get_height(goals->label_source)) : static_cast<float>(font_size);
    obs_video_info video_info = {};
    const float canvas_width = obs_get_video_info(&video_info) && video_info.base_width
        ? static_cast<float>(video_info.base_width) : 1920.0f;
    const float canvas_height = video_info.base_height ? static_cast<float>(video_info.base_height) : 1080.0f;
    const float x = static_cast<float>(x_offset) * canvas_width / 100.0f;
    const float y = static_cast<float>(y_offset) * canvas_height / 100.0f;

    // Render text labels only if enable_text is true
    if (show_text) {
        render_at(goals->label_source, x, y);
        render_at(goals->count_source, x, y);
    }

    // Progress bar elements always render when goal is enabled
    float bar_offset_y = show_text ? (y + label_height + 30.0f) : y;
    render_at(goals->completed_source, x, bar_offset_y);
    const float completed_width = goals->completed_source
        ? static_cast<float>(obs_source_get_width(goals->completed_source)) : 0.0f;
    render_at(goals->remaining_source, x + completed_width, bar_offset_y);
}

void goals_enum_sources(beep_boop_goals_data *goals, obs_source_enum_proc_t cb,
                        void *param)
{
    if (!goals || !goals->parent) return;
    if (goals->label_source) cb(goals->parent, goals->label_source, param);
    if (goals->count_source) cb(goals->parent, goals->count_source, param);
    if (goals->completed_source) cb(goals->parent, goals->completed_source, param);
    if (goals->remaining_source) cb(goals->parent, goals->remaining_source, param);
}

void goals_destroy(beep_boop_goals_data *goals)
{
    if (!goals) return;
    obs_source_t **sources[] = {&goals->label_source, &goals->count_source, &goals->completed_source,
                                &goals->remaining_source};
    for (obs_source_t **source : sources) {
        if (*source) {
            if (goals->parent) obs_source_remove_active_child(goals->parent, *source);
            obs_source_release(*source);
            *source = nullptr;
        }
    }
    goals->parent = nullptr;
}

void goals_add_properties(obs_properties_t *props, obs_source_t *source)
{
    obs_property_t *metric = obs_properties_add_list(props, "goal_metric", "Goal Type",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(metric, "Followers", GOAL_FOLLOWERS);
    obs_property_list_add_int(metric, "Subscribers", GOAL_SUBSCRIBERS);

    obs_property_t *period = obs_properties_add_list(props, "goal_period", "Goal Period",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(period, "Total", GOAL_TOTAL);
    obs_property_list_add_int(period, "Monthly", GOAL_MONTHLY);
    obs_property_list_add_int(period, "Weekly", GOAL_WEEKLY);
    obs_property_list_add_int(period, "Daily", GOAL_DAILY);
    obs_property_set_modified_callback(period, goal_period_modified);

    obs_property_t *scene_prop = obs_properties_add_list(props, "goal_complete_scene",
        "Goal Complete Scene", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    obs_source_t *current_scene = obs_frontend_get_current_scene();
    const char *current_name = current_scene ? obs_source_get_name(current_scene) : nullptr;
    if (current_name && *current_name)
        obs_property_list_add_string(scene_prop, current_name, current_name);
    for (size_t i = 0; i < scenes.sources.num; ++i) {
        obs_source_t *scene = scenes.sources.array[i];
        const char *name = scene ? obs_source_get_name(scene) : nullptr;
        if (name && *name && (!current_name || strcmp(name, current_name) != 0))
            obs_property_list_add_string(scene_prop, name, name);
    }
    obs_frontend_source_list_free(&scenes);
    if (current_scene) obs_source_release(current_scene);

    obs_property_t *target = obs_properties_add_int_slider(props, "goal_target", "Goal #", 0, 50, 1);
    int initial_period = GOAL_MONTHLY;
    if (source) {
        obs_data_t *source_settings = obs_source_get_settings(source);
        initial_period = static_cast<int>(obs_data_get_int(source_settings, "goal_period"));
        obs_data_release(source_settings);
    }
    obs_property_int_set_limits(target, 0, initial_period == GOAL_TOTAL ? 9000 : 50, 1);

    obs_property_t *gx = obs_properties_add_int_slider(props, "goal_x", "Goal X", 0, 100, 1);
    obs_property_int_set_suffix(gx, " %");
    obs_property_t *gy = obs_properties_add_int_slider(props, "goal_y", "Goal Y", 0, 100, 1);
    obs_property_int_set_suffix(gy, " %");
    obs_property_t *gfs = obs_properties_add_int_slider(props, "goal_font_size", "Goal Font Size", 8, 256, 1);
    obs_property_int_set_suffix(gfs, " px");
    obs_property_t *gfont = obs_properties_add_list(props, "goal_font_face", "Goal Font Style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(gfont, "Default DejaVu", "DejaVu Sans");
    obs_property_list_add_string(gfont, "Liberation Sans", "Liberation Sans");
    obs_property_list_add_string(gfont, "Liberation Serif", "Liberation Serif");
    obs_property_list_add_string(gfont, "DejaVu Sans Mono", "DejaVu Sans Mono");
    obs_properties_add_color(props, "goal_font_color", "Goal Font Color");
    obs_properties_add_color(props, "goal_completed_color", "Completed Bar Color");
    obs_properties_add_color(props, "goal_remaining_color", "Empty Bar Color");
}