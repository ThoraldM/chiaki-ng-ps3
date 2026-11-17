// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "audioring.h"

#include <string.h>

// Level smoothing per pulled block of 256 frames (~5.3 ms): time constant ~0.3 s.
#define LEVEL_SMOOTHING 0.02
// Full speed correction is reached when the level is off by this fraction of the target.
#define CORRECTION_SPAN 0.5
// Hard limit: beyond target * this, drop down to the target (e.g. after a long stall).
#define OVERFLOW_FACTOR 4
// Adaptive target: +10 ms per underrun, -5 ms after 30 s without one.
#define TARGET_UP_MS     10
#define TARGET_DOWN_MS   5
#define TARGET_CALM_S    30

static size_t ms_to_frames(unsigned ms)
{
	return (size_t)AR_RATE * ms / 1000;
}

void ar_init(AudioRing *r, unsigned start_ms, unsigned min_ms, unsigned max_ms)
{
	memset(r, 0, sizeof(*r));
	r->target_min = ms_to_frames(min_ms);
	r->target_max = ms_to_frames(max_ms);
	r->target = ms_to_frames(start_ms);
	if(r->target < r->target_min) r->target = r->target_min;
	if(r->target > r->target_max) r->target = r->target_max;
	r->ratio = 1.0;
	r->level = (double)r->target;
}

static void drop_oldest(AudioRing *r, size_t n)
{
	if(n > r->fill)
		n = r->fill;
	r->read = (r->read + n) % AR_CAPACITY;
	r->fill -= n;
	r->dropped_frames += n;
}

void ar_push(AudioRing *r, const int16_t *stereo, size_t frames)
{
	for(size_t i = 0; i < frames; i++)
	{
		if(r->fill == AR_CAPACITY)
			drop_oldest(r, 1);
		size_t w = (r->read + r->fill) % AR_CAPACITY;
		r->buf[w * 2] = stereo[i * 2];
		r->buf[w * 2 + 1] = stereo[i * 2 + 1];
		r->fill++;
	}
	if(r->fill > r->target * OVERFLOW_FACTOR)
	{
		drop_oldest(r, r->fill - r->target);
		r->level = (double)r->target;
	}
}

void ar_pull(AudioRing *r, float *out, size_t frames)
{
	if(!r->primed && r->fill >= r->target)
	{
		r->primed = true;
		r->frac = 0;
		r->level = (double)r->fill;
	}

	// Adjust the playback speed from the smoothed level (proportional control).
	if(r->primed)
	{
		r->level += LEVEL_SMOOTHING * ((double)r->fill - r->level);
		double err = (r->level - (double)r->target) / ((double)r->target * CORRECTION_SPAN);
		if(err > 1.0) err = 1.0;
		if(err < -1.0) err = -1.0;
		r->ratio = 1.0 + err * AR_MAX_RATIO_DEV;
	}

	size_t i = 0;
	for(; r->primed && i < frames; i++)
	{
		if(r->fill < 2)
		{
			r->underruns++;
			r->primed = false; // refill to the target before playing again
			r->frames_since_underrun = 0;
			r->target += ms_to_frames(TARGET_UP_MS);
			if(r->target > r->target_max)
				r->target = r->target_max;
			break;
		}
		size_t i0 = r->read, i1 = (r->read + 1) % AR_CAPACITY;
		float f = (float)r->frac;
		float l = r->buf[i0 * 2] + (r->buf[i1 * 2] - r->buf[i0 * 2]) * f;
		float rr = r->buf[i0 * 2 + 1] + (r->buf[i1 * 2 + 1] - r->buf[i0 * 2 + 1]) * f;
		out[i * 2] = l * (1.0f / 32768.0f);
		out[i * 2 + 1] = rr * (1.0f / 32768.0f);
		r->frac += r->ratio;
		while(r->frac >= 1.0 && r->fill > 0)
		{
			r->frac -= 1.0;
			r->read = (r->read + 1) % AR_CAPACITY;
			r->fill--;
		}
	}
	if(i < frames)
		memset(out + i * 2, 0, (frames - i) * 2 * sizeof(float));

	if(r->primed)
	{
		r->frames_since_underrun += frames;
		if(r->frames_since_underrun >= (uint64_t)AR_RATE * TARGET_CALM_S)
		{
			r->frames_since_underrun = 0;
			size_t down = ms_to_frames(TARGET_DOWN_MS);
			r->target = r->target > r->target_min + down ? r->target - down : r->target_min;
		}
	}
}

unsigned ar_buffered_ms(const AudioRing *r)
{
	return (unsigned)(r->fill * 1000 / AR_RATE);
}

unsigned ar_target_ms(const AudioRing *r)
{
	return (unsigned)(r->target * 1000 / AR_RATE);
}
