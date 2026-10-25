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
 *     -r  raw: time the ARM's own uncached accesses to the window, with no
 *         FPGA involved (any core will do): read, write, write + barrier,
 *         write then read back. Splits the round trip into its ARM half and
 *         its FPGA half.
 *     -S  open /dev/mem without O_SYNC (does the mapping type change the
 *         cost? on a page Linux does not own it may not)
 *     -B  no barriers around the request write in the echo loop
 *     -F  read the request word back right after writing it, to see whether
 *         that pushes the write out to DDR3 sooner (step 1c)
 *     -L  step 1d: time the lightweight HPS-to-FPGA bridge instead of DDR3.
 *         Needs a core built with sys/lw_echo.sv (AmigaCD_MiSTer
 *         claude/lwbridge-echo) and installed under a path containing
 *         "lwbridge": with no slave behind 0xFF200000 an access can hang the
 *         ARM's bus, so -L refuses to run on any other core. -f overrides the
 *         check (only for a core you know has the echo).
 *
 * Without O_SYNC the mapping is cacheable (-r reads 9.8 ns): a request then
 * waits in the ARM's cache and the round trip never completes, so the echo
 * loop refuses -S, and gives up after MAX_TIMEOUTS consecutive timeouts
 * either way rather than running for hours.
 *
 * Results of the first run (2026-10-25, seed-20 probe core): median round
 * trip 2.51 us, p99 ~23 us, idle and with Main busy alike; FPGA DDR3 read
 * 141 ns; a median of 10 empty polls (~2 us) per request, i.e. most of the
 * round trip is spent before the FPGA sees the request. -r is the follow-up.
 * Run it twice: once with the Amiga idle, once with Main busy (a CD32 title
 * streaming from a CHD). For the cleanest number, idle the 68k on a config
 * with no fast RAM, which shares the FPGA's DDR3 port ahead of the probe.
 *
 * Build: make -C tools/latprobe (the same arm-none-linux-gnueabihf toolchain
 * as Main; static, so it runs on any MiSTer image).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
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
#define RAW_OFF          0x8090UL      /* Avalon 0x1012: unused by the probe and the mailbox */

#define TIMEOUT_NS       100000000LL   /* 100 ms: the probe is not answering */
#define MAX_TIMEOUTS     5             /* consecutive, then give up */
#define FPGA_MHZ         113.5

#define LW_BASE          0xFF200000UL  /* lightweight HPS-to-FPGA bridge */
#define LW_WINDOW        0x1000UL
#define LW_MAGIC         0x4C574252UL  /* "LWBR", lw_echo.sv register 1 */
#define LW_MHZ           100.0         /* lw_echo.sv runs on clk_100m */

#define PCT(a, p) (a)[(long)((double)(done - 1) * (p))]

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
static int no_barrier, flush_rb;

static inline void write_req(volatile uint32_t *w, uint32_t seq)
{
	w[0] = seq;
	if (!no_barrier) __sync_synchronize();
	w[1] = PROBE_MAGIC;
	if (!no_barrier) __sync_synchronize();
	if (flush_rb) (void)w[1];
}

/* -r: ns per operation, as the median over batches of RAW_BATCH operations,
 * so a timer interrupt landing in one batch does not move the result. */
#define RAW_BATCH 1000

static double raw_one(volatile uint32_t *w, int kind, long batches)
{
	double *per = malloc(sizeof(*per) * batches);
	uint32_t sink = 0;
	if (!per) return -1;
	for (long b = 0; b < batches; b++) {
		int64_t t0 = now_ns();
		for (int i = 0; i < RAW_BATCH; i++) {
			switch (kind) {
			case 0: sink += w[0]; break;                               /* read */
			case 1: w[0] = (uint32_t)i; break;                         /* write */
			case 2: w[0] = (uint32_t)i; __sync_synchronize(); break;   /* write + dmb */
			case 3: w[0] = (uint32_t)i; sink += w[0]; break;           /* write, read back */
			}
		}
		per[b] = (double)(now_ns() - t0) / RAW_BATCH;
	}
	for (long i = 1; i < batches; i++) {          /* insertion sort: small n */
		double v = per[i]; long j = i - 1;
		while (j >= 0 && per[j] > v) { per[j + 1] = per[j]; j--; }
		per[j + 1] = v;
	}
	double med = per[batches / 2];
	free(per);
	if (sink == 0xDEADBEEF) fprintf(stderr, " ");  /* keep the reads */
	return med;
}

/* -L: is the running core one with the bridge echo? Main_MiSTer is started
 * with the .rbf path as an argument: look for an argument ending in ".rbf"
 * whose path contains "lwbridge", in any process's command line. */
static int lw_core_loaded(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int found = 0;
	if (!d) return 0;
	while (!found && (e = readdir(d))) {
		char path[300], buf[4096];
		if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
		snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
			size_t l = strlen(buf + i);
			if (l > 4 && !strcmp(buf + i + l - 4, ".rbf") && strstr(buf + i, "lwbridge")) found = 1;
		}
	}
	closedir(d);
	return found;
}

/* -L: the echo register is lw[0], MAGIC lw[1], CYCLES lw[2], COUNT lw[3].
 * Batched medians per access kind, as -r does, then each write-and-read-back
 * round trip timed on its own for the tail, checking every value echoes. */
static int lw_mode(volatile uint32_t *lw, long count, int cpu)
{
	uint32_t magic = lw[1];
	if (magic != LW_MAGIC) {
		fprintf(stderr, "no lw_echo behind the bridge: register 1 reads %08x, not %08lx\n",
		        magic, (unsigned long)LW_MAGIC);
		return 1;
	}
	long batches = count / RAW_BATCH;
	static const char *name[] = { "read", "write", "write + dmb", "write, read back" };
	if (batches < 11) batches = 11;
	printf("latprobe -L: lightweight HPS-to-FPGA bridge, cpu %d, %ld batches of %d\n", cpu, batches, RAW_BATCH);
	for (int k = 0; k < 4; k++)
		printf("  %-17s %7.1f ns per op (median of batches)\n", name[k], raw_one(lw, k, batches));

	/* The FPGA's own clock across one batch of reads: no ARM timer in it. */
	uint32_t c0 = lw[2];
	for (int i = 0; i < RAW_BATCH; i++) (void)lw[0];
	uint32_t c1 = lw[2];
	printf("  read, by the FPGA's clock: %.1f ns per op (%u cycles at %.0f MHz for %d)\n",
	       (c1 - c0) * 1000.0 / LW_MHZ / RAW_BATCH, c1 - c0, LW_MHZ, RAW_BATCH);

	int64_t *rt = malloc(sizeof(*rt) * count);
	if (!rt) { fprintf(stderr, "out of memory\n"); return 1; }
	int64_t clk_over = INT64_MAX;
	for (int i = 0; i < 1000; i++) {                 /* the cost of the timer itself */
		int64_t a = now_ns(), b = now_ns();
		if (b - a < clk_over) clk_over = b - a;
	}
	long bad = 0, done = count;
	uint32_t v = (uint32_t)now_ns();
	for (long i = 0; i < count; i++) {
		v = v * 1664525u + 1013904223u;
		int64_t t0 = now_ns();
		lw[0] = v;
		uint32_t back = lw[0];
		rt[i] = now_ns() - t0;
		if (back != v) bad++;
	}
	qsort(rt, count, sizeof(*rt), cmp_i64);
	double sum = 0;
	for (long i = 0; i < count; i++) sum += rt[i];
	printf("echo round trip (write, read back), each timed alone, ARM clock (ns, includes the timer's own %lld):\n",
	       (long long)clk_over);
	printf("  min %lld  median %lld  mean %.0f  p90 %lld  p99 %lld  p99.9 %lld  max %lld\n",
	       (long long)rt[0], (long long)PCT(rt, 0.5), sum / done, (long long)PCT(rt, 0.9),
	       (long long)PCT(rt, 0.99), (long long)PCT(rt, 0.999), (long long)rt[done - 1]);
	printf("  %ld of %ld read back a different value\n", bad, count);
	uint32_t n = lw[3];
	printf("  lw_echo served %u reads and %u writes (16-bit counts, wrapping)\n", n & 0xFFFF, n >> 16);
	free(rt);
	return bad ? 1 : 0;
}

static void raw_mode(uint8_t *base, long count, int cpu, int sync)
{
	volatile uint32_t *w = (volatile uint32_t *)(base + RAW_OFF);
	long batches = count / RAW_BATCH;
	static const char *name[] = { "read", "write", "write + dmb", "write, read back" };
	if (batches < 11) batches = 11;
	printf("latprobe -r: ARM-side uncached access cost, cpu %d, %s mapping, %ld batches of %d\n",
	       cpu, sync ? "O_SYNC" : "no O_SYNC", batches, RAW_BATCH);
	for (int k = 0; k < 4; k++)
		printf("  %-17s %7.1f ns per op (median of batches)\n", name[k], raw_one(w, k, batches));
}

int main(int argc, char **argv)
{
	long count = 100000, gap_us = 0;
	int cpu = 0, opt, raw = 0, sync = 1, lw = 0, force = 0;
	const char *dev = NULL;

	while ((opt = getopt(argc, argv, "n:c:g:d:rSBFLfh")) != -1) {
		switch (opt) {
		case 'n': count = strtol(optarg, NULL, 0); break;
		case 'c': cpu = (int)strtol(optarg, NULL, 0); break;
		case 'g': gap_us = strtol(optarg, NULL, 0); break;
		case 'd': dev = optarg; break;
		case 'r': raw = 1; break;
		case 'S': sync = 0; break;
		case 'B': no_barrier = 1; break;
		case 'F': flush_rb = 1; break;
		case 'L': lw = 1; break;
		case 'f': force = 1; break;
		default:
			fprintf(stderr, "usage: %s [-n count] [-c cpu] [-g gap_us] [-r] [-S] [-B] [-F] [-L [-f]]\n", argv[0]);
			return 2;
		}
	}
	if (count < 1) count = 1;

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		fprintf(stderr, "warning: cannot pin to cpu %d: %s\n", cpu, strerror(errno));

	if (lw) {
		if (!dev && !force && !lw_core_loaded()) {
			fprintf(stderr, "-L: no running core has \"lwbridge\" in its path. Without lw_echo.sv behind the "
			                "bridge an access can hang the ARM, so -L only runs on that core (-f overrides).\n");
			return 2;
		}
		int lfd = open(dev ? dev : "/dev/mem", O_RDWR | O_SYNC);
		if (lfd < 0) { perror(dev ? dev : "/dev/mem"); return 1; }
		void *lb = mmap(NULL, LW_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, lfd, dev ? 0 : LW_BASE);
		if (lb == MAP_FAILED) { perror("mmap"); return 1; }
		return lw_mode((volatile uint32_t *)lb, count, cpu);
	}

	int fd = open(dev ? dev : "/dev/mem", O_RDWR | (sync ? O_SYNC : 0));
	if (fd < 0) { perror(dev ? dev : "/dev/mem"); return 1; }
	uint8_t *base = mmap(NULL, DDR3_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, dev ? 0 : DDR3_FLAT_BASE);
	if (base == MAP_FAILED) { perror("mmap"); return 1; }

	if (raw) { raw_mode(base, count, cpu, sync); return 0; }
	if (!sync && !dev) {
		fprintf(stderr, "-S maps the window cacheable: requests never reach DDR3 promptly, so the round trip "
		                "cannot be timed. -S is for -r only.\n");
		return 2;
	}

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

	long done = 0, timeouts = 0, in_a_row = 0;
	for (long i = 0; i < count; i++) {
		seq += 2;
		int64_t t0 = now_ns();
		write_req(req, seq);
		while (resp[1] != seq) {
			if (now_ns() - t0 > TIMEOUT_NS) break;
		}
		int64_t t1 = now_ns();
		if (resp[1] != seq) {
			timeouts++;
			if (++in_a_row >= MAX_TIMEOUTS) {
				fprintf(stderr, "giving up: %d round trips in a row timed out (%ld done)\n", MAX_TIMEOUTS, done);
				break;
			}
			continue;
		}
		in_a_row = 0;
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

	printf("latprobe: %ld round trips on cpu %d, %ld timed out, gap %ld us%s%s%s\n", done, cpu, timeouts, gap_us,
	       sync ? "" : ", no O_SYNC", no_barrier ? ", no barriers" : "", flush_rb ? ", read-back flush" : "");
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
