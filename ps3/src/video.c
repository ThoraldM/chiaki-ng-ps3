// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PS3 video output.
//
// Decoding: libvdec (the firmware H.264 decoder, runs on SPUs). Each decoded
// picture is converted to ARGB by vdec itself and written straight into one of
// three frame buffers in RSX local memory.
//
// Display: every vsync the newest decoded frame is blitted (scaled, keeping
// the aspect ratio) by the RSX into the back buffer, which is then flipped.
// No pixel touches the PPU.

#include "video.h"
#include "framequeue.h"
#include "h264_sps.h"

#include <chiaki/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-asm.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysmodule/sysmodule.h>
#include <codec/vdec.h>
#include <sys/memory.h>
#include <sys/systime.h>

#define CB_SIZE        0x100000
#define HOST_SIZE      (32 * 1024 * 1024)
#define LABEL_INDEX    255

// Frame buffers are sized for up to 1080p so a resolution change needs no realloc.
#define FRAME_MAX_W    1920
#define FRAME_MAX_H    1088
#define FRAME_BUFS     FQ_BUFS

static ChiakiLog *vlog;

// ---------------------------------------------------------------------------
// display state

static gcmContextData *context;
static void *host_addr;
static uint32_t display_w, display_h;
static uint32_t color_pitch;
static uint32_t *color_buffer[2];
static uint32_t color_offset[2];
static uint32_t cur_fb;
static bool first_flip = true;
static uint32_t label_val = 1;
static uint32_t background = 0x000000;

// decoded frames (RSX local memory)
static uint32_t *frame_buf[FRAME_BUFS];
static uint32_t frame_offset[FRAME_BUFS];
static uint32_t frame_w[FRAME_BUFS], frame_h[FRAME_BUFS];
static ChiakiMutex frame_mutex;
static FrameQueue frame_queue;   // which buffers wait, are shown, are being decoded
static FqPacing frame_pacing = FQ_PACING_BALANCED;
static bool frame_mutex_ok;

static VideoStats stats;

// Menu images (RSX local memory), double-buffered: one may be read by an
// in-flight blit while the other is being filled.
#define UI_IMG_W 1280
#define UI_IMG_H 720
static uint32_t *ui_buf[2];
static uint32_t ui_offset[2];
static uint32_t *ovl_buf[2];
static uint32_t ovl_offset[2];
static int ovl_front = -1;
static bool ovl_on;
static int ui_front = -1;
static volatile bool ui_mode;

// Decoder delay measurement: AUs are tagged with a sequence number (userdata).
#define AU_TIMES 64
static uint64_t au_submit_us[AU_TIMES];
static uint64_t au_seq;
static uint64_t delay_sum_us, delay_count, delay_max_us;
// "Held" frames: how many newer AUs had been submitted when a picture came out
// (0 = output right after decoding, 1 = held back until the next frame arrived).
static uint64_t held_sum, held_count;
static bool low_latency_decode = true;
static uint64_t sps_rewrites;
static char last_sps_desc[160];

// End-of-picture marker: an access unit delimiter (NAL type 9) appended after
// each frame tells the decoder the picture is complete, so it does not have to
// wait for the start of the next frame before output. Switched off for the rest
// of the session if pictures stop coming out with it.
#define AUD_STALL_AUS 30
static const uint8_t aud_nal[6] = { 0, 0, 0, 1, 0x09, 0xf0 };
static bool aud_enabled;
static volatile uint64_t last_out_seq;  // au_seq when the latest picture came out

static void rsx_wait_finish(void)
{
	rsxSetWriteBackendLabel(context, LABEL_INDEX, label_val);
	rsxFlushBuffer(context);
	while(*(vu32 *)gcmGetLabelAddress(LABEL_INDEX) != label_val)
		usleep(30);
	label_val++;
}

static void rsx_wait_idle(void)
{
	rsxSetWriteBackendLabel(context, LABEL_INDEX, label_val);
	rsxSetWaitLabel(context, LABEL_INDEX, label_val);
	label_val++;
	rsx_wait_finish();
}

static void set_render_target(uint32_t index)
{
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = color_offset[index];
	sf.colorPitch[0] = color_pitch;
	for(int i = 1; i < 4; i++)
	{
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z16;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = 0;
	sf.depthPitch = 64;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = display_w;
	sf.height = display_h;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(context, &sf);
}

bool video_display_init(ChiakiLog *log)
{
	vlog = log;
	host_addr = memalign(1024 * 1024, HOST_SIZE);
	if(!host_addr)
		return false;
	if(rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr) != 0 || !context)
	{
		CHIAKI_LOGE(vlog, "rsxInit failed");
		return false;
	}

	videoState state;
	videoGetState(VIDEO_PRIMARY, 0, &state);
	uint8_t resolution = state.displayMode.resolution;
	// The stream is 720p, so 720p output avoids a scaling pass when the TV supports it.
	if(videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_AUTO, 0))
		resolution = VIDEO_RESOLUTION_720;

	videoResolution res;
	videoGetResolution(resolution, &res);

	videoConfiguration vconfig;
	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.aspect = VIDEO_ASPECT_AUTO;
	vconfig.pitch = res.width * sizeof(uint32_t);

	rsx_wait_idle();
	if(videoConfigure(VIDEO_PRIMARY, &vconfig, NULL, 0) != 0)
	{
		// Fall back to whatever the XMB is using.
		resolution = state.displayMode.resolution;
		videoGetResolution(resolution, &res);
		vconfig.resolution = resolution;
		vconfig.pitch = res.width * sizeof(uint32_t);
		videoConfigure(VIDEO_PRIMARY, &vconfig, NULL, 0);
	}
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	display_w = res.width;
	display_h = res.height;
	color_pitch = display_w * sizeof(uint32_t);
	for(int i = 0; i < 2; i++)
	{
		color_buffer[i] = rsxMemalign(64, display_h * color_pitch);
		if(!color_buffer[i])
			return false;
		rsxAddressToOffset(color_buffer[i], &color_offset[i]);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, display_w, display_h);
	}

	for(int i = 0; i < FRAME_BUFS; i++)
	{
		frame_buf[i] = rsxMemalign(128, FRAME_MAX_W * FRAME_MAX_H * 4);
		if(!frame_buf[i])
		{
			CHIAKI_LOGE(vlog, "Out of RSX memory for frame buffers");
			return false;
		}
		rsxAddressToOffset(frame_buf[i], &frame_offset[i]);
		frame_w[i] = frame_h[i] = 0;
	}
	for(int i = 0; i < 2; i++)
	{
		ui_buf[i] = rsxMemalign(128, UI_IMG_W * UI_IMG_H * 4);
		if(!ui_buf[i])
		{
			CHIAKI_LOGE(vlog, "Out of RSX memory for menu images");
			return false;
		}
		rsxAddressToOffset(ui_buf[i], &ui_offset[i]);
	}
	for(int i = 0; i < 2; i++)
	{
		ovl_buf[i] = rsxMemalign(128, VIDEO_OVERLAY_W * VIDEO_OVERLAY_H * 4);
		if(ovl_buf[i])
			rsxAddressToOffset(ovl_buf[i], &ovl_offset[i]);
		else
			CHIAKI_LOGW(vlog, "No RSX memory for the stats overlay");
	}
	fq_init(&frame_queue, frame_pacing);
	chiaki_mutex_init(&frame_mutex, false);
	frame_mutex_ok = true;

	gcmResetFlipStatus();
	set_render_target(cur_fb);
	CHIAKI_LOGI(vlog, "Display %ux%u", (unsigned)display_w, (unsigned)display_h);
	return true;
}

void video_display_fini(void)
{
	if(!context)
		return;
	gcmSetWaitFlip(context);
	rsxFinish(context, 1);
	if(frame_mutex_ok)
		chiaki_mutex_fini(&frame_mutex);
	frame_mutex_ok = false;
}

void video_display_set_background(uint32_t rgb)
{
	background = rgb;
}

void video_display_get_size(uint32_t *width, uint32_t *height)
{
	*width = display_w;
	*height = display_h;
}

volatile const char *video_present_stage = "idle";
static uint64_t cmdbuf_rewinds;

// The display froze ~89 s into streaming in two runs: the GPU stopped consuming
// the command buffer while the firmware callback was wrapping it around (the
// buffer is only ~28 KB, so that happens about twice a second; why it
// eventually stalls is unknown). With this manual rewind it ran 7+ minutes
// without a freeze: once the buffer is half used, and the GPU is idle (it has
// executed everything up to here), jump back to the start ourselves.
static void cmdbuf_rewind_if_needed(void)
{
	uint32_t used = (uint32_t)((context->current - context->begin) * sizeof(uint32_t));
	uint32_t total = (uint32_t)((context->end - context->begin) * sizeof(uint32_t));
	if(used < total / 2)
		return;

	video_present_stage = "rewind_idle";
	rsx_wait_idle(); // GPU has executed everything written so far: get == put

	uint32_t begin_offset;
	if(rsxAddressToOffset(context->begin, &begin_offset) != 0)
	{
		CHIAKI_LOGE(vlog, "Command buffer rewind: no offset for the buffer start");
		return;
	}

	// Write the jump without the usual flush (which would move put past it),
	// then point put at the start: the GPU reads the jump, lands at the start
	// with get == put and waits for new commands there.
	rsxSetJumpCommand(context, begin_offset);
	__sync_synchronize();
	gcmControlRegister *ctrl = gcmGetControlRegister();
	ctrl->put = begin_offset;
	__sync_synchronize();

	video_present_stage = "rewind_jump";
	uint64_t start = (uint64_t)sysGetSystemTime();
	while(ctrl->get != begin_offset)
	{
		if((uint64_t)sysGetSystemTime() - start > 100 * 1000)
		{
			CHIAKI_LOGE(vlog, "Command buffer rewind: GPU did not take the jump (get 0x%x, start 0x%x)",
					(unsigned)ctrl->get, (unsigned)begin_offset);
			break;
		}
		usleep(20);
	}
	context->current = context->begin;

	cmdbuf_rewinds++;
	if(cmdbuf_rewinds <= 3 || cmdbuf_rewinds % 1000 == 0)
		CHIAKI_LOGI(vlog, "Command buffer rewound (%llu, %u of %u bytes used)",
				(unsigned long long)cmdbuf_rewinds, (unsigned)used, (unsigned)total);
}
static uint64_t flip_timeouts;

// Late frame pick: after a vsync, sleep until shortly before the next one and
// only then pick the newest frame, so a frame that became ready meanwhile is
// shown at that vsync instead of one refresh later (~13 ms less on average in
// tools/pacing_sim.c). The margin adapts: +1 ms after a missed vsync, -0.5 ms
// after 10 s without one.
#define LATCH_MARGIN_START_US 4000
#define LATCH_MARGIN_MIN_US   3000
#define LATCH_MARGIN_MAX_US   10000
static bool late_latch = true;
static double vsync_period_us = 16683.0; // 59.94 Hz until measured
static uint64_t last_vsync_us;
static uint32_t latch_margin_us = LATCH_MARGIN_START_US;
static uint32_t latch_good_vsyncs;
static bool latch_was_active;

static void wait_flip(void)
{
	// A flip normally completes within one vsync (16.7 ms). Never wait forever:
	// a hang here would freeze the whole display loop.
	for(int i = 0; gcmGetFlipStatus() != 0; i++)
	{
		if(i >= 500) // 500 * 200 us = 100 ms
		{
			flip_timeouts++;
			if(flip_timeouts <= 5 || flip_timeouts % 100 == 0)
				CHIAKI_LOGW(vlog, "Flip did not complete within 100 ms (%llu times)", (unsigned long long)flip_timeouts);
			break;
		}
		usleep(200);
	}
	gcmResetFlipStatus();
}

static void blit_rect(uint32_t img_offset, uint32_t fw, uint32_t fh, int32_t out_x, int32_t out_y, uint32_t out_w, uint32_t out_h);

static void blit_image(uint32_t img_offset, uint32_t fw, uint32_t fh)
{
	if(!fw || !fh)
		return;

	// Fit into the display, keeping the aspect ratio (letterbox/pillarbox).
	uint32_t out_w = display_w;
	uint32_t out_h = (uint32_t)((uint64_t)display_w * fh / fw);
	if(out_h > display_h)
	{
		out_h = display_h;
		out_w = (uint32_t)((uint64_t)display_h * fw / fh);
	}
	int32_t out_x = (int32_t)(display_w - out_w) / 2;
	int32_t out_y = (int32_t)(display_h - out_h) / 2;
	blit_rect(img_offset, fw, fh, out_x, out_y, out_w, out_h);
}

static void blit_rect(uint32_t img_offset, uint32_t fw, uint32_t fh, int32_t out_x, int32_t out_y, uint32_t out_w, uint32_t out_h)
{
	gcmTransferScale scale;
	memset(&scale, 0, sizeof(scale));
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_X8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = 0;
	scale.clipY = 0;
	scale.clipW = display_w;
	scale.clipH = display_h;
	scale.outX = out_x;
	scale.outY = out_y;
	scale.outW = out_w;
	scale.outH = out_h;
	scale.ratioX = rsxGetFixedSint32((float)fw / (float)out_w);
	scale.ratioY = rsxGetFixedSint32((float)fh / (float)out_h);
	scale.inW = fw;
	scale.inH = fh;
	scale.pitch = fw * 4;
	scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = (fw == out_w && fh == out_h) ? GCM_TRANSFER_INTERPOLATOR_NEAREST : GCM_TRANSFER_INTERPOLATOR_LINEAR;
	scale.offset = img_offset;
	scale.inX = 0;
	scale.inY = 0;

	gcmTransferSurface surface;
	memset(&surface, 0, sizeof(surface));
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = color_pitch;
	surface.offset = color_offset[cur_fb];

	rsxSetTransferScaleMode(context, GCM_TRANSFER_LOCAL_TO_LOCAL, GCM_TRANSFER_SURFACE);
	rsxSetTransferScaleSurface(context, &scale, &surface);
}

void video_display_present(void)
{
	// Wait until the previous flip happened. After that the RSX has finished
	// the previous blit, so the frame it read from may be reused by the decoder.
	video_present_stage = "wait_flip";
	if(!first_flip)
		wait_flip();
	else
		gcmResetFlipStatus();
	first_flip = false;
	uint64_t vsync_us = (uint64_t)sysGetSystemTime(); // the previous flip just happened

	// Track the refresh period and missed vsyncs (a flip that took two refreshes).
	if(last_vsync_us)
	{
		double dt = (double)(vsync_us - last_vsync_us);
		if(dt > vsync_period_us * 0.5 && dt < vsync_period_us * 1.5)
			vsync_period_us += (dt - vsync_period_us) * 0.02;
		else if(dt >= vsync_period_us * 1.5 && dt < vsync_period_us * 2.5 && latch_was_active)
		{
			stats.flips_late++;
			latch_good_vsyncs = 0;
			if(latch_margin_us < LATCH_MARGIN_MAX_US)
				latch_margin_us += 1000;
		}
	}
	last_vsync_us = vsync_us;

	cmdbuf_rewind_if_needed();

	latch_was_active = late_latch && !ui_mode;
	if(latch_was_active)
	{
		video_present_stage = "late_latch";
		uint64_t wake = vsync_us + (uint64_t)vsync_period_us - latch_margin_us;
		uint64_t now = (uint64_t)sysGetSystemTime();
		if(wake > now && wake - now < 20000)
			usleep((useconds_t)(wake - now));
		if(++latch_good_vsyncs >= 600) // ~10 s without a missed vsync
		{
			latch_good_vsyncs = 0;
			if(latch_margin_us > LATCH_MARGIN_MIN_US)
				latch_margin_us -= 500;
		}
	}
	stats.latch_margin_us = latch_was_active ? latch_margin_us : 0;
	stats.vsync_hz_x100 = (uint32_t)(100e6 / vsync_period_us);

	video_present_stage = "frame_mutex";
	chiaki_mutex_lock(&frame_mutex);
	int idx = ui_mode ? -1 : fq_present(&frame_queue, NULL);
	stats.frames_presented = frame_queue.presented;
	stats.frames_skipped = frame_queue.skipped;
	stats.frames_repeated = frame_queue.repeated;
	chiaki_mutex_unlock(&frame_mutex);

	video_present_stage = "rsx_commands";
	rsxSetClearColor(context, background);
	rsxSetClearDepthStencil(context, 0xffff);
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	if(ui_mode && ui_front >= 0)
		blit_image(ui_offset[ui_front], UI_IMG_W, UI_IMG_H);
	else if(idx >= 0)
		blit_image(frame_offset[idx], frame_w[idx], frame_h[idx]);
	if(!ui_mode && ovl_on && ovl_front >= 0)
	{
		// sized for a 720p screen, scaled with the display height
		uint32_t ow = VIDEO_OVERLAY_W * display_h / 720, oh = VIDEO_OVERLAY_H * display_h / 720;
		int32_t m = (int32_t)(24 * display_h / 720);
		blit_rect(ovl_offset[ovl_front], VIDEO_OVERLAY_W, VIDEO_OVERLAY_H, m, m, ow, oh);
	}

	video_present_stage = "flip";
	gcmSetFlip(context, cur_fb);
	rsxFlushBuffer(context);
	gcmSetWaitFlip(context);
	cur_fb ^= 1;
	set_render_target(cur_fb);
	video_present_stage = "idle";
}

void video_display_show_ui(const uint32_t *pixels)
{
	// Fill the image that is not on screen; the previous present() waited for
	// its flip, so no blit is reading it any more.
	int back = ui_front == 0 ? 1 : 0;
	memcpy(ui_buf[back], pixels, UI_IMG_W * UI_IMG_H * 4);
	__sync_synchronize();
	ui_front = back;
	ui_mode = true;
}

void video_display_hide_ui(void)
{
	ui_mode = false;
}

void video_display_set_overlay(const uint32_t *pixels)
{
	if(!pixels || !ovl_buf[0] || !ovl_buf[1])
	{
		ovl_on = false;
		return;
	}
	// Called from the display thread between presents: the image not on screen is free.
	int back = ovl_front == 0 ? 1 : 0;
	memcpy(ovl_buf[back], pixels, VIDEO_OVERLAY_W * VIDEO_OVERLAY_H * 4);
	__sync_synchronize();
	ovl_front = back;
	ovl_on = true;
}

void video_set_low_latency_decode(bool enabled)
{
	low_latency_decode = enabled;
	// Tested on hardware (PS5 stream): vdec still held every picture for one
	// frame with the marker, so it stays off. Kept for further experiments.
	aud_enabled = false;
	last_out_seq = au_seq;
	last_sps_desc[0] = 0;
	sps_rewrites = 0;
}

void video_set_late_latch(bool enabled)
{
	late_latch = enabled;
	latch_margin_us = LATCH_MARGIN_START_US;
	latch_good_vsyncs = 0;
}

void video_set_pacing(int pacing)
{
	frame_pacing = (FqPacing)pacing;
	if(frame_mutex_ok)
	{
		chiaki_mutex_lock(&frame_mutex);
		fq_set_pacing(&frame_queue, frame_pacing);
		chiaki_mutex_unlock(&frame_mutex);
	}
}

// ---------------------------------------------------------------------------
// decoder

static bool vdec_module_loaded;
static uint32_t vdec_handle;
static bool vdec_open;
static sys_mem_addr_t vdec_mem;
static ChiakiMutex vdec_mutex;
static ChiakiCond vdec_cond;
static volatile bool au_pending;
static uint8_t *au_buf;
static size_t au_buf_size;

// Called by vdec on its own PPU thread.
static uint32_t vdec_picture_out(void)
{
	uint32_t pic_addr = 0;
	if(vdecGetPicItem(vdec_handle, &pic_addr) != 0 || !pic_addr)
		return 0;

	vdecPicture *pic = (vdecPicture *)(uintptr_t)pic_addr;
	if(pic->access_units > 0)
	{
		uint64_t submitted = au_submit_us[pic->userdata[0] % AU_TIMES];
		uint64_t now = (uint64_t)sysGetSystemTime();
		if(submitted && now >= submitted)
		{
			uint64_t d = now - submitted;
			delay_sum_us += d;
			delay_count++;
			if(d > delay_max_us)
				delay_max_us = d;
		}
		last_out_seq = au_seq;
		uint64_t latest = au_seq - 1;
		if(latest >= pic->userdata[0])
		{
			held_sum += latest - pic->userdata[0];
			held_count++;
		}
	}
	uint32_t w = 0, h = 0;
	if(pic->codec_specific_addr)
	{
		vdecH264Info *info = (vdecH264Info *)(uintptr_t)pic->codec_specific_addr;
		w = info->width;
		h = info->height;
	}

	vdecPictureFormat fmt;
	fmt.format_type = VDEC_PICFMT_ARGB32;
	fmt.color_matrix = VDEC_COLOR_MATRIX_BT709;
	fmt.alpha = 0xff;

	// Like Movian: a non-zero status can still come with a usable picture, so
	// only drop pictures vdec marks as skipped (attr) or that we cannot hold.
	if(pic->status != 0)
		stats.decode_errors++;
	if(pic->attr != VDEC_PICTURE_NORMAL || w == 0 || h == 0 || w > FRAME_MAX_W || h > FRAME_MAX_H)
	{
		vdecGetPicture(vdec_handle, &fmt, NULL); // drop
		stats.frames_dropped++;
		return 0;
	}

	// A buffer that is neither on screen nor waiting in the queue.
	chiaki_mutex_lock(&frame_mutex);
	int idx = fq_acquire(&frame_queue);
	chiaki_mutex_unlock(&frame_mutex);

	vdecGetPicture(vdec_handle, &fmt, frame_buf[idx]);

	chiaki_mutex_lock(&frame_mutex);
	frame_w[idx] = w;
	frame_h[idx] = h;
	fq_commit(&frame_queue, idx);
	chiaki_mutex_unlock(&frame_mutex);

	stats.frames_out++;
	stats.width = w;
	stats.height = h;
	return 0;
}

static uint32_t vdec_callback(uint32_t handle, uint32_t msg_type, uint32_t msg_data, uint32_t arg)
{
	(void)handle; (void)arg;
	switch(msg_type)
	{
		case VDEC_CALLBACK_AUDONE:
			chiaki_mutex_lock(&vdec_mutex);
			au_pending = false;
			chiaki_cond_signal(&vdec_cond);
			chiaki_mutex_unlock(&vdec_mutex);
			break;
		case VDEC_CALLBACK_PICOUT:
			vdec_picture_out();
			break;
		case VDEC_CALLBACK_SEQDONE:
			break;
		case VDEC_CALLBACK_ERROR:
			stats.decode_errors++;
			CHIAKI_LOGW(vlog, "vdec error 0x%x", (unsigned)msg_data);
			break;
	}
	return 0;
}

bool video_decoder_init(void)
{
	if(!vdec_module_loaded)
	{
		if(sysModuleLoad(SYSMODULE_VDEC_H264) != 0)
		{
			CHIAKI_LOGE(vlog, "Could not load the H.264 decoder module");
			return false;
		}
		vdec_module_loaded = true;
	}

	vdecType type;
	type.codec_type = VDEC_CODEC_TYPE_H264;
	type.profile_level = 42; // High profile, level 4.2

	vdecAttr attr;
	memset(&attr, 0, sizeof(attr));
	if(vdecQueryAttr(&type, &attr) != 0)
	{
		CHIAKI_LOGE(vlog, "vdecQueryAttr failed");
		return false;
	}

	size_t alloc = (attr.mem_size + 0xfffff) & ~(size_t)0xfffff;
	if(sysMemoryAllocate(alloc, 0x400 /* 1 MB pages */, &vdec_mem) != 0)
	{
		CHIAKI_LOGE(vlog, "Could not allocate %u bytes for the decoder", (unsigned)attr.mem_size);
		return false;
	}

	chiaki_mutex_init(&vdec_mutex, false);
	chiaki_cond_init(&vdec_cond);

	vdecClosure closure;
	closure.fn = (uint32_t)__get_opd32(vdec_callback);
	closure.arg = 0;

	// Try with as many SPUs as possible; fewer may be free on some setups.
	int r = -1;
	for(int spus = 4; spus >= 1 && r != 0; spus--)
	{
		vdecConfig config;
		memset(&config, 0, sizeof(config));
		config.mem_addr = (uint32_t)vdec_mem;
		config.mem_size = attr.mem_size;
		config.ppu_thread_prio = 400;
		config.ppu_thread_stack_size = 0x10000;
		config.spu_thread_prio = 200;
		config.num_spus = spus;
		r = vdecOpen(&type, &config, &closure, &vdec_handle);
		if(r == 0)
			CHIAKI_LOGI(vlog, "H.264 decoder opened with %d SPUs (%u KB)", spus, (unsigned)(attr.mem_size / 1024));
	}
	if(r != 0)
	{
		CHIAKI_LOGE(vlog, "vdecOpen failed: 0x%x", (unsigned)r);
		sysMemoryFree(vdec_mem);
		chiaki_cond_fini(&vdec_cond);
		chiaki_mutex_fini(&vdec_mutex);
		return false;
	}
	vdecStartSequence(vdec_handle);
	chiaki_mutex_lock(&frame_mutex);
	fq_init(&frame_queue, frame_pacing);
	chiaki_mutex_unlock(&frame_mutex);
	memset(&stats, 0, sizeof(stats));
	vdec_open = true;
	return true;
}

void video_decoder_fini(void)
{
	if(!vdec_open)
		return;
	vdecEndSequence(vdec_handle);
	sysUsleep(50 * 1000); // let SEQDONE and pending pictures drain
	vdecClose(vdec_handle);
	sysMemoryFree(vdec_mem);
	chiaki_cond_fini(&vdec_cond);
	chiaki_mutex_fini(&vdec_mutex);
	free(au_buf);
	au_buf = NULL;
	au_buf_size = 0;
	vdec_open = false;
}

bool video_decoder_decode(const uint8_t *buf, size_t buf_size)
{
	if(!vdec_open)
		return false;
	stats.frames_in++;

	// vdec reads the AU asynchronously (until AUDONE), so keep our own copy
	// (room for SPS rewrites, which add a few bytes).
	size_t need = buf_size + 4 * H264_SPS_REWRITE_GROWTH + sizeof(aud_nal);
	if(need > au_buf_size)
	{
		size_t new_size = (need + 0xffff) & ~(size_t)0xffff;
		uint8_t *nb = memalign(128, new_size);
		if(!nb)
			return false;
		free(au_buf);
		au_buf = nb;
		au_buf_size = new_size;
	}
	char desc[160];
	if(h264_sps_describe(buf, buf_size, desc, sizeof(desc)) && strcmp(desc, last_sps_desc) != 0)
	{
		snprintf(last_sps_desc, sizeof(last_sps_desc), "%s", desc);
		CHIAKI_LOGI(vlog, "H.264 header from the console: %s", desc);
	}

	size_t au_size = 0;
	if(low_latency_decode)
	{
		int n = 0;
		au_size = h264_low_latency_rewrite(buf, buf_size, au_buf, au_buf_size, &n);
		if(n > 0)
		{
			sps_rewrites += (uint64_t)n;
			if(sps_rewrites <= 3)
				CHIAKI_LOGI(vlog, "H.264 header: declared no frame reordering for low-latency decoding");
		}
	}
	if(!au_size)
	{
		memcpy(au_buf, buf, buf_size);
		au_size = buf_size;
	}
	if(aud_enabled)
	{
		if(au_seq - last_out_seq >= AUD_STALL_AUS)
		{
			aud_enabled = false;
			CHIAKI_LOGW(vlog, "End-of-picture marker: no pictures came out, switched off for this session");
		}
		else
		{
			memcpy(au_buf + au_size, aud_nal, sizeof(aud_nal));
			au_size += sizeof(aud_nal);
		}
	}

	vdecAU au;
	memset(&au, 0, sizeof(au));
	au.packet_addr = (uint32_t)(uintptr_t)au_buf;
	au.packet_size = (uint32_t)au_size;
	au.pts.low = au.pts.hi = VDEC_TS_INVALID;
	au.dts.low = au.dts.hi = VDEC_TS_INVALID;
	au.userdata = au_seq;
	au_submit_us[au_seq % AU_TIMES] = (uint64_t)sysGetSystemTime();
	au_seq++;

	chiaki_mutex_lock(&vdec_mutex);
	au_pending = true;
	int r;
	for(int tries = 0; ; tries++)
	{
		r = vdecDecodeAu(vdec_handle, VDEC_DECODER_MODE_NORMAL, &au);
		if(r != (int)VDEC_ERROR_BUSY || tries >= 50)
			break;
		chiaki_mutex_unlock(&vdec_mutex);
		sysUsleep(1000);
		chiaki_mutex_lock(&vdec_mutex);
	}
	if(r != 0)
	{
		au_pending = false;
		chiaki_mutex_unlock(&vdec_mutex);
		stats.decode_errors++;
		CHIAKI_LOGW(vlog, "vdecDecodeAu failed: 0x%x", (unsigned)r);
		return false;
	}
	while(au_pending)
	{
		if(chiaki_cond_timedwait(&vdec_cond, &vdec_mutex, 1000) == CHIAKI_ERR_TIMEOUT)
		{
			au_pending = false;
			chiaki_mutex_unlock(&vdec_mutex);
			CHIAKI_LOGE(vlog, "vdec did not consume an access unit within 1 s");
			return false;
		}
	}
	chiaki_mutex_unlock(&vdec_mutex);
	return true;
}

void video_peek_stats(VideoStats *out)
{
	*out = stats;
	uint64_t n = delay_count, sum = delay_sum_us; // written by the vdec thread; a rough value is fine here
	out->decode_ms_avg = n ? (uint32_t)(sum / n / 1000) : 0;
	out->decode_ms_max = (uint32_t)(delay_max_us / 1000);
}

void video_get_stats(VideoStats *out)
{
	*out = stats;
	out->decode_ms_avg = delay_count ? (uint32_t)(delay_sum_us / delay_count / 1000) : 0;
	out->decode_ms_max = (uint32_t)(delay_max_us / 1000);
	out->eop_marker = aud_enabled;
	out->decode_held_x100 = held_count ? (uint32_t)(held_sum * 100 / held_count) : 0;
	delay_sum_us = delay_count = delay_max_us = 0;
	held_sum = held_count = 0;
}
