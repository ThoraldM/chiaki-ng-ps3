// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Frame pacing: decoded frames wait in a short FIFO and are shown one per vsync,
// instead of always showing only the newest (which skips a frame whenever two
// arrive within one refresh and repeats one when none arrives).
// Platform independent and not thread safe: the caller holds a lock.
#ifndef CHIAKI_PS3_FRAMEQUEUE_H
#define CHIAKI_PS3_FRAMEQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#define FQ_BUFS       5   // queue (FQ_QUEUE_MAX) + on screen + being decoded
#define FQ_QUEUE_MAX  3

// Trade-off between smoothness and delay. When at least catchup_len frames
// are still waiting after a vsync for catchup_vsyncs vsyncs in a row, one is
// skipped to bring the delay back down. Simulated (tools/pacing_sim.c) with
// jitter like a real session: newest-only shows ~90% of frames at ~8 ms wait.
typedef enum
{
	FQ_PACING_LOWEST_DELAY = 0, // ~93% shown, ~10 ms
	FQ_PACING_BALANCED = 1,     // ~97% shown, ~15 ms
	FQ_PACING_SMOOTHEST = 2     // ~99% shown, ~26 ms
} FqPacing;

typedef struct
{
	int queue[FQ_QUEUE_MAX]; // buffer indices, oldest first
	int len;
	int shown;               // buffer on screen, -1 = none
	int writing;             // buffer the decoder is filling, -1 = none
	int backlog_vsyncs;
	int catchup_len;
	int catchup_vsyncs;

	uint64_t presented;      // new frames that reached the screen
	uint64_t skipped;        // decoded frames never shown
	uint64_t repeated;       // vsyncs that showed the previous frame again
} FrameQueue;

void fq_init(FrameQueue *q, FqPacing pacing);
// Change the policy without losing queued frames.
void fq_set_pacing(FrameQueue *q, FqPacing pacing);
// Decoder: a buffer to write the next frame into (never the one on screen).
int fq_acquire(FrameQueue *q);
// Decoder: the frame in buffer idx is complete.
void fq_commit(FrameQueue *q, int idx);
// Decoder: the frame in buffer idx was not usable.
void fq_abort(FrameQueue *q, int idx);
// Display, once per vsync: returns the buffer to show (-1 = nothing yet).
// *is_new is set when it differs from the previous vsync.
int fq_present(FrameQueue *q, bool *is_new);

#endif
