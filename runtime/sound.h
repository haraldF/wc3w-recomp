/* The mixer behind DirectSound's buffers (dsound.cpp) and waveOut (winmm.cpp): voices that play
 * 8- or 16-bit PCM from the program's memory, mixed on the host's audio thread into 16-bit stereo
 * at MIX_RATE. Play positions advance as the host consumes the sound (headless: in real time). */
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

inline constexpr unsigned MIX_RATE = 44100;

struct voice {
	/* the sound: a DirectSound buffer's memory, or (waveOut) the queued headers' */
	uint32_t data = 0, size = 0;
	unsigned channels = 1, bits = 8, rate = 22050;
	double pos = 0; /* in frames */
	bool playing = false, looping = false;
	float left = 1, right = 1; /* volume and pan */
	/* waveOut: WAVEHDR addresses queued, and those played (for the game thread to report) */
	bool wave_out = false;
	std::deque<uint32_t> queue, done;
};

/* Sound from the runtime itself (the HD movies): 16-bit stereo at MIX_RATE, consumed as mixed */
struct pcm_stream {
	std::deque<int16_t> samples; /* interleaved left, right */
	uint64_t consumed = 0;        /* frames mixed so far */
	bool paused = false;
};

/* Held while touching voices and streams (the host's audio thread mixes them). */
extern std::mutex sound_lock;
void sound_add(voice *v);
void sound_remove(voice *v);
void sound_stream_add(pcm_stream *s);
void sound_stream_remove(pcm_stream *s);
/* Called by the host on its audio thread: mixes frames of stereo into out. */
void sound_mix(int16_t *out, unsigned frames);
/* On the program's thread: waveOut's finished headers to the program (winmm.cpp). */
void wave_out_poll(void);
