/* Fixes to the game's own flaws, as hooks (hooks.txt), ported from the enhancement patch
 * (Mod_Mouse.cpp; MIT, by Matt Wells). */
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

struct init {
	init() { hook_register(0x452861, centre_mouse_on_mission_start); }
} init_;

} // namespace
