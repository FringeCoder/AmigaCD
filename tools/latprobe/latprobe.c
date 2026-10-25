/*
 * latprobe -- time the ARM -> DDR3 -> FPGA -> DDR3 -> ARM round trip.
 *
 * MEASUREMENT ONLY. Step 1 of the plan for a 68k that runs on the ARM, the way
 * a PiStorm's does: every chip RAM and custom register access such a CPU made
 * would pay this round trip, so this number decides whether the design is
 * worth building. It needs the core built with latprobe_ddr3.v (AmigaCD_MiSTer,
 * rtl/A2065/), which answers each request with an echo.
 *
 * Protocol (minimig_a2065_ddr3.h has the rest of this window):
 *   REQ  ARM 0x1FF08080  we write  {MAGIC[63:32], seq[31:0]}
 *   RESP ARM 0x1FF08088  FPGA writes {seq[63:32], rd_lat[31:16], polls[15:0]}
 * rd_lat is the FPGA's own DDR3 read of REQ, in ~114 MHz cycles; polls is how
 * many reads it made since its last answer that found nothing new.
 *
 * Usage (on the MiSTer, with the AmigaCD core loaded):
 *   latprobe [-n count] [-c cpu] [-g gap_us]
 *     -n  round trips to time (default 100000)
 *     -c  pin to this CPU core (default 0; Main_MiSTer pins itself to 1)
 *     -g  microseconds to wait between round trips (default 0)
 *     -d FILE  map FILE at offset 0 instead of /dev/mem (host test only;
 *              tools/latprobe/test_echo.py plays the FPGA)
 * Run it twice: once with the Amiga idle, once with Main busy (a CD32 title
 * streaming from a CHD). For the cleanest number, idle the 68k on a config
 * with no fast RAM, which shares the FPGA's DDR3 port ahead of the probe.
 *
 * Build: make -C tools/latprobe (the same arm-none-linux-gnueabihf toolchain
 * as Main; static, so it runs on any MiSTer image).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DDR3_FLAT_BASE   0x1FF00000UL
#define DDR3_WINDOW      0x10000UL
#define PROBE_REQ_OFF    0x8080UL      /* Avalon 0x1010 */
#define PROBE_RESP_OFF   0x8088UL      /* Avalon 0x1011 */
#define PROBE_MAGIC      0x4C415450UL  /* "LATP" */

#define TIMEOUT_NS       100000000LL   /* 100 ms: the probe is not answering */
#define FPGA_MHZ         113.5

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

static int cmp_u16(const void *a, const void *b)
{
	return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* The window is mapped uncached (O_SYNC), so every access is a real bus
 * transaction. 64-bit words are written low half, then high half: the probe
 * only acts when the high half holds MAGIC and the low half a new seq, so the
 * low half must already be in place when MAGIC lands. */
static inline void write_req(volatile uint32_t *w, uint32_t seq)
{
	w[0] = seq;
	__sync_synchronize();
	w[1] = PROBE_MAGIC;
	__sync_synchronize();
}

int main(int argc, char **argv)
{
	long count = 100000, gap_us = 0;
	int cpu = 0, opt;
	const char *dev = NULL;

	while ((opt = getopt(argc, argv, "n:c:g:d:h")) != -1) {
		switch (opt) {
		case 'n': count = strtol(optarg, NULL, 0); break;
		case 'c': cpu = (int)strtol(optarg, NULL, 0); break;
		case 'g': gap_us = strtol(optarg, NULL, 0); break;
		case 'd': dev = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-n count] [-c cpu] [-g gap_us]\n", argv[0]);
			return 2;
		}
	}
	if (count < 1) count = 1;

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		fprintf(stderr, "warning: cannot pin to cpu %d: %s\n", cpu, strerror(errno));

	int fd = open(dev ? dev : "/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror(dev ? dev : "/dev/mem"); return 1; }
	uint8_t *base = mmap(NULL, DDR3_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, dev ? 0 : DDR3_FLAT_BASE);
	if (base == MAP_FAILED) { perror("mmap"); return 1; }

	volatile uint32_t *req  = (volatile uint32_t *)(base + PROBE_REQ_OFF);
	volatile uint32_t *resp = (volatile uint32_t *)(base + PROBE_RESP_OFF);

	int64_t  *rt     = malloc(sizeof(*rt) * count);
	uint16_t *rd_lat = malloc(sizeof(*rd_lat) * count);
	uint16_t *polls  = malloc(sizeof(*polls) * count);
	if (!rt || !rd_lat || !polls) { fprintf(stderr, "out of memory\n"); return 1; }

	/* A seq the probe cannot have seen last, whatever an earlier run left. */
	uint32_t seq = (uint32_t)now_ns() | 1u;
	if (seq == resp[1]) seq += 2;

	/* Arm, and let the probe leave its slow idle poll (~144 us) first. */
	write_req(req, seq);
	int64_t t_arm = now_ns();
	while (resp[1] != seq) {
		if (now_ns() - t_arm > TIMEOUT_NS) {
			fprintf(stderr, "no answer from the FPGA in %lld ms: is the core built with latprobe_ddr3.v?\n",
			        TIMEOUT_NS / 1000000LL);
			req[1] = 0;
			return 1;
		}
	}

	long done = 0, timeouts = 0;
	for (long i = 0; i < count; i++) {
		seq += 2;
		int64_t t0 = now_ns();
		write_req(req, seq);
		while (resp[1] != seq) {
			if (now_ns() - t0 > TIMEOUT_NS) break;
		}
		int64_t t1 = now_ns();
		if (resp[1] != seq) { timeouts++; continue; }
		uint32_t lo = resp[0];
		rt[done]     = t1 - t0;
		rd_lat[done] = (uint16_t)(lo >> 16);
		polls[done]  = (uint16_t)(lo & 0xFFFF);
		done++;
		if (gap_us) usleep(gap_us);
	}

	/* Disarm: the probe drops back to its slow poll. */
	req[1] = 0;
	__sync_synchronize();

	if (!done) { fprintf(stderr, "every round trip timed out (%ld)\n", timeouts); return 1; }

	qsort(rt, done, sizeof(*rt), cmp_i64);
	qsort(rd_lat, done, sizeof(*rd_lat), cmp_u16);
	qsort(polls, done, sizeof(*polls), cmp_u16);

	double sum = 0;
	for (long i = 0; i < done; i++) sum += rt[i];

#define PCT(a, p) (a)[(long)((double)(done - 1) * (p))]
	printf("latprobe: %ld round trips on cpu %d, %ld timed out, gap %ld us\n", done, cpu, timeouts, gap_us);
	printf("round trip, ARM clock (ns):\n");
	printf("  min %lld  median %lld  mean %.0f  p90 %lld  p99 %lld  p99.9 %lld  max %lld\n",
	       (long long)rt[0], (long long)PCT(rt, 0.5), sum / done, (long long)PCT(rt, 0.9),
	       (long long)PCT(rt, 0.99), (long long)PCT(rt, 0.999), (long long)rt[done - 1]);
	printf("  => %.0f round trips per second at the median\n", 1e9 / (double)PCT(rt, 0.5));
	printf("FPGA's DDR3 read of the request (cycles at ~%.1f MHz):\n", FPGA_MHZ);
	printf("  min %u (%.0f ns)  median %u (%.0f ns)  p99 %u (%.0f ns)  max %u\n",
	       rd_lat[0], rd_lat[0] * 1000.0 / FPGA_MHZ,
	       PCT(rd_lat, 0.5), PCT(rd_lat, 0.5) * 1000.0 / FPGA_MHZ,
	       PCT(rd_lat, 0.99), PCT(rd_lat, 0.99) * 1000.0 / FPGA_MHZ, rd_lat[done - 1]);
	printf("FPGA polls that found nothing new before each request:\n");
	printf("  median %u  p99 %u  max %u\n", PCT(polls, 0.5), PCT(polls, 0.99), polls[done - 1]);
	return 0;
}
