/* The host platform around the program: the window (later: DirectDraw's picture), keyboard, mouse
 * and sound (host_sdl.cpp). The program runs on a thread of its own while the host owns the main
 * thread, which macOS needs for windows. The emulation never includes SDL. */
#pragma once
#include <cstdint>

/* The folder the program's files (image.bin) are in: the executable's, with a separator at the end. */
const char *host_base_dir(void);
/* Run game on a thread of its own (with a stack large enough for the recompiled code's deep C
 * calls) until the program exits. */
[[noreturn]] void host_run(void (*game)(void));
/* Start fn(arg) on a host thread with a stack as large as the game's (a program's thread). */
void host_thread(void (*fn)(void *), void *arg);
/* The program ends with this code, on its thread. */
[[noreturn]] void host_exit(int code);
/* The joystick (winmm.cpp), from the host's thread: axes 0..65535 (32767 centred; x left to right,
 * y up to down), buttons a bit each, the hat in hundredths of a degree (0xffff centred). */
struct joystick_state {
	bool present = false;
	char name[32] = "";
	unsigned buttons_count = 0;
	bool has_z = false, has_r = false, has_pov = false;
	uint32_t x = 32767, y = 32767, z = 32767, r = 32767;
	uint32_t buttons = 0, pov = 0xffff;
};
void win_joystick(const joystick_state &s);

/* A file in the game folder was written or deleted (closed): for a host that keeps such files. */
void host_file_saved(const char *path);
