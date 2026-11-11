/* Fixes to the game's own flaws, as hooks (hooks.txt), ported from the enhancement patch
 * (Mod_Mouse.cpp, Mod_GeneralFixes.cpp; MIT, by Matt Wells). */
#include <cstdio>
#include <string>

#include "win.h"

void user_set_pointer(int x, int y); /* user32.cpp */

namespace {

/* A mission's start (the end of its setup, 452861h): the mouse steers by the pointer's distance
 * from the centre (320, 240), but the game leaves the pointer where Fly MISSION was clicked (the
 * right edge), so the ship turned right from the start. Centred, here and in the game's mouse
 * state (4A7E5Ah) and flight's copy of it (4A9B92h). */
bool centre_mouse_on_mission_start(void)
{
	if (rd32(0x4a7e54)) { /* a mouse */
		user_set_pointer(320, 240);
		wr16(0x4a7e5a, 320);
		wr16(0x4a7e5c, 240);
		wr16(0x4a9b92, 320);
		wr16(0x4a9b94, 240);
	}
	return false; /* the original code goes on */
}

/* The remaster's DATA folder (Mod_GeneralFixes.cpp, load_data_file): before the game looks for a
 * file in its archives (483A82h: the TRE lookup, 484CB0h, given the file object, its name first),
 * a path "..\..\DATA\..." that the game folder has as a loose file under DATA is renamed in place to
 * ".\DATA\...": the lookup misses and the game opens that file. The remaster's fixes are there: the
 * scenes (DATA\GAMEFLOW), the characters' profiles, some movies. */
bool load_data_file(void)
{
	uint32_t name = rd32(rd(R_SP, 4));
	std::string n = guest_str(name);
	if (n.compare(0, 6, "..\\..\\") == 0 && win_file_exists(n.substr(4))) {
		put_str(name, n.substr(4), (uint32_t)n.size() + 1);
		if (wc_trace) fprintf(stderr, " [DATA: %s]", n.c_str() + 4);
	}
	return false;
}

/* The game's fixed-point helpers (cdecl, 32-bit arguments) multiply into EDX:EAX and divide it with
 * IDIV, which faults when the quotient doesn't fit in 32 bits: the "crashes when imul/idiv were
 * overflowing" that the Wing Commander IV enhancement patch fixes in the same functions of WC4W.EXE
 * (Mod_GeneralFixes.cpp; MIT, by Matt Wells). They are replaced by 64-bit arithmetic whose result is
 * cut to 32 bits, as the patch does: what the recompiled IDIV gave too, but without the overflow
 * reports. A zero divisor stops the program as it did. */
int64_t arg_s(int i) { return (int32_t)rd32((uint32_t)rd(R_SP, 4) + 4 + 4 * (uint32_t)i); }

/* the function's return: EAX, and the RET (cdecl: the caller pops the arguments) */
bool returns(int64_t value)
{
	wr(R_AX, 4, (uint64_t)value);
	wr(R_SP, 4, rd(R_SP, 4) + 4);
	return true;
}

int64_t divide(int64_t a, int64_t b, uint32_t at)
{
	if (!b) cpu_div0(at);
	return a / b; /* |a| < 2^62: no INT64_MIN / -1 */
}

bool mul_256_div(void) { return returns(divide(arg_s(0) * 256, arg_s(1), 0x46eabc)); }       /* a * 256 / b */
bool mul_div_256(void) { return returns((arg_s(0) * arg_s(1)) >> 8); }                        /* a * b / 256 */
bool mul_div(void) { return returns(divide(arg_s(0) * arg_s(1), arg_s(2), 0x46eade)); }       /* a * b / c */

struct init {
	init()
	{
		hook_register(0x452861, centre_mouse_on_mission_start);
		hook_register(0x483a82, load_data_file);
		hook_register(0x46eabc, mul_256_div);
		hook_register(0x46eacf, mul_div_256);
		hook_register(0x46eade, mul_div);
	}
} init_;

} // namespace
