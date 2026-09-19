/*
 * Low-latency H.264 encoding through FFmpeg.
 *
 * Preference order with "auto": VAAPI (Intel/AMD GPU), NVENC (NVIDIA),
 * then libx264 on the CPU. No B-frames, no lookahead: every input frame
 * produces its packet immediately.
 */
#include "encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

struct encoder {
	const char *name;
	AVCodecContext *ctx;
	AVBufferRef *hw_device;
	AVBufferRef *hw_frames;
	AVFrame *sw_frame;
	AVPacket *pkt;
	struct SwsContext *sws;
	enum AVPixelFormat sws_src;
	int sws_stride;
	int64_t last_pts;
};

static void set_opt(AVCodecContext *ctx, const char *key, const char *val)
{
	/* Options differ between encoders and FFmpeg versions; ignore misses. */
	av_opt_set(ctx->priv_data, key, val, 0);
}

static void encoder_free(struct encoder *e)
{
	if (!e)
		return;
	avcodec_free_context(&e->ctx);
	av_buffer_unref(&e->hw_frames);
	av_buffer_unref(&e->hw_device);
	av_frame_free(&e->sw_frame);
	av_packet_free(&e->pkt);
	sws_freeContext(e->sws);
	free(e);
}

static struct encoder *try_open(const char *name, bool low_power, const char *device,
				int width, int height, int fps, int64_t bitrate)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return NULL;

	struct encoder *e = calloc(1, sizeof(*e));
	if (!e)
		return NULL;
	bool vaapi = strstr(name, "vaapi") != NULL;
	enum AVPixelFormat sw_fmt = strcmp(name, "libx264") == 0 ? AV_PIX_FMT_YUV420P
								 : AV_PIX_FMT_NV12;
	int ret;

	e->name = name;
	e->last_pts = -1;
	e->ctx = avcodec_alloc_context3(codec);
	e->pkt = av_packet_alloc();
	e->sw_frame = av_frame_alloc();
	if (!e->ctx || !e->pkt || !e->sw_frame)
		goto fail;

	AVCodecContext *ctx = e->ctx;
	ctx->width = width;
	ctx->height = height;
	ctx->time_base = (AVRational){1, 1000000};
	ctx->framerate = (AVRational){fps, 1};
	ctx->gop_size = fps * 10;
	ctx->max_b_frames = 0;
	ctx->bit_rate = bitrate;
	ctx->rc_max_rate = bitrate * 3 / 2;
	ctx->rc_buffer_size = (int)(bitrate / 2);
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->color_range = AVCOL_RANGE_MPEG;
	ctx->colorspace = AVCOL_SPC_BT709;
	ctx->color_primaries = AVCOL_PRI_BT709;
	ctx->color_trc = AVCOL_TRC_BT709;
	ctx->pix_fmt = sw_fmt;

	if (vaapi) {
		ret = av_hwdevice_ctx_create(&e->hw_device, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0);
		if (ret < 0)
			goto fail;
		e->hw_frames = av_hwframe_ctx_alloc(e->hw_device);
		if (!e->hw_frames)
			goto fail;
		AVHWFramesContext *fc = (AVHWFramesContext *)e->hw_frames->data;
		fc->format = AV_PIX_FMT_VAAPI;
		fc->sw_format = sw_fmt;
		fc->width = width;
		fc->height = height;
		fc->initial_pool_size = 8;
		if (av_hwframe_ctx_init(e->hw_frames) < 0)
			goto fail;
		ctx->pix_fmt = AV_PIX_FMT_VAAPI;
		ctx->hw_frames_ctx = av_buffer_ref(e->hw_frames);
		set_opt(ctx, "async_depth", "1");
		set_opt(ctx, "low_power", low_power ? "1" : "0");
	} else if (strstr(name, "nvenc")) {
		set_opt(ctx, "preset", "p1");
		set_opt(ctx, "tune", "ull");
		set_opt(ctx, "zerolatency", "1");
		set_opt(ctx, "delay", "0");
		set_opt(ctx, "rc", "vbr");
	} else {
		set_opt(ctx, "preset", "superfast");
		set_opt(ctx, "tune", "zerolatency");
	}
	set_opt(ctx, "forced-idr", "1");
	set_opt(ctx, "profile", "high");

	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
		goto fail;

	e->sw_frame->format = sw_fmt;
	e->sw_frame->width = width;
	e->sw_frame->height = height;
	if (av_frame_get_buffer(e->sw_frame, 0) < 0)
		goto fail;
	return e;

fail:
	encoder_free(e);
	return NULL;
}

struct encoder *encoder_open(const char *name, const char *device, int width, int height,
			     int fps, int64_t bitrate)
{
	static const struct { const char *name; bool low_power; } candidates[] = {
		{"h264_vaapi", false},
		{"h264_vaapi", true},
		{"h264_nvenc", false},
		{"libx264", false},
	};
	bool any = strcmp(name, "auto") == 0;

	av_log_set_level(AV_LOG_ERROR);
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (!any && strcmp(name, candidates[i].name) != 0)
			continue;
		struct encoder *e = try_open(candidates[i].name, candidates[i].low_power, device,
					     width, height, fps, bitrate);
		if (e)
			return e;
		fprintf(stderr, "encoder: %s%s unavailable\n", candidates[i].name,
			candidates[i].low_power ? " (low power)" : "");
	}
	if (!any) {
		struct encoder *e = try_open(name, false, device, width, height, fps, bitrate);
		if (e)
			return e;
	}
	fprintf(stderr, "encoder: no usable H.264 encoder\n");
	return NULL;
}

const char *encoder_name(const struct encoder *e)
{
	return e->name;
}

static enum AVPixelFormat av_format(enum frame_format f)
{
	switch (f) {
	case FRAME_BGRA: return AV_PIX_FMT_BGRA;
	case FRAME_RGBX: return AV_PIX_FMT_RGB0;
	case FRAME_RGBA: return AV_PIX_FMT_RGBA;
	default: return AV_PIX_FMT_BGR0;
	}
}

int encoder_encode(struct encoder *e, const struct frame *f, int64_t pts_us,
		   bool force_keyframe, encoder_output_fn out, void *user)
{
	AVCodecContext *ctx = e->ctx;
	enum AVPixelFormat src = av_format(f->format);
	int ret;

	if (f->width != ctx->width || f->height != ctx->height) {
		fprintf(stderr, "encoder: frame is %dx%d, expected %dx%d\n", f->width, f->height,
			ctx->width, ctx->height);
		return -1;
	}

	if (!e->sws || e->sws_src != src) {
		sws_freeContext(e->sws);
		e->sws = sws_getContext(f->width, f->height, src, ctx->width, ctx->height,
					e->sw_frame->format, SWS_BILINEAR, NULL, NULL, NULL);
		if (!e->sws)
			return -1;
		const int *bt709 = sws_getCoefficients(SWS_CS_ITU709);
		sws_setColorspaceDetails(e->sws, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
		e->sws_src = src;
	}

	if (av_frame_make_writable(e->sw_frame) < 0)
		return -1;
	const uint8_t *src_data[4] = {f->data};
	int src_stride[4] = {f->stride};
	sws_scale(e->sws, src_data, src_stride, 0, f->height, e->sw_frame->data,
		  e->sw_frame->linesize);

	AVFrame *input = e->sw_frame;
	AVFrame *hw = NULL;
	if (e->hw_frames) {
		hw = av_frame_alloc();
		if (!hw || av_hwframe_get_buffer(e->hw_frames, hw, 0) < 0 ||
		    av_hwframe_transfer_data(hw, e->sw_frame, 0) < 0) {
			av_frame_free(&hw);
			return -1;
		}
		input = hw;
	}

	/* Encoders require strictly increasing timestamps. */
	if (pts_us <= e->last_pts)
		pts_us = e->last_pts + 1;
	e->last_pts = pts_us;
	input->pts = pts_us;
	input->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

	ret = avcodec_send_frame(ctx, input);
	av_frame_free(&hw);
	if (ret < 0)
		return ret;

	while ((ret = avcodec_receive_packet(ctx, e->pkt)) == 0) {
		int r = out(user, e->pkt->data, e->pkt->size,
			    (e->pkt->flags & AV_PKT_FLAG_KEY) != 0, e->pkt->pts);
		av_packet_unref(e->pkt);
		if (r < 0)
			return r;
	}
	return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

void encoder_close(struct encoder *e)
{
	encoder_free(e);
}
