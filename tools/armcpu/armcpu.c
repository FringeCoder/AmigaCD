/*
 * armcpu -- a 68000 on the ARM, driving the Amiga's bus through the FPGA.
 *
 * MEASUREMENT ONLY, step 3 of docs/arm-cpu-plan.md. The core's bus proxy
 * (AmigaCD_MiSTer claude/lwbridge-echo: rtl/armcpu_bridge.v and
 * rtl/proxy68k.v, behind the lightweight HPS-to-FPGA bridge) makes 68000
 * bus cycles for whoever owns it. This owns it: Musashi (tools/armcpu/
 * musashi, MIT) runs the 68000, Kickstart and 8 MB of fast RAM sit in ARM
 * memory, and every other access -- chip RAM, the custom chips, the CIAs,
 * the core's own autoconfig boards, slow RAM -- goes over the bridge.
 *
 *   armcpu [-r rom] [-t seconds] [-m mode] [-c cpu] [-F MB] [-q] [-d file] [-f]
 *     -r  Kickstart image (default /media/fat/games/Amiga500HD/
 *         1.3-scsi-patched.rom); 256 KB is mirrored, 512 KB fills
 *         $F80000-$FFFFFF
 *     -t  seconds to run before giving the bus back (default 20; 0 = until
 *         Ctrl-C)
 *     -m  armcpu_bridge CTRL bits besides OWN: 2 FAST reads, 4 FASTW writes
 *         (default 6, both)
 *     -c  pin to this ARM core (default 0; Main_MiSTer runs on 1)
 *     -F  fast RAM in ARM memory, in MB: 0, 1, 2, 4 or 8 (default 8)
 *     -q  no progress lines
 *     -d  host test: map this file as the bridge (MAGIC and the 68000 bit
 *         already in it); test_armcpu.py checks what lands in it
 *     -f  run without the "lwbridge" core check (only on a core you know has
 *         the proxy: with nothing behind 0xFF200000 an access hangs the ARM)
 *     -w  warm start: leave chip RAM as it is. By default the ExecBase pointer
 *         at $4 is cleared first, so Kickstart cold-starts instead of trusting
 *         whatever the previous CPU left in RAM (its capture vectors and
 *         resident list point into code that is no longer there)
 *     -D  debug: keep the last 8192 instructions (Musashi's instruction hook,
 *         so the timing stays as it is without -D) and every CIA-A write,
 *         RESET and interrupt level change, and dump them disassembled when a
 *         slice ends with the PC in low memory with the overlay on or the
 *         stack pointer outside RAM, or at the end (code below $400 is not a
 *         fault: a disk's trackloader lives there)
 *
 * The bridge's 1 MB window is in the 68k's byte order, so a halfword or word
 * is byte-swapped here (latprobe -P does the same). The emulator follows the
 * ROM overlay itself -- CIA-A PRA bit 0 through DDRA, as Gary does -- and
 * reads $000000-$07FFFF from its own copy of the ROM while it is on, so the
 * reset vectors never depend on the bus. Interrupts: Paula's level is read
 * from armcpu_bridge STATUS every slice of 500 68000 cycles and handed to
 * Musashi again each time, which autovectors (Musashi drops a level once it
 * has taken it; the Amiga's stay up until the handler clears them). RESET pulls the CPU's reset output for the
 * instruction's 124 clocks (CTRL bit 3), which resets the chipset and CIAs.
 *
 * Fast RAM: an emulated Zorro II memory board, first in the autoconfig
 * chain. Until Kickstart configures it, this answers $E80000-$E8FFFF itself
 * with the board's ROM nibbles (inverted where the spec says); the base
 * Kickstart writes to $4A and then $48 maps the board, and a write to $4C
 * shuts it up. Either way the board leaves the chain and $E8xxxx goes to the
 * bus again, where the core's own boards configure as before. Accesses inside
 * the board never touch the bus. RESET unconfigures it, as on a real board.
 *
 * Taking the bus holds the fx68k in reset; giving it back restarts the
 * machine from its own reset vector.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "m68k.h"

#define LW_BASE     0xFF200000UL   /* the lightweight bridge */
#define LW_SPAN     0x200000UL
#define PX_CTL      0x080000UL     /* armcpu_bridge control */
#define PX_WIN      0x100000UL     /* the 68k window, 1 MB per BANK */
#define PX_MAGIC    0x41435055UL   /* "ACPU" */

#define CTRL_OWN    1u
#define CTRL_RESET  8u

#define SLICE       500            /* 68000 cycles between interrupt polls */

static volatile uint32_t *ctl;
static volatile uint8_t  *win;
static uint32_t bank_cur = 0xFFFFFFFFu;
static uint32_t ctrl_bits;         /* OWN plus the mode bits */

static uint8_t  rom[0x80000];      /* $F80000-$FFFFFF */
static int      ovl = 1;           /* ROM overlay on $000000 (after reset) */
static uint8_t  cia_pra, cia_ddra;

static uint64_t n_rom, n_bus_rd, n_bus_wr;

/* -D: what led up to the PC leaving the ROM */
#define RING 8192
static int debug;
static struct { uint32_t pc, sr, a7; uint8_t ovl, ipl; } ring[RING];
static unsigned ring_n;
static int level;

void armcpu_instr_hook(unsigned int pc)
{
	if (!debug) return;
	unsigned k = ring_n++ % RING;
	ring[k].pc  = pc & 0xFFFFFF;
	ring[k].sr  = m68k_get_reg(NULL, M68K_REG_SR);
	ring[k].a7  = m68k_get_reg(NULL, M68K_REG_A7);
	ring[k].ovl = ovl;
	ring[k].ipl = level;
}
static char events[64][96];
static unsigned n_events;
static void event(const char *fmt, uint32_t a, uint32_t b)
{
	if (!debug) return;
	snprintf(events[n_events % 64], sizeof(events[0]), fmt, a, b);
	n_events++;
}
static volatile sig_atomic_t stop;

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- the bus ---------------------------------------------------------------- */

static inline void set_bank(uint32_t a)
{
	uint32_t b = a >> 20;
	if (b != bank_cur) { ctl[2] = b; bank_cur = b; }
}
static inline uint8_t  bus_rd8(uint32_t a)  { set_bank(a); n_bus_rd++; return win[a & 0xFFFFF]; }
static inline uint16_t bus_rd16(uint32_t a) { set_bank(a); n_bus_rd++; return __builtin_bswap16(*(volatile uint16_t *)(win + (a & 0xFFFFF))); }
static inline void bus_wr8(uint32_t a, uint8_t v)   { set_bank(a); n_bus_wr++; win[a & 0xFFFFF] = v; }
static inline void bus_wr16(uint32_t a, uint16_t v) { set_bank(a); n_bus_wr++; *(volatile uint16_t *)(win + (a & 0xFFFFF)) = __builtin_bswap16(v); }

/* CIA-A PRA bit 0 is OVL; with DDRA bit 0 an input the line is pulled up */
static inline void cia_track(uint32_t a, uint8_t v)
{
	if ((a & 0xFF0000) != 0xBF0000 || (a & 0x1001) != 0x0001) return;   /* CIA-A, odd byte */
	switch ((a >> 8) & 15) {
	case 0: cia_pra  = v; break;
	case 2: cia_ddra = v; break;
	default: return;
	}
	ovl = !((cia_ddra & 1) && !(cia_pra & 1));
	event("CIA-A write %06x = %02x", a, v);
}

static inline int is_rom(uint32_t a)
{
	return a >= 0xF80000 || (ovl && a < 0x080000);
}

/* ---- fast RAM: a Zorro II board in ARM memory ------------------------------------ */

static uint8_t *fast;                    /* the board's memory, 68k byte order */
static uint32_t fast_size;               /* 0: no board */
static uint32_t fast_base = 0xFFFFFFFFu; /* where autoconfig put it */
static int      ac_mine;                 /* the board is the one at $E80000 */
static uint8_t  ac_lo;                   /* the nibble written to $4A */
static uint64_t n_fast;

/* er_Type (Zorro II, memory into the free list, size), er_Product, er_Flags
 * (prefers the 8 MB space), reserved, manufacturer $07DB (the "hackers" ID),
 * serial and the diagnostic vector all 0 */
static uint8_t ac_rom[16] = { 0xE0, 0x01, 0x80, 0x00, 0x07, 0xDB };

static uint32_t fast_last = 0xFFFFFFFFu; /* where it was last configured */

static inline int is_fast(uint32_t a)
{
	return fast_base != 0xFFFFFFFFu && a - fast_base < fast_size;
}

static void fast_reset(void)
{
	ac_mine   = fast_size != 0;
	fast_base = 0xFFFFFFFFu;
}

/* a byte read of the board's config space: each byte register is two nibbles
 * at $E8xxx0/2, high nibble first, in bits 7-4; all but er_Type and the
 * interrupt registers ($40 up) read inverted */
static uint8_t ac_read(uint32_t a)
{
	uint32_t o = a & 0xFF;
	if (o & 1) return 0xFF;
	if (o >= 0x40) return 0x00;
	uint8_t r = ac_rom[o >> 2];
	uint8_t n = (o & 2) ? (r & 0x0F) : (r >> 4);
	if (o >= 0x04) n = ~n & 0x0F;
	return n << 4;
}

static void ac_write(uint32_t a, uint8_t v)
{
	switch (a & 0xFF) {
	case 0x4A: ac_lo = v >> 4; break;
	case 0x48:
		fast_base = ((uint32_t)(v >> 4) << 20) | ((uint32_t)ac_lo << 16);
		fast_last = fast_base;
		ac_mine = 0;
		event("fast RAM configured at %06x (%x)", fast_base, 0);
		break;
	case 0x4C:
		ac_mine = 0;
		event("fast RAM shut up (%x %x)", 0, 0);
		break;
	}
}

static inline int is_ac(uint32_t a)
{
	return ac_mine && (a & 0xFF0000) == 0xE80000;
}

/* ---- chip RAM size ---------------------------------------------------------------
 * FAST and FASTW reach chip RAM through the SDRAM CPU port, which answers all
 * of $000000-$1FFFFF; the chip bus wraps at the machine's real chip size, as
 * Agnus does, and Kickstart sizes chip RAM by looking for that wrap. Without
 * it Kickstart finds 2 MB and puts screens where Agnus cannot fetch. So the
 * size is measured once over the chip bus (68000 timing), and chip addresses
 * are wrapped to it here, whichever path they then take. */
static uint32_t chip_mask = 0x1FFFFF;

static void armcpu_reset_instr(void);

static uint32_t chip_probe(void)
{
	const uint32_t t = 0x00F000;           /* clear of $0-$107, which exec reads at boot */
	ctl[1] = CTRL_OWN;                     /* the chip bus, no FAST */
	bus_wr8(0xBFE201, 0x03);               /* CIA-A DDRA, then PRA: the overlay off, */
	bus_wr8(0xBFE001, 0x00);               /* as Kickstart does before it sizes */
	for (int k = 3; k >= 1; k--) bus_wr16(t + k * 0x80000, (uint16_t)(0xA500 + k));
	bus_wr16(t, 0x5AA5);
	uint32_t size = 0x200000;
	for (int k = 1; k < 4; k++)
		if (bus_rd16(t + k * 0x80000) != (uint16_t)(0xA500 + k)) { size = k * 0x80000; break; }
	if (size == 0x180000) size = 0x200000;  /* not a size chip RAM comes in */
	ctl[1] = ctrl_bits;
	armcpu_reset_instr();                  /* the CIAs, and the overlay, as reset left them */
	return size;
}

/* ---- Musashi's memory interface ---------------------------------------------- */

unsigned int m68k_read_memory_8(unsigned int a)
{
	a &= 0xFFFFFF;
	if (is_rom(a)) { n_rom++; return rom[a & 0x7FFFF]; }
	if (is_fast(a)) { n_fast++; return fast[a - fast_base]; }
	if (is_ac(a)) return ac_read(a);
	if (a < 0x200000) a &= chip_mask;
	return bus_rd8(a);
}

unsigned int m68k_read_memory_16(unsigned int a)
{
	a &= 0xFFFFFF;
	if (is_rom(a)) { n_rom++; return (rom[a & 0x7FFFF] << 8) | rom[(a + 1) & 0x7FFFF]; }
	if (is_fast(a)) { n_fast++; return (fast[a - fast_base] << 8) | fast[a - fast_base + 1]; }
	if (is_ac(a)) return (ac_read(a) << 8) | ac_read(a + 1);
	if (a < 0x200000) a &= chip_mask;
	return bus_rd16(a);
}

unsigned int m68k_read_memory_32(unsigned int a)
{
	a &= 0xFFFFFF;
	/* two word cycles either way, as on a 68000; a pair is one bridge read only
	 * when both halves are in one aligned word of the same region */
	return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16((a + 2) & 0xFFFFFF);
}

void m68k_write_memory_8(unsigned int a, unsigned int v)
{
	a &= 0xFFFFFF;
	if (a >= 0xF80000) return;                    /* ROM */
	if (is_fast(a)) { n_fast++; fast[a - fast_base] = (uint8_t)v; return; }
	if (is_ac(a)) { ac_write(a, (uint8_t)v); return; }
	cia_track(a, (uint8_t)v);
	if (a < 0x200000) a &= chip_mask;
	bus_wr8(a, (uint8_t)v);
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
	a &= 0xFFFFFF;
	if (a >= 0xF80000) return;
	if (is_fast(a)) { n_fast++; fast[a - fast_base] = (uint8_t)(v >> 8); fast[a - fast_base + 1] = (uint8_t)v; return; }
	if (is_ac(a)) { ac_write(a, (uint8_t)(v >> 8)); return; }   /* the even byte */
	cia_track(a + 1, (uint8_t)v);                 /* a word write: CIA-A on the low byte */
	if (a < 0x200000) a &= chip_mask;
	bus_wr16(a, (uint16_t)v);
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
	m68k_write_memory_16(a, v >> 16);
	m68k_write_memory_16((a + 2) & 0xFFFFFF, v & 0xFFFF);
}

unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* RESET: the CPU's reset output low for 124 68000 clocks (~17.5 us) */
static void armcpu_reset_instr(void)
{
	event("RESET at PC %06x (%x)", m68k_get_reg(NULL, M68K_REG_PPC) & 0xFFFFFF, 0);
	ctl[1] = ctrl_bits | CTRL_RESET;
	int64_t t0 = now_ns();
	while (now_ns() - t0 < 20000) ;
	ctl[1] = ctrl_bits;
	cia_pra = cia_ddra = 0;
	ovl = 1;
	bank_cur = 0xFFFFFFFFu;
	fast_reset();
}

/* ---- the run -------------------------------------------------------------------- */

static void on_signal(int s) { (void)s; stop = 1; }

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

int main(int argc, char **argv)
{
	const char *rom_path = "/media/fat/games/Amiga500HD/1.3-scsi-patched.rom";
	const char *dev = NULL;
	double seconds = 20;
	int cpu = 0, quiet = 0, force = 0, warm = 0, fast_mb = 8, opt;
	unsigned mode = 6;

	while ((opt = getopt(argc, argv, "r:t:m:c:F:d:qfwDh")) != -1) {
		switch (opt) {
		case 'r': rom_path = optarg; break;
		case 't': seconds = atof(optarg); break;
		case 'm': mode = (unsigned)strtoul(optarg, NULL, 0) & 6; break;
		case 'c': cpu = atoi(optarg); break;
		case 'd': dev = optarg; break;
		case 'q': quiet = 1; break;
		case 'f': force = 1; break;
		case 'D': debug = 1; break;
		case 'w': warm = 1; break;
		case 'F': fast_mb = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-r rom] [-t seconds] [-m mode] [-c cpu] [-F MB] [-q] [-d file] [-f] [-w] [-D]\n", argv[0]);
			return 2;
		}
	}

	/* Zorro II sizes: 1, 2, 4 MB are codes 5, 6, 7 and 8 MB is code 0 */
	switch (fast_mb) {
	case 0: break;
	case 1: ac_rom[0] = 0xE5; break;
	case 2: ac_rom[0] = 0xE6; break;
	case 4: ac_rom[0] = 0xE7; break;
	case 8: ac_rom[0] = 0xE0; break;
	default: fprintf(stderr, "-F: 0, 1, 2, 4 or 8 MB\n"); return 2;
	}
	fast_size = (uint32_t)fast_mb << 20;
	if (fast_size && !(fast = calloc(1, fast_size))) { fprintf(stderr, "out of memory for fast RAM\n"); return 1; }

	FILE *rf = fopen(rom_path, "rb");
	if (!rf) { perror(rom_path); return 1; }
	size_t got = fread(rom, 1, sizeof(rom), rf);
	fclose(rf);
	if (got == 0x40000) memcpy(rom + 0x40000, rom, 0x40000);
	else if (got != 0x80000) { fprintf(stderr, "%s: %zu bytes, want 256 or 512 KB\n", rom_path, got); return 1; }

	if (!dev && !force && !lw_core_loaded()) {
		fprintf(stderr, "no running core has \"lwbridge\" in its path: without the proxy behind the bridge "
		                "an access can hang the ARM (-f overrides)\n");
		return 2;
	}

	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		fprintf(stderr, "warning: cannot pin to cpu %d: %s\n", cpu, strerror(errno));

	int fd = open(dev ? dev : "/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror(dev ? dev : "/dev/mem"); return 1; }
	volatile uint8_t *lw = mmap(NULL, LW_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, dev ? 0 : LW_BASE);
	if (lw == MAP_FAILED) { perror("mmap"); return 1; }
	ctl = (volatile uint32_t *)(lw + PX_CTL);
	win = lw + PX_WIN;

	if (ctl[0] != PX_MAGIC) {
		fprintf(stderr, "no armcpu_bridge behind the bridge: control register 0 reads %08x\n", ctl[0]);
		return 1;
	}
	if (!(ctl[3] & 0x100)) {
		fprintf(stderr, "the CPU is not set to 68000: the proxy only replaces the fx68k\n");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	ctrl_bits = CTRL_OWN | mode;
	ctl[1] = ctrl_bits;
	if ((ctl[1] & 7) != ctrl_bits) { fprintf(stderr, "CTRL did not take %u\n", ctrl_bits); return 1; }

	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_set_reset_instr_callback(armcpu_reset_instr);
	armcpu_reset_instr();             /* a clean machine: chipset and CIAs reset, overlay on */
	uint32_t chip_size = chip_probe();
	chip_mask = chip_size - 1;
	/* a cold start: no ExecBase at $4, and no 'HELP' at $0 with a crash at
	 * $100 -- exec shows that as the last alert before the reset */
	if (!warm) {
		for (uint32_t a = 0; a < 8; a += 2) bus_wr16(a, 0);
		for (uint32_t a = 0x100; a < 0x108; a += 2) bus_wr16(a, 0);
	}
	m68k_pulse_reset();

	printf("armcpu: Musashi 68000 on cpu %d, %s, mode %u (%s%s), ROM %s\n", cpu,
	       dev ? dev : "the bridge", mode, mode & 2 ? "FAST" : "68000-timed reads",
	       mode & 4 ? ", FASTW" : ", 68000-timed writes", rom_path);
	printf("  reset: SSP %08x PC %08x; chip RAM %u KB (measured on the chip bus); fast RAM %d MB in ARM memory\n",
	       m68k_get_reg(NULL, M68K_REG_SP), m68k_get_reg(NULL, M68K_REG_PC), chip_size >> 10, fast_mb);

	int64_t t_start = now_ns(), t_report = t_start;
	uint64_t cycles = 0, cyc_report = 0, rd0 = 0, wr0 = 0, rom0 = 0, fast0 = 0;
	int dumped = 0;
	while (!stop) {
		/* Paula's level, handed over every slice: the Amiga's interrupts are
		 * level-triggered, but Musashi without an acknowledge callback clears
		 * its pending level when it takes the interrupt, so a level that
		 * stays up has to be given again or it is taken only once */
		int lv = ctl[3] & 7;
		if (lv != level) { level = lv; event("IPL now %d (%x)", level, 0); }
		m68k_set_irq(level);
		cycles += m68k_execute(SLICE);
		if (debug) {
			uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC) & 0xFFFFFF;
			uint32_t sp = m68k_get_reg(NULL, M68K_REG_A7) & 0xFFFFFF;
			if ((ovl && pc < 0x80000) || sp < 0x400 ||
			    (sp > 0x200000 && sp < 0xC00000 && !is_fast(sp - 1)) || sp > 0xD80000) { dumped = 1; break; }
		}

		int64_t t = now_ns();
		if (seconds > 0 && t - t_start >= (int64_t)(seconds * 1e9)) break;
		if (t - t_report >= 1000000000LL) {
			double dt = (t - t_report) / 1e9;
			if (!quiet)
				printf("  %5.1f s: %6.2f MHz of 68000, bus %7.0f reads/s %7.0f writes/s, ROM %8.0f/s, fast %8.0f/s, PC %06x SR %04x IPL %d\n",
				       (t - t_start) / 1e9, (cycles - cyc_report) / dt / 1e6,
				       (n_bus_rd - rd0) / dt, (n_bus_wr - wr0) / dt, (n_rom - rom0) / dt, (n_fast - fast0) / dt,
				       m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR), level);
			fflush(stdout);
			t_report = t; cyc_report = cycles; rd0 = n_bus_rd; wr0 = n_bus_wr; rom0 = n_rom; fast0 = n_fast;
		}
	}
	if (debug) {
		char buf[100];
		printf("debug: %s; last %u of %u instructions (PC SR A7 OVL IPL), newest last:\n",
		       dumped ? "the PC or stack went where Kickstart never takes them" : "ran out of time",
		       ring_n < RING ? ring_n : RING, ring_n);
		for (unsigned i = ring_n > RING ? ring_n - RING : 0; i < ring_n; i++) {
			unsigned k = i % RING;
			m68k_disassemble(buf, ring[k].pc, M68K_CPU_TYPE_68000);
			printf("  %06x %04x %08x %d %d  %s\n", ring[k].pc, ring[k].sr, ring[k].a7, ring[k].ovl, ring[k].ipl, buf);
		}
		printf("  now: PC %06x SR %04x A7 %08x OVL %d\n", m68k_get_reg(NULL, M68K_REG_PC),
		       m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_A7), ovl);
		printf("debug: last %u of %u events:\n", n_events < 64 ? n_events : 64, n_events);
		for (unsigned i = n_events > 64 ? n_events - 64 : 0; i < n_events; i++) printf("  %s\n", events[i % 64]);
	}
	/* exec's memory list, as the 68000 sees it: ExecBase at $4, MemList at
	 * +$142, each MemHeader with its attributes, bounds and free bytes */
	{
		uint32_t eb = m68k_read_memory_32(4), node = eb ? m68k_read_memory_32(eb + 0x142) : 0;
		printf("  exec memory list (ExecBase %06x):\n", eb);
		for (int i = 0; i < 8 && node && m68k_read_memory_32(node); i++) {
			printf("    %06x attr %04x pri %4d  %06x-%06x  free %7u\n", node,
			       m68k_read_memory_16(node + 0x0E), (int8_t)m68k_read_memory_8(node + 0x09),
			       m68k_read_memory_32(node + 0x14), m68k_read_memory_32(node + 0x18),
			       m68k_read_memory_32(node + 0x1C));
			node = m68k_read_memory_32(node);
		}
	}
	double total = (now_ns() - t_start) / 1e9;
	printf("  ran %.1f s: %llu 68000 cycles (%.2f MHz), %llu bus reads, %llu bus writes, %llu ROM reads, "
	       "%llu fast RAM accesses (board last at %06x); PC %06x\n",
	       total, (unsigned long long)cycles, cycles / total / 1e6, (unsigned long long)n_bus_rd,
	       (unsigned long long)n_bus_wr, (unsigned long long)n_rom, (unsigned long long)n_fast,
	       fast_last, m68k_get_reg(NULL, M68K_REG_PC));

	ctl[1] = 0;
	printf("  bus given back: the 68000 restarts from its reset vector\n");
	return 0;
}
