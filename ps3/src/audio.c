// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PS3 audio output.
//
// Decoded PCM from chiaki's Opus decoder goes into a jitter buffer with clock
// drift correction (audioring.c). A thread woken by libaudio (once per consumed
// 256-sample block) keeps a few blocks ahead of the hardware read position.

#include "audio.h"
#include "audioring.h"

#include <chiaki/thread.h>

#include <string.h>
#include <stdlib.h>

#include <audio/audio.h>
#include <sys/event_queue.h>

#define OUT_RATE        48000
// Jitter buffer target: starts at 70 ms, adapts between 60 and 150 ms
// (chosen with tools/audio_sim.c).
#define TARGET_START_MS 70
#define TARGET_MIN_MS   60
#define TARGET_MAX_MS   150
#define BLOCKS_AHEAD    3

static ChiakiLog *alog;
static bool initialized;
static uint32_t port;
static audioPortConfig port_config;
static sys_event_queue_t event_queue;
static sys_ipc_key_t event_key;

static ChiakiThread thread;
static volatile bool running;

static ChiakiMutex ring_mutex;
static AudioRing ring;
static uint32_t in_channels = 2;
static uint32_t in_rate = OUT_RATE;

static AudioStats stats;

void audio_set_format(uint32_t channels, uint32_t rate)
{
	if(alog)
		CHIAKI_LOGI(alog, "Audio stream: %u channels, %u Hz", (unsigned)channels, (unsigned)rate);
	in_channels = channels ? channels : 2;
	in_rate = rate;
	if(rate != OUT_RATE && alog)
		CHIAKI_LOGW(alog, "Audio rate %u Hz is not 48000 Hz, playback will be off-pitch", (unsigned)rate);
}

void audio_push(const int16_t *pcm, size_t frames)
{
	if(!initialized)
		return;
	int16_t stereo[512 * 2];
	chiaki_mutex_lock(&ring_mutex);
	while(frames > 0)
	{
		size_t n = frames < 512 ? frames : 512;
		for(size_t i = 0; i < n; i++)
		{
			if(in_channels == 1)
				stereo[i * 2] = stereo[i * 2 + 1] = pcm[i];
			else
			{
				stereo[i * 2] = pcm[i * in_channels];
				stereo[i * 2 + 1] = pcm[i * in_channels + 1];
			}
		}
		ar_push(&ring, stereo, n);
		pcm += n * (in_channels == 1 ? 1 : in_channels);
		frames -= n;
	}
	chiaki_mutex_unlock(&ring_mutex);
}

void audio_reset(void)
{
	if(!initialized)
		return;
	chiaki_mutex_lock(&ring_mutex);
	ar_init(&ring, TARGET_START_MS, TARGET_MIN_MS, TARGET_MAX_MS);
	chiaki_mutex_unlock(&ring_mutex);
}

static void fill_block(float *out)
{
	chiaki_mutex_lock(&ring_mutex);
	ar_pull(&ring, out, AUDIO_BLOCK_SAMPLES);
	stats.underruns = ring.underruns;
	stats.dropped_frames = ring.dropped_frames;
	stats.buffered_ms = ar_buffered_ms(&ring);
	stats.target_ms = ar_target_ms(&ring);
	stats.speed_ppm = ring.primed ? (int32_t)((ring.ratio - 1.0) * 1e6) : 0;
	chiaki_mutex_unlock(&ring_mutex);
}

static void *audio_thread(void *arg)
{
	(void)arg;
	float *data = (float *)(uintptr_t)port_config.audioDataStart;
	uint64_t blocks = port_config.numBlocks;
	uint64_t write_block = (*(volatile uint64_t *)(uintptr_t)port_config.readIndex + 1) % blocks;

	while(running)
	{
		sys_event_t event;
		sysEventQueueReceive(event_queue, &event, 20 * 1000);
		uint64_t read_block = *(volatile uint64_t *)(uintptr_t)port_config.readIndex;
		// Stay BLOCKS_AHEAD blocks in front of the hardware.
		while(((write_block + blocks - read_block) % blocks) <= BLOCKS_AHEAD)
		{
			fill_block(data + write_block * port_config.channelCount * AUDIO_BLOCK_SAMPLES);
			write_block = (write_block + 1) % blocks;
		}
	}
	return NULL;
}

bool audio_init(ChiakiLog *log)
{
	alog = log;
	if(audioInit() != 0)
	{
		CHIAKI_LOGE(alog, "audioInit failed");
		return false;
	}

	audioPortParam params;
	memset(&params, 0, sizeof(params));
	params.numChannels = AUDIO_PORT_2CH;
	params.numBlocks = AUDIO_BLOCK_8;
	params.attrib = AUDIO_PORT_INITLEVEL;
	params.level = 1.0f;
	if(audioPortOpen(&params, &port) != 0 || audioGetPortConfig(port, &port_config) != 0)
	{
		CHIAKI_LOGE(alog, "audioPortOpen failed");
		audioQuit();
		return false;
	}
	memset((void *)(uintptr_t)port_config.audioDataStart, 0, port_config.portSize);

	if(audioCreateNotifyEventQueue(&event_queue, &event_key) != 0 || audioSetNotifyEventQueue(event_key) != 0)
	{
		CHIAKI_LOGE(alog, "audio event queue setup failed");
		audioPortClose(port);
		audioQuit();
		return false;
	}
	sysEventQueueDrain(event_queue);

	chiaki_mutex_init(&ring_mutex, false);
	ar_init(&ring, TARGET_START_MS, TARGET_MIN_MS, TARGET_MAX_MS);
	initialized = true;

	audioPortStart(port);
	running = true;
	if(chiaki_thread_create(&thread, audio_thread, NULL) != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(alog, "audio thread creation failed");
		running = false;
		audio_fini();
		return false;
	}
	CHIAKI_LOGI(alog, "Audio output ready (%u blocks)", (unsigned)port_config.numBlocks);
	return true;
}

void audio_fini(void)
{
	if(!initialized)
		return;
	if(running)
	{
		running = false;
		chiaki_thread_join(&thread, NULL);
	}
	audioPortStop(port);
	audioRemoveNotifyEventQueue(event_key);
	audioPortClose(port);
	sysEventQueueDestroy(event_queue, 0);
	audioQuit();
	chiaki_mutex_fini(&ring_mutex);
	initialized = false;
}

void audio_get_stats(AudioStats *out)
{
	*out = stats;
}
