/*
 * fmvbench -- can the MiSTer's ARM decode the CD32 FMV module's MPEG-1?
 *
 * MEASUREMENT ONLY. Phase 1 of docs/fmv-plan.md: the FMV plan decodes MPEG on
 * the ARM, in Main, and puts only a register front end and an overlay in the
 * FPGA. That only works if a Cortex-A9 core can keep up, with room to spare,
 * while Main and the core's CD streaming run alongside. This times it.
 *
 * The stream is demuxed into memory first (untimed). Then the video
 * (libmpeg2, then YUV 4:2:0 -> RGB565, the format the core will scan out) and
 * the audio (kjmp2, MPEG-1 Layer II) are decoded, timing every picture and
 * every audio frame.
 *
 * Input, detected from the file:
 *   - an MPEG-1 (or MPEG-2) program stream (.mpg / .mpeg)
 *   - a Video CD .dat file (RIFF/CDXA: 2352-byte raw sectors after a header)
 *   - a raw 2352-byte track, as `bin` or ripped from a CHD (Mode 1, or Mode 2
 *     Form 1/2). This is how Cannon Fodder's MPEG track will be fed.
 *
 * Usage:
 *   fmvbench [options] FILE
 *     -c CPU    pin to this core (default 0; Main pins itself to 1). -1: no pin
 *     -t SECS   demux at most this much content (default 120)
 *     -n N      decode the demuxed content N times (default 1)
 *     -a ACCEL  c | arm   motion compensation in C, or libmpeg2's ARM
 *               assembler (only in a binary built with it; default arm if
 *               present). The two are timed by running twice.
 *     -p        paced: decode in real time, sleeping between pictures, and
 *               count pictures that missed their display deadline. Gives the
 *               CPU share of real playback rather than the flat-out rate.
 *     -x PREFIX demux only: write PREFIX.m1v and PREFIX.mp2 and stop (tests)
 *     -y N      write displayed picture N as raw I420 to stdout and stop
 *               (tests compare it with ffmpeg's decode of the same picture)
 *     -Y N      the same, as the RGB565 (little-endian) this tool converts to
 *     -q        no per-run detail, only the summary lines
 *
 * Output ends with key=value summary lines starting "FMVBENCH" and one gate
 * line, "FMVBENCH GATE: PASS|FAIL ...", judged as phase 1 states it: p99
 * time per picture (decode + convert) under 30 ms, and total decode work
 * (video + audio) at most 60 % of one core at the stream's frame rate.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "config.h"   /* lib/libmpeg2: ARCH_ARM when built with the assembler */
#include "mpeg2.h"
#include "kjmp2.h"

#define GATE_P99_MS   30.0
#define GATE_LOAD_PCT 60.0

/* ---------------------------------------------------------------- utility */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double cpu_s(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 +
	       ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
}

static void die(const char *msg)
{
	fprintf(stderr, "fmvbench: %s\n", msg);
	exit(2);
}

typedef struct {
	uint8_t *p;
	size_t   len, cap;
} buf_t;

static void buf_add(buf_t *b, const uint8_t *src, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 1 << 20;
		while (cap < b->len + n)
			cap *= 2;
		b->p = realloc(b->p, cap);
		if (!b->p)
			die("out of memory");
		b->cap = cap;
	}
	memcpy(b->p + b->len, src, n);
	b->len += n;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

/* p in [0,1]; v is sorted. Nearest-rank, so p99 of 100 samples is the 99th. */
static double pct(const double *v, size_t n, double p)
{
	if (!n)
		return 0;
	size_t k = (size_t)(p * n + 0.999999);
	if (k < 1)
		k = 1;
	if (k > n)
		k = n;
	return v[k - 1];
}

/* --------------------------------------------------------- sector unwrapping */

static const uint8_t SYNC[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff,
                                  0xff, 0xff, 0xff, 0xff, 0xff, 0 };

/*
 * User data of one raw 2352-byte sector. Mode 1: 2048 bytes at 16. Mode 2:
 * an 8-byte subheader at 16, then Form 1 (2048 bytes) or Form 2 (2324 bytes,
 * submode bit 5) at 24. A Video CD's MPEG track is Mode 2 Form 2. Returns the
 * length, 0 for a sector with no sync (audio, or unreadable).
 */
static size_t sector_payload(const uint8_t *s, const uint8_t **out)
{
	if (memcmp(s, SYNC, 12))
		return 0;
	if (s[15] == 1) {
		*out = s + 16;
		return 2048;
	}
	if (s[15] == 2) {
		*out = s + 24;
		return (s[18] & 0x20) ? 2324 : 2048;
	}
	return 0;
}

/* ------------------------------------------------------------------ demux */

typedef struct {
	buf_t  video, audio;
	int    video_id, audio_id;   /* first stream of each kind seen */
	size_t packs, packets, skipped;
	int    mpeg2_ps;
} demux_t;

/*
 * Feed a whole program stream (MPEG-1 ISO 11172-1, or MPEG-2 PS) and append
 * the first video and first audio stream's payload. Tolerates junk between
 * packets -- a Video CD track has empty and padding sectors -- by scanning for
 * the next start code. limit: stop once this many video bytes are held.
 */
static void demux(demux_t *d, const uint8_t *p, size_t n, size_t limit)
{
	size_t i = 0;
	while (i + 4 <= n && d->video.len < limit) {
		if (p[i] || p[i + 1] || p[i + 2] != 1) {
			i++;
			continue;
		}
		uint8_t code = p[i + 3];
		if (code == 0xb9) {                 /* end code */
			i += 4;
			continue;
		}
		if (code == 0xba) {                 /* pack header */
			if (i + 5 > n)
				break;
			d->packs++;
			if ((p[i + 4] & 0xc0) == 0x40) { /* MPEG-2: 14 + stuffing */
				if (i + 14 > n)
					break;
				d->mpeg2_ps = 1;
				i += 14 + (p[i + 13] & 7);
			} else {
				i += 12;
			}
			continue;
		}
		if (code < 0xbb) {                  /* not a system start code */
			i++;
			continue;
		}
		if (i + 6 > n)
			break;
		size_t len = ((size_t)p[i + 4] << 8) | p[i + 5];
		size_t end = i + 6 + len;
		if (end > n)
			break;
		int is_audio = code >= 0xc0 && code <= 0xdf;
		int is_video = code >= 0xe0 && code <= 0xef;
		if (!is_audio && !is_video) {       /* system header, padding, ... */
			i = end;
			continue;
		}
		d->packets++;
		size_t h = i + 6;
		if (h < end && (p[h] & 0xc0) == 0x80) {
			/* MPEG-2 PES header */
			if (h + 3 > end) { i = end; continue; }
			h += 3 + p[h + 2];
		} else {
			while (h < end && p[h] == 0xff)          /* stuffing */
				h++;
			if (h < end && (p[h] & 0xc0) == 0x40)    /* STD buffer */
				h += 2;
			if (h < end && (p[h] & 0xf0) == 0x20)        h += 5;  /* PTS */
			else if (h < end && (p[h] & 0xf0) == 0x30)   h += 10; /* PTS DTS */
			else if (h < end && p[h] == 0x0f)            h += 1;
		}
		if (h > end) {
			d->skipped++;
			i = end;
			continue;
		}
		if (is_video) {
			if (d->video_id < 0)
				d->video_id = code;
			if (code == d->video_id)
				buf_add(&d->video, p + h, end - h);
		} else {
			if (d->audio_id < 0)
				d->audio_id = code;
			if (code == d->audio_id)
				buf_add(&d->audio, p + h, end - h);
		}
		i = end;
	}
}

/*
 * Read FILE and return its program stream: the file itself, or the user data
 * of its sectors for a .dat or raw track. *kind says which.
 */
static uint8_t *load_stream(const char *path, size_t *out_len, const char **kind)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "fmvbench: %s: %s\n", path, strerror(errno));
		exit(2);
	}
	buf_t raw = { 0 };
	uint8_t chunk[1 << 16];
	size_t r;
	while ((r = fread(chunk, 1, sizeof chunk, f)) > 0)
		buf_add(&raw, chunk, r);
	fclose(f);

	size_t off = 0;
	int sectored = 0;
	if (raw.len >= 44 && !memcmp(raw.p, "RIFF", 4) && !memcmp(raw.p + 8, "CDXA", 4)) {
		/* walk the chunks to "data" */
		size_t c = 12;
		while (c + 8 <= raw.len) {
			uint32_t sz = raw.p[c + 4] | raw.p[c + 5] << 8 |
			              raw.p[c + 6] << 16 | (uint32_t)raw.p[c + 7] << 24;
			if (!memcmp(raw.p + c, "data", 4)) {
				off = c + 8;
				break;
			}
			c += 8 + sz + (sz & 1);
		}
		if (!off)
			die("CDXA file without a data chunk");
		sectored = 1;
		*kind = "vcd-dat";
	} else if (raw.len >= 2352 && !memcmp(raw.p, SYNC, 12)) {
		sectored = 1;
		*kind = "raw-2352";
	}

	if (!sectored) {
		*kind = "program-stream";
		*out_len = raw.len;
		return raw.p;
	}

	buf_t ps = { 0 };
	for (size_t s = off; s + 2352 <= raw.len; s += 2352) {
		const uint8_t *data;
		size_t n = sector_payload(raw.p + s, &data);
		if (n)
			buf_add(&ps, data, n);
	}
	free(raw.p);
	*out_len = ps.len;
	return ps.p;
}

/* ------------------------------------------------- YUV 4:2:0 -> RGB565 */

static uint8_t clamp_tab[1024];
#define CLAMP(x) clamp_tab[((x) >> 8) + 384]

static void convert_init(void)
{
	for (int i = 0; i < 1024; i++) {
		int v = i - 384;
		clamp_tab[i] = v < 0 ? 0 : v > 255 ? 255 : v;
	}
}

/*
 * ITU-R BT.601, limited range (16-235 / 16-240), as MPEG-1 specifies. One
 * chroma sample covers 2x2 luma. Output is the RGB565 picture the core would
 * read from DDR3 -- here ordinary memory; see the note in docs/fmv-plan.md
 * phase 1 on what writing it uncached would add.
 */
static void yuv420_to_rgb565(uint8_t *const buf[3], int w, int h, int cw,
                             uint16_t *out)
{
	for (int y = 0; y < h; y += 2) {
		const uint8_t *y0 = buf[0] + y * w, *y1 = y0 + w;
		const uint8_t *u = buf[1] + (y / 2) * cw, *v = buf[2] + (y / 2) * cw;
		uint16_t *o0 = out + y * w, *o1 = o0 + w;
		for (int x = 0; x < w; x += 2) {
			int d = *u++ - 128, e = *v++ - 128;
			int rr = 409 * e + 128, gg = -100 * d - 208 * e + 128, bb = 516 * d + 128;
#define PIX(o, yy) do { int c = 298 * ((yy) - 16); \
	*(o) = (uint16_t)((CLAMP(c + rr) >> 3) << 11 | (CLAMP(c + gg) >> 2) << 5 | CLAMP(c + bb) >> 3); \
} while (0)
			PIX(o0 + x, y0[x]);
			PIX(o0 + x + 1, y0[x + 1]);
			PIX(o1 + x, y1[x]);
			PIX(o1 + x + 1, y1[x + 1]);
#undef PIX
		}
	}
}

/* ------------------------------------------------------------ video run */

typedef struct {
	double *dec, *conv, *tot;    /* per decoded picture, seconds */
	char   *type;                /* 'I' 'P' 'B' 'D' */
	size_t  n, cap;
	size_t  displayed;
	double  frame_period;        /* seconds, from the sequence header */
	int     width, height;
	size_t  late;                /* paced: pictures past their deadline */
	double  worst_late;
} vstats_t;

static void vs_push(vstats_t *s, double dec, double conv, char type)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 4096;
		s->dec  = realloc(s->dec,  s->cap * sizeof(double));
		s->conv = realloc(s->conv, s->cap * sizeof(double));
		s->tot  = realloc(s->tot,  s->cap * sizeof(double));
		s->type = realloc(s->type, s->cap);
		if (!s->dec || !s->conv || !s->tot || !s->type)
			die("out of memory");
	}
	s->dec[s->n] = dec;
	s->conv[s->n] = conv;
	s->tot[s->n] = dec + conv;
	s->type[s->n] = type;
	s->n++;
}

static char coding_type(const mpeg2_picture_t *pic)
{
	if (!pic)
		return '?';
	switch (pic->flags & PIC_MASK_CODING_TYPE) {
	case PIC_FLAG_CODING_TYPE_I: return 'I';
	case PIC_FLAG_CODING_TYPE_P: return 'P';
	case PIC_FLAG_CODING_TYPE_B: return 'B';
	case PIC_FLAG_CODING_TYPE_D: return 'D';
	}
	return '?';
}

/*
 * Decode es once. Time is attributed to pictures as it accrues: everything
 * mpeg2_parse() spends between one finished picture and the next belongs to
 * the next, so headers and slices are both counted and nothing is counted
 * twice. dump_n >= 0: write that displayed picture as I420 to stdout and stop.
 */
static int dump_rgb;   /* -Y: dump RGB565 instead of I420 */

static int run_video(const buf_t *es, vstats_t *s, int paced, long dump_n)
{
	mpeg2dec_t *dec = mpeg2_init();
	if (!dec)
		die("mpeg2_init failed");
	const mpeg2_info_t *info = mpeg2_info(dec);
	uint16_t *rgb = NULL;
	const uint8_t *p = es->p, *end = es->p + es->len;
	static uint8_t seq_end[4] = { 0, 0, 1, 0xb7 };
	double acc = 0, play0 = 0;
	char cur_type = '?';
	int done = 0, flushed = 0;

	while (!done) {
		double a = now_s();
		mpeg2_state_t st = mpeg2_parse(dec);
		acc += now_s() - a;

		switch (st) {
		case STATE_BUFFER:
			if (p >= end) {
				/* A sequence end code makes libmpeg2 finish and show the
				 * last picture, which it otherwise holds waiting for more. */
				if (flushed) {
					done = 1;
					break;
				}
				flushed = 1;
				mpeg2_buffer(dec, seq_end, seq_end + 4);
				break;
			}
			{
				const uint8_t *q = p + 4096 < end ? p + 4096 : end;
				mpeg2_buffer(dec, (uint8_t *)p, (uint8_t *)q);
				p = q;
			}
			break;
		case STATE_SEQUENCE:
			s->width = info->sequence->width;
			s->height = info->sequence->height;
			s->frame_period = info->sequence->frame_period / 27e6;
			free(rgb);
			rgb = malloc((size_t)s->width * s->height * 2);
			if (!rgb)
				die("out of memory");
			break;
		case STATE_PICTURE:
			cur_type = coding_type(info->current_picture);
			break;
		case STATE_SLICE:
		case STATE_END:
		case STATE_INVALID_END: {
			double conv = 0;
			if (info->display_fbuf && rgb) {
				if (dump_n >= 0 && (long)s->displayed == dump_n && dump_rgb) {
					yuv420_to_rgb565(info->display_fbuf->buf, s->width, s->height,
					                 info->sequence->chroma_width, rgb);
					for (size_t k = 0; k < (size_t)s->width * s->height; k++) {
						uint8_t le[2] = { rgb[k] & 0xff, rgb[k] >> 8 };
						fwrite(le, 1, 2, stdout);
					}
					mpeg2_close(dec);
					free(rgb);
					return 0;
				}
				if (dump_n >= 0 && (long)s->displayed == dump_n) {
					const mpeg2_sequence_t *q = info->sequence;
					fwrite(info->display_fbuf->buf[0], 1, (size_t)q->width * q->height, stdout);
					fwrite(info->display_fbuf->buf[1], 1, (size_t)q->chroma_width * q->chroma_height, stdout);
					fwrite(info->display_fbuf->buf[2], 1, (size_t)q->chroma_width * q->chroma_height, stdout);
					mpeg2_close(dec);
					free(rgb);
					return 0;
				}
				double c0 = now_s();
				yuv420_to_rgb565(info->display_fbuf->buf, s->width, s->height,
				                 info->sequence->chroma_width, rgb);
				conv = now_s() - c0;
				s->displayed++;
				if (paced && s->frame_period > 0) {
					if (!play0)
						play0 = now_s();
					double due = play0 + s->displayed * s->frame_period;
					double t = now_s();
					if (t > due) {
						s->late++;
						if (t - due > s->worst_late)
							s->worst_late = t - due;
					} else {
						struct timespec ts;
						double w = due - t;
						ts.tv_sec = (time_t)w;
						ts.tv_nsec = (long)((w - ts.tv_sec) * 1e9);
						nanosleep(&ts, NULL);
					}
				}
			}
			/* STATE_SLICE ends a decoded picture. END and INVALID_END
			 * only release the last one for display; their time (little)
			 * stays in acc and lands on the next picture, if any. */
			if (st == STATE_SLICE) {
				vs_push(s, acc, conv, cur_type);
				acc = 0;
			}
			break;
		}
		case STATE_INVALID:
			/* libmpeg2 resyncs by itself; nothing to do */
			break;
		default:
			break;
		}
	}
	mpeg2_close(dec);
	free(rgb);
	return dump_n >= 0 ? 1 : 0;   /* asked to dump a picture that never came */
}

/* ------------------------------------------------------------ audio run */

typedef struct {
	double *t;
	size_t  n, cap;
	int     rate;
	size_t  bad;      /* bytes skipped looking for sync */
} astats_t;

static int mp2_header_ok(const uint8_t *h)
{
	/* sync 0xFFF, layer II (bits 2:1 of byte 1 == 10), a valid rate and freq */
	return h[0] == 0xff && (h[1] & 0xf6) == 0xf4 &&
	       (h[2] & 0xf0) != 0xf0 && (h[2] & 0xf0) != 0 && (h[2] & 0x0c) != 0x0c;
}

static void run_audio(const buf_t *es, astats_t *s)
{
	static kjmp2_context_t mp2;
	static signed short pcm[KJMP2_SAMPLES_PER_FRAME * 2];
	kjmp2_init(&mp2);
	size_t i = 0;
	while (i + 4 <= es->len) {
		if (!mp2_header_ok(es->p + i)) {
			i++;
			s->bad++;
			continue;
		}
		/* NULL pcm: size only, so a truncated last frame is never decoded */
		unsigned long sz = kjmp2_decode_frame(&mp2, es->p + i, NULL);
		if (!sz || i + sz > es->len)
			break;
		if (!s->rate)
			s->rate = kjmp2_get_sample_rate(es->p + i);
		double a = now_s();
		kjmp2_decode_frame(&mp2, es->p + i, pcm);
		double t = now_s() - a;
		if (s->n == s->cap) {
			s->cap = s->cap ? s->cap * 2 : 4096;
			s->t = realloc(s->t, s->cap * sizeof(double));
			if (!s->t)
				die("out of memory");
		}
		s->t[s->n++] = t;
		i += sz;
	}
}

/* ------------------------------------------------------------------ main */

static double sum(const double *v, size_t n)
{
	double t = 0;
	for (size_t i = 0; i < n; i++)
		t += v[i];
	return t;
}

int main(int argc, char **argv)
{
	int cpu = 0, repeat = 1, paced = 0, quiet = 0;
	double secs = 120;
	const char *xprefix = NULL, *accel_name = NULL;
	long dump_n = -1;
	int opt;

	while ((opt = getopt(argc, argv, "c:t:n:a:px:y:Y:q")) != -1) {
		switch (opt) {
		case 'c': cpu = atoi(optarg); break;
		case 't': secs = atof(optarg); break;
		case 'n': repeat = atoi(optarg); break;
		case 'a': accel_name = optarg; break;
		case 'p': paced = 1; break;
		case 'x': xprefix = optarg; break;
		case 'y': dump_n = atol(optarg); break;
		case 'Y': dump_n = atol(optarg); dump_rgb = 1; break;
		case 'q': quiet = 1; break;
		default:
			fprintf(stderr, "usage: fmvbench [-c cpu] [-t secs] [-n repeat] "
			        "[-a c|arm] [-p] [-x prefix] [-y n | -Y n] [-q] FILE\n");
			return 2;
		}
	}
	if (optind != argc - 1) {
		fprintf(stderr, "fmvbench: one input file expected\n");
		return 2;
	}
	if (repeat < 1)
		repeat = 1;

	/* Acceleration has to be chosen before mpeg2_init(), which would detect. */
	uint32_t accel = 0;
#ifdef ARCH_ARM
	accel = MPEG2_ACCEL_ARM;
#endif
	if (accel_name) {
		if (!strcmp(accel_name, "c"))
			accel = 0;
		else if (!strcmp(accel_name, "arm")) {
#ifndef ARCH_ARM
			die("-a arm: this binary was built without libmpeg2's ARM assembler");
#endif
		} else
			die("-a: c or arm");
	}
	uint32_t got = mpeg2_accel(accel);
	const char *accel_used = (got & MPEG2_ACCEL_ARM) && accel ? "arm" : "c";

	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof set, &set))
			fprintf(stderr, "fmvbench: cannot pin to cpu %d: %s (continuing)\n",
			        cpu, strerror(errno));
	}

	const char *kind;
	size_t ps_len;
	uint8_t *ps = load_stream(argv[optind], &ps_len, &kind);

	demux_t d = { .video_id = -1, .audio_id = -1 };
	/* ~1.4 Mbit/s is the Video CD rate; bound the demux by content time. The
	 * byte limit is generous so a low-rate stream is not cut short. */
	size_t limit = secs > 0 ? (size_t)(secs * 1.2e6) : (size_t)-1;
	demux(&d, ps, ps_len, limit);
	free(ps);

	if (xprefix) {
		char path[4096];
		snprintf(path, sizeof path, "%s.m1v", xprefix);
		FILE *f = fopen(path, "wb");
		if (!f) die("cannot write the .m1v");
		fwrite(d.video.p, 1, d.video.len, f);
		fclose(f);
		snprintf(path, sizeof path, "%s.mp2", xprefix);
		f = fopen(path, "wb");
		if (!f) die("cannot write the .mp2");
		fwrite(d.audio.p, 1, d.audio.len, f);
		fclose(f);
		printf("FMVBENCH DEMUX kind=%s packs=%zu packets=%zu video=%zu audio=%zu "
		       "video_id=0x%02x audio_id=0x%02x mpeg2_ps=%d\n",
		       kind, d.packs, d.packets, d.video.len, d.audio.len,
		       d.video_id & 0xff, d.audio_id & 0xff, d.mpeg2_ps);
		return 0;
	}
	if (!d.video.len)
		die("no video stream found");

	convert_init();

	if (dump_n >= 0) {
		vstats_t s = { 0 };
		int r = run_video(&d.video, &s, 0, dump_n);
		if (r)
			fprintf(stderr, "fmvbench: picture %ld not reached\n", dump_n);
		return r;
	}

	vstats_t vs = { 0 };
	astats_t as = { 0 };
	double wall0 = now_s(), cpu0 = cpu_s();
	for (int r = 0; r < repeat; r++) {
		run_video(&d.video, &vs, paced, -1);
		if (d.audio.len)
			run_audio(&d.audio, &as);
	}
	double wall = now_s() - wall0, cpu_used = cpu_s() - cpu0;

	/* content time: what was decoded, at the stream's own rates */
	double vcontent = vs.displayed * vs.frame_period;
	double acontent = as.rate ? (double)as.n * KJMP2_SAMPLES_PER_FRAME / as.rate : 0;
	double vwork = sum(vs.tot, vs.n), awork = sum(as.t, as.n);
	double load = vcontent > 0 ? 100.0 * (vwork + awork) / vcontent : 0;

	double *srt = malloc((vs.n ? vs.n : 1) * sizeof(double));
	memcpy(srt, vs.tot, vs.n * sizeof(double));
	qsort(srt, vs.n, sizeof(double), cmp_double);
	double p50 = pct(srt, vs.n, .50), p90 = pct(srt, vs.n, .90);
	double p99 = pct(srt, vs.n, .99), pmax = vs.n ? srt[vs.n - 1] : 0;

	size_t worst = 0, cnt[4] = { 0 };
	double tsum[4] = { 0 }, tmax[4] = { 0 };
	for (size_t i = 0; i < vs.n; i++) {
		if (vs.tot[i] > vs.tot[worst])
			worst = i;
		int k = vs.type[i] == 'I' ? 0 : vs.type[i] == 'P' ? 1 : vs.type[i] == 'B' ? 2 : 3;
		cnt[k]++;
		tsum[k] += vs.tot[i];
		if (vs.tot[i] > tmax[k])
			tmax[k] = vs.tot[i];
	}
	qsort(vs.conv, vs.n, sizeof(double), cmp_double);
	double *asrt = as.t;
	qsort(asrt, as.n, sizeof(double), cmp_double);

	if (!quiet) {
		static const char tn[4] = { 'I', 'P', 'B', '?' };
		printf("input %s (%s), demux: %zu packs, %zu packets, video %zu bytes (id 0x%02x), "
		       "audio %zu bytes (id 0x%02x)\n", argv[optind], kind, d.packs, d.packets,
		       d.video.len, d.video_id & 0xff, d.audio.len, d.audio_id & 0xff);
		printf("video %dx%d at %.3f fps, %zu pictures decoded, %zu displayed, x%d\n",
		       vs.width, vs.height, vs.frame_period > 0 ? 1 / vs.frame_period : 0,
		       vs.n, vs.displayed, repeat);
		for (int k = 0; k < 4; k++)
			if (cnt[k])
				printf("  %c: %6zu pictures, mean %6.2f ms, max %6.2f ms\n",
				       tn[k], cnt[k], 1e3 * tsum[k] / cnt[k], 1e3 * tmax[k]);
		printf("convert to RGB565: p50 %.2f ms, p99 %.2f ms, max %.2f ms\n",
		       1e3 * pct(vs.conv, vs.n, .5), 1e3 * pct(vs.conv, vs.n, .99),
		       vs.n ? 1e3 * vs.conv[vs.n - 1] : 0);
		printf("audio: %zu Layer II frames at %d Hz, p50 %.3f ms, p99 %.3f ms, max %.3f ms, "
		       "%zu bytes skipped for sync\n", as.n, as.rate, 1e3 * pct(asrt, as.n, .5),
		       1e3 * pct(asrt, as.n, .99), as.n ? 1e3 * asrt[as.n - 1] : 0, as.bad);
		if (paced)
			printf("paced: %zu of %zu pictures late, worst by %.2f ms\n",
			       vs.late, vs.displayed, 1e3 * vs.worst_late);
	}

	printf("FMVBENCH VIDEO accel=%s pictures=%zu displayed=%zu fps=%.3f "
	       "p50_ms=%.3f p90_ms=%.3f p99_ms=%.3f max_ms=%.3f worst_index=%zu worst_type=%c\n",
	       accel_used, vs.n, vs.displayed, vs.frame_period > 0 ? 1 / vs.frame_period : 0,
	       1e3 * p50, 1e3 * p90, 1e3 * p99, 1e3 * pmax, worst, vs.n ? vs.type[worst] : '-');
	printf("FMVBENCH AUDIO frames=%zu rate=%d content_s=%.2f total_ms=%.1f\n",
	       as.n, as.rate, acontent, 1e3 * awork);
	printf("FMVBENCH LOAD content_s=%.2f video_work_s=%.3f audio_work_s=%.3f "
	       "load_pct=%.1f wall_s=%.2f cpu_s=%.2f paced=%d late=%zu\n",
	       vcontent, vwork, awork, load, wall, cpu_used, paced, vs.late);

	int pass = vs.n > 0 && p99 * 1e3 < GATE_P99_MS && load <= GATE_LOAD_PCT &&
	           (!paced || vs.late == 0);
	printf("FMVBENCH GATE: %s (p99 %.2f ms < %.0f ms; load %.1f %% <= %.0f %% of one core%s)\n",
	       pass ? "PASS" : "FAIL", 1e3 * p99, GATE_P99_MS, load, GATE_LOAD_PCT,
	       paced ? (vs.late ? "; LATE pictures" : "; none late") : "");
	return pass ? 0 : 1;
}
