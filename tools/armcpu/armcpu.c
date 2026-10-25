/*
 * armcpu -- a 68000 on the ARM, driving the Amiga's bus through the FPGA.
 *
 * MEASUREMENT ONLY, step 3 of docs/arm-cpu-plan.md. The core's bus proxy
 * (AmigaCD_MiSTer claude/lwbridge-echo: rtl/armcpu_bridge.v and
 * rtl/proxy68k.v, behind the lightweight HPS-to-FPGA bridge) makes 68000
 * bus cycles for whoever owns it. This owns it: Musashi (tools/armcpu/
 * musashi, MIT) runs the 68000, Kickstart sits in ARM memory, and every
 * other access -- chip RAM, the custom chips, the CIAs, autoconfig, slow and
 * fast RAM in the core -- goes over the bridge.
 *
 *   armcpu [-r rom] [-t seconds] [-m mode] [-c cpu] [-q] [-d file] [-f]
 *     -r  Kickstart image (default /media/fat/games/Amiga500HD/
 *         1.3-scsi-patched.rom); 256 KB is mirrored, 512 KB fills
 *         $F80000-$FFFFFF
 *     -t  seconds to run before giving the bus back (default 20; 0 = until
 *         Ctrl-C)
 *     -m  armcpu_bridge CTRL bits besides OWN: 2 FAST reads, 4 FASTW writes
 *         (default 6, both)
 *     -c  pin to this ARM core (default 0; Main_MiSTer runs on 1)
 *     -q  no progress lines
 *     -d  host test: map this file as the bridge (MAGIC and the 68000 bit
 *         already in it); test_armcpu.py checks what lands in it
 *     -f  run without the "lwbridge" core check (only on a core you know has
 *         the proxy: with nothing behind 0xFF200000 an access hangs the ARM)
 *
 * The bridge's 1 MB window is in the 68k's byte order, so a halfword or word
 * is byte-swapped here (latprobe -P does the same). The emulator follows the
 * ROM overlay itself -- CIA-A PRA bit 0 through DDRA, as Gary does -- and
 * reads $000000-$07FFFF from its own copy of the ROM while it is on, so the
 * reset vectors never depend on the bus. Interrupts: Paula's level is read
 * from armcpu_bridge STATUS every slice of 68000 cycles and handed to
 * Musashi, which autovectors. RESET pulls the CPU's reset output for the
 * instruction's 124 clocks (CTRL bit 3), which resets the chipset and CIAs.
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

#define SLICE       2000           /* 68000 cycles between interrupt polls */

static volatile uint32_t *ctl;
static volatile uint8_t  *win;
static uint32_t bank_cur = 0xFFFFFFFFu;
static uint32_t ctrl_bits;         /* OWN plus the mode bits */

static uint8_t  rom[0x80000];      /* $F80000-$FFFFFF */
static int      ovl = 1;           /* ROM overlay on $000000 (after reset) */
static uint8_t  cia_pra, cia_ddra;

static uint64_t n_rom, n_bus_rd, n_bus_wr;
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
}

static inline int is_rom(uint32_t a)
{
	return a >= 0xF80000 || (ovl && a < 0x080000);
}

/* ---- Musashi's memory interface ---------------------------------------------- */

unsigned int m68k_read_memory_8(unsigned int a)
{
	a &= 0xFFFFFF;
	if (is_rom(a)) { n_rom++; return rom[a & 0x7FFFF]; }
	return bus_rd8(a);
}

unsigned int m68k_read_memory_16(unsigned int a)
{
	a &= 0xFFFFFF;
	if (is_rom(a)) { n_rom++; return (rom[a & 0x7FFFF] << 8) | rom[(a + 1) & 0x7FFFF]; }
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
	cia_track(a, (uint8_t)v);
	bus_wr8(a, (uint8_t)v);
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
	a &= 0xFFFFFF;
	if (a >= 0xF80000) return;
	cia_track(a + 1, (uint8_t)v);                 /* a word write: CIA-A on the low byte */
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
	ctl[1] = ctrl_bits | CTRL_RESET;
	int64_t t0 = now_ns();
	while (now_ns() - t0 < 20000) ;
	ctl[1] = ctrl_bits;
	cia_pra = cia_ddra = 0;
	ovl = 1;
	bank_cur = 0xFFFFFFFFu;
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
	int cpu = 0, quiet = 0, force = 0, opt;
	unsigned mode = 6;

	while ((opt = getopt(argc, argv, "r:t:m:c:d:qfh")) != -1) {
		switch (opt) {
		case 'r': rom_path = optarg; break;
		case 't': seconds = atof(optarg); break;
		case 'm': mode = (unsigned)strtoul(optarg, NULL, 0) & 6; break;
		case 'c': cpu = atoi(optarg); break;
		case 'd': dev = optarg; break;
		case 'q': quiet = 1; break;
		case 'f': force = 1; break;
		default:
			fprintf(stderr, "usage: %s [-r rom] [-t seconds] [-m mode] [-c cpu] [-q] [-d file] [-f]\n", argv[0]);
			return 2;
		}
	}

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
	m68k_pulse_reset();

	printf("armcpu: Musashi 68000 on cpu %d, %s, mode %u (%s%s), ROM %s\n", cpu,
	       dev ? dev : "the bridge", mode, mode & 2 ? "FAST" : "68000-timed reads",
	       mode & 4 ? ", FASTW" : ", 68000-timed writes", rom_path);
	printf("  reset: SSP %08x PC %08x\n", m68k_get_reg(NULL, M68K_REG_SP), m68k_get_reg(NULL, M68K_REG_PC));

	int64_t t_start = now_ns(), t_report = t_start;
	uint64_t cycles = 0, cyc_report = 0, rd0 = 0, wr0 = 0, rom0 = 0;
	int level = 0;
	while (!stop) {
		int lv = ctl[3] & 7;
		if (lv != level) { level = lv; m68k_set_irq(level); }
		cycles += m68k_execute(SLICE);

		int64_t t = now_ns();
		if (seconds > 0 && t - t_start >= (int64_t)(seconds * 1e9)) break;
		if (t - t_report >= 1000000000LL) {
			double dt = (t - t_report) / 1e9;
			if (!quiet)
				printf("  %5.1f s: %6.2f MHz of 68000, bus %7.0f reads/s %7.0f writes/s, ROM %8.0f/s, PC %06x SR %04x IPL %d\n",
				       (t - t_start) / 1e9, (cycles - cyc_report) / dt / 1e6,
				       (n_bus_rd - rd0) / dt, (n_bus_wr - wr0) / dt, (n_rom - rom0) / dt,
				       m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR), level);
			fflush(stdout);
			t_report = t; cyc_report = cycles; rd0 = n_bus_rd; wr0 = n_bus_wr; rom0 = n_rom;
		}
	}
	double total = (now_ns() - t_start) / 1e9;
	printf("  ran %.1f s: %llu 68000 cycles (%.2f MHz), %llu bus reads, %llu bus writes, %llu ROM reads; PC %06x\n",
	       total, (unsigned long long)cycles, cycles / total / 1e6, (unsigned long long)n_bus_rd,
	       (unsigned long long)n_bus_wr, (unsigned long long)n_rom, m68k_get_reg(NULL, M68K_REG_PC));

	ctl[1] = 0;
	printf("  bus given back: the 68000 restarts from its reset vector\n");
	return 0;
}
