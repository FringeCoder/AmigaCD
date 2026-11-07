// The CD32 FMV module's picture, the pure part (support/minimig/fmv_picture.cpp)
// and the model's phase 3 hooks, against docs/fmv-contract.md v5 and the
// CL450 manual's SetWindow()/SetBorder()/SetBlank()/SetVideoFormat()
// (pp. 11-29 to 11-44). Each check names its rule.

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../support/minimig/fmv_picture.h"
#include "../support/minimig/fmv_model.h"
#include "fmv_fpga_ref.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void eq(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld, want %ld\n", what, got, want);
		failures++;
	}
}

static fmv_cl_display pal_352x288()
{
	fmv_cl_display d;
	memset(&d, 0, sizeof d);
	d.seq_w = 352;
	d.seq_h = 288;
	d.video_format = 3;
	d.video_on = true;
	return d;
}

static const fmv_calib no_calib = { 0, 0, false, false };

// --------------------------------------------------------------- window

static void test_window_defaults()
{
	fmv_window w;
	fmv_cl_display d = pal_352x288();
	fmv_window_compute(d, no_calib, &w);
	eq("auto-size: the whole picture, width [CL 11-42]", w.src_w, 352);
	eq("auto-size: the whole picture, height", w.src_h, 288);
	eq("no crop by default", w.src_x + w.src_y, 0);
	eq("auto-centre a full-width window: leftBorder 60 [CL 11-32]", w.disp_x, 60);
	eq("auto-centre a full-height window: topBorder 18", w.disp_y, 18);

	// NTSC: at most 240 lines [CL table 11-7], so a 288-line picture is cut
	d.video_format = 4;
	fmv_window_compute(d, no_calib, &w);
	eq("NTSC: the window is at most 240 lines", w.src_h, 240);
	eq("NTSC: full height, so topBorder 18", w.disp_y, 18);

	// A smaller picture centres in the maximum
	d = pal_352x288();
	d.seq_w = 320;
	d.seq_h = 240;
	fmv_window_compute(d, no_calib, &w);
	eq("a 320-wide picture: 320 source pixels", w.src_w, 320);
	eq("centred: 60 + (704 - 640) / 2", w.disp_x, 92);
	eq("centred: 18 + (288 - 240) / 2", w.disp_y, 42);
}

static void test_window_args()
{
	fmv_window w;
	fmv_cl_display d = pal_352x288();
	// SetWindow(16, 8, 640, 200): offsets in source pixels, width in display
	// pixels, two per source pixel [CL 11-44]
	d.window[0] = 16; d.window[1] = 8; d.window[2] = 640; d.window[3] = 200;
	fmv_window_compute(d, no_calib, &w);
	eq("SetWindow xOffset in source pixels", w.src_x, 16);
	eq("SetWindow yOffset in source pixels", w.src_y, 8);
	eq("SetWindow width is in display pixels: 640 -> 320", w.src_w, 320);
	eq("SetWindow height in lines", w.src_h, 200);

	// Past the picture: clamped to the recommended maxima [CL table 11-8]
	d.window[0] = 100; d.window[2] = 704; d.window[1] = 200; d.window[3] = 200;
	fmv_window_compute(d, no_calib, &w);
	eq("width at most 2 * (horizontal_size - xOffset), in fours", w.src_w, 252);
	eq("height at most vertical_size - yOffset", w.src_h, 88);

	// A width that is not a multiple of 8 display pixels is cut to fours
	d = pal_352x288();
	d.window[2] = 650;
	fmv_window_compute(d, no_calib, &w);
	eq("the buffer holds source pixels in fours (contract v5)", w.src_w, 324);

	// SetBorder(leftBorder, topBorder): given, not centred; calibration added
	d = pal_352x288();
	d.border[0] = 100; d.border[1] = 30;
	fmv_calib c = { -7, 3, false, false };
	fmv_window_compute(d, c, &w);
	eq("SetBorder leftBorder in display pixels, plus HOFS", w.disp_x, 93);
	eq("SetBorder topBorder in lines, plus VOFS", w.disp_y, 33);
	d.border[0] = 0xFC64;                    // bits 15:10 are not leftBorder
	fmv_window_compute(d, no_calib, &w);
	eq("leftBorder is bits 9:0", w.disp_x, 100);
}

static void test_regs()
{
	uint16_t r[7];
	fmv_cl_display d = pal_352x288();
	d.border[2] = 0x0012;                    // red
	d.border[3] = 0x3456;                    // green 0x34, blue 0x56
	fmv_disp_regs(d, no_calib, true, r);
	eq("ON from the IO word's video-on bit", r[0] & FMV_DISP_ON, FMV_DISP_ON);
	eq("not blanked with a picture, in PLAY, unblanked", r[0] & FMV_DISP_BLANK, 0);
	eq("BUF is the ring's, not set here", r[0] & 0x000C, 0);
	eq("DISP_X", r[1], 60);
	eq("DISP_Y", r[2], 18);
	eq("DISP_W", r[3], 352);
	eq("DISP_H", r[4], 288);
	eq("BORDER_RG: rBorder 7:0, gbBorder 15:8 [CL 11-30]", r[5], 0x1234);
	eq("BORDER_B: gbBorder 7:0", r[6], 0x56);

	fmv_disp_regs(d, no_calib, false, r);
	check(r[0] & FMV_DISP_BLANK, "BLANK until the first picture [CL 11-6]");
	d.blank = true;
	fmv_disp_regs(d, no_calib, true, r);
	check(r[0] & FMV_DISP_BLANK, "BLANK while SetBlank(1) [CL 11-29]");
	d.blank = false;
	d.idle = true;
	fmv_disp_regs(d, no_calib, true, r);
	check(r[0] & FMV_DISP_BLANK, "BLANK in IDLE");
	d.idle = false;
	d.video_on = false;
	fmv_disp_regs(d, no_calib, true, r);
	check(!(r[0] & FMV_DISP_ON), "OFF while the IO word's video-on bit is clear");
	fmv_calib c = { 0, 0, true, false };
	fmv_disp_regs(d, c, true, r);
	check((r[0] & FMV_DISP_ON) && !(r[0] & FMV_DISP_KEY_ALL), "AMIGACD_FMV_SHOW=1: on, keyed");
	c.key_all = true;
	fmv_disp_regs(d, c, true, r);
	check(r[0] & FMV_DISP_KEY_ALL, "AMIGACD_FMV_SHOW=2: KEY_ALL");
	eq("buffer field", fmv_disp_buf(3), 0x000C);
}

// ----------------------------------------------------------------- ring

static void test_bufring()
{
	fmv_bufring r;
	fmv_bufring_init(&r);
	eq("nothing to present at first", fmv_bufring_present(&r), -1);
	int a = fmv_bufring_acquire(&r), b = fmv_bufring_acquire(&r);
	int c = fmv_bufring_acquire(&r), d = fmv_bufring_acquire(&r);
	check(a >= 0 && b >= 0 && c >= 0 && d >= 0, "four buffers to decode into");
	eq("no fifth", fmv_bufring_acquire(&r), -1);
	fmv_bufring_ready(&r, c);
	fmv_bufring_ready(&r, a);
	eq("present names the oldest ready (decode order)", fmv_bufring_present(&r), c);
	fmv_bufring_ready(&r, b);
	eq("then the next", fmv_bufring_present(&r), a);
	eq("the one shown before is RETIRING, not free", r.st[c], FMV_BUF_RETIRING);
	eq("so nothing is free yet", fmv_bufring_acquire(&r), -1);
	fmv_bufring_ready(&r, d);
	eq("present", fmv_bufring_present(&r), b);
	eq("a buffer is free a whole present after it was replaced", r.st[c], FMV_BUF_FREE);
	eq("and only that one", fmv_bufring_count(&r, FMV_BUF_FREE), 1);
	eq("nothing new ready keeps the one shown", (fmv_bufring_present(&r), r.shown), d);
	eq("...and its RETIRING stays", fmv_bufring_count(&r, FMV_BUF_RETIRING), 1);
	eq("no READY left, so the next present is -1", fmv_bufring_present(&r), -1);
	eq("-1 does not retire the shown one", r.st[d], FMV_BUF_SHOWN);

	// flush: READY buffers are dropped; the shown one stays shown
	int e = fmv_bufring_acquire(&r);
	fmv_bufring_ready(&r, e);
	fmv_bufring_flush(&r);
	eq("flush frees READY", r.st[e], FMV_BUF_FREE);
	eq("flush keeps the one the core shows", r.st[d], FMV_BUF_SHOWN);
	int g = fmv_bufring_acquire(&r);
	fmv_bufring_abort(&r, g);
	eq("abort frees a DECODING buffer", r.st[g], FMV_BUF_FREE);
	fmv_bufring_ready(&r, g);
	eq("ready on a FREE buffer is ignored", r.st[g], FMV_BUF_FREE);
}

// --------------------------------------------------------------- pixels

static void test_convert()
{
	// A 16 x 8 picture: luma ramps, chroma flat grey, then one red block
	const int W = 16, H = 8;
	std::vector<uint8_t> y(W * H), u(W * H / 4, 128), v(W * H / 4, 128);
	for (int i = 0; i < W * H; i++)
		y[i] = 16;
	y[0] = 235;
	const uint8_t *yuv[3] = { y.data(), u.data(), v.data() };
	std::vector<uint8_t> out(FMV_FB_STRIDE * H, 0xAA);
	fmv_window w = { 0, 0, 16, 8, 0, 0 };
	fmv_yuv420_to_rgb565(yuv, W, H, w, out.data());
	eq("Y 235 is white [BT.601 limited range]", out[0] | out[1] << 8, 0xFFFF);
	eq("Y 16 is black", out[2] | out[3] << 8, 0x0000);
	eq("rows FMV_FB_STRIDE apart: past the window is untouched", out[32], 0xAA);

	// Pure red: Y 81, Cb 90, Cr 240
	for (int i = 0; i < W * H; i++)
		y[i] = 81;
	for (size_t i = 0; i < u.size(); i++) { u[i] = 90; v[i] = 240; }
	fmv_yuv420_to_rgb565(yuv, W, H, w, out.data());
	uint16_t p = (uint16_t)(out[0] | out[1] << 8);
	check((p >> 11) >= 30 && ((p >> 5) & 63) <= 2 && (p & 31) <= 1, "BT.601 red is red in RGB565");
	// red is 0xF8xx: not a byte palindrome, so the order shows
	check((out[1] >> 3) >= 30 && out[0] < 0x80, "little-endian: low byte first (contract v5)");

	// Chroma is per 2 x 2 luma: columns 2-3 take the second chroma sample
	for (size_t i = 0; i < u.size(); i++) {
		bool red = (i % (W / 2)) == 1;
		u[i] = red ? 90 : 128;
		v[i] = red ? 240 : 128;
	}
	fmv_yuv420_to_rgb565(yuv, W, H, w, out.data());
	uint16_t p2 = (uint16_t)(out[4] | out[5] << 8), p0 = (uint16_t)(out[0] | out[1] << 8);
	check((p2 >> 11) >= 30 && ((p2 >> 5) & 63) <= 8, "pixel 2 takes chroma sample 1 (red)");
	check((p0 >> 11) < 20, "pixel 0 takes chroma sample 0 (grey)");

	// Crop: the window starts inside the picture; chroma follows the picture
	for (int i = 0; i < W * H; i++)
		y[i] = (uint8_t)(16 + (i % W) * 10 + (i / W));
	for (size_t i = 0; i < u.size(); i++) { u[i] = 128; v[i] = 128; }
	fmv_window c = { 4, 2, 8, 4, 0, 0 };
	std::vector<uint8_t> o2(FMV_FB_STRIDE * 4, 0);
	fmv_yuv420_to_rgb565(yuv, W, H, c, o2.data());
	std::vector<uint8_t> full(FMV_FB_STRIDE * H, 0);
	fmv_yuv420_to_rgb565(yuv, W, H, w, full.data());
	bool same = true;
	for (int r = 0; r < 4; r++)
		for (int x = 0; x < 8; x++)
			same &= o2[r * FMV_FB_STRIDE + 2 * x] == full[(r + 2) * FMV_FB_STRIDE + 2 * (x + 4)] &&
			        o2[r * FMV_FB_STRIDE + 2 * x + 1] == full[(r + 2) * FMV_FB_STRIDE + 2 * (x + 4) + 1];
	check(same, "the cropped window is the picture's pixels from (xOffset, yOffset)");

	// A window wider than the picture shows black past its edge
	fmv_window wide = { 8, 0, 12, 1, 0, 0 };
	std::vector<uint8_t> o3(FMV_FB_STRIDE, 0xAA);
	fmv_yuv420_to_rgb565(yuv, W, H, wide, o3.data());
	eq("past the picture's right edge: black", o3[2 * 9] | o3[2 * 9 + 1] << 8, 0);
}

// ---------------------------------------------------- the model's hooks

struct vfpga : fmv_fpga_ref {
	fmv_model *model = 0;
	uint32_t ev_count = 0;
	uint32_t host_rdidx = 0;
	std::vector<uint8_t> bytes;
	int flushes = 0, pictures = 0;
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
		host_rdidx = ev_count;
	}
};

static void h_update(void *ctx, const fmv_update &u) { vfpga *f = (vfpga *)ctx; f->apply_update(u, f->host_rdidx); }
static void h_dram(void *, uint32_t, uint16_t) {}
static void h_log(void *, const char *) {}
static void v_bytes(void *ctx, const uint8_t *p, size_t n) { vfpga *f = (vfpga *)ctx; f->bytes.insert(f->bytes.end(), p, p + n); }
static void v_flush(void *ctx) { ((vfpga *)ctx)->flushes++; }
static void v_picture(void *ctx) { ((vfpga *)ctx)->pictures++; }

enum { CL = FMV_OFF_CL450 };

static void command(vfpga &f, uint16_t code, uint16_t a1 = 0, uint16_t a2 = 0, uint16_t a3 = 0,
                    uint16_t a4 = 0)
{
	f.write(CL + CL_HOST_RADDR, 0);
	f.write(CL + CL_HOST_RDATA, code);
	f.write(CL + CL_HOST_RDATA, a1);
	f.write(CL + CL_HOST_RDATA, a2);
	f.write(CL + CL_HOST_RDATA, a3);
	f.write(CL + CL_HOST_RDATA, a4);
	f.write(CL + CL_HOST_NEWCMD, 1);
	fmv_model_advance(f.model, 100);
}

static void test_model_hooks()
{
	vfpga f;
	fmv_hooks h = { &f, h_update, h_dram, h_log, 0, 0, 0, 0, 0, 0, 0 };
	f.model = fmv_model_new(h);
	fmv_model_set_video_hooks(f.model, &f, v_bytes, v_flush, v_picture);
	fmv_model_reset(f.model);
	int fl0 = f.flushes;
	check(fl0 >= 1, "a board reset flushes the decoder");

	// boot the microapplication [CL 10-9]
	f.write(CL + CL_HOST_RADDR, HMEM_INIT_FLAG);
	f.write(CL + CL_HOST_RDATA, 0x1234);
	f.write(CL + CL_CPU_PC, 0x0100);
	f.write(CL + CL_CPU_CONTROL, 1);
	fmv_model_advance(f.model, 3000);

	fmv_cl_display d;
	fmv_model_display(f.model, &d);
	eq("SetVideoFormat default: NTSC [CL table 10-3]", d.video_format, 4);
	check(d.idle, "IDLE after initialisation");
	check(!d.video_on, "video off until the IO word says so");

	command(f, CLC_SET_VIDEO_FORMAT, 3);
	command(f, CLC_SET_WINDOW, 1, 2, 3, 4);
	command(f, CLC_SET_BORDER, 5, 6, 7, 8);
	command(f, CLC_SET_BLANK, 1);
	fmv_model_display(f.model, &d);
	eq("SetVideoFormat(3): PAL", d.video_format, 3);
	check(d.window[0] == 1 && d.window[3] == 4, "SetWindow arguments kept");
	check(d.border[0] == 5 && d.border[3] == 8, "SetBorder arguments kept");
	check(d.blank, "SetBlank(1)");
	command(f, CLC_SET_VIDEO_FORMAT, 5);
	fmv_model_display(f.model, &d);
	eq("SetVideoFormat accepts 3 and 4 only [CL 11-40]", d.video_format, 3);
	command(f, CLC_SET_BLANK, 0);
	fmv_model_display(f.model, &d);
	check(!d.blank, "SetBlank(0)");

	// The IO word's video-on bit (0x4000 as written), by byte lane
	f.write(FMV_OFF_IO, 0x4000);
	fmv_model_display(f.model, &d);
	check(d.video_on, "IO write with 0x4000: video on");
	fmv_event e;
	memset(&e, 0, sizeof e);
	e.offset = FMV_OFF_IO; e.lds = true; e.data = 0x0000;
	fmv_model_event(f.model, e);
	fmv_model_display(f.model, &d);
	check(d.video_on, "a low-byte write does not reach bit 14");
	e.uds = true; e.lds = false;
	fmv_model_event(f.model, e);
	fmv_model_display(f.model, &d);
	check(!d.video_on, "a high-byte write clears it");

	// Words written in IDLE wait in CMEM [CL 8-9]; they reach the decoder
	// too, in their place in the stream
	f.write(FMV_OFF_CMEM, 0x1122);
	f.write(FMV_OFF_CMEM, 0x3344);
	check(f.bytes.size() == 4 && f.bytes[0] == 0x11 && f.bytes[3] == 0x44,
	      "video_bytes: words held in CMEM are handed over, high byte first");
	command(f, CLC_FLUSH);
	f.bytes.clear();

	// The bitstream reaches the decoder in order, and each scheduled
	// picture asks for the next one to be shown
	f.write(CL + CL_HOST_CONTROL, 0x0080);
	command(f, CLC_PLAY);
	std::vector<uint8_t> es = { 0, 0, 1, 0xB3, 0x16, 0x01, 0x20, 0x23, 0xFF, 0xFF, 0xE0, 0x18 };
	for (int p = 0; p < 4; p++) {
		const uint8_t ph[] = { 0, 0, 1, 0x00, 0, (uint8_t)(1 << 3) };
		es.insert(es.end(), ph, ph + sizeof ph);
		for (int i = 0; i < 200; i++) es.push_back(0x55);
	}
	const uint8_t end[] = { 0, 0, 1, 0xB7 };
	es.insert(es.end(), end, end + 4);
	for (size_t i = 0; i < es.size(); i += 2)
		f.write(FMV_OFF_CMEM, (uint16_t)(es[i] << 8 | es[i + 1]));
	check(f.bytes == es, "video_bytes: every accepted byte, in order");
	fmv_model_advance(f.model, 200000);
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	eq("video_picture once per picture the schedule takes", f.pictures, (long)s.cl_pictures);
	check(f.pictures > 0, "pictures were taken");
	fmv_model_display(f.model, &d);
	check(!d.idle, "not IDLE while playing");

	int fl = f.flushes;
	command(f, CLC_FLUSH);
	eq("FlushBitstream() flushes the decoder", f.flushes, fl + 1);
	command(f, CLC_RESET);
	eq("Reset() flushes the decoder", f.flushes, fl + 2);
	fmv_model_free(f.model);
}

int main()
{
	test_window_defaults();
	test_window_args();
	test_regs();
	test_bufring();
	test_convert();
	test_model_hooks();
	if (failures) {
		printf("test_fmv_picture: %d FAILED\n", failures);
		return 1;
	}
	printf("test_fmv_picture: OK\n");
	return 0;
}
