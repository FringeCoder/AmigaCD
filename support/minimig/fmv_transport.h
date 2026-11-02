// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FMV module's DDR3 transport, Main's side of docs/fmv-contract.md (v4):
// the board image (ROM, decoder variables in RAM), the event ring from the
// FPGA, and the update queue back to it, with the chip model in between.
//
// Pure logic over a mapped window, so tests/test_fmv_transport.cpp can run it
// against a plain buffer and a reference FPGA. fmv.cpp does the Main side:
// the switch, the mapping, the ROM file, the clock.

#ifndef FMV_TRANSPORT_H
#define FMV_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "fmv_model.h"

// The window (fmv-contract.md, "The window"), as byte offsets.
enum {
	FMV_WIN_PHYS     = 0x27000000,
	FMV_WIN_SIZE     = 0x00200000,
	FMV_WIN_EV_WPTR  = 0x100000,
	FMV_WIN_UQ_RDPTR = 0x100008,
	FMV_WIN_UQ_WPTR  = 0x100040,
	FMV_WIN_UQ       = 0x101000,
	FMV_WIN_EV       = 0x110000,
	FMV_EV_ENTRIES   = 4096,
	FMV_UQ_ENTRIES   = 1024,
	FMV_ROM_BYTES    = 0x40000,
};

struct fmv_xport;

typedef void (*fmv_log_fn)(void *ctx, const char *line);

// `win` is the 2 MB window, mapped uncached. A new session: the pointers in
// the window are zeroed, not trusted (contract v4). The caller holds
// status[64] clear from before this call until fmv_xport_start() returns,
// so the core's counters are at 0 too.
fmv_xport *fmv_xport_new(volatile uint8_t *win, fmv_log_fn log, void *log_ctx);
void fmv_xport_free(fmv_xport *x);

// Put the ROM image in the window, identify it by CRC-32, decode and queue
// its autoconfig nibbles, then raise ALIVE. Returns 0 if the image is
// unusable (wrong size). The board joins the autoconfig chain at the next
// Amiga reset.
int fmv_xport_start(fmv_xport *x, const uint8_t *rom, size_t len, uint32_t crc32);

// Lower ALIVE: the board leaves the chain at the next Amiga reset.
void fmv_xport_stop(fmv_xport *x);

// Drain the event ring into the model, advance the model by `elapsed_us`,
// and push the model's updates (and HOST_RDIDX) through the update queue.
void fmv_xport_poll(fmv_xport *x, uint32_t elapsed_us);

struct fmv_xport_stats {
	uint32_t events;        // drained in total
	uint32_t not_landed;    // passes that stopped at an entry whose SEQ lagged
	uint32_t ev_overruns;   // the ring got ahead by more than it holds
	uint32_t updates;       // written to the queue in total
	uint32_t uq_waits;      // passes that found the queue full
	uint32_t pending;       // updates waiting in Main now
};
void fmv_xport_get_stats(const fmv_xport *x, fmv_xport_stats *s);

fmv_model *fmv_xport_model(fmv_xport *x);

// True once the core has failed to take the update queue for 2 s (logged).
bool fmv_xport_stalled(const fmv_xport *x);

// The core's FMV diag window (hps_ext UIO_DMA_READ 0xFA00, laid out in
// rtl/fmv/fmv.v of AmigaCD_MiSTer): `w` is what the read returned, `n`
// words. Finds the 0xA5F4 signature, so a read misaligned by a word or two
// still decodes, and writes one line to `out`. Returns 0 if there is no
// window (a core without it, or a version this decoder does not know), with
// a line that says so.
int fmv_diag_format(const uint16_t *w, int n, char *out, size_t len);

// What the ROM's first 128 bytes say as autoconfig (for the log, and tests).
struct fmv_autoconfig {
	uint8_t  er_type, er_product, er_flags;
	uint16_t manufacturer;
	uint32_t serial;
	uint32_t size_bytes;     // 0 if the size code is not a Zorro II one
	bool     zorro2, has_rom;
};
void fmv_decode_autoconfig(const uint8_t *rom, fmv_autoconfig *ac);

// Known dumps of the cartridge ROM (WinUAE rommgr.cpp, as an observation;
// fmv-plan.md phase 0 item 2). Returns 0 for an unknown dump.
const char *fmv_rom_name(uint32_t crc32);

#endif // FMV_TRANSPORT_H
