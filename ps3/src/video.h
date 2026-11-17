// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PS3 video: RSX display output and hardware H.264 decoding with libvdec.
#ifndef CHIAKI_PS3_VIDEO_H
#define CHIAKI_PS3_VIDEO_H

#include <chiaki/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Display (RSX). Prefers 720p output when the TV supports it.
bool video_display_init(ChiakiLog *log);
void video_display_fini(void);
// Background color shown when no video frame is available (0xRRGGBB).
void video_display_set_background(uint32_t rgb);
// Draws the most recent decoded frame (or the background) and flips on vsync.
// Call once per frame from the main loop.
void video_display_present(void);
void video_display_get_size(uint32_t *width, uint32_t *height);
// Menu mode: show a 1280x720 X8R8G8B8 image (copied) instead of video, until hidden.
void video_display_show_ui(const uint32_t *pixels);
void video_display_hide_ui(void);
// Small image drawn over the video in the top-left corner (stats overlay):
// VIDEO_OVERLAY_W x VIDEO_OVERLAY_H X8R8G8B8 pixels (copied), or NULL to hide it.
#define VIDEO_OVERLAY_W 640
#define VIDEO_OVERLAY_H 64
void video_display_set_overlay(const uint32_t *pixels);
// Frame pacing: 0 = lowest delay, 1 = balanced, 2 = smoothest (see framequeue.h).
void video_set_pacing(int pacing);
// Rewrite the H.264 SPS to declare no frame reordering (see h264_sps.h). Default on.
void video_set_low_latency_decode(bool enabled);
// Pick the frame shortly before the vsync instead of right after the previous one. Default on.
void video_set_late_latch(bool enabled);
// Which step video_display_present() is in (for hang diagnostics).
extern volatile const char *video_present_stage;

// Decoder (libvdec, runs on the SPUs).
bool video_decoder_init(void);
void video_decoder_fini(void);
// Feed one H.264 access unit (Annex B, as delivered by chiaki). Thread-safe
// w.r.t. the display, but must be called from a single thread.
bool video_decoder_decode(const uint8_t *buf, size_t buf_size);

typedef struct
{
	uint64_t frames_in;
	uint64_t frames_out;
	uint64_t frames_presented; // new frames that reached the screen
	uint64_t frames_skipped;   // decoded but never shown (backlog catch-up)
	uint64_t frames_repeated;  // vsyncs that showed the previous frame again
	uint64_t frames_dropped;   // pictures vdec marked as skipped
	uint64_t decode_errors;
	uint32_t width;
	uint32_t height;
	// decoder delay (AU submitted -> picture out) since the last call
	uint32_t decode_ms_avg;
	uint32_t decode_ms_max;
	uint32_t decode_held_x100; // average newer frames already submitted when a picture came out, x100
	uint64_t flips_late;       // vsyncs missed with late frame pick active
	uint32_t latch_margin_us;  // current late frame pick margin (0 = off)
	uint32_t vsync_hz_x100;    // measured display refresh rate
	bool eop_marker;           // end-of-picture marker active
} VideoStats;
// Copies the counters without resetting the decoder delay window.
void video_peek_stats(VideoStats *stats);
// Returns the stats and resets the decoder delay window.
void video_get_stats(VideoStats *stats);

#endif
