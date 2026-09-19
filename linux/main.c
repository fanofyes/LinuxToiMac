/*
 * vscreen-send: turn a Mac on the network into an extra GNOME monitor.
 *
 *   connect to VScreen.app -> create virtual monitor -> capture -> H.264 -> TCP
 *
 * The virtual monitor only exists while the receiver is connected, so
 * windows never get stranded on an invisible screen.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <linux/sockios.h>
#include <pipewire/pipewire.h>

#include "encoder.h"
#include "protocol.h"
#include "screencast.h"

struct options {
	const char *host;
	const char *port;
	int width, height, fps;
	int64_t bitrate;
	const char *encoder;
	const char *device;
};

struct session {
	int fd;
	uint8_t inbuf[VS_HDR_LEN];
	size_t inlen;
	bool want_keyframe;
	/* stats */
	int64_t stats_start;
	int frames;
	int64_t bytes;
	int64_t encode_us;
};

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put_u32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put_u64(uint8_t *p, uint64_t v) { put_u32(p, v >> 32); put_u32(p + 4, (uint32_t)v); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int send_msg(int fd, uint8_t type, uint8_t flags, int64_t pts_us,
		    const uint8_t *payload, size_t len)
{
	uint8_t hdr[VS_HDR_LEN] = {0};
	put_u32(hdr, VS_MAGIC);
	hdr[4] = type;
	hdr[5] = flags;
	put_u32(hdr + 8, (uint32_t)len);
	put_u64(hdr + 12, (uint64_t)pts_us);

	struct iovec iov[2] = {{hdr, sizeof(hdr)}, {(void *)payload, len}};
	int iovcnt = len ? 2 : 1;
	struct msghdr msg = {.msg_iov = iov, .msg_iovlen = iovcnt};

	while (iovcnt > 0) {
		ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		while (n > 0 && msg.msg_iovlen > 0) {
			if ((size_t)n >= msg.msg_iov->iov_len) {
				n -= msg.msg_iov->iov_len;
				msg.msg_iov++;
				msg.msg_iovlen--;
			} else {
				msg.msg_iov->iov_base = (uint8_t *)msg.msg_iov->iov_base + n;
				msg.msg_iov->iov_len -= n;
				n = 0;
			}
		}
		iovcnt = msg.msg_iovlen;
	}
	return 0;
}

static int connect_to(const struct options *o)
{
	struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res, *ai;
	int err = getaddrinfo(o->host, o->port, &hints, &res);
	if (err) {
		fprintf(stderr, "resolve %s: %s\n", o->host, gai_strerror(err));
		return -1;
	}
	int fd = -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd >= 0) {
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}
	return fd;
}

/* Read receiver -> sender messages. Returns -1 when the connection is gone. */
static int read_control(struct session *s)
{
	struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
	while (poll(&pfd, 1, 0) > 0) {
		if (pfd.revents & (POLLERR | POLLHUP))
			return -1;
		ssize_t n = recv(s->fd, s->inbuf + s->inlen, sizeof(s->inbuf) - s->inlen,
				 MSG_DONTWAIT);
		if (n == 0)
			return -1;
		if (n < 0)
			return errno == EAGAIN || errno == EINTR ? 0 : -1;
		s->inlen += n;
		if (s->inlen < VS_HDR_LEN)
			continue;
		s->inlen = 0;
		if (get_u32(s->inbuf) != VS_MAGIC || get_u32(s->inbuf + 8) != 0)
			return -1;
		if (s->inbuf[4] == VS_MSG_KEYFRAME_REQ)
			s->want_keyframe = true;
	}
	return 0;
}

static int on_packet(void *user, const uint8_t *data, int size, bool keyframe, int64_t pts_us)
{
	struct session *s = user;
	s->bytes += size;
	return send_msg(s->fd, VS_MSG_VIDEO, keyframe ? VS_FLAG_KEYFRAME : 0, pts_us, data, size);
}

static void print_stats(struct session *s, const char *encoder)
{
	int64_t now = now_us();
	int64_t span = now - s->stats_start;
	if (span < 5000000)
		return;
	if (s->frames > 0)
		fprintf(stderr, "%s: %.1f fps, %.1f Mbit/s, encode %.1f ms/frame\n", encoder,
			s->frames * 1e6 / span, s->bytes * 8.0 / span, s->encode_us / 1000.0 / s->frames);
	s->stats_start = now;
	s->frames = 0;
	s->bytes = 0;
	s->encode_us = 0;
}

/* Returns 0 on disconnect (retry), -1 on a fatal local error. */
static int run_session(const struct options *o, int fd)
{
	struct session s = {.fd = fd, .want_keyframe = true, .stats_start = now_us()};
	int result = 0;

	struct encoder *enc = encoder_open(o->encoder, o->device, o->width, o->height, o->fps,
					   o->bitrate);
	if (!enc)
		return -1;

	uint8_t hello[12];
	put_u16(hello, VS_VERSION);
	put_u16(hello + 2, VS_CODEC_H264);
	put_u32(hello + 4, o->width);
	put_u32(hello + 8, o->height);
	if (send_msg(fd, VS_MSG_HELLO, 0, 0, hello, sizeof(hello)) < 0) {
		encoder_close(enc);
		return 0;
	}

	struct screencast *sc = screencast_start(o->width, o->height, o->fps);
	if (!sc) {
		encoder_close(enc);
		return -1;
	}
	fprintf(stderr, "virtual monitor %dx%d active, encoding with %s\n", o->width, o->height,
		encoder_name(enc));

	/* Don't queue more than ~1/30 s of video in the kernel. */
	int congestion_limit = (int)(o->bitrate / 8 / 30);
	if (congestion_limit < 128 * 1024)
		congestion_limit = 128 * 1024;

	while (!stop) {
		if (!screencast_alive(sc)) {
			fprintf(stderr, "screen sharing ended by the compositor\n");
			result = -1;
			break;
		}
		if (read_control(&s) < 0) {
			fprintf(stderr, "receiver disconnected\n");
			break;
		}

		int queued = 0;
		if (ioctl(fd, SIOCOUTQ, &queued) == 0 && queued > congestion_limit) {
			/* Leave the newest frame waiting in the capture mailbox. */
			usleep(2000);
			continue;
		}

		const struct frame *f = screencast_next_frame(sc, 20);
		if (!f && s.want_keyframe)
			f = screencast_last_frame(sc); /* static screen: resend it */
		if (!f)
			continue;

		int64_t t0 = now_us();
		if (encoder_encode(enc, f, f->time_us, s.want_keyframe, on_packet, &s) < 0) {
			if (errno == EPIPE || errno == ECONNRESET) {
				fprintf(stderr, "receiver disconnected\n");
				break;
			}
			fprintf(stderr, "encode/send failed\n");
			break;
		}
		s.want_keyframe = false;
		s.encode_us += now_us() - t0;
		s.frames++;
		print_stats(&s, encoder_name(enc));
	}

	screencast_stop(sc);
	encoder_close(enc);
	return result;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [options] RECEIVER_HOST\n"
		"  -p PORT       receiver port (default %d)\n"
		"  -s WxH        virtual monitor size (default 2560x1440)\n"
		"  -r FPS        max frame rate (default 60)\n"
		"  -b MBIT       bitrate in Mbit/s (default 40)\n"
		"  -e ENCODER    auto, h264_vaapi, h264_nvenc, libx264 (default auto)\n"
		"  -d DEVICE     VAAPI device (default /dev/dri/renderD128)\n",
		argv0, VS_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
	static char default_port[8];
	snprintf(default_port, sizeof(default_port), "%d", VS_DEFAULT_PORT);
	struct options o = {
		.port = default_port,
		.width = 2560,
		.height = 1440,
		.fps = 60,
		.bitrate = 40 * 1000 * 1000,
		.encoder = "auto",
		.device = "/dev/dri/renderD128",
	};
	int c;

	while ((c = getopt(argc, argv, "p:s:r:b:e:d:h")) != -1) {
		switch (c) {
		case 'p': o.port = optarg; break;
		case 's':
			if (sscanf(optarg, "%dx%d", &o.width, &o.height) != 2 ||
			    o.width < 320 || o.height < 200 || o.width % 2 || o.height % 2) {
				fprintf(stderr, "bad size %s (use even WxH)\n", optarg);
				return 2;
			}
			break;
		case 'r': o.fps = atoi(optarg); break;
		case 'b': o.bitrate = (int64_t)(atof(optarg) * 1000 * 1000); break;
		case 'e': o.encoder = optarg; break;
		case 'd': o.device = optarg; break;
		default: usage(argv[0]); return c == 'h' ? 0 : 2;
		}
	}
	if (optind != argc - 1 || o.fps <= 0 || o.bitrate <= 0) {
		usage(argv[0]);
		return 2;
	}
	o.host = argv[optind];

	struct sigaction sa = {.sa_handler = on_signal};
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	pw_init(&argc, &argv);

	bool announced = false;
	int ret = 0;
	while (!stop) {
		int fd = connect_to(&o);
		if (fd < 0) {
			if (!announced)
				fprintf(stderr, "waiting for VScreen on %s:%s ...\n", o.host, o.port);
			announced = true;
			sleep(2);
			continue;
		}
		announced = false;
		fprintf(stderr, "connected to %s:%s\n", o.host, o.port);
		int r = run_session(&o, fd);
		close(fd);
		if (r < 0) {
			ret = 1;
			break;
		}
		if (!stop)
			sleep(1);
	}

	pw_deinit();
	return ret;
}
