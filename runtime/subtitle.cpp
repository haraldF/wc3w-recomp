/* The subtitles' text drawn into a movie frame with FreeType: white, a black outline, centred near
 * the bottom, its height a twentieth of the picture's, lines wrapped to 90% of its width. The font
 * is the system's (no font comes with the program): WC_SUBTITLE_FONT=file, else the first of a few
 * known ones there is. A cue's text is rasterised once and blended into each frame it is shown on
 * (on the movie's decoder thread). */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "subtitle.h"

namespace {

const char *const FONTS[] = {
	"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
	"/System/Library/Fonts/Supplemental/Arial.ttf",
	"/System/Library/Fonts/Helvetica.ttc",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
	"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
	"C:\\Windows\\Fonts\\arialbd.ttf",
};

std::mutex lock;
FT_Library library;
FT_Face face;
bool tried;

bool open_font(void)
{
	if (tried) return face != nullptr;
	tried = true;
	if (FT_Init_FreeType(&library)) return false;
	const char *e = getenv("WC_SUBTITLE_FONT");
	std::vector<const char *> files;
	if (e && *e) files.push_back(e);
	else files.assign(std::begin(FONTS), std::end(FONTS));
	for (const char *f : files)
		if (!FT_New_Face(library, f, 0, &face)) return true;
	face = nullptr;
	fprintf(stderr, "wc: no font for the subtitles (WC_SUBTITLE_FONT=file): %s\n", e && *e ? e : "none of the system's known");
	return false;
}

/* a cue's text rasterised: the letters' coverage and the outline's (0-255), mw x mh */
struct mask {
	std::u32string text;
	int frame_w = 0, frame_h = 0;
	int mw = 0, mh = 0;
	std::vector<uint8_t> fill, edge;
} cached;

int advance(char32_t c)
{
	if (FT_Load_Char(face, c, FT_LOAD_DEFAULT)) return 0;
	return (int)(face->glyph->advance.x >> 6);
}

/* the text's lines, wrapped at spaces to at most max pixels where they can be */
std::vector<std::u32string> wrap(const std::u32string &text, int max)
{
	std::vector<std::u32string> out;
	size_t from = 0;
	while (from <= text.size()) {
		size_t nl = text.find('\n', from);
		std::u32string line = text.substr(from, nl == std::u32string::npos ? std::u32string::npos : nl - from);
		from = nl == std::u32string::npos ? text.size() + 1 : nl + 1;
		while (!line.empty()) {
			int width = 0;
			size_t i = 0, space = std::u32string::npos;
			for (; i < line.size(); i++) {
				if (line[i] == ' ') space = i;
				width += advance(line[i]);
				if (width > max && space != std::u32string::npos) break;
			}
			if (i == line.size()) {
				out.push_back(line);
				break;
			}
			out.push_back(line.substr(0, space));
			line.erase(0, space + 1);
		}
	}
	return out;
}

void rasterise(const std::u32string &text, int w, int h)
{
	cached = {};
	cached.text = text, cached.frame_w = w, cached.frame_h = h;
	int px = std::max(12, h / 20);
	FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
	int ascender = (int)(face->size->metrics.ascender >> 6), line_h = (int)(face->size->metrics.height >> 6);
	int pad = std::max(1, px / 14); /* the outline's width */
	std::vector<std::u32string> lines = wrap(text, w * 9 / 10);
	std::vector<int> widths;
	int widest = 0;
	for (const auto &l : lines) {
		int x = 0;
		for (char32_t c : l) x += advance(c);
		widths.push_back(x);
		widest = std::max(widest, x);
	}
	cached.mw = widest + 2 * pad + 2, cached.mh = (int)lines.size() * line_h + 2 * pad + 2;
	cached.fill.assign((size_t)cached.mw * cached.mh, 0);
	for (size_t n = 0; n < lines.size(); n++) {
		int pen = pad + 1 + (widest - widths[n]) / 2, base = pad + 1 + (int)n * line_h + ascender;
		for (char32_t c : lines[n]) {
			if (FT_Load_Char(face, c, FT_LOAD_RENDER)) continue;
			const FT_Bitmap &b = face->glyph->bitmap;
			int x0 = pen + face->glyph->bitmap_left, y0 = base - face->glyph->bitmap_top;
			for (int y = 0; y < (int)b.rows; y++)
				for (int x = 0; x < (int)b.width; x++) {
					int mx = x0 + x, my = y0 + y;
					if (mx < 0 || my < 0 || mx >= cached.mw || my >= cached.mh) continue;
					uint8_t &d = cached.fill[(size_t)my * cached.mw + mx];
					d = std::max(d, b.buffer[y * b.pitch + x]);
				}
			pen += (int)(face->glyph->advance.x >> 6);
		}
	}
	/* the outline: the letters widened by pad in every direction (a disc) */
	cached.edge.assign(cached.fill.size(), 0);
	for (int y = 0; y < cached.mh; y++)
		for (int x = 0; x < cached.mw; x++) {
			uint8_t m = 0;
			for (int dy = -pad; dy <= pad; dy++)
				for (int dx = -pad; dx <= pad; dx++) {
					int sx = x + dx, sy = y + dy;
					if (dx * dx + dy * dy > pad * pad + pad || sx < 0 || sy < 0 || sx >= cached.mw || sy >= cached.mh) continue;
					m = std::max(m, cached.fill[(size_t)sy * cached.mw + sx]);
				}
			cached.edge[(size_t)y * cached.mw + x] = m;
		}
}

uint32_t blend(uint32_t p, uint32_t colour, unsigned a)
{
	uint32_t out = 0;
	for (int s = 0; s < 24; s += 8) {
		int c = (int)(p >> s & 0xff), t = (int)(colour >> s & 0xff);
		out |= (uint32_t)((c * (255 - (int)a) + t * (int)a + 127) / 255) << s;
	}
	return out;
}

} // namespace

bool subtitle_draw(uint32_t *pixels, int w, int h, const std::u32string &text)
{
	std::lock_guard<std::mutex> g(lock);
	if (!open_font()) return false;
	if (cached.text != text || cached.frame_w != w || cached.frame_h != h) rasterise(text, w, h);
	int x0 = (w - cached.mw) / 2, y0 = h - h / 20 - cached.mh;
	for (int y = 0; y < cached.mh; y++) {
		int py = y0 + y;
		if (py < 0 || py >= h) continue;
		for (int x = 0; x < cached.mw; x++) {
			int px = x0 + x;
			if (px < 0 || px >= w) continue;
			size_t i = (size_t)y * cached.mw + x;
			uint32_t &p = pixels[(size_t)py * w + px];
			if (cached.edge[i]) p = blend(p, 0x000000, cached.edge[i]);
			if (cached.fill[i]) p = blend(p, 0xffffff, cached.fill[i]);
		}
	}
	return true;
}
