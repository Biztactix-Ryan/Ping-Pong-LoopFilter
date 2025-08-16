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
#include <cstdio>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-pingpong-loop-filter", "en-US")

#define PLUGIN_NAME        "Looper"
#define PLUGIN_ID          "com.biztactix.obs.looper"

// ----------------------------- Utilities -----------------------------

static inline double fps_from_ovi(const obs_video_info &ovi)
{
	return ovi.fps_den ? (double)ovi.fps_num / (double)ovi.fps_den : 60.0;
}

// Clamp utility
template<typename T> static inline T clampv(T v, T lo, T hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// --------------------------- Filter State ----------------------------

struct loop_filter {
	obs_source_t *context = nullptr;

	// Settings
	int buffer_seconds = 30; // 10–60
	bool ping_pong = true;
	bool loop_enabled = false;
	double playback_speed = 1.0; // 0.1–2.0x

	// Derived
	uint32_t base_w = 0;
	uint32_t base_h = 0;
	double fps = 60.0;
	size_t max_frames = 0;
	int capture_skip_frames = 2; // Capture every Nth frame

	// Capture + Playback
	std::deque<gs_texrender_t *> frames; // FIFO of frame textures
	std::mutex frames_mtx;

	// Playback cursor
	size_t play_index = 0;    // 0..frames.size()-1
	int direction = +1;       // +1 forward, -1 backward
	double frame_accum = 0.0; // fractional frame step timing
	int total_loops = 0;      // Track how many times we've looped

	// Hotkey
	obs_hotkey_id hotkey_toggle = OBS_INVALID_HOTKEY_ID;

	// Frame capture state
	bool dimensions_valid = false;
	int frame_skip_counter = 0;     // To reduce capture rate
	uint64_t last_capture_time = 0; // Track last capture time in nanoseconds

	// UI state - removed toggle_button pointer to avoid lifetime issues
	double last_ui_update = 0.0;        // Track last UI update time
	uint64_t capture_start_time = 0;    // Track when capture started (nanoseconds)
	size_t frames_captured_count = 0;   // Track total frames captured
	size_t last_logged_frame_count = 0; // Track last frame count for UI updates

	// Resource management
	size_t max_memory_mb = 4096;     // Maximum memory usage in MB (default 4GB)
	size_t current_memory_usage = 0; // Track current memory usage
	uint32_t last_width = 0;         // Track resolution changes
	uint32_t last_height = 0;
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
static void loop_filter_show(void *data);
static void loop_filter_hide(void *data);

// Hotkey
static void loop_filter_register_hotkeys(loop_filter *lf);
static void loop_filter_toggle_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

// ----------------------------- Helpers -----------------------------

static void clear_frames_locked(loop_filter *lf)
{
	if (!lf) {
		blog(LOG_ERROR, "[" PLUGIN_ID "] clear_frames_locked called with null filter");
		return;
	}

	blog(LOG_INFO, "[" PLUGIN_ID "] Clearing %zu frames from buffer", lf->frames.size());

	for (auto *tr : lf->frames) {
		if (tr) {
			gs_texrender_destroy(tr);
		}
	}
	lf->frames.clear();
	lf->play_index = 0;
	lf->direction = +1;
	lf->frame_accum = 0.0;
	lf->frame_skip_counter = 0;
	lf->last_capture_time = 0;
}

static size_t estimate_memory_usage(uint32_t width, uint32_t height, size_t frame_count)
{
	// Each frame uses approximately width * height * 4 bytes (RGBA)
	// Plus overhead for texrender structure
	size_t bytes_per_frame = width * height * 4 + 256;      // 256 bytes overhead estimate
	return (bytes_per_frame * frame_count) / (1024 * 1024); // Return in MB
}

static void recalc_buffer(loop_filter *lf)
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		lf->fps = fps_from_ovi(ovi);
		blog(LOG_INFO, "[" PLUGIN_ID "] Detected OBS FPS: %.2f (num=%d, den=%d)", lf->fps, ovi.fps_num,
		     ovi.fps_den);
	} else {
		lf->fps = 60.0;
		blog(LOG_WARNING, "[" PLUGIN_ID "] Could not get video info, defaulting to 60 fps");
	}

	// Calculate frames based on actual capture rate (every Nth frame)
	// We want to capture 'buffer_seconds' worth of real-time content
	// If OBS is 60fps and we skip every 2 frames, we capture at 30fps
	// But we need to account for the actual render callback rate
	double effective_fps = lf->fps / lf->capture_skip_frames;
	lf->max_frames = (size_t)std::llround(effective_fps * clampv(lf->buffer_seconds, 10, 60));
	if (lf->max_frames < 2)
		lf->max_frames = 2;

	// Check memory limits if we have dimensions
	if (lf->base_w > 0 && lf->base_h > 0) {
		size_t estimated_mb = estimate_memory_usage(lf->base_w, lf->base_h, lf->max_frames);
		if (estimated_mb > lf->max_memory_mb) {
			// Reduce frame count to fit within memory limit
			size_t new_max_frames = (lf->max_memory_mb * 1024 * 1024) / (lf->base_w * lf->base_h * 4 + 256);
			blog(LOG_WARNING,
			     "[" PLUGIN_ID
			     "] Memory limit exceeded! Estimated: %zuMB > Limit: %zuMB. Reducing frames from %zu to %zu",
			     estimated_mb, lf->max_memory_mb, lf->max_frames, new_max_frames);
			lf->max_frames = new_max_frames;
			if (lf->max_frames < 2)
				lf->max_frames = 2;
		}
	}

	double base_playback = lf->buffer_seconds * 2.0; // ping-pong at 1x speed
	blog(LOG_INFO,
	     "[" PLUGIN_ID
	     "] Buffer config: %d seconds content, skip=%d frames, effective fps=%.1f, max_frames=%zu (ping-pong at 1x = %.1f seconds)",
	     lf->buffer_seconds, lf->capture_skip_frames, effective_fps, lf->max_frames, base_playback);
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
	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf)
		return;

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
	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf)
		return;

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
			if (f)
				gs_texrender_destroy(f);
		}
	}
	obs_leave_graphics();
}

static obs_properties_t *loop_filter_properties(void *data)
{
	auto *lf = reinterpret_cast<loop_filter *>(data);

	obs_properties_t *props = obs_properties_create();

	// Store the filter pointer for callbacks
	obs_properties_set_param(props, lf, nullptr);

	// Add buffer length with callback to update duration display
	auto *buffer_prop = obs_properties_add_int(props, "buffer_seconds", "Buffer Length (seconds)", 10, 60, 1);
	obs_property_set_modified_callback(buffer_prop, [](obs_properties_t *props, obs_property_t *,
							   obs_data_t *settings) {
		// Update duration text when buffer length changes
		auto *lf = reinterpret_cast<loop_filter *>(obs_properties_get_param(props));
		if (lf) {
			lf->buffer_seconds = (int)obs_data_get_int(settings, "buffer_seconds");
			// Update duration display
			double base_duration = lf->ping_pong ? (lf->buffer_seconds * 2.0) : lf->buffer_seconds;
			double actual_duration = base_duration / lf->playback_speed;
			char duration_text[256];
			snprintf(duration_text, sizeof(duration_text),
				 "⏱️ Playback Duration: %.1f seconds at %.1fx speed", actual_duration,
				 lf->playback_speed);
			obs_property_set_description(obs_properties_get(props, "duration_info"), duration_text);
		}
		return true;
	});

	// Add ping-pong toggle with callback
	auto *pingpong_prop = obs_properties_add_bool(props, "ping_pong", "Ping-Pong (Forward/Reverse)");
	obs_property_set_modified_callback(pingpong_prop, [](obs_properties_t *props, obs_property_t *,
							     obs_data_t *settings) {
		auto *lf = reinterpret_cast<loop_filter *>(obs_properties_get_param(props));
		if (lf) {
			lf->ping_pong = obs_data_get_bool(settings, "ping_pong");
			// Update duration display
			double base_duration = lf->ping_pong ? (lf->buffer_seconds * 2.0) : lf->buffer_seconds;
			double actual_duration = base_duration / lf->playback_speed;
			char duration_text[256];
			snprintf(duration_text, sizeof(duration_text),
				 "⏱️ Playback Duration: %.1f seconds at %.1fx speed", actual_duration,
				 lf->playback_speed);
			obs_property_set_description(obs_properties_get(props, "duration_info"), duration_text);
		}
		return true;
	});

	// Add playback speed with callback
	auto *speed_prop = obs_properties_add_float_slider(props, "playback_speed", "Playback Speed", 0.1, 2.0, 0.1);
	obs_property_set_modified_callback(speed_prop, [](obs_properties_t *props, obs_property_t *,
							  obs_data_t *settings) {
		auto *lf = reinterpret_cast<loop_filter *>(obs_properties_get_param(props));
		if (lf) {
			lf->playback_speed = obs_data_get_double(settings, "playback_speed");
			// Update duration display immediately
			double base_duration = lf->ping_pong ? (lf->buffer_seconds * 2.0) : lf->buffer_seconds;
			double actual_duration = base_duration / lf->playback_speed;
			char duration_text[256];
			snprintf(duration_text, sizeof(duration_text),
				 "⏱️ Playback Duration: %.1f seconds at %.1fx speed", actual_duration,
				 lf->playback_speed);
			obs_property_set_description(obs_properties_get(props, "duration_info"), duration_text);
		}
		return true;
	});

	// Add playback duration info as separate text field
	if (lf) {
		double base_duration = lf->ping_pong ? (lf->buffer_seconds * 2.0) : lf->buffer_seconds;
		double actual_duration = base_duration / lf->playback_speed;
		char duration_text[256];
		snprintf(duration_text, sizeof(duration_text), "⏱️ Playback Duration: %.1f seconds at %.1fx speed",
			 actual_duration, lf->playback_speed);
		obs_properties_add_text(props, "duration_info", duration_text, OBS_TEXT_INFO);
	}

	// Add buffer status info - this will show current state but won't auto-update
	// (auto-updates interfere with slider dragging)
	if (lf) {
		std::lock_guard<std::mutex> lk(lf->frames_mtx);
		size_t frame_count = lf->frames.size();
		double content_seconds = frame_count * lf->capture_skip_frames / lf->fps;
		char status_text[256];

		if (lf->loop_enabled) {
			snprintf(status_text, sizeof(status_text),
				 "🔄 LOOPING: %zu frames (%.1f sec content) | Press Stop to update status", frame_count,
				 content_seconds);
		} else if (frame_count > 0) {
			int percent = (int)((frame_count * 100) / lf->max_frames);
			if (frame_count >= lf->max_frames) {
				snprintf(status_text, sizeof(status_text),
					 "✅ BUFFER FULL: %zu frames (%.1f seconds) - Ready to loop!", frame_count,
					 content_seconds);
			} else {
				snprintf(status_text, sizeof(status_text),
					 "📼 RECORDING: %zu/%zu frames (%d%%) | %.1f/%.1f seconds", frame_count,
					 lf->max_frames, percent, content_seconds, (double)lf->buffer_seconds);
			}
		} else {
			snprintf(status_text, sizeof(status_text),
				 "⏸️ READY: Buffer empty - video will be captured when playing");
		}

		auto *status_prop = obs_properties_add_text(props, "buffer_status", "Buffer Status", OBS_TEXT_INFO);
		obs_property_set_description(status_prop, status_text);
	}

	// A button to toggle loop state from UI
	const char *button_text = lf && lf->loop_enabled ? "Stop Loop ⏹" : "Start Loop ▶";
	obs_properties_add_button(
		props, "toggle_loop", button_text, [](obs_properties_t *, obs_property_t *prop, void *data) -> bool {
			auto *lf = reinterpret_cast<loop_filter *>(data);
			if (!lf)
				return false;

			lf->loop_enabled = !lf->loop_enabled;

			std::lock_guard<std::mutex> lk(lf->frames_mtx);
			size_t frame_count = lf->frames.size();

			if (lf->loop_enabled) {
				if (frame_count > 0) {
					lf->play_index = frame_count - 1;
					lf->direction = -1;
					lf->frame_accum = 0.0;
					lf->total_loops = 0;
					double content_seconds = frame_count * lf->capture_skip_frames / lf->fps;
					double playback_seconds = content_seconds / lf->playback_speed;
					if (lf->ping_pong)
						playback_seconds *= 2.0;
					blog(LOG_INFO,
					     "[" PLUGIN_ID
					     "] Loop STARTED: %zu frames = %.1f seconds content, playback at %.1fx = ~%.1f seconds",
					     frame_count, content_seconds, lf->playback_speed, playback_seconds);
					obs_property_set_description(prop, "Stop Loop ⏹");
				} else {
					blog(LOG_WARNING, "[" PLUGIN_ID "] No frames buffered yet!");
					lf->loop_enabled = false;
				}
			} else {
				blog(LOG_INFO, "[" PLUGIN_ID "] Loop STOPPED after %d complete cycles",
				     lf->total_loops / 2);
				obs_property_set_description(prop, "Start Loop ▶");
			}
			return true;
		});

	// A button to clear the buffer
	obs_properties_add_button(
		props, "clear_buffer", "Clear Buffer 🗑️", [](obs_properties_t *, obs_property_t *, void *data) -> bool {
			auto *lf = reinterpret_cast<loop_filter *>(data);
			if (!lf) {
				blog(LOG_ERROR, "[" PLUGIN_ID "] Clear buffer: filter is null!");
				return false;
			}

			blog(LOG_INFO, "[" PLUGIN_ID "] Clear buffer button pressed");

			// Stop looping first if active
			if (lf->loop_enabled) {
				lf->loop_enabled = false;
				// Update button through properties refresh
				obs_source_update_properties(lf->context);
			}

			// Clear all frames with extra safety checks
			size_t frame_count = 0;
			obs_enter_graphics();
			{
				std::lock_guard<std::mutex> lk(lf->frames_mtx);
				frame_count = lf->frames.size();
				if (frame_count > 0) {
					clear_frames_locked(lf);
					// Reset capture tracking
					lf->capture_start_time = 0;
					lf->frames_captured_count = 0;
					lf->last_logged_frame_count = 0;
					lf->last_capture_time = 0;
				}
			}
			obs_leave_graphics();

			blog(LOG_INFO, "[" PLUGIN_ID "] Buffer CLEARED: %zu frames removed", frame_count);

			// Force UI update to show empty buffer
			obs_source_update_properties(lf->context);

			return true;
		});

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
	if (!data)
		return;

	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf || !lf->context)
		return;

	// Update UI periodically when recording to show buffer fill progress
	// But NOT when looping (to avoid interfering with controls)
	if (!lf->loop_enabled) {
		lf->last_ui_update += seconds;
		if (lf->last_ui_update >= 1.0) {
			lf->last_ui_update = 0.0;
			// Update properties if we're actively recording or just filled
			std::lock_guard<std::mutex> lk(lf->frames_mtx);
			size_t frame_count = lf->frames.size();
			if (frame_count > 0) {
				// Always update when buffer just filled
				bool just_filled =
					(lf->last_logged_frame_count < lf->max_frames && frame_count >= lf->max_frames);
				if (just_filled || frame_count < lf->max_frames) {
					obs_source_update_properties(lf->context);
					if (just_filled) {
						blog(LOG_INFO, "[" PLUGIN_ID "] Buffer FULL! %zu frames captured",
						     frame_count);
					}
				}
				lf->last_logged_frame_count = frame_count;
			}
		}
	}

	// Update dimensions
	uint32_t w = obs_source_get_base_width(lf->context);
	uint32_t h = obs_source_get_base_height(lf->context);

	if (w != lf->base_w || h != lf->base_h) {
		blog(LOG_INFO, "[" PLUGIN_ID "] Dimensions changed: %ux%u -> %ux%u", lf->base_w, lf->base_h, w, h);

		// Clear buffer on resolution change to avoid mixing different resolutions
		if (lf->base_w > 0 && lf->base_h > 0 && (w != lf->base_w || h != lf->base_h)) {
			obs_enter_graphics();
			{
				std::lock_guard<std::mutex> lk(lf->frames_mtx);
				if (lf->frames.size() > 0) {
					blog(LOG_INFO, "[" PLUGIN_ID "] Clearing %zu frames due to resolution change",
					     lf->frames.size());
					clear_frames_locked(lf);
					lf->capture_start_time = 0;
					lf->frames_captured_count = 0;
					lf->last_logged_frame_count = 0;
				}
			}
			obs_leave_graphics();

			// Stop looping if active
			if (lf->loop_enabled) {
				lf->loop_enabled = false;
				obs_source_update_properties(lf->context);
			}
		}

		lf->base_w = w;
		lf->base_h = h;
		lf->dimensions_valid = (w > 0 && h > 0);
		lf->last_width = w;
		lf->last_height = h;

		// Recalculate buffer with new dimensions for memory limits
		if (lf->dimensions_valid) {
			recalc_buffer(lf);
		}
	}

	if (!lf->loop_enabled || !lf->dimensions_valid) {
		return;
	}

	// Advance playback cursor based on playback_speed
	// Our buffer contains frames that represent content at the capture rate
	// We want to play them back at a rate that stretches them to the original duration
	// 300 frames over 10 seconds = 30 fps playback rate at 1x speed
	double frames_per_second = (lf->frames.size() / (double)lf->buffer_seconds) * lf->playback_speed;
	double step = seconds * frames_per_second;
	lf->frame_accum += step;

	// Prevent overflow - reset accumulator if it gets too large
	if (lf->frame_accum > 1000000.0) {
		blog(LOG_WARNING, "[" PLUGIN_ID "] Frame accumulator overflow protection triggered");
		lf->frame_accum = 0.0;
	}

	size_t frames_to_advance = (size_t)lf->frame_accum;
	lf->frame_accum -= (double)frames_to_advance;

	if (frames_to_advance == 0)
		return;

	std::lock_guard<std::mutex> lk(lf->frames_mtx);
	if (lf->frames.size() < 2)
		return;

	for (size_t i = 0; i < frames_to_advance; ++i) {
		// Move the index
		if (lf->direction > 0) {
			if (lf->play_index + 1 >= lf->frames.size()) {
				if (lf->ping_pong) {
					lf->direction = -1;
					if (lf->play_index > 0)
						lf->play_index--;
					lf->total_loops++;
					// Prevent overflow of loop counter
					if (lf->total_loops > 1000000) {
						lf->total_loops = 0;
					}
				} else {
					lf->play_index = 0;
					lf->total_loops++;
					// Prevent overflow of loop counter
					if (lf->total_loops > 1000000) {
						lf->total_loops = 0;
					}
				}
			} else {
				lf->play_index++;
			}
		} else {
			if (lf->play_index == 0) {
				if (lf->ping_pong) {
					lf->direction = +1;
					if (lf->frames.size() > 1)
						lf->play_index++;
					lf->total_loops++;
					// Prevent overflow of loop counter
					if (lf->total_loops > 1000000) {
						lf->total_loops = 0;
					}
				} else {
					lf->play_index = lf->frames.size() - 1;
					lf->total_loops++;
					// Prevent overflow of loop counter
					if (lf->total_loops > 1000000) {
						lf->total_loops = 0;
					}
				}
			} else {
				lf->play_index--;
			}
		}
	}
}

static void loop_filter_render(void *data, gs_effect_t *effect)
{
	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf || !lf->context)
		return;

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
		// Keep mutex locked while accessing frame to prevent race condition
		std::lock_guard<std::mutex> lk(lf->frames_mtx);

		if (!lf->frames.empty() && lf->play_index < lf->frames.size()) {
			gs_texrender_t *frame_to_draw = lf->frames[lf->play_index];

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
		}
		// If no valid frame, skip the filter
		obs_source_skip_video_filter(lf->context);
		return;
	}

	// Default: capture source to buffer if not looping
	// First, we need to capture the current frame
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (!texrender) {
		blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to create texrender for capture");
		obs_source_skip_video_filter(lf->context);
		return;
	}

	// Use RAII-style cleanup to ensure texrender is always destroyed
	bool render_success = false;

	if (gs_texrender_begin(texrender, w, h)) {
		vec4 clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
		gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);

		// Render parent source
		obs_source_t *parent = obs_filter_get_parent(lf->context);
		if (parent) {
			obs_source_video_render(parent);
		}

		gs_texrender_end(texrender);
		render_success = true;
	}

	if (render_success) {

		// Capture frame to buffer based on time intervals to ensure correct timing
		uint64_t current_time = os_gettime_ns();

		// Calculate minimum time between captures based on desired frame rate
		// We want to capture at effective_fps = fps / capture_skip_frames
		uint64_t min_capture_interval = (uint64_t)(1000000000.0 * lf->capture_skip_frames / lf->fps);

		// Check if enough time has passed since last capture
		if (current_time - lf->last_capture_time >= min_capture_interval) {
			lf->last_capture_time = current_time;

			// Only capture if not looping
			std::lock_guard<std::mutex> lk(lf->frames_mtx);
			if (!lf->loop_enabled) {

				// Track capture start
				if (lf->frames.empty() && lf->capture_start_time == 0) {
					lf->capture_start_time = current_time;
					lf->frames_captured_count = 0;
					blog(LOG_INFO,
					     "[" PLUGIN_ID
					     "] Starting buffer capture at fps=%.2f, target capture rate=%.2f fps",
					     lf->fps, lf->fps / lf->capture_skip_frames);
				}

				// Create new texrender for this frame
				gs_texrender_t *frame_copy = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
				if (!frame_copy) {
					blog(LOG_ERROR, "[" PLUGIN_ID "] Failed to create frame copy texrender");
				} else if (gs_texrender_begin(frame_copy, w, h)) {
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
					lf->frames_captured_count++;

					if (lf->frames.size() > lf->max_frames) {
						auto *oldest = lf->frames.front();
						lf->frames.pop_front();
						if (oldest)
							gs_texrender_destroy(oldest);
					}

					// Log periodically and when buffer fills
					if (!lf->loop_enabled) {
						static int log_counter = 0;
						bool should_log = (++log_counter % 30 == 0) ||
								  (lf->frames.size() == lf->max_frames);

						if (should_log && lf->capture_start_time > 0) {
							double elapsed =
								(current_time - lf->capture_start_time) / 1000000000.0;
							double buffer_seconds =
								lf->frames.size() * lf->capture_skip_frames / lf->fps;
							double capture_rate = lf->frames_captured_count / elapsed;
							blog(LOG_INFO,
							     "[" PLUGIN_ID
							     "] Buffer: %zu/%zu frames (%.1f/%.1f sec content) | Elapsed: %.1fs | Capture rate: %.1f fps",
							     lf->frames.size(), lf->max_frames, buffer_seconds,
							     (double)lf->buffer_seconds, elapsed, capture_rate);

							if (lf->frames.size() == lf->max_frames) {
								blog(LOG_INFO,
								     "[" PLUGIN_ID
								     "] Buffer FILLED in %.1f seconds (expected ~%d seconds) - Timing %s",
								     elapsed, lf->buffer_seconds,
								     (elapsed < lf->buffer_seconds * 0.9) ? "TOO FAST!"
													  : "OK");
								// Reset for next capture but don't clear start time for continued logging
							}
						}
					}
				} else if (frame_copy) {
					gs_texrender_destroy(frame_copy);
				}
			}
		}
	}

	// Always cleanup texrender
	gs_texrender_destroy(texrender);

	// Pass through the source video
	obs_source_skip_video_filter(lf->context);
}

static void loop_filter_show(void *data)
{
	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf)
		return;

	blog(LOG_INFO, "[" PLUGIN_ID "] Filter shown - clearing buffer to start fresh");

	// Clear buffer when filter is shown to avoid stale content
	obs_enter_graphics();
	{
		std::lock_guard<std::mutex> lk(lf->frames_mtx);
		if (lf->frames.size() > 0) {
			clear_frames_locked(lf);
			// Reset all capture tracking
			lf->capture_start_time = 0;
			lf->frames_captured_count = 0;
			lf->last_logged_frame_count = 0;
			lf->last_capture_time = 0;
		}
	}
	obs_leave_graphics();

	// Stop looping if it was active
	if (lf->loop_enabled) {
		lf->loop_enabled = false;
		// Properties will update on next refresh
	}
}

static void loop_filter_hide(void *data)
{
	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf)
		return;

	blog(LOG_INFO, "[" PLUGIN_ID "] Filter hidden - stopping loop and clearing buffer");

	// Stop looping when hidden
	if (lf->loop_enabled) {
		lf->loop_enabled = false;
		// Properties will update on next refresh
	}

	// Clear buffer when filter is hidden
	obs_enter_graphics();
	{
		std::lock_guard<std::mutex> lk(lf->frames_mtx);
		if (lf->frames.size() > 0) {
			size_t frame_count = lf->frames.size();
			clear_frames_locked(lf);
			blog(LOG_INFO, "[" PLUGIN_ID "] Cleared %zu frames on hide", frame_count);
			// Reset all capture tracking
			lf->capture_start_time = 0;
			lf->frames_captured_count = 0;
			lf->last_logged_frame_count = 0;
			lf->last_capture_time = 0;
		}
	}
	obs_leave_graphics();
}

// ----------------------------- Hotkeys -----------------------------

static void loop_filter_toggle_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *lf = reinterpret_cast<loop_filter *>(data);
	if (!lf)
		return;

	lf->loop_enabled = !lf->loop_enabled;

	std::lock_guard<std::mutex> lk(lf->frames_mtx);
	size_t frame_count = lf->frames.size();

	if (lf->loop_enabled) {
		if (frame_count > 0) {
			lf->play_index = frame_count - 1;
			lf->direction = -1;
			lf->frame_accum = 0.0;
			lf->total_loops = 0;
			double content_seconds = frame_count * lf->capture_skip_frames / lf->fps;
			double playback_seconds = content_seconds / lf->playback_speed;
			if (lf->ping_pong)
				playback_seconds *= 2.0;
			blog(LOG_INFO,
			     "[" PLUGIN_ID
			     "] Hotkey: Loop STARTED - %zu frames = %.1f seconds content, playback at %.1fx = ~%.1f seconds",
			     frame_count, content_seconds, lf->playback_speed, playback_seconds);
			// Properties will update on next refresh
			obs_source_update_properties(lf->context);
		} else {
			blog(LOG_WARNING, "[" PLUGIN_ID "] No frames in buffer!");
			lf->loop_enabled = false;
		}
	} else {
		blog(LOG_INFO, "[" PLUGIN_ID "] Hotkey: Loop STOPPED after %d complete cycles", lf->total_loops / 2);
		// Properties will update on next refresh
		obs_source_update_properties(lf->context);
	}
}

static void loop_filter_register_hotkeys(loop_filter *lf)
{
	lf->hotkey_toggle =
		obs_hotkey_register_source(lf->context, "looper_toggle", "Looper: Toggle", loop_filter_toggle_cb, lf);
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
	loop_filter_info.show = loop_filter_show;
	loop_filter_info.hide = loop_filter_hide;

	obs_register_source(&loop_filter_info);

	blog(LOG_INFO, "[" PLUGIN_ID "] Module loaded successfully");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[" PLUGIN_ID "] Module unloaded");
}
