#ifndef VSCREEN_ENCODER_H
#define VSCREEN_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include "screencast.h"

struct encoder;

typedef int (*encoder_output_fn)(void *user, const uint8_t *data, int size,
				 bool keyframe, int64_t pts_us);

/*
 * name: "auto" or an FFmpeg encoder name (h264_vaapi, h264_nvenc, libx264).
 * device: VAAPI render node, e.g. /dev/dri/renderD128.
 */
struct encoder *encoder_open(const char *name, const char *device, int width, int height,
			     int fps, int64_t bitrate);

/* Encode one frame; every produced packet is passed to out(). */
int encoder_encode(struct encoder *e, const struct frame *f, int64_t pts_us,
		   bool force_keyframe, encoder_output_fn out, void *user);

const char *encoder_name(const struct encoder *e);

void encoder_close(struct encoder *e);

#endif
