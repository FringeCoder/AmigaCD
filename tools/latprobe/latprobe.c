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
 *     -V  step 1e: how long a DDR3 write takes to become visible across the
 *         HPS controller's ports, each direction on its own, with the bridge
 *         as the start and stop signal. Needs visprobe_ddr3.v (AmigaCD_MiSTer
 *         claude/lwbridge-echo 2053af4 or later) and the same "lwbridge" path
 *         as -L. -n is the trials per measurement (10000 takes seconds).
 *         With -d, the file holds the bridge registers at 0 and the DDR3
 *         window at 0x1000 (test_echo.py plays both).
 *     -P  step 2: drive the Amiga's bus from the ARM, through the 68000 bus
 *         proxy (armcpu_bridge.v, proxy68k.v; AmigaCD_MiSTer
 *         claude/lwbridge-echo). Needs the CPU set to 68000 and the same
 *         "lwbridge" path as -L. Takes the bus (the 68000 is held in reset,
 *         so the Amiga stops), checks chip RAM reads back what was written in
 *         every size, reads the beam position live from VHPOSR, times each
 *         access kind with 68000 timing and with FAST (reads through the
 *         SDRAM CPU port), checks FAST reads see both the ARM's own writes and
 *         a blitter clear, and gives the bus back -- the Amiga then reboots. With
 *         -d, the file is the bridge's 2 MB (a plain file is a fair model of
 *         memory order; the counters stand still).
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
#define LW_WORDS_OFF     0x1000UL      /* -V -d: where the DDR3 window starts in the file */
#define VIS_A_OFF        0x8090UL      /* Avalon 0x1012, visprobe_ddr3.v word A */
#define VIS_B_OFF        0x8098UL      /* Avalon 0x1013, visprobe_ddr3.v word B */
#define VIS_SPIN_NS      10000000LL    /* 10 ms: a visprobe step that never finishes */

#define PX_SPAN          0x200000UL    /* -P: the whole lightweight bridge */
#define PX_CTL           0x080000UL    /* armcpu_bridge.v control registers */
#define PX_WIN           0x100000UL    /* the 68k window, 1 MB per BANK */
#define PX_MAGIC         0x41435055UL  /* "ACPU" */
#define PX_TEST          0x070000UL    /* chip RAM -P may overwrite (the Amiga reboots after) */

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

/* The bridge's clock, measured: CYCLES (lw[2]) against the ARM's over 50 ms.
 * Step 1d ran it on clk_100m, step 1e on the core's DDRAM_CLK; 0 if the
 * counter does not move (the host test's plain file). */
static double lw_mhz(volatile uint32_t *lw)
{
	uint32_t c0 = lw[2];
	int64_t t0 = now_ns();
	usleep(50000);
	uint32_t c1 = lw[2];
	int64_t t1 = now_ns();
	return (c1 - c0) * 1000.0 / (double)(t1 - t0);
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
	double mhz = lw_mhz(lw);
	uint32_t c0 = lw[2];
	for (int i = 0; i < RAW_BATCH; i++) (void)lw[0];
	uint32_t c1 = lw[2];
	if (mhz > 1)
		printf("  read, by the FPGA's clock: %.1f ns per op (%u cycles at %.1f MHz for %d)\n",
		       (c1 - c0) * 1000.0 / mhz / RAW_BATCH, c1 - c0, mhz, RAW_BATCH);
	else
		printf("  read, by the FPGA's clock: CYCLES is not counting\n");

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

/* -V: one measurement's results, in bridge-clock cycles and stale reads. */
struct vis_set {
	const char *name;
	long n, timeouts;
	uint32_t *cyc;      /* the probe's own cycle count for each trial */
	uint32_t *miss;     /* reads that came back stale (A: the FPGA's, B: the ARM's) */
};

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return (x > y) - (x < y);
}

/* Spin on a bridge register until (value & mask) == want; 0 on timeout. */
static int lw_wait(volatile uint32_t *r, uint32_t mask, uint32_t want)
{
	int64_t t0 = now_ns();
	while ((*r & mask) != want)
		if (now_ns() - t0 > VIS_SPIN_NS) return 0;
	return 1;
}

static void vis_print(struct vis_set *v, double mhz)
{
	long done = v->n;
	printf("  %-33s", v->name);
	if (!done) { printf(" every trial timed out (%ld)\n", v->timeouts); return; }
	qsort(v->cyc, done, sizeof(*v->cyc), cmp_u32);
	qsort(v->miss, done, sizeof(*v->miss), cmp_u32);
	printf(" ns: min %5.0f  median %5.0f  p90 %5.0f  p99 %6.0f  max %7.0f | stale reads: median %u  p99 %u  max %u",
	       v->cyc[0] * 1000.0 / mhz, PCT(v->cyc, 0.5) * 1000.0 / mhz, PCT(v->cyc, 0.9) * 1000.0 / mhz,
	       PCT(v->cyc, 0.99) * 1000.0 / mhz, v->cyc[done - 1] * 1000.0 / mhz,
	       PCT(v->miss, 0.5), PCT(v->miss, 0.99), v->miss[done - 1]);
	if (v->timeouts) printf("  (%ld timed out)", v->timeouts);
	printf("\n");
}

/* -V. Bridge registers: write lw[4] A_GO, lw[5] B_GO, lw[6] B_SEEN; read
 * lw[4] {a_done, a_timeout, a_busy, .., a_misses}, lw[5] a_cycles,
 * lw[6] {b_done, b_wrote, b_pending, .., b_issue}, lw[7] b_cycles.
 * Values are kept to 29 bits so a stand-in on a plain file can tell an ARM
 * write from its own status (the real registers do not need it). */
#define A_DONE    0x80000000u
#define A_TIMEOUT 0x40000000u
#define A_BUSY    0x20000000u
#define B_DONE    0x80000000u
#define B_WROTE   0x40000000u

static int vis_mode(volatile uint32_t *lw, uint8_t *ddr, long count, int cpu)
{
	volatile uint32_t *word_a = (volatile uint32_t *)(ddr + VIS_A_OFF);
	volatile uint32_t *word_b = (volatile uint32_t *)(ddr + VIS_B_OFF);

	if (lw[1] != LW_MAGIC) {
		fprintf(stderr, "no lw_echo behind the bridge: register 1 reads %08x, not %08lx\n",
		        lw[1], (unsigned long)LW_MAGIC);
		return 1;
	}
	double mhz = lw_mhz(lw);
	if (mhz < 1) mhz = 113.5;   /* plain file: the cycles are whatever the stand-in says */

	static struct vis_set set[7] = {
		{ "A, word written 50 us earlier", 0, 0, NULL, NULL },
		{ "A, word written just before", 0, 0, NULL, NULL },
		{ "A, written and read back first", 0, 0, NULL, NULL },
		{ "B, B_SEEN without polling", 0, 0, NULL, NULL },
		{ "B, polled until visible", 0, 0, NULL, NULL },
		{ "B, one read of word A instead", 0, 0, NULL, NULL },
		{ "B, polled, then word B again", 0, 0, NULL, NULL },
	};
	for (int k = 0; k < 7; k++) {
		set[k].cyc  = malloc(sizeof(uint32_t) * count);
		set[k].miss = malloc(sizeof(uint32_t) * count);
		if (!set[k].cyc || !set[k].miss) { fprintf(stderr, "out of memory\n"); return 1; }
	}
	uint32_t *b_issue = malloc(sizeof(uint32_t) * count);
	if (!b_issue) { fprintf(stderr, "out of memory\n"); return 1; }
	long n_issue = 0;

	uint32_t v = (uint32_t)now_ns();
	int give_up = 0;
	for (long i = 0; i < count && !give_up; i++) {
		/* A: the ARM writes word A, the bridge says go, the FPGA polls it. */
		for (int k = 0; k < 3 && !give_up; k++) {
			struct vis_set *s = &set[k];
			v = (v * 1664525u + 1013904223u) & 0x1FFFFFFFu;
			*word_a = v;
			if (k == 0) {
				int64_t t = now_ns();
				while (now_ns() - t < 50000) ;
			}
			if (k == 2) (void)*word_a;
			__sync_synchronize();
			lw[4] = v;
			if (!lw_wait(&lw[4], A_DONE | A_TIMEOUT | A_BUSY, A_DONE) &&
			    !(lw[4] & A_TIMEOUT)) {
				fprintf(stderr, "visprobe never finished an A trial: is the core built with visprobe_ddr3.v?\n");
				give_up = 1;
				break;
			}
			uint32_t st = lw[4];
			if (st & A_TIMEOUT) { s->timeouts++; continue; }
			s->cyc[s->n]  = lw[5];
			s->miss[s->n] = st & 0xFFFF;
			s->n++;
		}

		/* B: the bridge says go, the FPGA writes word B, the ARM watches. */
		for (int k = 3; k < 7 && !give_up; k++) {
			struct vis_set *s = &set[k];
			v = (v * 1664525u + 1013904223u) & 0x1FFFFFFFu;
			lw[5] = v;
			if (!lw_wait(&lw[6], B_WROTE, B_WROTE)) {
				fprintf(stderr, "visprobe never wrote word B: is the core built with visprobe_ddr3.v?\n");
				give_up = 1;
				break;
			}
			uint32_t stale = 0;
			int seen = 1;
			if (k == 4 || k == 6) {
				int64_t t0 = now_ns();
				while (*word_b != v) {
					stale++;
					if (now_ns() - t0 > VIS_SPIN_NS) { seen = 0; break; }
				}
			}
			/* controls: is the stall that word's pending write, or any read? */
			if (k == 5 && *word_a == 0xFFFFFFFFu) stale++;
			if (k == 6 && *word_b != v) stale++;
			lw[6] = 0;
			if (!lw_wait(&lw[6], B_DONE, B_DONE) || !seen) { s->timeouts++; continue; }
			if (k == 3) b_issue[n_issue++] = lw[6] & 0xFFFF;
			s->cyc[s->n]  = lw[7];
			s->miss[s->n] = stale;
			s->n++;
		}
	}

	printf("latprobe -V: DDR3 write visibility, cpu %d, bridge clock %.1f MHz, %ld trials each\n", cpu, mhz, count);
	printf("ARM to FPGA: A_GO arriving to the FPGA's read returning the new word (stale reads: the FPGA's)\n");
	for (int k = 0; k < 3; k++) vis_print(&set[k], mhz);
	printf("FPGA to ARM: the FPGA's write accepted to B_SEEN arriving (stale reads: the ARM's)\n");
	for (int k = 3; k < 7; k++) vis_print(&set[k], mhz);
	if (n_issue) {
		long done = n_issue;
		qsort(b_issue, done, sizeof(*b_issue), cmp_u32);
		printf("  B_GO arriving to the write accepted: median %.0f ns  p99 %.0f ns\n",
		       PCT(b_issue, 0.5) * 1000.0 / mhz, PCT(b_issue, 0.99) * 1000.0 / mhz);
	}
	if (set[3].n && set[4].n) {
		long done = set[3].n;
		double m3 = PCT(set[3].cyc, 0.5);
		done = set[4].n;
		double m4 = PCT(set[4].cyc, 0.5);
		printf("  => the FPGA's write reaches the ARM ~%.0f ns after it is accepted, at the median (polled minus not)\n",
		       (m4 - m3) * 1000.0 / mhz);
	}
	if (set[3].n && set[5].n && set[6].n) {
		long done = set[3].n;
		double m3 = PCT(set[3].cyc, 0.5);
		done = set[4].n;
		double m4 = PCT(set[4].cyc, 0.5);
		done = set[5].n;
		double m5 = PCT(set[5].cyc, 0.5);
		done = set[6].n;
		double m6 = PCT(set[6].cyc, 0.5);
		printf("  controls: a read of another word costs ~%.0f ns there; a second read of word B ~%.0f ns\n",
		       (m5 - m3) * 1000.0 / mhz, (m6 - m4) * 1000.0 / mhz);
	}
	return give_up ? 1 : 0;
}

/* -P: the 68k's memory through the bridge is in the 68k's byte order, so a
 * halfword or word is byte-swapped on the ARM. ctl[] is armcpu_bridge.v's
 * control block: 0 MAGIC, 1 CTRL (bit 0 OWN), 2 BANK, 3 STATUS, 4 CYCLES,
 * 5 LAST, 6 WORDS. */
static volatile uint32_t *px_ctl;
static volatile uint8_t  *px_win;
static uint32_t px_bank = 0xFFFFFFFFu;

static inline void px_set_bank(uint32_t a)
{
	uint32_t b = a >> 20;
	if (b != px_bank) { px_ctl[2] = b; px_bank = b; }
}
static inline uint8_t  px_rd8(uint32_t a)  { px_set_bank(a); return px_win[a & 0xFFFFF]; }
static inline uint16_t px_rd16(uint32_t a) { px_set_bank(a); return __builtin_bswap16(*(volatile uint16_t *)(px_win + (a & 0xFFFFF))); }
static inline uint32_t px_rd32(uint32_t a) { px_set_bank(a); return __builtin_bswap32(*(volatile uint32_t *)(px_win + (a & 0xFFFFF))); }
static inline void px_wr8(uint32_t a, uint8_t v)   { px_set_bank(a); px_win[a & 0xFFFFF] = v; }
static inline void px_wr16(uint32_t a, uint16_t v) { px_set_bank(a); *(volatile uint16_t *)(px_win + (a & 0xFFFFF)) = __builtin_bswap16(v); }
static inline void px_wr32(uint32_t a, uint32_t v) { px_set_bank(a); *(volatile uint32_t *)(px_win + (a & 0xFFFFF)) = __builtin_bswap32(v); }

/* ns per access, median over batches, ARM clock; and the bridge's own count
 * of clk cycles for the last access (LAST) */
static double px_time(int kind, uint32_t a, long batches, uint32_t *last)
{
	double *per = malloc(sizeof(*per) * batches);
	uint32_t sink = 0;
	if (!per) return -1;
	for (long b = 0; b < batches; b++) {
		int64_t t0 = now_ns();
		for (int i = 0; i < 100; i++) {
			switch (kind) {
			case 0: sink += px_rd8(a + (i & 1)); break;
			case 1: sink += px_rd16(a); break;
			case 2: sink += px_rd32(a); break;
			case 3: px_wr8(a + (i & 1), (uint8_t)i); break;
			case 4: px_wr16(a, (uint16_t)i); break;
			case 5: px_wr32(a, (uint32_t)i); break;
			}
		}
		(void)px_ctl[0];      /* a write is posted: wait for the last one */
		per[b] = (double)(now_ns() - t0) / 100;
	}
	*last = px_ctl[5];
	for (long i = 1; i < batches; i++) {
		double v = per[i]; long j = i - 1;
		while (j >= 0 && per[j] > v) { per[j + 1] = per[j]; j--; }
		per[j + 1] = v;
	}
	double med = per[batches / 2];
	free(per);
	if (sink == 0xDEADBEEF) fprintf(stderr, " ");
	return med;
}

static int px_mode(volatile uint8_t *lw, long count, int cpu)
{
	px_ctl = (volatile uint32_t *)(lw + PX_CTL);
	px_win = lw + PX_WIN;
	int bad = 0;

	if (px_ctl[0] != PX_MAGIC) {
		fprintf(stderr, "no armcpu_bridge behind the bridge: control register 0 reads %08x, not %08lx\n",
		        px_ctl[0], (unsigned long)PX_MAGIC);
		return 1;
	}
	uint32_t st = px_ctl[3];
	if (!(st & 0x100)) {
		fprintf(stderr, "the CPU is not set to 68000 (STATUS %08x): the proxy only replaces the fx68k\n", st);
		return 1;
	}
	printf("latprobe -P: the Amiga's bus from the ARM, cpu %d; interrupt level before taking the bus: %u\n",
	       cpu, st & 7);

	px_ctl[1] = 1;
	if (!(px_ctl[1] & 1)) { fprintf(stderr, "OWN did not take\n"); return 1; }
	px_bank = 0xFFFFFFFFu;

	/* chip RAM: every size, both ways, and the byte order */
	uint32_t a = PX_TEST;
	for (uint32_t i = 0; i < 256; i++) px_wr8(a + i, (uint8_t)(i * 7 + 3));
	for (uint32_t i = 0; i < 256; i++) if (px_rd8(a + i) != (uint8_t)(i * 7 + 3)) bad++;
	for (uint32_t i = 0; i < 128; i++) if (px_rd16(a + 2 * i) != (uint16_t)(((i * 14 + 3) & 0xFF) << 8 | ((i * 14 + 10) & 0xFF))) bad++;
	px_wr32(a, 0x11223344u);
	if (px_rd16(a) != 0x1122 || px_rd16(a + 2) != 0x3344 || px_rd8(a) != 0x11 || px_rd8(a + 3) != 0x44) bad++;
	px_wr16(a + 4, 0xA55A);
	if (px_rd32(a + 4) >> 16 != 0xA55A) bad++;
	uint32_t v = (uint32_t)now_ns();
	for (long i = 0; i < count; i++) {
		v = v * 1664525u + 1013904223u;
		uint32_t at = a + 0x100 + ((v >> 8) & 0xFFC);
		px_wr32(at, v);
		if (px_rd32(at) != v) bad++;
	}
	printf("  chip RAM at $%06lx: every size written and read back, %ld random words: %d wrong\n",
	       (unsigned long)a, count, bad);

	/* the custom chips, live: VHPOSR moves with the beam */
	uint16_t h0 = px_rd16(0xDFF006), h1 = h0;
	int moved = 0;
	for (int i = 0; i < 100 && !moved; i++) { h1 = px_rd16(0xDFF006); moved = (h1 != h0); }
	printf("  VHPOSR $DFF006: %04x then %04x -- %s\n", h0, h1, moved ? "the beam is moving" : "NOT MOVING");
	printf("  DMACONR %04x  INTENAR %04x  INTREQR %04x  interrupt level now %u\n",
	       px_rd16(0xDFF002), px_rd16(0xDFF01C), px_rd16(0xDFF01E), px_ctl[3] & 7);

	/* time each kind on chip RAM */
	static const char *name[] = { "read byte", "read halfword", "read word", "write byte", "write halfword", "write word" };
	long batches = count / 100;
	if (batches < 11) batches = 11;
	/* the bridge's clock, from its CYCLES against ours (clk_sys in the core) */
	uint32_t c0 = px_ctl[4];
	int64_t t0 = now_ns();
	usleep(50000);
	double mhz = (px_ctl[4] - c0) * 1000.0 / (double)(now_ns() - t0);
	uint32_t w0 = px_ctl[6];
	for (int fast = 0; fast < 2; fast++) {
		px_ctl[1] = fast ? 3 : 1;
		if ((px_ctl[1] & 3) != (fast ? 3u : 1u)) { fprintf(stderr, "CTRL did not take FAST=%d\n", fast); return 1; }
		printf("  per access, chip RAM, %s, median of %ld batches of 100 (ARM clock) | last one by the bridge's clock:\n",
		       fast ? "FAST (reads through the SDRAM port)" : "68000 timing", batches);
		for (int k = 0; k < 6; k++) {
			uint32_t last;
			double ns = px_time(k, a + 0x2000, batches, &last);
			if (mhz > 1)
				printf("    %-15s %7.1f ns | %u cycles (%.0f ns at %.1f MHz)\n", name[k], ns, last, last * 1000.0 / mhz, mhz);
			else
				printf("    %-15s %7.1f ns | %u cycles (CYCLES not counting)\n", name[k], ns, last);
		}
	}
	printf("  68k word cycles made: %u\n", px_ctl[6] - w0);

	/* FAST reads come from the CPU port's cache: it must see what the ARM
	 * wrote over the chip bus, and what the blitter wrote behind its back */
	int stale_w = 0, stale_b = 0, blit_ok = 1;
	uint32_t r = PX_TEST + 0x1000;
	for (uint32_t i = 0; i < 256; i++) px_wr16(r + 2 * i, 0xA5A5);
	for (uint32_t i = 0; i < 256; i++) if (px_rd16(r + 2 * i) != 0xA5A5) stale_w++;
	for (uint32_t i = 0; i < 256; i++) px_wr16(r + 2 * i, (uint16_t)(0x5A00 + i));
	for (uint32_t i = 0; i < 256; i++) if (px_rd16(r + 2 * i) != (uint16_t)(0x5A00 + i)) stale_w++;
	px_wr16(0xDFF096, 0x8240);            /* DMACON: DMAEN | BLTEN */
	px_wr16(0xDFF040, 0x0100);            /* BLTCON0: D only, minterm 0: clear */
	px_wr16(0xDFF042, 0x0000);
	px_wr16(0xDFF066, 0x0000);            /* BLTDMOD */
	px_wr32(0xDFF054, r);                 /* BLTDPT */
	px_wr16(0xDFF058, (16 << 6) | 16);    /* BLTSIZE: 16 lines of 16 words */
	(void)px_rd16(0xDFF002);
	int64_t tb = now_ns();
	while (px_rd16(0xDFF002) & 0x4000)    /* BBUSY */
		if (now_ns() - tb > 100000000LL) { blit_ok = 0; break; }
	for (uint32_t i = 0; i < 256; i++) if (px_rd16(r + 2 * i) != 0) stale_b++;
	printf("  FAST reads after ARM writes: %d of 512 stale; after a blitter clear%s: %d of 256 stale\n",
	       stale_w, blit_ok ? "" : " (BBUSY never cleared)", stale_b);
	if (stale_w || stale_b || !blit_ok) bad++;

	px_ctl[1] = 0;
	printf("  bus given back (OWN %u): the 68000 restarts from its reset vector\n", px_ctl[1] & 1);
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
	int cpu = 0, opt, raw = 0, sync = 1, lw = 0, vis = 0, prox = 0, force = 0;
	const char *dev = NULL;

	while ((opt = getopt(argc, argv, "n:c:g:d:rSBFLVPfh")) != -1) {
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
		case 'V': vis = 1; break;
		case 'P': prox = 1; break;
		case 'f': force = 1; break;
		default:
			fprintf(stderr, "usage: %s [-n count] [-c cpu] [-g gap_us] [-r] [-S] [-B] [-F] [-L|-V|-P [-f]]\n", argv[0]);
			return 2;
		}
	}
	if (count < 1) count = 1;

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		fprintf(stderr, "warning: cannot pin to cpu %d: %s\n", cpu, strerror(errno));

	if (lw || vis || prox) {
		if (!dev && !force && !lw_core_loaded()) {
			fprintf(stderr, "-L/-V/-P: no running core has \"lwbridge\" in its path. Without lw_echo.sv behind the "
			                "bridge an access can hang the ARM, so -L, -V and -P only run on that core (-f overrides).\n");
			return 2;
		}
		int lfd = open(dev ? dev : "/dev/mem", O_RDWR | O_SYNC);
		if (lfd < 0) { perror(dev ? dev : "/dev/mem"); return 1; }
		if (prox) {
			void *pb = mmap(NULL, PX_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, lfd, dev ? 0 : LW_BASE);
			if (pb == MAP_FAILED) { perror("mmap"); return 1; }
			return px_mode((volatile uint8_t *)pb, count, cpu);
		}
		void *lb = mmap(NULL, LW_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, lfd, dev ? 0 : LW_BASE);
		if (lb == MAP_FAILED) { perror("mmap"); return 1; }
		if (lw) return lw_mode((volatile uint32_t *)lb, count, cpu);

		int dfd = dev ? lfd : open("/dev/mem", O_RDWR | O_SYNC);
		if (dfd < 0) { perror("/dev/mem"); return 1; }
		uint8_t *ddr = mmap(NULL, DDR3_WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, dfd,
		                    dev ? LW_WORDS_OFF : DDR3_FLAT_BASE);
		if (ddr == MAP_FAILED) { perror("mmap"); return 1; }
		return vis_mode((volatile uint32_t *)lb, ddr, count, cpu);
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
