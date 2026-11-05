// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's picture, the pure part (docs/fmv-plan.md phase 3,
// docs/fmv-contract.md v5 "The picture"):
//   - where the CL450 puts its output window, from the host's SetWindow(),
//     SetBorder(), SetVideoFormat() and SetBlank() and the sequence header,
//     as the display registers 0xE0-0xE6 the core latches at VSYNC;
//   - the four frame buffers: which one is shown, which may still be, and
//     which the decoder may write;
//   - YUV 4:2:0 to RGB565, cropped to the window, in the buffer layout.
// No I/O, no threads: it builds in tests/. fmv_decoder.cpp runs libmpeg2
// behind it, and fmv.cpp carries the registers to the core.
//
//   [CL p]  C-Cube CL450 MPEG Video Decoder User's Manual, 90-1450-101, 1994

#ifndef FMV_PICTURE_H
#define FMV_PICTURE_H

#include <stddef.h>
#include <stdint.h>

enum {
	FMV_FB_PHYS    = 0x27200000,   // contract v5
	FMV_FB_BUFS    = 4,
	FMV_FB_BUF_BYTES = 0x80000,
	FMV_FB_STRIDE  = 1024,         // bytes a line
	FMV_FB_MAX_W   = 384,
	FMV_FB_MAX_H   = 512,
};

// Display register indices (contract v5).
enum {
	FMV_IDX_DISP_CTRL = 0xE0, FMV_IDX_DISP_X = 0xE1, FMV_IDX_DISP_Y = 0xE2,
	FMV_IDX_DISP_W = 0xE3, FMV_IDX_DISP_H = 0xE4,
	FMV_IDX_BORDER_RG = 0xE5, FMV_IDX_BORDER_B = 0xE6,
};
enum {
	FMV_DISP_ON = 1 << 0, FMV_DISP_BLANK = 1 << 1, FMV_DISP_KEY_ALL = 1 << 4,
};
static inline uint16_t fmv_disp_buf(int b) { return (uint16_t)((b & 3) << 2); }

// What the CL450 has been told, as the model holds it.
struct fmv_cl_display {
	uint16_t seq_w, seq_h;        // horizontal_size, vertical_size; 0 before a sequence header
	uint16_t window[4];           // SetWindow(xOffset, yOffset, width, height) [CL 11-42]
	uint16_t border[4];           // SetBorder(leftBorder, topBorder, rBorder, gbBorder) [CL 11-30]
	uint16_t video_format;        // SetVideoFormat(): 3 PAL, 4 NTSC [CL 11-40]
	bool     blank;               // SetBlank(1) in force [CL 11-29]
	bool     idle;                // the IDLE state: the window is blanked [CL 10-*]
	bool     video_on;            // the board's IO "CL450 video on" bit, as last written
};

// Board-side placement not in the chip: where the core's beam counter sits
// against the CL450's HSYNC/VSYNC reference, found on hardware.
//
// Measured 2026-11-05 (phase 3 run v23, Cannon Fodder, HDMI PAL): with the
// manual's auto-centre (leftBorder 60, topBorder 18) the picture's left was
// off screen and its top clipped. 93 display pixels and 3 lines more put
// the 704-pixel window 23 px from the left and 24 from the right of the
// MiSTer's visible area. Centred on the MiSTer's picture, not checked
// against a real CD32 with the cartridge. The ini values add to these.
enum { FMV_CALIB_HOFS = 93, FMV_CALIB_VOFS = 3 };
struct fmv_calib {
	int  hofs;                    // display pixels added to DISP_X
	int  vofs;                    // lines added to DISP_Y
	bool force_on;                // ON whatever the IO word says (the bit is unverified)
	bool key_all;                 // show the cartridge everywhere (calibration)
};

// The window as the core and the decoder need it.
struct fmv_window {
	// in the picture, source pixels: the crop (SetWindow's offsets)
	uint16_t src_x, src_y;
	// size, source pixels and lines; src_w a multiple of 4
	uint16_t src_w, src_h;
	// on the screen: SetBorder's left/top, after auto-centering and calib
	int      disp_x, disp_y;
};

// The window and the registers 0xE0-0xE6 (all but the BUF field of
// DISP_CTRL, which the buffer ring owns), from the chip state.
// have_picture: one has been decoded and named since the last reset/flush.
void fmv_window_compute(const fmv_cl_display &d, const fmv_calib &c, fmv_window *w);
void fmv_disp_regs(const fmv_cl_display &d, const fmv_calib &c, bool have_picture,
                   uint16_t regs[7]);

// ---- the four buffers ----
//
// A buffer is FREE, DECODING (the decoder writes it), READY (complete, not
// yet named), SHOWN (named in BUF), or RETIRING (was named; the core may
// show it until the VSYNC after the next name, so it waits one present).
// That is the contract's rule: never write a buffer until a whole frame
// after it was last named.
enum fmv_buf_state { FMV_BUF_FREE, FMV_BUF_DECODING, FMV_BUF_READY, FMV_BUF_SHOWN, FMV_BUF_RETIRING };

struct fmv_bufring {
	fmv_buf_state st[FMV_FB_BUFS];
	uint32_t      seq[FMV_FB_BUFS];   // decode order of a READY buffer
	uint32_t      next_seq;
	int           shown;              // -1 before the first present
};
void fmv_bufring_init(fmv_bufring *r);
// A buffer to decode into, or -1 (none free: the decoder waits).
int  fmv_bufring_acquire(fmv_bufring *r);
void fmv_bufring_ready(fmv_bufring *r, int b);
// Drop a DECODING buffer back to FREE (a flush mid-picture).
void fmv_bufring_abort(fmv_bufring *r, int b);
// The display tick: name the oldest READY buffer. Returns it, or -1 if none
// is ready (the shown one stays). The one shown before goes RETIRING, and
// the RETIRING one before that is FREE now.
int  fmv_bufring_present(fmv_bufring *r);
// A flush or reset: everything not shown is FREE again. The shown buffer
// stays shown: the core displays it until BUF changes.
void fmv_bufring_flush(fmv_bufring *r);
int  fmv_bufring_count(const fmv_bufring *r, fmv_buf_state s);

// ---- the pixels ----
//
// BT.601 limited range, as MPEG-1 specifies; chroma 2x2. Writes the window
// (w.src_x/src_y/src_w/src_h of a pic_w x pic_h picture) to out, rows
// FMV_FB_STRIDE bytes apart, RGB565 little-endian (contract v5). Rows or
// columns the picture does not have are written black.
void fmv_yuv420_to_rgb565(const uint8_t *const yuv[3], int pic_w, int pic_h,
                          const fmv_window &w, uint8_t *out);

#endif // FMV_PICTURE_H
