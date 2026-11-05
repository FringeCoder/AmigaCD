// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's picture, the pure part. See fmv_picture.h.

#include "fmv_picture.h"

#include <string.h>

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void fmv_window_compute(const fmv_cl_display &d, const fmv_calib &c, fmv_window *w)
{
	// Table 11-7 [CL 11-40]: 352 x 288 PAL, 352 x 240 NTSC, in source pixels.
	const int max_h = d.video_format == 4 ? 240 : 288;
	const int max_w = 352;
	int pic_w = d.seq_w ? d.seq_w : max_w;
	int pic_h = d.seq_h ? d.seq_h : max_h;

	// SetWindow [CL 11-42 to 11-44]: offsets in source pixels inside the
	// picture; width in display pixels (two per source pixel), height in
	// lines; 0 for either means the whole decoded picture. Clamped to the
	// recommended maxima, so the window never shows outside the picture.
	int xoff = clampi(d.window[0] & 0x7FFF, 0, pic_w - 1);
	int yoff = clampi(d.window[1] & 0x7FFF, 0, pic_h - 1);
	int width = d.window[2] & 0x7FFF;
	if (!width) width = 2 * pic_w;
	width = clampi(width, 0, 2 * (pic_w - xoff));
	width = clampi(width, 0, 704);
	int height = d.window[3] & 0x7FFF;
	if (!height) height = pic_h;
	height = clampi(height, 0, pic_h - yoff);
	height = clampi(height, 0, max_h);

	// The frame buffer holds source pixels in fours (contract v5).
	int src_w = clampi((width / 2) & ~3, 0, FMV_FB_MAX_W);
	int src_h = clampi(height, 0, FMV_FB_MAX_H);

	// SetBorder [CL 11-30 to 11-32]: left in display pixels from HSYNC,
	// top in lines from VSYNC; 0 centres, taking 60 and 18 as the borders
	// that centre a window of the maximum size.
	int left = d.border[0] & 0x3FF;
	if (!left) left = 60 + (2 * max_w - 2 * src_w) / 2;
	int top = d.border[1] & 0x7FFF;
	if (!top) top = 18 + (max_h - src_h) / 2;

	w->src_x = (uint16_t)xoff;
	w->src_y = (uint16_t)yoff;
	w->src_w = (uint16_t)src_w;
	w->src_h = (uint16_t)src_h;
	w->disp_x = clampi(left + c.hofs, 0, 2047);
	w->disp_y = clampi(top + c.vofs, 0, 1023);
}

void fmv_disp_regs(const fmv_cl_display &d, const fmv_calib &c, bool have_picture,
                   uint16_t regs[7])
{
	fmv_window w;
	fmv_window_compute(d, c, &w);
	uint16_t ctrl = 0;
	if (d.video_on || c.force_on)
		ctrl |= FMV_DISP_ON;
	// The window shows the border colour while blanked by the host, in
	// IDLE, and until the first picture is there to show [CL 11-29, 11-6].
	if (d.blank || d.idle || !have_picture)
		ctrl |= FMV_DISP_BLANK;
	if (c.key_all)
		ctrl |= FMV_DISP_KEY_ALL;
	regs[0] = ctrl;
	regs[1] = (uint16_t)w.disp_x;
	regs[2] = (uint16_t)w.disp_y;
	regs[3] = w.src_w;
	regs[4] = w.src_h;
	// RGB colour mode [CL 11-30]: rBorder 7:0 red; gbBorder 15:8 green,
	// 7:0 blue.
	regs[5] = (uint16_t)(((d.border[2] & 0xFF) << 8) | (d.border[3] >> 8));
	regs[6] = (uint16_t)(d.border[3] & 0xFF);
}

// ------------------------------------------------------------- the ring

void fmv_bufring_init(fmv_bufring *r)
{
	memset(r, 0, sizeof *r);
	r->shown = -1;
}

int fmv_bufring_acquire(fmv_bufring *r)
{
	for (int b = 0; b < FMV_FB_BUFS; b++)
		if (r->st[b] == FMV_BUF_FREE) {
			r->st[b] = FMV_BUF_DECODING;
			return b;
		}
	return -1;
}

void fmv_bufring_ready(fmv_bufring *r, int b)
{
	if (b < 0 || b >= FMV_FB_BUFS || r->st[b] != FMV_BUF_DECODING)
		return;
	r->st[b] = FMV_BUF_READY;
	r->seq[b] = r->next_seq++;
}

void fmv_bufring_abort(fmv_bufring *r, int b)
{
	if (b >= 0 && b < FMV_FB_BUFS && r->st[b] == FMV_BUF_DECODING)
		r->st[b] = FMV_BUF_FREE;
}

int fmv_bufring_present(fmv_bufring *r)
{
	int pick = -1;
	for (int b = 0; b < FMV_FB_BUFS; b++)
		if (r->st[b] == FMV_BUF_READY &&
		    (pick < 0 || (int32_t)(r->seq[b] - r->seq[pick]) < 0))
			pick = b;
	if (pick < 0)
		return -1;
	for (int b = 0; b < FMV_FB_BUFS; b++)
		if (r->st[b] == FMV_BUF_RETIRING)
			r->st[b] = FMV_BUF_FREE;
	if (r->shown >= 0)
		r->st[r->shown] = FMV_BUF_RETIRING;
	r->st[pick] = FMV_BUF_SHOWN;
	r->shown = pick;
	return pick;
}

void fmv_bufring_flush(fmv_bufring *r)
{
	for (int b = 0; b < FMV_FB_BUFS; b++)
		if (r->st[b] == FMV_BUF_READY)
			r->st[b] = FMV_BUF_FREE;
}

int fmv_bufring_count(const fmv_bufring *r, fmv_buf_state s)
{
	int n = 0;
	for (int b = 0; b < FMV_FB_BUFS; b++)
		n += r->st[b] == s;
	return n;
}

// ------------------------------------------------------------- pixels

static uint8_t clamp_tab[1024];
static bool    clamp_ready;
#define CLAMP(x) clamp_tab[((x) >> 8) + 384]

static void clamp_init()
{
	for (int i = 0; i < 1024; i++) {
		int v = i - 384;
		clamp_tab[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
	clamp_ready = true;
}

void fmv_yuv420_to_rgb565(const uint8_t *const yuv[3], int pic_w, int pic_h,
                          const fmv_window &w, uint8_t *out)
{
	if (!clamp_ready)
		clamp_init();
	const int cw = (pic_w + 1) / 2;
	for (int r = 0; r < w.src_h; r++) {
		uint8_t *o = out + (size_t)r * FMV_FB_STRIDE;
		const int py = w.src_y + r;
		if (py >= pic_h) {
			memset(o, 0, (size_t)w.src_w * 2);
			continue;
		}
		const uint8_t *yr = yuv[0] + (size_t)py * pic_w;
		const uint8_t *ur = yuv[1] + (size_t)(py / 2) * cw;
		const uint8_t *vr = yuv[2] + (size_t)(py / 2) * cw;
		for (int c = 0; c < w.src_w; c++) {
			const int px = w.src_x + c;
			uint16_t p = 0;
			if (px < pic_w) {
				int d = ur[px / 2] - 128, e = vr[px / 2] - 128;
				int y = 298 * (yr[px] - 16);
				int rr = CLAMP(y + 409 * e + 128);
				int gg = CLAMP(y - 100 * d - 208 * e + 128);
				int bb = CLAMP(y + 516 * d + 128);
				p = (uint16_t)((rr >> 3) << 11 | (gg >> 2) << 5 | bb >> 3);
			}
			o[2 * c]     = (uint8_t)p;
			o[2 * c + 1] = (uint8_t)(p >> 8);
		}
	}
}
