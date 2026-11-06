// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FMV module's DDR3 transport (support/minimig/fmv_transport.cpp)
// against a reference FPGA (fmv_fpga_ref.h) that talks to it only through a
// 2 MB buffer laid out as docs/fmv-contract.md (v4) says: the board image,
// the event ring with SEQ, EV_WPTR, the update queue, UQ_WPTR and UQ_RDPTR.
//
// What is checked is the plumbing, not the chips (test_fmv_model.cpp does
// those): every event reaches the model exactly once and in order, an event
// whose entry has not landed is waited for and never skipped, Main never
// overruns the update queue, back-pressure sees Main's read count, the ROM
// and its autoconfig land where the FPGA reads them, and decoder variables
// land in the board RAM in 68k byte order.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "../support/minimig/fmv_transport.h"
#include "../support/minimig/minimig_crc32.h"
#include "fmv_fpga_ref.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static void eq(const char *what, unsigned long got, unsigned long want)
{
	if (got != want) {
		printf("FAIL %s: got 0x%lx want 0x%lx\n", what, got, want);
		failures++;
	}
}

// ---------------------------------------------------- the window and FPGA

static uint64_t rd64(const uint8_t *w, uint32_t off)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | w[off + i];
	return v;
}

static void wr64(uint8_t *w, uint32_t off, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		w[off + i] = (uint8_t)(v >> (8 * i));
}

struct wfpga : fmv_fpga_ref {
	std::vector<uint8_t> mem;
	uint8_t *win;
	uint32_t uq_rd = 0;
	uint32_t applied = 0;
	bool overran = false;     // an access was posted into a full ring

	wfpga() : mem(FMV_WIN_SIZE, 0), win(mem.data()) {}

	bool ring_full() const
	{
		// Back-pressure on HOST_RDIDX's low half, modulo 2^16 (contract v3)
		return (uint16_t)(ev_count - R[FMV_IDX_RDIDX_LO]) >= FMV_EV_ENTRIES - 1;
	}

	void put_event(uint64_t e)
	{
		if (ring_full())
			overran = true;     // the core would have held the 68k here
		e |= (uint64_t)(ev_count & 0xFFFF) << 48;
		wr64(win, FMV_WIN_EV + 8 * (ev_count & (FMV_EV_ENTRIES - 1)), e);
		ev_count++;
		wr64(win, FMV_WIN_EV_WPTR, ev_count);
	}

	void post(uint32_t off, bool read, uint16_t data, uint8_t haddr) override
	{
		put_event((uint64_t)read | 1u << 1 | 1u << 2 | (uint64_t)(off & 0xFFFFF) << 4 |
		          (uint64_t)(haddr & 15) << 24 | (uint64_t)data << 32);
	}

	void amiga_reset() { put_event(1u << 3); }

	// Apply what Main has queued, as the core's queue reader does.
	void service()
	{
		uint32_t w = (uint32_t)rd64(win, FMV_WIN_UQ_WPTR);
		while (uq_rd != w) {
			uint32_t off = FMV_WIN_UQ + 16 * (uq_rd & (FMV_UQ_ENTRIES - 1));
			uint64_t w0 = rd64(win, off), w1 = rd64(win, off + 8);
			if ((uint16_t)(w0 >> 48) != (uint16_t)uq_rd)
				break;
			fmv_update u = { (uint8_t)w0, (bool)((w0 >> 8) & 1), (bool)((w0 >> 9) & 1),
			                 (uint16_t)w1, (uint16_t)(w1 >> 16) };
			apply_update(u, (uint32_t)(w1 >> 32));
			uq_rd++;
			applied++;
		}
		wr64(win, FMV_WIN_UQ_RDPTR, uq_rd);
	}
};

static std::vector<std::string> g_log;
static void logcb(void *, const char *line) { g_log.push_back(line); }

static bool logged(const char *needle)
{
	for (auto &l : g_log)
		if (l.find(needle) != std::string::npos)
			return true;
	return false;
}

// A ROM image whose first 128 bytes are autoconfig nibbles for a 1 MB
// Zorro II board: type 0xC5 (not inverted), product 0x33, manufacturer
// 0x0202, each stored inverted from byte 0x04 on, as the Zorro convention
// has it.
static std::vector<uint8_t> fake_rom()
{
	std::vector<uint8_t> r(FMV_ROM_BYTES);
	for (size_t i = 0; i < r.size(); i++)
		r[i] = (uint8_t)(i * 7 + 3);
	uint8_t nib[64];
	memset(nib, 0xF, sizeof nib);                 // inverted 0
	nib[0] = 0xC; nib[1] = 0x5;                   // er_Type
	nib[2] = (uint8_t)(~0x3 & 0xF); nib[3] = (uint8_t)(~0x3 & 0xF);   // product 0x33
	nib[8] = 0xF; nib[9] = (uint8_t)(~0x2 & 0xF); nib[10] = 0xF; nib[11] = (uint8_t)(~0x2 & 0xF);
	for (int n = 0; n < 64; n++) {
		r[2 * n] = (uint8_t)(nib[n] << 4);
		r[2 * n + 1] = 0;
	}
	return r;
}

static void pump(fmv_xport *x, wfpga &f, uint32_t us = 0)
{
	fmv_xport_poll(x, us);
	f.service();
}

// ------------------------------------------------------------------ tests

static void test_start()
{
	wfpga f;
	g_log.clear();
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	pump(x, f);
	eq("masks reach the FPGA: HOST_raddr", f.M[0x44], 0x000F);
	eq("reset values reach it: CL450 INT inactive", f.R[0x48] & 0x80, 0x80);
	check(!f.alive(), "not ALIVE before a ROM is loaded");

	std::vector<uint8_t> small(1000);
	check(!fmv_xport_start(x, small.data(), small.size(), 0), "a ROM of the wrong size is refused");
	check(!f.alive(), "and the board stays down");

	std::vector<uint8_t> rom = fake_rom();
	uint32_t crc = crc32_compute(rom.data(), (int)rom.size());
	check(fmv_xport_start(x, rom.data(), rom.size(), crc), "a 256 KB ROM is taken");
	f.service();
	check(memcmp(f.win, rom.data(), rom.size()) == 0, "the ROM image lands at window offset 0, in file order");
	check(f.alive(), "ALIVE once the ROM is in");
	check(logged("unknown dump"), "an unknown dump is named as such");
	check(logged("Zorro II, 1024 KB"), "autoconfig decoded as a 1 MB Zorro II board");

	// AUTOCFG: nibble n in bits 4(n&3) of register 0xD0 + n/4
	for (int n = 0; n < 64; n++) {
		unsigned got = (f.R[FMV_IDX_AUTOCFG + n / 4] >> (4 * (n & 3))) & 0xF;
		if (got != (unsigned)(rom[2 * n] >> 4)) {
			printf("FAIL AUTOCFG nibble %d: got %x want %x\n", n, got, rom[2 * n] >> 4);
			failures++;
			break;
		}
	}
	fmv_autoconfig ac;
	fmv_decode_autoconfig(rom.data(), &ac);
	eq("er_Type", ac.er_type, 0xC5);
	eq("er_Product, un-inverted", ac.er_product, 0x33);
	eq("manufacturer, un-inverted", ac.manufacturer, 0x0202);
	eq("size", ac.size_bytes, 1u << 20);

	fmv_xport_stop(x);
	f.service();
	check(!f.alive(), "stop lowers ALIVE");
	fmv_xport_free(x);
}

static void test_rom_names()
{
	check(fmv_rom_name(0xf11158eb) != 0, "40.22 dump known");
	check(fmv_rom_name(0xc35c37bf) != 0, "40.30 dump known");
	check(fmv_rom_name(0x12345678) == 0, "anything else unknown");
}

static void test_events_end_to_end()
{
	wfpga f;
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	pump(x, f);
	// The CL450 loading sequence and a command, entirely through the window
	f.write(FMV_OFF_CL450 + CL_HOST_RADDR, HMEM_INIT_FLAG);
	f.write(FMV_OFF_CL450 + CL_HOST_RDATA, 0x5A5A);
	f.write(FMV_OFF_CL450 + CL_CPU_CONTROL, 1);
	pump(x, f, 3000);
	f.write(FMV_OFF_CL450 + CL_HOST_RADDR, HMEM_INIT_FLAG);
	eq("HMEM[15] cleared by the model, through the queue", f.read(FMV_OFF_CL450 + CL_HOST_RDATA), 0);

	f.write(FMV_OFF_CL450 + CL_HOST_RADDR, 0);
	f.write(FMV_OFF_CL450 + CL_HOST_RDATA, CLC_SET_THRESHOLD);
	f.write(FMV_OFF_CL450 + CL_HOST_RDATA, 0x0800);
	f.write(FMV_OFF_CL450 + CL_HOST_NEWCMD, 1);
	eq("HOST_newcmd set in the FPGA at once", f.R[0x2B] & 1, 1);
	pump(x, f, 100);
	pump(x, f, 100);
	eq("and cleared by the model once it took the command", f.R[0x2B] & 1, 0);
	fmv_xport_stats st;
	fmv_xport_get_stats(x, &st);
	eq("every event drained exactly once", st.events, f.ev_count);
	eq("HOST_RDIDX published: low half", f.R[FMV_IDX_RDIDX_LO], f.ev_count & 0xFFFF);
	fmv_xport_free(x);
}

static void test_seq_not_landed()
{
	wfpga f;
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	pump(x, f);
	f.write(FMV_OFF_L64111 + 2 * 1, 0x01);       // entry 0
	f.write(FMV_OFF_L64111 + 2 * 3, 0x81);       // entry 1
	// Entry 1 has not landed yet: its SEQ still holds an old value
	uint32_t off1 = FMV_WIN_EV + 8 * 1;
	uint64_t real = rd64(f.win, off1);
	wr64(f.win, off1, (real & 0x0000FFFFFFFFFFFFull) | (0xFFFFull << 48));
	pump(x, f);
	fmv_xport_stats st;
	fmv_xport_get_stats(x, &st);
	eq("stops at the entry that has not landed", st.events, 1);
	check(st.not_landed >= 1, "and counts the wait");
	eq("HOST_RDIDX only covers what was taken", f.R[FMV_IDX_RDIDX_LO], 1);
	wr64(f.win, off1, real);                     // now it lands
	pump(x, f);
	fmv_xport_get_stats(x, &st);
	eq("taken on the next pass, not skipped", st.events, 2);
	check(logged("reg  3 = 0x81"), "the late entry reached the model");
	fmv_xport_free(x);
}

static void test_backlog_counted()
{
	// The 68k is held while the ring is full, so a drain that finds it near
	// full says Main was the slow side (run v26).
	wfpga f;
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	pump(x, f);
	fmv_xport_stats st;
	fmv_xport_get_stats(x, &st);
	uint32_t d0 = st.drains;
	for (int i = 0; i < 4050; i++)
		f.write(FMV_OFF_CMEM, 0x0000);
	pump(x, f);
	fmv_xport_get_stats(x, &st);
	eq("one drain more", st.drains, d0 + 1);
	eq("it found the ring near full", st.near_full, 1);
	check(st.backlog_max >= 4050, "the most found waiting is recorded");
	fmv_xport_clear_max(x);
	for (int i = 0; i < 10; i++)
		f.write(FMV_OFF_CMEM, 0x0000);
	pump(x, f);
	fmv_xport_get_stats(x, &st);
	eq("a short backlog is not near full", st.near_full, 1);
	eq("the max restarts after a clear", st.backlog_max, 10);
	fmv_xport_free(x);
}

static void test_queue_never_overrun()
{
	wfpga f;
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	// The FPGA applies nothing; Amiga resets make the model re-push its
	// masks and reset values each time, far more than the queue holds.
	for (int i = 0; i < 6; i++)
		f.amiga_reset();
	for (int i = 0; i < 4; i++) {
		fmv_xport_poll(x, 0);
		uint32_t w = (uint32_t)rd64(f.win, FMV_WIN_UQ_WPTR);
		uint32_t r = (uint32_t)rd64(f.win, FMV_WIN_UQ_RDPTR);
		check(w - r <= FMV_UQ_ENTRIES, "UQ_WPTR - UQ_RDPTR never exceeds 1024");
	}
	fmv_xport_stats st;
	fmv_xport_get_stats(x, &st);
	check(st.pending > 0 && st.uq_waits > 0, "the rest waits in Main");
	// Once the FPGA reads, everything goes through, in order
	for (int i = 0; i < 20 && st.pending; i++) {
		pump(x, f);
		fmv_xport_get_stats(x, &st);
	}
	eq("nothing left waiting", st.pending, 0);
	eq("every update applied", f.applied, st.updates);
	eq("final state is the reset state: CR3", f.R[FMV_IDX_L64111 + 3] & 0xFF, 0x80);
	fmv_xport_free(x);
}

static void test_back_pressure_sees_main()
{
	wfpga f;
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	pump(x, f);
	// Main stops polling; the 68k streams data-port words
	int posted = 0;
	while (!f.ring_full() && posted < 10000) {
		f.write(FMV_OFF_CMEM, 0x1234);
		posted++;
	}
	eq("the ring fills at 4095 entries without Main", posted, FMV_EV_ENTRIES - 1);
	check(!f.overran, "no entry was written into a full ring");
	pump(x, f);
	check(!f.ring_full(), "a poll publishes HOST_RDIDX and frees the ring");
	fmv_xport_stats st;
	fmv_xport_get_stats(x, &st);
	eq("all of them drained", st.events, (unsigned)posted);
	eq("no overrun", st.ev_overruns, 0);
	fmv_xport_free(x);
}

static void test_dram_variables()
{
	wfpga f;
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	pump(x, f);
	f.write(FMV_OFF_CL450 + CL_CPU_CONTROL, 1);
	pump(x, f, 3000);
	f.write(FMV_OFF_CL450 + CL_HOST_RADDR, 0);
	f.write(FMV_OFF_CL450 + CL_HOST_RDATA, CLC_PLAY);
	f.write(FMV_OFF_CL450 + CL_HOST_NEWCMD, 1);
	pump(x, f, 100);
	pump(x, f, 100);
	// A sequence header for 352 x 288
	const uint8_t sh[] = { 0, 0, 1, 0xB3, 0x16, 0x01, 0x20, 0x23, 0xFF, 0xFF, 0xE0, 0x18 };
	for (size_t i = 0; i < sizeof sh; i += 2)
		f.write(FMV_OFF_CMEM, (uint16_t)(sh[i] << 8 | sh[i + 1]));
	pump(x, f);
	// HORIZONTAL_SIZE is DRAM word 0x12 [CL table A-2], board RAM in 68k order
	uint32_t o = FMV_OFF_RAM + 2 * 0x12;
	eq("HORIZONTAL_SIZE high byte at the lower address", f.win[o], 0x01);
	eq("HORIZONTAL_SIZE low byte", f.win[o + 1], 0x60);
	fmv_xport_free(x);
}

static void test_stale_window()
{
	// The hardware failure of 2026-11-02: DDR3 is not cleared between core
	// loads, so a fresh core (its counters at 0) faces a window holding
	// garbage, or the pointers of a session that ended. Main must not take
	// them as the FPGA's: a session starts from zero on both sides.
	wfpga f;
	for (size_t i = 0; i < f.mem.size(); i++)
		f.mem[i] = (uint8_t)(0xA5 ^ (i * 13));
	wr64(f.win, FMV_WIN_EV_WPTR, 300);
	wr64(f.win, FMV_WIN_UQ_RDPTR, 777);
	wr64(f.win, FMV_WIN_UQ_WPTR, 779);
	g_log.clear();
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	eq("EV_WPTR zeroed", rd64(f.win, FMV_WIN_EV_WPTR), 0);
	eq("UQ_RDPTR zeroed", rd64(f.win, FMV_WIN_UQ_RDPTR), 0);
	check(rd64(f.win, FMV_WIN_UQ_WPTR) <= FMV_UQ_ENTRIES, "UQ_WPTR counts from 0");
	std::vector<uint8_t> rom = fake_rom();
	check(fmv_xport_start(x, rom.data(), rom.size(), 0), "the ROM is taken");
	for (int k = 0; k < 8; k++)
		pump(x, f);
	check(f.alive(), "a fresh core facing a stale window comes ALIVE");
	fmv_xport_stats st;
	fmv_xport_get_stats(x, &st);
	eq("no event invented from the stale ring", st.events, 0);
	f.write(FMV_OFF_CL450 + 2 * 0x48, 0x0080);
	pump(x, f);
	fmv_xport_get_stats(x, &st);
	eq("the first real event is seen", st.events, 1);
	check(logged("core is reading the update queue"), "Main says when the core takes the queue");
	fmv_xport_free(x);
}

static void test_core_not_reading()
{
	// A core whose front end never reads the queue (not loaded, status[64]
	// not reaching it, a dead DDR3 path) is named in the log, not silent.
	wfpga f;
	g_log.clear();
	fmv_xport *x = fmv_xport_new(f.win, logcb, 0);
	std::vector<uint8_t> rom = fake_rom();
	fmv_xport_start(x, rom.data(), rom.size(), 0);
	for (int k = 0; k < 30; k++)
		fmv_xport_poll(x, 100000);      // 3 s, and the FPGA never services
	check(logged("has not taken"), "a stalled queue is reported");
	check(!logged("core is reading the update queue"), "and not reported as read");
	fmv_xport_free(x);
}

static void test_diag_format()
{
	// The core's diag window as hps_ext returns it, one word late (the
	// signature search), stuck in S_QW_D with waitrequest low and no beats.
	uint16_t w[12] = { 0x0000, 0xA5F4, 0xF401,
	                   (uint16_t)(7 << 12 | 1 << 11 | 1 << 9 | 1 << 7 | 0 << 2 | 1),
	                   5, 0, 0xFFFF, 0, 0, 0, 0, 0 };
	char line[400];
	check(fmv_diag_format(w, 12, line, sizeof line) == 1, "diag: decoded");
	check(strstr(line, "fmv_ddr QW_D for 65535+ clocks") != 0, "diag: state and saturated dwell");
	check(strstr(line, "on 1, up_full 0, ev_empty 1") != 0, "diag: flag bits");
	check(strstr(line, "avl read 1 write 0") != 0, "diag: avalon bits");
	check(strstr(line, "ALIVE 0, status[64] 1") != 0, "diag: bus side");
	check(strstr(line, "UQ_WPTR polls 5, read beats 0") != 0, "diag: counters in word order");
	// With the arbiter block: fmv_arb busy, owned by m0 for a read with 1
	// beat outstanding, m1 requesting and held off; q2 holding a read.
	uint16_t wx[22] = { 0x0000, 0xA5F4, 0xF401, w[3], 5, 0, 0xFFFF, 0, 0, 0,
	                    0xA5F5,
	                    (uint16_t)(1 << 15 | 0 << 14 | 1 << 13 | 1 << 12 | 1 << 11 | 1 << 8 | 1),
	                    0x0300,
	                    (uint16_t)(1 << 15 | 1 << 14 | 1 << 11 | 1 << 10 | 2),   // mirror_arb
	                    0, 0, 0x0102, (uint16_t)(0xA0 << 8 | 4), 0x0500 };
	char big[1200];
	check(fmv_diag_format(wx, 22, big, sizeof big) == 1, "diag x: decoded");
	check(strstr(big, "fmv_arb busy 1 owner m0 read, req m0 1 m1 1, wait s 0 m0 0 m1 1, beats 1") != 0,
	      "diag x: fmv_arb fields");
	check(strstr(big, "fmv_arb busy 1 owner m0 read, req m0 1 m1 1, wait s 0 m0 0 m1 1, beats 1, grants m0 3 m1 0") != 0,
	      "diag x: fmv_arb grants");
	check(strstr(big, "ddram_ctrl arb busy 0") != 0 && strstr(big, "grants m0 1 m1 2") != 0,
	      "diag x: ddram_ctrl's arbiter and its grants");
	check(strstr(big, "mirror_arb busy 1 owner m1 write, req m0 0 m1 1, wait s 1 m0 0 m1 0, beats 2, grants m0 5 m1 0") != 0,
	      "diag x: mirror_arb grants from the last word");
	check(strstr(big, "dsnd_arb busy 0 owner m0 write, req m0 0 m1 0, wait s 0 m0 0 m1 0, beats 0, m1 grants 4") != 0,
	      "diag x: dsnd_arb and its m1 grants");
	check(strstr(big, "q2_v 1 (write), ss_port_own 0") != 0, "diag x: q2 bits");
	check(strstr(big, "HPS port") == 0, "diag x: no HPS block without its marker");

	// With the HPS block: 4 commands taken, 4 beats back, the last a read of
	// word 03fe1003 (the A2065's INT_STATE), a read of 03fe1002 on the pins,
	// BUSY high for 65535+ clocks.
	uint16_t wy[31];
	memcpy(wy, wx, sizeof wx);
	// Words in the order the core serves them (ddram_ctrl.v dbg_hps, word 0
	// first): cmds, beats, last {rd,wr,0,a[28:16]}, last a[15:0], pins
	// {rd,wr,0,a[28:16]}, pins a[15:0], BUSY run, {pins bc, last bc}. The
	// first version of this vector had the two address words of each
	// command swapped -- the decoder's own mistake, so it passed; hardware
	// (2026-11-02 ~23:00) is what showed it. The values below are that
	// run's: the last a read of 03fe1003, a write of 03fe1001 on the pins.
	uint16_t yb[9] = { 0xA5F6, 4, 0, (uint16_t)(0x8000 | 0x03FE), 0x1003,
	                   (uint16_t)(0x4000 | 0x03FE), 0x1001, 0xFFFF, 0x0102 };
	memcpy(wy + 19, yb, sizeof yb);
	check(fmv_diag_format(wy, 31, big, sizeof big) == 1, "diag y: decoded");
	check(strstr(big, "HPS port: 4 commands accepted, 0 read beats, last read 03fe1003 (byte 1ff08018) x2") != 0,
	      "diag y: counts, last command, its burstcount");
	check(strstr(big, "on the pins write 03fe1001 (byte 1ff08008) x1, BUSY high for 65535+ clocks") != 0,
	      "diag y: the held command and the BUSY run");
	// The Amiga's interrupt block after 0xA5F7 (gate 1 run v18): INT2 storm
	// or wait. INTENA 0x4008: master and PORTS on; INTREQ 0x0008 with Akiko
	// IRQ up.
	uint16_t wz[34];
	memcpy(wz, wy, sizeof wy);
	wz[28] = 0xA5F7; wz[29] = 0x4008; wz[30] = (uint16_t)(0x8000 | 0x0008);
	check(fmv_diag_format(wz, 34, big, sizeof big) == 1, "diag z: decoded");
	check(strstr(big, "Amiga: INTENA 4008 INTREQ 0008 (PORTS enabled 1, master 1, requested 1), Akiko IRQ 1") != 0,
	      "diag z: INTENA, INTREQ, PORTS and Akiko IRQ");
	check(strstr(big, "68k:") == 0, "diag z: no PC claimed from a short block");
	// With the PC block (run v19's question): PC in the FMV ROM, not halted,
	// a ring of four
	uint16_t wp[44];
	memset(wp, 0, sizeof wp);
	memcpy(wp, wz, sizeof wz);
	uint16_t pcb[11] = { 0x1234, 0x0020, 0x0000,               // PC 0x201234, halt word
	                     0x0040, 0x00FC, 0x0042, 0x00FC,         // ring: fc0040, fc0042,
	                     0x1234, 0x0020, 0x1236, 0x0020 };       //       201234, 201236
	memcpy(wp + 31, pcb, sizeof pcb);
	check(fmv_diag_format(wp, 44, big, sizeof big) == 1, "diag pc: decoded");
	check(strstr(big, "68k: PC 201234 (FMV ROM), halted 0, ring fc0040 fc0042 201234 201236") != 0,
	      "diag pc: PC, region, halt flag and ring in order");
	wp[33] = 0x8000;
	fmv_diag_format(wp, 44, big, sizeof big);
	check(strstr(big, "halted 1") != 0, "diag pc: the halt flag");
	check(fmv_diag_port_dead(wy, 31), "dead port: 4 commands, 0 beats, BUSY held (2026-11-02)");
	wy[19 + 7] = 0;                                    // BUSY not held: the beats rule alone
	check(fmv_diag_port_dead(wy, 31), "dead port: 4 commands, 0 beats, BUSY low (the =4 run)");
	wy[19 + 2] = 4; wy[19 + 7] = 0;                    // 4 beats back, BUSY run 0
	check(!fmv_diag_port_dead(wy, 31), "healthy port: every read answered (2026-11-03)");
	wy[19 + 7] = 0xFFFF;
	check(fmv_diag_port_dead(wy, 31), "dead port: BUSY held 65535+ clocks");
	check(!fmv_diag_port_dead(wx, 22), "no HPS block: no verdict");
	check(fmv_diag_format(wx, 11, big, sizeof big) == 1 && !strstr(big, "fmv_arb"),
	      "diag x: a short read decodes without the arbiter block");
	// Layout 0xF402: word 6 has INT2, the L64111 latch and the CL450 INT on
	// top, UQ_WPTR's low 12 bits below
	// d[0] is index 2: version, flags, polls, beats, dwell, uq_rd, word 6, events
	uint16_t w2[12] = { 0x0000, 0xA5F4, 0xF402, w[3], 5, 0, 0xFFFF, 0,
	                    (uint16_t)(1 << 15 | 1 << 14 | 0x0123), 7, 0, 0 };
	check(fmv_diag_format(w2, 12, line, sizeof line) == 1, "diag F402: decoded");
	check(strstr(line, "UQ_WPTR read 291 (low 12 bits)") != 0, "diag F402: UQ_WPTR without the flag bits");
	check(strstr(line, "FPGA: INT2 1, L64111 latch 1, CL450 INT 0") != 0, "diag F402: the interrupt lines");
	check(fmv_diag_format(w, 12, line, sizeof line) == 1 && !strstr(line, "FPGA: INT2"),
	      "diag F401: no interrupt lines claimed");
	uint16_t offw[12] = { 0x0000, 0xA5F4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	check(fmv_diag_format(offw, 12, line, sizeof line) == 0 && strstr(line, "off in this core"),
	      "diag: a core built without the taps says so");
	uint16_t none[12] = { 0 };
	check(fmv_diag_format(none, 12, line, sizeof line) == 0 && strstr(line, "no window"),
	      "diag: a core without the window says so");
	w[2] = 0xF4FF;
	check(fmv_diag_format(w, 12, line, sizeof line) == 0 && strstr(line, "unknown layout"),
	      "diag: an unknown version is not decoded");
}

static void test_guard_wraps()
{
	struct nullf : fmv_fpga_ref {
		void post(uint32_t, bool, uint16_t, uint8_t) override { ev_count++; }
	} f;
	f.l64_acked = true;
	f.l64_ack_idx = 0xFFFFFFF0u;
	fmv_update raise = { FMV_IDX_L64_INT, false, true, 1, 1 };
	f.apply_update(raise, 0xFFFFFFE0u);
	check(!f.l64_int(), "a raise from before the ack is dropped");
	f.apply_update(raise, 0x00000010u);         // after the counter wrapped
	check(f.l64_int(), "a raise from after the ack is applied across the wrap");
}

int main()
{
	test_backlog_counted();
	test_start();
	test_rom_names();
	test_events_end_to_end();
	test_seq_not_landed();
	test_queue_never_overrun();
	test_back_pressure_sees_main();
	test_dram_variables();
	test_stale_window();
	test_core_not_reading();
	test_guard_wraps();
	test_diag_format();
	if (failures) {
		printf("test_fmv_transport: %d FAILED\n", failures);
		return 1;
	}
	printf("test_fmv_transport: OK\n");
	return 0;
}
