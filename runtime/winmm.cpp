/* WINMM: the joystick (none yet: a game controller through SDL is to come) and waveOut for XanLib's
 * sound (to come). */
#include "win.h"

static const uint32_t JOYERR_UNPLUGGED = 167;

API(WINMM, joyGetDevCapsA, 3) { return JOYERR_UNPLUGGED; }
API(WINMM, joyGetPosEx, 2) { return JOYERR_UNPLUGGED; }
