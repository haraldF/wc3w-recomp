/* DirectDraw (version 1 and 2 interfaces): the display mode (640x480, 8 bits), surfaces in the
 * program's memory (the primary with its back buffer, off-screen ones), palettes, clippers. The
 * objects are COM objects in the program's memory: a pointer to a table of thunks, whose methods
 * run here. What the primary surface shows goes to the host (present()) at each Flip, and when the
 * program writes to the primary itself (Unlock, Blt). */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "host.h"
#include "win.h"

namespace {

enum : uint32_t {
	DD_OK = 0, DDERR_GENERIC = 0x80004005, DDERR_INVALIDPARAMS = 0x80070057, DDERR_UNSUPPORTED = 0x80004001,
	E_NOINTERFACE = 0x80004002,
	DDSD_CAPS = 1, DDSD_HEIGHT = 2, DDSD_WIDTH = 4, DDSD_PITCH = 8, DDSD_BACKBUFFERCOUNT = 0x20,
	DDSD_LPSURFACE = 0x800, DDSD_PIXELFORMAT = 0x1000,
	DDSCAPS_BACKBUFFER = 4, DDSCAPS_COMPLEX = 8, DDSCAPS_FLIP = 0x10, DDSCAPS_FRONTBUFFER = 0x20,
	DDSCAPS_OFFSCREENPLAIN = 0x40, DDSCAPS_PRIMARYSURFACE = 0x200, DDSCAPS_SYSTEMMEMORY = 0x800,
	DDSCAPS_VIDEOMEMORY = 0x4000, DDSCAPS_VISIBLE = 0x8000, DDSCAPS_LOCALVIDMEM = 0x10000000,
	DDBLT_COLORFILL = 0x400, DDBLT_KEYSRC = 0x8000, DDBLTFAST_SRCCOLORKEY = 1,
};

struct surface {
	uint32_t w, h, pitch, bits, caps;
	uint32_t back = 0;    /* the primary's back buffer (a surface object) */
	uint32_t palette = 0; /* a palette object */
	uint32_t src_key = 0;
	bool has_src_key = false;
};
std::map<uint32_t, surface> surfaces; /* by object address */
std::map<uint32_t, std::vector<uint8_t>> palettes; /* by object address: 256 x RGBA (PALETTEENTRY) */
uint32_t primary;
uint32_t mode_w = 640, mode_h = 480, mode_bpp = 8;

/* the frame the host shows */
std::mutex frame_lock;
uint8_t frame[640 * 480];
uint8_t frame_palette[256 * 3];
int frame_w = 640, frame_h = 480;
uint64_t frame_count;

uint32_t vt_dd1, vt_dd2, vt_surface, vt_palette, vt_clipper;

uint32_t make_object(uint32_t vtable, uint32_t size = 16)
{
	uint32_t o = heap_alloc(size);
	wr32(o, vtable);
	return o;
}

/* A vtable: thunks for the methods in order; nullptr ones report themselves when called. */
struct method {
	const char *name;
	int nargs; /* with this */
	win_fn fn;
};
uint32_t make_vtable(std::initializer_list<method> methods)
{
	uint32_t vt = heap_alloc(4 * (uint32_t)methods.size());
	uint32_t i = 0;
	for (const method &m : methods) wr32(vt + 4 * i++, thunk_native(m.name, m.nargs, m.fn));
	return vt;
}

void present(void)
{
	auto it = surfaces.find(primary);
	if (it == surfaces.end()) return;
	const surface &s = it->second;
	std::lock_guard<std::mutex> g(frame_lock);
	frame_w = (int)std::min(s.w, 640u);
	frame_h = (int)std::min(s.h, 480u);
	for (int y = 0; y < frame_h; y++) memcpy(frame + y * frame_w, M + s.bits + (uint32_t)y * s.pitch, (size_t)frame_w);
	auto p = palettes.find(s.palette);
	if (p != palettes.end()) {
		for (int i = 0; i < 256; i++) {
			for (int c = 0; c < 3; c++) frame_palette[3 * i + c] = p->second[4 * i + c];
		}
	}
	frame_count++;
}

/* ---- IUnknown ---- */

uint32_t AddRef() { return 1; }
uint32_t Release() { return 0; }

/* ---- IDirectDraw, IDirectDraw2 ---- */

bool is_iid(uint32_t p, uint32_t d1) { return rd32(p) == d1; }

uint32_t DD_QueryInterface()
{
	uint32_t iid = arg(1), out = arg(2);
	if (is_iid(iid, 0xb3a6f3e0)) { /* IID_IDirectDraw2 */
		wr32(out, make_object(vt_dd2));
		return DD_OK;
	}
	if (is_iid(iid, 0x6c14db80)) { /* IID_IDirectDraw */
		wr32(out, arg(0));
		return DD_OK;
	}
	if (wc_trace) fprintf(stderr, " [no interface %08x]", rd32(iid));
	wr32(out, 0);
	return E_NOINTERFACE;
}

uint32_t DD_SetCooperativeLevel() { return DD_OK; }

uint32_t DD_SetDisplayMode1()
{
	mode_w = arg(1), mode_h = arg(2), mode_bpp = arg(3);
	if (wc_trace) fprintf(stderr, " [mode %ux%ux%u]", mode_w, mode_h, mode_bpp);
	return mode_bpp == 8 && mode_w <= 640 && mode_h <= 480 ? DD_OK : DDERR_UNSUPPORTED;
}
uint32_t DD_RestoreDisplayMode() { return DD_OK; }
uint32_t DD_FlipToGDISurface() { return DD_OK; }

uint32_t DD_WaitForVerticalBlank()
{
	using namespace std::chrono;
	static auto next = steady_clock::now();
	auto now = steady_clock::now();
	next = std::max(next + microseconds(16667), now);
	std::this_thread::sleep_until(next);
	win_poll();
	return DD_OK;
}

void put_pixel_format(uint32_t pf)
{
	memset(M + pf, 0, 32);
	wr32(pf, 32);
	wr32(pf + 4, 0x20 | 0x40); /* DDPF_PALETTEINDEXED8 | DDPF_RGB */
	wr32(pf + 12, 8);
}

/* A DDSURFACEDESC (108 bytes; its dwSize says so, but programs don't always set it) */
void put_desc(uint32_t d, const surface &s)
{
	if (wc_trace && rd32(d) != 108) fprintf(stderr, " [DDSURFACEDESC of size %u]", rd32(d));
	memset(M + d, 0, 108);
	wr32(d, 108);
	wr32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE);
	wr32(d + 8, s.h);
	wr32(d + 12, s.w);
	wr32(d + 16, s.pitch);
	wr32(d + 36, s.bits);
	put_pixel_format(d + 72);
	wr32(d + 104, s.caps);
}

uint32_t new_surface(uint32_t w, uint32_t h, uint32_t caps)
{
	uint32_t o = make_object(vt_surface);
	surface s{ w, h, (w + 3) & ~3u, 0, caps };
	s.bits = region_alloc(0, s.pitch * h);
	surfaces[o] = s;
	return o;
}

uint32_t DD_CreateSurface()
{
	uint32_t d = arg(1), out = arg(2);
	uint32_t flags = rd32(d + 4), caps = rd32(d + 104);
	uint32_t o;
	if (caps & DDSCAPS_PRIMARYSURFACE) {
		o = primary = new_surface(mode_w, mode_h, caps | DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY);
		if ((flags & DDSD_BACKBUFFERCOUNT) && rd32(d + 20)) {
			surfaces[o].back = new_surface(mode_w, mode_h, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_VIDEOMEMORY);
		}
	}
	else {
		uint32_t w = flags & DDSD_WIDTH ? rd32(d + 12) : mode_w, h = flags & DDSD_HEIGHT ? rd32(d + 8) : mode_h;
		o = new_surface(w, h, caps | (caps & DDSCAPS_SYSTEMMEMORY ? 0 : DDSCAPS_VIDEOMEMORY));
	}
	if (wc_trace) fprintf(stderr, " [surface %x: %ux%u, caps %x, bits at %x]", o, surfaces[o].w, surfaces[o].h, surfaces[o].caps, surfaces[o].bits);
	wr32(out, o);
	return DD_OK;
}

uint32_t DD_CreatePalette()
{
	uint32_t entries = arg(2), out = arg(3);
	uint32_t o = make_object(vt_palette);
	std::vector<uint8_t> &p = palettes[o];
	p.assign(M + entries, M + entries + 1024);
	wr32(out, o);
	return DD_OK;
}

uint32_t DD_CreateClipper()
{
	wr32(arg(2), make_object(vt_clipper));
	return DD_OK;
}

uint32_t DD_GetCaps()
{
	for (int k = 1; k <= 2; k++) {
		uint32_t c = arg(k);
		if (!c) continue;
		uint32_t size = std::min(rd32(c), 316u); /* DDCAPS: 316 bytes in DirectX 3 */
		if (wc_trace) fprintf(stderr, " [DDCAPS of size %u]", rd32(c));
		memset(M + c, 0, size);
		wr32(c, size);
		wr32(c + 4, 0x40 | 0x200); /* DDCAPS_BLT | DDCAPS_PALETTE */
		wr32(c + 60, 8 * 1024 * 1024 >> 0); /* dwVidMemTotal */
		wr32(c + 64, 4 * 1024 * 1024);      /* dwVidMemFree */
	}
	return DD_OK;
}

uint32_t DD_GetDisplayMode()
{
	surface s{ mode_w, mode_h, mode_w, 0, 0 };
	put_desc(arg(1), s);
	wr32(arg(1) + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
	return DD_OK;
}

/* The modes there are: 640x480 and 320x200, 8 bits */
uint32_t DD_EnumDisplayModes()
{
	uint32_t context = arg(3), callback = arg(4);
	static const uint32_t modes[][2] = { { 320, 200 }, { 640, 480 } };
	uint32_t d = heap_alloc(108);
	for (const auto &m : modes) {
		wr32(d, 108);
		surface s{ m[0], m[1], m[0], 0, 0 };
		put_desc(d, s);
		wr32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
		if (!win_call(callback, { d, context })) break;
	}
	heap_free(d);
	return DD_OK;
}

uint32_t DD_GetAvailableVidMem()
{
	if (arg(2)) wr32(arg(2), 8 << 20);
	if (arg(3)) wr32(arg(3), 4 << 20);
	return DD_OK;
}

/* ---- IDirectDrawSurface ---- */

surface *get_surface(uint32_t o)
{
	auto it = surfaces.find(o);
	return it == surfaces.end() ? nullptr : &it->second;
}

uint32_t S_QueryInterface()
{
	wr32(arg(2), arg(0)); /* IDirectDrawSurface2/3 have the same methods first */
	return DD_OK;
}

uint32_t S_GetAttachedSurface()
{
	surface *s = get_surface(arg(0));
	if (!s || !s->back) return DDERR_GENERIC;
	wr32(arg(2), s->back);
	return DD_OK;
}

uint32_t S_Lock()
{
	surface *s = get_surface(arg(0));
	if (!s) return DDERR_INVALIDPARAMS;
	uint32_t rect = arg(1), d = arg(2);
	put_desc(d, *s);
	if (rect) wr32(d + 36, s->bits + rd32(rect + 4) * s->pitch + rd32(rect));
	return DD_OK;
}

uint32_t S_Unlock()
{
	if (arg(0) == primary) present();
	return DD_OK;
}

/* The primary and its back buffer swap their memory; the back buffer is the other one then. */
uint32_t S_Flip()
{
	surface *s = get_surface(arg(0));
	surface *b = s ? get_surface(s->back) : nullptr;
	if (!b) return DDERR_GENERIC;
	std::swap(s->bits, b->bits);
	present();
	win_poll();
	return DD_OK;
}

/* A rectangle (left, top, right, bottom) or the whole surface */
void get_rect(uint32_t r, const surface &s, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1)
{
	if (r) {
		*x0 = (int32_t)rd32(r), *y0 = (int32_t)rd32(r + 4), *x1 = (int32_t)rd32(r + 8), *y1 = (int32_t)rd32(r + 12);
	}
	else {
		*x0 = 0, *y0 = 0, *x1 = (int32_t)s.w, *y1 = (int32_t)s.h;
	}
}

void blit(surface &d, int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, const surface &s, int32_t sx0, int32_t sy0,
	int32_t sx1, int32_t sy1, bool keyed)
{
	int32_t dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
	std::vector<uint8_t> row((size_t)dw);
	for (int32_t y = 0; y < dh; y++) {
		int32_t ty = dy0 + y, fy = sy0 + (int32_t)((int64_t)y * sh / dh);
		if (ty < 0 || ty >= (int32_t)d.h || fy < 0 || fy >= (int32_t)s.h) continue;
		for (int32_t x = 0; x < dw; x++) {
			int32_t fx = sx0 + (int32_t)((int64_t)x * sw / dw);
			row[(size_t)x] = fx >= 0 && fx < (int32_t)s.w ? M[s.bits + (uint32_t)fy * s.pitch + (uint32_t)fx] : 0;
		}
		for (int32_t x = 0; x < dw; x++) {
			int32_t tx = dx0 + x;
			if (tx < 0 || tx >= (int32_t)d.w || (keyed && row[(size_t)x] == s.src_key)) continue;
			M[d.bits + (uint32_t)ty * d.pitch + (uint32_t)tx] = row[(size_t)x];
		}
	}
}

uint32_t S_Blt()
{
	surface *d = get_surface(arg(0));
	if (!d) return DDERR_INVALIDPARAMS;
	uint32_t src = arg(2), flags = arg(4), fx = arg(5);
	int32_t x0, y0, x1, y1;
	get_rect(arg(1), *d, &x0, &y0, &x1, &y1);
	if (flags & DDBLT_COLORFILL) {
		uint8_t c = (uint8_t)rd32(fx + 80);
		for (int32_t y = std::max(y0, 0); y < std::min(y1, (int32_t)d->h); y++) {
			for (int32_t x = std::max(x0, 0); x < std::min(x1, (int32_t)d->w); x++) M[d->bits + (uint32_t)y * d->pitch + (uint32_t)x] = c;
		}
	}
	else if (surface *s = get_surface(src)) {
		int32_t sx0, sy0, sx1, sy1;
		get_rect(arg(3), *s, &sx0, &sy0, &sx1, &sy1);
		blit(*d, x0, y0, x1, y1, *s, sx0, sy0, sx1, sy1, (flags & DDBLT_KEYSRC) && s->has_src_key);
	}
	else {
		if (wc_trace) fprintf(stderr, " [Blt flags %x not done]", flags);
	}
	if (arg(0) == primary) present();
	return DD_OK;
}

uint32_t S_BltFast()
{
	surface *d = get_surface(arg(0)), *s = get_surface(arg(3));
	if (!d || !s) return DDERR_INVALIDPARAMS;
	int32_t sx0, sy0, sx1, sy1;
	get_rect(arg(4), *s, &sx0, &sy0, &sx1, &sy1);
	int32_t x = (int32_t)arg(1), y = (int32_t)arg(2);
	blit(*d, x, y, x + sx1 - sx0, y + sy1 - sy0, *s, sx0, sy0, sx1, sy1, (arg(5) & DDBLTFAST_SRCCOLORKEY) && s->has_src_key);
	if (arg(0) == primary) present();
	return DD_OK;
}

uint32_t S_GetSurfaceDesc()
{
	surface *s = get_surface(arg(0));
	if (!s) return DDERR_INVALIDPARAMS;
	put_desc(arg(1), *s);
	return DD_OK;
}

uint32_t S_GetCaps()
{
	surface *s = get_surface(arg(0));
	wr32(arg(1), s ? s->caps : 0);
	return DD_OK;
}

uint32_t S_GetPixelFormat()
{
	put_pixel_format(arg(1));
	return DD_OK;
}

uint32_t S_SetPalette()
{
	surface *s = get_surface(arg(0));
	if (!s) return DDERR_INVALIDPARAMS;
	s->palette = arg(1);
	if (arg(0) == primary) present();
	return DD_OK;
}

uint32_t S_GetPalette()
{
	surface *s = get_surface(arg(0));
	if (!s || !s->palette) return 0x887600ff; /* DDERR_NOPALETTEATTACHED */
	wr32(arg(1), s->palette);
	return DD_OK;
}

uint32_t S_SetColorKey()
{
	surface *s = get_surface(arg(0));
	if (!s) return DDERR_INVALIDPARAMS;
	if (arg(1) & 8 /* DDCKEY_SRCBLT */) {
		s->has_src_key = arg(2) != 0;
		if (arg(2)) s->src_key = rd32(arg(2));
	}
	return DD_OK;
}

uint32_t S_IsLost() { return DD_OK; }
uint32_t S_Restore() { return DD_OK; }
uint32_t S_SetClipper() { return DD_OK; }

/* ---- IDirectDrawPalette ---- */

uint32_t P_SetEntries()
{
	auto it = palettes.find(arg(0));
	if (it == palettes.end()) return DDERR_INVALIDPARAMS;
	uint32_t start = arg(2), count = std::min(arg(3), 256 - std::min(arg(2), 256u));
	memcpy(it->second.data() + 4 * start, M + arg(4), 4 * count);
	if (wc_trace) {
		unsigned sum = 0;
		for (uint32_t i = 0; i < 4 * count; i++) sum += M[arg(4) + i];
		fprintf(stderr, " [entries %u..%u, sum %u]", start, start + count - 1, sum);
	}
	if (surface *p = get_surface(primary); p && p->palette == arg(0)) present();
	return DD_OK;
}

uint32_t P_GetEntries()
{
	auto it = palettes.find(arg(0));
	if (it == palettes.end()) return DDERR_INVALIDPARAMS;
	uint32_t start = arg(2), count = std::min(arg(3), 256 - std::min(arg(2), 256u));
	memcpy(M + arg(4), it->second.data() + 4 * start, 4 * count);
	return DD_OK;
}

/* ---- IDirectDrawClipper ---- */

uint32_t C_SetHWnd() { return DD_OK; }

void init(void)
{
	if (vt_dd1) return;
	auto dd = [](bool v2) {
		return make_vtable({
			{ "IDirectDraw::QueryInterface", 3, DD_QueryInterface }, { "IDirectDraw::AddRef", 1, AddRef },
			{ "IDirectDraw::Release", 1, Release }, { "IDirectDraw::Compact", 1, nullptr },
			{ "IDirectDraw::CreateClipper", 4, DD_CreateClipper }, { "IDirectDraw::CreatePalette", 5, DD_CreatePalette },
			{ "IDirectDraw::CreateSurface", 4, DD_CreateSurface }, { "IDirectDraw::DuplicateSurface", 3, nullptr },
			{ "IDirectDraw::EnumDisplayModes", 5, DD_EnumDisplayModes }, { "IDirectDraw::EnumSurfaces", 5, nullptr },
			{ "IDirectDraw::FlipToGDISurface", 1, DD_FlipToGDISurface }, { "IDirectDraw::GetCaps", 3, DD_GetCaps },
			{ "IDirectDraw::GetDisplayMode", 2, DD_GetDisplayMode }, { "IDirectDraw::GetFourCCCodes", 3, nullptr },
			{ "IDirectDraw::GetGDISurface", 2, nullptr }, { "IDirectDraw::GetMonitorFrequency", 2, nullptr },
			{ "IDirectDraw::GetScanLine", 2, nullptr }, { "IDirectDraw::GetVerticalBlankStatus", 2, nullptr },
			{ "IDirectDraw::Initialize", 2, nullptr }, { "IDirectDraw::RestoreDisplayMode", 1, DD_RestoreDisplayMode },
			{ "IDirectDraw::SetCooperativeLevel", 3, DD_SetCooperativeLevel },
			v2 ? method{ "IDirectDraw2::SetDisplayMode", 6, DD_SetDisplayMode1 } : method{ "IDirectDraw::SetDisplayMode", 4, DD_SetDisplayMode1 },
			{ "IDirectDraw::WaitForVerticalBlank", 3, DD_WaitForVerticalBlank },
			{ "IDirectDraw2::GetAvailableVidMem", 4, DD_GetAvailableVidMem },
		});
	};
	vt_dd1 = dd(false);
	vt_dd2 = dd(true);
	vt_surface = make_vtable({
		{ "IDirectDrawSurface::QueryInterface", 3, S_QueryInterface }, { "IDirectDrawSurface::AddRef", 1, AddRef },
		{ "IDirectDrawSurface::Release", 1, Release }, { "IDirectDrawSurface::AddAttachedSurface", 2, nullptr },
		{ "IDirectDrawSurface::AddOverlayDirtyRect", 2, nullptr }, { "IDirectDrawSurface::Blt", 6, S_Blt },
		{ "IDirectDrawSurface::BltBatch", 4, nullptr }, { "IDirectDrawSurface::BltFast", 6, S_BltFast },
		{ "IDirectDrawSurface::DeleteAttachedSurface", 3, nullptr }, { "IDirectDrawSurface::EnumAttachedSurfaces", 3, nullptr },
		{ "IDirectDrawSurface::EnumOverlayZOrders", 4, nullptr }, { "IDirectDrawSurface::Flip", 3, S_Flip },
		{ "IDirectDrawSurface::GetAttachedSurface", 3, S_GetAttachedSurface }, { "IDirectDrawSurface::GetBltStatus", 2, nullptr },
		{ "IDirectDrawSurface::GetCaps", 2, S_GetCaps }, { "IDirectDrawSurface::GetClipper", 2, nullptr },
		{ "IDirectDrawSurface::GetColorKey", 3, nullptr }, { "IDirectDrawSurface::GetDC", 2, nullptr },
		{ "IDirectDrawSurface::GetFlipStatus", 2, nullptr }, { "IDirectDrawSurface::GetOverlayPosition", 3, nullptr },
		{ "IDirectDrawSurface::GetPalette", 2, S_GetPalette }, { "IDirectDrawSurface::GetPixelFormat", 2, S_GetPixelFormat },
		{ "IDirectDrawSurface::GetSurfaceDesc", 2, S_GetSurfaceDesc }, { "IDirectDrawSurface::Initialize", 3, nullptr },
		{ "IDirectDrawSurface::IsLost", 1, S_IsLost }, { "IDirectDrawSurface::Lock", 5, S_Lock },
		{ "IDirectDrawSurface::ReleaseDC", 2, nullptr }, { "IDirectDrawSurface::Restore", 1, S_Restore },
		{ "IDirectDrawSurface::SetClipper", 2, S_SetClipper }, { "IDirectDrawSurface::SetColorKey", 3, S_SetColorKey },
		{ "IDirectDrawSurface::SetOverlayPosition", 3, nullptr }, { "IDirectDrawSurface::SetPalette", 2, S_SetPalette },
		{ "IDirectDrawSurface::Unlock", 2, S_Unlock }, { "IDirectDrawSurface::UpdateOverlay", 6, nullptr },
		{ "IDirectDrawSurface::UpdateOverlayDisplay", 2, nullptr }, { "IDirectDrawSurface::UpdateOverlayZOrder", 3, nullptr },
	});
	vt_palette = make_vtable({
		{ "IDirectDrawPalette::QueryInterface", 3, nullptr }, { "IDirectDrawPalette::AddRef", 1, AddRef },
		{ "IDirectDrawPalette::Release", 1, Release }, { "IDirectDrawPalette::GetCaps", 2, nullptr },
		{ "IDirectDrawPalette::GetEntries", 5, P_GetEntries }, { "IDirectDrawPalette::Initialize", 4, nullptr },
		{ "IDirectDrawPalette::SetEntries", 5, P_SetEntries },
	});
	vt_clipper = make_vtable({
		{ "IDirectDrawClipper::QueryInterface", 3, nullptr }, { "IDirectDrawClipper::AddRef", 1, AddRef },
		{ "IDirectDrawClipper::Release", 1, Release }, { "IDirectDrawClipper::GetClipList", 4, nullptr },
		{ "IDirectDrawClipper::GetHWnd", 2, nullptr }, { "IDirectDrawClipper::Initialize", 3, nullptr },
		{ "IDirectDrawClipper::IsClipListChanged", 2, nullptr }, { "IDirectDrawClipper::SetClipList", 3, nullptr },
		{ "IDirectDrawClipper::SetHWnd", 3, C_SetHWnd },
	});
}

} // namespace

API(DDRAW, DirectDrawCreate, 3)
{
	init();
	wr32(arg(1), make_object(vt_dd1));
	return DD_OK;
}

uint64_t ddraw_frame(uint8_t *pixels, uint8_t palette[256 * 3], int *w, int *h)
{
	std::lock_guard<std::mutex> g(frame_lock);
	memcpy(pixels, frame, (size_t)frame_w * frame_h);
	memcpy(palette, frame_palette, sizeof frame_palette);
	*w = frame_w;
	*h = frame_h;
	return frame_count;
}
