// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "framequeue.h"

#include <string.h>

void fq_set_pacing(FrameQueue *q, FqPacing pacing)
{
	switch(pacing)
	{
		case FQ_PACING_LOWEST_DELAY: q->catchup_len = 1; q->catchup_vsyncs = 2; break;
		case FQ_PACING_SMOOTHEST: q->catchup_len = 2; q->catchup_vsyncs = 8; break;
		case FQ_PACING_BALANCED:
		default: q->catchup_len = 1; q->catchup_vsyncs = 8; break;
	}
}

void fq_init(FrameQueue *q, FqPacing pacing)
{
	memset(q, 0, sizeof(*q));
	q->shown = -1;
	q->writing = -1;
	fq_set_pacing(q, pacing);
}

static bool in_use(const FrameQueue *q, int idx)
{
	if(idx == q->shown || idx == q->writing)
		return true;
	for(int i = 0; i < q->len; i++)
		if(q->queue[i] == idx)
			return true;
	return false;
}

static int pop_oldest(FrameQueue *q)
{
	int idx = q->queue[0];
	q->len--;
	memmove(q->queue, q->queue + 1, (size_t)q->len * sizeof(int));
	return idx;
}

int fq_acquire(FrameQueue *q)
{
	for(int idx = 0; idx < FQ_BUFS; idx++)
	{
		if(!in_use(q, idx))
		{
			q->writing = idx;
			return idx;
		}
	}
	// Cannot happen with FQ_BUFS = FQ_QUEUE_MAX + 2, but stay safe: reuse the oldest waiting frame.
	q->skipped++;
	q->writing = pop_oldest(q);
	return q->writing;
}

void fq_commit(FrameQueue *q, int idx)
{
	if(idx != q->writing)
		return;
	q->writing = -1;
	if(q->len == FQ_QUEUE_MAX)
	{
		pop_oldest(q); // far behind: drop the oldest waiting frame
		q->skipped++;
	}
	q->queue[q->len++] = idx;
}

void fq_abort(FrameQueue *q, int idx)
{
	if(idx == q->writing)
		q->writing = -1;
}

int fq_present(FrameQueue *q, bool *is_new)
{
	bool fresh = false;
	if(q->len > 0)
	{
		q->shown = pop_oldest(q);
		q->presented++;
		fresh = true;

		// Keep the delay low: a sustained backlog means we are a frame behind.
		if(q->len >= q->catchup_len)
		{
			if(++q->backlog_vsyncs >= q->catchup_vsyncs)
			{
				// the frame popped above is replaced before it is shown
				q->shown = pop_oldest(q);
				q->skipped++;
				q->backlog_vsyncs = 0;
			}
		}
		else
			q->backlog_vsyncs = 0;
	}
	else if(q->shown >= 0)
		q->repeated++;
	if(is_new)
		*is_new = fresh;
	return q->shown;
}
