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

struct init {
	init()
	{
		hook_register(0x452861, centre_mouse_on_mission_start);
		hook_register(0x483a82, load_data_file);
	}
} init_;

} // namespace
