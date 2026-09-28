// Ksila — Vulkan lobby. Entry point: window, input, main loop.
#include <cstdio>
#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "font_data.h"
#include "renderer.h"
#include "ui.h"

static ksila::Renderer *g_renderer = nullptr;
static ksila::Lobby *g_lobby = nullptr;

static void glfw_framebuffer_resize(GLFWwindow *, int, int) {
	if (g_renderer) {
		g_renderer->notify_resize();
	}
}

static void glfw_cursor_pos(GLFWwindow *, double p_x, double p_y) {
	if (g_lobby) {
		g_lobby->on_mouse_move(float(p_x), float(p_y));
	}
}

static void glfw_mouse_button(GLFWwindow *p_window, int p_button, int p_action, int) {
	if (g_lobby && p_button == GLFW_MOUSE_BUTTON_LEFT) {
		double x = 0.0, y = 0.0;
		glfwGetCursorPos(p_window, &x, &y);
		g_lobby->on_mouse_button(float(x), float(y), p_action == GLFW_PRESS);
	}
}

static void glfw_key(GLFWwindow *p_window, int p_key, int, int p_action, int) {
	if (p_key == GLFW_KEY_ESCAPE && p_action == GLFW_PRESS) {
		glfwSetWindowShouldClose(p_window, GLFW_TRUE);
		return;
	}
	if (g_lobby) {
		g_lobby->on_key(p_key, p_action);
	}
}

static void print_help() {
	std::printf(
			"Ksila — лобби на Vulkan API (кнопки в стиле Godot)\n\n"
			"Использование: ksila [опции]\n"
			"  --width N       ширина окна (по умолчанию 1280)\n"
			"  --height N      высота окна (по умолчанию 720)\n"
			"  --validate      включить Vulkan validation layers\n"
			"  --help          эта справка\n\n"
			"Управление: мышь, Tab/стрелки + Enter, Esc — выход.\n");
}

int main(int p_argc, char **p_argv) {
	int width = 1280;
	int height = 720;
	bool validate = false;

	for (int i = 1; i < p_argc; i++) {
		if (std::strcmp(p_argv[i], "--width") == 0 && i + 1 < p_argc) {
			width = std::atoi(p_argv[++i]);
		} else if (std::strcmp(p_argv[i], "--height") == 0 && i + 1 < p_argc) {
			height = std::atoi(p_argv[++i]);
		} else if (std::strcmp(p_argv[i], "--validate") == 0) {
			validate = true;
		} else if (std::strcmp(p_argv[i], "--help") == 0 || std::strcmp(p_argv[i], "-h") == 0) {
			print_help();
			return 0;
		}
	}
	if (width < 320) width = 320;
	if (height < 240) height = 240;

	if (!glfwInit()) {
		std::fprintf(stderr, "Ksila: failed to initialize GLFW.\n");
		return 1;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

	GLFWwindow *window = glfwCreateWindow(width, height, "Ksila — Лобби (Vulkan)", nullptr, nullptr);
	if (!window) {
		std::fprintf(stderr, "Ksila: failed to create a window. Is there a display/Wayland/X11 session?\n");
		glfwTerminate();
		return 1;
	}

	ksila::FontAtlas font;
	if (!font.init(FONT_REGULAR_TTF, FONT_REGULAR_TTF_SIZE, FONT_BOLD_TTF, FONT_BOLD_TTF_SIZE)) {
		std::fprintf(stderr, "Ksila: failed to bake the font atlas.\n");
		glfwDestroyWindow(window);
		glfwTerminate();
		return 1;
	}

	ksila::Renderer renderer;
	ksila::Lobby lobby;
	lobby.init(&font);

	g_renderer = &renderer;
	g_lobby = &lobby;
	glfwSetFramebufferSizeCallback(window, glfw_framebuffer_resize);
	glfwSetCursorPosCallback(window, glfw_cursor_pos);
	glfwSetMouseButtonCallback(window, glfw_mouse_button);
	glfwSetKeyCallback(window, glfw_key);

	if (!renderer.init(window, font, validate)) {
		renderer.shutdown();
		glfwDestroyWindow(window);
		glfwTerminate();
		return 1;
	}

	ksila::DrawList draw_list;
	double last_time = glfwGetTime();

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		double now = glfwGetTime();
		double delta = now - last_time;
		last_time = now;

		lobby.update(delta);

		int fbw = 0, fbh = 0;
		glfwGetFramebufferSize(window, &fbw, &fbh);
		if (fbw > 0 && fbh > 0) {
			draw_list.clear();
			lobby.build(draw_list, float(fbw), float(fbh));
			if (!renderer.draw_frame(draw_list)) {
				std::fprintf(stderr, "Ksila: lost the Vulkan device, exiting.\n");
				break;
			}
		}
	}

	renderer.shutdown();
	g_renderer = nullptr;
	g_lobby = nullptr;
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
