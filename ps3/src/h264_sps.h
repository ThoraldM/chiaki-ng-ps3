// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// H.264 SPS rewrite for low-latency decoding (platform independent).
//
// Without VUI bitstream_restriction, an H.264 decoder must assume frames may
// be reordered and can hold decoded pictures back before output. Remote Play
// streams never reorder, so declaring max_num_reorder_frames = 0 lets the
// decoder output each picture as soon as it is decoded (Moonlight does the
// same for some hardware decoders).
#ifndef CHIAKI_PS3_H264_SPS_H
#define CHIAKI_PS3_H264_SPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Extra bytes h264_low_latency_rewrite() may add per SPS.
#define H264_SPS_REWRITE_GROWTH 32

// Copies an Annex B buffer to out, rewriting every SPS NAL unit so that its VUI
// declares max_num_reorder_frames = 0 and max_dec_frame_buffering =
// max_num_ref_frames. out_cap must be >= in_size + H264_SPS_REWRITE_GROWTH *
// (number of SPS units). Returns the output size, or 0 if out_cap is too small.
// *rewritten (may be NULL) counts the SPS units changed; an SPS that cannot be
// parsed is copied unchanged.
size_t h264_low_latency_rewrite(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_cap, int *rewritten);

// Writes a one-line description of the first SPS in the buffer (profile, level,
// reorder fields). Returns false if the buffer holds no SPS.
bool h264_sps_describe(const uint8_t *in, size_t in_size, char *out, size_t out_cap);

#endif
