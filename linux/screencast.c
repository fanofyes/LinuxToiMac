/*
 * Virtual monitor capture on GNOME/Wayland.
 *
 * Mutter exposes org.gnome.Mutter.ScreenCast on the session bus. A session's
 * RecordVirtual() call creates a brand new monitor (it shows up in
 * Settings -> Displays) whose size is decided by the PipeWire format we
 * negotiate. Frames arrive as shared-memory buffers on the PipeWire thread;
 * we copy the newest one into a mailbox that the encoder thread drains, so a
 * slow encoder skips frames instead of building up latency.
 */
#define _GNU_SOURCE
#include "screencast.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <gio/gio.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#define MUTTER_BUS "org.gnome.Mutter.ScreenCast"
#define MUTTER_PATH "/org/gnome/Mutter/ScreenCast"
#define MUTTER_IFACE "org.gnome.Mutter.ScreenCast"
#define SESSION_IFACE "org.gnome.Mutter.ScreenCast.Session"
#define STREAM_IFACE "org.gnome.Mutter.ScreenCast.Stream"

#define CURSOR_MODE_EMBEDDED 1

struct screencast {
	int width, height, fps;

	GDBusConnection *bus;
	char *session_path;
	char *stream_path;
	guint sub_stream_added, sub_closed;
	uint32_t node_id;
	bool have_node;
	bool closed;

	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info_raw format;
	bool format_ok;

	/*
	 * Triple buffer: `writing` belongs to the PipeWire thread, `reading`
	 * to the encoder thread, `latest` is swapped between them under lock.
	 */
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct frame bufs[3];
	struct frame *writing, *latest, *reading;
	bool latest_fresh;
	bool have_reading;
	bool failed;
};

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void set_failed(struct screencast *sc, const char *why)
{
	fprintf(stderr, "capture: %s\n", why);
	pthread_mutex_lock(&sc->lock);
	sc->failed = true;
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->lock);
}

/* ---- PipeWire ---------------------------------------------------------- */

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct screencast *sc = data;
	(void)seq;
	fprintf(stderr, "pipewire: error id:%u %s (%s)\n", id, message, spa_strerror(res));
	if (id == PW_ID_CORE)
		set_failed(sc, "lost connection to PipeWire");
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_state_changed(void *data, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	struct screencast *sc = data;
	(void)old;
	fprintf(stderr, "pipewire: stream %s%s%s\n", pw_stream_state_as_string(state),
		error ? ": " : "", error ? error : "");
	if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
		set_failed(sc, "stream stopped");
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct screencast *sc = data;
	uint32_t media_type, media_subtype;

	if (param == NULL || id != SPA_PARAM_Format)
		return;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
	    media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;
	if (spa_format_video_raw_parse(param, &sc->format) < 0)
		return;

	switch (sc->format.format) {
	case SPA_VIDEO_FORMAT_BGRx:
	case SPA_VIDEO_FORMAT_BGRA:
	case SPA_VIDEO_FORMAT_RGBx:
	case SPA_VIDEO_FORMAT_RGBA:
		break;
	default:
		set_failed(sc, "compositor picked an unsupported pixel format");
		return;
	}
	sc->format_ok = true;
	fprintf(stderr, "pipewire: negotiated %ux%u\n", sc->format.size.width,
		sc->format.size.height);

	/* Ask for mappable memory (no DMA-BUF) and the header meta. */
	uint8_t buffer[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType,
		SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(sc->stream, params, 2);
}

static enum frame_format frame_format_from_spa(enum spa_video_format f)
{
	switch (f) {
	case SPA_VIDEO_FORMAT_BGRA: return FRAME_BGRA;
	case SPA_VIDEO_FORMAT_RGBx: return FRAME_RGBX;
	case SPA_VIDEO_FORMAT_RGBA: return FRAME_RGBA;
	default: return FRAME_BGRX;
	}
}

static void on_process(void *data)
{
	struct screencast *sc = data;
	struct pw_buffer *b = NULL, *next;

	/* Only the newest buffer matters. */
	while ((next = pw_stream_dequeue_buffer(sc->stream)) != NULL) {
		if (b)
			pw_stream_queue_buffer(sc->stream, b);
		b = next;
	}
	if (b == NULL)
		return;

	struct spa_buffer *buf = b->buffer;
	struct spa_data *d = &buf->datas[0];
	struct spa_meta_header *h =
		spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h));

	if (!sc->format_ok || d->data == NULL || d->chunk->size == 0)
		goto done;
	if (h && (h->flags & SPA_META_HEADER_FLAG_CORRUPTED))
		goto done;

	int w = sc->format.size.width;
	int hgt = sc->format.size.height;
	int stride = d->chunk->stride > 0 ? d->chunk->stride : w * 4;
	size_t need = (size_t)stride * hgt;
	if ((size_t)d->chunk->offset + need > d->maxsize)
		goto done;

	struct frame *f = sc->writing;
	if (f->cap < need) {
		free(f->data);
		f->data = malloc(need);
		f->cap = f->data ? need : 0;
		if (!f->data)
			goto done;
	}
	memcpy(f->data, (uint8_t *)d->data + d->chunk->offset, need);
	f->width = w;
	f->height = hgt;
	f->stride = stride;
	f->format = frame_format_from_spa(sc->format.format);
	f->time_us = now_us();

	pthread_mutex_lock(&sc->lock);
	sc->writing = sc->latest;
	sc->latest = f;
	sc->latest_fresh = true;
	pthread_cond_signal(&sc->cond);
	pthread_mutex_unlock(&sc->lock);

done:
	pw_stream_queue_buffer(sc->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static int start_pipewire(struct screencast *sc)
{
	sc->loop = pw_thread_loop_new("vscreen-capture", NULL);
	if (!sc->loop)
		return -1;
	sc->context = pw_context_new(pw_thread_loop_get_loop(sc->loop), NULL, 0);
	if (!sc->context)
		return -1;
	if (pw_thread_loop_start(sc->loop) < 0)
		return -1;

	pw_thread_loop_lock(sc->loop);

	sc->core = pw_context_connect(sc->context, NULL, 0);
	if (!sc->core) {
		pw_thread_loop_unlock(sc->loop);
		fprintf(stderr, "pipewire: cannot connect: %s\n", strerror(errno));
		return -1;
	}
	pw_core_add_listener(sc->core, &sc->core_listener, &core_events, sc);

	sc->stream = pw_stream_new(sc->core, "vscreen",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Screen", NULL));
	if (!sc->stream) {
		pw_thread_loop_unlock(sc->loop);
		return -1;
	}
	pw_stream_add_listener(sc->stream, &sc->stream_listener, &stream_events, sc);

	/*
	 * A fixed size is what makes Mutter create the virtual monitor with
	 * that resolution. Variable frame rate: Mutter only sends on damage.
	 */
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
			SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
			SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(sc->width, sc->height)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
		SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(sc->fps, 1), &SPA_FRACTION(1, 1), &SPA_FRACTION(sc->fps, 1)));

	int res = pw_stream_connect(sc->stream, PW_DIRECTION_INPUT, sc->node_id,
				    PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
				    params, 1);
	pw_thread_loop_unlock(sc->loop);
	if (res < 0) {
		fprintf(stderr, "pipewire: connect failed: %s\n", spa_strerror(res));
		return -1;
	}
	return 0;
}

/* ---- Mutter D-Bus ------------------------------------------------------- */

static void on_stream_added(GDBusConnection *c, const char *sender, const char *path,
			    const char *iface, const char *signal, GVariant *params,
			    gpointer data)
{
	struct screencast *sc = data;
	(void)c, (void)sender, (void)path, (void)iface, (void)signal;
	g_variant_get(params, "(u)", &sc->node_id);
	sc->have_node = true;
}

static void on_session_closed(GDBusConnection *c, const char *sender, const char *path,
			      const char *iface, const char *signal, GVariant *params,
			      gpointer data)
{
	struct screencast *sc = data;
	(void)c, (void)sender, (void)path, (void)iface, (void)signal, (void)params;
	sc->closed = true;
}

static GVariant *call(struct screencast *sc, const char *path, const char *iface,
		      const char *method, GVariant *args, const char *reply_type)
{
	GError *err = NULL;
	GVariant *ret = g_dbus_connection_call_sync(sc->bus, MUTTER_BUS, path, iface, method,
		args, reply_type ? G_VARIANT_TYPE(reply_type) : NULL,
		G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
	if (!ret) {
		fprintf(stderr, "mutter: %s failed: %s\n", method, err->message);
		g_error_free(err);
	}
	return ret;
}

static void pump_dbus(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
}

static int start_mutter(struct screencast *sc)
{
	GError *err = NULL;
	GVariant *ret;
	GVariantBuilder props;

	sc->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!sc->bus) {
		fprintf(stderr, "dbus: %s\n", err->message);
		g_error_free(err);
		return -1;
	}

	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	ret = call(sc, MUTTER_PATH, MUTTER_IFACE, "CreateSession",
		   g_variant_new("(a{sv})", &props), "(o)");
	if (!ret) {
		fprintf(stderr, "mutter: is this a GNOME Wayland session?\n");
		return -1;
	}
	g_variant_get(ret, "(o)", &sc->session_path);
	g_variant_unref(ret);

	sc->sub_closed = g_dbus_connection_signal_subscribe(sc->bus, NULL, SESSION_IFACE,
		"Closed", sc->session_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_session_closed, sc, NULL);

	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&props, "{sv}", "cursor-mode",
			      g_variant_new_uint32(CURSOR_MODE_EMBEDDED));
	g_variant_builder_add(&props, "{sv}", "is-platform", g_variant_new_boolean(TRUE));
	ret = call(sc, sc->session_path, SESSION_IFACE, "RecordVirtual",
		   g_variant_new("(a{sv})", &props), "(o)");
	if (!ret)
		return -1;
	g_variant_get(ret, "(o)", &sc->stream_path);
	g_variant_unref(ret);

	sc->sub_stream_added = g_dbus_connection_signal_subscribe(sc->bus, NULL, STREAM_IFACE,
		"PipeWireStreamAdded", sc->stream_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
		on_stream_added, sc, NULL);

	ret = call(sc, sc->session_path, SESSION_IFACE, "Start", NULL, NULL);
	if (!ret)
		return -1;
	g_variant_unref(ret);

	int64_t deadline = now_us() + 5000000;
	while (!sc->have_node && !sc->closed && now_us() < deadline) {
		pump_dbus();
		g_usleep(5000);
	}
	if (!sc->have_node) {
		fprintf(stderr, "mutter: no PipeWire stream appeared\n");
		return -1;
	}
	return 0;
}

/* ---- public ------------------------------------------------------------- */

struct screencast *screencast_start(int width, int height, int fps)
{
	struct screencast *sc = calloc(1, sizeof(*sc));
	if (!sc)
		return NULL;
	sc->width = width;
	sc->height = height;
	sc->fps = fps;
	pthread_mutex_init(&sc->lock, NULL);
	pthread_cond_init(&sc->cond, NULL);
	sc->writing = &sc->bufs[0];
	sc->latest = &sc->bufs[1];
	sc->reading = &sc->bufs[2];

	if (start_mutter(sc) < 0 || start_pipewire(sc) < 0) {
		screencast_stop(sc);
		return NULL;
	}
	return sc;
}

bool screencast_alive(struct screencast *sc)
{
	pump_dbus();
	pthread_mutex_lock(&sc->lock);
	bool failed = sc->failed;
	pthread_mutex_unlock(&sc->lock);
	return !failed && !sc->closed;
}

const struct frame *screencast_next_frame(struct screencast *sc, int timeout_ms)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += (long)timeout_ms * 1000000;
	deadline.tv_sec += deadline.tv_nsec / 1000000000;
	deadline.tv_nsec %= 1000000000;

	const struct frame *out = NULL;
	pthread_mutex_lock(&sc->lock);
	while (!sc->latest_fresh && !sc->failed) {
		if (pthread_cond_timedwait(&sc->cond, &sc->lock, &deadline) == ETIMEDOUT)
			break;
	}
	if (sc->latest_fresh) {
		struct frame *tmp = sc->reading;
		sc->reading = sc->latest;
		sc->latest = tmp;
		sc->latest_fresh = false;
		sc->have_reading = true;
		out = sc->reading;
	}
	pthread_mutex_unlock(&sc->lock);
	return out;
}

const struct frame *screencast_last_frame(struct screencast *sc)
{
	return sc->have_reading ? sc->reading : NULL;
}

void screencast_stop(struct screencast *sc)
{
	if (!sc)
		return;

	if (sc->loop)
		pw_thread_loop_stop(sc->loop);
	if (sc->stream) {
		spa_hook_remove(&sc->stream_listener);
		pw_stream_destroy(sc->stream);
	}
	if (sc->core)
		pw_core_disconnect(sc->core);
	if (sc->context)
		pw_context_destroy(sc->context);
	if (sc->loop)
		pw_thread_loop_destroy(sc->loop);

	if (sc->bus) {
		if (sc->session_path && !sc->closed) {
			GVariant *ret = call(sc, sc->session_path, SESSION_IFACE, "Stop", NULL, NULL);
			if (ret)
				g_variant_unref(ret);
		}
		if (sc->sub_stream_added)
			g_dbus_connection_signal_unsubscribe(sc->bus, sc->sub_stream_added);
		if (sc->sub_closed)
			g_dbus_connection_signal_unsubscribe(sc->bus, sc->sub_closed);
		pump_dbus();
		g_object_unref(sc->bus);
	}
	g_free(sc->session_path);
	g_free(sc->stream_path);

	for (int i = 0; i < 3; i++)
		free(sc->bufs[i].data);
	pthread_mutex_destroy(&sc->lock);
	pthread_cond_destroy(&sc->cond);
	free(sc);
}
