// Ksila — Android entry point.
//
// Uses NativeActivity + the NDK's native_app_glue (no Java code at all:
// android:hasCode="false"). The window is a plain ANativeWindow rendered
// with Vulkan via VK_KHR_android_surface.
//
// Input: touch acts as the mouse (tap = press, drag = move, release = click);
// the system Back button quits the app.

#include <android/log.h>
// The NDK ships this header directly in sources/android/native_app_glue/,
// which is on the include path via the native_app_glue CMake target.
#include <android_native_app_glue.h>

#include <chrono>
#include <cmath>
#include <csignal>

#include "font_data.h"
#include "renderer.h"
#include "ui.h"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "Ksila", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "Ksila", __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, "Ksila", __VA_ARGS__))

namespace {

struct AppState {
	ksila::FontAtlas font;
	ksila::Renderer renderer;
	ksila::Lobby lobby;
	bool renderer_ready = false;
	bool init_failed = false;
	bool resumed = false;
	bool focused = true;
};

// Turns silent native crashes into logcat entries (tag "Ksila").
void crash_handler(int p_signal) {
	__android_log_print(ANDROID_LOG_ERROR, "Ksila",
			"FATAL: signal %d. Пришлите вывод 'adb logcat -s Ksila DEBUG' для диагностики.",
			p_signal);
	::signal(p_signal, SIG_DFL);
	::raise(p_signal);
}

void install_crash_handlers() {
	const int crash_signals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
	for (int sig : crash_signals) {
		::signal(sig, crash_handler);
	}
}

void destroy_renderer(AppState *p_state) {
	if (p_state->renderer_ready) {
		p_state->renderer.shutdown();
		p_state->renderer_ready = false;
		LOGI("renderer destroyed");
	}
}

bool init_renderer(android_app *p_app, AppState *p_state) {
	if (p_app->window == nullptr) {
		return false;
	}
	const int w = ANativeWindow_getWidth(p_app->window);
	const int h = ANativeWindow_getHeight(p_app->window);
	if (w <= 0 || h <= 0) {
		return false;
	}
	LOGI("initializing Vulkan renderer (%dx%d)...", w, h);
	if (!p_state->renderer.init(p_app->window, p_state->font, /*p_validation=*/false)) {
		LOGE("renderer init failed (нет Vulkan / поверхность не создалась)");
		return false;
	}
	p_state->renderer_ready = true;
	LOGI("renderer ready (%dx%d)", w, h);
	return true;
}

void handle_cmd(android_app *p_app, int32_t p_cmd) {
	AppState *state = static_cast<AppState *>(p_app->userData);
	switch (p_cmd) {
		case APP_CMD_INIT_WINDOW:
			// The window (re)appeared — (re)create the renderer.
			if (!state->renderer_ready && !state->init_failed) {
				if (!init_renderer(p_app, state)) {
					// Clean exit instead of a black ANR screen.
					state->init_failed = true;
					ANativeActivity_finish(p_app->activity);
				}
			}
			break;
		case APP_CMD_TERM_WINDOW:
		case APP_CMD_DESTROY:
			// The window is going away — release all Vulkan objects.
			destroy_renderer(state);
			break;
		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONFIG_CHANGED:
			if (state->renderer_ready) {
				state->renderer.notify_resize();
			}
			break;
		case APP_CMD_RESUME:
			state->resumed = true;
			break;
		case APP_CMD_PAUSE:
			state->resumed = false;
			break;
		case APP_CMD_GAINED_FOCUS:
			state->focused = true;
			break;
		case APP_CMD_LOST_FOCUS:
			state->focused = false;
			break;
		default:
			break;
	}
}

int32_t handle_input(android_app *p_app, AInputEvent *p_event) {
	AppState *state = static_cast<AppState *>(p_app->userData);
	const int32_t type = AInputEvent_getType(p_event);

	if (type == AINPUT_EVENT_TYPE_MOTION) {
		const int32_t action = AMotionEvent_getAction(p_event) & AMOTION_EVENT_ACTION_MASK;
		const float x = AMotionEvent_getX(p_event, 0);
		const float y = AMotionEvent_getY(p_event, 0);
		switch (action) {
			case AMOTION_EVENT_ACTION_DOWN:
				state->lobby.on_mouse_button(x, y, true);
				return 1;
			case AMOTION_EVENT_ACTION_MOVE:
				state->lobby.on_mouse_move(x, y);
				return 1;
			case AMOTION_EVENT_ACTION_UP:
			case AMOTION_EVENT_ACTION_CANCEL:
				state->lobby.on_mouse_button(x, y, false);
				return 1;
			default:
				break;
		}
		return 0;
	}

	if (type == AINPUT_EVENT_TYPE_KEY) {
		const int32_t code = AKeyEvent_getKeyCode(p_event);
		const int32_t action = AKeyEvent_getAction(p_event);
		if (code == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_UP) {
			ANativeActivity_finish(p_app->activity);
			return 1;
		}
		if (action == AKEY_EVENT_ACTION_DOWN) {
			// Reuse the desktop keyboard paths (GLFW key codes).
			if (code == AKEYCODE_ENTER || code == AKEYCODE_NUMPAD_ENTER || code == AKEYCODE_DPAD_CENTER) {
				state->lobby.on_key(257 /*ENTER*/, 1 /*PRESS*/);
				return 1;
			}
			if (code == AKEYCODE_TAB || code == AKEYCODE_DPAD_DOWN) {
				state->lobby.on_key(258 /*TAB*/, 1 /*PRESS*/);
				return 1;
			}
			if (code == AKEYCODE_DPAD_UP) {
				state->lobby.on_key(265 /*UP*/, 1 /*PRESS*/);
				return 1;
			}
		}
	}
	return 0;
}

} // namespace

void android_main(android_app *p_app) {
	install_crash_handlers();
	LOGI("ksila native started");

	AppState state;
	p_app->userData = &state;
	p_app->onAppCmd = handle_cmd;
	p_app->onInputEvent = handle_input;

	try {
		if (!state.font.init(FONT_REGULAR_TTF, FONT_REGULAR_TTF_SIZE, FONT_BOLD_TTF, FONT_BOLD_TTF_SIZE)) {
			LOGE("font atlas bake failed");
			ANativeActivity_finish(p_app->activity);
		}
		state.lobby.init(&state.font);

		ksila::DrawList draw_list;
		auto last_time = std::chrono::steady_clock::now();

		while (!p_app->destroyRequested) {
			// Drain all pending events. Recompute the activity EVERY iteration:
			// block (-1) only while there is nothing to render, otherwise poll
			// without blocking and keep drawing. (A stale timeout value here
			// froze the app after the startup event burst.)
			for (;;) {
				const bool active_now = state.renderer_ready && state.resumed && state.focused;
				int events = 0;
				android_poll_source *source = nullptr;
				int result = ALooper_pollAll(active_now ? 0 : -1, nullptr, &events,
						reinterpret_cast<void **>(&source));
				if (result < 0) {
					break; // no more pending events — go render
				}
				if (source != nullptr) {
					source->process(p_app, source);
				}
				if (p_app->destroyRequested) {
					break;
				}
			}
			if (p_app->destroyRequested) {
				break;
			}

			if (!state.renderer_ready || !state.resumed || !state.focused || p_app->window == nullptr) {
				continue;
			}

			const int w = ANativeWindow_getWidth(p_app->window);
			const int h = ANativeWindow_getHeight(p_app->window);
			if (w <= 0 || h <= 0) {
				continue;
			}

			auto now = std::chrono::steady_clock::now();
			double delta = std::chrono::duration<double>(now - last_time).count();
			last_time = now;
			if (delta > 0.25 || delta < 0.0) {
				delta = 0.016; // clamp after long pauses
			}
			state.lobby.update(delta);

			draw_list.clear();
			state.lobby.build(draw_list, float(w), float(h));

			state.renderer.set_framebuffer_size(w, h);
			if (!state.renderer.draw_frame(draw_list)) {
				LOGE("draw_frame failed, finishing");
				ANativeActivity_finish(p_app->activity);
			}
		}
	} catch (const std::exception &p_error) {
		LOGE("FATAL: std::exception: %s", p_error.what());
	} catch (...) {
		LOGE("FATAL: unknown C++ exception");
	}

	destroy_renderer(&state);
	LOGI("android_main exit");
}
