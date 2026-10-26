// armuae: Amiberry's 68k CPU core and JIT, run on their own. PROTOTYPE.
//
// Step one of putting the JIT behind armcpu (docs/arm-cpu-plan.md): the
// CPU's whole 24-bit address space is host memory here ("natmem"), as it
// would be with a flat window onto the FPGA's bus, with no chipset behind
// it. That is enough to boot a ROM and to time code: chip RAM and fast RAM
// are plain memory, $A00000-$F7FFFF answers zero and ignores writes, the
// ROM is a read-only copy at $F80000 (and at $000000 for the reset vectors).
//
//   armuae -r rom [-C 68000|68020|68030|68040|68060] [-j KB] [-t s] [-a addr]
//
// -j sets the JIT's translation cache (0: interpreter). -a names a long
// the ROM's program counts in, reported per second; -v a long to print at
// the end (bench.c's checksum at $408).
//
// The JIT translates only while the 68k's instruction cache is on (CACR bit
// 0 on a 68020, bit 15 on a 68040): Kickstart turns it on, a bare ROM has to
// (bench/crt0.S does).
#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "uae.h"
#include "compemu.h"
#include <getopt.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>

// ---- the memory map: one flat 16 MB block --------------------------------------
#define NATMEM_SIZE (16 << 20)
// natmem_offset lives in newcpu.cpp, beside regs (patch_newcpu.py)
size_t natmem_reserved_size;
bool canbang = true;
bool jit_direct_compatible_memory = true;
int special_mem, special_mem_default;
int jit_n_addr_unsafe, jit_n_addr_bank_unsafe;
addrbank *mem_banks[MEMORY_BANKS];
addrbank *thread_mem_banks[MEMORY_BANKS];
uae_u8 *baseaddr[MEMORY_BANKS];
uae_u8 ce_banktype[65536], ce_cachable[65536];

static uae_u32 REGPARAM2 ram_lget(uaecptr a) { return do_get_mem_long((uae_u32 *)(natmem_offset + (a & 0xFFFFFF))); }
static uae_u32 REGPARAM2 ram_wget(uaecptr a) { return do_get_mem_word((uae_u16 *)(natmem_offset + (a & 0xFFFFFF))); }
static uae_u32 REGPARAM2 ram_bget(uaecptr a) { return natmem_offset[a & 0xFFFFFF]; }
static void REGPARAM2 ram_lput(uaecptr a, uae_u32 v) { do_put_mem_long((uae_u32 *)(natmem_offset + (a & 0xFFFFFF)), v); }
static void REGPARAM2 ram_wput(uaecptr a, uae_u32 v) { do_put_mem_word((uae_u16 *)(natmem_offset + (a & 0xFFFFFF)), v); }
static void REGPARAM2 ram_bput(uaecptr a, uae_u32 v) { natmem_offset[a & 0xFFFFFF] = (uae_u8)v; }
static uae_u8 *REGPARAM2 ram_xlate(uaecptr a) { return natmem_offset + (a & 0xFFFFFF); }
static int REGPARAM2 ram_check(uaecptr, uae_u32) { return 1; }
static void REGPARAM2 rom_lput(uaecptr, uae_u32) { }
static void REGPARAM2 rom_wput(uaecptr, uae_u32) { }
static void REGPARAM2 rom_bput(uaecptr, uae_u32) { }

static uae_u32 REGPARAM2 io_get(uaecptr) { return 0; }
static void REGPARAM2 io_put(uaecptr, uae_u32) { }
static uae_u8 *REGPARAM2 io_xlate(uaecptr a) { fprintf(stderr, "armuae: code or pointer into I/O at %06x\n", a); return natmem_offset + (a & 0xFFFFFF); }
static int REGPARAM2 io_check(uaecptr, uae_u32) { return 0; }

static addrbank ram_bank = {
	ram_lget, ram_wget, ram_bget, ram_lput, ram_wput, ram_bput,
	ram_xlate, ram_check, NULL, _T("ram"), _T("RAM"), ram_lget, ram_wget,
	ABFLAG_RAM, 0, 0
};
static addrbank rom_bank = {
	ram_lget, ram_wget, ram_bget, rom_lput, rom_wput, rom_bput,
	ram_xlate, ram_check, NULL, _T("rom"), _T("ROM"), ram_lget, ram_wget,
	ABFLAG_ROM, 0, S_WRITE
};
static addrbank io_bank = {
	io_get, io_get, io_get, io_put, io_put, io_put,
	io_xlate, io_check, NULL, _T("io"), _T("I/O"), io_get, io_get,
	ABFLAG_IO, S_READ, S_WRITE
};
addrbank dummy_bank, kickmem_bank, rtarea_bank, a3000lmem_bank, a3000hmem_bank, chipmem_bank;

static void map(addrbank *b, uae_u32 start, uae_u32 size)
{
	b->baseaddr = b->flags & ABFLAG_IO ? NULL : natmem_offset + start;
	b->baseaddr_direct_r = b->flags & ABFLAG_IO ? NULL : natmem_offset + start;
	b->baseaddr_direct_w = b->flags & ABFLAG_RAM ? natmem_offset + start : NULL;
	b->start = b->startaccessmask = start;
	b->mask = 0xFFFFFF;                    // offsets from start stay inside the bank
	b->allocated_size = b->reserved_size = size;
	for (uae_u32 a = start; a < start + size; a += 0x10000) {
		mem_banks[bankindex(a)] = b;
		baseaddr[bankindex(a)] = b->baseaddr ? natmem_offset : (uae_u8 *)b + 1;
	}
	// the 24-bit space mirrors through the 4 GB the bank table spans
	for (uae_u32 m = 1; m < MEMORY_BANKS / 256; m++)
		for (uae_u32 a = start; a < start + size; a += 0x10000) {
			mem_banks[bankindex(a) + m * 256] = b;
			baseaddr[bankindex(a) + m * 256] = b->baseaddr ? natmem_offset - m * 0x1000000u : (uae_u8 *)b + 1;
		}
}

addrbank *get_mem_bank_real(uaecptr a) { return &get_mem_bank(a); }
bool memory_address_space_rebuild_needed(void) { return false; }
void memory_clear(void) { }
void memory_restore(void) { }
void restore_banks(void) { }

// UAE's own accessors, as memory.cpp has them
uae_u32 memory_get_longi(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? do_get_mem_long((uae_u32 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->lgeti, a); }
uae_u32 memory_get_wordi(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? do_get_mem_word((uae_u16 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->wgeti, a); }
uae_u32 memory_get_long(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? do_get_mem_long((uae_u32 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->lget, a); }
uae_u32 memory_get_word(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? do_get_mem_word((uae_u16 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->wget, a); }
uae_u32 memory_get_byte(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ab->baseaddr_direct_r[(a - ab->startaccessmask) & ab->mask] : call_mem_get_func(ab->bget, a); }
void memory_put_long(uaecptr a, uae_u32 v) { addrbank *ab = &get_mem_bank(a); if (ab->baseaddr_direct_w) do_put_mem_long((uae_u32 *)(ab->baseaddr_direct_w + ((a - ab->startaccessmask) & ab->mask)), v); else call_mem_put_func(ab->lput, a, v); }
void memory_put_word(uaecptr a, uae_u32 v) { addrbank *ab = &get_mem_bank(a); if (ab->baseaddr_direct_w) do_put_mem_word((uae_u16 *)(ab->baseaddr_direct_w + ((a - ab->startaccessmask) & ab->mask)), v); else call_mem_put_func(ab->wput, a, v); }
void memory_put_byte(uaecptr a, uae_u32 v) { addrbank *ab = &get_mem_bank(a); if (ab->baseaddr_direct_w) ab->baseaddr_direct_w[(a - ab->startaccessmask) & ab->mask] = (uae_u8)v; else call_mem_put_func(ab->bput, a, v); }
uae_u8 *memory_get_real_address(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask) : ab->xlateaddr(a); }
int memory_valid_address(uaecptr a, uae_u32 size) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? 1 : ab->check(a, size); }

// only the cycle-exact modes wait on the chipset; this build never runs them
uae_u32 wait_cpu_cycle_read(uaecptr, int) { return 0; }
void wait_cpu_cycle_write(uaecptr, int, uae_u32) { }
uae_u32 wait_cpu_cycle_read_ce020(uaecptr, int) { return 0; }
void wait_cpu_cycle_write_ce020(uaecptr, int, uae_u32) { }

// ---- interrupts: the level Paula asks for (none, without a chipset) -------------
static int ipl_now;
int intlev(void) { return ipl_now; }
void intlev_ack(int) { }
bool INTREQ_0(uae_u16) { return false; }
void INTREQ_f(uae_u16) { }
void INTREQ_INT(int, int) { }

// ---- time: one event, every slice ------------------------------------------------
evt_t currcycle, nextevent, currcycle_cck;
struct ev eventtab[ev_max];
struct ev2 eventtab2[ev2_max];
#define SLICE_CYCLES (2000 * CYCLE_UNIT)

static volatile sig_atomic_t stop;
static double seconds = 5;
static uae_u32 count_addr, check_addr;
static struct timespec t0, t_last;
static uae_u32 n_last;

static double since(const struct timespec *a)
{
	struct timespec b;
	clock_gettime(CLOCK_MONOTONIC, &b);
	return (b.tv_sec - a->tv_sec) + (b.tv_nsec - a->tv_nsec) / 1e9;
}

static void slice(void)
{
	doint();                              // a level change becomes SPCFLAG_INT
	if (since(&t_last) >= 1.0) {
		uae_u32 n = count_addr ? do_get_mem_long((uae_u32 *)(natmem_offset + count_addr)) : 0;
		printf("  %5.1f s: PC %08x, count %u (+%u/s)\n", since(&t0), m68k_getpc(), n, (n - n_last));
		fflush(stdout);
		n_last = n;
		clock_gettime(CLOCK_MONOTONIC, &t_last);
	}
	if (stop || (seconds > 0 && since(&t0) >= seconds)) {
		quit_program = UAE_QUIT;
		set_special(SPCFLAG_MODE_CHANGE);
	}
}

void events_schedule(void) { nextevent = currcycle + SLICE_CYCLES; }
void clear_events(void) { events_schedule(); }
// As events.cpp has it: time is paid out of pissoff, the budget compiled code
// counts down between calls (they call with 0 and leave the debt in pissoff).
// Only when it runs out does the clock move, by what was spent.
void do_cycles_slow(int cycles)
{
	if ((pissoff -= cycles) >= 0)
		return;
	cycles = -pissoff;
	pissoff = pissoff_value;
	currcycle += cycles;
	if (currcycle >= nextevent) {
		events_schedule();
		slice();
	}
}
void do_cycles_ce(int c) { do_cycles_slow(c); }
// 5.7.4 calls through a pointer, set by the CPU speed; both land here
void do_cycles_cpu_fastest(int c) { do_cycles_slow(c); }
void do_cycles_cpu_norm(int c) { do_cycles_slow(c); }
do_cycles_func do_cycles = do_cycles_cpu_norm;
void do_cycles_ce020(int c) { do_cycles_slow(c); }
void event2_newevent_xx(int, evt_t, uae_u32 data, evfunc2 func) { func(data); }

// ---- the RESET instruction: the chipset's reset line ---------------------------------
void custom_reset(bool, bool) { regs.natmem_offset = natmem_offset; }

static void on_signal(int) { stop = 1; }

// SIGSEGV: what the host faulted on, for JIT debugging
#include <ucontext.h>
static void on_segv(int, siginfo_t *si, void *uc)
{
	ucontext_t *u = (ucontext_t *)uc;
#ifdef __arm__
	fprintf(stderr, "SEGV: addr %p, host pc %08lx lr %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx; natmem %p, 68k pc %08x\n",
	        si->si_addr, u->uc_mcontext.arm_pc, u->uc_mcontext.arm_lr, u->uc_mcontext.arm_r0,
	        u->uc_mcontext.arm_r1, u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3, natmem_offset, regs.pc);
	uae_u32 *ip = (uae_u32 *)u->uc_mcontext.arm_pc;
	fprintf(stderr, "  code: %08x %08x [%08x] %08x %08x\n", ip[-2], ip[-1], ip[0], ip[1], ip[2]);
#else
	(void)u; fprintf(stderr, "SEGV: addr %p
", si->si_addr);
#endif
	_exit(3);
}

// SIGUSR1: where are we? the 68k's PC and the host's stack
#include <execinfo.h>
static void on_where(int)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf), "where: 68k PC %08x, spcflags %08x, pissoff %d, currcycle %lld\n",
	                 regs.pc, (unsigned)regs.spcflags, pissoff, (long long)currcycle);
	(void)n; fputs(buf, stderr);
	void *bt[32];
	backtrace_symbols_fd(bt, backtrace(bt, 32), 2);
}

int main(int argc, char **argv)
{
	const char *rom_path = NULL;
	int model = 68020, jit_kb = 0, opt;
	while ((opt = getopt(argc, argv, "r:C:j:t:a:v:")) != -1) {
		switch (opt) {
		case 'r': rom_path = optarg; break;
		case 'C': model = atoi(optarg); break;
		case 'j': jit_kb = atoi(optarg); break;
		case 't': seconds = atof(optarg); break;
		case 'a': count_addr = strtoul(optarg, NULL, 0) & 0xFFFFFC; break;
		case 'v': check_addr = strtoul(optarg, NULL, 0) & 0xFFFFFE; break;
		default:
			fprintf(stderr, "usage: %s -r rom [-C model] [-j KB] [-t s] [-a addr]\n", argv[0]);
			return 2;
		}
	}
	if (!rom_path) { fprintf(stderr, "-r rom\n"); return 2; }

	#ifdef MAP_32BIT
	// x86-64's JIT addresses natmem with 32-bit displacements
	natmem_offset = (uae_u8 *)mmap(NULL, NATMEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
	natmem_offset = (uae_u8 *)mmap(NULL, NATMEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
	if (natmem_offset == MAP_FAILED) { perror("natmem"); return 1; }
	natmem_reserved_size = NATMEM_SIZE;
	regs.natmem_offset = natmem_offset;   // 5.7.4's JIT reads NATMEM_OFFSET from regs

	FILE *f = fopen(rom_path, "rb");
	if (!f) { perror(rom_path); return 1; }
	size_t got = fread(natmem_offset + 0xF80000, 1, 0x80000, f);
	fclose(f);
	if (got == 0x40000) memcpy(natmem_offset + 0xFC0000, natmem_offset + 0xF80000, 0x40000);
	memcpy(natmem_offset, natmem_offset + 0xF80000, 8);   // the overlay's reset vectors

	for (int i = 0; i < MEMORY_BANKS; i++) { mem_banks[i] = &io_bank; baseaddr[i] = (uae_u8 *)&io_bank + 1; }
	map(&io_bank, 0xA00000, 0x580000);
	map(&ram_bank, 0x000000, 0xA00000);   // chip RAM and the Zorro II fast RAM space
	map(&rom_bank, 0xF80000, 0x080000);
	dummy_bank = kickmem_bank = rtarea_bank = a3000lmem_bank = a3000hmem_bank = io_bank;
	chipmem_bank = ram_bank;

	// a 68k of the chosen model, as fast as it goes, no MMU or FPU
	currprefs.cpu_model = model;
	currprefs.fpu_model = 0;
	currprefs.mmu_model = 0;
	currprefs.address_space_24 = model <= 68020 ? true : false;
	currprefs.cpu_compatible = false;
	currprefs.cpu_cycle_exact = false;
	currprefs.cpu_memory_cycle_exact = false;
	currprefs.m68k_speed = -1;
	currprefs.cachesize = model >= 68020 ? jit_kb : 0;
	currprefs.compfpu = false;
	currprefs.comptrustbyte = currprefs.comptrustword = currprefs.comptrustlong = currprefs.comptrustnaddr = 0;
	currprefs.compnf = true;
	currprefs.comp_constjump = true;
	currprefs.int_no_unimplemented = true;
	currprefs.fpu_no_unimplemented = true;
	currprefs.cpu_clock_multiplier = 0;
	changed_prefs = currprefs;
	if (model > 68020) {
		// a 32-bit CPU on the 24-bit map: the map mirrors, so it boots
		currprefs.address_space_24 = changed_prefs.address_space_24 = false;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_where);
	{ struct sigaction sa = {}; sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); }
	setvbuf(stdout, NULL, _IOLBF, 0);
	// the JIT is built by build_comp(), which build_cpufunctbl() calls on reset
	init_m68k();
	events_schedule();
	printf("armuae: Amiberry 5.7.4 68k core, %d, %s, ROM %s\n", model,
	       currprefs.cachesize ? "JIT" : "interpreter", rom_path);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	t_last = t0;
	quit_program = UAE_RESET_HARD;
	m68k_go(1);
	double t = since(&t0);
	uae_u32 n = count_addr ? do_get_mem_long((uae_u32 *)(natmem_offset + count_addr)) : 0;
	printf("  ran %.2f s: PC %08x, count %u, %.0f/s\n", t, m68k_getpc(), n, n / t);
	if (check_addr)
		printf("  long at %06x: %08x (count %u)\n", check_addr,
		       do_get_mem_long((uae_u32 *)(natmem_offset + check_addr)), n);
	return 0;
}
