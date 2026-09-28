// Ksila — UI core: draw list, font atlas and the Godot-style lobby.
//
// This module is intentionally independent of Vulkan/GLFW so that the same
// geometry can be rendered by the Vulkan renderer (src/renderer.cpp) and by
// the software preview tool (tools/preview.cpp).
//
// All visual constants for buttons follow Godot 4's default theme
// (scene/theme/default_theme.cpp of the Godot Engine source):
//   - corner radius 3, corner detail min(ceil(1.5*r), 6)
//   - content margin 4, font size 16 (scaled here for a 720p lobby)
//   - style normal   = rgba(0.1, 0.1, 0.1, 0.6)
//   - style hover    = rgba(0.225, 0.225, 0.225, 0.6)
//   - style pressed  = rgba(0, 0, 0, 0.6)
//   - focus outline  = 2px rgba(1, 1, 1, 0.75), no fill
//   - font color     = 0.875 / 0.95 (hover) / 1.0 (pressed)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ksila {

// ------------------------------------------------------------------- color --

struct Color {
	float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
};

constexpr Color make_color(float p_r, float p_g, float p_b, float p_a = 1.f) {
	return Color{ p_r, p_g, p_b, p_a };
}

constexpr Color color_from_rgba8(uint8_t p_r, uint8_t p_g, uint8_t p_b, uint8_t p_a = 255) {
	return Color{ p_r / 255.f, p_g / 255.f, p_b / 255.f, p_a / 255.f };
}

inline Color color_lerp(const Color &p_a, const Color &p_b, float p_t) {
	return Color{
		p_a.r + (p_b.r - p_a.r) * p_t,
		p_a.g + (p_b.g - p_a.g) * p_t,
		p_a.b + (p_b.b - p_a.b) * p_t,
		p_a.a + (p_b.a - p_a.a) * p_t,
	};
}

// Packed R8G8B8A8 (matches VK_FORMAT_R8G8B8A8_UNORM vertex attribute).
inline uint32_t color_pack(const Color &p_c) {
	uint32_t r = uint32_t(p_c.r * 255.f + 0.5f);
	uint32_t g = uint32_t(p_c.g * 255.f + 0.5f);
	uint32_t b = uint32_t(p_c.b * 255.f + 0.5f);
	uint32_t a = uint32_t(p_c.a * 255.f + 0.5f);
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	if (a > 255) a = 255;
	return (r << 0) | (g << 8) | (b << 16) | (a << 24);
}

// Godot 4 default theme palette (extracted from Godot's source code).
namespace godot {

constexpr Color style_normal = make_color(0.100f, 0.100f, 0.100f, 0.60f); // Button/normal
constexpr Color style_hover = make_color(0.225f, 0.225f, 0.225f, 0.60f); // Button/hover
constexpr Color style_pressed = make_color(0.000f, 0.000f, 0.000f, 0.60f); // Button/pressed
constexpr Color style_focus = make_color(1.000f, 1.000f, 1.000f, 0.75f); // focus outline

constexpr Color font_normal = make_color(0.875f, 0.875f, 0.875f, 1.f);
constexpr Color font_hover = make_color(0.950f, 0.950f, 0.950f, 1.f);
constexpr Color font_pressed = make_color(1.000f, 1.000f, 1.000f, 1.f);
constexpr Color font_lower = make_color(0.650f, 0.650f, 0.650f, 1.f);

// Godot brand colors (editor color map: "Godot Blue" + GUI highlight).
constexpr Color blue = color_from_rgba8(0x47, 0x8c, 0xbf); // #478cbf
constexpr Color blue_highlight = color_from_rgba8(0x69, 0x9c, 0xe8); // #699ce8

} // namespace godot

// --------------------------------------------------------------- draw list --

struct Vertex {
	float x, y; // pixels
	float u, v; // atlas UV
	uint32_t color; // R8G8B8A8_UNORM
};

class DrawList {
public:
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	// UV of a fully white texel in the atlas (used for untextured shapes).
	float white_u = 0.99f;
	float white_v = 0.99f;

	void clear() {
		vertices.clear();
		indices.clear();
	}

	void reserve(size_t p_quads) {
		vertices.reserve(p_quads * 4);
		indices.reserve(p_quads * 6);
	}

	// Adds a quad from 4 corner points (counter-clockwise or clockwise —
	// backface culling is disabled).
	void quad(const Vertex p_v[4]) {
		uint32_t base = uint32_t(vertices.size());
		for (int i = 0; i < 4; i++) {
			vertices.push_back(p_v[i]);
		}
		indices.push_back(base + 0);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base + 0);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	}

	Vertex make_vertex(float p_x, float p_y, uint32_t p_color) const {
		return Vertex{ p_x, p_y, white_u, white_v, p_color };
	}

	void rect(float p_x, float p_y, float p_w, float p_h, Color p_color) {
		uint32_t c = color_pack(p_color);
		Vertex v[4] = {
			make_vertex(p_x, p_y, c),
			make_vertex(p_x + p_w, p_y, c),
			make_vertex(p_x + p_w, p_y + p_h, c),
			make_vertex(p_x, p_y + p_h, c),
		};
		quad(v);
	}

	// Vertical gradient (per-vertex color interpolation, like Godot's
	// StyleBoxTexture-free gradients).
	void rect_vgrad(float p_x, float p_y, float p_w, float p_h, Color p_top, Color p_bottom) {
		uint32_t ct = color_pack(p_top);
		uint32_t cb = color_pack(p_bottom);
		Vertex v[4] = {
			make_vertex(p_x, p_y, ct),
			make_vertex(p_x + p_w, p_y, ct),
			make_vertex(p_x + p_w, p_y + p_h, cb),
			make_vertex(p_x, p_y + p_h, cb),
		};
		quad(v);
	}

	// Filled rounded rectangle. Corner detail follows Godot's default theme:
	// min(ceil(1.5 * radius), 6) segments per corner.
	void rounded_rect(float p_x, float p_y, float p_w, float p_h, float p_radius, Color p_color);

	// Rounded rectangle outline (border), like Godot's focus StyleBoxFlat
	// (draw_center = false, border_width = 2).
	void rounded_rect_outline(float p_x, float p_y, float p_w, float p_h, float p_radius, float p_border, Color p_color);
};

// ------------------------------------------------------------------- font ---

struct GlyphInfo {
	// Quad relative to the pen position (baseline origin), in bake pixels.
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	// Atlas UVs.
	float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
	float advance = 0;
};

struct FontBake {
	float px = 0; // bake pixel size
	float ascent = 0, descent = 0, line_gap = 0; // bake pixels
	std::unordered_map<uint32_t, GlyphInfo> glyphs;
};

class FontAtlas {
public:
	bool init(const unsigned char *p_regular_ttf, size_t p_regular_size,
			const unsigned char *p_bold_ttf, size_t p_bold_size);

	int width() const { return width_; }
	int height() const { return height_; }
	const uint8_t *pixels() const { return atlas_.data(); } // R8, one byte per texel

	void set_white_uv(DrawList &p_dl) const {
		p_dl.white_u = white_u_;
		p_dl.white_v = white_v_;
	}

	// Width of a UTF-8 string at the requested pixel size.
	float text_width(bool p_bold, float p_px, const char *p_utf8) const;

	// Draws text with the baseline at (x, y).
	void draw_text(DrawList &p_dl, bool p_bold, float p_x, float p_y, float p_px, const char *p_utf8, Color p_color) const;

	// Metrics at the requested pixel size.
	float ascent(bool p_bold, float p_px) const;
	float descent(bool p_bold, float p_px) const;

private:
	const FontBake &bake_for(bool p_bold, float p_px) const;
	const GlyphInfo *find_glyph(bool p_bold, uint32_t p_cp, const FontBake *&r_bake) const;

	int width_ = 0;
	int height_ = 0;
	std::vector<uint8_t> atlas_;
	FontBake regular_ui_; // regular face, UI size
	FontBake bold_title_; // bold face, title size
	float white_u_ = 0.99f;
	float white_v_ = 0.99f;
};

// ------------------------------------------------------------------- lobby --

enum LobbyAction {
	LOBBY_ACTION_NONE = 0,
	LOBBY_ACTION_PLAY, // «Играть»
	LOBBY_ACTION_SETTINGS, // «Настройки»
	LOBBY_ACTION_CLASSES, // «Классы»
	LOBBY_ACTION_QUIT, // Esc / window close
};

struct LobbyButtonRect {
	float x = 0, y = 0, w = 0, h = 0;
};

class Lobby {
public:
	void init(FontAtlas *p_font) { font_ = p_font; }

	// --- input events -------------------------------------------------------
	void on_mouse_move(float p_x, float p_y);
	void on_mouse_button(float p_x, float p_y, bool p_pressed);
	void on_key(int p_key, int p_action); // GLFW key/action codes

	// --- simulation ---------------------------------------------------------
	void update(double p_delta);

	// --- rendering ----------------------------------------------------------
	// Rebuilds geometry for the given window size.
	void build(DrawList &p_dl, float p_w, float p_h) const;

	// Button rectangles in current window coordinates (filled by build()).
	const LobbyButtonRect *button_rects() const { return buttons_; }
	int button_count() const { return 3; }
	int hovered() const { return hovered_; }
	int pressed() const { return pressed_; }
	int focused() const { return focused_; }

private:
	void activate(int p_index);

	FontAtlas *font_ = nullptr;

	// Layout constants, in "design pixels" (720p reference, scaled by h/720).
	static constexpr float DESIGN_H = 720.f;
	static constexpr float BUTTON_W = 300.f;
	static constexpr float BUTTON_H = 48.f;
	static constexpr float BUTTON_GAP = 14.f;
	static constexpr float BUTTON_FONT = 20.f;
	static constexpr float BUTTON_RADIUS = 4.f;
	static constexpr float TITLE_FONT = 84.f;
	static constexpr float SUBTITLE_FONT = 19.f;
	static constexpr float FOOTER_FONT = 15.f;

	mutable LobbyButtonRect buttons_[3];
	float scale_ = 1.f;
	float mouse_x_ = -1.f, mouse_y_ = -1.f;
	int hovered_ = -1;
	int pressed_ = -1;
	int focused_ = -1;

	// Status toast («в разработке»).
	std::string status_text_;
	double status_timer_ = 0.0;
};

} // namespace ksila
