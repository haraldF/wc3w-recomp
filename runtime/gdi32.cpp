/* GDI32: what the program asks about the screen (an 8-bit palette device, as under DirectDraw's
 * 640x480x8 mode) and palettes. Drawing with GDI (CreateDIBSection, StretchDIBits: the windowed
 * mode) is to come when the program does it. */
#include "win.h"

API(GDI32, GetDeviceCaps, 2)
{
	switch (arg(1)) {
	case 8: return 640;      /* HORZRES */
	case 10: return 480;     /* VERTRES */
	case 12: return 8;       /* BITSPIXEL */
	case 14: return 1;       /* PLANES */
	case 38: return 0x100;   /* RASTERCAPS: RC_PALETTE */
	case 104: return 256;    /* SIZEPALETTE */
	case 106: return 20;     /* NUMRESERVED */
	case 108: return 18;     /* COLORRES */
	}
	return 0;
}

/* Palettes: the LOGPALETTE copied (version, count, entries) */
API(GDI32, CreatePalette, 1)
{
	uint32_t lp = arg(0), n = (uint32_t)mrd32(lp + 2, 2);
	uint32_t h = heap_alloc(4 + 4 * n);
	memcpy(M + h, M + lp, 4 + 4 * n);
	return h;
}

API(GDI32, SelectPalette, 3) { return arg(1); }
API(GDI32, RealizePalette, 1) { return 0; }
API(GDI32, GetStockObject, 1) { return 0x40000 + arg(0); }
API(GDI32, DeleteObject, 1) { return 1; }

/* A DIB section: its bits in the program's memory (a region of their own), the BITMAPINFO kept
 * with them for drawing it later. */
API(GDI32, CreateDIBSection, 6)
{
	uint32_t bi = arg(1);
	int32_t w = (int32_t)rd32(bi + 4), h = (int32_t)rd32(bi + 8);
	uint32_t bpp = (uint32_t)mrd32(bi + 14, 2);
	uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4, size = stride * (uint32_t)(h < 0 ? -h : h);
	uint32_t bits = region_alloc(0, size);
	if (!bits) return 0;
	if (arg(3)) wr32(arg(3), bits);
	uint32_t hbm = heap_alloc(16);
	wr32(hbm, bits);
	wr32(hbm + 4, (uint32_t)w);
	wr32(hbm + 8, (uint32_t)h);
	wr32(hbm + 12, bpp);
	if (wc_trace) fprintf(stderr, " [%dx%d, %u bits at %x]", w, h, bpp, bits);
	return hbm;
}
