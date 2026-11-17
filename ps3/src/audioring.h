// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Audio jitter buffer with clock-drift correction (platform independent, not
// thread safe: the caller holds a lock).
//
// The console's audio clock and the PS3's output clock differ slightly, so a
// plain FIFO slowly fills up (more delay, then dropped audio) or drains
// (underruns). Here the buffer level is held at a target by playing back up to
// +-0.5 % faster or slower, using linear interpolation (inaudible at such small
// ratios). The target itself adapts to the network: it grows after an
// underrun and shrinks again after a calm period.
#ifndef CHIAKI_PS3_AUDIORING_H
#define CHIAKI_PS3_AUDIORING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AR_RATE      48000
#define AR_CAPACITY  (AR_RATE / 2)  // frames (500 ms)
#define AR_MAX_RATIO_DEV 0.005      // +-0.5 % playback speed

typedef struct
{
	int16_t buf[AR_CAPACITY * 2];   // stereo frames
	size_t read;                    // index of the current frame
	size_t fill;                    // frames available from read
	double frac;                    // position between buf[read] and buf[read + 1]
	double ratio;                   // input frames consumed per output frame
	double level;                   // smoothed fill level (frames)
	size_t target;                  // frames; adapts between target_min and target_max
	size_t target_min, target_max;
	uint64_t frames_since_underrun; // output frames played since the last underrun
	bool primed;

	uint64_t underruns;
	uint64_t dropped_frames;
} AudioRing;

// min_ms <= start_ms <= max_ms
void ar_init(AudioRing *r, unsigned start_ms, unsigned min_ms, unsigned max_ms);
// Interleaved stereo int16.
void ar_push(AudioRing *r, const int16_t *stereo, size_t frames);
// Produces exactly `frames` interleaved stereo floats (silence while not primed).
void ar_pull(AudioRing *r, float *out, size_t frames);
// Current delay and target in milliseconds.
unsigned ar_buffered_ms(const AudioRing *r);
unsigned ar_target_ms(const AudioRing *r);

#endif
