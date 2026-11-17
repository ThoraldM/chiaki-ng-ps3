// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "h264_sps.h"

#include <stdio.h>
#include <string.h>

#define SPS_MAX 512

// --- bit reader / writer ---------------------------------------------------------

typedef struct { const uint8_t *d; size_t bits; size_t pos; bool err; } BitReader;
typedef struct { uint8_t *d; size_t cap_bits; size_t pos; bool err; } BitWriter;

static uint32_t rd_bit(BitReader *r)
{
	if(r->pos >= r->bits) { r->err = true; return 0; }
	uint32_t b = (r->d[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
	r->pos++;
	return b;
}

static uint32_t rd_bits(BitReader *r, int n)
{
	uint32_t v = 0;
	for(int i = 0; i < n; i++)
		v = (v << 1) | rd_bit(r);
	return v;
}

static uint32_t rd_ue(BitReader *r)
{
	int zeros = 0;
	while(!rd_bit(r))
	{
		if(r->err || ++zeros > 31) { r->err = true; return 0; }
	}
	return (uint32_t)((1ull << zeros) - 1 + rd_bits(r, zeros));
}

static int32_t rd_se(BitReader *r)
{
	uint32_t k = rd_ue(r);
	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

static void wr_bit(BitWriter *w, uint32_t b)
{
	if(w->pos >= w->cap_bits) { w->err = true; return; }
	uint8_t *p = &w->d[w->pos >> 3];
	int sh = 7 - (int)(w->pos & 7);
	if(sh == 7)
		*p = 0;
	*p |= (uint8_t)((b & 1) << sh);
	w->pos++;
}

static void wr_bits(BitWriter *w, uint32_t v, int n)
{
	for(int i = n - 1; i >= 0; i--)
		wr_bit(w, (v >> i) & 1);
}

static void wr_ue(BitWriter *w, uint32_t v)
{
	uint64_t x = (uint64_t)v + 1;
	int len = 0;
	while((x >> len) > 1)
		len++;
	wr_bits(w, 0, len);
	wr_bits(w, (uint32_t)x, len + 1);
}

// --- SPS parsing -------------------------------------------------------------------

typedef struct
{
	size_t vui_flag_pos;  // bit position of vui_parameters_present_flag
	bool vui_present;
	uint32_t profile, level;
	size_t bsr_flag_pos;  // bit position of bitstream_restriction_flag (if VUI present)
	bool bsr_present;
	uint32_t max_num_ref_frames;
	// existing bitstream_restriction fields (or defaults)
	uint32_t mv_over_pic_boundaries, max_bytes_per_pic_denom, max_bits_per_mb_denom;
	uint32_t log2_max_mv_h, log2_max_mv_v, max_num_reorder, max_dec_buffering;
} SpsInfo;

static void skip_scaling_list(BitReader *r, int size)
{
	int last = 8, next = 8;
	for(int j = 0; j < size && !r->err; j++)
	{
		if(next != 0)
			next = (last + rd_se(r) + 256) % 256;
		last = next == 0 ? last : next;
	}
}

static void skip_hrd(BitReader *r)
{
	uint32_t cpb_cnt = rd_ue(r) + 1;
	if(cpb_cnt > 32) { r->err = true; return; }
	rd_bits(r, 4); // bit_rate_scale
	rd_bits(r, 4); // cpb_size_scale
	for(uint32_t i = 0; i < cpb_cnt && !r->err; i++)
	{
		rd_ue(r); // bit_rate_value_minus1
		rd_ue(r); // cpb_size_value_minus1
		rd_bit(r); // cbr_flag
	}
	rd_bits(r, 5 * 4); // the four *_length_minus1 fields
}

static bool parse_sps(const uint8_t *rbsp, size_t len, SpsInfo *s)
{
	BitReader r = { rbsp, len * 8, 0, false };
	memset(s, 0, sizeof(*s));
	uint32_t profile = rd_bits(&r, 8);
	rd_bits(&r, 8);  // constraint flags
	s->profile = profile;
	s->level = rd_bits(&r, 8);  // level_idc
	rd_ue(&r);       // seq_parameter_set_id
	if(profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
			profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
			profile == 139 || profile == 134 || profile == 135)
	{
		uint32_t chroma = rd_ue(&r);
		if(chroma == 3)
			rd_bit(&r); // separate_colour_plane_flag
		rd_ue(&r); // bit_depth_luma_minus8
		rd_ue(&r); // bit_depth_chroma_minus8
		rd_bit(&r); // qpprime_y_zero_transform_bypass_flag
		if(rd_bit(&r)) // seq_scaling_matrix_present_flag
		{
			for(int i = 0; i < (chroma != 3 ? 8 : 12) && !r.err; i++)
				if(rd_bit(&r))
					skip_scaling_list(&r, i < 6 ? 16 : 64);
		}
	}
	rd_ue(&r); // log2_max_frame_num_minus4
	uint32_t poc_type = rd_ue(&r);
	if(poc_type == 0)
		rd_ue(&r); // log2_max_pic_order_cnt_lsb_minus4
	else if(poc_type == 1)
	{
		rd_bit(&r);
		rd_se(&r);
		rd_se(&r);
		uint32_t n = rd_ue(&r);
		if(n > 255) return false;
		for(uint32_t i = 0; i < n && !r.err; i++)
			rd_se(&r);
	}
	s->max_num_ref_frames = rd_ue(&r);
	rd_bit(&r); // gaps_in_frame_num_value_allowed_flag
	rd_ue(&r);  // pic_width_in_mbs_minus1
	rd_ue(&r);  // pic_height_in_map_units_minus1
	if(!rd_bit(&r)) // frame_mbs_only_flag
		rd_bit(&r); // mb_adaptive_frame_field_flag
	rd_bit(&r); // direct_8x8_inference_flag
	if(rd_bit(&r)) // frame_cropping_flag
	{
		rd_ue(&r); rd_ue(&r); rd_ue(&r); rd_ue(&r);
	}
	if(r.err)
		return false;
	s->vui_flag_pos = r.pos;
	s->vui_present = rd_bit(&r);

	// defaults when bitstream_restriction is absent
	s->mv_over_pic_boundaries = 1;
	s->max_bytes_per_pic_denom = 2;
	s->max_bits_per_mb_denom = 1;
	s->log2_max_mv_h = 16;
	s->log2_max_mv_v = 16;
	if(!s->vui_present)
		return !r.err;

	if(rd_bit(&r)) // aspect_ratio_info_present_flag
	{
		if(rd_bits(&r, 8) == 255) // Extended_SAR
			rd_bits(&r, 32);
	}
	if(rd_bit(&r)) // overscan_info_present_flag
		rd_bit(&r);
	if(rd_bit(&r)) // video_signal_type_present_flag
	{
		rd_bits(&r, 4); // video_format, video_full_range_flag
		if(rd_bit(&r)) // colour_description_present_flag
			rd_bits(&r, 24);
	}
	if(rd_bit(&r)) // chroma_loc_info_present_flag
	{
		rd_ue(&r);
		rd_ue(&r);
	}
	if(rd_bit(&r)) // timing_info_present_flag
	{
		rd_bits(&r, 32);
		rd_bits(&r, 32);
		rd_bit(&r);
	}
	bool nal_hrd = rd_bit(&r);
	if(nal_hrd)
		skip_hrd(&r);
	bool vcl_hrd = rd_bit(&r);
	if(vcl_hrd)
		skip_hrd(&r);
	if(nal_hrd || vcl_hrd)
		rd_bit(&r); // low_delay_hrd_flag
	rd_bit(&r); // pic_struct_present_flag
	if(r.err)
		return false;
	s->bsr_flag_pos = r.pos;
	s->bsr_present = rd_bit(&r);
	if(s->bsr_present)
	{
		s->mv_over_pic_boundaries = rd_bit(&r);
		s->max_bytes_per_pic_denom = rd_ue(&r);
		s->max_bits_per_mb_denom = rd_ue(&r);
		s->log2_max_mv_h = rd_ue(&r);
		s->log2_max_mv_v = rd_ue(&r);
		s->max_num_reorder = rd_ue(&r);
		s->max_dec_buffering = rd_ue(&r);
	}
	return !r.err;
}

// Builds the new RBSP; returns its length or 0.
static size_t rewrite_sps(const uint8_t *rbsp, size_t len, uint8_t *out, size_t cap)
{
	SpsInfo s;
	if(!parse_sps(rbsp, len, &s))
		return 0;
	uint32_t want_buffering = s.max_num_ref_frames ? s.max_num_ref_frames : 1;
	if(s.bsr_present && s.max_num_reorder == 0 && s.max_dec_buffering <= want_buffering)
		return 0; // already low latency: leave it alone

	BitReader r = { rbsp, len * 8, 0, false };
	BitWriter w = { out, cap * 8, 0, false };
	size_t copy_until = s.vui_present ? s.bsr_flag_pos : s.vui_flag_pos;
	while(r.pos < copy_until)
		wr_bit(&w, rd_bit(&r));
	if(!s.vui_present)
	{
		wr_bit(&w, 1);    // vui_parameters_present_flag
		wr_bits(&w, 0, 8); // aspect, overscan, video_signal, chroma_loc, timing, nal_hrd, vcl_hrd, pic_struct
	}
	uint32_t dec_buffering = s.max_num_ref_frames ? s.max_num_ref_frames : 1;
	if(s.bsr_present && s.max_dec_buffering > dec_buffering)
		dec_buffering = s.max_dec_buffering;
	wr_bit(&w, 1); // bitstream_restriction_flag
	wr_bit(&w, s.mv_over_pic_boundaries);
	wr_ue(&w, s.max_bytes_per_pic_denom);
	wr_ue(&w, s.max_bits_per_mb_denom);
	wr_ue(&w, s.log2_max_mv_h);
	wr_ue(&w, s.log2_max_mv_v);
	wr_ue(&w, 0);              // max_num_reorder_frames
	wr_ue(&w, dec_buffering);  // max_dec_frame_buffering
	wr_bit(&w, 1);             // rbsp_stop_one_bit
	while(w.pos & 7)
		wr_bit(&w, 0);
	return w.err ? 0 : w.pos / 8;
}

// --- NAL handling --------------------------------------------------------------------

static size_t unescape(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	size_t o = 0, zeros = 0;
	for(size_t i = 0; i < len; i++)
	{
		if(zeros >= 2 && in[i] == 3)
		{
			zeros = 0;
			continue;
		}
		if(o >= cap)
			return 0;
		out[o++] = in[i];
		zeros = in[i] == 0 ? zeros + 1 : 0;
	}
	return o;
}

static size_t escape(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	size_t o = 0, zeros = 0;
	for(size_t i = 0; i < len; i++)
	{
		if(zeros >= 2 && in[i] <= 3)
		{
			if(o >= cap)
				return 0;
			out[o++] = 3;
			zeros = 0;
		}
		if(o >= cap)
			return 0;
		out[o++] = in[i];
		zeros = in[i] == 0 ? zeros + 1 : 0;
	}
	return o;
}

// Position of the next 00 00 01 at or after i, or size.
static size_t next_start_code(const uint8_t *d, size_t size, size_t i)
{
	for(; i + 2 < size; i++)
		if(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
			return i;
	return size;
}

size_t h264_low_latency_rewrite(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_cap, int *rewritten)
{
	if(rewritten)
		*rewritten = 0;
	size_t o = 0;
	size_t sc = next_start_code(in, in_size, 0);
	if(sc > out_cap)
		return 0;
	memcpy(out, in, sc); // anything before the first start code
	o = sc;
	while(sc < in_size)
	{
		size_t nal = sc + 3;
		size_t next = next_start_code(in, in_size, nal);
		size_t nal_end = next;
		while(nal_end > nal && in[nal_end - 1] == 0)
			nal_end--; // trailing zeros belong to the next (4-byte) start code

		// start code
		if(o + 3 > out_cap)
			return 0;
		memcpy(out + o, in + sc, 3);
		o += 3;

		bool done = false;
		if(nal < nal_end && (in[nal] & 0x1f) == 7 && nal_end - nal - 1 <= SPS_MAX)
		{
			uint8_t rbsp[SPS_MAX], new_rbsp[SPS_MAX + H264_SPS_REWRITE_GROWTH];
			size_t rlen = unescape(in + nal + 1, nal_end - nal - 1, rbsp, sizeof(rbsp));
			size_t nlen = rlen ? rewrite_sps(rbsp, rlen, new_rbsp, sizeof(new_rbsp)) : 0;
			if(nlen && o + 1 < out_cap)
			{
				out[o] = in[nal]; // NAL header
				size_t elen = escape(new_rbsp, nlen, out + o + 1, out_cap - o - 1);
				if(elen)
				{
					o += 1 + elen;
					done = true;
					if(rewritten)
						(*rewritten)++;
				}
			}
		}
		if(!done)
		{
			if(o + (nal_end - nal) > out_cap)
				return 0;
			memcpy(out + o, in + nal, nal_end - nal);
			o += nal_end - nal;
		}
		// trailing zero bytes before the next start code
		if(o + (next - nal_end) > out_cap)
			return 0;
		memcpy(out + o, in + nal_end, next - nal_end);
		o += next - nal_end;
		sc = next;
	}
	return o;
}

bool h264_sps_describe(const uint8_t *in, size_t in_size, char *out, size_t out_cap)
{
	size_t sc = next_start_code(in, in_size, 0);
	while(sc < in_size)
	{
		size_t nal = sc + 3;
		size_t next = next_start_code(in, in_size, nal);
		size_t nal_end = next;
		while(nal_end > nal && in[nal_end - 1] == 0)
			nal_end--;
		if(nal < nal_end && (in[nal] & 0x1f) == 7 && nal_end - nal - 1 <= SPS_MAX)
		{
			uint8_t rbsp[SPS_MAX];
			size_t rlen = unescape(in + nal + 1, nal_end - nal - 1, rbsp, sizeof(rbsp));
			SpsInfo si;
			if(!rlen || !parse_sps(rbsp, rlen, &si))
			{
				snprintf(out, out_cap, "SPS (could not parse, %u bytes)", (unsigned)rlen);
				return true;
			}
			if(si.bsr_present)
				snprintf(out, out_cap, "profile %u level %u ref_frames %u vui yes reorder %u dec_buffering %u",
						(unsigned)si.profile, (unsigned)si.level, (unsigned)si.max_num_ref_frames,
						(unsigned)si.max_num_reorder, (unsigned)si.max_dec_buffering);
			else
				snprintf(out, out_cap, "profile %u level %u ref_frames %u vui %s, no reorder info",
						(unsigned)si.profile, (unsigned)si.level, (unsigned)si.max_num_ref_frames,
						si.vui_present ? "yes" : "no");
			return true;
		}
		sc = next;
	}
	return false;
}

#ifdef H264_SPS_TEST
// Test helper: removes bitstream_restriction (mode 1) or the whole VUI (mode 2) from every SPS.
static size_t strip_sps(const uint8_t *rbsp, size_t len, uint8_t *out, size_t cap, int mode)
{
	SpsInfo s;
	if(!parse_sps(rbsp, len, &s) || !s.vui_present)
		return 0;
	BitReader r = { rbsp, len * 8, 0, false };
	BitWriter w = { out, cap * 8, 0, false };
	size_t until = mode == 1 ? s.bsr_flag_pos : s.vui_flag_pos;
	while(r.pos < until)
		wr_bit(&w, rd_bit(&r));
	wr_bit(&w, 0); // flag off
	wr_bit(&w, 1);
	while(w.pos & 7)
		wr_bit(&w, 0);
	return w.err ? 0 : w.pos / 8;
}

size_t h264_sps_test_strip(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_cap, int mode)
{
	size_t o = 0, sc = next_start_code(in, in_size, 0);
	memcpy(out, in, sc);
	o = sc;
	while(sc < in_size)
	{
		size_t nal = sc + 3, next = next_start_code(in, in_size, nal), nal_end = next;
		while(nal_end > nal && in[nal_end - 1] == 0)
			nal_end--;
		memcpy(out + o, in + sc, 3);
		o += 3;
		size_t nlen = 0;
		if((in[nal] & 0x1f) == 7)
		{
			uint8_t rbsp[SPS_MAX], nr[SPS_MAX];
			size_t rlen = unescape(in + nal + 1, nal_end - nal - 1, rbsp, sizeof(rbsp));
			nlen = strip_sps(rbsp, rlen, nr, sizeof(nr), mode);
			if(nlen)
			{
				out[o] = in[nal];
				o += 1 + escape(nr, nlen, out + o + 1, out_cap - o - 1);
			}
		}
		if(!nlen)
		{
			memcpy(out + o, in + nal, nal_end - nal);
			o += nal_end - nal;
		}
		memcpy(out + o, in + nal_end, next - nal_end);
		o += next - nal_end;
		sc = next;
	}
	return o;
}
#endif
