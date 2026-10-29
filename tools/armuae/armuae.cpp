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
// -b runs it on the MiSTer's real Amiga instead (bridge mode, below). Only a
// core with the flat window can take it: one installed under a path holding
// "lwbridge-flat" (AmigaCD_MiSTer claude/lwbridge-echo 628d728 or later).
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
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>

// ---- the memory map: one flat 16 MB block --------------------------------------
#define NATMEM_SIZE (16 << 20)

// natmem as the 68k sees it, a word or a long at a time. Not Amiberry's
// do_get_mem_word() and friends: on this target they work a byte at a time,
// which is harmless in host memory and wrong on the bus -- through the window
// every byte is its own 68k cycle, and the custom chips decode the whole data
// bus on every cycle, so a word written as two bytes reaches INTENA as $xxFF
// first (measured: $C020 left INTENA at $40FF). A byte read of a register
// that clears on read loses the other half the same way.
static inline uae_u16 ld16(void *p) { return __builtin_bswap16(*(volatile uae_u16 *)p); }
static inline uae_u32 ld32(void *p) { return __builtin_bswap32(*(volatile uae_u32 *)p); }
static inline void st16(void *p, uae_u16 v) { *(volatile uae_u16 *)p = __builtin_bswap16(v); }
static inline void st32(void *p, uae_u32 v) { *(volatile uae_u32 *)p = __builtin_bswap32(v); }
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

static uae_u32 REGPARAM2 ram_lget(uaecptr a) { return ld32((uae_u32 *)(natmem_offset + (a & 0xFFFFFF))); }
static uae_u32 REGPARAM2 ram_wget(uaecptr a) { return ld16((uae_u16 *)(natmem_offset + (a & 0xFFFFFF))); }
static uae_u32 REGPARAM2 ram_bget(uaecptr a) { return natmem_offset[a & 0xFFFFFF]; }
static void REGPARAM2 ram_lput(uaecptr a, uae_u32 v) { st32((uae_u32 *)(natmem_offset + (a & 0xFFFFFF)), v); }
static void REGPARAM2 ram_wput(uaecptr a, uae_u32 v) { st16((uae_u16 *)(natmem_offset + (a & 0xFFFFFF)), v); }
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

// ---- bridge mode (-b): the real Amiga, through the FPGA ----------------------------
//
// natmem is the core's flat window, the full HPS-to-FPGA bridge at ARM
// physical 0xC0000000, where every access is a cycle on the 68k's bus: chip
// RAM, slow RAM, the custom chips and the CIAs are what the machine has, at
// their own addresses. Two pieces are mapped over it in ARM memory: an 8 MB
// fast RAM at $200000, offered by autoconfig as armcpu's board is, and a copy
// of the Kickstart at $F80000 (the core serves the same ROM at $000000 while
// the overlay is on). armcpu_bridge's control registers, on the lightweight
// bridge, take the bus (OWN, with FAST and FASTW), give the RESET instruction
// its line, and report the interrupt level.
//
// Chip and slow RAM are direct banks, so the JIT reads and writes them in
// place; the I/O areas go through bank functions over the same window. The
// 68k's misaligned word and long accesses become ARM ones on device memory,
// which the kernel fixes up (/proc/cpu/alignment).
//
// Chip RAM, from step 5 on, is read from a copy instead: the DDR3 shadow at
// 0x3B000000, which the core's chip_mirror keeps up to date with every write
// the chipset makes (MIRROR in CTRL). An uncached DDR3 read costs the ARM
// ~140 ns where a read through the window costs 564-886. The shadow is mapped
// over natmem's first 2 MB read-only, so the JIT and the fetches read it in
// place; a write goes through shd_*put to both the shadow and, through the
// window, chip RAM. Compiled code that stores there directly (a block
// compiled for some other memory, run with a chip RAM address) faults, and
// on_segv makes the store through shd_*put and has the block recompiled,
// as Amiberry's own handler does for its I/O areas. -n turns this off.
#define LW_BASE     0xFF200000UL
#define LW_SPAN     0x200000UL
#define PX_CTL      0x080000UL
#define PX_MAGIC    0x41435055UL
#define HF_BASE     0xC0000000UL
#define CTRL_OWN    1u
#define CTRL_RESET  8u
#define CTRL_MIRROR 0x10u
#define SHADOW_PHYS 0x3B000000UL
#define CHIP_SIZE   0x200000UL
static int bridge;                    // -b
static unsigned bmode = 6;            // FAST and FASTW (and MIRROR, with the shadow)
static int shadow = 1;                // -n: chip RAM through the window, as before step 5
static uae_u8 *shadow_rw, *chipwin;   // writable views of the shadow and of the window's chip RAM
// Step 6: with tools/armuae/kmod's armwin.ko loaded, the window and the
// shadow's write view are mapped through /dev/armwin as Device memory, so
// the ARM posts its stores instead of waiting on each (/dev/mem makes them
// strongly ordered). The control registers and the shadow's read view stay
// strongly ordered; dsb() puts the posted stores ahead of a control access
// (a different peripheral, so Device ordering alone does not). -N: /dev/mem.
static int devwin = 1;
static inline void dsb(void)
{
#ifdef __arm__
	asm volatile("dsb sy" ::: "memory");
#endif
}
static uae_u32 n_resync, n_segv_st;
static volatile uae_u32 *ctl;
static int owning;

// armcpu's fast RAM board: 8 MB Zorro II, first in the autoconfig chain
static uae_u8 ac_rom[16] = { 0xE0, 0x01, 0x80, 0x00, 0x07, 0xDB };
static int ac_mine = 1;
static uae_u8 ac_lo;
static uae_u32 fast_base = 0xFFFFFFFF;

static uae_u32 ac_nibble(uaecptr a)
{
	uae_u32 o = a & 0xFF;
	if (o & 1) return 0xFF;
	if (o >= 0x40) return 0x00;
	uae_u8 r = ac_rom[o >> 2];
	uae_u8 n = (o & 2) ? (r & 0x0F) : (r >> 4);
	if (o >= 0x04) n = ~n & 0x0F;
	return n << 4;
}
static uae_u32 REGPARAM2 ac_bget(uaecptr a) { return ac_mine ? ac_nibble(a) : ram_bget(a); }
static uae_u32 REGPARAM2 ac_wget(uaecptr a) { return ac_mine ? (ac_nibble(a) << 8) | ac_nibble(a + 1) : ram_wget(a); }
static uae_u32 REGPARAM2 ac_lget(uaecptr a) { return ac_mine ? (ac_wget(a) << 16) | ac_wget(a + 2) : ram_lget(a); }
static void REGPARAM2 ac_bput(uaecptr a, uae_u32 v)
{
	if (!ac_mine) { ram_bput(a, v); return; }
	switch (a & 0xFF) {
	case 0x4A: ac_lo = (v >> 4) & 0x0F; break;
	case 0x48:
		fast_base = ((uae_u32)((v >> 4) & 0x0F) << 20) | ((uae_u32)ac_lo << 16);
		ac_mine = 0;
		if (fast_base != 0x200000)
			fprintf(stderr, "armuae: fast RAM configured at %06x, mapped at $200000 only\n", fast_base);
		break;
	case 0x4C: ac_mine = 0; break;          // shut up
	}
}
static void REGPARAM2 ac_wput(uaecptr a, uae_u32 v) { ac_bput(a, v >> 8); }
static void REGPARAM2 ac_lput(uaecptr a, uae_u32 v) { ac_bput(a, v >> 24); }

static addrbank wio_bank = {       // the window, through functions: I/O
	ram_lget, ram_wget, ram_bget, ram_lput, ram_wput, ram_bput,
	io_xlate, io_check, NULL, _T("wio"), _T("bus I/O"), ram_lget, ram_wget,
	ABFLAG_IO, S_READ, S_WRITE
};
static addrbank wram_bank = {      // the window, direct: chip and slow RAM
	ram_lget, ram_wget, ram_bget, ram_lput, ram_wput, ram_bput,
	ram_xlate, ram_check, NULL, _T("wram"), _T("bus RAM"), ram_lget, ram_wget,
	ABFLAG_RAM, 0, 0
};
static addrbank fast_bank = {      // ARM memory: the fast RAM board
	ram_lget, ram_wget, ram_bget, ram_lput, ram_wput, ram_bput,
	ram_xlate, ram_check, NULL, _T("fast"), _T("Fast RAM"), ram_lget, ram_wget,
	ABFLAG_RAM, 0, 0
};
static addrbank ac_bank = {        // autoconfig: the board, then whatever follows on the bus
	ac_lget, ac_wget, ac_bget, ac_lput, ac_wput, ac_bput,
	io_xlate, io_check, NULL, _T("autoconfig"), _T("Autoconfig"), ac_lget, ac_wget,
	ABFLAG_IO, S_READ, S_WRITE
};

// chip RAM through the shadow: reads from natmem (the shadow), writes to both
static uae_u32 REGPARAM2 shd_lget(uaecptr a) { return ld32((uae_u32 *)(natmem_offset + (a & (CHIP_SIZE - 1)))); }
static uae_u32 REGPARAM2 shd_wget(uaecptr a) { return ld16((uae_u16 *)(natmem_offset + (a & (CHIP_SIZE - 1)))); }
static uae_u32 REGPARAM2 shd_bget(uaecptr a) { return natmem_offset[a & (CHIP_SIZE - 1)]; }
static void REGPARAM2 shd_lput(uaecptr a, uae_u32 v)
{
	a &= CHIP_SIZE - 1;
	st32((uae_u32 *)(chipwin + a), v);
	st32((uae_u32 *)(shadow_rw + a), v);
}
static void REGPARAM2 shd_wput(uaecptr a, uae_u32 v)
{
	a &= CHIP_SIZE - 1;
	st16((uae_u16 *)(chipwin + a), v);
	st16((uae_u16 *)(shadow_rw + a), v);
}
static void REGPARAM2 shd_bput(uaecptr a, uae_u32 v)
{
	a &= CHIP_SIZE - 1;
	chipwin[a] = (uae_u8)v;
	shadow_rw[a] = (uae_u8)v;
}
static addrbank shd_bank = {       // chip RAM: read from the shadow in place, written through
	shd_lget, shd_wget, shd_bget, shd_lput, shd_wput, shd_bput,
	ram_xlate, ram_check, NULL, _T("shadow"), _T("chip RAM, DDR3 shadow"), shd_lget, shd_wget,
	ABFLAG_RAM, 0, S_WRITE
};
static void REGPARAM2 chip_lput(uaecptr a, uae_u32 v) { if (shadow) shd_lput(a, v); else ram_lput(a, v); }
static void REGPARAM2 chip_wput(uaecptr a, uae_u32 v) { if (shadow) shd_wput(a, v); else ram_wput(a, v); }
static void REGPARAM2 chip_bput(uaecptr a, uae_u32 v) { if (shadow) shd_bput(a, v); else ram_bput(a, v); }

// The overlay is the ARM's to give, as in armcpu: this machine's chipset does
// not put the ROM back at $000000 for the RESET line (measured: after the
// pulse, $4 reads chip RAM). While it is on, reads of $000000-$07FFFF come
// from the ROM copy and writes go to chip RAM; CIA-A's PA0 turns it off.
static uae_u8 *rom_copy(uaecptr a) { return natmem_offset + 0xF80000 + (a & 0x7FFFF); }
static uae_u32 REGPARAM2 ovl_lget(uaecptr a) { return ld32((uae_u32 *)rom_copy(a)); }
static uae_u32 REGPARAM2 ovl_wget(uaecptr a) { return ld16((uae_u16 *)rom_copy(a)); }
static uae_u32 REGPARAM2 ovl_bget(uaecptr a) { return *rom_copy(a); }
static uae_u8 *REGPARAM2 ovl_xlate(uaecptr a) { return rom_copy(a); }
static addrbank ovl_bank = {
	ovl_lget, ovl_wget, ovl_bget, chip_lput, chip_wput, chip_bput,
	ovl_xlate, ram_check, NULL, _T("overlay"), _T("ROM overlay"), ovl_lget, ovl_wget,
	ABFLAG_IO, S_READ, S_WRITE
};
static void map(addrbank *b, uae_u32 start, uae_u32 size);
static int ovl = 1;
static uae_u8 cia_pra, cia_ddra;
static addrbank *chip_ram_bank = &wram_bank;   // what the overlay gives way to (-c: counted)
static void set_ovl(int on)
{
	ovl = on;
	map(on ? &ovl_bank : chip_ram_bank, 0x000000, 0x080000);
}
static void cia_track(uaecptr a, uae_u8 v)
{
	if ((a & 0xFF0000) != 0xBF0000 || (a & 0x1001) != 0x0001) return;   // CIA-A, odd byte
	switch ((a >> 8) & 15) {
	case 0: cia_pra  = v; break;
	case 2: cia_ddra = v; break;
	default: return;
	}
	int on = !((cia_ddra & 1) && !(cia_pra & 1));
	if (on != ovl) set_ovl(on);
}
static void REGPARAM2 cia_bput(uaecptr a, uae_u32 v) { ram_bput(a, v); cia_track(a, (uae_u8)v); }
static void REGPARAM2 cia_wput(uaecptr a, uae_u32 v) { ram_wput(a, v); cia_track(a + 1, (uae_u8)v); }
static void REGPARAM2 cia_lput(uaecptr a, uae_u32 v) { ram_lput(a, v); cia_track(a + 1, (uae_u8)(v >> 16)); cia_track(a + 3, (uae_u8)v); }
// DEBUG: CIA-A's ICR and SDR as the CPU reads them, on change (the keyboard)
static uae_u32 n_icra_rd;
static uae_u32 REGPARAM2 cia_bget(uaecptr a)
{
	uae_u32 v = ram_bget(a);
	static int n_icr;
	if ((a & 0xFFFFFF) == 0xBFED01) n_icra_rd++;
	if ((a & 0xFFFFFF) == 0xBFED01 && ((v & 8) || n_icr < 5)) { n_icr++; fprintf(stderr, "ICR %02x\n", v); }
	if ((a & 0xFFFFFF) == 0xBFEC01) { static uae_u32 last = 0x100; if (v != last) fprintf(stderr, "SDR %02x\n", v); last = v; }
	return v;
}
static addrbank ciaw_bank = {
	ram_lget, ram_wget, cia_bget, cia_lput, cia_wput, cia_bput,
	io_xlate, io_check, NULL, _T("cia"), _T("CIA"), ram_lget, ram_wget,
	ABFLAG_IO, S_READ, S_WRITE
};

// -c: count the CPU's bus traffic, by area, instead of letting the JIT address
// chip and slow RAM directly. MEASUREMENT: it slows the CPU, and instruction
// fetches (through pointers) are not counted, so it is a lower bound.
enum { R_CHIP, R_CIA, R_SLOW, R_CUSTOM, R_OTHER, R_N };
static const char *const r_name[R_N] = { "chip", "CIA", "slow", "custom", "other" };
static int counting;
static uae_u64 n_rd[R_N], n_wr[R_N];
static inline int region(uaecptr a)
{
	a &= 0xFFFFFF;
	return a < 0x200000 ? R_CHIP : a < 0xA00000 ? R_OTHER : a < 0xC00000 ? R_CIA :
	       a < 0xD80000 ? R_SLOW : a < 0xE00000 ? R_CUSTOM : R_OTHER;
}
static uae_u32 REGPARAM2 cnt_lget(uaecptr a) { n_rd[region(a)]++; return ram_lget(a); }
static uae_u32 REGPARAM2 cnt_wget(uaecptr a) { n_rd[region(a)]++; return ram_wget(a); }
static uae_u32 REGPARAM2 cnt_bget(uaecptr a) { n_rd[region(a)]++; return ram_bget(a); }
static void REGPARAM2 cnt_lput(uaecptr a, uae_u32 v) { n_wr[region(a)]++; ram_lput(a, v); }
static void REGPARAM2 cnt_wput(uaecptr a, uae_u32 v) { n_wr[region(a)]++; ram_wput(a, v); }
static void REGPARAM2 cnt_bput(uaecptr a, uae_u32 v) { n_wr[region(a)]++; ram_bput(a, v); }
static void REGPARAM2 cntcia_lput(uaecptr a, uae_u32 v) { n_wr[R_CIA]++; cia_lput(a, v); }
static void REGPARAM2 cntcia_wput(uaecptr a, uae_u32 v) { n_wr[R_CIA]++; cia_wput(a, v); }
static void REGPARAM2 cntcia_bput(uaecptr a, uae_u32 v) { n_wr[R_CIA]++; cia_bput(a, v); }
static addrbank cnt_ram_bank = {   // chip/slow RAM through functions; code still runs there
	cnt_lget, cnt_wget, cnt_bget, cnt_lput, cnt_wput, cnt_bput,
	ram_xlate, ram_check, NULL, _T("cntram"), _T("bus RAM, counted"), cnt_lget, cnt_wget,
	ABFLAG_RAM, S_READ, S_WRITE   // RAM, so fetches work (as I/O the boot stops at $F80CA0);
	                              // S_READ/S_WRITE make the JIT call the functions too
};
static addrbank cnt_io_bank = {
	cnt_lget, cnt_wget, cnt_bget, cnt_lput, cnt_wput, cnt_bput,
	io_xlate, io_check, NULL, _T("cntio"), _T("bus I/O, counted"), cnt_lget, cnt_wget,
	ABFLAG_IO, S_READ, S_WRITE
};
static addrbank cnt_cia_bank = {
	cnt_lget, cnt_wget, cnt_bget, cntcia_lput, cntcia_wput, cntcia_bput,
	io_xlate, io_check, NULL, _T("cntcia"), _T("CIA, counted"), cnt_lget, cnt_wget,
	ABFLAG_IO, S_READ, S_WRITE
};

static void release_bus(void)
{
	dsb();
	if (owning && ctl) { ctl[1] = 0; owning = 0; }
}

// the chipset's reset line, as the 68000's RESET drives it (~17 us)
static void bus_reset_pulse(void)
{
	dsb();
	ctl[1] = CTRL_OWN | bmode | CTRL_RESET;
	usleep(20);
	ctl[1] = CTRL_OWN | bmode;
	ac_mine = 1;
	fast_base = 0xFFFFFFFF;
	cia_pra = cia_ddra = 0;
	set_ovl(1);
}

static int flat_core_loaded(void)
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
			if (l > 4 && !strcmp(buf + i + l - 4, ".rbf") && strstr(buf + i, "lwbridge-flat")) found = 1;
		}
	}
	closedir(d);
	return found;
}

static void map(addrbank *b, uae_u32 start, uae_u32 size)
{
	// Every direct bank is natmem at its own address, so a bank is described
	// as natmem from address 0: the same bank can then be mapped at several
	// places (chip and slow RAM, and back and forth under the overlay).
	b->baseaddr = b->flags & ABFLAG_IO ? NULL : natmem_offset;
	b->baseaddr_direct_r = b->flags & ABFLAG_IO ? NULL : natmem_offset;
	b->baseaddr_direct_w = (b->flags & ABFLAG_RAM) && b != &shd_bank ? natmem_offset : NULL;
	// the shadow is read-only in natmem: its data has to go through shd_*put
	int funcs = !b->baseaddr || b == &cnt_ram_bank || b == &shd_bank;
	b->start = b->startaccessmask = 0;
	b->mask = 0xFFFFFF;
	b->allocated_size = b->reserved_size = 0x1000000;
	for (uae_u32 a = start; a < start + size; a += 0x10000) {
		mem_banks[bankindex(a)] = b;
		// -c and the shadow: data through the functions; fetches still use b->baseaddr
		baseaddr[bankindex(a)] = funcs ? (uae_u8 *)b + 1 : natmem_offset;
	}
	// the 24-bit space mirrors through the 4 GB the bank table spans
	for (uae_u32 m = 1; m < MEMORY_BANKS / 256; m++)
		for (uae_u32 a = start; a < start + size; a += 0x10000) {
			mem_banks[bankindex(a) + m * 256] = b;
			baseaddr[bankindex(a) + m * 256] = funcs ? (uae_u8 *)b + 1 : natmem_offset - m * 0x1000000u;
		}
}

addrbank *get_mem_bank_real(uaecptr a) { return &get_mem_bank(a); }
bool memory_address_space_rebuild_needed(void) { return false; }
void memory_clear(void) { }
void memory_restore(void) { }
void restore_banks(void) { }

// UAE's own accessors, as memory.cpp has them
uae_u32 memory_get_longi(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ld32((uae_u32 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->lgeti, a); }
uae_u32 memory_get_wordi(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ld16((uae_u16 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->wgeti, a); }
uae_u32 memory_get_long(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ld32((uae_u32 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->lget, a); }
uae_u32 memory_get_word(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ld16((uae_u16 *)(ab->baseaddr_direct_r + ((a - ab->startaccessmask) & ab->mask))) : call_mem_get_func(ab->wget, a); }
uae_u32 memory_get_byte(uaecptr a) { addrbank *ab = &get_mem_bank(a); return ab->baseaddr_direct_r ? ab->baseaddr_direct_r[(a - ab->startaccessmask) & ab->mask] : call_mem_get_func(ab->bget, a); }
void memory_put_long(uaecptr a, uae_u32 v) { addrbank *ab = &get_mem_bank(a); if (ab->baseaddr_direct_w) st32((uae_u32 *)(ab->baseaddr_direct_w + ((a - ab->startaccessmask) & ab->mask)), v); else call_mem_put_func(ab->lput, a, v); }
void memory_put_word(uaecptr a, uae_u32 v) { addrbank *ab = &get_mem_bank(a); if (ab->baseaddr_direct_w) st16((uae_u16 *)(ab->baseaddr_direct_w + ((a - ab->startaccessmask) & ab->mask)), v); else call_mem_put_func(ab->wput, a, v); }
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

static uae_u32 n_lev[8];               // rising edges to each interrupt level, per report

// The shadow afresh from chip RAM, through the window (~0.5 s): at the start,
// and when the mirror has dropped a write. MIRROR is set first, so a DMA
// write during the copy reaches the shadow too.
static void shadow_copy(void)
{
	dsb();
	ctl[1] = CTRL_OWN | (bmode & ~CTRL_MIRROR);
	ctl[1] = CTRL_OWN | bmode;           // MIRROR rising clears STATUS[10]
	for (uae_u32 a = 0; a < CHIP_SIZE; a += 4)
		*(volatile uae_u32 *)(shadow_rw + a) = *(volatile uae_u32 *)(chipwin + a);
	dsb();
}

static void slice(void)
{
	if (bridge) {
		dsb();
		uae_u32 st = ctl[3];
		static int ipl_prev;
		ipl_now = st & 7;                 // Paula's level
		if (ipl_now != ipl_prev && ipl_now) n_lev[ipl_now]++;
		ipl_prev = ipl_now;
		if (st & 0x200) {
			fprintf(stderr, "armuae: the bridge gave up a bus cycle (watchdog), stopping\n");
			stop = 1;
		}
		if (shadow && (st & 0x400)) {    // the mirror dropped a write
			shadow_copy();
			n_resync++;
		}
	}
	doint();                              // a level change becomes SPCFLAG_INT
	if (since(&t_last) >= 1.0) {
		uae_u32 n = count_addr ? ld32((uae_u32 *)(natmem_offset + count_addr)) : 0;
		printf("  %5.1f s: PC %08x, count %u (+%u/s)\n", since(&t0), m68k_getpc(), n, (n - n_last));
		if (ctl) {
			// the bridge's own count of 68k word cycles: every access, the JIT's direct ones too
			static uae_u32 l_words;
			uae_u32 w = ctl[6];
			printf("          bus: %u word cycles/s, interrupts/s by level: %u %u %u %u %u %u %u\n", w - l_words,
			       n_lev[1], n_lev[2], n_lev[3], n_lev[4], n_lev[5], n_lev[6], n_lev[7]);
			printf("          CIA-A ICR reads: %u, SDR %02x\n", n_icra_rd, natmem_offset[0xBFEC01]);
			if (shadow) printf("          shadow: %u resyncs, %u stores from compiled code\n", n_resync, n_segv_st);
			if (devwin) printf("          window: Device memory (/dev/armwin)\n");
			n_icra_rd = 0;
			memset(n_lev, 0, sizeof(n_lev));
			l_words = w;
		}
		if (counting) {
			static uae_u64 l_rd[R_N], l_wr[R_N];
			printf("          reads/s:");
			for (int r = 0; r < R_N; r++) printf(" %s %llu", r_name[r], (unsigned long long)(n_rd[r] - l_rd[r]));
			printf("\n          writes/s:");
			for (int r = 0; r < R_N; r++) printf(" %s %llu", r_name[r], (unsigned long long)(n_wr[r] - l_wr[r]));
			printf("\n");
			memcpy(l_rd, n_rd, sizeof(l_rd));
			memcpy(l_wr, n_wr, sizeof(l_wr));
		}
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
void custom_reset(bool, bool)
{
	regs.natmem_offset = natmem_offset;
	if (bridge) bus_reset_pulse();
}

static void on_signal(int) { stop = 1; }
static void on_fatal(int sig) { release_bus(); signal(sig, SIG_DFL); raise(sig); }

// SIGSEGV: a store into the read-only shadow is made through shd_*put (and a
// compiled block that made it is thrown away, so it is compiled again with
// the bank's S_WRITE: indirect); anything else is what the host faulted on,
// for JIT debugging
#include <ucontext.h>
extern uae_u8 *compiled_code, *current_compile_p;
extern blockinfo *active, *dormant;
void invalidate_block(blockinfo *bi);
void raise_in_cl_list(blockinfo *bi);
static int drop_block(blockinfo *bi, void *pc)
{
	for (; bi; bi = bi->next)
		if (bi->handler && (uae_u8 *)bi->direct_handler <= pc && bi->nexthandler > pc) {
			invalidate_block(bi);
			raise_in_cl_list(bi);
			return 1;
		}
	return 0;
}
static void on_segv(int, siginfo_t *si, void *uc)
{
	ucontext_t *u = (ucontext_t *)uc;
#ifdef __arm__
	uae_u8 *fa = (uae_u8 *)si->si_addr;
	if (shadow && fa >= natmem_offset && fa < natmem_offset + CHIP_SIZE) {
		unsigned long *r = &u->uc_mcontext.arm_r0;     // r0-r15, in order
		uae_u32 op = *(uae_u32 *)u->uc_mcontext.arm_pc;
		uaecptr a = (uaecptr)(fa - natmem_offset);
		int rd = (op >> 12) & 15, done = 0;
		if (!((op >> 20) & 1) && rd != 15) {            // a store, not of the PC
			if (((op >> 25) & 7) == 0 && ((op >> 5) & 3) == 1) {          // STRH
				shd_wput(a, __builtin_bswap16((uae_u16)r[rd])); done = 1;
			}
			else if (((op >> 25) & 7) == 2 || ((op >> 25) & 7) == 3) {  // STR, STRB
				if ((op >> 22) & 1) shd_bput(a, (uae_u8)r[rd]);
				else shd_lput(a, __builtin_bswap32((uae_u32)r[rd]));
				done = 1;
			}
		}
		if (done) {
			void *pc = (void *)u->uc_mcontext.arm_pc;
			u->uc_mcontext.arm_pc += 4;
			n_segv_st++;
			if ((uae_u8 *)pc >= compiled_code && (uae_u8 *)pc < current_compile_p)
				if (!drop_block(active, pc)) drop_block(dormant, pc);
			return;
		}
	}
	fprintf(stderr, "SEGV: addr %p, host pc %08lx lr %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx; natmem %p, 68k pc %08x\n",
	        si->si_addr, u->uc_mcontext.arm_pc, u->uc_mcontext.arm_lr, u->uc_mcontext.arm_r0,
	        u->uc_mcontext.arm_r1, u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3, natmem_offset, regs.pc);
	uae_u32 *ip = (uae_u32 *)u->uc_mcontext.arm_pc;
	fprintf(stderr, "  code: %08x %08x [%08x] %08x %08x\n", ip[-2], ip[-1], ip[0], ip[1], ip[2]);
#else
	(void)u; fprintf(stderr, "SEGV: addr %p\n", si->si_addr);
#endif
	release_bus();
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
	n = snprintf(buf, sizeof(buf), "  ipl_now %d, STATUS %08x, intmask %d, ipl_pin %d, sr %04x\n",
	             ipl_now, bridge ? ctl[3] : 0, regs.intmask, regs.ipl_pin, regs.sr);
	fputs(buf, stderr);
	void *bt[32];
	backtrace_symbols_fd(bt, backtrace(bt, 32), 2);
}

int main(int argc, char **argv)
{
	const char *rom_path = NULL;
	int model = 68020, jit_kb = 0, opt;
	int force = 0;
	while ((opt = getopt(argc, argv, "r:C:j:t:a:v:bm:fcnN")) != -1) {
		switch (opt) {
		case 'r': rom_path = optarg; break;
		case 'C': model = atoi(optarg); break;
		case 'j': jit_kb = atoi(optarg); break;
		case 't': seconds = atof(optarg); break;
		case 'a': count_addr = strtoul(optarg, NULL, 0) & 0xFFFFFC; break;
		case 'v': check_addr = strtoul(optarg, NULL, 0) & 0xFFFFFE; break;
		case 'b': bridge = 1; break;
		case 'm': bmode = (unsigned)strtoul(optarg, NULL, 0) & 6; break;
		case 'f': force = 1; break;
		case 'c': counting = 1; break;
		case 'n': shadow = 0; break;
		case 'N': devwin = 0; break;
		default:
			fprintf(stderr, "usage: %s -r rom [-C model] [-j KB] [-t s] [-a addr] [-v addr] [-b [-m mode] [-f] [-n] [-c]]\n", argv[0]);
			return 2;
		}
	}
	if (!rom_path) { fprintf(stderr, "-r rom\n"); return 2; }

	if (bridge) {
		if (!force && !flat_core_loaded()) {
			fprintf(stderr, "no running core has \"lwbridge-flat\" in its path: without the flat window "
			                "an access to it can hang the ARM (-f overrides)\n");
			return 2;
		}
		int memfd = open("/dev/mem", O_RDWR | O_SYNC);
		if (memfd < 0) { perror("/dev/mem"); return 1; }
		volatile uae_u8 *lw = (volatile uae_u8 *)mmap(NULL, LW_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, LW_BASE);
		if (lw == MAP_FAILED) { perror("mmap lw"); return 1; }
		ctl = (volatile uae_u32 *)(lw + PX_CTL);
		if (ctl[0] != PX_MAGIC) { fprintf(stderr, "no armcpu_bridge: control register 0 reads %08x\n", ctl[0]); return 1; }
		if (!(ctl[3] & 0x100)) { fprintf(stderr, "the CPU is not set to 68000: the proxy only replaces the fx68k\n"); return 1; }
		int winfd = devwin ? open("/dev/armwin", O_RDWR) : -1;
		if (devwin && winfd < 0) { devwin = 0; winfd = memfd; fprintf(stderr, "armuae: no /dev/armwin (insmod armwin.ko): window strongly ordered\n"); }
		if (!devwin) winfd = memfd;
		// natmem is the window; fast RAM and the ROM copy go over it
		natmem_offset = (uae_u8 *)mmap(NULL, NATMEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, winfd, HF_BASE);
		if (natmem_offset == MAP_FAILED) { perror("mmap window"); return 1; }
		if (mmap(natmem_offset + 0x200000, 0x800000, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
		    mmap(natmem_offset + 0xF80000, 0x080000, PROT_READ | PROT_WRITE,
		         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
			perror("mmap fast/ROM over the window");
			return 1;
		}
		atexit(release_bus);
		signal(SIGBUS, on_fatal);
		signal(SIGILL, on_fatal);
		signal(SIGFPE, on_fatal);
		signal(SIGABRT, on_fatal);
		signal(SIGHUP, on_signal);
		owning = 1;
		ctl[1] = CTRL_OWN | bmode;
		if ((ctl[1] & 7) != (CTRL_OWN | bmode)) { fprintf(stderr, "CTRL did not take %u\n", CTRL_OWN | bmode); return 1; }
		// a cold start, as armcpu makes it: no ExecBase at $4 (Kickstart would
		// take the last session's for a warm reboot and reuse its lists), and
		// no 'HELP' at $0 with an alert at $100
		for (uae_u32 a = 0; a < 8; a += 2) *(volatile uae_u16 *)(natmem_offset + a) = 0;
		for (uae_u32 a = 0x100; a < 0x108; a += 2) *(volatile uae_u16 *)(natmem_offset + a) = 0;
		if (shadow) {
			shadow_rw = (uae_u8 *)mmap(NULL, CHIP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, winfd, SHADOW_PHYS);
			chipwin   = (uae_u8 *)mmap(NULL, CHIP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, winfd, HF_BASE);
			if (shadow_rw == MAP_FAILED || chipwin == MAP_FAILED) { perror("mmap shadow"); return 1; }
			bmode |= CTRL_MIRROR;
			struct timespec c0;
			clock_gettime(CLOCK_MONOTONIC, &c0);
			shadow_copy();
			if (!(ctl[1] & CTRL_MIRROR)) { fprintf(stderr, "this core has no chip RAM mirror (CTRL[4]): run with -n\n"); return 1; }
			if (mmap(natmem_offset, CHIP_SIZE, PROT_READ, MAP_SHARED | MAP_FIXED, memfd, SHADOW_PHYS) == MAP_FAILED) {
				perror("mmap shadow over the window");
				return 1;
			}
			printf("armuae: chip RAM from the DDR3 shadow, copied in %.2f s\n", since(&c0));
		}
	}
	else {
#ifdef MAP_32BIT
	// x86-64's JIT addresses natmem with 32-bit displacements
	natmem_offset = (uae_u8 *)mmap(NULL, NATMEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#else
	natmem_offset = (uae_u8 *)mmap(NULL, NATMEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
	if (natmem_offset == MAP_FAILED) { perror("natmem"); return 1; }
	}
	natmem_reserved_size = NATMEM_SIZE;
	regs.natmem_offset = natmem_offset;   // 5.7.4's JIT reads NATMEM_OFFSET from regs

	FILE *f = fopen(rom_path, "rb");
	if (!f) { perror(rom_path); return 1; }
	size_t got = fread(natmem_offset + 0xF80000, 1, 0x80000, f);
	fclose(f);
	if (got == 0x40000) memcpy(natmem_offset + 0xFC0000, natmem_offset + 0xF80000, 0x40000);
	if (!bridge)
		memcpy(natmem_offset, natmem_offset + 0xF80000, 8);   // the overlay's reset vectors

	for (int i = 0; i < MEMORY_BANKS; i++) { mem_banks[i] = &io_bank; baseaddr[i] = (uae_u8 *)&io_bank + 1; }
	if (bridge) {
		if (shadow) chip_ram_bank = &shd_bank;
		if (counting) chip_ram_bank = &cnt_ram_bank;
		map(chip_ram_bank, 0x000000, 0x200000);  // chip RAM (and the ROM, while the overlay is on)
		map(&fast_bank, 0x200000, 0x800000);  // the board, once autoconfig has placed it
		map(counting ? &cnt_cia_bank : &ciaw_bank, 0xA00000, 0x200000);  // the CIAs (CIA-A's PA0 is the overlay)
		map(counting ? &cnt_ram_bank : &wram_bank, 0xC00000, 0x180000);  // slow RAM, never mirrored
		map(counting ? &cnt_io_bank : &wio_bank, 0xD80000, 0x100000);  // the custom chips, RTC, IDE
		map(&wio_bank,  0xE00000, 0x080000);
		map(&ac_bank,   0xE80000, 0x080000);  // autoconfig
		map(&wio_bank,  0xF00000, 0x080000);
		map(&rom_bank,  0xF80000, 0x080000);
	}
	else {
		map(&io_bank, 0xA00000, 0x580000);
		map(&ram_bank, 0x000000, 0xA00000);   // chip RAM and the Zorro II fast RAM space
		map(&rom_bank, 0xF80000, 0x080000);
	}
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
	printf("armuae: Amiberry 5.7.4 68k core, %d, %s, %s, ROM %s\n", model,
	       currprefs.cachesize ? "JIT" : "interpreter", bridge ? "the Amiga through the flat window" : "no chipset", rom_path);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	t_last = t0;
	quit_program = UAE_RESET_HARD;
	m68k_go(1);
	double t = since(&t0);
	uae_u32 n = count_addr ? ld32((uae_u32 *)(natmem_offset + count_addr)) : 0;
	printf("  ran %.2f s: PC %08x, count %u, %.0f/s\n", t, m68k_getpc(), n, n / t);
	if (check_addr)
		printf("  long at %06x: %08x (count %u)\n", check_addr,
		       ld32((uae_u32 *)(natmem_offset + check_addr)), n);
	if (bridge) {
		release_bus();
		printf("  bus given back: the 68000 restarts from its reset vector\n");
	}
	return 0;
}
