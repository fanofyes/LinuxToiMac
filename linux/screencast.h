#ifndef VSCREEN_SCREENCAST_H
#define VSCREEN_SCREENCAST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pixel layouts PipeWire may hand us (all 4 bytes per pixel). */
enum frame_format { FRAME_BGRX, FRAME_BGRA, FRAME_RGBX, FRAME_RGBA };

struct frame {
	uint8_t *data;
	size_t cap;
	int width, height, stride;
	enum frame_format format;
	int64_t time_us;
};

struct screencast;

/*
 * Ask GNOME Mutter for a new virtual monitor of the given size and start
 * receiving its contents over PipeWire. The monitor disappears when
 * screencast_stop() is called or the process exits.
 */
struct screencast *screencast_start(int width, int height, int fps);

/* Dispatch D-Bus events; false once the compositor or PipeWire gave up. */
bool screencast_alive(struct screencast *sc);

/*
 * Wait up to timeout_ms for a frame newer than the last one returned.
 * Returns the frame (valid until the next call) or NULL on timeout.
 */
const struct frame *screencast_next_frame(struct screencast *sc, int timeout_ms);

/* The most recently returned frame, or NULL if none yet. */
const struct frame *screencast_last_frame(struct screencast *sc);

void screencast_stop(struct screencast *sc);

#endif
