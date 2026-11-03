// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FMV chip model (support/minimig/fmv_model.cpp) against the host
// protocols in the CL450 and L64111 manuals, driven the way the FMV ROM
// drives the real chips: through 68k register accesses.
//
// The accesses go through `fpga`, below, built on fmv_fpga_ref.h: a reference
// implementation of the FPGA side of docs/fmv-contract.md (write masks, HMEM indirection, the
// INT line, the L64111 read acknowledge and the guarded latch). So these
// tests exercise the contract as well as the model, and `fpga` is the
// behaviour the core's RTL bench has to match.
//
// Page references are to the manuals, as in fmv_model.h.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "../support/minimig/fmv_model.h"
#include "fmv_fpga_ref.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static void eq(const char *what, unsigned got, unsigned want)
{
	if (got != want) {
		printf("FAIL %s: got 0x%x want 0x%x\n", what, got, want);
		failures++;
	}
}

// ------------------------------------------------------- the FPGA reference

struct fpga : fmv_fpga_ref {
	uint32_t host_rdidx = 0;     // what Main has consumed (all, here)
	fmv_model *model = nullptr;
	std::vector<std::string> log;
	std::vector<fmv_update> updates;
	uint16_t dram[0x100];

	fpga() { memset(dram, 0, sizeof dram); }

	void apply(const fmv_update &u, uint32_t rdidx)
	{
		updates.push_back(u);
		apply_update(u, rdidx);
	}

	void post(uint32_t off, bool read, uint16_t data, uint8_t haddr) override
	{
		fmv_event e;
		memset(&e, 0, sizeof e);
		e.index = ev_count++;
		e.offset = off;
		e.read = read;
		e.uds = e.lds = true;
		e.data = data;
		e.haddr = haddr;
		fmv_model_event(model, e);
		host_rdidx = ev_count;          // Main drains at once in these tests
	}
};

static fpga *g_fpga;

static void hook_update(void *ctx, const fmv_update &u)
{
	fpga *f = (fpga *)ctx;
	f->apply(u, f->host_rdidx);
}

static void hook_dram(void *ctx, uint32_t waddr, uint16_t v)
{
	fpga *f = (fpga *)ctx;
	if (waddr < 0x100)
		f->dram[waddr] = v;
}

static void hook_log(void *ctx, const char *line)
{
	((fpga *)ctx)->log.push_back(line);
}

static void attach(fpga &f)
{
	fmv_hooks h = { &f, hook_update, hook_dram, hook_log };
	f.model = fmv_model_new(h);
	g_fpga = &f;
}

static bool logged(const fpga &f, const char *needle)
{
	for (auto &l : f.log)
		if (l.find(needle) != std::string::npos)
			return true;
	return false;
}

// ---------------------------------------------------------- CL450 helpers

enum { CL = FMV_OFF_CL450 };

static void cl_boot(fpga &f)
{
	// The loading sequence [CL 10-9]: HMEM[15] set non-zero as the
	// "initialisation complete" flag, CPU_pc, then CPU_control = 1.
	f.write(CL + CL_HOST_RADDR, HMEM_INIT_FLAG);
	f.write(CL + CL_HOST_RDATA, 0x1234);
	f.write(CL + CL_CPU_PC, 0x0100);
	f.write(CL + CL_CPU_CONTROL, 1);
}

static uint16_t hmem_read(fpga &f, int a)
{
	f.write(CL + CL_HOST_RADDR, (uint16_t)a);    // must re-write before each read [CL 8-14]
	return f.read(CL + CL_HOST_RDATA);
}

// Issue a macro command as figure 11-1 does, and let the model take it.
static void cl_command(fpga &f, uint16_t code, uint16_t a1 = 0, uint16_t a2 = 0,
                       uint16_t a3 = 0, uint16_t a4 = 0)
{
	check(!(f.read(CL + CL_HOST_NEWCMD) & 1), "HOST_newcmd clear before a command");
	f.write(CL + CL_HOST_RADDR, 0);
	f.write(CL + CL_HOST_RDATA, code);
	f.write(CL + CL_HOST_RDATA, a1);
	f.write(CL + CL_HOST_RDATA, a2);
	f.write(CL + CL_HOST_RDATA, a3);
	f.write(CL + CL_HOST_RDATA, a4);
	f.write(CL + CL_HOST_NEWCMD, 1);
	check(f.read(CL + CL_HOST_NEWCMD) & 1, "HOST_newcmd reads 1 until the command is taken");
	fmv_model_advance(f.model, 100);
}

// A synthetic MPEG-1 video elementary stream: a sequence header (352x288,
// picture_rate 3 = 25 Hz), a GOP header, and `pictures` pictures of
// `size` bytes each, I then P. Filler bytes contain no start codes.
static std::vector<uint8_t> video_es(int pictures, int size)
{
	std::vector<uint8_t> v = { 0, 0, 1, 0xB3, 0x16, 0x01, 0x20, 0x23, 0xFF, 0xFF, 0xE0, 0x18 };
	const uint8_t gop[] = { 0, 0, 1, 0xB8, 0x00, 0x08, 0x00, 0x00 };
	v.insert(v.end(), gop, gop + sizeof gop);
	for (int p = 0; p < pictures; p++) {
		uint8_t type = p == 0 ? 1 : 2;
		const uint8_t ph[] = { 0, 0, 1, 0x00, (uint8_t)(p >> 2), (uint8_t)(((p & 3) << 6) | (type << 3)) };
		v.insert(v.end(), ph, ph + sizeof ph);
		for (int i = 6; i < size; i++)
			v.push_back(0x55);
	}
	const uint8_t end[] = { 0, 0, 1, 0xB7 };
	v.insert(v.end(), end, end + sizeof end);
	if (v.size() & 1)
		v.push_back(0);
	return v;
}

static void cl_send(fpga &f, const std::vector<uint8_t> &es, size_t from, size_t to)
{
	for (size_t i = from; i < to && i + 1 < es.size() + 1; i += 2)
		f.write(FMV_OFF_CMEM, (uint16_t)(es[i] << 8 | (i + 1 < es.size() ? es[i + 1] : 0)));
}

// ------------------------------------------------------------------- tests

static void test_reset_state()
{
	fpga f;
	attach(f);
	// Masks from the manuals' R/W columns
	eq("HOST_raddr mask", f.M[CL_HOST_RADDR >> 1], 0x000F);
	eq("HOST_control mask: AIC, VIE, Int", f.M[CL_HOST_CONTROL >> 1], 0xC080);
	eq("HOST_newcmd mask", f.M[CL_HOST_NEWCMD >> 1], 0x0001);
	eq("CMEM_dmactrl mask: enables and DE only", f.M[CL_CMEM_DMACTRL >> 1], 0x001F);
	eq("HMEM writable", f.M[FMV_IDX_HMEM + 3], 0xFFFF);
	eq("CMEM_status read-only", f.M[CL_CMEM_STATUS >> 1], 0);
	eq("L64111 CR1 writable, low byte", f.M[FMV_IDX_L64111 + 1], 0x00FF);
	eq("L64111 status 2 not writable", f.M[FMV_IDX_L64111 + 5], 0);
	// Reset values
	check(!f.cl_int(), "CL450 INT inactive after reset: Int = 1 [CL table 10-6]");
	check(!f.int2(), "no INT2 after reset");
	eq("L64111 CR3 ignores the stream ID [L64 4-8]", f.R[FMV_IDX_L64111 + 3] & 0xFF, 0x80);
	eq("CMEM empty: all four empty bits", f.R[CL_CMEM_DMACTRL >> 1] & 0x01E0, 0x01E0);
	// The IO word's interrupt bits follow the lines, active low
	uint16_t io = f.read(FMV_OFF_IO);
	eq("IO: CL450 int bit high while inactive", io & 0x8000, 0x8000);
	eq("IO: L64111 int bit high while inactive", io & 0x4000, 0x4000);
	// The whole idle word is WinUAE's: CFLEVEL 1 (CMEM has room, no enable
	// set), FALF 0 (gate 1, 2026-11-03: the ROM read 0xc400 here before)
	eq("IO idle word as WinUAE reads it", io, 0xC800);
	fmv_model_free(f.model);
}

static void test_hmem_indirection()
{
	fpga f;
	attach(f);
	// Writes auto-increment the pointer; reads need it rewritten [CL 8-13].
	f.write(CL + CL_HOST_RADDR, 2);
	f.write(CL + CL_HOST_RDATA, 0xAAAA);
	f.write(CL + CL_HOST_RDATA, 0xBBBB);
	eq("HAddr incremented twice", f.R[0x44] & 15, 4);
	eq("HMEM[2] read back", hmem_read(f, 2), 0xAAAA);
	eq("HMEM[3] read back", hmem_read(f, 3), 0xBBBB);
	// The model's mirror agrees with the FPGA's register file
	eq("model mirror HMEM[3]", fmv_model_reg(f.model, FMV_IDX_HMEM + 3), 0xBBBB);
	fmv_model_free(f.model);
}

static void test_boot_and_commands()
{
	fpga f;
	attach(f);
	cl_boot(f);
	eq("HMEM[15] holds the host's flag while initialising", hmem_read(f, HMEM_INIT_FLAG), 0x1234);
	// Commands are not taken before the microapplication runs
	f.write(CL + CL_HOST_NEWCMD, 1);
	fmv_model_advance(f.model, 500);
	check(f.read(CL + CL_HOST_NEWCMD) & 1, "no command taken during initialisation");
	fmv_model_advance(f.model, 2000);
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	check(s.cl_running, "microapplication running after initialisation");
	eq("HMEM[15] cleared: initialisation complete [CL 10-9]", hmem_read(f, HMEM_INIT_FLAG), 0);
	eq("HOST_newcmd cleared by initialisation [CL table 10-1]", f.read(CL + CL_HOST_NEWCMD) & 1, 0);
	eq("HOST_scr2 default: CS=1, Div=444 [CL table 10-1]", f.R[CL_HOST_SCR2 >> 1], 0x1DE0);
	fmv_model_advance(f.model, 1000);
	fmv_model_get_state(f.model, &s);
	eq("the command written during initialisation is never executed", s.cl_commands, 0);

	cl_command(f, CLC_SET_THRESHOLD, 2048);
	check(!(f.read(CL + CL_HOST_NEWCMD) & 1), "SetThreshold taken: HOST_newcmd cleared");
	check(logged(f, "command SetThreshold (0x0103) 0800"), "SetThreshold logged with its argument");

	cl_command(f, CLC_SET_WINDOW, 0, 0, 352, 288);
	fmv_model_get_state(f.model, &s);
	eq("SetWindow width (IDLE runs low-priority commands at once)", s.cl_window[2], 352);

	// AccessSCR: write, then read back through HMEM [CL 11-12]
	cl_command(f, CLC_ACCESS_SCR, 0x0001, 0x0002, 0x0003);     // write
	cl_command(f, CLC_ACCESS_SCR, 0x8000);                     // read
	eq("AccessSCR read: SCR bits 32:30", hmem_read(f, 1) & 7, 1);
	eq("AccessSCR read: SCR bits 29:15", hmem_read(f, 2) & 0x7FFF, 2);
	check((hmem_read(f, 3) & 0x7FFF) >= 3, "AccessSCR read: SCR bits 14:0 counted on");

	cl_command(f, 0x7777);
	check(logged(f, "UNKNOWN (0x7777)"), "unknown function code logged");
	fmv_model_free(f.model);
}

static void test_play_and_interrupts()
{
	fpga f;
	attach(f);
	cl_boot(f);
	fmv_model_advance(f.model, 3000);
	f.write(CL + CL_HOST_CONTROL, 0x0080);       // non-vectored, Int inactive
	cl_command(f, CLC_SET_INT_MASK, CLI_PIC_D | CLI_SEQ_D);
	cl_command(f, CLC_PLAY);
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	eq("Play() from IDLE enters PLAY-SETUP [CL 11-6]", s.cl_state, 1);

	std::vector<uint8_t> es = video_es(10, 1000);
	cl_send(f, es, 0, es.size());
	fmv_model_get_state(f.model, &s);
	eq("all of the stream in the buffer", s.cl_buffer_bytes, (unsigned)es.size());
	eq("sequence header: width", s.cl_width, 352);
	eq("sequence header: height", s.cl_height, 288);
	eq("sequence header: 25 Hz", s.cl_frame_period_us, 40000);
	eq("HORIZONTAL_SIZE written to DRAM [CL table A-2]", f.dram[0x12], 352);

	// One picture per frame period
	fmv_model_advance(f.model, 2000);
	fmv_model_get_state(f.model, &s);
	eq("PLAY after setup and the first picture", s.cl_state, 2);
	check(f.cl_int() && f.int2(), "INT asserted");
	uint16_t st = hmem_read(f, HMEM_INT_STATUS);
	check(st & CLI_SEQ_D, "Interrupt Status has SEQ-D, the first event after the sequence header");
	check(!(st & CLI_PIC_V), "PIC-V: not raised per picture (WinUAE), and not enabled here");

	// Host service sequence [CL 12-37]: set Int to 1, then clear status.
	f.write(CL + CL_HOST_CONTROL, 0x0080);
	check(!f.cl_int(), "INT drops the moment the host sets Int: no wait on Main");
	fmv_model_advance(f.model, 100000);
	check(!f.cl_int(), "no new INT while Interrupt Status is non-zero [CL 12-7]");
	f.write(CL + CL_HOST_RADDR, HMEM_INT_STATUS);
	f.write(CL + CL_HOST_RDATA, 0);
	fmv_model_advance(f.model, 1000);
	check(f.cl_int(), "queued interrupts posted once the host clears Interrupt Status");
	check(hmem_read(f, HMEM_INT_STATUS) & CLI_PIC_D, "the display tick's PIC-D was queued behind it");

	fmv_model_get_state(f.model, &s);
	check(s.cl_pictures >= 3 && s.cl_pictures <= 4, "about one picture per 40 ms");
	fmv_model_advance(f.model, 1000000);
	fmv_model_get_state(f.model, &s);
	eq("every picture decoded", s.cl_pictures, 10);
	check(logged(f, "underflow"), "underflow once the buffer runs dry");

	// InquireBufferFullness: HMEM[0xB] in bytes (WinUAE; gate 1 2026-11-03)
	f.write(CL + CL_HOST_CONTROL, 0x0080);
	f.write(CL + CL_HOST_RADDR, HMEM_INT_STATUS);
	f.write(CL + CL_HOST_RDATA, 0);
	cl_send(f, es, 0, 400);
	cl_command(f, CLC_INQUIRE_FULLNESS);
	fmv_model_get_state(f.model, &s);
	eq("InquireBufferFullness in HMEM[0xB], bytes", hmem_read(f, HMEM_FULLNESS), s.cl_buffer_bytes);
	cl_command(f, CLC_FLUSH, 3);
	eq("FlushBitstream empties the buffer [CL 11-16]", hmem_read(f, HMEM_FULLNESS), 0);
	fmv_model_free(f.model);
}

static void test_cmem_in_idle()
{
	fpga f;
	attach(f);
	cl_boot(f);
	fmv_model_advance(f.model, 3000);
	// IDLE accepts no bitstream data [CL 11-6]: it waits in CMEM.
	for (int i = 0; i < 8; i++)
		f.write(FMV_OFF_CMEM, 0x1111);
	eq("8 words in CMEM: 1Q 2Q set, 3Q 4Q clear", f.R[CL_CMEM_DMACTRL >> 1] & 0x01E0, 0x0180);
	eq("CMEM_status difference counter", (f.R[CL_CMEM_STATUS >> 1] >> 8) & 0x1F, 8);
	for (int i = 0; i < 4; i++)
		f.write(FMV_OFF_CMEM, 0x1111);
	eq("12 words: exactly 4 empty, 1Q only", f.R[CL_CMEM_DMACTRL >> 1] & 0x01E0, 0x0100);
	f.write(FMV_OFF_CMEM, 0x1111);
	eq("13 words: 3 empty, no bits [CL table 8-5]", f.R[CL_CMEM_DMACTRL >> 1] & 0x01E0, 0);
	for (int i = 0; i < 3; i++)
		f.write(FMV_OFF_CMEM, 0x1111);
	eq("CMEM full: no empty bits", f.R[CL_CMEM_DMACTRL >> 1] & 0x01E0, 0);
	f.write(FMV_OFF_CMEM, 0x1111);
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	eq("a write past full CMEM is lost and counted", s.cl_cmem_overflow, 1);
	// CFLEVEL on 1Q via 1QE [CL table 8-6]; the board shows it in the IO word
	f.write(CL + CL_CMEM_DMACTRL, 0x0010);
	eq("CFLEVEL low while CMEM is full", f.read(FMV_OFF_IO) & 0x0800, 0);
	f.write(CL + CL_CMEM_CONTROL, 0x0003);     // CRst
	eq("CRst empties CMEM: CFLEVEL asserted", f.read(FMV_OFF_IO) & 0x0800, 0x0800);
	fmv_model_free(f.model);
}

static void test_und_starts_the_stream()
{
	// Gate 1 runs v12/v13b/v15 (2026-11-03), and WinUAE as the oracle:
	// after Play with nothing sent, the audio tick posts UND. This ROM's
	// ISR reads 0x0100 from HMEM[0xA] and starts sending. Status-0
	// interrupts alone (v15) never start it. The ROM never clears the
	// status, and neither does WinUAE, so it stays.
	fpga f;
	attach(f);
	cl_boot(f);
	fmv_model_advance(f.model, 3000);
	f.write(CL + CL_HOST_CONTROL, 0x0081);
	cl_command(f, CLC_SET_INT_MASK, CLI_UND | CLI_PIC_V | CLI_RDY | 0x0801);
	cl_command(f, CLC_PLAY);
	fmv_model_advance(f.model, 20000);
	eq("UND at Play with the buffer empty: the start signal", hmem_read(f, HMEM_INT_STATUS), CLI_UND);
	check(f.cl_int(), "INT asserted for it");
	f.write(CL + CL_HOST_CONTROL, 0x0081);        // the ISR's acknowledge
	fmv_model_advance(f.model, 100000);
	check(!f.cl_int(), "a latched status blocks every later post (no status-0 INTs either)");
	eq("the status stays 0x0100: nothing clears it but the host", hmem_read(f, HMEM_INT_STATUS), CLI_UND);
	std::vector<uint8_t> es = video_es(10, 1000);
	cl_send(f, es, 0, 400);
	fmv_model_advance(f.model, 20000);
	cl_send(f, es, 400, es.size());
	fmv_model_advance(f.model, 100000);
	fmv_model_state st;
	fmv_model_get_state(f.model, &st);
	check(st.cl_pictures > 0, "pictures decode once the data is in");
	check(!(hmem_read(f, HMEM_INT_STATUS) & CLI_PIC_V), "no PIC-V latched per picture, though enabled");
	// Host clears the status while data is buffered. The first post after
	// it carries what was pending from the empty ticks before the data came
	// (WinUAE: pending bits wait for the next post); clear that too, and
	// then nothing new is raised while the buffer holds data.
	f.write(CL + CL_HOST_RADDR, HMEM_INT_STATUS);
	f.write(CL + CL_HOST_RDATA, 0);
	fmv_model_advance(f.model, 1000);
	f.write(CL + CL_HOST_RADDR, HMEM_INT_STATUS);
	f.write(CL + CL_HOST_RDATA, 0);
	f.write(CL + CL_HOST_CONTROL, 0x0081);
	size_t n0 = f.log.size();
	fmv_model_advance(f.model, 30000);
	bool und_again = false;
	for (size_t i = n0; i < f.log.size(); i++)
		if (f.log[i].find("underflow") != std::string::npos) und_again = true;
	check(!und_again, "no UND logged while the buffer holds data");
	check(!(hmem_read(f, HMEM_INT_STATUS) & CLI_UND), "no UND posted while the buffer holds data");
	// Run it dry: UND again, and on every empty tick, not once per spell
	fmv_model_advance(f.model, 1000000);
	f.write(CL + CL_HOST_RADDR, HMEM_INT_STATUS);
	f.write(CL + CL_HOST_RDATA, 0);
	fmv_model_advance(f.model, 20000);
	eq("UND once the buffer has run dry", hmem_read(f, HMEM_INT_STATUS) & CLI_UND, CLI_UND);
	f.write(CL + CL_HOST_RADDR, HMEM_INT_STATUS);
	f.write(CL + CL_HOST_RDATA, 0);
	fmv_model_advance(f.model, 20000);
	eq("and again after the host clears it, while still dry", hmem_read(f, HMEM_INT_STATUS) & CLI_UND, CLI_UND);
	fmv_model_free(f.model);
}

static void test_rdy()
{
	// RDY: in NewPacket mode, when the buffer has fallen below the
	// SetThreshold level (WinUAE, the oracle). Not before any NewPacket,
	// and not while the buffer is above the threshold.
	fpga f;
	attach(f);
	cl_boot(f);
	fmv_model_advance(f.model, 3000);
	f.write(CL + CL_HOST_CONTROL, 0x0081);
	cl_command(f, CLC_SET_THRESHOLD, 0x0800);      // 2048 bytes
	cl_command(f, CLC_SET_INT_MASK, CLI_RDY);
	cl_command(f, CLC_PLAY);
	std::vector<uint8_t> es = video_es(30, 1000);
	cl_send(f, es, 0, 4000);                       // above the threshold
	fmv_model_advance(f.model, 3000);
	check(!(hmem_read(f, HMEM_INT_STATUS) & CLI_RDY), "no RDY without a NewPacket, even in PLAY");
	cl_command(f, CLC_NEW_PACKET, 0x0400);
	fmv_model_advance(f.model, 3000);
	fmv_model_state st;
	fmv_model_get_state(f.model, &st);
	check(st.cl_buffer_bytes >= 2048, "buffer above the threshold");
	check(!(hmem_read(f, HMEM_INT_STATUS) & CLI_RDY), "no RDY while the buffer is above the threshold");
	// Decoded down to the last picture (about 1000 bytes, still incomplete)
	fmv_model_advance(f.model, 400000);
	check(f.cl_int() && (hmem_read(f, HMEM_INT_STATUS) & CLI_RDY),
	      "RDY once the buffer has run below the threshold");
	fmv_model_free(f.model);
}

static void test_activity()
{
	// The 68k's accesses since the last line, including the polls the log
	// leaves out (gate 1, 2026-11-03: quiet after SetThreshold)
	fpga f;
	attach(f);
	char a[512];
	fmv_model_activity(f.model, a, sizeof a);           // clear
	for (int i = 0; i < 3; i++) f.read(CL + CL_HOST_NEWCMD);
	f.read(CL + CL_CMEM_DMACTRL);
	f.read(CL + CL_HOST_CONTROL);
	f.read(CL + CL_HOST_CONTROL);
	f.read(FMV_OFF_IO);
	fmv_model_activity(f.model, a, sizeof a);
	check(strstr(a, "7 board accesses") != 0, "activity: every access counted");
	check(strstr(a, "HOST_newcmd 3, CMEM_dmactrl 1, HOST_control 2") != 0,
	      "activity: the unlogged polls, per register");
	check(strstr(a, "microcode not running") != 0, "activity: CL450 state shown");
	check(strstr(a, "L64111 INTR 0, status 1 0x00 2 0x00") != 0, "activity: L64111 state shown");
	fmv_model_activity(f.model, a, sizeof a);
	check(strstr(a, " 0 board accesses") != 0, "activity: counts reset after each line");
	fmv_model_free(f.model);
}

static void test_byte_swap()
{
	fpga f;
	attach(f);
	cl_boot(f);
	fmv_model_advance(f.model, 3000);
	f.write(CL + CL_CMEM_CONTROL, 0x0022);     // BS, CRE [CL 8-8]
	cl_command(f, CLC_PLAY);
	// The sequence header, byte-swapped word by word
	std::vector<uint8_t> es = video_es(2, 100);
	for (size_t i = 0; i + 1 < es.size(); i += 2)
		f.write(FMV_OFF_CMEM, (uint16_t)(es[i + 1] << 8 | es[i]));
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	// 288 and 25 Hz are not the defaults (240, 30 Hz [CL table 10-4]), so
	// these only pass if the header really was parsed.
	eq("byte-swapped stream parsed: height", s.cl_height, 288);
	eq("byte-swapped stream parsed: 25 Hz", s.cl_frame_period_us, 40000);
	fmv_model_free(f.model);
}

// ------------------------------------------------------------- L64111

enum { L64 = FMV_OFF_L64111 };

static void l64_put(fpga &f, const std::vector<uint8_t> &b)
{
	for (uint8_t x : b)
		f.write(L64 + 0, x);                    // 8-bit data register [L64 4-4]
}

// A Layer II frame header: 224 kbit/s, 44.1 kHz, stereo, no CRC, no padding.
// Frame length 144000 * 224 / 44100 = 731 bytes.
static std::vector<uint8_t> mp2_frame()
{
	std::vector<uint8_t> f = { 0xFF, 0xFD, 0xB0, 0x04 };
	f.resize(731, 0x00);
	return f;
}

// An ISO 11172-1 audio packet: stuffing, STD buffer, PTS, payload.
static std::vector<uint8_t> audio_packet(const std::vector<uint8_t> &payload, uint64_t pts)
{
	std::vector<uint8_t> h = { 0xFF, 0xFF, 0x40, 0x20,
	                           (uint8_t)(0x21 | ((pts >> 29) & 0x0E)), (uint8_t)(pts >> 22),
	                           (uint8_t)((pts >> 14) | 1), (uint8_t)(pts >> 7), (uint8_t)((pts << 1) | 1) };
	size_t len = h.size() + payload.size();
	std::vector<uint8_t> p = { 0, 0, 1, 0xC0, (uint8_t)(len >> 8), (uint8_t)len };
	p.insert(p.end(), h.begin(), h.end());
	p.insert(p.end(), payload.begin(), payload.end());
	return p;
}

static std::vector<uint8_t> pack_header()
{
	return { 0, 0, 1, 0xBA, 0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x1B, 0x91 };
}

static void test_l64111()
{
	fpga f;
	attach(f);
	// Enable SYNC SYS, SYNC AUD, PTS AVAILABLE and NEW FRAME OUT. The mask
	// positions are one above the status positions [L64 4-10, 4-11].
	f.write(L64 + 2 * 5, (1 << 6) | (1 << 5) | (1 << 4) | (1 << 1));
	std::vector<uint8_t> frame = mp2_frame();
	for (int p = 0; p < 4; p++) {
		l64_put(f, pack_header());
		l64_put(f, audio_packet(frame, 90000u * (uint64_t)p + 0x1ABCDEF01ull));
	}
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	check(s.l64_sync_sys, "SYNC SYS after three packs [L64 4-11]");
	check(s.l64_sync_aud, "SYNC AUD after three frames in place [L64 4-11]");
	eq("four frames detected", s.l64_frames_in, 4);
	eq("channel buffer holds them", s.l64_cb_frames, 4);
	eq("sample rate from the header", s.l64_sample_rate, 44100);
	eq("parametric word 1: ID, layer II, no CRC, index 11 [L64 4-14]", f.R[FMV_IDX_L64111 + 9] & 0xFF,
	   0x80 | (2 << 5) | (1 << 4) | 0xB);
	eq("parametric word 2: 44.1 kHz, stereo, no padding [L64 4-15]", f.R[FMV_IDX_L64111 + 10] & 0xFF, 0x00);
	check(f.l64_int() && f.int2(), "L64111 INT on an enabled source");
	eq("PTS bit 32 [L64 4-17]", f.R[FMV_IDX_L64111 + 16] & 1, 1);
	// The registers hold the latest stamp: each one overwrites the last [L64 4-17]
	eq("PTS bits 7:0 of the last packet", f.R[FMV_IDX_L64111 + 12] & 0xFF,
	   (unsigned)((0x1ABCDEF01ull + 3 * 90000ull) & 0xFF));

	// Every raise of the latch is guarded (fmv-contract.md, update queue)
	bool all_guarded = true, any = false;
	for (auto &u : f.updates)
		if (u.index == FMV_IDX_L64_INT && !u.maskfile && (u.value & 1)) {
			any = true;
			all_guarded &= u.guard;
		}
	check(any && all_guarded, "L64111 latch raised only through guarded updates");

	uint16_t st2 = f.read(L64 + 2 * 5);
	check(!f.l64_int(), "reading status 2 deasserts INTR [L64 4-9]");
	check(st2 & 0x10, "status 2: SYNC AUD");
	check(st2 & 0x08, "status 2: SYNC SYS");
	check(st2 & 0x20, "status 2: PTS AVAILABLE");
	check(st2 & 0x04, "status 2: FRAME DETECT IN");
	eq("channel buffer status counter [L64 4-18]", f.read(L64 + 2 * 18) & 0x7F, 4);

	// Play: STARTI [L64 4-6]. 1152 samples at 44.1 kHz is 26.12 ms a frame.
	fmv_model_advance(f.model, 100000);
	fmv_model_get_state(f.model, &s);
	eq("nothing plays before STARTI", s.l64_frames_out, 0);
	f.write(L64 + 2 * 1, 0x01);
	fmv_model_advance(f.model, 53000);
	fmv_model_get_state(f.model, &s);
	eq("two frames out in 53 ms", s.l64_frames_out, 2);
	check(f.l64_int(), "NEW FRAME OUT interrupts (mask bit 1)");
	fmv_model_advance(f.model, 100000);
	fmv_model_get_state(f.model, &s);
	eq("all four played", s.l64_frames_out, 4);

	// Soft reset clears itself [L64 4-5]
	f.write(L64 + 2 * 1, 0x02);
	eq("SOFT_RST self-clears", f.R[FMV_IDX_L64111 + 1] & 0x02, 0);
	fmv_model_get_state(f.model, &s);
	check(!s.l64_sync_aud, "soft reset drops synchronisation");
	fmv_model_free(f.model);
}

static void test_l64111_level_and_pts()
{
	// Gate 1 run v16, 2026-11-03: status 2 held 0x38, mask 2 was 0x42, INTR
	// stayed 0, and the game waited for it to resume the CD. As WinUAE (the
	// oracle) has it: INTR is the level of status & mask, a read clears the
	// whole register, and PTS AVAILABLE comes with every audio PTS once
	// synchronised.
	fpga f;
	attach(f);
	std::vector<uint8_t> frame = mp2_frame();
	for (int p = 0; p < 4; p++) {
		l64_put(f, pack_header());
		l64_put(f, audio_packet(frame, 90000u * (uint64_t)p + 0x100000ull));
	}
	check(!f.l64_int(), "no INTR with every mask off");
	check(f.R[FMV_IDX_L64111 + 5] & 0x20, "PTS AVAILABLE set");
	f.write(L64 + 2 * 5, 0x42);                  // the game's mask: NEW FRAME OUT, PTS AVAILABLE
	check(f.l64_int(), "a mask written after the status raises INTR at once (a level)");
	uint16_t st2 = f.read(L64 + 2 * 5);
	check(st2 & 0x20, "the read reports PTS AVAILABLE");
	check(!f.l64_int(), "and drops INTR");
	eq("the read clears all of status 2, sync bits included", f.R[FMV_IDX_L64111 + 5] & 0xFF, 0);
	l64_put(f, pack_header());
	l64_put(f, audio_packet(frame, 0x200000ull));
	check(f.l64_int(), "the next audio PTS raises it again: every PTS, not once");
	f.read(L64 + 2 * 5);
	f.write(L64 + 2 * 5, 0x00);
	l64_put(f, pack_header());
	l64_put(f, audio_packet(frame, 0x300000ull));
	check(!f.l64_int(), "nothing with the mask off");
	f.write(L64 + 2 * 4, 0x00);                  // a mask-1 write re-evaluates too: still nothing
	check(!f.l64_int(), "and a mask 1 write with nothing set in status 1 leaves it low");
	fmv_model_free(f.model);
}

static void test_l64111_masks()
{
	// Status 2 bit 0 has two masks: LAST FRAME alone (mask bit 0) must not
	// be enabled by the NEW FRAME OUT mask, and vice versa [L64 4-10].
	fpga f;
	attach(f);
	f.write(L64 + 2 * 5, 0x01);                 // LAST FRAME only
	std::vector<uint8_t> frame = mp2_frame();
	for (int p = 0; p < 3; p++)
		l64_put(f, audio_packet(frame, 0));
	f.write(L64 + 2 * 1, 0x01);
	fmv_model_advance(f.model, 27000);          // one frame out, two left
	check(!f.l64_int(), "NEW FRAME OUT does not interrupt on the LAST FRAME mask");
	fmv_model_advance(f.model, 27000);          // second out, one left
	check(f.l64_int(), "LAST FRAME interrupts when one frame is left");
	fmv_model_free(f.model);
}

static void test_l64111_mask_shift()
{
	// One source, one mask bit: SYNC SYS is status 2 bit 3 and is enabled by
	// mask 2 bit 4 [L64 4-10, 4-11]. Only packs are sent, so nothing else can
	// fire, and a model that lines the two up wrongly raises nothing.
	fpga f;
	attach(f);
	f.write(L64 + 2 * 5, 1 << 4);
	for (int p = 0; p < 3; p++)
		l64_put(f, pack_header());
	check(f.l64_int(), "SYNC SYS interrupts on mask 2 bit 4, one above its status bit");
	fmv_model_free(f.model);

	fpga g;
	attach(g);
	g.write(L64 + 2 * 5, 1 << 3);              // the status position: wrong
	for (int p = 0; p < 3; p++)
		l64_put(g, pack_header());
	check(!g.l64_int(), "mask 2 bit 3 does not enable SYNC SYS");
	fmv_model_free(g.model);
}

static void test_reset_event()
{
	fpga f;
	attach(f);
	cl_boot(f);
	fmv_model_advance(f.model, 3000);
	fmv_event e;
	memset(&e, 0, sizeof e);
	e.reset = true;
	fmv_model_event(f.model, e);
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	check(!s.cl_running, "an Amiga reset stops the microapplication");
	check(!f.cl_int(), "and leaves INT inactive");
	fmv_model_free(f.model);
}

int main()
{
	test_reset_state();
	test_hmem_indirection();
	test_boot_and_commands();
	test_play_and_interrupts();
	test_cmem_in_idle();
	test_und_starts_the_stream();
	test_rdy();
	test_activity();
	test_byte_swap();
	test_l64111();
	test_l64111_level_and_pts();
	test_l64111_masks();
	test_l64111_mask_shift();
	test_reset_event();
	if (failures) {
		printf("test_fmv_model: %d FAILED\n", failures);
		return 1;
	}
	printf("test_fmv_model: OK\n");
	return 0;
}
