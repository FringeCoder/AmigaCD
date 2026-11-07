// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's video decoder (docs/fmv-plan.md phase 3): libmpeg2
// on a thread of its own, pinned to ARM core 0 (Main runs on core 1; phase 1
// measured about 28 % of a core for a 352x288 25 fps stream). It takes the
// bitstream the CL450 model accepts, decodes it, and writes each picture,
// cropped to the CL450's output window, as RGB565 into one of the four frame
// buffers (fmv_picture.h, contract v5). The model's picture schedule says
// when the next one is to be shown; fmv_decoder_present() names it.
//
// All calls are from Main's thread. The frame buffers are written only by
// the decoder thread, and only into a buffer the ring has given it.

#ifndef FMV_DECODER_H
#define FMV_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "fmv_picture.h"

struct fmv_decoder;

// fb: the four buffers, FMV_FB_BUFS * FMV_FB_BUF_BYTES (the uncached DDR3
// mapping on the MiSTer, ordinary memory in tests). cpu: the core to pin the
// thread to, or -1.
fmv_decoder *fmv_decoder_new(volatile uint8_t *fb, int cpu);
void fmv_decoder_free(fmv_decoder *d);

// Bitstream bytes, in order (the model's video_bytes hook). Staged here and
// handed to the thread by fmv_decoder_poll().
void fmv_decoder_bytes(fmv_decoder *d, const uint8_t *p, size_t n);
// The bitstream restarts (FlushBitstream(), Reset(), a board reset): what
// is queued or decoded and not yet shown is dropped.
void fmv_decoder_flush(fmv_decoder *d);
// The window the next pictures are cropped to.
void fmv_decoder_set_window(fmv_decoder *d, const fmv_window &w);
// Hand staged bytes to the thread. Call once per transport poll.
void fmv_decoder_poll(fmv_decoder *d);
// The display tick: the buffer to name in DISP_CTRL now, or -1 if no new
// picture is ready (keep the one shown).
int  fmv_decoder_present(fmv_decoder *d);

struct fmv_decoder_stats {
	uint32_t bytes_in;        // bitstream bytes handed over
	uint32_t pictures;        // decoded and written to a buffer
	uint32_t presented;       // named by fmv_decoder_present()
	uint32_t late;            // presents with nothing new ready
	uint32_t dropped_bytes;   // lost to a full ring (the decoder fell behind)
	uint32_t flushes;
	uint32_t sequences;       // sequence headers seen
	uint16_t width, height;   // of the last sequence
	int      shown;           // the buffer named, -1 before the first
};
void fmv_decoder_get_stats(fmv_decoder *d, fmv_decoder_stats *s);

// Wait until the thread has consumed everything handed to it and has
// nothing left to write (tests only: there is no display tick to wait on).
void fmv_decoder_drain(fmv_decoder *d);

#endif // FMV_DECODER_H
