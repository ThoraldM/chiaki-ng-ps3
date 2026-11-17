// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Simulates frame pacing on a PC: PS4 frames at 60 fps with arrival jitter,
// PS3 display at 59.94 Hz. Compares "show newest only" with src/framequeue.c.
// Build: gcc -O2 -o pacing_sim tools/pacing_sim.c src/framequeue.c -lm
#include "../src/framequeue.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static double randn(void)
{
	double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

typedef struct { double jitter_ms; double burst_prob; double burst_ms; } Net;

#define FRAMES 24000  // ~6.7 minutes
static double arrival[FRAMES];

static void make_arrivals(Net n, unsigned seed)
{
	srand(seed);
	double extra = 0;
	for(int k = 0; k < FRAMES; k++)
	{
		if((double)rand() / RAND_MAX < n.burst_prob)
			extra = n.burst_ms; // a stall: this and following frames late, then they bunch up
		double t = k * (1000.0 / 60.0) + 17.0 /* decode */ + fabs(randn()) * n.jitter_ms + extra;
		extra *= 0.5;
		arrival[k] = t;
	}
	// a frame cannot be decoded before the previous one
	for(int k = 1; k < FRAMES; k++)
		if(arrival[k] < arrival[k - 1] + 1.0)
			arrival[k] = arrival[k - 1] + 1.0;
}

static double LATCH_MARGIN_MS = -1; // <0: pick right after the previous vsync (display one refresh later)
static void run(const char *label, Net n, unsigned seed, FqPacing pacing)
{
	make_arrivals(n, seed);
	const double vsync = 1000.0 / 59.94;
	double end = arrival[FRAMES - 1];

	// old: newest frame at each vsync
	int k = 0, last_shown = -1; long shown_old = 0, repeat_old = 0;
	double lat_old = 0;
	for(double t = vsync; t < end; t += vsync)
	{
		int newest = -1;
		while(k < FRAMES && arrival[k] <= t) newest = k++;
		if(newest >= 0) { shown_old++; lat_old += t + vsync - arrival[newest]; last_shown = newest; }
		else if(last_shown >= 0) repeat_old++;
	}

	// new: frame queue; picked at pick_t, on screen at the next vsync
	FrameQueue q; fq_init(&q, pacing);
	int frame_of[FQ_BUFS];
	k = 0; long repeat_new = 0; double lat_new = 0; long lat_n = 0;
	for(double t = vsync; t < end; t += vsync)
	{
		double pick_t = LATCH_MARGIN_MS < 0 ? t : t + vsync - LATCH_MARGIN_MS;
		double screen_t = t + vsync;
		while(k < FRAMES && arrival[k] <= pick_t)
		{
			int b = fq_acquire(&q); frame_of[b] = k; fq_commit(&q, b); k++;
		}
		bool fresh; int b = fq_present(&q, &fresh);
		if(fresh) { lat_new += screen_t - arrival[frame_of[b]]; lat_n++; }
		else if(b >= 0) repeat_new++;
	}
	printf("%-28s old: shown %5.1f%%  repeats %5ld  on screen after %4.1f ms | queue: shown %5.1f%%  repeats %5ld  skipped %4llu  on screen after %4.1f ms\n",
			label, 100.0 * shown_old / FRAMES, repeat_old, lat_old / shown_old,
			100.0 * lat_n / FRAMES, repeat_new, (unsigned long long)q.skipped, lat_new / lat_n);
}

int main(int argc, char **argv)
{
	if(argc > 1)
		LATCH_MARGIN_MS = atof(argv[1]);
	printf("frame picked %s; times are decode -> on screen\n", LATCH_MARGIN_MS < 0 ? "right after the previous vsync" : "shortly before the vsync");
	Net calm = { 2.0, 0.002, 30 }, typical = { 4.0, 0.01, 40 }, rough = { 7.0, 0.03, 60 };
	const char *names[] = { "lowest delay", "balanced", "smoothest" };
	for(int p = 0; p < 3; p++)
	{
		printf("--- pacing: %s\n", names[p]);
		run("calm network", calm, 1, (FqPacing)p);
		run("typical (like the log)", typical, 2, (FqPacing)p);
		run("rough", rough, 3, (FqPacing)p);
	}
	return 0;
}
