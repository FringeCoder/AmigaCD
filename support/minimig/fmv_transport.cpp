// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FMV module's DDR3 transport. See fmv_transport.h and
// docs/fmv-contract.md (v4) for the layout every access below follows.

#include "fmv_transport.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <deque>

namespace {

struct pending_update {
	fmv_update u;
	uint32_t   rdidx;     // events Main had consumed when the model made it
};

} // namespace

struct fmv_xport {
	volatile uint8_t *win;
	fmv_log_fn log;
	void *log_ctx;
	fmv_model *model;
	uint32_t ev_rd;          // events consumed
	uint32_t uq_wr;          // update queue entries written
	uint32_t rdidx_pub;      // HOST_RDIDX last published
	bool     alive;
	bool     core_seen;      // UQ_RDPTR has moved: the core reads the queue
	bool     stall_logged;
	uint32_t stall_us;       // how long entries have waited untaken
	std::deque<pending_update> pend;
	fmv_xport_stats st;
};

// ---- window access: 64-bit words, little-endian, as the FPGA sees them ----

static uint64_t rd64(const fmv_xport *x, uint32_t off)
{
	volatile const uint32_t *p = (volatile const uint32_t *)(x->win + off);
	uint32_t lo = p[0], hi = p[1];
	return ((uint64_t)hi << 32) | lo;
}

static void wr64(fmv_xport *x, uint32_t off, uint64_t v)
{
	volatile uint32_t *p = (volatile uint32_t *)(x->win + off);
	p[0] = (uint32_t)v;
	p[1] = (uint32_t)(v >> 32);
}

static void barrier() { __sync_synchronize(); }

static void xlog(fmv_xport *x, const char *fmt, ...)
{
	if (!x->log)
		return;
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	x->log(x->log_ctx, line);
}

// ---- hooks from the model ----

static void hook_update(void *ctx, const fmv_update &u)
{
	fmv_xport *x = (fmv_xport *)ctx;
	pending_update p = { u, x->ev_rd };
	x->pend.push_back(p);
}

// A decoder variable in the CL450's DRAM, which is the board RAM at board
// offset 0x80000 (fmv-contract.md, the board image). Board byte order is the
// 68k's: the high byte first.
static void hook_dram(void *ctx, uint32_t waddr, uint16_t v)
{
	fmv_xport *x = (fmv_xport *)ctx;
	uint32_t o = FMV_OFF_RAM + 2 * waddr;
	if (o + 1 >= 0x100000)
		return;
	x->win[o] = (uint8_t)(v >> 8);
	x->win[o + 1] = (uint8_t)v;
}

static void hook_log(void *ctx, const char *line)
{
	fmv_xport *x = (fmv_xport *)ctx;
	if (x->log)
		x->log(x->log_ctx, line);
}

// ---- the update queue ----

static void flush_updates(fmv_xport *x)
{
	uint32_t rd = (uint32_t)rd64(x, FMV_WIN_UQ_RDPTR);
	uint32_t used = x->uq_wr - rd;
	if (rd != 0 && !x->core_seen && used <= FMV_UQ_ENTRIES) {
		x->core_seen = true;
		xlog(x, "FMV: the core is reading the update queue");
	}
	if (used > FMV_UQ_ENTRIES) {
		// The FPGA cannot have applied more than was written; a value
		// like this is a stale or torn read. Wait for the next pass.
		return;
	}
	bool wrote = false;
	while (!x->pend.empty() && used < FMV_UQ_ENTRIES) {
		const pending_update &p = x->pend.front();
		uint32_t k = x->uq_wr;
		uint32_t off = FMV_WIN_UQ + 16 * (k & (FMV_UQ_ENTRIES - 1));
		uint64_t w0 = (uint64_t)p.u.index | ((uint64_t)p.u.maskfile << 8) |
		              ((uint64_t)p.u.guard << 9) | ((uint64_t)(k & 0xFFFF) << 48);
		uint64_t w1 = (uint64_t)p.u.value | ((uint64_t)p.u.mask << 16) |
		              ((uint64_t)p.rdidx << 32);
		// Word 1 first, then word 0 with SEQ: a matching SEQ means word 1
		// has landed too (fmv-contract.md, the update queue).
		wr64(x, off + 8, w1);
		barrier();
		wr64(x, off, w0);
		x->pend.pop_front();
		x->uq_wr++;
		x->st.updates++;
		used++;
		wrote = true;
	}
	if (wrote) {
		barrier();
		wr64(x, FMV_WIN_UQ_WPTR, x->uq_wr);
	}
	if (!x->pend.empty())
		x->st.uq_waits++;
}

static void publish_rdidx(fmv_xport *x)
{
	if (x->ev_rd == x->rdidx_pub)
		return;
	x->rdidx_pub = x->ev_rd;
	// High half first: back-pressure only uses the low half, so a torn
	// read cannot matter there (fmv-contract.md, the event ring).
	fmv_update hi = { FMV_IDX_RDIDX_HI, false, false, (uint16_t)(x->ev_rd >> 16), 0xFFFF };
	fmv_update lo = { FMV_IDX_RDIDX_LO, false, false, (uint16_t)x->ev_rd, 0xFFFF };
	hook_update(x, hi);
	hook_update(x, lo);
}

// ---- the event ring ----

static void drain_events(fmv_xport *x)
{
	uint32_t wptr = (uint32_t)rd64(x, FMV_WIN_EV_WPTR);
	uint32_t n = wptr - x->ev_rd;
	if (n > FMV_EV_ENTRIES) {
		// The FPGA holds the 68k while the ring is full, so this means Main
		// lost track: skip to the oldest entry the ring still holds.
		x->st.ev_overruns++;
		xlog(x, "FMV event ring overrun: %u behind, skipping to the oldest", n);
		x->ev_rd = wptr - FMV_EV_ENTRIES;
		n = FMV_EV_ENTRIES;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t idx = x->ev_rd;
		uint64_t e = rd64(x, FMV_WIN_EV + 8 * (idx & (FMV_EV_ENTRIES - 1)));
		if ((uint16_t)(e >> 48) != (uint16_t)idx) {
			// WPTR is visible but this entry is not yet: take it next pass,
			// never skip it (fmv-contract.md, SEQ).
			x->st.not_landed++;
			break;
		}
		fmv_event ev;
		memset(&ev, 0, sizeof ev);
		ev.index = idx;
		ev.read  = e & 1;
		ev.uds   = (e >> 1) & 1;
		ev.lds   = (e >> 2) & 1;
		ev.reset = (e >> 3) & 1;
		ev.offset = (uint32_t)(e >> 4) & 0xFFFFF;
		ev.haddr = (uint8_t)((e >> 24) & 0x0F);
		ev.data  = (uint16_t)(e >> 32);
		x->ev_rd++;
		x->st.events++;
		fmv_model_event(x->model, ev);
		// Keep back-pressure honest during a long drain.
		if (!(x->ev_rd & 1023)) {
			publish_rdidx(x);
			flush_updates(x);
		}
	}
}

// ---- public ----

fmv_xport *fmv_xport_new(volatile uint8_t *win, fmv_log_fn log, void *log_ctx)
{
	fmv_xport *x = new fmv_xport();
	x->win = win;
	x->log = log;
	x->log_ctx = log_ctx;
	memset(&x->st, 0, sizeof x->st);
	// A session starts from zero on both sides (fmv-contract.md v4): the
	// core holds its counters at 0 while status[64] is clear, and the caller
	// keeps it clear until fmv_xport_start() has run. Whatever the window
	// holds -- DDR3 is not cleared between core loads -- is not the core's.
	x->ev_rd = 0;
	x->uq_wr = 0;
	wr64(x, FMV_WIN_EV_WPTR, 0);
	wr64(x, FMV_WIN_UQ_RDPTR, 0);
	wr64(x, FMV_WIN_UQ_WPTR, 0);
	barrier();
	x->rdidx_pub = x->ev_rd - 1;     // publish once at the first poll
	fmv_hooks h = { x, hook_update, hook_dram, hook_log };
	x->model = fmv_model_new(h);     // pushes the masks and reset values
	return x;
}

void fmv_xport_free(fmv_xport *x)
{
	if (!x)
		return;
	fmv_model_free(x->model);
	delete x;
}

fmv_model *fmv_xport_model(fmv_xport *x) { return x->model; }

void fmv_decode_autoconfig(const uint8_t *rom, fmv_autoconfig *ac)
{
	// Nibble n is the high nibble of byte 2n. Bytes 0x00/0x02 are read as
	// they are; from 0x04 on they are stored inverted (the Zorro
	// autoconfig convention).
	uint8_t nib[64];
	for (int n = 0; n < 64; n++)
		nib[n] = (uint8_t)(rom[2 * n] >> 4);
	auto inv = [&](int n) { return (uint8_t)(~nib[n] & 0xF); };
	memset(ac, 0, sizeof *ac);
	ac->er_type = (uint8_t)(nib[0] << 4 | nib[1]);
	ac->er_product = (uint8_t)(inv(2) << 4 | inv(3));
	ac->er_flags = (uint8_t)(inv(4) << 4 | inv(5));
	ac->manufacturer = (uint16_t)(inv(8) << 12 | inv(9) << 8 | inv(10) << 4 | inv(11));
	for (int n = 12; n < 20; n++)
		ac->serial = ac->serial << 4 | inv(n);
	ac->zorro2 = (ac->er_type & 0xC0) == 0xC0;
	ac->has_rom = ac->er_type & 0x10;
	static const uint32_t sizes[8] = { 8 << 20, 64 << 10, 128 << 10, 256 << 10,
	                                   512 << 10, 1 << 20, 2 << 20, 4 << 20 };
	ac->size_bytes = ac->zorro2 ? sizes[ac->er_type & 7] : 0;
}

const char *fmv_rom_name(uint32_t crc32)
{
	switch (crc32) {
	case 0xf11158eb: return "CD32 FMV ROM 40.22 (391777-01)";
	case 0xc35c37bf: return "CD32 FMV ROM 40.30";
	}
	return 0;
}

int fmv_xport_start(fmv_xport *x, const uint8_t *rom, size_t len, uint32_t crc32)
{
	if (len != FMV_ROM_BYTES) {
		xlog(x, "FMV ROM is %zu bytes, not 256 KB: not used", len);
		return 0;
	}
	const char *name = fmv_rom_name(crc32);
	xlog(x, "FMV ROM: %s (CRC-32 %08x)", name ? name : "unknown dump", crc32);

	for (size_t i = 0; i < len; i += 4) {
		uint32_t w = (uint32_t)rom[i] | (uint32_t)rom[i + 1] << 8 |
		             (uint32_t)rom[i + 2] << 16 | (uint32_t)rom[i + 3] << 24;
		*(volatile uint32_t *)(x->win + i) = w;
	}

	fmv_autoconfig ac;
	fmv_decode_autoconfig(rom, &ac);
	xlog(x, "FMV autoconfig: type %02x (%s, %u KB%s), product %02x, flags %02x, "
	     "manufacturer %04x, serial %08x", ac.er_type, ac.zorro2 ? "Zorro II" : "NOT Zorro II",
	     ac.size_bytes >> 10, ac.has_rom ? ", diag ROM" : "", ac.er_product,
	     ac.er_flags, ac.manufacturer, ac.serial);
	if (!ac.zorro2 || ac.size_bytes != 1 << 20)
		xlog(x, "FMV autoconfig is not a 1 MB Zorro II board: the board map assumes one "
		     "(fmv-plan.md phase 0); continuing so the log shows what happens");

	for (int r = 0; r < 16; r++) {
		uint16_t v = 0;
		for (int k = 0; k < 4; k++)
			v |= (uint16_t)((rom[2 * (4 * r + k)] >> 4) << (4 * k));
		fmv_update u = { (uint8_t)(FMV_IDX_AUTOCFG + r), false, false, v, 0xFFFF };
		hook_update(x, u);
	}
	fmv_update alive = { FMV_IDX_CTRL, false, false, 1, 1 };
	hook_update(x, alive);
	x->alive = true;
	flush_updates(x);
	xlog(x, "FMV board alive: it joins the autoconfig chain at the next Amiga reset");
	return 1;
}

void fmv_xport_stop(fmv_xport *x)
{
	fmv_update dead = { FMV_IDX_CTRL, false, false, 0, 1 };
	hook_update(x, dead);
	x->alive = false;
	flush_updates(x);
	xlog(x, "FMV board off: it leaves the chain at the next Amiga reset");
}

void fmv_xport_poll(fmv_xport *x, uint32_t elapsed_us)
{
	drain_events(x);
	fmv_model_advance(x->model, elapsed_us);
	publish_rdidx(x);
	flush_updates(x);

	// A core that never takes the queue is named, once: on hardware this is
	// the difference between "the board is not in the chain" and a reason.
	if (x->core_seen || x->uq_wr == 0) {
		x->stall_us = 0;
	} else if (!x->stall_logged && (x->stall_us += elapsed_us) >= 2000000) {
		x->stall_logged = true;
		xlog(x, "FMV: the core has not taken the update queue in 2 s (UQ_WPTR %u, "
		     "UQ_RDPTR %u, EV_WPTR %u): status[64] not reaching it, or its DDR3 path",
		     x->uq_wr, (uint32_t)rd64(x, FMV_WIN_UQ_RDPTR), (uint32_t)rd64(x, FMV_WIN_EV_WPTR));
	}
}

void fmv_xport_get_stats(const fmv_xport *x, fmv_xport_stats *s)
{
	*s = x->st;
	s->pending = (uint32_t)x->pend.size();
}
