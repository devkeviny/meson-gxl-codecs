// SPDX-License-Identifier: GPL-2.0
/*
 * venc-stage - staged H.264 encode test for the GXL HCodec port.
 *
 * Same ioctls as venc.c, but one stage at a time, each logged to
 * /root/enc-test.log (fsync'd BEFORE it runs) so a hard crash leaves
 * the last attempted stage on disk. Sleeps 1s between stages.
 *
 *   venc-stage -s 1280x720 [-q QP] < in.nv12 > out.h264
 *
 * Stages: 0 open, 1 CONFIG_INIT (poweron + firmware load),
 * 2 GET_BUFFINFO + mmap, 3 SEQUENCE (SPS/PPS), 4 IDR frame,
 * 5 NON_IDR frames (5x), 6 done.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOC(n)			_IOW('E', n, uint32_t)
#define IOC_NEW_CMD		IOC(0x02)
#define IOC_GET_STAGE		IOC(0x03)
#define IOC_GET_OUTPUT_SIZE	IOC(0x04)
#define IOC_CONFIG_INIT		IOC(0x05)
#define IOC_GET_BUFFINFO	IOC(0x08)
#define IOC_SUBMIT		IOC(0x09)

enum { SEQUENCE = 1, IDR = 3, NON_IDR = 4, PICTURE_DONE = 8, IDR_DONE = 9, NON_IDR_DONE = 10 };
enum { FMT_NV12 = 3 };
#define LOCAL_BUFF	0
#define FLUSH_INPUT	0x1
#define FLUSH_OUTPUT	0x2
#define FLUSH_CBR	0x80
#define CBR_TABLE_SIZE	0x800
#define ALIGN(x, a)	(((x) + (a) - 1) / (a) * (a))

static int fd;
static FILE *logf;
static int delay_ms = 1000;

static void nap(void)
{
	if (delay_ms > 0)
		usleep(delay_ms * 1000);
}

static void logline(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(logf, fmt, ap);
	va_end(ap);
	fputc('\n', logf);
	fflush(logf);
	fsync(fileno(logf));

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void die(const char *msg)
{
	logline("FATAL: %s%s%s", msg, errno ? ": " : "", errno ? strerror(errno) : "");
	exit(1);
}

static uint32_t run(uint32_t *a)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	uint32_t stage = 0;

	if (ioctl(fd, IOC_NEW_CMD, a))
		die("NEW_CMD");
	if (poll(&p, 1, 3000) <= 0)
		die("encoder timeout (3s poll)");
	ioctl(fd, IOC_GET_STAGE, &stage);
	return stage;
}

static uint32_t out_size(void)
{
	uint32_t o[4] = { 0 };

	ioctl(fd, IOC_GET_OUTPUT_SIZE, o);
	return o[0];
}

static int read_full(void *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(0, (char *)buf + got, n - got);

		if (r <= 0)
			return 0;
		got += r;
	}
	return 1;
}

static void write_full(const void *buf, size_t n)
{
	while (n) {
		ssize_t r = write(1, buf, n);

		if (r <= 0)
			die("write");
		buf = (const char *)buf + r;
		n -= r;
	}
}

static void put_plane(uint8_t *dst, uint32_t pitch, uint32_t rows, const uint8_t *src,
		      uint32_t w, uint32_t h, uint32_t sstride)
{
	uint32_t r;

	for (r = 0; r < rows; r++)
		memcpy(dst + r * pitch, src + (size_t)(r < h ? r : h - 1) * sstride, w);
}

int main(int argc, char **argv)
{
	uint32_t w = 0, h = 0, fmt = FMT_NV12, qp = 26, gop = 60;
	uint32_t sstride = 0, uv_off = 0, frame_bytes = 0;
	int opt, nframes = 5;

	while ((opt = getopt(argc, argv, "s:q:n:D:")) != -1) {
		switch (opt) {
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) return 2; break;
		case 'q': qp = atoi(optarg); break;
		case 'n': nframes = atoi(optarg); break;
		case 'D': delay_ms = atoi(optarg); break;
		default: return 2;
		}
	}
	if (!w || !h || (w & 1) || (h & 1) || qp < 10 || qp > 51)
		return 2;

	logf = fopen("/root/enc-test.log", "a");
	if (!logf) {
		perror("fopen log");
		return 1;
	}

	uint32_t pitch = ALIGN(w, 32), vh = ALIGN(h, 16);
	if (!sstride)
		sstride = w;
	if (!uv_off)
		uv_off = sstride * h;
	if (!frame_bytes)
		frame_bytes = uv_off + sstride * h / 2;
	size_t in_size = frame_bytes;
	uint32_t a[56] = { 0 }, bufsize = 0, dct_off, bs_off, bs_size, cbr_off;
	uint8_t *mem, *in, hdr[256];
	uint32_t hdr_len;
	unsigned long long total = 0;
	int n;

	logline("=== venc-stage start %ux%u qp=%u ===", w, h, qp);

	logline("S0: open /dev/amvenc_avc ...");
	fd = open("/dev/amvenc_avc", O_RDWR);
	if (fd < 0)
		die("open /dev/amvenc_avc");
	logline("S0: open OK (fd=%d)", fd);
	nap();

	logline("S1: CONFIG_INIT (poweron + firmware load) ...");
	a[0] = 0;		/* UCODE_MODE_FULL */
	a[1] = vh / 16;		/* one slice */
	a[2] = w;
	a[3] = h;
	if (ioctl(fd, IOC_CONFIG_INIT, a))
		die("CONFIG_INIT");
	dct_off = a[1];
	bs_off = a[3];
	bs_size = a[4];
	cbr_off = a[9];
	logline("S1: CONFIG_INIT OK (dct_off=%u bs_off=%u bs_size=%u cbr_off=%u)",
		dct_off, bs_off, bs_size, cbr_off);
	nap();

	logline("S2: GET_BUFFINFO + mmap ...");
	ioctl(fd, IOC_GET_BUFFINFO, &bufsize);
	mem = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	in = malloc(in_size);
	if (mem == MAP_FAILED || !in)
		die("mmap/malloc");
	logline("S2: mmap OK (bufsize=%u)", bufsize);
	nap();

	logline("S3: SEQUENCE (SPS/PPS) ...");
	memset(a, 0, sizeof(a));
	a[0] = SEQUENCE;
	a[2] = qp;
	a[3] = FLUSH_OUTPUT;
	if (run(a) != PICTURE_DONE)
		die("SPS/PPS generation failed");
	n = out_size();
	hdr_len = (n >> 16) + (n & 0xffff);
	if (hdr_len > sizeof(hdr))
		die("headers too large");
	memcpy(hdr, mem + bs_off, hdr_len);
	logline("S3: SPS/PPS OK (hdr_len=%u)", hdr_len);
	nap();

	for (n = 0; n < nframes && read_full(in, in_size); n++) {
		int idr = n == 0 || (gop && n % gop == 0);
		uint32_t cmd = idr ? IDR : NON_IDR, stage, sz;
		uint8_t *y = mem + dct_off, *c = y + pitch * vh;

		logline("S%d: frame %d (%s) ...", idr ? 4 : 5, n, idr ? "IDR" : "NON_IDR");
		put_plane(y, pitch, vh, in, w, h, sstride);
		put_plane(c, pitch, vh / 2, in + uv_off, w, h / 2, sstride);
		memset(mem + cbr_off, qp, CBR_TABLE_SIZE);
		memset(a, 0, sizeof(a));
		a[0] = cmd;
		a[2] = LOCAL_BUFF;
		a[3] = fmt;
		a[5] = pitch * vh * 3 / 2;
		a[6] = qp;
		a[7] = FLUSH_INPUT | FLUSH_OUTPUT | FLUSH_CBR;
		a[8] = 2000;
		a[13] = w;
		a[14] = h;
		stage = run(a);
		sz = out_size();
		if ((stage != IDR_DONE && stage != NON_IDR_DONE) || !sz || sz > bs_size) {
			logline("FATAL: frame %d failed (stage %u, %u bytes)", n, stage, sz);
			return 1;
		}
		if (idr)
			write_full(hdr, hdr_len);
		write_full(mem + bs_off, sz);
		ioctl(fd, IOC_SUBMIT, &cmd);
		total += sz;
		logline("S%d: frame %d OK (stage %u, %u bytes)", idr ? 4 : 5, n, stage, sz);
		nap();
	}
	logline("S6: DONE (%d frames, %llu bytes)", n, total);
	fclose(logf);
	close(fd);
	return 0;
}
