// SPDX-License-Identifier: GPL-3.0-or-later
//
// CD32 FMV module chip model, phase 2 (logging). See fmv_model.h for what it
// is, what it is not, and the citation keys used below.

#include "fmv_model.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>

#include "fmv_picture.h"

const fmv_board fmv_board_default = { 0x8000, 0x4000, 0x0800, 0x0400, 0x0000, 0x4000, 0x0200 };

namespace {

// Command states [CL 11-5, figure 11-2].
enum { ST_IDLE = 0, ST_SETUP = 1, ST_PLAY = 2, ST_PAUSE = 3, ST_OTHER = 4 };

struct cmd_t { uint16_t w[5]; };

// A start code seen in the bitstream buffer, at absolute stream position pos
// (the byte after the 00 00 01 xx).
struct mark_t { uint64_t pos; uint8_t code; uint8_t ptype; };

const uint16_t l2_kbps[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192,
                               224, 256, 320, 384, 0 };          // [L64 4-15]
const uint16_t l1_kbps[16] = { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288,
                               320, 352, 384, 416, 448, 0 };
const uint32_t fs_hz[4] = { 44100, 48000, 32000, 0 };             // [L64 4-15]
// MPEG-1 picture_rate code to frame period, microseconds (ISO 11172-2).
const uint32_t pic_period_us[16] = { 0, 41708, 41667, 40000, 33367, 33333,
                                     20000, 16683, 16667, 0, 0, 0, 0, 0, 0, 0 };

} // namespace

struct fmv_model {
	fmv_hooks h;
	fmv_board b;
	uint16_t R[256], M[256];

	// ---- CL450 ----
	bool     cl_cpu_on;        // CPU_control CEn written 1
	int32_t  cl_init_us;       // >0: microapplication initialising
	bool     cl_running;
	int      cl_state, cl_play_cmd;
	int32_t  cl_setup_us;
	int32_t  cl_cmd_us;        // >0: a new command is being read
	bool     cmd_at_event;     // accept a command at its HOST_newcmd event (fmv_model_set_cmd_at_event)
	cmd_t    cl_cmd;
	bool     cl_reset_tail;    // Reset(): HMEM cleared after newcmd
	int32_t  cl_reset_us;
	std::deque<cmd_t> cl_fifo;
	uint16_t int_mask, int_pending;
	uint32_t threshold;
	uint64_t scr;              // 33-bit, 90 kHz
	uint32_t scr_frac;         // microseconds not yet counted
	// bitstream buffer: only its accounting; phase 3 keeps the bytes
	uint64_t in_pos, out_pos;  // bytes ever accepted / consumed
	uint32_t cmem_words;
	uint32_t cmem_overflow;
	uint32_t sc_shift;         // start code scanner
	int      sc_capture;       // bytes still to capture after a start code
	uint8_t  sc_code, sc_bytes[4];
	int      sc_nbytes;
	std::deque<mark_t> marks;
	uint16_t width, height;
	uint32_t frame_us;
	int32_t  decode_us, vsync_us, disp_us;
	bool     und_reported;
	uint8_t  dry_ticks;        // audio ticks in a row with the buffer empty, to 2
	int32_t  und_us;           // to the next audio tick
	bool     np_mode;          // a NewPacket since Play, a flush or reset
	uint32_t pictures;
	uint32_t commands;
	uint16_t last_cmd;
	// 68k activity since the last fmv_model_activity() (Main prints it
	// every 10 s): all events, and the reads the log leaves out
	uint32_t act_events, act_rd_newcmd, act_rd_dmactrl, act_rd_hostctl;
	uint32_t act_int0;         // INTs posted with Interrupt Status 0
	int32_t  l64_rearm_us;     // to the next re-send of a raised L64111 latch
	uint16_t window[4], border[4];
	uint16_t video_format;     // SetVideoFormat(): 3 PAL, 4 NTSC [CL 11-40]
	bool     blank;            // SetBlank(1) [CL 11-29]
	bool     io_video_on;      // the IO word's video-on bit, as last written
	bool     io_audio_mute;    // the IO word's audio mute bit, as last written
	// the frame being taken into the channel buffer, for audio_frame
	uint8_t  af_buf[2048];
	uint32_t af_len;
	bool     af_cap;
	uint16_t last_fullness;
	bool     cflevel;
	// data-port statistics, logged once a second rather than per word
	uint32_t stat_words, stat_l64_bytes;
	int32_t  stat_us;

	// ---- L64111 ----
	uint8_t  l64_mask1, l64_mask2;
	uint8_t  l64_st1, l64_st2;
	bool     l64_int;
	bool     l64_newout_ev, l64_last_ev;   // which event set status bit 0
	// system-stream pre-parser [L64 2-5]
	int      ps_state;
	uint32_t ps_shift, ps_left;
	uint8_t  ps_code;
	int      ps_hdr_left;      // bytes of the current header field still to come
	int      ps_kind;
	int      ps_pts_n;
	uint8_t  ps_pts[5];
	uint32_t packs_in_place;
	// audio framer
	uint8_t  af_hdr[4];
	int      af_hdr_n;
	uint32_t af_skip;          // bytes of the current frame still to come
	uint32_t af_in_place;      // consecutive syncs found where expected
	uint32_t frames_in, frames_out, cb_frames;
	uint32_t sample_rate;
	int32_t  play_us;
	bool     l64_sync_sys, l64_sync_aud, l64_pts_ok;
};

// ------------------------------------------------------------------ helpers

static void logf(fmv_model *m, const char *fmt, ...)
{
	if (!m->h.log)
		return;
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	m->h.log(m->h.ctx, line);
}

// Write register bits through the update queue, and keep the mirror in step
// with what the FPGA will hold.
static void set_reg(fmv_model *m, uint8_t i, uint16_t value, uint16_t mask, bool guard = false)
{
	uint16_t v = (uint16_t)((m->R[i] & ~mask) | (value & mask));
	if (v == m->R[i] && !guard)
		return;
	m->R[i] = v;
	fmv_update u = { i, false, guard, value, mask };
	if (m->h.update)
		m->h.update(m->h.ctx, u);
}

static void set_mask(fmv_model *m, uint8_t i, uint16_t mask)
{
	m->M[i] = mask;
	fmv_update u = { i, true, false, mask, 0xFFFF };
	if (m->h.update)
		m->h.update(m->h.ctx, u);
}

static uint8_t clx(uint32_t byte_off) { return (uint8_t)(FMV_IDX_CL450 + (byte_off >> 1)); }

static const char *cl_name(uint32_t o)
{
	switch (o) {
	case CL_VID_Y: return "VID_y"; case CL_CMEM_DATA: return "CMEM_data";
	case CL_VID_CHROMA: return "VID_chroma"; case CL_CPU_CONTROL: return "CPU_control";
	case CL_CPU_PC: return "CPU_pc"; case CL_CPU_INTENB: return "CPU_intenb";
	case CL_CPU_TADDR: return "CPU_taddr"; case CL_CPU_IADDR: return "CPU_iaddr";
	case CL_CPU_IMEM: return "CPU_imem"; case CL_CPU_TMEM: return "CPU_tmem";
	case CL_CPU_INT: return "CPU_int"; case CL_HOST_NEWCMD: return "HOST_newcmd";
	case CL_CMEM_CONTROL: return "CMEM_control"; case CL_CMEM_STATUS: return "CMEM_status";
	case CL_CMEM_DMACTRL: return "CMEM_dmactrl"; case CL_HOST_RADDR: return "HOST_raddr";
	case CL_HOST_RDATA: return "HOST_rdata"; case CL_HOST_CONTROL: return "HOST_control";
	case CL_HOST_SCR0: return "HOST_scr0"; case CL_HOST_SCR1: return "HOST_scr1";
	case CL_HOST_SCR2: return "HOST_scr2"; case CL_HOST_INTVECW: return "HOST_intvecw";
	case CL_HOST_INTVECR: return "HOST_intvecr"; case CL_DRAM_REFCNT: return "DRAM_refcnt";
	case CL_VID_CONTROL: return "VID_control"; case CL_VID_REGDATA: return "VID_regdata";
	}
	return "reserved";
}

static const char *cmd_name(uint16_t c)
{
	switch (c) {
	case CLC_SET_BLANK: return "SetBlank"; case CLC_SET_BORDER: return "SetBorder";
	case CLC_SET_COLOR_MODE: return "SetColorMode"; case CLC_SET_INT_MASK: return "SetInterruptMask";
	case CLC_SET_THRESHOLD: return "SetThreshold"; case CLC_SET_VIDEO_FORMAT: return "SetVideoFormat";
	case CLC_SET_WINDOW: return "SetWindow"; case CLC_DISPLAY_STILL: return "DisplayStill";
	case CLC_PAUSE: return "Pause"; case CLC_PLAY: return "Play"; case CLC_SCAN: return "Scan";
	case CLC_SINGLE_STEP: return "SingleStep"; case CLC_SLOW_MOTION: return "SlowMotion";
	case CLC_ACCESS_SCR: return "AccessSCR"; case CLC_FLUSH: return "FlushBitstream";
	case CLC_INQUIRE_FULLNESS: return "InquireBufferFullness"; case CLC_NEW_PACKET: return "NewPacket";
	case CLC_RESET: return "Reset";
	}
	return 0;
}

// ------------------------------------------------------------------ CL450

static uint32_t cl_fill(const fmv_model *m) { return (uint32_t)(m->in_pos - m->out_pos); }

static void cl_publish_cmem(fmv_model *m)
{
	// Empty status bits 8:5 [CL 8-10, table 8-5]: 1Q..4Q set at 4, 8, 12, 16
	// empty entries.
	uint32_t e = FMV_CMEM_WORDS - m->cmem_words;
	uint16_t q = (uint16_t)(((e >= 4) << 8) | ((e >= 8) << 7) | ((e >= 12) << 6) | ((e >= 16) << 5));
	set_reg(m, clx(CL_CMEM_DMACTRL), q, 0x01E0);
	// CMEM_status [CL 8-11]: only the difference counter is modelled.
	set_reg(m, clx(CL_CMEM_STATUS), (uint16_t)(m->cmem_words << 8), 0x1F00);
	// CFLEVEL [CL 8-10, table 8-6]: asserted on the empty bit the first
	// enable selects.
	// With no enable set, the board's CL450CFLEVEL bit reads 1 while CMEM
	// has room: WinUAE, the oracle for board wiring the manuals leave open,
	// reads it as 1 throughout (gate 1 run 2026-11-03: the ROM's first IO
	// read here was 0xc400, WinUAE's idle value is 0xc800).
	uint16_t en = m->R[clx(CL_CMEM_DMACTRL)];
	bool lvl = (en & 0x10) ? (q & 0x100) : (en & 0x08) ? (q & 0x80) :
	           (en & 0x04) ? (q & 0x40) : (en & 0x02) ? (q & 0x20) :
	           (m->cmem_words < FMV_CMEM_WORDS);
	if (lvl != m->cflevel) {
		m->cflevel = lvl;
		set_reg(m, FMV_IDX_IO, lvl ? m->b.io_cflevel : 0, m->b.io_cflevel);
	}
}

// An interrupt event [CL 12-7] as WinUAE (the oracle) posts it: the
// enabled bits join the pending set; if Interrupt Status (HMEM[0xA]) is 0,
// the pending set is written there and INT is driven by clearing Int,
// HOST_control bit 7 -- even when nothing enabled is pending, so a zero
// status still interrupts. A non-zero status blocks the event until the
// host clears it. Gate 1 (2026-11-03 ~20:05): with the per-frame PIC-D
// event masked out, the first version posted nothing at Play at all, and
// this ROM waits for an interrupt after Play before it sends any data.
static bool l64_level(const fmv_model *m);   // L64111 section below

static void cl_raise(fmv_model *m, uint16_t bits)
{
	m->int_pending |= (uint16_t)(bits & m->int_mask);
	if (!m->cl_cpu_on || m->R[FMV_IDX_HMEM + HMEM_INT_STATUS] != 0)
		return;
	uint16_t p = m->int_pending;
	m->int_pending = 0;
	if (p)
		set_reg(m, FMV_IDX_HMEM + HMEM_INT_STATUS, p, 0xFFFF);
	set_reg(m, clx(CL_HOST_CONTROL), 0x0000, 0x0080);
	if (p)
		logf(m, "FMV CL450 INT posted, status 0x%04x", p);
	else
		m->act_int0++;
}

// Post queued interrupts when Interrupt Status is 0 [CL 12-7, 12.2.1]: write
// them to HMEM[0xA] and drive INT by clearing Int, HOST_control bit 7
// [CL 8-12].
static void cl_try_post(fmv_model *m)
{
	if (!m->int_pending || m->R[FMV_IDX_HMEM + HMEM_INT_STATUS] != 0)
		return;
	uint16_t p = m->int_pending;
	m->int_pending = 0;
	set_reg(m, FMV_IDX_HMEM + HMEM_INT_STATUS, p, 0xFFFF);
	set_reg(m, clx(CL_HOST_CONTROL), 0x0000, 0x0080);
	logf(m, "FMV CL450 INT posted, status 0x%04x", p);
}

static void cl_publish_fullness(fmv_model *m, bool force)
{
	// HMEM[0xB], in bytes. The manual says "accurate to +-32 words" on
	// 11-19 but counts the threshold in bytes on 11-38. Words was the first
	// reading. WinUAE (the oracle) reports bytes, and this ROM compares the
	// value with a SetThreshold of 0x2000 (gate 1, 2026-11-03).
	uint32_t fb = cl_fill(m);
	uint16_t w = (uint16_t)(fb > 0xFFFF ? 0xFFFF : fb);
	int d = (int)w - (int)m->last_fullness;
	if (!force && d < 32 && d > -32)
		return;
	m->last_fullness = w;
	set_reg(m, FMV_IDX_HMEM + HMEM_FULLNESS, w, 0xFFFF);
}

// Table 10-1, "Initialization" and "Reset()" columns, and the HMEM clear
// [CL 10-2, 10-3]. The masks keep host-owned bits (AIC/VIE excepted, which
// the table sets to 0) untouched.
static void cl_defaults(fmv_model *m, bool init)
{
	set_reg(m, clx(CL_HOST_NEWCMD), 0, 0x0001);
	set_reg(m, clx(CL_CMEM_CONTROL), 0x0002, 0x0003);
	set_reg(m, clx(CL_HOST_CONTROL), 0x0081, 0xC081);
	if (init) {
		set_reg(m, clx(CL_HOST_SCR2), 0x1DE0, 0xFFFF);   // CS=1, Div=444
		set_reg(m, clx(CL_HOST_SCR1), 0, 0xFFFF);
		set_reg(m, clx(CL_HOST_SCR0), 0, 0xFFFF);
		m->int_mask = 0;                                  // Table 10-3
		m->threshold = 4096;
		m->video_format = 4;                              // NTSC
		m->blank = false;
		memset(m->window, 0, sizeof m->window);           // auto-size
		memset(m->border, 0, sizeof m->border);           // auto-centre, sub-black
	}
	for (int i = 0; i < 16; i++)
		set_reg(m, (uint8_t)(FMV_IDX_HMEM + i), 0, 0xFFFF);
	m->cl_state = ST_IDLE;
	m->cl_fifo.clear();
	m->int_pending = 0;
	m->in_pos = m->out_pos = 0;
	m->cmem_words = 0;
	m->marks.clear();
	m->sc_shift = 0xFFFFFFFF;
	m->sc_capture = 0;
	m->und_reported = false;
	m->dry_ticks = 0;
	m->np_mode = false;
	m->last_fullness = 0xFFFF;
	cl_publish_cmem(m);
	// DRAM-resident variable defaults [CL 10-8, table 10-7; appendix A]
	if (m->h.dram_write) {
		m->h.dram_write(m->h.ctx, 0x10, 0);   // SEQ_SEM
		m->h.dram_write(m->h.ctx, 0x11, 0);   // SEQ_CONTROL
		m->h.dram_write(m->h.ctx, 0x16, 0);   // PIC_SEM
	}
}

static void cl_sequence_header(fmv_model *m, const uint8_t *b)
{
	m->width  = (uint16_t)((b[0] << 4) | (b[1] >> 4));
	m->height = (uint16_t)(((b[1] & 0x0F) << 8) | b[2]);
	uint8_t rate = b[3] & 0x0F;
	if (pic_period_us[rate])
		m->frame_us = pic_period_us[rate];
	// Sequence variable group [CL appendix A, table A-2]
	if (m->h.dram_write) {
		m->h.dram_write(m->h.ctx, 0x12, m->width);
		m->h.dram_write(m->h.ctx, 0x13, m->height);
		m->h.dram_write(m->h.ctx, 0x14, rate);
	}
}

// Feed accepted bitstream bytes through the start-code scanner. Only the
// marks are kept: phase 2 decodes nothing.
static void cl_scan(fmv_model *m, uint8_t byte)
{
	m->in_pos++;
	if (m->sc_capture) {
		m->sc_bytes[m->sc_nbytes++] = byte;
		if (--m->sc_capture == 0) {
			if (m->sc_code == 0xB3)
				cl_sequence_header(m, m->sc_bytes);
			else if (m->sc_code == 0x00 && !m->marks.empty())
				m->marks.back().ptype = (uint8_t)((m->sc_bytes[1] >> 3) & 7);
		}
	}
	m->sc_shift = (m->sc_shift << 8) | byte;
	if ((m->sc_shift & 0xFFFFFF00) != 0x00000100)
		return;
	uint8_t code = (uint8_t)m->sc_shift;
	if (code != 0x00 && code != 0xB3 && code != 0xB7 && code != 0xB8)
		return;
	mark_t k = { m->in_pos, code, 0 };
	m->marks.push_back(k);
	if (code == 0xB3 || code == 0x00) {
		m->sc_code = code;
		m->sc_capture = code == 0xB3 ? 4 : 2;
		m->sc_nbytes = 0;
	}
}

static bool cl_accepting(const fmv_model *m)
{
	// IDLE does not accept bitstream data [CL 11-6]; nor does a halted CPU,
	// nor a cleared CRE [CL 8-9].
	return m->cl_running && m->cl_state != ST_IDLE &&
	       (m->R[clx(CL_CMEM_CONTROL)] & 0x0002) && !(m->R[clx(CL_CMEM_CONTROL)] & 0x0001);
}

static void cl_drain_cmem(fmv_model *m)
{
	// CMEM empties into the bitstream buffer while there is room [CL 8-9].
	// The words themselves were scanned on arrival; only the count waits.
	while (m->cmem_words && cl_accepting(m) && cl_fill(m) + 2 <= FMV_CL_BUFFER_BYTES)
		m->cmem_words--;
}

static void cl_video_word(fmv_model *m, uint16_t w)
{
	if (m->h.video_bytes) {
		uint8_t b[2] = { (uint8_t)(w >> 8), (uint8_t)w };
		m->h.video_bytes(m->h.vctx, b, 2);
	}
}

static void cl_video_flush(fmv_model *m)
{
	if (m->h.video_flush)
		m->h.video_flush(m->h.vctx);
}

static void cl_data_word(fmv_model *m, uint16_t w)
{
	m->stat_words++;
	if (m->R[clx(CL_CMEM_CONTROL)] & 0x0020)            // BS [CL 8-8]
		w = (uint16_t)((w >> 8) | (w << 8));
	if (cl_accepting(m) && !m->cmem_words && cl_fill(m) + 2 <= FMV_CL_BUFFER_BYTES) {
		cl_scan(m, (uint8_t)(w >> 8));
		cl_scan(m, (uint8_t)w);
		cl_video_word(m, w);
		m->und_reported = false;
		m->dry_ticks = 0;
		return;
	}
	if (m->cmem_words >= FMV_CMEM_WORDS) {
		// The host must not overrun CMEM [CL 8-9]; the word is lost.
		if (m->cmem_overflow++ == 0)
			logf(m, "FMV CL450 CMEM overflow: the 68k wrote past a full CMEM");
		return;
	}
	m->cmem_words++;
	cl_scan(m, (uint8_t)(w >> 8));
	cl_scan(m, (uint8_t)w);
	cl_video_word(m, w);
	cl_publish_cmem(m);
}

static void cl_set_state(fmv_model *m, int st, const char *why)
{
	if (st != m->cl_state)
		logf(m, "FMV CL450 state %d -> %d (%s)", m->cl_state, st, why);
	m->cl_state = st;
}

static void cl_execute(fmv_model *m, const cmd_t &c);

// Low-priority commands run when the state allows [CL 11-9]: at once in
// IDLE and PAUSE, otherwise after each decoded picture.
static void cl_run_fifo(fmv_model *m)
{
	while (!m->cl_fifo.empty()) {
		cmd_t c = m->cl_fifo.front();
		m->cl_fifo.pop_front();
		cl_execute(m, c);
		if (m->cl_state == ST_SETUP)   // a Play-type command suspends processing
			break;
	}
}

static void cl_execute(fmv_model *m, const cmd_t &c)
{
	const uint16_t *a = c.w;
	switch (a[0]) {
	case CLC_SET_INT_MASK:
		m->int_mask = a[1];
		break;
	case CLC_SET_THRESHOLD:
		m->threshold = a[1];
		break;
	case CLC_SET_WINDOW:
		memcpy(m->window, a + 1, sizeof m->window);
		break;
	case CLC_SET_BORDER:
		memcpy(m->border, a + 1, sizeof m->border);
		break;
	case CLC_SET_BLANK:
		m->blank = a[1] & 1;
		break;
	case CLC_SET_VIDEO_FORMAT:
		if ((a[1] & 7) == 3 || (a[1] & 7) == 4)
			m->video_format = a[1] & 7;
		break;
	case CLC_PLAY: case CLC_PAUSE: case CLC_SCAN: case CLC_SINGLE_STEP:
	case CLC_SLOW_MOTION: case CLC_DISPLAY_STILL: {
		int target = a[0] == CLC_PLAY ? ST_PLAY : a[0] == CLC_PAUSE ? ST_PAUSE : ST_OTHER;
		if (m->cl_state == ST_IDLE) {
			// IDLE -> PLAY-SETUP -> PLAY for the first picture, then the
			// state the command asked for [CL 11-6].
			m->cl_play_cmd = target;
			m->cl_setup_us = 1000;
			m->np_mode = false;
			cl_set_state(m, ST_SETUP, cmd_name(a[0]));
		} else {
			cl_set_state(m, target, cmd_name(a[0]));
		}
		break;
	}
	default:
		break;   // SetColorMode: logged; RGB is assumed (the default)
	}
}

// The microapplication reads HMEM[0..4] once HOST_newcmd is set [CL 8-15].
static void cl_accept_command(fmv_model *m)
{
	cmd_t c;
	for (int i = 0; i < 5; i++)
		c.w[i] = m->R[FMV_IDX_HMEM + i];
	m->commands++;
	m->last_cmd = c.w[0];
	const char *nm = cmd_name(c.w[0]);
	logf(m, "FMV CL450 command %s (0x%04x) %04x %04x %04x %04x", nm ? nm : "UNKNOWN",
	     c.w[0], c.w[1], c.w[2], c.w[3], c.w[4]);

	if (c.w[0] & 0x8000) {
		// High priority: done before HOST_newcmd is cleared [CL 11-12..11-27].
		switch (c.w[0]) {
		case CLC_INQUIRE_FULLNESS:
			cl_publish_fullness(m, true);
			break;
		case CLC_ACCESS_SCR:
			if (c.w[1] & 0x8000) {
				set_reg(m, FMV_IDX_HMEM + 1, (uint16_t)((m->scr >> 30) & 7), 0xFFFF);
				set_reg(m, FMV_IDX_HMEM + 2, (uint16_t)((m->scr >> 15) & 0x7FFF), 0xFFFF);
				set_reg(m, FMV_IDX_HMEM + 3, (uint16_t)(m->scr & 0x7FFF), 0xFFFF);
			} else {
				m->scr = ((uint64_t)(c.w[1] & 7) << 30) | ((uint64_t)(c.w[2] & 0x7FFF) << 15) |
				         (c.w[3] & 0x7FFF);
			}
			break;
		case CLC_FLUSH:
			// Discard the buffer and pending NewPacket()s [CL 11-16].
			m->out_pos = m->in_pos;
			m->cmem_words = 0;
			m->marks.clear();
			m->np_mode = false;
			m->sc_shift = 0xFFFFFFFF;
			for (auto it = m->cl_fifo.begin(); it != m->cl_fifo.end();)
				it = it->w[0] == CLC_NEW_PACKET ? m->cl_fifo.erase(it) : it + 1;
			cl_publish_cmem(m);
			cl_publish_fullness(m, true);
			cl_video_flush(m);
			break;
		case CLC_RESET:
			// Reset() is not complete when HOST_newcmd clears [CL 11-27]:
			// the HMEM clear, including HMEM[15], follows.
			m->cl_reset_tail = true;
			m->cl_reset_us = 1000;
			m->cl_state = ST_IDLE;
			m->cl_fifo.clear();
			cl_video_flush(m);
			break;
		default:
			if (!nm)
				logf(m, "FMV CL450 unknown function code 0x%04x: indeterminate [CL 11-11]", c.w[0]);
			break;
		}
	} else if (c.w[0] == CLC_NEW_PACKET) {
		// Queued until its data is decoded; nothing to execute [CL 11-20].
		m->np_mode = true;
	} else {
		if (m->cl_fifo.size() >= FMV_CL_CMD_FIFO)
			logf(m, "FMV CL450 command FIFO overflow [CL 11-7]");
		else
			m->cl_fifo.push_back(c);
		if (m->cl_state == ST_IDLE || m->cl_state == ST_PAUSE)
			cl_run_fifo(m);
	}
	set_reg(m, clx(CL_HOST_NEWCMD), 0, 0x0001);
}

// One picture time in a decoding state: decode the oldest complete picture.
// A picture is complete once a later start code has arrived after it.
static bool cl_decode_tick(fmv_model *m)
{
	while (!m->marks.empty() && m->marks.front().code != 0x00) {
		const mark_t &k = m->marks.front();
		if (k.code == 0xB3)
			cl_raise(m, CLI_SEQ_D);
		if (k.code == 0xB7)
			cl_raise(m, CLI_END_D | CLI_END_V);
		m->marks.pop_front();
	}
	if (m->marks.empty()) {
		// No picture in progress: what is left (headers, an end code) has
		// been read by the start-code scanner already, so the buffer is
		// empty, as WinUAE's decoder empties it. Without this a trailing
		// end code kept the fill above 0 and no underflow ever came.
		m->out_pos = m->in_pos;
		cl_publish_fullness(m, false);
	}
	if (m->marks.size() < 2) {
		// No complete picture: wait for one. Underflow is not judged here
		// but on the audio tick (see fmv_model_advance), as WinUAE has it.
		return false;
	}
	mark_t pic = m->marks.front();
	m->marks.pop_front();
	m->out_pos = m->marks.front().pos - 4;     // up to the next start code
	m->pictures++;
	if (m->h.video_picture)
		m->h.video_picture(m->h.vctx);
	// No PIC-V or GOP per decoded picture: WinUAE raises neither, and this
	// ROM enables PIC-V but never clears it from Interrupt Status, so each
	// would latch for good. PIC-D comes from the display tick instead.
	(void)pic;
	if (m->cl_state == ST_SETUP || m->cl_play_cmd) {
		if (m->cl_play_cmd) {
			cl_set_state(m, m->cl_play_cmd, "first picture decoded");
			m->cl_play_cmd = 0;
		}
	}
	if (m->cl_state == ST_OTHER)
		cl_raise(m, CLI_SCN);
	cl_drain_cmem(m);
	cl_publish_cmem(m);
	cl_run_fifo(m);
	return true;
}

static void cl_advance(fmv_model *m, int32_t us)
{
	if (m->cl_cpu_on && !m->cl_running) {
		m->cl_init_us -= us;
		if (m->cl_init_us <= 0) {
			m->cl_running = true;
			cl_defaults(m, true);
			logf(m, "FMV CL450 microapplication started, initialisation complete");
		}
	}
	if (!m->cl_running)
		return;

	if (m->cl_reset_tail) {
		m->cl_reset_us -= us;
		if (m->cl_reset_us <= 0) {
			m->cl_reset_tail = false;
			cl_defaults(m, false);
			logf(m, "FMV CL450 Reset() complete");
		}
	}
	if (m->cl_cmd_us > 0) {
		m->cl_cmd_us -= us;
		if (m->cl_cmd_us <= 0)
			cl_accept_command(m);
	}
	if (m->cl_state == ST_SETUP) {
		m->cl_setup_us -= us;
		if (m->cl_setup_us <= 0)
			cl_set_state(m, ST_PLAY, "PLAY-SETUP done");
	}

	// SCR at 90 kHz [CL 8-16]
	m->scr_frac += (uint32_t)us * 9;
	m->scr = (m->scr + m->scr_frac / 100) & 0x1FFFFFFFFULL;
	m->scr_frac %= 100;

	bool decoding = m->cl_state == ST_PLAY || m->cl_state == ST_OTHER ||
	                (m->cl_state == ST_SETUP && m->cl_play_cmd);
	if (decoding) {
		m->decode_us -= us;
		while (m->decode_us <= 0) {
			// A starved decoder starts again as soon as a picture is
			// complete, not a whole frame period later: retry at the next
			// display-time check.
			if (cl_decode_tick(m))
				m->decode_us += (int32_t)m->frame_us;
			else
				m->decode_us = 512;
		}
	}
	// Display-time checks about every eight lines [CL 11-38, 12-3]: buffer
	// fullness, RDY, and posting of queued interrupts.
	m->disp_us -= us;
	while (m->disp_us <= 0) {
		m->disp_us += 512;
		cl_drain_cmem(m);
		if (m->cl_state != ST_IDLE && m->cl_state != ST_SETUP) {
			cl_publish_fullness(m, false);
			// RDY: in NewPacket mode, the buffer has fallen below the
			// SetThreshold level, i.e. it is running low. WinUAE (the oracle)
			// has it so. The first version raised it while *free* space was
			// above the threshold, and with no NewPacket yet, so it fired
			// the moment PLAY-SETUP ended, before any data.
			if (m->np_mode && cl_fill(m) < m->threshold)
				cl_raise(m, CLI_RDY);
		}
		cl_try_post(m);
	}
	// The underflow check, once every 64 lines (4096 us), as WinUAE's audio
	// handler runs it. Empty-buffer ticks count up to 2 and reset when the
	// buffer holds data. From then on UND is raised on every empty tick, in
	// every state, Play or not. So the first tick after Play, with nothing
	// sent yet, posts 0x0100. That is this ROM's signal to start sending:
	// its ISR reads HMEM[0xA] and goes straight to NewPacket (gate 1 runs
	// v12/v13b). It never clears it, and in WinUAE nothing else does
	// either, so the status stays 0x0100 and the driver streams by polling.
	// The two versions before this gated UND on data having arrived since
	// Play; that took the start signal away (run v15).
	m->und_us -= us;
	while (m->und_us <= 0) {
		m->und_us += 4096;
		if (!m->cl_running)
			continue;
		if (cl_fill(m) == 0) {
			if (m->dry_ticks < 2)
				m->dry_ticks++;
			if (m->dry_ticks >= 2) {
				cl_raise(m, CLI_UND);
				if (!m->und_reported) {
					m->und_reported = true;
					logf(m, "FMV CL450 underflow: the bitstream buffer is empty");
				}
			}
		} else {
			m->dry_ticks = 0;
			m->und_reported = false;
		}
	}
	// PIC-D, once a display frame (40 ms, the PAL CD32's two fields) while
	// the microcode runs, in every state and before any data: WinUAE posts
	// it from its display timing, not per decoded picture.
	m->vsync_us -= us;
	while (m->vsync_us <= 0) {
		m->vsync_us += FMV_PIC_D_US;
		if (m->cl_running)
			cl_raise(m, CLI_PIC_D);
	}
}

static void cl_event(fmv_model *m, const fmv_event &e)
{
	uint32_t o = e.offset & 0xFE;
	if (e.read) {
		if (o != CL_HOST_NEWCMD && o != CL_CMEM_DMACTRL && o != CL_HOST_CONTROL)
			logf(m, "FMV R CL450 %-12s = 0x%04x%s", cl_name(o), e.data,
			     o == CL_HOST_RDATA ? " (HMEM)" : "");
		if (o == CL_HOST_RDATA)
			logf(m, "FMV     HMEM[0x%x]", e.haddr);
		return;
	}
	// The FPGA has already applied the write under the 68k mask; keep the
	// mirror in step the same way (fmv-contract.md).
	if (o == CL_HOST_RDATA) {
		uint8_t i = (uint8_t)(FMV_IDX_HMEM + (e.haddr & 15));
		m->R[i] = (uint16_t)((m->R[i] & ~m->M[i]) | (e.data & m->M[i]));
		uint8_t ra = clx(CL_HOST_RADDR);
		m->R[ra] = (uint16_t)((m->R[ra] & ~0x000F) | ((e.haddr + 1) & 0x000F));
		logf(m, "FMV W CL450 HMEM[0x%x] = 0x%04x", e.haddr, e.data);
		if ((e.haddr & 15) == HMEM_INT_STATUS && e.data == 0)
			cl_try_post(m);    // the host cleared Interrupt Status [CL 12-7]
		return;
	}
	uint8_t i = clx(o);
	m->R[i] = (uint16_t)((m->R[i] & ~m->M[i]) | (e.data & m->M[i]));
	if (o != CL_CPU_IMEM)
		logf(m, "FMV W CL450 %-12s = 0x%04x", cl_name(o), e.data);
	switch (o) {
	case CL_CPU_CONTROL:
		if ((e.data & 1) && !m->cl_cpu_on) {
			m->cl_cpu_on = true;
			m->cl_init_us = 2000;
			logf(m, "FMV CL450 CPU enabled at CPU_pc 0x%04x", m->R[clx(CL_CPU_PC)]);
		} else if (!(e.data & 1) && m->cl_cpu_on) {
			m->cl_cpu_on = m->cl_running = false;
			logf(m, "FMV CL450 CPU halted [CL 10-10]");
		}
		break;
	case CL_HOST_NEWCMD:
		if ((e.data & 1) && m->cl_running) {
			if (m->cmd_at_event) {
				// Accepted here, in ring order: every HMEM write before
				// it has been applied, and none after. Waiting for the
				// next fmv_model_advance() instead cost the 68k the rest of
				// the drain and the model's pass for each of ~110 commands
				// a second, spinning on HOST_newcmd (run v30).
				m->cl_cmd_us = 0;
				cl_accept_command(m);
			} else {
				m->cl_cmd_us = 20;
			}
		}
		break;
	case CL_CMEM_CONTROL:
		if (e.data & 0x0001) {           // CRst [CL 8-9]
			m->cmem_words = 0;
			cl_publish_cmem(m);
		}
		if (e.data & 0x0040) {           // Rst: software reset [CL 8-8]
			m->cl_cpu_on = m->cl_running = false;
			logf(m, "FMV CL450 software reset");
		}
		break;
	case CL_CMEM_DMACTRL:
		cl_publish_cmem(m);
		break;
	case CL_HOST_INTVECW:
		set_reg(m, clx(CL_HOST_INTVECR), e.data, 0xFFFF);
		break;
	default:
		break;
	}
}

// ----------------------------------------------------------------- L64111

static void l64_publish(fmv_model *m)
{
	set_reg(m, FMV_IDX_L64111 + 4, m->l64_st1, 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 5, m->l64_st2, 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 18, (uint16_t)(m->cb_frames > 63 ? 63 : m->cb_frames), 0x007F);
}

// INTR is a level: asserted while any status bit is set and enabled, and
// dropped by the read that clears the status (WinUAE, the oracle; it
// re-evaluates status & mask on every check). Mask bit positions differ from
// status bit positions in register 2 [L64 4-10, 4-11]: status bit n (1..6) is
// enabled by mask bit n+1, and status bit 0 by mask bit 1 (NEW FRAME OUT) or
// mask bit 0 (LAST FRAME). The first version raised INTR only at the instant
// a bit was set with its mask already on. With PTS AVAILABLE set before its
// mask, the line never rose (gate 1 run v16, 2026-11-03: status 2 0x38, mask
// 2 0x42, INTR 0, and the game waited for it to resume the CD).
static bool l64_level(const fmv_model *m)
{
	if (m->l64_st1 & m->l64_mask1)
		return true;
	for (int b = 1; b < 7; b++)
		if ((m->l64_st2 & (1 << b)) && (m->l64_mask2 & (1 << (b + 1))))
			return true;
	return (m->l64_newout_ev && (m->l64_mask2 & 2)) || (m->l64_last_ev && (m->l64_mask2 & 1));
}

static void l64_check_int(fmv_model *m)
{
	bool lvl = l64_level(m);
	if (lvl && !m->l64_int) {
		m->l64_int = true;
		set_reg(m, FMV_IDX_L64_INT, 1, 1, true);
	} else if (!lvl && m->l64_int) {
		// Both ways: a mask write that disables what is pending drops the
		// line, as a level does. A status read already dropped it in the
		// FPGA; this covers the mask.
		m->l64_int = false;
		set_reg(m, FMV_IDX_L64_INT, 0, 1);
	}
}

static void l64_raise(fmv_model *m, int reg, uint8_t status_bit, bool last_frame = false)
{
	if (reg == 1) {
		m->l64_st1 |= status_bit;
	} else {
		m->l64_st2 |= status_bit;
		if (status_bit == 1) {
			if (last_frame) m->l64_last_ev = true; else m->l64_newout_ev = true;
		}
	}
	l64_publish(m);
	l64_check_int(m);
}

static void l64_reset(fmv_model *m)
{
	m->l64_mask1 = m->l64_mask2 = 0;          // all disabled after reset [L64 4-9]
	m->l64_st1 = m->l64_st2 = 0;
	m->l64_int = false;
	m->l64_newout_ev = m->l64_last_ev = false;
	m->ps_state = 0;
	m->ps_shift = 0xFFFFFFFF;
	m->packs_in_place = 0;
	m->af_hdr_n = 0;
	m->af_skip = 0;
	m->af_in_place = 0;
	m->cb_frames = 0;
	m->play_us = 0;
	m->af_cap = false;
	m->l64_sync_sys = m->l64_sync_aud = m->l64_pts_ok = false;
	if (m->h.audio_flush)
		m->h.audio_flush(m->h.vctx);
	// Reset values [L64 4-4..4-8]: CR3 ignores the stream ID; CR1 STARTI and
	// CR2 AUDIO ONLY follow pins the board fixes, taken as 0 here.
	set_reg(m, FMV_IDX_L64111 + 1, 0x00, 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 2, 0x00, 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 3, 0x80, 0x00FF);
	set_reg(m, FMV_IDX_L64_INT, 0, 1);
	l64_publish(m);
}

static uint32_t l64_frame_bytes(const uint8_t *h)
{
	int layer = (h[1] >> 1) & 3;              // 3 = I, 2 = II, 1 = III
	uint32_t fs = fs_hz[(h[2] >> 2) & 3];
	int bri = h[2] >> 4, pad = (h[2] >> 1) & 1;
	if (!fs)
		return 0;
	if (layer == 2 && l2_kbps[bri])
		return 144000u * l2_kbps[bri] / fs + pad;
	if (layer == 3 && l1_kbps[bri])
		return (12000u * l1_kbps[bri] / fs + pad) * 4;
	return 0;
}

static void l64_frame_header(fmv_model *m, const uint8_t *h)
{
	uint32_t n = l64_frame_bytes(h);
	int layer = (h[1] >> 1) & 3;
	if (!n || layer == 1 || layer == 0) {
		// Layer III and reserved are syntax errors [L64 4-14].
		l64_raise(m, 2, 0x40);
		m->af_in_place = 0;
		return;
	}
	// Parametric data words [L64 4-14..4-17]
	uint8_t p1 = (uint8_t)(((h[1] >> 3) & 1) << 7 | (layer << 5) | ((h[1] & 1) << 4) | (h[2] >> 4));
	uint8_t p2 = (uint8_t)(((h[2] >> 2) & 3) << 6 | ((h[2] >> 1) & 1) << 5 | (h[2] & 1) << 4 |
	                       ((h[3] >> 6) & 3) << 2 | ((h[3] >> 4) & 3));
	uint8_t p3 = (uint8_t)(((h[3] >> 3) & 1) << 7 | ((h[3] >> 2) & 1) << 6 | (h[3] & 3) << 4);
	set_reg(m, FMV_IDX_L64111 + 9, p1, 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 10, p2, 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 11, p3, 0x00FF);
	m->sample_rate = fs_hz[(h[2] >> 2) & 3];
	m->af_skip = n - 4;
	m->frames_in++;
	l64_raise(m, 2, 0x04);                         // FRAME DETECT IN
	if (++m->af_in_place == 3 && !m->l64_sync_aud) {
		m->l64_sync_aud = true;                    // three in place [L64 4-11]
		l64_raise(m, 2, 0x10);
		logf(m, "FMV L64111 synchronised to the audio stream, %u Hz", m->sample_rate);
	}
	if (m->cb_frames >= FMV_L64_CB_FRAMES) {
		l64_raise(m, 1, 0x08);                     // ERR BUF OVFL [L64 4-10]
		return;
	}
	m->cb_frames++;
	l64_publish(m);
	// Keep the frame for the decoder (phase 4): the header now, the rest
	// as it arrives.
	if (m->h.audio_frame && n <= sizeof m->af_buf) {
		memcpy(m->af_buf, h, 4);
		m->af_len = 4;
		m->af_cap = true;
	}
}

// Audio elementary-stream bytes: find frames [L64 2-5, 2-7].
static void l64_audio_byte(fmv_model *m, uint8_t byte)
{
	if (m->af_skip) {
		m->af_skip--;
		if (m->af_cap) {
			m->af_buf[m->af_len++] = byte;
			if (!m->af_skip) {
				m->af_cap = false;
				m->h.audio_frame(m->h.vctx, m->af_buf, m->af_len, m->sample_rate);
			}
		}
		return;
	}
	m->af_cap = false;          // a frame cut short (resync): not handed over
	if (m->af_hdr_n == 0 && byte != 0xFF) {
		m->af_in_place = 0;                        // not where a sync should be
		return;
	}
	if (m->af_hdr_n == 1 && (byte & 0xF0) != 0xF0) {
		m->af_hdr_n = 0;
		m->af_in_place = 0;
		return;
	}
	m->af_hdr[m->af_hdr_n++] = byte;
	if (m->af_hdr_n == 4) {
		m->af_hdr_n = 0;
		l64_frame_header(m, m->af_hdr);
	}
}

// A presentation time stamp from a packet header: into registers 12-16 and
// PTS AVAILABLE [L64 4-11, 4-17].
static void l64_pts(fmv_model *m)
{
	const uint8_t *p = m->ps_pts;
	uint64_t pts = ((uint64_t)(p[0] >> 1 & 7) << 30) | ((uint64_t)p[1] << 22) |
	               ((uint64_t)(p[2] >> 1) << 15) | ((uint64_t)p[3] << 7) | (p[4] >> 1);
	for (int k = 0; k < 4; k++)
		set_reg(m, (uint8_t)(FMV_IDX_L64111 + 12 + k), (uint8_t)(pts >> (8 * k)), 0x00FF);
	set_reg(m, FMV_IDX_L64111 + 16, (uint16_t)((pts >> 32) & 1), 0x00FF);
	// Every audio PTS once synchronised raises PTS AVAILABLE, as WinUAE has
	// it. The first version raised it once per session, and this game
	// enables it (mask 2 0x42) and paces its CD reads on it.
	m->l64_pts_ok = true;
	if (m->l64_sync_aud)
		l64_raise(m, 2, 0x20);
}

// ISO 11172-1 system stream pre-parser [L64 1-6, 2-5]. States:
// 0 scan, 1 length hi, 2 length lo, 3 packet header, 4 payload, 5 skip.
static void l64_system_byte(fmv_model *m, uint8_t byte)
{
	switch (m->ps_state) {
	case 0:
		m->ps_shift = (m->ps_shift << 8) | byte;
		if ((m->ps_shift & 0xFFFFFF00) != 0x00000100)
			return;
		m->ps_code = byte;
		if (byte == 0xBA) {
			if (++m->packs_in_place == 3 && !m->l64_sync_sys) {
				m->l64_sync_sys = true;
				l64_raise(m, 2, 0x08);             // SYNC SYS [L64 4-11]
			}
			m->ps_left = 8;                        // MPEG-1 pack header body
			m->ps_state = 5;
		} else if (byte >= 0xBB) {
			m->ps_state = 1;
		}
		return;
	case 1:
		m->ps_left = (uint32_t)byte << 8;
		m->ps_state = 2;
		return;
	case 2: {
		m->ps_left |= byte;
		uint8_t id = m->ps_code;
		uint8_t cr3 = (uint8_t)m->R[FMV_IDX_L64111 + 3];
		bool audio = id >= 0xC0 && id <= 0xDF &&
		             ((cr3 & 0x80) || (id & 0x1F) == (cr3 & 0x1F));   // [L64 4-8]
		m->ps_state = audio ? 3 : 5;
		m->ps_hdr_left = 0;
		m->ps_pts_n = 0;
		m->ps_kind = 0;
		if (!m->ps_left)
			m->ps_state = 0;
		return;
	}
	case 3:      // packet header: stuffing, STD buffer, PTS/DTS [ISO 11172-1]
		m->ps_left--;
		if (m->ps_hdr_left) {
			// inside a field: ps_kind 1 = STD buffer, 2 = PTS, 3 = PTS+DTS
			if (m->ps_kind >= 2 && m->ps_pts_n < 5)
				m->ps_pts[m->ps_pts_n++] = byte;
			if (--m->ps_hdr_left == 0 && m->ps_kind >= 2) {
				l64_pts(m);
				m->ps_state = 4;                   // a time stamp ends the header
			}
		} else if (byte == 0xFF) {
			// stuffing
		} else if ((byte & 0xC0) == 0x40) {
			m->ps_kind = 1;
			m->ps_hdr_left = 1;                    // second STD byte
		} else if ((byte & 0xF0) == 0x20 || (byte & 0xF0) == 0x30) {
			m->ps_kind = (byte & 0xF0) == 0x30 ? 3 : 2;
			m->ps_pts[0] = byte;
			m->ps_pts_n = 1;
			m->ps_hdr_left = m->ps_kind == 3 ? 9 : 4;
		} else if (byte == 0x0F) {
			m->ps_state = 4;                       // no time stamp
		} else {
			m->ps_state = 4;                       // malformed: take it as data
			l64_audio_byte(m, byte);
		}
		if (!m->ps_left)
			m->ps_state = 0;
		return;
	case 4:
		l64_audio_byte(m, byte);
		if (--m->ps_left == 0)
			m->ps_state = 0;
		return;
	case 5:
		if (--m->ps_left == 0)
			m->ps_state = 0;
		return;
	}
}

static void l64_data_byte(fmv_model *m, uint8_t byte)
{
	m->stat_l64_bytes++;
	if (m->R[FMV_IDX_L64111 + 2] & 0x08)          // AUDIO ONLY [L64 4-7]
		l64_audio_byte(m, byte);
	else
		l64_system_byte(m, byte);
}

static void l64_advance(fmv_model *m, int32_t us)
{
	bool start = m->R[FMV_IDX_L64111 + 1] & 0x01;   // STARTI [L64 4-6]
	if (!start || !m->sample_rate)
		return;
	if (!m->cb_frames) {
		m->play_us = 0;                            // waits for data [L64 4-6]
		return;
	}
	int32_t frame_us = (int32_t)(1152ull * 1000000ull / m->sample_rate);
	m->play_us += us;
	while (m->play_us >= frame_us && m->cb_frames) {
		m->play_us -= frame_us;
		m->cb_frames--;
		m->frames_out++;
		if (m->h.audio_tick)
			m->h.audio_tick(m->h.vctx);
		l64_raise(m, 2, 0x01, false);              // NEW FRAME OUT
		if (m->cb_frames == 1)
			l64_raise(m, 2, 0x01, true);           // LAST FRAME
	}
	l64_publish(m);
}

static void l64_event(fmv_model *m, const fmv_event &e)
{
	int r = (int)((e.offset >> 1) & 0x1F);
	if (e.read) {
		if (r == 4 || r == 5) {
			// Reading either status register deasserts INTR [L64 4-9]; the
			// FPGA already did. Event-type bits are taken to clear on the
			// read that reports them (an assumption: the manual says they
			// "continue to reflect" status, which for an event is "since
			// the last read").
			m->l64_int = false;
			// The read clears the register it reports, all of it, as WinUAE
			// has it (the first version kept the sync and PTS bits as
			// levels). INTR then follows what is still set and enabled.
			if (r == 4)
				m->l64_st1 = 0;
			else {
				m->l64_st2 = 0;
				m->l64_newout_ev = m->l64_last_ev = false;
			}
			l64_publish(m);
			l64_check_int(m);
		}
		if (r != 4 && r != 5 && r != 18)
			logf(m, "FMV R L64111 reg %2d = 0x%02x", r, e.data & 0xFF);
		return;
	}
	uint8_t i = (uint8_t)(FMV_IDX_L64111 + r);
	m->R[i] = (uint16_t)((m->R[i] & ~m->M[i]) | (e.data & m->M[i]));
	if (r == 0) {
		// The data register: 8 bits on D[7:0] by default, 16 bits when
		// SEL_16/8_BIT is set [L64 4-4, 4-7]. High byte first is assumed.
		if (m->R[FMV_IDX_L64111 + 2] & 0x10)
			l64_data_byte(m, (uint8_t)(e.data >> 8));
		l64_data_byte(m, (uint8_t)e.data);
		return;
	}
	logf(m, "FMV W L64111 reg %2d = 0x%02x", r, e.data & 0xFF);
	switch (r) {
	case 1:
		if (e.data & 0x02) {                       // SOFT_RST, self-clearing
			l64_reset(m);
			set_reg(m, FMV_IDX_L64111 + 1, 0, 0x0002);
			logf(m, "FMV L64111 soft reset");
		}
		if (e.data & 0x04) {                       // BUF_RST: not self-clearing
			m->cb_frames = 0;
			m->af_cap = false;
			l64_publish(m);
			if (m->h.audio_flush)
				m->h.audio_flush(m->h.vctx);
		}
		break;
	case 4: m->l64_mask1 = (uint8_t)e.data; l64_check_int(m); break;
	case 5: m->l64_mask2 = (uint8_t)e.data; l64_check_int(m); break;
	case 19: case 20: case 25: case 26:
		logf(m, "FMV L64111 channel buffer counter write at reg %d: cue/review "
		     "(the manual gives these as 19/20 and 25/26) [L64 4-3, 4-19]", r);
		break;
	}
}

// ------------------------------------------------------------------- masks

// The 68k write masks: which bits the host owns in each register, from the
// manuals' R/W columns. Read-only and write-only registers get 0: their
// writes still reach the model through the event ring.
static void push_masks(fmv_model *m)
{
	static const struct { uint8_t o; uint16_t mask; } cl[] = {
		{ CL_CPU_CONTROL, 0xFFFF }, { CL_CPU_PC, 0xFFFF }, { CL_CPU_INTENB, 0xFFFF },
		{ CL_CPU_TADDR, 0xFFFF }, { CL_CPU_IADDR, 0xFFFF }, { CL_CPU_TMEM, 0xFFFF },
		{ CL_CPU_INT, 0xFFFF }, { CL_HOST_NEWCMD, 0x0001 },
		{ CL_CMEM_CONTROL, 0x0063 },  // Rst, BS, CRE, CRst [CL 8-8, 8-9]
		{ CL_CMEM_DMACTRL, 0x001F },  // enables and DE; 8:5 are status [CL 8-9]
		{ CL_HOST_RADDR, 0x000F },    // [CL 8-13]
		{ CL_HOST_CONTROL, 0xC080 },  // AIC, VIE, Int [CL 8-12]
		{ CL_HOST_SCR0, 0xFFFF }, { CL_HOST_SCR1, 0xFFFF }, { CL_HOST_SCR2, 0xFFFF },
		{ CL_DRAM_REFCNT, 0xFFFF }, { CL_VID_CONTROL, 0xFFFF },
	};
	for (int i = 0; i < 256; i++)
		if (i < FMV_IDX_CTRL)
			set_mask(m, (uint8_t)i, 0);
	for (auto &e : cl)
		set_mask(m, clx(e.o), e.mask);
	for (int i = 0; i < 16; i++)
		set_mask(m, (uint8_t)(FMV_IDX_HMEM + i), 0xFFFF);   // [CL 8-14]
	// L64111 [L64 table 4.1]: control 1-3, the timers 6-8 and the channel
	// buffer counters are host read/write, 8 bits on D[7:0] (the low byte
	// is assumed: board wiring).
	const int rw[] = { 1, 2, 3, 6, 7, 8 };
	for (int r : rw)
		set_mask(m, (uint8_t)(FMV_IDX_L64111 + r), 0x00FF);
	const int cb[] = { 19, 20, 25, 26 };
	for (int r : cb)
		set_mask(m, (uint8_t)(FMV_IDX_L64111 + r), 0x007F);
}

// ------------------------------------------------------------------- API

fmv_model *fmv_model_new(const fmv_hooks &hooks)
{
	fmv_model *m = new fmv_model();
	memset(m->R, 0, sizeof m->R);
	memset(m->M, 0, sizeof m->M);
	m->h = hooks;
	m->b = fmv_board_default;
	m->cmd_at_event = true;
	fmv_model_reset(m);
	return m;
}

void fmv_model_set_cmd_at_event(fmv_model *m, bool on) { m->cmd_at_event = on; }
uint32_t fmv_model_commands(const fmv_model *m) { return m->commands; }

void fmv_model_free(fmv_model *m) { delete m; }

void fmv_model_set_video_hooks(fmv_model *m, void *vctx,
                               void (*bytes)(void *, const uint8_t *, size_t),
                               void (*flush)(void *), void (*picture)(void *))
{
	m->h.vctx = vctx;
	m->h.video_bytes = bytes;
	m->h.video_flush = flush;
	m->h.video_picture = picture;
}

void fmv_model_reset(fmv_model *m)
{
	push_masks(m);
	m->cl_cpu_on = m->cl_running = false;
	m->cl_init_us = 0;
	m->cl_state = ST_IDLE;
	m->cl_play_cmd = 0;
	m->cl_cmd_us = 0;
	m->cl_reset_tail = false;
	m->cl_fifo.clear();
	m->int_mask = m->int_pending = 0;
	m->threshold = 4096;
	m->scr = 0;
	m->scr_frac = 0;
	m->in_pos = m->out_pos = 0;
	m->cmem_words = 0;
	m->cmem_overflow = 0;
	m->marks.clear();
	m->sc_shift = 0xFFFFFFFF;
	m->sc_capture = 0;
	m->width = 352;                 // sequence defaults [CL table 10-4]
	m->height = 240;
	m->frame_us = pic_period_us[4];
	m->decode_us = 0;
	m->vsync_us = 0;
	m->disp_us = 512;
	m->und_reported = false;
	m->dry_ticks = 0;
	m->np_mode = false;
	m->pictures = 0;
	m->last_fullness = 0xFFFF;
	m->cflevel = false;
	m->stat_words = m->stat_l64_bytes = 0;
	m->stat_us = 0;
	memset(m->window, 0, sizeof m->window);
	memset(m->border, 0, sizeof m->border);
	m->video_format = 4;
	m->blank = false;
	m->io_video_on = false;
	m->io_audio_mute = false;
	cl_video_flush(m);
	// Hardware reset of the CL450: Int is 1 (INT inactive) [CL table 10-6].
	set_reg(m, clx(CL_HOST_CONTROL), 0x0080, 0xFFFF);
	set_reg(m, clx(CL_HOST_NEWCMD), 0, 0xFFFF);
	cl_publish_cmem(m);
	// Board wiring for the IO word's interrupt mirrors (fmv-contract.md).
	set_reg(m, FMV_IDX_IOMAP_CL, m->b.io_cl450_int, 0xFFFF);
	set_reg(m, FMV_IDX_IOMAP_L64, m->b.io_l64111_int, 0xFFFF);
	set_reg(m, FMV_IDX_IOPOL, m->b.io_int_pol, 0xFFFF);
	// The audio decoder's FIFO never fills here (the pre-parser is
	// instant), so FALF reads as inactive. WinUAE never sets this bit, so
	// inactive is 0 on this board (the first version took the pin's
	// active-low sense and idled it high; see cl_publish_cmem for the run).
	set_reg(m, FMV_IDX_IO, 0, m->b.io_l64_falf);
	l64_reset(m);
	logf(m, "FMV model reset");
}

void fmv_model_event(fmv_model *m, const fmv_event &e)
{
	m->act_events++;
	if (e.read && (e.offset & 0xF0000) == FMV_OFF_CL450) {
		uint32_t o = e.offset & 0xFE;
		if (o == CL_HOST_NEWCMD) m->act_rd_newcmd++;
		else if (o == CL_CMEM_DMACTRL) m->act_rd_dmactrl++;
		else if (o == CL_HOST_CONTROL) m->act_rd_hostctl++;
	}
	if (e.reset) {
		logf(m, "FMV Amiga reset");
		fmv_model_reset(m);
		return;
	}
	uint32_t o = e.offset & 0xFFFFE;
	if (o < FMV_OFF_IO) {
		logf(m, "FMV W ROM +0x%05x = 0x%04x: write to ROM dropped", o, e.data);
	} else if (o < FMV_OFF_L64111) {
		if (e.read)
			logf(m, "FMV R IO = 0x%04x", e.data);
		else {
			logf(m, "FMV W IO = 0x%04x", e.data);
			// Only a write that reaches the bit's byte lane changes it.
			uint16_t lanes = (uint16_t)((e.uds ? 0xFF00 : 0) | (e.lds ? 0x00FF : 0));
			if (lanes & m->b.io_video_on)
				m->io_video_on = (e.data & m->b.io_video_on & lanes) != 0;
			if (lanes & m->b.io_audio_mute)
				m->io_audio_mute = (e.data & m->b.io_audio_mute & lanes) != 0;
		}
	} else if (o < FMV_OFF_CMEM) {
		l64_event(m, e);
	} else if (o < FMV_OFF_CL450) {
		if (e.read)
			logf(m, "FMV R CMEM: write-only port read [CL table 4-1]");
		else
			cl_data_word(m, e.data);
	} else if (o < FMV_OFF_RAM) {
		cl_event(m, e);
	}
}

void fmv_model_advance(fmv_model *m, uint32_t usec)
{
	// Advance in slices no longer than a display-time check, so events
	// keep their order relative to one another.
	while (usec) {
		int32_t s = usec > 256 ? 256 : (int32_t)usec;
		usec -= (uint32_t)s;
		cl_advance(m, s);
		l64_advance(m, s);
		// The L64111 latch, re-sent every 20 ms while the level holds. A raise
		// is guarded: the FPGA drops one made before the 68k's latest status
		// read, and Main is to raise it again once it has caught up if the
		// source is still active (fmv-contract.md, the update queue). The model
		// never did that, so a dropped raise would leave it believing the latch
		// was up while the FPGA had it down. Gate 1 run v17 ended in that state
		// as far as Main could see (model INTR 1, the 68k idle). Re-sending is
		// idempotent in the FPGA.
		m->l64_rearm_us -= s;
		while (m->l64_rearm_us <= 0) {
			m->l64_rearm_us += 20000;
			if (m->l64_int && l64_level(m))
				set_reg(m, FMV_IDX_L64_INT, 1, 1, true);
		}
		m->stat_us += s;
		if (m->stat_us >= 1000000) {
			if (m->stat_words || m->stat_l64_bytes)
				logf(m, "FMV stream: %u CL450 data words, %u L64111 bytes in the last second; "
				     "video buffer %u bytes, %u pictures; audio %u frames in, %u out",
				     m->stat_words, m->stat_l64_bytes, cl_fill(m), m->pictures,
				     m->frames_in, m->frames_out);
			m->stat_words = m->stat_l64_bytes = 0;
			m->stat_us -= 1000000;
		}
	}
}

uint16_t fmv_model_reg(const fmv_model *m, uint8_t index) { return m->R[index]; }

void fmv_model_set_audio_hooks(fmv_model *m,
                               void (*frame)(void *, const uint8_t *, size_t, uint32_t),
                               void (*tick)(void *), void (*flush)(void *))
{
	m->h.audio_frame = frame;
	m->h.audio_tick = tick;
	m->h.audio_flush = flush;
}

bool fmv_model_audio_muted(const fmv_model *m)
{
	return (m->R[FMV_IDX_L64111 + 2] & 0x20) || m->io_audio_mute;
}

void fmv_model_display(const fmv_model *m, fmv_cl_display *d)
{
	d->seq_w = m->width;
	d->seq_h = m->height;
	memcpy(d->window, m->window, sizeof d->window);
	memcpy(d->border, m->border, sizeof d->border);
	d->video_format = m->video_format;
	d->blank = m->blank;
	d->idle = !m->cl_running || m->cl_state == ST_IDLE;
	d->video_on = m->io_video_on;
}

void fmv_model_get_state(const fmv_model *m, fmv_model_state *s)
{
	memset(s, 0, sizeof *s);
	s->cl_running = m->cl_running;
	s->cl_state = m->cl_state;
	s->cl_buffer_bytes = cl_fill(m);
	s->cl_pictures = m->pictures;
	s->cl_cmem_overflow = m->cmem_overflow;
	s->cl_int_mask = m->int_mask;
	s->cl_int_pending = m->int_pending;
	s->cl_commands = m->commands;
	s->cl_last_command = m->last_cmd;
	memcpy(s->cl_window, m->window, sizeof s->cl_window);
	memcpy(s->cl_border, m->border, sizeof s->cl_border);
	s->cl_width = m->width;
	s->cl_height = m->height;
	s->cl_frame_period_us = m->frame_us;
	s->l64_started = m->R[FMV_IDX_L64111 + 1] & 1;
	s->l64_sync_sys = m->l64_sync_sys;
	s->l64_sync_aud = m->l64_sync_aud;
	s->l64_frames_in = m->frames_in;
	s->l64_frames_out = m->frames_out;
	s->l64_cb_frames = m->cb_frames;
	s->l64_sample_rate = m->sample_rate;
	s->l64_int = m->l64_int;
}

void fmv_model_activity(fmv_model *m, char *out, size_t len)
{
	static const char *st[] = { "IDLE", "SETUP", "PLAY", "PAUSE", "OTHER" };
	snprintf(out, len,
	         "FMV activity: %u board accesses from the 68k since the last line "
	         "(unlogged reads: HOST_newcmd %u, CMEM_dmactrl %u, HOST_control %u); "
	         "%u INTs with status 0; "
	         "CL450 %s%s, %u commands, last 0x%04x, int mask 0x%04x pending 0x%04x, "
	         "HMEM[0xA] 0x%04x, HOST_control 0x%04x, threshold %u, CMEM %u words; "
	         "L64111 INTR %u, status 1 0x%02x 2 0x%02x, mask 1 0x%02x 2 0x%02x, "
	         "channel buffer %u frames, sync audio %u system %u",
	         m->act_events, m->act_rd_newcmd, m->act_rd_dmactrl, m->act_rd_hostctl, m->act_int0,
	         st[m->cl_state < 5 ? m->cl_state : 4], m->cl_running ? "" : " (microcode not running)",
	         m->commands, m->last_cmd, m->int_mask, m->int_pending,
	         m->R[FMV_IDX_HMEM + 0xA], m->R[clx(CL_HOST_CONTROL)], m->threshold, m->cmem_words,
	         m->l64_int, m->l64_st1, m->l64_st2, m->l64_mask1, m->l64_mask2, m->cb_frames,
	         m->l64_sync_aud, m->l64_sync_sys);
	m->act_events = m->act_rd_newcmd = m->act_rd_dmactrl = m->act_rd_hostctl = 0;
	m->act_int0 = 0;
}
