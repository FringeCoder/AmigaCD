// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's chips, as seen from the 68k: the C-Cube CL450 MPEG
// video decoder, the LSI L64111 MPEG audio decoder, and the board's IO word.
// Phase 2 of docs/fmv-plan.md: a LOGGING model. It runs the chips' host
// protocols (macro commands, interrupts, buffer fullness, the audio decoder's
// parsing and frame counting) on the real stream, and schedules decoding at
// the stream's own rate, but it decodes no pixels and no samples. Phase 3 and
// 4 put libmpeg2 and kjmp2 behind the same schedule.
//
// Written from the chips' manuals, not from WinUAE (fmv-plan.md, phase 0
// item 1). Every behaviour below names the page it comes from:
//   [CL p]  C-Cube CL450 MPEG Video Decoder User's Manual, 90-1450-101, 1994
//   [L64 p] LSI L64111 MPEG Audio Decoder Technical Manual, MD72-000101-99 A
//           (our notes: docs/reference/l64111-notes.md)
// Board wiring no chip manual covers (which IO-word bit is which) is in
// fmv_board below, each entry marked as unverified until phase 2's logs
// confirm it.
//
// Pure logic: no I/O, no MiSTer headers, so it builds in tests/. The transport
// (fmv.cpp) feeds it the FPGA's event ring and carries its updates back
// through the update queue (docs/fmv-contract.md, v2).

#ifndef FMV_MODEL_H
#define FMV_MODEL_H

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- contract

// Register file indices (fmv-contract.md, "The register file").
enum {
	FMV_IDX_CL450     = 0x00,   // + byte offset / 2
	FMV_IDX_L64111    = 0x80,   // + register number
	FMV_IDX_IO        = 0xA0,
	FMV_IDX_HMEM      = 0xB0,   // + HMEM address
	FMV_IDX_CTRL      = 0xC0,
	FMV_IDX_L64_INT   = 0xC1,
	FMV_IDX_RDIDX_LO  = 0xC2,
	FMV_IDX_RDIDX_HI  = 0xC3,
	FMV_IDX_IOMAP_CL  = 0xC4,
	FMV_IDX_IOMAP_L64 = 0xC5,
	FMV_IDX_IOPOL     = 0xC6,
	FMV_IDX_AUTOCFG   = 0xD0,   // 16 registers, 4 nibbles each
};

// Board offsets of the register windows (fmv-contract.md, "The board image").
enum {
	FMV_OFF_IO     = 0x40000,
	FMV_OFF_L64111 = 0x50000,
	FMV_OFF_CMEM   = 0x60000,
	FMV_OFF_CL450  = 0x70000,
	FMV_OFF_RAM    = 0x80000,
};

// CL450 direct registers, byte offsets [CL 8-3, table 8-1].
enum {
	CL_VID_Y        = 0x00, CL_CMEM_DATA   = 0x02, CL_VID_CHROMA = 0x0A,
	CL_CPU_CONTROL  = 0x20, CL_CPU_PC      = 0x22, CL_CPU_INTENB = 0x26,
	CL_CPU_TADDR    = 0x38, CL_CPU_IADDR   = 0x3E, CL_CPU_IMEM   = 0x42,
	CL_CPU_TMEM     = 0x46, CL_CPU_INT     = 0x54, CL_HOST_NEWCMD = 0x56,
	CL_CMEM_CONTROL = 0x80, CL_CMEM_STATUS = 0x82, CL_CMEM_DMACTRL = 0x84,
	CL_HOST_RADDR   = 0x88, CL_HOST_RDATA  = 0x8C, CL_HOST_CONTROL = 0x90,
	CL_HOST_SCR0    = 0x92, CL_HOST_SCR1   = 0x94, CL_HOST_SCR2  = 0x96,
	CL_HOST_INTVECW = 0x98, CL_HOST_INTVECR = 0x9C, CL_DRAM_REFCNT = 0xAC,
	CL_VID_CONTROL  = 0xEC, CL_VID_REGDATA = 0xEE,
};

// HMEM locations [CL 11-2, table 11-1].
enum { HMEM_INT_STATUS = 0xA, HMEM_FULLNESS = 0xB, HMEM_INIT_FLAG = 0xF };

// CL450 macro command function codes [CL 11-11, table 11-2].
enum {
	CLC_SET_BLANK = 0x030F, CLC_SET_BORDER = 0x0407, CLC_SET_COLOR_MODE = 0x0111,
	CLC_SET_INT_MASK = 0x0104, CLC_SET_THRESHOLD = 0x0103,
	CLC_SET_VIDEO_FORMAT = 0x0105, CLC_SET_WINDOW = 0x0406,
	CLC_DISPLAY_STILL = 0x000C, CLC_PAUSE = 0x000E, CLC_PLAY = 0x000D,
	CLC_SCAN = 0x000A, CLC_SINGLE_STEP = 0x000B, CLC_SLOW_MOTION = 0x0109,
	CLC_ACCESS_SCR = 0x8312, CLC_FLUSH = 0x8102, CLC_INQUIRE_FULLNESS = 0x8001,
	CLC_NEW_PACKET = 0x0408, CLC_RESET = 0x8000,
};

// CL450 interrupt bits, as in SetInterruptMask() and Interrupt Status
// [CL 12-2, table 12-1].
enum {
	CLI_ERR = 1 << 0, CLI_PIC_V = 1 << 1, CLI_GOP = 1 << 2, CLI_SEQ_V = 1 << 3,
	CLI_END_V = 1 << 4, CLI_END_D = 1 << 5, CLI_PIC_D = 1 << 6, CLI_UND = 1 << 8,
	CLI_SEQ_D = 1 << 9, CLI_RDY = 1 << 10, CLI_SCN = 1 << 11,
};

// One FPGA event-ring entry, decoded (fmv-contract.md, "The event ring").
struct fmv_event {
	uint32_t index;   // ring index n
	uint32_t offset;  // board byte offset, even
	uint16_t data;    // write data, or the value the read returned
	uint8_t  haddr;   // HMEM address used, for a HOST_rdata access
	bool     read, uds, lds;
	bool     reset;   // the Amiga was reset (bit 3)
};

// One update for the FPGA's register file (fmv-contract.md, "The update
// queue"). The transport adds SEQ and RDIDX.
struct fmv_update {
	uint8_t  index;
	bool     maskfile;  // write the 68k write mask, not the register
	bool     guard;     // raising the L64111 latch: drop if acked since
	uint16_t value, mask;
};

struct fmv_hooks {
	void *ctx;
	void (*update)(void *ctx, const fmv_update &u);
	// A 16-bit word of the CL450's DRAM (board RAM), word address `waddr`,
	// written by the decoder's microcode [CL ch. 14, appendix A].
	void (*dram_write)(void *ctx, uint32_t waddr, uint16_t value);
	void (*log)(void *ctx, const char *line);
};

// ------------------------------------------------------------- board wiring

// What no chip manual says: how the board puts the chips' pins into the IO
// word at $240000. Taken from WinUAE cd32_fmv.cpp as an OBSERVATION (its bit
// names), not as code, and every entry is unverified until phase 2 logs what
// the FMV ROM does with it (fmv-plan.md, phase 0 item 1).
struct fmv_board {
	uint16_t io_cl450_int;    // 0x8000 "CL450INT_": low while asserted
	uint16_t io_l64111_int;   // 0x4000 "L64111INT_": low while asserted
	uint16_t io_cflevel;      // 0x0800 "CL450CFLEVEL"
	uint16_t io_l64_falf;     // 0x0400: WinUAE gives FALF, FALE and FEMP the
	                          // same bit; taken here as FALF (stop writing)
	uint16_t io_int_pol;      // value of the int bits while asserted
};
extern const fmv_board fmv_board_default;

// ------------------------------------------------------------------ model

// Sizes assumed where the manuals leave them to the microcode or the board.
enum {
	// The bitstream buffer in DRAM. The CL450 manual leaves its size to the
	// microcode; MPEG-1's constrained parameters cap the VBV buffer at
	// 20 x 16384 bits = 40960 bytes, which a Video CD obeys.
	FMV_CL_BUFFER_BYTES = 40960,
	FMV_CMEM_WORDS      = 16,      // [CL 8-7]
	FMV_L64_FIFO_BYTES  = 128,     // [L64 2-5]
	FMV_L64_CB_FRAMES   = 64,      // Layer II [L64 4-18]
	FMV_CL_CMD_FIFO     = 126,     // [CL 11-7]
};

struct fmv_model;

fmv_model *fmv_model_new(const fmv_hooks &hooks);
void fmv_model_free(fmv_model *m);

// Power on, or an Amiga reset: put the chips in their reset state and push
// the 68k write masks and reset values to the FPGA.
void fmv_model_reset(fmv_model *m);

// One event from the ring, in ring order.
void fmv_model_event(fmv_model *m, const fmv_event &e);

// Time passes: VSYNC, display-time checks, decode and audio schedules.
void fmv_model_advance(fmv_model *m, uint32_t usec);

// The register file as the model believes the FPGA holds it (tests).
uint16_t fmv_model_reg(const fmv_model *m, uint8_t index);

// Model state for tests and the status line.
struct fmv_model_state {
	// CL450
	bool     cl_running;          // CPU_control CEn set and initialised
	int      cl_state;            // 0 IDLE, 1 PLAY-SETUP, 2 PLAY, 3 PAUSE, 4 other
	uint32_t cl_buffer_bytes;     // bitstream buffer fill
	uint32_t cl_pictures;         // pictures "decoded"
	uint32_t cl_cmem_overflow;    // words lost
	uint16_t cl_int_mask, cl_int_pending;
	uint32_t cl_commands;         // macro commands accepted
	uint16_t cl_last_command;
	uint16_t cl_window[4];        // SetWindow(x, y, w, h)
	uint16_t cl_border[4];        // SetBorder(left, top, r, gb)
	uint16_t cl_width, cl_height; // from the last sequence header
	uint32_t cl_frame_period_us;
	// L64111
	bool     l64_started;
	bool     l64_sync_sys, l64_sync_aud;
	uint32_t l64_frames_in, l64_frames_out;
	uint32_t l64_cb_frames;       // channel buffer fill
	uint32_t l64_sample_rate;
	bool     l64_int;             // the INTR latch as the model holds it
};
void fmv_model_get_state(const fmv_model *m, fmv_model_state *s);

#endif // FMV_MODEL_H
