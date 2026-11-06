/* DirectSound (dsound.dll, which the game loads itself): secondary buffers in the program's memory
 * that the mixer (sound.cpp) plays, with their play and write cursors, volume, pan and frequency.
 * The primary buffer is only for show (its format is the mixer's business). COM objects as in
 * ddraw.cpp: a pointer to a table of thunks. */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "sound.h"
#include "win.h"

namespace {

enum : uint32_t {
	DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057, DSERR_GENERIC = 0x80004005, E_NOINTERFACE = 0x80004002,
	DSBCAPS_PRIMARYBUFFER = 1, DSBPLAY_LOOPING = 1, DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2,
	DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4,
};

struct buffer {
	voice v;
	bool primary = false;
	buffer *original = nullptr; /* a duplicate's: the buffer whose memory it plays */
	int duplicates = 0;         /* an original's: duplicates still there */
	bool released = false;      /* an original released while duplicates play its memory */
	uint32_t flags = 0;
	unsigned base_rate = 22050;
	int32_t volume = 0, pan = 0; /* hundredths of a dB */
};
std::map<uint32_t, buffer *> buffers; /* by object address */
uint32_t vt_dsound, vt_buffer;

uint32_t make_object(uint32_t vtable)
{
	uint32_t o = heap_alloc(16);
	wr32(o, vtable);
	return o;
}

struct method {
	const char *name;
	int nargs;
	win_fn fn;
};
uint32_t make_vtable(std::initializer_list<method> methods)
{
	uint32_t vt = heap_alloc(4 * (uint32_t)methods.size());
	uint32_t i = 0;
	for (const method &m : methods) wr32(vt + 4 * i++, thunk_native(m.name, m.nargs, m.fn));
	return vt;
}

buffer *get(uint32_t o)
{
	auto it = buffers.find(o);
	return it == buffers.end() ? nullptr : it->second;
}

/* volume and pan (attenuations in hundredths of a dB) as the left and right gains */
void set_gains(buffer *b)
{
	float g = powf(10, (float)std::clamp(b->volume, -10000, 0) / 2000);
	float l = powf(10, (float)std::clamp(-b->pan, -10000, 0) / 2000), r = powf(10, (float)std::clamp(b->pan, -10000, 0) / 2000);
	b->v.left = g * l;
	b->v.right = g * r;
}

uint32_t AddRef() { return 1; }

/* ---- IDirectSound ---- */

uint32_t DS_Release() { return 0; }
uint32_t DS_SetCooperativeLevel() { return DS_OK; }
uint32_t DS_Compact() { return DS_OK; }
uint32_t DS_Initialize() { return DS_OK; }
uint32_t DS_SetSpeakerConfig() { return DS_OK; }

uint32_t DS_GetSpeakerConfig()
{
	wr32(arg(1), 4); /* DSSPEAKER_STEREO */
	return DS_OK;
}

uint32_t DS_GetCaps()
{
	uint32_t c = arg(1), size = std::min(rd32(c), 96u);
	memset(M + c, 0, size);
	wr32(c, size);
	wr32(c + 4, 0x1f | 0x400 | 0x800 | 0x1000 | 0x2000); /* primary and secondary, mono and stereo, 8 and 16 bits, any rate */
	wr32(c + 8, 4000);    /* dwMinSecondarySampleRate */
	wr32(c + 12, 100000); /* dwMaxSecondarySampleRate */
	wr32(c + 16, 1);      /* dwPrimaryBuffers */
	wr32(c + 20, 64);     /* dwMaxHwMixingAllBuffers */
	wr32(c + 24, 64);
	wr32(c + 28, 64);
	return DS_OK;
}

uint32_t DS_CreateSoundBuffer()
{
	uint32_t desc = arg(1), out = arg(2);
	uint32_t flags = rd32(desc + 4), bytes = rd32(desc + 8), fmt = rd32(desc + 16);
	buffer *b = new buffer;
	b->flags = flags;
	b->primary = flags & DSBCAPS_PRIMARYBUFFER;
	if (fmt) {
		b->v.channels = (unsigned)mrd32(fmt + 2, 2);
		b->v.rate = b->base_rate = rd32(fmt + 4);
		b->v.bits = (unsigned)mrd32(fmt + 14, 2);
	}
	if (!b->primary) {
		if (!bytes || (b->v.bits != 8 && b->v.bits != 16) || (b->v.channels != 1 && b->v.channels != 2)) {
			delete b;
			return DSERR_INVALIDPARAM;
		}
		b->v.size = bytes;
		b->v.data = heap_alloc(bytes);
		/* silence */
		memset(M + b->v.data, b->v.bits == 8 ? 0x80 : 0, bytes);
		sound_add(&b->v);
	}
	uint32_t o = make_object(vt_buffer);
	buffers[o] = b;
	if (wc_trace) fprintf(stderr, " [buffer %x: %u bytes, %u Hz, %u bits, %u channels, flags %x]", o, bytes, b->v.rate, b->v.bits, b->v.channels, flags);
	wr32(out, o);
	return DS_OK;
}

/* ---- IDirectSoundBuffer ---- */

uint32_t B_QueryInterface()
{
	wr32(arg(2), 0);
	return E_NOINTERFACE;
}

/* The memory goes when the last buffer playing it does (an original and its duplicates) */
void drop(buffer *b)
{
	if (b->original) {
		buffer *o = b->original;
		delete b;
		if (--o->duplicates == 0 && o->released) drop(o);
		return;
	}
	if (b->duplicates) {
		b->released = true;
		return;
	}
	if (b->v.data) heap_free(b->v.data);
	delete b;
}

uint32_t B_Release()
{
	auto it = buffers.find(arg(0));
	if (it != buffers.end()) {
		sound_remove(&it->second->v);
		buffer *b = it->second;
		buffers.erase(it);
		drop(b);
	}
	return 0;
}

/* A buffer playing the same memory, with a position, volume, pan and frequency of its own */
uint32_t DS_DuplicateSoundBuffer()
{
	buffer *src = get(arg(1));
	if (!src || src->primary) return DSERR_INVALIDPARAM;
	buffer *o = src->original ? src->original : src;
	buffer *b = new buffer;
	b->v.data = o->v.data;
	b->v.size = o->v.size;
	b->v.channels = o->v.channels;
	b->v.bits = o->v.bits;
	b->v.rate = b->base_rate = o->base_rate;
	b->flags = src->flags;
	b->volume = src->volume;
	b->pan = src->pan;
	set_gains(b);
	b->original = o;
	o->duplicates++;
	sound_add(&b->v);
	uint32_t obj = make_object(vt_buffer);
	buffers[obj] = b;
	wr32(arg(2), obj);
	return DS_OK;
}

uint32_t B_GetCaps()
{
	buffer *b = get(arg(0));
	uint32_t c = arg(1);
	if (!b) return DSERR_INVALIDPARAM;
	wr32(c + 4, b->flags);
	wr32(c + 8, b->v.size);
	return DS_OK;
}

uint32_t frame_bytes(const buffer *b) { return b->v.channels * (b->v.bits / 8); }

/* The play cursor where the mixer is, the write cursor a little ahead (as a sound card's) */
uint32_t B_GetCurrentPosition()
{
	buffer *b = get(arg(0));
	if (!b) return DSERR_INVALIDPARAM;
	uint32_t play = 0, write = 0;
	if (!b->primary && b->v.size) {
		std::lock_guard<std::mutex> g(sound_lock);
		play = (uint32_t)b->v.pos * frame_bytes(b) % b->v.size;
		write = b->v.playing ? (play + b->v.rate / 100 * frame_bytes(b)) % b->v.size : play;
	}
	if (arg(1)) wr32(arg(1), play);
	if (arg(2)) wr32(arg(2), write);
	return DS_OK;
}

uint32_t B_GetFormat()
{
	buffer *b = get(arg(0));
	uint32_t f = arg(1);
	if (!b || !f) return DSERR_INVALIDPARAM;
	wr16(f, 1);
	wr16(f + 2, (uint16_t)b->v.channels);
	wr32(f + 4, b->base_rate);
	wr32(f + 8, b->base_rate * frame_bytes(b));
	wr16(f + 12, (uint16_t)frame_bytes(b));
	wr16(f + 14, (uint16_t)b->v.bits);
	if (arg(2) >= 18) wr16(f + 16, 0);
	if (arg(3)) wr32(arg(3), 18);
	return DS_OK;
}

uint32_t B_GetVolume()
{
	buffer *b = get(arg(0));
	wr32(arg(1), b ? (uint32_t)b->volume : 0);
	return DS_OK;
}

uint32_t B_GetPan()
{
	buffer *b = get(arg(0));
	wr32(arg(1), b ? (uint32_t)b->pan : 0);
	return DS_OK;
}

uint32_t B_GetFrequency()
{
	buffer *b = get(arg(0));
	wr32(arg(1), b ? b->v.rate : 0);
	return DS_OK;
}

uint32_t B_GetStatus()
{
	buffer *b = get(arg(0));
	uint32_t s = 0;
	if (b) {
		std::lock_guard<std::mutex> g(sound_lock);
		s = (b->v.playing ? DSBSTATUS_PLAYING : 0) | (b->v.playing && b->v.looping ? DSBSTATUS_LOOPING : 0);
	}
	wr32(arg(1), s);
	return DS_OK;
}

/* The buffer is the program's memory already: Lock hands out its parts, Unlock has nothing to do */
uint32_t B_Lock()
{
	buffer *b = get(arg(0));
	if (!b || b->primary || !b->v.size) return DSERR_INVALIDPARAM;
	uint32_t offset = arg(1), bytes = arg(2), flags = arg(7);
	if (flags & DSBLOCK_FROMWRITECURSOR) {
		std::lock_guard<std::mutex> g(sound_lock);
		uint32_t play = (uint32_t)b->v.pos * frame_bytes(b) % b->v.size;
		offset = b->v.playing ? (play + b->v.rate / 100 * frame_bytes(b)) % b->v.size : play;
	}
	if (flags & DSBLOCK_ENTIREBUFFER) bytes = b->v.size;
	offset %= b->v.size;
	bytes = std::min(bytes, b->v.size);
	uint32_t first = std::min(bytes, b->v.size - offset);
	wr32(arg(3), b->v.data + offset);
	wr32(arg(4), first);
	if (arg(5)) wr32(arg(5), first < bytes ? b->v.data : 0);
	if (arg(6)) wr32(arg(6), bytes - first);
	return DS_OK;
}

uint32_t B_Unlock() { return DS_OK; }

uint32_t B_Play()
{
	buffer *b = get(arg(0));
	if (!b) return DSERR_INVALIDPARAM;
	std::lock_guard<std::mutex> g(sound_lock);
	b->v.playing = !b->primary;
	b->v.looping = arg(3) & DSBPLAY_LOOPING;
	return DS_OK;
}

uint32_t B_Stop()
{
	buffer *b = get(arg(0));
	if (!b) return DSERR_INVALIDPARAM;
	std::lock_guard<std::mutex> g(sound_lock);
	b->v.playing = false;
	return DS_OK;
}

uint32_t B_SetCurrentPosition()
{
	buffer *b = get(arg(0));
	if (!b || b->primary) return b ? DS_OK : DSERR_INVALIDPARAM;
	std::lock_guard<std::mutex> g(sound_lock);
	b->v.pos = (double)(arg(1) % std::max(b->v.size, 1u) / frame_bytes(b));
	return DS_OK;
}

uint32_t B_SetFormat() { return DS_OK; } /* the primary's: the mixer has its own */

uint32_t B_SetVolume()
{
	buffer *b = get(arg(0));
	if (!b) return DSERR_INVALIDPARAM;
	std::lock_guard<std::mutex> g(sound_lock);
	b->volume = (int32_t)arg(1);
	set_gains(b);
	return DS_OK;
}

uint32_t B_SetPan()
{
	buffer *b = get(arg(0));
	if (!b) return DSERR_INVALIDPARAM;
	std::lock_guard<std::mutex> g(sound_lock);
	b->pan = (int32_t)arg(1);
	set_gains(b);
	return DS_OK;
}

uint32_t B_SetFrequency()
{
	buffer *b = get(arg(0));
	if (!b) return DSERR_INVALIDPARAM;
	std::lock_guard<std::mutex> g(sound_lock);
	b->v.rate = arg(1) ? arg(1) : b->base_rate;
	return DS_OK;
}

uint32_t B_Restore() { return DS_OK; }

void init(void)
{
	if (vt_dsound) return;
	vt_dsound = make_vtable({
		{ "IDirectSound::QueryInterface", 3, B_QueryInterface }, { "IDirectSound::AddRef", 1, AddRef },
		{ "IDirectSound::Release", 1, DS_Release }, { "IDirectSound::CreateSoundBuffer", 4, DS_CreateSoundBuffer },
		{ "IDirectSound::GetCaps", 2, DS_GetCaps }, { "IDirectSound::DuplicateSoundBuffer", 3, DS_DuplicateSoundBuffer },
		{ "IDirectSound::SetCooperativeLevel", 3, DS_SetCooperativeLevel }, { "IDirectSound::Compact", 1, DS_Compact },
		{ "IDirectSound::GetSpeakerConfig", 2, DS_GetSpeakerConfig },
		{ "IDirectSound::SetSpeakerConfig", 2, DS_SetSpeakerConfig }, { "IDirectSound::Initialize", 2, DS_Initialize },
	});
	vt_buffer = make_vtable({
		{ "IDirectSoundBuffer::QueryInterface", 3, B_QueryInterface }, { "IDirectSoundBuffer::AddRef", 1, AddRef },
		{ "IDirectSoundBuffer::Release", 1, B_Release }, { "IDirectSoundBuffer::GetCaps", 2, B_GetCaps },
		{ "IDirectSoundBuffer::GetCurrentPosition", 3, B_GetCurrentPosition }, { "IDirectSoundBuffer::GetFormat", 4, B_GetFormat },
		{ "IDirectSoundBuffer::GetVolume", 2, B_GetVolume }, { "IDirectSoundBuffer::GetPan", 2, B_GetPan },
		{ "IDirectSoundBuffer::GetFrequency", 2, B_GetFrequency }, { "IDirectSoundBuffer::GetStatus", 2, B_GetStatus },
		{ "IDirectSoundBuffer::Initialize", 3, DS_Initialize }, { "IDirectSoundBuffer::Lock", 8, B_Lock },
		{ "IDirectSoundBuffer::Play", 4, B_Play }, { "IDirectSoundBuffer::SetCurrentPosition", 2, B_SetCurrentPosition },
		{ "IDirectSoundBuffer::SetFormat", 2, B_SetFormat }, { "IDirectSoundBuffer::SetVolume", 2, B_SetVolume },
		{ "IDirectSoundBuffer::SetPan", 2, B_SetPan }, { "IDirectSoundBuffer::SetFrequency", 2, B_SetFrequency },
		{ "IDirectSoundBuffer::Stop", 1, B_Stop }, { "IDirectSoundBuffer::Unlock", 5, B_Unlock },
		{ "IDirectSoundBuffer::Restore", 1, B_Restore },
	});
}

} // namespace

API(DSOUND, DirectSoundCreate, 3)
{
	init();
	wr32(arg(1), make_object(vt_dsound));
	return DS_OK;
}
