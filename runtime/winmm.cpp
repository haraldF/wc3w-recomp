/* WINMM: the joystick (the host's flight stick or game controller: joyGetDevCapsA, joyGetPosEx),
 * and waveOut, through
 * which XanLib streams its sound: queued WAVEHDRs play on a voice of the mixer (sound.cpp); the
 * ones played are marked done and reported to the program on its thread (wave_out_poll): a
 * callback function is called like an interrupt (all registers kept), a window gets MM_WOM_DONE. */
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include "host.h"
#include "sound.h"
#include "win.h"

/* ---- the joystick ----
 * The game asks for the capabilities once, at its start (so the host registers a stick that's
 * plugged in before that), then reads X, Y, Z (the throttle), R (the twist: it only turns at the
 * last eighth of either side), four buttons and the hat (JOYINFOEX flags CFh). */

static const uint32_t JOYERR_NOERROR = 0, JOYERR_PARMS = 165, JOYERR_UNPLUGGED = 167;
static std::mutex joy_lock;
static joystick_state joy;

void win_joystick(const joystick_state &s)
{
	std::lock_guard<std::mutex> g(joy_lock);
	joy = s;
}

API(WINMM, joyGetDevCapsA, 3)
{
	uint32_t id = arg(0), c = arg(1), size = std::min(arg(2), 404u);
	joystick_state s;
	{
		std::lock_guard<std::mutex> g(joy_lock);
		s = joy;
	}
	if (id != 0) return JOYERR_PARMS;
	if (!s.present) return JOYERR_UNPLUGGED;
	memset(guest_ptr(c, size), 0, size);
	if (size < 112) return JOYERR_PARMS;
	wr16(c, 1);
	wr16(c + 2, 1);
	put_str(c + 4, s.name, 32);
	wr32(c + 40, 65535); /* wXmax */
	wr32(c + 48, 65535); /* wYmax */
	wr32(c + 56, 65535); /* wZmax */
	wr32(c + 60, std::min(s.buttons_count, 32u));
	wr32(c + 64, 10);    /* wPeriodMin, wPeriodMax: ms */
	wr32(c + 68, 1000);
	wr32(c + 76, 65535); /* wRmax */
	wr32(c + 84, 65535); /* wUmax, wVmax */
	wr32(c + 92, 65535);
	/* wCaps: JOYCAPS_HASZ 1, HASR 2, HASPOV 0x10, POV4DIR 0x20 */
	wr32(c + 96, (s.has_z ? 1 : 0) | (s.has_r ? 2 : 0) | (s.has_pov ? 0x30 : 0));
	wr32(c + 100, 6);
	wr32(c + 104, 2 + (s.has_z ? 1 : 0) + (s.has_r ? 1 : 0));
	wr32(c + 108, 32);
	if (wc_trace) fprintf(stderr, " [joystick \"%s\": %u buttons%s%s%s]", s.name, s.buttons_count, s.has_z ? ", throttle" : "",
		s.has_r ? ", twist" : "", s.has_pov ? ", hat" : "");
	return JOYERR_NOERROR;
}

API(WINMM, joyGetPosEx, 2)
{
	uint32_t id = arg(0), p = arg(1);
	joystick_state s;
	{
		std::lock_guard<std::mutex> g(joy_lock);
		s = joy;
	}
	if (id != 0) return JOYERR_PARMS;
	if (!s.present) return JOYERR_UNPLUGGED;
	wr32(p + 8, s.x);
	wr32(p + 12, s.y);
	wr32(p + 16, s.z);
	wr32(p + 20, s.r);
	wr32(p + 24, 32767);
	wr32(p + 28, 32767);
	wr32(p + 32, s.buttons);
	uint32_t number = 0;
	for (uint32_t b = s.buttons; b; b &= b - 1) number++;
	wr32(p + 36, number);
	wr32(p + 40, s.pov);
	return JOYERR_NOERROR;
}

void user_post(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam); /* user32.cpp */

namespace {

enum : uint32_t {
	MMSYSERR_NOERROR = 0, MMSYSERR_INVALHANDLE = 5, WAVERR_BADFORMAT = 32,
	WHDR_DONE = 1, WHDR_PREPARED = 2, WHDR_INQUEUE = 0x10,
	WOM_OPEN = 0x3bb, WOM_CLOSE = 0x3bc, WOM_DONE = 0x3bd,
	CALLBACK_TYPEMASK = 0x70000, CALLBACK_WINDOW = 0x10000, CALLBACK_FUNCTION = 0x30000, WAVE_FORMAT_QUERY = 1,
};

struct device {
	voice v;
	uint32_t callback = 0, instance = 0, flags = 0;
	uint32_t volume = 0xffffffff;
};
std::map<uint32_t, device *> devices; /* by handle */
uint32_t next_handle = 0x5000;

/* The program's callback, called between two of its instructions: its registers as they were */
void notify(uint32_t h, const device *d, uint32_t message, uint32_t p1)
{
	switch (d->flags & CALLBACK_TYPEMASK) {
	case CALLBACK_FUNCTION: {
		uint8_t saved[sizeof R];
		memcpy(saved, R, sizeof R);
		/* below the stack in use, leaving room as an interrupt would */
		wr(R_SP, 4, rd(R_SP, 4) - 64);
		win_call(d->callback, { h, message, d->instance, p1, 0 });
		memcpy(R, saved, sizeof R);
		break;
	}
	case CALLBACK_WINDOW:
		user_post(d->callback, message, h, p1);
		break;
	}
}

device *get(uint32_t h)
{
	auto it = devices.find(h);
	return it == devices.end() ? nullptr : it->second;
}

void set_volume(device *d)
{
	d->v.left = (float)(d->volume & 0xffff) / 0xffff;
	d->v.right = (float)(d->volume >> 16) / 0xffff;
}

} // namespace

void wave_out_poll(void)
{
	static bool busy;
	if (busy) return;
	busy = true;
	for (auto &[h, d] : devices) {
		std::vector<uint32_t> done;
		{
			std::lock_guard<std::mutex> g(sound_lock);
			done.assign(d->v.done.begin(), d->v.done.end());
			d->v.done.clear();
		}
		for (uint32_t hdr : done) {
			wr32(hdr + 16, (rd32(hdr + 16) & ~WHDR_INQUEUE) | WHDR_DONE);
			notify(h, d, WOM_DONE, hdr);
		}
	}
	busy = false;
}

API(WINMM, waveOutGetDevCapsA, 3)
{
	uint32_t c = arg(1), size = std::min(arg(2), 52u);
	memset(guest_ptr(c, size), 0, size);
	if (size >= 52) {
		wr16(c, 1);
		wr16(c + 2, 1);
		wr32(c + 4, 0x100);
		put_str(c + 8, "SDL", 32);
		wr32(c + 40, 0xfff); /* every 11, 22, 44 kHz format */
		wr16(c + 44, 2);
		wr32(c + 48, 0x0c);  /* WAVECAPS_VOLUME | WAVECAPS_LRVOLUME */
	}
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutOpen, 6)
{
	uint32_t fmt = arg(2), flags = arg(5);
	unsigned tag = (unsigned)mrd32(fmt, 2), channels = (unsigned)mrd32(fmt + 2, 2), bits = (unsigned)mrd32(fmt + 14, 2);
	if (tag != 1 || (channels != 1 && channels != 2) || (bits != 8 && bits != 16)) return WAVERR_BADFORMAT;
	if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
	device *d = new device;
	d->v.wave_out = true;
	d->v.channels = channels;
	d->v.bits = bits;
	d->v.rate = rd32(fmt + 4);
	d->v.playing = true;
	d->callback = arg(3);
	d->instance = arg(4);
	d->flags = flags;
	uint32_t h = next_handle++;
	devices[h] = d;
	sound_add(&d->v);
	if (arg(0)) wr32(arg(0), h);
	if (wc_trace) fprintf(stderr, " [waveOut %x: %u Hz, %u bits, %u channels, callback %x (%x)]", h, d->v.rate, bits, channels, d->callback, flags);
	notify(h, d, WOM_OPEN, 0);
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutClose, 1)
{
	device *d = get(arg(0));
	if (!d) return MMSYSERR_INVALHANDLE;
	sound_remove(&d->v);
	notify(arg(0), d, WOM_CLOSE, 0);
	devices.erase(arg(0));
	delete d;
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutPrepareHeader, 3)
{
	wr32(arg(1) + 16, rd32(arg(1) + 16) | WHDR_PREPARED);
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutUnprepareHeader, 3)
{
	wr32(arg(1) + 16, rd32(arg(1) + 16) & ~WHDR_PREPARED);
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutWrite, 3)
{
	device *d = get(arg(0));
	if (!d) return MMSYSERR_INVALHANDLE;
	uint32_t hdr = arg(1);
	wr32(hdr + 16, (rd32(hdr + 16) & ~WHDR_DONE) | WHDR_INQUEUE);
	std::lock_guard<std::mutex> g(sound_lock);
	d->v.queue.push_back(hdr);
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutPause, 1)
{
	device *d = get(arg(0));
	if (!d) return MMSYSERR_INVALHANDLE;
	std::lock_guard<std::mutex> g(sound_lock);
	d->v.playing = false;
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutRestart, 1)
{
	device *d = get(arg(0));
	if (!d) return MMSYSERR_INVALHANDLE;
	std::lock_guard<std::mutex> g(sound_lock);
	d->v.playing = true;
	return MMSYSERR_NOERROR;
}

/* Everything queued is done (reported right away), playing goes on */
API(WINMM, waveOutReset, 1)
{
	device *d = get(arg(0));
	if (!d) return MMSYSERR_INVALHANDLE;
	{
		std::lock_guard<std::mutex> g(sound_lock);
		for (uint32_t h : d->v.queue) d->v.done.push_back(h);
		d->v.queue.clear();
		d->v.pos = 0;
		d->v.playing = true;
	}
	wave_out_poll();
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutGetVolume, 2)
{
	device *d = get(arg(0));
	wr32(arg(1), d ? d->volume : 0xffffffff);
	return MMSYSERR_NOERROR;
}

API(WINMM, waveOutSetVolume, 2)
{
	device *d = get(arg(0));
	if (!d) return MMSYSERR_NOERROR; /* the device's volume: left alone */
	std::lock_guard<std::mutex> g(sound_lock);
	d->volume = arg(1);
	set_volume(d);
	return MMSYSERR_NOERROR;
}

API(WINMM, DefDriverProc, 5) { return 0; }
