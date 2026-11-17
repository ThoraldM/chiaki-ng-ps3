// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 audio output: 48 kHz stereo PCM through libaudio.
#ifndef CHIAKI_PS3_AUDIO_H
#define CHIAKI_PS3_AUDIO_H

#include <chiaki/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool audio_init(ChiakiLog *log);
void audio_fini(void);
// Stream format announced by the console (from the Opus header).
void audio_set_format(uint32_t channels, uint32_t rate);
// Interleaved signed 16-bit PCM, samples_count frames per channel.
void audio_push(const int16_t *pcm, size_t samples_count);
// Empties the buffer and resets the statistics (call when a new stream starts).
void audio_reset(void);

typedef struct
{
	uint64_t underruns;
	uint64_t dropped_frames;
	uint32_t buffered_ms;
	uint32_t target_ms;   // adaptive jitter buffer target
	int32_t speed_ppm;    // drift correction: playback speed offset in parts per million
} AudioStats;
void audio_get_stats(AudioStats *stats);

#endif
