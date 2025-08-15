// pingpong_loop_filter.cpp
// A simple OBS filter that records 10–60s of a source into a circular buffer,
// and when toggled, plays it back forward -> backward -> forward (ping-pong).

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <cmath>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-pingpong-loop-filter", "en-US")

#define PLUGIN_NAME        "PingPong Loop Filter"
#define PLUGIN_ID          "com.example.obs.pingpong_loop_filter"

// ----------------------------- Utilities -----------------------------

static inline double fps_from_ovi(const obs_video_info &ovi)
{
    return ovi.fps_den ? (double)ovi.fps_num / (double)ovi.fps_den : 60.0;
}

// Clamp utility
template<typename T>
static inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// --------------------------- Filter State ----------------------------

struct loop_filter {
    obs_source_t *context = nullptr;

    // Settings
    int buffer_seconds = 30;        // 10–60
    bool ping_pong = true;
    bool loop_enabled = false;
    double playback_speed = 1.0;    // 0.1–2.0x

    // Derived
    uint32_t base_w = 0;
    uint32_t base_h = 0;
    double fps = 60.0;
    size_t max_frames = 0;

    // Capture + Playback
    std::deque<gs_texrender_t*> frames;   // FIFO of frame textures
    std::mutex frames_mtx;

    // Playback cursor
    size_t play_index = 0;          // 0..frames.size()-1
    int direction = +1;             // +1 forward, -1 backward
    double frame_accum = 0.0;       // fractional frame step timing

    // Hotkey
    obs_hotkey_id hotkey_toggle = OBS_INVALID_HOTKEY_ID;
    
    // Frame capture state
    bool dimensions_valid = false;
    int frame_skip_counter = 0;  // To reduce capture rate
};

// ----------------------------- Forward Decls -----------------------------

static const char *loop_filter_get_name(void *);
static void *loop_filter_create(obs_data_t *settings, obs_source_t *context);
static void loop_filter_destroy(void *data);
static void loop_filter_update(void *data, obs_data_t *settings);
static obs_properties_t *loop_filter_properties(void *data);
static void loop_filter_get_defaults(obs_data_t *settings);
static void loop_filter_tick(void *data, float seconds);
static void loop_filter_render(void *data, gs_effect_t *effect);

// Hotkey
static void loop_filter_register_hotkeys(loop_filter *lf);
static void loop_filter_toggle_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

// ----------------------------- Helpers -----------------------------

static void clear_frames_locked(loop_filter *lf)
{
    for (auto *tr : lf->frames) {
        if (tr) {
            gs_texrender_destroy(tr);
        }
    }
    lf->frames.clear();
    lf->play_index = 0;
    lf->direction = +1;
    lf->frame_accum = 0.0;
}

static void recalc_buffer(loop_filter *lf)
{
    obs_video_info ovi;
    if (obs_get_video_info(&ovi)) {
        lf->fps = fps_from_ovi(ovi);
    } else {
        lf->fps = 60.0;
    }

    lf->max_frames = (size_t)std::llround(lf->fps * clampv(lf->buffer_seconds, 10, 60));
    if (lf->max_frames < 2) lf->max_frames = 2;
    
    blog(LOG_INFO, "[" PLUGIN_ID "] Buffer recalculated: %zu max frames at %.1f fps", 
         lf->max_frames, lf->fps);
}

// ----------------------------- OBS Callbacks -----------------------------

static const char *loop_filter_get_name(void *)
{
    return PLUGIN_NAME;
}

static void *loop_filter_create(obs_data_t *settings, obs_source_t *context)
{
    blog(LOG_INFO, "[" PLUGIN_ID "] Creating filter instance...");
    
    if (!context) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] No source context provided!");
        return nullptr;
    }
    
    auto *lf = new loop_filter();
    lf->context = context;
    lf->base_w = 0;
    lf->base_h = 0;
    lf->dimensions_valid = false;

    loop_filter_get_defaults(settings);
    loop_filter_update(lf, settings);
    recalc_buffer(lf);
    loop_filter_register_hotkeys(lf);

    blog(LOG_INFO, "[" PLUGIN_ID "] Filter created successfully");
    return lf;
}

static void loop_filter_destroy(void *data)
{
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf) return;

    blog(LOG_INFO, "[" PLUGIN_ID "] Destroying filter instance...");
    
    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        clear_frames_locked(lf);
    }
    obs_leave_graphics();

    delete lf;
}

static void loop_filter_update(void *data, obs_data_t *settings)
{
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf) return;

    lf->buffer_seconds = (int)obs_data_get_int(settings, "buffer_seconds");
    lf->buffer_seconds = clampv(lf->buffer_seconds, 10, 60);

    lf->ping_pong = obs_data_get_bool(settings, "ping_pong");
    lf->playback_speed = obs_data_get_double(settings, "playback_speed");
    lf->playback_speed = clampv(lf->playback_speed, 0.1, 2.0);

    recalc_buffer(lf);

    // If we shrank the buffer, trim old frames
    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        while (lf->frames.size() > lf->max_frames) {
            auto *f = lf->frames.front();
            lf->frames.pop_front();
            if (f) gs_texrender_destroy(f);
        }
    }
    obs_leave_graphics();
}

static obs_properties_t *loop_filter_properties(void *data)
{
    UNUSED_PARAMETER(data);

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_int(props, "buffer_seconds", "Buffer Length (seconds)", 10, 60, 1);
    obs_properties_add_bool(props, "ping_pong", "Ping-Pong (Forward/Reverse)");
    obs_properties_add_float_slider(props, "playback_speed", "Playback Speed", 0.1, 2.0, 0.1);

    // A button to toggle loop state from UI
    obs_properties_add_button(props, "toggle_loop", "Toggle Loop",
        [](obs_properties_t*, obs_property_t*, void *data) -> bool {
            auto *lf = reinterpret_cast<loop_filter*>(data);
            if (!lf) return false;
            
            lf->loop_enabled = !lf->loop_enabled;
            blog(LOG_INFO, "[" PLUGIN_ID "] Button toggle: %s", 
                 lf->loop_enabled ? "ENABLED" : "DISABLED");
            
            std::lock_guard<std::mutex> lk(lf->frames_mtx);
            size_t frame_count = lf->frames.size();
            if (lf->loop_enabled && frame_count > 0) {
                lf->play_index = frame_count - 1;
                lf->direction = -1;
                blog(LOG_INFO, "[" PLUGIN_ID "] Starting from frame %zu of %zu", 
                     lf->play_index, frame_count);
            } else if (lf->loop_enabled && frame_count == 0) {
                blog(LOG_WARNING, "[" PLUGIN_ID "] No frames buffered yet!");
                lf->loop_enabled = false;
            }
            return true;
        }
    );

    return props;
}

static void loop_filter_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "buffer_seconds", 30);
    obs_data_set_default_bool(settings, "ping_pong", true);
    obs_data_set_default_double(settings, "playback_speed", 1.0);
}

static void loop_filter_tick(void *data, float seconds)
{
    if (!data) return;
    
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf || !lf->context) return;

    // Update dimensions
    uint32_t w = obs_source_get_base_width(lf->context);
    uint32_t h = obs_source_get_base_height(lf->context);
    
    if (w != lf->base_w || h != lf->base_h) {
        blog(LOG_INFO, "[" PLUGIN_ID "] Dimensions changed: %ux%u -> %ux%u", 
             lf->base_w, lf->base_h, w, h);
        lf->base_w = w;
        lf->base_h = h;
        lf->dimensions_valid = (w > 0 && h > 0);
    }

    if (!lf->loop_enabled || !lf->dimensions_valid) {
        return;
    }

    // Advance playback cursor based on fps and playback_speed
    double step = seconds * lf->fps * lf->playback_speed;
    lf->frame_accum += step;

    size_t frames_to_advance = (size_t)lf->frame_accum;
    lf->frame_accum -= (double)frames_to_advance;

    if (frames_to_advance == 0) return;

    std::lock_guard<std::mutex> lk(lf->frames_mtx);
    if (lf->frames.size() < 2) return;

    for (size_t i = 0; i < frames_to_advance; ++i) {
        // Move the index
        if (lf->direction > 0) {
            if (lf->play_index + 1 >= lf->frames.size()) {
                if (lf->ping_pong) {
                    lf->direction = -1;
                    if (lf->play_index > 0) lf->play_index--;
                } else {
                    lf->play_index = 0;
                }
            } else {
                lf->play_index++;
            }
        } else {
            if (lf->play_index == 0) {
                if (lf->ping_pong) {
                    lf->direction = +1;
                    if (lf->frames.size() > 1) lf->play_index++;
                } else {
                    lf->play_index = lf->frames.size() - 1;
                }
            } else {
                lf->play_index--;
            }
        }
    }
}

static void loop_filter_render(void *data, gs_effect_t *effect)
{
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf || !lf->context) return;
    
    UNUSED_PARAMETER(effect);

    // Update dimensions if needed
    uint32_t w = obs_source_get_base_width(lf->context);
    uint32_t h = obs_source_get_base_height(lf->context);
    
    if (w == 0 || h == 0) {
        obs_source_skip_video_filter(lf->context);
        return;
    }
    
    lf->base_w = w;
    lf->base_h = h;
    lf->dimensions_valid = true;

    // If loop is enabled and we have frames, play from buffer
    if (lf->loop_enabled) {
        gs_texrender_t *frame_to_draw = nullptr;
        {
            std::lock_guard<std::mutex> lk(lf->frames_mtx);
            if (!lf->frames.empty() && lf->play_index < lf->frames.size()) {
                frame_to_draw = lf->frames[lf->play_index];
            }
        }
        
        if (frame_to_draw) {
            gs_texture_t *tex = gs_texrender_get_texture(frame_to_draw);
            if (tex) {
                // Use obs_source_draw to render the buffered frame
                gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
                gs_eparam_t *image = gs_effect_get_param_by_name(default_effect, "image");
                gs_effect_set_texture(image, tex);
                
                while (gs_effect_loop(default_effect, "Draw")) {
                    gs_draw_sprite(tex, 0, w, h);
                }
                return;
            }
        }
        // If no valid frame, skip the filter
        obs_source_skip_video_filter(lf->context);
        return;
    }

    // Default: capture source to buffer if not looping
    // First, we need to capture the current frame
    gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    
    if (texrender) {
        // Render source to texrender
        if (gs_texrender_begin(texrender, w, h)) {
            vec4 clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
            gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);
            
            // Render parent source
            obs_source_t *parent = obs_filter_get_parent(lf->context);
            if (parent) {
                obs_source_video_render(parent);
            }
            
            gs_texrender_end(texrender);
            
            // Capture frame to buffer if not looping (only every few frames to save memory)
            if (!lf->loop_enabled) {
                lf->frame_skip_counter++;
                if (lf->frame_skip_counter >= 2) {  // Capture every 2nd frame
                    lf->frame_skip_counter = 0;
                    
                    std::lock_guard<std::mutex> lk(lf->frames_mtx);
                    
                    // Create new texrender for this frame
                    gs_texrender_t *frame_copy = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
                    if (frame_copy && gs_texrender_begin(frame_copy, w, h)) {
                        vec4 clear = {0.0f, 0.0f, 0.0f, 0.0f};
                        gs_clear(GS_CLEAR_COLOR, &clear, 1.0f, 0);
                        
                        // Copy the texture
                        gs_texture_t *src_tex = gs_texrender_get_texture(texrender);
                        if (src_tex) {
                            gs_effect_t *copy_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
                            gs_eparam_t *image = gs_effect_get_param_by_name(copy_effect, "image");
                            gs_effect_set_texture(image, src_tex);
                            
                            while (gs_effect_loop(copy_effect, "Draw")) {
                                gs_draw_sprite(src_tex, 0, w, h);
                            }
                        }
                        
                        gs_texrender_end(frame_copy);
                        
                        // Add to buffer
                        lf->frames.push_back(frame_copy);
                        if (lf->frames.size() > lf->max_frames) {
                            auto *oldest = lf->frames.front();
                            lf->frames.pop_front();
                            if (oldest) gs_texrender_destroy(oldest);
                        }
                        
                        // Log periodically
                        static int log_counter = 0;
                        if (++log_counter % 30 == 0) {
                            blog(LOG_INFO, "[" PLUGIN_ID "] Buffer: %zu/%zu frames", 
                                 lf->frames.size(), lf->max_frames);
                        }
                    } else if (frame_copy) {
                        gs_texrender_destroy(frame_copy);
                    }
                }
            }
        }
        
        gs_texrender_destroy(texrender);
    }
    
    // Pass through the source video
    obs_source_skip_video_filter(lf->context);
}

// ----------------------------- Hotkeys -----------------------------

static void loop_filter_toggle_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf) return;

    lf->loop_enabled = !lf->loop_enabled;
    
    blog(LOG_INFO, "[" PLUGIN_ID "] Hotkey toggle: %s", lf->loop_enabled ? "ENABLED" : "DISABLED");

    if (lf->loop_enabled) {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        size_t frame_count = lf->frames.size();
        if (frame_count > 0) {
            lf->play_index = frame_count - 1;
            lf->direction = -1;
            lf->frame_accum = 0.0;
            blog(LOG_INFO, "[" PLUGIN_ID "] Starting playback from frame %zu of %zu", 
                 lf->play_index, frame_count);
        } else {
            blog(LOG_WARNING, "[" PLUGIN_ID "] No frames in buffer!");
            lf->loop_enabled = false;
        }
    }
}

static void loop_filter_register_hotkeys(loop_filter *lf)
{
    lf->hotkey_toggle = obs_hotkey_register_source(
        lf->context,
        "pingpong_toggle",
        "PingPong Loop: Toggle",
        loop_filter_toggle_cb,
        lf
    );
}

// ----------------------------- Registration -----------------------------

bool obs_module_load(void)
{
    blog(LOG_INFO, "[" PLUGIN_ID "] Loading module...");
    
    obs_source_info loop_filter_info = {};
    
    loop_filter_info.id = PLUGIN_ID;
    loop_filter_info.type = OBS_SOURCE_TYPE_FILTER;
    loop_filter_info.output_flags = OBS_SOURCE_VIDEO;

    loop_filter_info.get_name = loop_filter_get_name;
    loop_filter_info.create = loop_filter_create;
    loop_filter_info.destroy = loop_filter_destroy;
    loop_filter_info.update = loop_filter_update;
    loop_filter_info.get_defaults = loop_filter_get_defaults;
    loop_filter_info.get_properties = loop_filter_properties;

    loop_filter_info.video_render = loop_filter_render;
    loop_filter_info.video_tick = loop_filter_tick;

    obs_register_source(&loop_filter_info);

    blog(LOG_INFO, "[" PLUGIN_ID "] Module loaded successfully");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[" PLUGIN_ID "] Module unloaded");
}