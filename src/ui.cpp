#include "ui.h"

#include <cmath>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace ksila {

// ---------------------------------------------------------------- geometry --

static inline int godot_corner_detail(float p_radius) {
	// Godot 4 default theme: MIN(Math::ceil(1.5 * p_corner_radius), 6)
	int detail = int(std::ceil(1.5f * p_radius));
	if (detail < 2) {
		detail = 2;
	}
	if (detail > 6) {
		detail = 6;
	}
	return detail;
}

void DrawList::rounded_rect(float p_x, float p_y, float p_w, float p_h, float p_radius, Color p_color) {
	if (p_radius <= 0.01f || p_radius * 2.f >= p_w || p_radius * 2.f >= p_h) {
		rect(p_x, p_y, p_w, p_h, p_color);
		return;
	}
	uint32_t c = color_pack(p_color);

	const float x0 = p_x, y0 = p_y, x1 = p_x + p_w, y1 = p_y + p_h;

	// Center + 4 straight edges (non-overlapping, like Godot's StyleBoxFlat).
	rect(x0 + p_radius, y0 + p_radius, p_w - 2.f * p_radius, p_h - 2.f * p_radius, p_color);
	rect(x0 + p_radius, y0, p_w - 2.f * p_radius, p_radius, p_color); // top
	rect(x0 + p_radius, y1 - p_radius, p_w - 2.f * p_radius, p_radius, p_color); // bottom
	rect(x0, y0 + p_radius, p_radius, p_h - 2.f * p_radius, p_color); // left
	rect(x1 - p_radius, y0 + p_radius, p_radius, p_h - 2.f * p_radius, p_color); // right

	const int detail = godot_corner_detail(p_radius);
	// Corner centers: (cx, cy) for each corner, arc from angle0 to angle1.
	struct Corner {
		float cx, cy, a0, a1;
	};
	const Corner corners[4] = {
		{ p_x + p_radius, p_y + p_radius, float(M_PI), 1.5f * float(M_PI) }, // top-left
		{ p_x + p_w - p_radius, p_y + p_radius, 1.5f * float(M_PI), 2.f * float(M_PI) }, // top-right
		{ p_x + p_w - p_radius, p_y + p_h - p_radius, 0.f, 0.5f * float(M_PI) }, // bottom-right
		{ p_x + p_radius, p_y + p_h - p_radius, 0.5f * float(M_PI), float(M_PI) }, // bottom-left
	};

	for (int k = 0; k < 4; k++) {
		const Corner &corner = corners[k];
		float px = corner.cx + p_radius * std::cos(corner.a0);
		float py = corner.cy + p_radius * std::sin(corner.a0);
		for (int i = 1; i <= detail; i++) {
			float t = float(i) / float(detail);
			float a = corner.a0 + (corner.a1 - corner.a0) * t;
			float nx = corner.cx + p_radius * std::cos(a);
			float ny = corner.cy + p_radius * std::sin(a);
			Vertex v[4] = {
				make_vertex(corner.cx, corner.cy, c),
				make_vertex(px, py, c),
				make_vertex(nx, ny, c),
				make_vertex(corner.cx, corner.cy, c),
			};
			// A triangle: reuse quad() with a degenerate 4th vertex.
			vertices.push_back(v[0]);
			vertices.push_back(v[1]);
			vertices.push_back(v[2]);
			uint32_t base = uint32_t(vertices.size()) - 3;
			indices.push_back(base + 0);
			indices.push_back(base + 1);
			indices.push_back(base + 2);
			px = nx;
			py = ny;
		}
	}
}

void DrawList::rounded_rect_outline(float p_x, float p_y, float p_w, float p_h, float p_radius, float p_border, Color p_color) {
	if (p_border <= 0.01f) {
		return;
	}
	if (p_radius <= 0.01f || p_radius * 2.f >= p_w || p_radius * 2.f >= p_h) {
		// Straight outline.
		rect(p_x, p_y, p_w, p_border, p_color);
		rect(p_x, p_y + p_h - p_border, p_w, p_border, p_color);
		rect(p_x, p_y + p_border, p_border, p_h - 2.f * p_border, p_color);
		rect(p_x + p_w - p_border, p_y + p_border, p_border, p_h - 2.f * p_border, p_color);
		return;
	}

	float ir = p_radius - p_border;
	if (ir < 0.f) {
		ir = 0.f;
	}
	const int detail = godot_corner_detail(p_radius);

	struct Corner {
		float cx, cy, a0, a1, icx, icy;
	};
	const float bx = p_x + p_border, by = p_y + p_border;
	const float bw = p_w - 2.f * p_border, bh = p_h - 2.f * p_border;
	const Corner corners[4] = {
		{ p_x + p_radius, p_y + p_radius, float(M_PI), 1.5f * float(M_PI), bx + ir, by + ir },
		{ p_x + p_w - p_radius, p_y + p_radius, 1.5f * float(M_PI), 2.f * float(M_PI), bx + bw - ir, by + ir },
		{ p_x + p_w - p_radius, p_y + p_h - p_radius, 0.f, 0.5f * float(M_PI), bx + bw - ir, by + bh - ir },
		{ p_x + p_radius, p_y + p_h - p_radius, 0.5f * float(M_PI), float(M_PI), bx + ir, by + bh - ir },
	};

	for (int k = 0; k < 4; k++) {
		const Corner &corner = corners[k];
		float pox = corner.cx + p_radius * std::cos(corner.a0);
		float poy = corner.cy + p_radius * std::sin(corner.a0);
		float pix = corner.icx + ir * std::cos(corner.a0);
		float piy = corner.icy + ir * std::sin(corner.a0);
		for (int i = 1; i <= detail; i++) {
			float t = float(i) / float(detail);
			float a = corner.a0 + (corner.a1 - corner.a0) * t;
			float nox = corner.cx + p_radius * std::cos(a);
			float noy = corner.cy + p_radius * std::sin(a);
			float nix = corner.icx + ir * std::cos(a);
			float niy = corner.icy + ir * std::sin(a);
			Vertex v[4] = {
				make_vertex(pox, poy, color_pack(p_color)),
				make_vertex(nox, noy, color_pack(p_color)),
				make_vertex(nix, niy, color_pack(p_color)),
				make_vertex(pix, piy, color_pack(p_color)),
			};
			quad(v);
			pox = nox;
			poy = noy;
			pix = nix;
			piy = niy;
		}
	}

	// Straight edges between the arcs.
	rect(p_x + p_radius, p_y, p_w - 2.f * p_radius, p_border, p_color);
	rect(p_x + p_radius, p_y + p_h - p_border, p_w - 2.f * p_radius, p_border, p_color);
	rect(p_x, p_y + p_radius, p_border, p_h - 2.f * p_radius, p_color);
	rect(p_x + p_w - p_border, p_y + p_radius, p_border, p_h - 2.f * p_radius, p_color);
}

// -------------------------------------------------------------------- font --

// Minimal UTF-8 decoder: returns the next codepoint and advances the pointer.
static uint32_t utf8_next(const char *&p_str) {
	const unsigned char *s = reinterpret_cast<const unsigned char *>(p_str);
	uint32_t c = *s++;
	if (c >= 0xF0) {
		c = ((c & 0x07) << 18) | (uint32_t(s[0] & 0x3F) << 12) | (uint32_t(s[1] & 0x3F) << 6) | uint32_t(s[2] & 0x3F);
		p_str += 4;
	} else if (c >= 0xE0) {
		c = ((c & 0x0F) << 12) | (uint32_t(s[0] & 0x3F) << 6) | uint32_t(s[1] & 0x3F);
		p_str += 3;
	} else if (c >= 0xC0) {
		c = ((c & 0x1F) << 6) | uint32_t(s[0] & 0x3F);
		p_str += 2;
	} else {
		p_str += 1;
	}
	return c;
}

bool FontAtlas::init(const unsigned char *p_regular_ttf, size_t p_regular_size,
		const unsigned char *p_bold_ttf, size_t p_bold_size) {
	width_ = 2048;
	height_ = 2048;
	atlas_.assign(size_t(width_) * height_, 0);

	constexpr int UI_PX = 40; // regular face bake size (buttons, footer, status)
	constexpr int TITLE_PX = 144; // bold face bake size (title)

	stbtt_pack_context pack;
	// padding keeps linear-filtered glyphs from bleeding into each other.
	if (!stbtt_PackBegin(&pack, atlas_.data(), width_, height_, width_, 3, nullptr)) {
		return false;
	}
	stbtt_PackSetOversampling(&pack, 4, 4);

	// Ranges baked for the bold face: only what the title needs (uppercase
	// Latin + space) — big sizes would not fit alongside everything else.
	struct Range {
		uint32_t first;
		int count;
	};
	static const Range ascii_ranges[] = {
		{ 0x20, 96 - 0x20 }, // ASCII 32..126
		{ 0xA0, 1 },
		{ 0xAB, 1 }, // «
		{ 0xB0, 1 }, // °
		{ 0xB7, 1 }, // ·
		{ 0xBB, 1 }, // »
		{ 0x401, 1 }, // Ё
		{ 0x410, 64 }, // А..я
		{ 0x451, 1 }, // ё
		{ 0x2013, 1 }, // –
		{ 0x2014, 1 }, // —
		{ 0x2026, 1 }, // …
	};
	static const Range title_ranges[] = {
		{ 0x20, 1 }, // space
		{ 0x41, 26 }, // A..Z
	};

	// bakes all ranges of a face at p_px, returns false if the atlas ran out
	// of space (stbtt_PackFontRange returns 0).
	auto bake = [&](FontBake &p_bake, const unsigned char *p_ttf, int p_px, const Range *p_ranges, int p_range_count) -> bool {
		p_bake.px = float(p_px);
		int ascent = 0, descent = 0, line_gap = 0;
		stbtt_fontinfo info;
		if (!stbtt_InitFont(&info, p_ttf, 0)) {
			return false;
		}
		stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
		float scale = stbtt_ScaleForPixelHeight(&info, float(p_px));
		p_bake.ascent = float(ascent) * scale;
		p_bake.descent = float(descent) * scale;
		p_bake.line_gap = float(line_gap) * scale;

		bool ok = true;
		for (int r = 0; r < p_range_count; r++) {
			const Range &range = p_ranges[r];
			std::vector<stbtt_packedchar> chars(range.count);
			if (!stbtt_PackFontRange(&pack, p_ttf, 0, float(p_px), int(range.first), range.count, chars.data())) {
				ok = false; // atlas full
				continue;
			}
			for (int i = 0; i < range.count; i++) {
				uint32_t cp = range.first + uint32_t(i);
				const stbtt_packedchar &ch = chars[i];
				if (ch.x0 >= ch.x1 && cp != ' ') {
					continue; // empty glyph
				}
				GlyphInfo g;
				g.x0 = ch.xoff;
				g.y0 = ch.yoff;
				g.x1 = ch.xoff2;
				g.y1 = ch.yoff2;
				g.u0 = float(ch.x0) / float(width_);
				g.v0 = float(ch.y0) / float(height_);
				g.u1 = float(ch.x1) / float(width_);
				g.v1 = float(ch.y1) / float(height_);
				g.advance = ch.xadvance;
				p_bake.glyphs[cp] = g;
			}
		}
		return ok;
	};

	const bool ui_ok = bake(regular_ui_, p_regular_ttf, UI_PX, ascii_ranges, int(sizeof(ascii_ranges) / sizeof(ascii_ranges[0])));
	const bool title_ok = bake(bold_title_, p_bold_ttf, TITLE_PX, title_ranges, int(sizeof(title_ranges) / sizeof(title_ranges[0])));

	stbtt_PackEnd(&pack);

	// White texel in the bottom-right corner for untextured shapes.
	atlas_[size_t(width_) * (height_ - 1) + (width_ - 1)] = 255;
	white_u_ = (float(width_) - 0.5f) / float(width_);
	white_v_ = (float(height_) - 0.5f) / float(height_);

	return ui_ok && title_ok && !regular_ui_.glyphs.empty() && !bold_title_.glyphs.empty();
}

const FontBake &FontAtlas::bake_for(bool p_bold, float p_px) const {
	// Pick the bake whose resolution is closest to the requested size.
	if (p_bold) {
		return bold_title_;
	}
	// Bold-forced sizes below ~56px also use the UI bake for crisper text.
	return regular_ui_;
}

float FontAtlas::ascent(bool p_bold, float p_px) const {
	const FontBake &b = bake_for(p_bold, p_px);
	return b.ascent * (p_px / b.px);
}

float FontAtlas::descent(bool p_bold, float p_px) const {
	const FontBake &b = bake_for(p_bold, p_px);
	return b.descent * (p_px / b.px);
}

const GlyphInfo *FontAtlas::find_glyph(bool p_bold, uint32_t p_cp, const FontBake *&r_bake) const {
	const FontBake *primary = p_bold ? &bold_title_ : &regular_ui_;
	auto it = primary->glyphs.find(p_cp);
	if (it != primary->glyphs.end()) {
		r_bake = primary;
		return &it->second;
	}
	// Fall back to the other bake (e.g. bold text with non-Latin glyphs).
	const FontBake *other = p_bold ? &regular_ui_ : &bold_title_;
	it = other->glyphs.find(p_cp);
	if (it != other->glyphs.end()) {
		r_bake = other;
		return &it->second;
	}
	r_bake = primary;
	return nullptr;
}

float FontAtlas::text_width(bool p_bold, float p_px, const char *p_utf8) const {
	float width = 0.f;
	const char *s = p_utf8;
	while (*s) {
		uint32_t cp = utf8_next(s);
		const FontBake *bake = nullptr;
		const GlyphInfo *g = find_glyph(p_bold, cp, bake);
		if (!g) {
			g = find_glyph(p_bold, '?', bake);
		}
		if (g) {
			width += g->advance * (p_px / bake->px);
		}
	}
	return width;
}

void FontAtlas::draw_text(DrawList &p_dl, bool p_bold, float p_x, float p_y, float p_px, const char *p_utf8, Color p_color) const {
	uint32_t c = color_pack(p_color);
	float pen = p_x;
	const char *s = p_utf8;
	while (*s) {
		uint32_t cp = utf8_next(s);
		const FontBake *bake = nullptr;
		const GlyphInfo *g = find_glyph(p_bold, cp, bake);
		if (!g) {
			g = find_glyph(p_bold, '?', bake);
		}
		if (!g) {
			continue;
		}
		float scale = p_px / bake->px;
		if (g->x1 > g->x0) {
			float x0 = pen + g->x0 * scale;
			float y0 = p_y + g->y0 * scale;
			float x1 = pen + g->x1 * scale;
			float y1 = p_y + g->y1 * scale;
			Vertex v[4] = {
				Vertex{ x0, y0, g->u0, g->v0, c },
				Vertex{ x1, y0, g->u1, g->v0, c },
				Vertex{ x1, y1, g->u1, g->v1, c },
				Vertex{ x0, y1, g->u0, g->v1, c },
			};
			p_dl.quad(v);
		}
		pen += g->advance * scale;
	}
}

// ------------------------------------------------------------------- lobby --

void Lobby::on_mouse_move(float p_x, float p_y) {
	mouse_x_ = p_x;
	mouse_y_ = p_y;
	hovered_ = -1;
	for (int i = 0; i < 3; i++) {
		const LobbyButtonRect &r = buttons_[i];
		if (p_x >= r.x && p_x <= r.x + r.w && p_y >= r.y && p_y <= r.y + r.h) {
			hovered_ = i;
			break;
		}
	}
	if (hovered_ >= 0) {
		focused_ = -1; // mouse takes over from keyboard focus, like Godot
	}
}

void Lobby::on_mouse_button(float p_x, float p_y, bool p_pressed) {
	on_mouse_move(p_x, p_y);
	if (p_pressed) {
		pressed_ = hovered_;
	} else if (pressed_ >= 0) {
		if (hovered_ == pressed_) {
			activate(pressed_);
		}
		pressed_ = -1;
	}
}

// GLFW key codes (kept here so the UI stays windowing-agnostic but simple).
#define KSILA_KEY_ESCAPE 256
#define KSILA_KEY_ENTER 257
#define KSILA_KEY_KP_ENTER 335
#define KSILA_KEY_TAB 258
#define KSILA_KEY_UP 265
#define KSILA_KEY_DOWN 264
#define KSILA_KEY_PRESS 1
#define KSILA_KEY_RELEASE 0

void Lobby::on_key(int p_key, int p_action) {
	if (p_action != KSILA_KEY_PRESS) {
		return;
	}
	switch (p_key) {
		case KSILA_KEY_ESCAPE:
			// Handled by the application (window close).
			break;
		case KSILA_KEY_ENTER:
		case KSILA_KEY_KP_ENTER: {
			if (focused_ >= 0) {
				pressed_ = focused_;
				activate(focused_);
				pressed_ = -1;
			}
			break;
		}
		case KSILA_KEY_TAB:
		case KSILA_KEY_DOWN:
		case KSILA_KEY_UP: {
			if (focused_ < 0) {
				focused_ = (p_key == KSILA_KEY_UP) ? 2 : 0;
			} else if (p_key == KSILA_KEY_UP) {
				focused_ = (focused_ + 2) % 3;
			} else {
				focused_ = (focused_ + 1) % 3;
			}
			break;
		}
		default:
			break;
	}
}

void Lobby::activate(int p_index) {
	static const char *messages[3] = {
		"«ИГРАТЬ» — РАЗДЕЛ В РАЗРАБОТКЕ",
		"«НАСТРОЙКИ» — РАЗДЕЛ В РАЗРАБОТКЕ",
		"«КЛАССЫ» — РАЗДЕЛ В РАЗРАБОТКЕ",
	};
	status_text_ = messages[p_index];
	status_timer_ = 2.8;
	focused_ = p_index;
}

void Lobby::update(double p_delta) {
	if (status_timer_ > 0.0) {
		status_timer_ -= p_delta;
		if (status_timer_ <= 0.0) {
			status_timer_ = 0.0;
			status_text_.clear();
		}
	}
}

void Lobby::build(DrawList &p_dl, float p_w, float p_h) const {
	font_->set_white_uv(p_dl);
	p_dl.reserve(220);

	const float s = std::fmin(p_h / DESIGN_H, p_w / 920.f); // UI scale (720p design)

	// ---- background: subtle vertical gradient --------------------------------
	p_dl.rect_vgrad(0.f, 0.f, p_w, p_h,
			make_color(0.135f, 0.153f, 0.180f),
			make_color(0.078f, 0.086f, 0.106f));

	// ---- title ----------------------------------------------------------------
	const float title_px = TITLE_FONT * s;
	const float title_baseline = 96.f * s + font_->ascent(true, title_px);
	const char *title = "KSILA";
	float tw = font_->text_width(true, title_px, title);
	font_->draw_text(p_dl, true, p_w * 0.5f - tw * 0.5f, title_baseline, title_px, title,
			make_color(0.95f, 0.95f, 0.95f));

	// Subtitle in Godot Blue (#478cbf).
	const float sub_px = SUBTITLE_FONT * s;
	const char *subtitle = "ЛОББИ · VULKAN API";
	float sw = font_->text_width(false, sub_px, subtitle);
	font_->draw_text(p_dl, false, p_w * 0.5f - sw * 0.5f, title_baseline + 34.f * s, sub_px, subtitle,
			godot::blue);

	// ---- buttons ---------------------------------------------------------------
	static const char *labels[3] = { "ИГРАТЬ", "НАСТРОЙКИ", "КЛАССЫ" };

	const float bw = BUTTON_W * s;
	const float bh = BUTTON_H * s;
	const float gap = BUTTON_GAP * s;
	const float radius = BUTTON_RADIUS * s;
	const float font_px = BUTTON_FONT * s;

	float start_y = p_h * 0.42f;
	float total_h = 3.f * bh + 2.f * gap;
	if (start_y + total_h > p_h - 70.f * s) {
		start_y = p_h - 70.f * s - total_h;
	}

	for (int i = 0; i < 3; i++) {
		float bx = p_w * 0.5f - bw * 0.5f;
		float by = start_y + float(i) * (bh + gap);

		buttons_[i] = LobbyButtonRect{ bx, by, bw, bh };

		Color style = godot::style_normal;
		Color text_color = godot::font_normal;
		if (pressed_ == i) {
			style = godot::style_pressed;
			text_color = godot::font_pressed;
		} else if (hovered_ == i) {
			style = godot::style_hover;
			text_color = godot::font_hover;
		}

		p_dl.rounded_rect(bx, by, bw, bh, radius, style);

		// Text centered in the button (baseline at ascent from the middle).
		float text_w = font_->text_width(false, font_px, labels[i]);
		float baseline = by + bh * 0.5f + font_->ascent(false, font_px) * 0.5f - 1.f * s;
		font_->draw_text(p_dl, false, bx + (bw - text_w) * 0.5f, baseline, font_px, labels[i], text_color);

		// Keyboard focus outline (Godot: 2px, rgba(1,1,1,0.75), no fill).
		if (focused_ == i) {
			p_dl.rounded_rect_outline(bx, by, bw, bh, radius, 2.f * s, godot::style_focus);
		}
	}

	// ---- status toast -----------------------------------------------------------
	if (status_timer_ > 0.0 && !status_text_.empty()) {
		float alpha = 1.f;
		if (status_timer_ > 2.55) {
			alpha = float((2.8 - status_timer_) / 0.25); // fade in
		} else if (status_timer_ < 0.45) {
			alpha = float(status_timer_ / 0.45); // fade out
		}
		if (alpha < 0.f) alpha = 0.f;
		if (alpha > 1.f) alpha = 1.f;

		const float st_px = 17.f * s;
		float stw = font_->text_width(false, st_px, status_text_.c_str());
		Color c = color_lerp(make_color(0.95f, 0.95f, 0.95f, 0.f), make_color(0.95f, 0.95f, 0.95f, 1.f), alpha);
		font_->draw_text(p_dl, false, p_w * 0.5f - stw * 0.5f, start_y + total_h + 44.f * s, st_px,
				status_text_.c_str(), c);
	}

	// ---- footer -----------------------------------------------------------------
	const float f_px = FOOTER_FONT * s;
	const float f_baseline = p_h - 20.f * s;
	const char *left = "KSILA v0.1.0";
	const char *right = "ESC — ВЫХОД";
	font_->draw_text(p_dl, false, 20.f * s, f_baseline, f_px, left, godot::font_lower);
	float rw = font_->text_width(false, f_px, right);
	font_->draw_text(p_dl, false, p_w - 20.f * s - rw, f_baseline, f_px, right, godot::font_lower);
}

} // namespace ksila
