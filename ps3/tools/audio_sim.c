// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Simulates PS4 -> PS3 audio on a PC: 10 ms packets from a clock running
// slightly fast, network jitter and stalls, playback of 256-frame blocks at
// exactly 48 kHz. Compares the previous FIFO with src/audioring.c.
// Build: gcc -O2 -o audio_sim tools/audio_sim.c src/audioring.c -lm
#include "../src/audioring.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 256
#define PACKET 480

static double randn(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

// --- the previous algorithm (audio.c before this change) ---
#define OLD_RING (AR_RATE / 2)
#define OLD_MAX (AR_RATE * 120 / 1000)
#define OLD_PRIME (BLOCK * 3)
static int16_t old_ring[OLD_RING * 2];
static size_t old_read, old_fill; static int old_primed; static long old_under, old_drop;
static void old_push(const int16_t *pcm, size_t n)
{
	for(size_t i = 0; i < n; i++)
	{
		if(old_fill == OLD_RING) { old_read = (old_read + 1) % OLD_RING; old_fill--; old_drop++; }
		size_t w = (old_read + old_fill) % OLD_RING;
		old_ring[w * 2] = pcm[i * 2]; old_ring[w * 2 + 1] = pcm[i * 2 + 1]; old_fill++;
	}
	if(old_fill > OLD_MAX) { size_t d = old_fill - OLD_MAX / 2; old_read = (old_read + d) % OLD_RING; old_fill -= d; old_drop += d; }
}
static void old_pull(float *out)
{
	if(!old_primed && old_fill >= OLD_PRIME) old_primed = 1;
	size_t avail = old_primed ? old_fill : 0, n = avail < BLOCK ? avail : BLOCK;
	for(size_t i = 0; i < n; i++)
	{
		size_t rd = (old_read + i) % OLD_RING;
		out[i * 2] = old_ring[rd * 2] / 32768.0f; out[i * 2 + 1] = old_ring[rd * 2 + 1] / 32768.0f;
	}
	old_read = (old_read + n) % OLD_RING; old_fill -= n;
	if(n < BLOCK) { memset(out + n * 2, 0, (BLOCK - n) * 2 * sizeof(float)); if(old_primed) { old_under++; old_primed = 0; } }
}

typedef struct { double drift_ppm, jitter_ms, stall_prob, stall_ms; } Net;

static unsigned T_START = 50, T_MIN = 40, T_MAX = 150;
static void run(const char *label, Net net, unsigned seed)
{
	srand(seed);
	static AudioRing ring;
	ar_init(&ring, T_START, T_MIN, T_MAX);
	memset(old_ring, 0, sizeof(old_ring)); old_read = old_fill = 0; old_primed = 0; old_under = old_drop = 0;

	const double seconds = 420;
	const double pkt_interval = 10.0 * (1.0 - net.drift_ppm * 1e-6); // PS4 clock a bit fast
	const double blk_interval = 1000.0 * BLOCK / AR_RATE;
	double next_pkt = 0, next_blk = 0, stall = 0, tone_phase = 0;
	int16_t pkt[PACKET * 2];
	float out[BLOCK * 2];
	float last_old = 0, last_new = 0;
	long clicks_old = 0, clicks_new = 0;
	long silent_old = 0, silent_new = 0, run_old = 0, run_new = 0;
	double lvl_old = 0, lvl_new = 0; long lvl_n = 0;
	double ratio_min = 2, ratio_max = 0;
	long pkt_k = 0;

	// packet arrival times: nominal + jitter + stalls (delayed packets then arrive together)
	double arrival = 0;
	for(double t = 0; t < seconds * 1000; )
	{
		double nominal = pkt_k * pkt_interval;
		if((double)rand() / RAND_MAX < net.stall_prob) stall = net.stall_ms;
		arrival = nominal + 30 + fabs(randn()) * net.jitter_ms + stall;
		stall = stall > pkt_interval ? stall - pkt_interval : 0;
		// play blocks due before this packet arrives
		while(next_blk < arrival)
		{
			old_pull(out);
			for(int i = 0; i < BLOCK; i++) { if(fabsf(out[i * 2] - last_old) > 0.2f) clicks_old++; last_old = out[i * 2]; if(out[i * 2] == 0.0f && out[i * 2 + 1] == 0.0f) { if(++run_old == 8 && next_blk > 5000) silent_old += 8; else if(run_old > 8 && next_blk > 5000) silent_old++; } else run_old = 0; }
			ar_pull(&ring, out, BLOCK);
			for(int i = 0; i < BLOCK; i++) { if(fabsf(out[i * 2] - last_new) > 0.2f) clicks_new++; last_new = out[i * 2]; if(out[i * 2] == 0.0f && out[i * 2 + 1] == 0.0f) { if(++run_new == 8 && next_blk > 5000) silent_new += 8; else if(run_new > 8 && next_blk > 5000) silent_new++; } else run_new = 0; }
			if(next_blk > 60000)
			{
				lvl_old += old_fill; lvl_new += ring.fill; lvl_n++;
				if(ring.primed) { if(ring.ratio < ratio_min) ratio_min = ring.ratio; if(ring.ratio > ratio_max) ratio_max = ring.ratio; }
			}
			next_blk += blk_interval;
		}
		for(int i = 0; i < PACKET; i++)
		{
			int16_t v = (int16_t)(sin(tone_phase) * 16000);
			tone_phase += 2 * M_PI * 1000.0 / AR_RATE;
			pkt[i * 2] = pkt[i * 2 + 1] = v;
		}
		old_push(pkt, PACKET);
		ar_push(&ring, pkt, PACKET);
		pkt_k++;
		t = arrival;
		(void)next_pkt;
	}
	printf("%-15s old: underruns %2ld silence %4.0f ms dropped %3.0f ms delay %3.0f ms | new: underruns %2llu silence %4.0f ms dropped %3.0f ms delay %3.0f ms (target %3u) speed %+.2f%%..%+.2f%%\n",
			label, old_under, silent_old * 1000.0 / AR_RATE, old_drop * 1000.0 / AR_RATE, lvl_old / lvl_n * 1000 / AR_RATE,
			(unsigned long long)ring.underruns, silent_new * 1000.0 / AR_RATE, ring.dropped_frames * 1000.0 / AR_RATE, lvl_new / lvl_n * 1000 / AR_RATE,
			ar_target_ms(&ring), (ratio_min - 1) * 100, (ratio_max - 1) * 100);
	(void)clicks_old; (void)clicks_new;
}

int main(int argc, char **argv)
{
	if(argc == 4) { T_START = atoi(argv[1]); T_MIN = atoi(argv[2]); T_MAX = atoi(argv[3]); }
	printf("target start %u ms, min %u ms, max %u ms\n", T_START, T_MIN, T_MAX);
	run("like the log", (Net){ 130, 3.0, 0.0004, 45 }, 1);
	run("calm", (Net){ 130, 1.5, 0.0001, 30 }, 2);
	run("rough network", (Net){ 130, 6.0, 0.002, 70 }, 3);
	run("PS3 clock fast", (Net){ -150, 3.0, 0.0004, 45 }, 4);
	return 0;
}
