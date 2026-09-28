// Ksila — software preview renderer.
//
// Rasterizes the exact same DrawList the Vulkan renderer consumes (same UI
// code in src/ui.cpp) with a tiny CPU rasterizer and writes a PNG. Used for
// visual iteration and as a screenshot generator; requires no GPU.
//
// Usage:
//   ksila-preview [--width N] [--height N] [--hover 0|1|2] [--press 0|1|2]
//                 [--focus 0|1|2] [--status 0|1|2] [--ssaa N] --out file.png

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "font_data.h"
#include "ui.h"

namespace {

struct FrameBuffer {
	int w = 0, h = 0; // supersampled dimensions
	int out_w = 0, out_h = 0; // logical (output) dimensions
	std::vector<float> premult_r, premult_g, premult_b, alpha; // premultiplied

	void init(int p_w, int p_h, int p_ssaa, float p_r, float p_g, float p_b) {
		out_w = p_w;
		out_h = p_h;
		w = p_w * p_ssaa;
		h = p_h * p_ssaa;
		size_t n = size_t(w) * size_t(h);
		premult_r.assign(n, p_r);
		premult_g.assign(n, p_g);
		premult_b.assign(n, p_b);
		alpha.assign(n, 1.f);
	}

	void blend(int p_x, int p_y, float p_r, float p_g, float p_b, float p_a) {
		if (p_a <= 0.f) {
			return;
		}
		size_t idx = size_t(p_y) * size_t(w) + size_t(p_x);
		float inv = 1.f - p_a;
		premult_r[idx] = p_r * p_a + premult_r[idx] * inv;
		premult_g[idx] = p_g * p_a + premult_g[idx] * inv;
		premult_b[idx] = p_b * p_a + premult_b[idx] * inv;
		alpha[idx] = p_a + alpha[idx] * inv;
	}
};

const ksila::FontAtlas *g_font = nullptr;

float sample_atlas_bilinear(float p_u, float p_v) {
	const int w = g_font->width();
	const int h = g_font->height();
	const uint8_t *px = g_font->pixels();
	float x = p_u * float(w) - 0.5f;
	float y = p_v * float(h) - 0.5f;
	int x0 = int(std::floor(x));
	int y0 = int(std::floor(y));
	float fx = x - float(x0);
	float fy = y - float(y0);
	auto at = [&](int xx, int yy) -> float {
		xx = xx < 0 ? 0 : (xx > w - 1 ? w - 1 : xx);
		yy = yy < 0 ? 0 : (yy > h - 1 ? h - 1 : yy);
		return float(px[size_t(yy) * size_t(w) + size_t(xx)]) / 255.f;
	};
	float v00 = at(x0, y0), v10 = at(x0 + 1, y0), v01 = at(x0, y0 + 1), v11 = at(x0 + 1, y0 + 1);
	return (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy;
}

inline uint32_t unpack_r(uint32_t c) { return c & 0xFF; }
inline uint32_t unpack_g(uint32_t c) { return (c >> 8) & 0xFF; }
inline uint32_t unpack_b(uint32_t c) { return (c >> 16) & 0xFF; }
inline uint32_t unpack_a(uint32_t c) { return (c >> 24) & 0xFF; }

void rasterize(const ksila::DrawList &p_list, FrameBuffer &p_fb, int p_ssaa) {
	const int w = p_fb.w * p_ssaa;
	const int h = p_fb.h * p_ssaa;

	for (uint32_t i = 0; i + 2 < p_list.indices.size(); i += 3) {
		const ksila::Vertex &v0 = p_list.vertices[p_list.indices[i + 0]];
		const ksila::Vertex &v1 = p_list.vertices[p_list.indices[i + 1]];
		const ksila::Vertex &v2 = p_list.vertices[p_list.indices[i + 2]];

		// Supersampled positions.
		float x0 = v0.x * p_ssaa, y0 = v0.y * p_ssaa;
		float x1 = v1.x * p_ssaa, y1 = v1.y * p_ssaa;
		float x2 = v2.x * p_ssaa, y2 = v2.y * p_ssaa;

		float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
		if (area == 0.f) {
			continue;
		}
		float sign = area > 0.f ? 1.f : -1.f;

		float min_x = std::fmin(std::fmin(x0, x1), x2);
		float max_x = std::fmax(std::fmax(x0, x1), x2);
		float min_y = std::fmin(std::fmin(y0, y1), y2);
		float max_y = std::fmax(std::fmax(y0, y1), y2);
		int sx0 = int(std::floor(min_x)), sx1 = int(std::ceil(max_x));
		int sy0 = int(std::floor(min_y)), sy1 = int(std::ceil(max_y));
		sx0 = sx0 < 0 ? 0 : sx0;
		sy0 = sy0 < 0 ? 0 : sy0;
		sx1 = sx1 > w ? w : sx1;
		sy1 = sy1 > h ? h : sy1;

		for (int sy = sy0; sy < sy1; sy++) {
			for (int sx = sx0; sx < sx1; sx++) {
				float px = float(sx) + 0.5f;
				float py = float(sy) + 0.5f;
				// Edge functions; each is >= 0 inside the triangle and its
				// magnitude is proportional to the sub-triangle OPPOSITE the
				// pre-last vertex of the edge, hence the weight assignment.
				float e01 = ((x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)) * sign; // weight of v2
				float e12 = ((x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)) * sign; // weight of v0
				float e20 = ((x0 - x2) * (py - y2) - (y0 - y2) * (px - x2)) * sign; // weight of v1
				if (e01 < 0.f || e12 < 0.f || e20 < 0.f) {
					continue;
				}
				float inv_area = 1.f / (area * sign);
				float l0 = e12 * inv_area, l1 = e20 * inv_area, l2 = e01 * inv_area;

				float u = v0.u * l0 + v1.u * l1 + v2.u * l2;
				float vv = v0.v * l0 + v1.v * l1 + v2.v * l2;
				uint32_t c0 = v0.color, c1 = v1.color, c2 = v2.color;
				float r = (float(unpack_r(c0)) * l0 + float(unpack_r(c1)) * l1 + float(unpack_r(c2)) * l2) / 255.f;
				float g = (float(unpack_g(c0)) * l0 + float(unpack_g(c1)) * l1 + float(unpack_g(c2)) * l2) / 255.f;
				float b = (float(unpack_b(c0)) * l0 + float(unpack_b(c1)) * l1 + float(unpack_b(c2)) * l2) / 255.f;
				float a = (float(unpack_a(c0)) * l0 + float(unpack_a(c1)) * l1 + float(unpack_a(c2)) * l2) / 255.f;

				float mask = sample_atlas_bilinear(u, vv);
				float alpha = a * mask;
				if (alpha <= 0.001f) {
					continue;
				}
				p_fb.blend(sx, sy, r, g, b, alpha);
			}
		}
	}
}

// Downsamples the supersampled framebuffer by averaging S×S blocks.
void resolve_and_write(const FrameBuffer &p_fb, int p_ssaa, const char *p_path) {
	int w = p_fb.out_w;
	int h = p_fb.out_h;
	std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4);
	float ssn = float(p_ssaa * p_ssaa);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float r = 0, g = 0, b = 0, a = 0;
			for (int dy = 0; dy < p_ssaa; dy++) {
				for (int dx = 0; dx < p_ssaa; dx++) {
					size_t idx = (size_t(y * p_ssaa + dy)) * size_t(p_fb.w) + size_t(x * p_ssaa + dx);
					r += p_fb.premult_r[idx];
					g += p_fb.premult_g[idx];
					b += p_fb.premult_b[idx];
					a += p_fb.alpha[idx];
				}
			}
			r /= ssn;
			g /= ssn;
			b /= ssn;
			a /= ssn;
			float ir = a > 0.0001f ? r / a : 0.f;
			float ig = a > 0.0001f ? g / a : 0.f;
			float ib = a > 0.0001f ? b / a : 0.f;
			unsigned char *px = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
			px[0] = (unsigned char)(ir * 255.f + 0.5f);
			px[1] = (unsigned char)(ig * 255.f + 0.5f);
			px[2] = (unsigned char)(ib * 255.f + 0.5f);
			px[3] = (unsigned char)(a * 255.f + 0.5f);
		}
	}
	if (!stbi_write_png(p_path, w, h, 4, rgba.data(), w * 4)) {
		std::fprintf(stderr, "preview: failed to write %s\n", p_path);
	}
}

int run(int p_argc, char **p_argv) {
	int width = 1280, height = 720;
	int hover = -1, press = -1, focus = -1, status = -1, ssaa = 4;
	const char *out = "preview.png";
	for (int i = 1; i < p_argc; i++) {
		auto next = [&](int &val) {
			val = i + 1 < p_argc ? std::atoi(p_argv[++i]) : val;
		};
		if (std::strcmp(p_argv[i], "--width") == 0) next(width);
		else if (std::strcmp(p_argv[i], "--height") == 0) next(height);
		else if (std::strcmp(p_argv[i], "--hover") == 0) next(hover);
		else if (std::strcmp(p_argv[i], "--press") == 0) next(press);
		else if (std::strcmp(p_argv[i], "--focus") == 0) next(focus);
		else if (std::strcmp(p_argv[i], "--status") == 0) next(status);
		else if (std::strcmp(p_argv[i], "--ssaa") == 0) next(ssaa);
		else if (std::strcmp(p_argv[i], "--out") == 0 && i + 1 < p_argc) out = p_argv[++i];
	}
	if (ssaa < 1) ssaa = 1;
	if (ssaa > 8) ssaa = 8;
	if (width < 320) width = 320;
	if (height < 240) height = 240;

	ksila::FontAtlas font;
	if (!font.init(FONT_REGULAR_TTF, FONT_REGULAR_TTF_SIZE, FONT_BOLD_TTF, FONT_BOLD_TTF_SIZE)) {
		std::fprintf(stderr, "preview: font atlas bake failed.\n");
		return 1;
	}
	g_font = &font;

	ksila::Lobby lobby;
	lobby.init(&font);

	// First build computes the layout (button rectangles).
	ksila::DrawList list;
	lobby.build(list, float(width), float(height));

	// Simulate input using the real event handlers.
	const ksila::LobbyButtonRect *rects = lobby.button_rects();
	if (hover >= 0 && hover < lobby.button_count()) {
		lobby.on_mouse_move(rects[hover].x + rects[hover].w * 0.5f, rects[hover].y + rects[hover].h * 0.5f);
	}
	if (press >= 0 && press < lobby.button_count()) {
		lobby.on_mouse_button(rects[press].x + rects[press].w * 0.5f, rects[press].y + rects[press].h * 0.5f, true);
	}
	if (focus >= 0 && focus < lobby.button_count()) {
		for (int i = 0; i <= focus; i++) {
			lobby.on_key(258 /*TAB*/, 1 /*PRESS*/); // NOLINT
		}
	}
	if (status >= 0 && status < lobby.button_count()) {
		lobby.on_mouse_button(rects[status].x + rects[status].w * 0.5f, rects[status].y + rects[status].h * 0.5f, true);
		lobby.on_mouse_button(rects[status].x + rects[status].w * 0.5f, rects[status].y + rects[status].h * 0.5f, false);
		lobby.update(0.3); // let the toast fade in
	}

	// Final build with the requested UI state.
	list.clear();
	lobby.build(list, float(width), float(height));

	FrameBuffer fb;
	fb.init(width, height, ssaa, 0.078f, 0.086f, 0.106f);
	rasterize(list, fb, ssaa);
	resolve_and_write(fb, ssaa, out);
	std::printf("preview: wrote %s (%dx%d, ssaa %d)\n", out, width, height, ssaa);
	return 0;
}

} // namespace

int main(int p_argc, char **p_argv) {
	return run(p_argc, p_argv);
}
