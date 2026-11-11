/* The mixer (sound.h). */
#include <algorithm>
#include <cmath>
#include <vector>

#include "sound.h"
#include "wc_rt.h"

std::mutex sound_lock;
static std::vector<voice *> voices;
static std::vector<pcm_stream *> streams;

void sound_stream_add(pcm_stream *s)
{
	std::lock_guard<std::mutex> g(sound_lock);
	streams.push_back(s);
}

void sound_stream_remove(pcm_stream *s)
{
	std::lock_guard<std::mutex> g(sound_lock);
	streams.erase(std::remove(streams.begin(), streams.end(), s), streams.end());
}

void sound_add(voice *v)
{
	std::lock_guard<std::mutex> g(sound_lock);
	voices.push_back(v);
}

void sound_remove(voice *v)
{
	std::lock_guard<std::mutex> g(sound_lock);
	voices.erase(std::remove(voices.begin(), voices.end(), v), voices.end());
}

/* A frame of the voice at frame index i of its data (left, right in -32768..32767) */
static void sample(const voice &v, uint32_t data, uint32_t i, float *l, float *r)
{
	uint32_t a = data + i * v.channels * (v.bits / 8);
	if (v.bits == 16) {
		*l = (float)(int16_t)mrd32(a, 2);
		*r = v.channels == 2 ? (float)(int16_t)mrd32(a + 2, 2) : *l;
	}
	else {
		*l = (float)((int)M[a & (MEM_SIZE - 1)] - 128) * 256;
		*r = v.channels == 2 ? (float)((int)M[(a + 1) & (MEM_SIZE - 1)] - 128) * 256 : *l;
	}
}

static void mix_voice(voice &v, float *acc, unsigned frames)
{
	double step = (double)v.rate / MIX_RATE;
	unsigned frame_bytes = v.channels * (v.bits / 8);
	for (unsigned k = 0; k < frames && v.playing; k++) {
		uint32_t data = v.data, n = frame_bytes ? v.size / frame_bytes : 0;
		if (v.wave_out) {
			/* the header being played: its data and length */
			if (v.queue.empty()) return;
			uint32_t h = v.queue.front();
			data = (uint32_t)mrd32(h, 4);
			n = (uint32_t)mrd32(h + 4, 4) / frame_bytes;
		}
		if (n == 0) {
			if (v.wave_out) {
				v.done.push_back(v.queue.front());
				v.queue.pop_front();
				v.pos = 0;
				continue;
			}
			return;
		}
		uint32_t i = (uint32_t)v.pos;
		float l, r, frac = (float)(v.pos - i);
		sample(v, data, i, &l, &r);
		if (frac > 0) {
			/* between two frames (another rate than the mixer's): interpolated; after the last one
			 * comes a looping buffer's first */
			uint32_t next = i + 1 < n ? i + 1 : v.looping && !v.wave_out ? 0 : i;
			float l2, r2;
			sample(v, data, next, &l2, &r2);
			l += (l2 - l) * frac;
			r += (r2 - r) * frac;
		}
		acc[2 * k] += l * v.left;
		acc[2 * k + 1] += r * v.right;
		v.pos += step;
		if (v.pos >= n) {
			if (v.wave_out) {
				v.done.push_back(v.queue.front());
				v.queue.pop_front();
				v.pos = 0;
			}
			else if (v.looping) {
				v.pos = fmod(v.pos, n);
			}
			else {
				v.pos = 0;
				v.playing = false;
			}
		}
	}
}

void sound_mix(int16_t *out, unsigned frames)
{
	static std::vector<float> acc;
	acc.assign(2 * (size_t)frames, 0.0f);
	{
		std::lock_guard<std::mutex> g(sound_lock);
		for (voice *v : voices) mix_voice(*v, acc.data(), frames);
		for (pcm_stream *s : streams) {
			if (s->paused) continue;
			unsigned n = (unsigned)std::min<size_t>(frames, s->samples.size() / 2);
			for (unsigned k = 0; k < 2 * n; k++) acc[k] += s->samples[k];
			s->samples.erase(s->samples.begin(), s->samples.begin() + 2 * n);
			s->consumed += n;
		}
	}
	for (size_t i = 0; i < acc.size(); i++) out[i] = (int16_t)std::clamp(acc[i], -32768.0f, 32767.0f);
}
