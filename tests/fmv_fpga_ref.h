// SPDX-License-Identifier: GPL-3.0-or-later
//
// A reference implementation of the FPGA side of docs/fmv-contract.md (v3):
// the register file with its 68k write masks, HMEM indirection through
// HOST_raddr/HOST_rdata, the IO word's interrupt mirrors, the INT line, the
// L64111 read acknowledge, and the update queue's masked, guarded writes.
//
// Shared by test_fmv_model.cpp (events delivered straight to the model) and
// test_fmv_transport.cpp (events and updates through a DDR3 window), and it
// is the behaviour the core's RTL bench (rtl/fmv in AmigaCD_MiSTer) has to
// match. How an access reaches Main is left to post().

#ifndef FMV_FPGA_REF_H
#define FMV_FPGA_REF_H

#include <stdint.h>
#include <string.h>

#include "../support/minimig/fmv_model.h"

struct fmv_fpga_ref {
	uint16_t R[256], M[256];
	uint32_t ev_count = 0;       // event ring entries written
	uint32_t l64_ack_idx = 0;
	bool     l64_acked = false;

	fmv_fpga_ref() { memset(R, 0, sizeof R); memset(M, 0, sizeof M); }
	virtual ~fmv_fpga_ref() {}

	// Deliver one ring-bound access to Main.
	virtual void post(uint32_t off, bool read, uint16_t data, uint8_t haddr) = 0;

	// The update queue: in order, masked; GUARD drops a raise made before
	// the 68k's latest acknowledge (a signed 32-bit compare, wrap-safe).
	void apply_update(const fmv_update &u, uint32_t rdidx)
	{
		if (u.maskfile) {
			M[u.index] = u.value;
			return;
		}
		if (u.guard && l64_acked && (int32_t)(rdidx - l64_ack_idx) <= 0)
			return;
		R[u.index] = (uint16_t)((R[u.index] & ~u.mask) | (u.value & u.mask));
	}

	bool alive() const { return R[FMV_IDX_CTRL] & 1; }
	bool cl_int() const { return !(R[0x48] & 0x80); }        // [CL 8-12]
	bool l64_int() const { return R[FMV_IDX_L64_INT] & 1; }
	bool int2() const { return cl_int() || l64_int(); }       // ALIVE gates it in the core

	static int index_of(uint32_t off)
	{
		if (off >= FMV_OFF_CL450 && off < FMV_OFF_RAM) return (int)((off & 0xFE) >> 1);
		if (off >= FMV_OFF_L64111 && off < FMV_OFF_CMEM) return FMV_IDX_L64111 + (int)((off >> 1) & 0x1F);
		if (off >= FMV_OFF_IO && off < FMV_OFF_L64111) return FMV_IDX_IO;
		return -1;
	}

	void write(uint32_t off, uint16_t data)
	{
		uint8_t haddr = 0;
		int i = index_of(off);
		if (i == 0x46) {                                     // HOST_rdata
			haddr = R[0x44] & 15;
			int h = FMV_IDX_HMEM + haddr;
			R[h] = (uint16_t)((R[h] & ~M[h]) | (data & M[h]));
			R[0x44] = (uint16_t)((R[0x44] & ~15) | ((haddr + 1) & 15));
		} else if (i >= 0) {
			R[i] = (uint16_t)((R[i] & ~M[i]) | (data & M[i]));
		}
		post(off, false, data, haddr);
	}

	uint16_t read(uint32_t off)
	{
		uint8_t haddr = 0;
		uint16_t v = 0xFFFF;
		int i = index_of(off);
		if (i == 0x46) {
			haddr = R[0x44] & 15;
			v = R[FMV_IDX_HMEM + haddr];
		} else if (i == FMV_IDX_IO) {
			v = R[FMV_IDX_IO];
			uint16_t pol = R[FMV_IDX_IOPOL];
			uint16_t mc = R[FMV_IDX_IOMAP_CL], ml = R[FMV_IDX_IOMAP_L64];
			v = (uint16_t)((v & ~mc) | ((cl_int() ? pol : ~pol) & mc));
			v = (uint16_t)((v & ~ml) | ((l64_int() ? pol : ~pol) & ml));
		} else if (i >= 0) {
			v = R[i];
		}
		if (i == FMV_IDX_L64111 + 4 || i == FMV_IDX_L64111 + 5) {
			R[FMV_IDX_L64_INT] &= (uint16_t)~1;              // [L64 4-9]
			l64_ack_idx = ev_count;
			l64_acked = true;
		}
		post(off, true, v, haddr);
		return v;
	}
};

#endif // FMV_FPGA_REF_H
