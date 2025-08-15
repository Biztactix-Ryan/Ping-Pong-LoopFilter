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

static inline uint64_t ns_now()
{
    return os_gettime_ns();
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
    std::deque<gs_texrender_t*> frames;   // FIFO of frame textures (each is a snapshot)
    std::mutex frames_mtx;

    // Playback cursor
    size_t play_index = 0;          // 0..frames.size()-1
    int direction = +1;             // +1 forward, -1 backward
    double frame_accum = 0.0;       // fractional frame step timing
    uint64_t last_tick_ns = 0;

    // Scratch
    gs_texrender_t *scratch = nullptr;  // where we render upstream each tick

    // Hotkey
    obs_hotkey_id hotkey_toggle = OBS_INVALID_HOTKEY_ID;
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
static uint32_t loop_filter_width(void *data);
static uint32_t loop_filter_height(void *data);
static void loop_filter_save(void *data, obs_data_t *settings);

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
}

static void ensure_scratch(loop_filter *lf)
{
    if (!lf->scratch && lf->base_w > 0 && lf->base_h > 0) {
        lf->scratch = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (lf->scratch) {
            blog(LOG_DEBUG, "[" PLUGIN_ID "] Created scratch texture (%ux%u)", lf->base_w, lf->base_h);
        } else {
            blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to create scratch texture (%ux%u)", lf->base_w, lf->base_h);
        }
    }
}

// Take upstream image into a new texrender and push into ring buffer
static void capture_upstream_frame(loop_filter *lf)
{
    if (lf->base_w <= 0 || lf->base_h <= 0) {
        static bool dimension_warning_shown = false;
        if (!dimension_warning_shown) {
            blog(LOG_WARNING, "[" PLUGIN_ID "] Cannot capture frame - invalid dimensions: %ux%u", 
                 lf->base_w, lf->base_h);
            dimension_warning_shown = true;
        }
        return;
    }

    ensure_scratch(lf);
    if (!lf->scratch) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Cannot capture frame - scratch texture not available");
        return;
    }

    // Render upstream into scratch
    if (gs_texrender_begin(lf->scratch, lf->base_w, lf->base_h)) {
        gs_ortho(0.0f, (float)lf->base_w, 0.0f, (float)lf->base_h, -1.0f, 1.0f);

        if (obs_source_process_filter_begin(lf->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
            // Draw upstream content into our scratch
            obs_source_process_filter_end(lf->context, nullptr, lf->base_w, lf->base_h);
        }

        gs_texrender_end(lf->scratch);
    } else {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to begin texrender for scratch");
        return;
    }

    // Duplicate the scratch into a persistent texrender for the buffer
    if (lf->base_w <= 0 || lf->base_h <= 0) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Invalid dimensions for copy: %ux%u", lf->base_w, lf->base_h);
        return;
    }
        
    gs_texrender_t *copy = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!copy) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to create copy texture for frame buffer");
        return;
    }

    if (gs_texrender_begin(copy, lf->base_w, lf->base_h)) {
        gs_ortho(0.0f, (float)lf->base_w, 0.0f, (float)lf->base_h, -1.0f, 1.0f);

        gs_texture_t *src_tex = gs_texrender_get_texture(lf->scratch);
        if (src_tex) {
            // Draw src texture to copy
            gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            gs_eparam_t *image = gs_effect_get_param_by_name(default_effect, "image");
            gs_effect_set_texture(image, src_tex);

            while (gs_effect_loop(default_effect, "Draw")) {
                gs_draw_sprite(src_tex, 0, lf->base_w, lf->base_h);
            }
        }
        gs_texrender_end(copy);
    } else {
        gs_texrender_destroy(copy);
        return;
    }

    // Push into ring buffer
    {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        lf->frames.push_back(copy);
        if (lf->frames.size() > lf->max_frames) {
            auto *oldest = lf->frames.front();
            lf->frames.pop_front();
            if (oldest) gs_texrender_destroy(oldest);
            // keep play_index sensible if we're in loop mode
            if (lf->play_index > 0) lf->play_index--;
        }
        
        // Log buffer status periodically
        static int capture_count = 0;
        if (++capture_count % 60 == 0) {  // Log every 60 frames (roughly 1 second at 60fps)
            blog(LOG_DEBUG, "[" PLUGIN_ID "] Buffer status: %zu/%zu frames", 
                 lf->frames.size(), lf->max_frames);
        }
    }
}

// Draw a frame texture to output
static void draw_frame(gs_texture_t *tex, uint32_t w, uint32_t h)
{
    if (!tex) return;
    gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(default_effect, "image");
    gs_effect_set_texture(image, tex);

    while (gs_effect_loop(default_effect, "Draw")) {
        gs_draw_sprite(tex, 0, w, h);
    }
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
    
    loop_filter *lf = nullptr;
    try {
        lf = new loop_filter();
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to allocate filter: %s", e.what());
        return nullptr;
    } catch (...) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to allocate filter: unknown error");
        return nullptr;
    }
    
    if (!lf) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to create filter instance");
        return nullptr;
    }
    
    lf->context = context;

    // Don't try to get dimensions yet - source may not be ready
    lf->base_w = 0;
    lf->base_h = 0;
    
    // Try to get initial dimensions (may be 0 if source not ready)
    uint32_t initial_w = obs_source_get_base_width(context);
    uint32_t initial_h = obs_source_get_base_height(context);
    blog(LOG_INFO, "[" PLUGIN_ID "] Initial source dimensions: %ux%u", initial_w, initial_h);

    loop_filter_get_defaults(settings);
    loop_filter_update(lf, settings);
    recalc_buffer(lf);
    // Don't create scratch yet - wait until we have valid dimensions

    try {
        loop_filter_register_hotkeys(lf);
    } catch (...) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to register hotkeys");
        delete lf;
        return nullptr;
    }

    blog(LOG_INFO, "[" PLUGIN_ID "] Filter created successfully (buffer: %d seconds, max frames: %zu)", 
         lf->buffer_seconds, lf->max_frames);
    return lf;
}

static void loop_filter_destroy(void *data)
{
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf) return;

    blog(LOG_INFO, "[" PLUGIN_ID "] Destroying filter instance...");
    
    size_t frame_count = 0;
    {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        frame_count = lf->frames.size();
        clear_frames_locked(lf);
    }
    
    blog(LOG_INFO, "[" PLUGIN_ID "] Cleared %zu buffered frames", frame_count);
    
    if (lf->scratch) {
        gs_texrender_destroy(lf->scratch);
        lf->scratch = nullptr;
    }

    blog(LOG_INFO, "[" PLUGIN_ID "] Filter destroyed");
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
    {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        while (lf->frames.size() > lf->max_frames) {
            auto *f = lf->frames.front();
            lf->frames.pop_front();
            if (f) gs_texrender_destroy(f);
        }
        lf->play_index = lf->frames.empty() ? 0 : lf->frames.size() - 1;
    }
}

static obs_properties_t *loop_filter_properties(void *data)
{
    UNUSED_PARAMETER(data);

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_int(props, "buffer_seconds", "Buffer Length (seconds)", 10, 60, 1);
    obs_properties_add_bool(props, "ping_pong", "Ping-Pong (Forward/Reverse)");
    obs_properties_add_float_slider(props, "playback_speed", "Playback Speed", 0.1, 2.0, 0.1);

    // A button to toggle loop state from UI (optional, hotkey recommended)
    obs_properties_add_button(props, "toggle_loop", "Toggle Loop",
        [](obs_properties_t*, obs_property_t*, void *data) -> bool {
            auto *lf = reinterpret_cast<loop_filter*>(data);
            if (!lf) return false;
            
            lf->loop_enabled = !lf->loop_enabled;
            blog(LOG_INFO, "[" PLUGIN_ID "] Button toggle: %s", 
                 lf->loop_enabled ? "ENABLED" : "DISABLED");
            
            // reposition to end for natural start
            std::lock_guard<std::mutex> lk(lf->frames_mtx);
            size_t frame_count = lf->frames.size();
            if (lf->loop_enabled && frame_count > 0) {
                lf->play_index = frame_count - 1;
                lf->direction = -1; // start moving backward so it looks continuous
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

static uint32_t loop_filter_width(void *data)
{
    if (!data) return 0;
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf || !lf->context) return 0;
    
    try {
        lf->base_w = obs_source_get_base_width(lf->context);
    } catch (...) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Exception in loop_filter_width");
        return 0;
    }
    return lf->base_w;
}

static uint32_t loop_filter_height(void *data)
{
    if (!data) return 0;
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf || !lf->context) return 0;
    
    try {
        lf->base_h = obs_source_get_base_height(lf->context);
    } catch (...) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Exception in loop_filter_height");
        return 0;
    }
    return lf->base_h;
}

static void loop_filter_tick(void *data, float seconds)
{
    if (!data) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] tick called with null data");
        return;
    }
    
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf || !lf->context) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] tick called with invalid filter or context");
        return;
    }

    // Skip tick operations if dimensions are 0
    if (lf->base_w == 0 || lf->base_h == 0) {
        // Try to get dimensions once per tick
        try {
            uint32_t w = obs_source_get_base_width(lf->context);
            uint32_t h = obs_source_get_base_height(lf->context);
            if (w > 0 && h > 0) {
                lf->base_w = w;
                lf->base_h = h;
                blog(LOG_INFO, "[" PLUGIN_ID "] Tick: Got valid dimensions %ux%u", w, h);
            }
        } catch (...) {
            // Silently ignore
        }
        return;  // Skip rest of tick if no dimensions
    }

    if (!lf->loop_enabled) {
        // Capture upstream into buffer each tick
        // (We capture during render so we don't re-enter the graph here.)
        // But tick is a good place to advance any timers if needed.
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
    UNUSED_PARAMETER(effect);
    
    if (!data) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] render called with null data");
        return;
    }
    
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf || !lf->context) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] render called with invalid filter or context");
        return;
    }

    // Get dimensions safely
    uint32_t w = 0, h = 0;
    try {
        w = obs_source_get_base_width(lf->context);
        h = obs_source_get_base_height(lf->context);
    } catch (...) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Exception getting dimensions in render");
        return;
    }
    
    // Log dimension changes
    static uint32_t last_w = 0, last_h = 0;
    static bool first_render = true;
    if (first_render) {
        blog(LOG_INFO, "[" PLUGIN_ID "] First render call, dimensions: %ux%u", w, h);
        first_render = false;
    }
    if (w != last_w || h != last_h) {
        blog(LOG_INFO, "[" PLUGIN_ID "] Source dimensions changed: %ux%u -> %ux%u", 
             last_w, last_h, w, h);
        last_w = w;
        last_h = h;
        lf->base_w = w;
        lf->base_h = h;
    }
    
    // If dimensions are invalid, do absolutely nothing - don't even try to render
    if (w == 0 || h == 0) {
        static int skip_count = 0;
        if (++skip_count % 60 == 0) {
            blog(LOG_WARNING, "[" PLUGIN_ID "] Zero dimensions %ux%u, skipping all rendering", w, h);
        }
        // Do NOT call any OBS filter functions with 0x0 dimensions
        return;
    }

    if (!lf->loop_enabled) {
        // Pass-through: render upstream and capture it into buffer
        try {
            if (obs_source_process_filter_begin(lf->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
                obs_source_process_filter_end(lf->context, nullptr, w, h);
            }
        } catch (...) {
            blog(LOG_ERROR, "[" PLUGIN_ID "] Exception during passthrough");
            return;
        }

        // Now capture upstream into our circular buffer (using scratch copy)
        // We do this here after upstream was drawn to guarantee we sample exactly what viewers see.
        capture_upstream_frame(lf);
        return;
    }

    // Looping: draw from the buffer at play_index
    gs_texrender_t *frame_to_draw = nullptr;
    {
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        if (!lf->frames.empty()) {
            // Safety: clamp play_index
            if (lf->play_index >= lf->frames.size())
                lf->play_index = lf->frames.size() - 1;
            frame_to_draw = lf->frames[lf->play_index];
        }
    }

    if (!frame_to_draw) {
        // If nothing buffered yet, just fall back to live
        if (obs_source_process_filter_begin(lf->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
            obs_source_process_filter_end(lf->context, nullptr, w, h);
        }
        return;
    }

    gs_texture_t *tex = gs_texrender_get_texture(frame_to_draw);
    if (!tex) {
        // Fallback to live
        if (obs_source_process_filter_begin(lf->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
            obs_source_process_filter_end(lf->context, nullptr, w, h);
        }
        return;
    }

    // Draw the chosen buffered frame
    gs_ortho(0.0f, (float)w, 0.0f, (float)h, -1.0f, 1.0f);
    draw_frame(tex, w, h);
}

static void loop_filter_save(void *data, obs_data_t *settings)
{
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(settings);
}

// ----------------------------- Hotkeys -----------------------------

static void loop_filter_toggle_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    auto *lf = reinterpret_cast<loop_filter*>(data);
    if (!lf) return;

    lf->loop_enabled = !lf->loop_enabled;
    
    blog(LOG_INFO, "[" PLUGIN_ID "] Loop toggled: %s", lf->loop_enabled ? "ENABLED" : "DISABLED");

    if (lf->loop_enabled) {
        // Start from the latest frame and head backward first (looks continuous)
        std::lock_guard<std::mutex> lk(lf->frames_mtx);
        size_t frame_count = lf->frames.size();
        if (frame_count > 0) {
            lf->play_index = frame_count - 1;
            lf->direction = -1;
            lf->frame_accum = 0.0;
            blog(LOG_INFO, "[" PLUGIN_ID "] Starting playback from frame %zu of %zu", 
                 lf->play_index, frame_count);
        } else {
            blog(LOG_WARNING, "[" PLUGIN_ID "] No frames in buffer to play!");
            lf->loop_enabled = false;  // Disable if no frames
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

static obs_source_info loop_filter_info = {};
bool obs_module_load(void)
{
    blog(LOG_INFO, "[" PLUGIN_ID "] Starting module load...");
    
    try {
        memset(&loop_filter_info, 0, sizeof(loop_filter_info));
        
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
        // Temporarily comment out width/height to see if they're the issue
        // loop_filter_info.get_width = loop_filter_width;
        // loop_filter_info.get_height = loop_filter_height;
        // loop_filter_info.save = loop_filter_save;

        obs_register_source(&loop_filter_info);

        blog(LOG_INFO, "[" PLUGIN_ID "] Module loaded successfully");
        return true;
    } catch (const std::exception& e) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to load module: %s", e.what());
        return false;
    } catch (...) {
        blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to load module: unknown error");
        return false;
    }
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[" PLUGIN_ID "] unloaded");
}