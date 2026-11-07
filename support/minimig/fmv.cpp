// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module, Main's side: the switch, the DDR3 window, the ROM
// file and the clock. Everything else is in fmv_transport.cpp (the contract
// plumbing) and fmv_model.cpp (the chips), which are pure and tested on the
// host. See fmv.h.

#include "fmv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../cfg.h"
#include "../../file_io.h"
#include "../../shmem.h"
#include "../../spi.h"
#include "../../fpga_io.h"
#include "../../user_io.h"
#include "fmv_transport.h"
#include "fmv_model.h"
#include "fmv_picture.h"
#include "fmv_decoder.h"
#include "fmv_audio.h"
#include "akiko_cd32.h"
#include "minimig_config.h"
#include "minimig_crc32.h"
#include "minimig_armcpu.h"
#include "minimig_profile.h"

static fmv_xport *xport;
static volatile uint8_t *window;

// ---- the picture (phase 3, contract v5) ----
static volatile uint8_t *fbmap;     // the four frame buffers
static fmv_decoder *decoder;
static int      cur_buf;            // the buffer named in DISP_CTRL
static bool     have_picture;       // one named since the last flush
static uint16_t disp_sent[7];
static bool     disp_valid;         // disp_sent is what the core holds
static fmv_window win_sent;

// ---- the sound (phase 4) ----
static fmv_audio *audio;
static uint32_t   audio_full;        // polls with a sector waiting and the FIFO full
static uint32_t   audio_cdda;        // ... and CD audio owning the FIFO
// The CDDA FIFO's own counters are 16 bits and wrap (run v26: underruns
// fell from one line to the next). Read every 100 ms and summed here into
// 32-bit totals for the session, they cannot wrap between two reads: the
// FIFO counts at most 44100 underrun periods a second.
static uint16_t   cdda_under_last, cdda_starve_last;
static uint32_t   cdda_under_total, cdda_starve_total;
static bool       cdda_base;
static uint64_t   cdda_read_us;

static void cdda_accumulate(uint64_t t)
{
	if (!cdda_base || t - cdda_read_us < 100000)
		return;
	cdda_read_us = t;
	uint16_t u, sv;
	if (!akiko_cd32_cdda_counters(&u, &sv))
		return;
	cdda_under_total  += (uint16_t)(u - cdda_under_last);
	cdda_starve_total += (uint16_t)(sv - cdda_starve_last);
	cdda_under_last = u;
	cdda_starve_last = sv;
}

static void aud_frame(void *, const uint8_t *p, size_t n, uint32_t rate)
{
	if (audio) fmv_audio_frame(audio, p, n, rate);
}

static void aud_tick(void *)
{
	if (audio) fmv_audio_tick(audio, fmv_model_audio_muted(fmv_xport_model(xport)));
}

static void aud_flush(void *)
{
	if (audio) fmv_audio_flush(audio);
}

// Hand the core what is due. The CDDA FIFO asks for a sector while it holds
// at most 1459 frames (33 ms); more than about 100 ms waiting here means
// the FIFO is not taking it (CD audio owns it, or Main stalled), and the
// sound would trail the picture by that much for good: drop it.
static void audio_poll()
{
	if (!audio)
		return;
	int16_t sec[2 * FMV_AUDIO_SECTOR_FRAMES];
	for (int k = 0; k < 2 && fmv_audio_pending(audio) >= FMV_AUDIO_SECTOR_FRAMES; k++) {
		int r = akiko_cd32_pcm_room();
		if (r) {
			if (r == 1) audio_cdda++; else audio_full++;
			break;
		}
		fmv_audio_sector(audio, sec);
		akiko_cd32_pcm_push(sec);
	}
	if (fmv_audio_pending(audio) > 8 * FMV_AUDIO_SECTOR_FRAMES)
		fmv_audio_drop_output(audio);
}

static void vid_bytes(void *, const uint8_t *p, size_t n)
{
	if (decoder) fmv_decoder_bytes(decoder, p, n);
}

static void vid_flush(void *)
{
	if (decoder) fmv_decoder_flush(decoder);
	have_picture = false;
}

// The model's picture schedule: show the next decoded picture, if there is
// one. If the decoder is late the one shown stays, as a CL450 repeats a
// picture it has not replaced.
static void vid_picture(void *)
{
	if (!decoder)
		return;
	int b = fmv_decoder_present(decoder);
	if (b >= 0) {
		cur_buf = b;
		have_picture = true;
	}
}

static fmv_calib calib()
{
	fmv_calib c;
	c.hofs = FMV_CALIB_HOFS + cfg.amigacd_fmv_hofs;
	c.vofs = FMV_CALIB_VOFS + cfg.amigacd_fmv_vofs;
	c.force_on = cfg.amigacd_fmv_show >= 1;
	c.key_all = cfg.amigacd_fmv_show >= 2;
	return c;
}

// The display registers, sent whenever they change (and all of them once a
// session starts: the core keeps 0xE0-0xE6 across sessions).
static void disp_update()
{
	fmv_cl_display d;
	fmv_model_display(fmv_xport_model(xport), &d);
	fmv_calib c = calib();
	fmv_window w;
	fmv_window_compute(d, c, &w);
	if (decoder && memcmp(&w, &win_sent, sizeof w)) {
		fmv_decoder_set_window(decoder, w);
		win_sent = w;
		printf("FMV picture window: %ux%u from (%u,%u) of the picture, at display pixel %d, "
		       "line %d\n", w.src_w, w.src_h, w.src_x, w.src_y, w.disp_x, w.disp_y);
	}
	uint16_t r[7];
	fmv_disp_regs(d, c, have_picture && decoder, r);
	r[0] |= fmv_disp_buf(cur_buf);
	if (!decoder)
		r[0] &= (uint16_t)~FMV_DISP_ON;   // nothing to show: leave the Amiga's picture alone
	for (int i = 0; i < 7; i++) {
		if (disp_valid && r[i] == disp_sent[i])
			continue;
		fmv_update u = { (uint8_t)(FMV_IDX_DISP_CTRL + i), false, false, r[i], 0xFFFF };
		fmv_xport_update(xport, u);
		if (i == 0 && (!disp_valid || ((r[0] ^ disp_sent[0]) & (FMV_DISP_ON | FMV_DISP_BLANK))))
			printf("FMV picture: %s%s\n", (r[0] & FMV_DISP_ON) ? "on" : "off",
			       (r[0] & FMV_DISP_BLANK) ? ", blanked (border colour)" : "");
		disp_sent[i] = r[i];
	}
	disp_valid = true;
}

static void picture_start()
{
	cur_buf = 0;
	have_picture = false;
	disp_valid = false;
	memset(&win_sent, 0xFF, sizeof win_sent);
	fbmap = (volatile uint8_t *)shmem_map(FMV_FB_PHYS, FMV_FB_BUFS * FMV_FB_BUF_BYTES);
	if (!fbmap) {
		printf("FMV: cannot map the frame buffers at 0x%08x; no picture\n", FMV_FB_PHYS);
	} else {
		decoder = fmv_decoder_new(fbmap, 0);     // core 0: Main runs on core 1
		if (!decoder)
			printf("FMV: the decoder thread did not start; no picture\n");
	}
	fmv_model_set_video_hooks(fmv_xport_model(xport), 0, vid_bytes, vid_flush, vid_picture);
	disp_update();
	audio = fmv_audio_new(0);           // core 0, beside the video decoder
	if (!audio)
		printf("FMV: the audio thread did not start; no sound\n");
	audio_full = audio_cdda = 0;
	cdda_base = akiko_cd32_cdda_counters(&cdda_under_last, &cdda_starve_last);
	cdda_under_total = cdda_starve_total = 0;
	cdda_read_us = 0;
	fmv_model_set_audio_hooks(fmv_xport_model(xport), aud_frame, aud_tick, aud_flush);
}

static void picture_stop()
{
	if (xport) {
		fmv_model_set_video_hooks(fmv_xport_model(xport), 0, 0, 0, 0);
		fmv_model_set_audio_hooks(fmv_xport_model(xport), 0, 0, 0);
	}
	fmv_audio_free(audio);
	audio = 0;
	fmv_decoder_free(decoder);
	decoder = 0;
	if (fbmap) {
		shmem_unmap((void *)fbmap, FMV_FB_BUFS * FMV_FB_BUF_BYTES);
		fbmap = 0;
	}
}
static uint64_t last_us;
static uint32_t win_polls;           // fmv_poll passes in this 10 s window
static uint64_t win_gap_max;         // the longest time between two of them, us
static uint64_t win_busy_us;         // time spent inside them, us
static int tried;           // one attempt per core load: a missing ROM is not retried every poll
static int diag_shown;      // diag snapshots printed this session
static uint64_t diag_last_us;

static uint64_t now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static void fmv_log(void *, const char *line)
{
	printf("%s\n", line);
}

// The ROM, by the names it goes by: WinUAE's "CD32FMV", and the plain one.
// Looked for beside the Kickstart this config actually loaded (CD32.rom may
// live in games/AmigaCD32 while HomeDir() is games/Amiga, as on the owner's
// card, 2026-11-02), then in HomeDir(), then in the root folder.
// Where the ROM is, by the names it goes by: WinUAE's "CD32FMV", and the
// plain one. Looked for beside the Kickstart this config actually loaded
// (CD32.rom may live in games/AmigaCD32 while HomeDir() is games/Amiga, as
// on the owner's card, 2026-11-02), then in HomeDir(), then in the root
// folder. 0 if none; the path in out.
static int find_rom(char *out, size_t outsz, int quiet)
{
	static const char *names[] = { "CD32FMV.rom", "fmv.rom" };
	char dirs[3][1024];
	int nd = 0;
	const char *ks = minimig_config.kickstart;
	const char *slash = strrchr(ks, '/');
	if (slash && slash > ks) {
		snprintf(dirs[nd], sizeof dirs[nd], "%.*s", (int)(slash - ks), ks);
		nd++;
	}
	snprintf(dirs[nd++], sizeof dirs[0], "%s", HomeDir());
	dirs[nd++][0] = 0;
	for (int d = 0; d < nd; d++) {
		for (const char *n : names) {
			if (dirs[d][0])
				snprintf(out, outsz, "%s/%s", dirs[d], n);
			else
				snprintf(out, outsz, "%s", n);
			if (FileLoad(out, 0, 0) > 0)
				return 1;
		}
	}
	if (!quiet)
		printf("FMV: looked for CD32FMV.rom / fmv.rom in %s%s%s and the root folder\n",
		       nd > 2 ? dirs[0] : "", nd > 2 ? ", " : "", HomeDir());
	return 0;
}

static uint8_t *load_rom(int *len)
{
	char path[1100];
	if (!find_rom(path, sizeof path, 0))
		return 0;
	int size = FileLoad(path, 0, 0);
	uint8_t *buf = (uint8_t *)malloc(size);
	if (!buf)
		return 0;
	if (FileLoad(path, buf, size) != size) {
		free(buf);
		return 0;
	}
	printf("FMV ROM file: %s\n", path);
	*len = size;
	return buf;
}

int fmv_rom_found(void)
{
	// The OSD asks on every redraw; the search is a few stat()s, so keep the
	// answer for a second, and against the Kickstart path it was made for.
	static char ks[sizeof minimig_config.kickstart];
	static uint64_t at;
	static int found;
	uint64_t t = now_us();
	if (at && t - at < 1000000 && !strcmp(ks, minimig_config.kickstart))
		return found;
	char path[1100];
	found = find_rom(path, sizeof path, 1);
	snprintf(ks, sizeof ks, "%s", minimig_config.kickstart);
	at = t;
	return found;
}

int fmv_blocked_by_armcpu(void)
{
	return armcpu_wanted();
}

int fmv_playing(void)
{
	// Save states leave the board out: the CL450/L64111 model and both
	// decoders live on the ARM, and nothing of them is in the state. That is
	// safe only while the board is idle, so a save or restore is refused
	// while it is not: the CL450 out of IDLE (decoding or showing a
	// picture), the L64111 started with frames in its channel buffer, or the
	// cartridge's picture on. Cannon Fodder's driver ends its intro with
	// Reset(), FlushBitstream() and the picture off (runs v21-v24), so after
	// the intro this reads 0 again.
	if (!xport)
		return 0;
	fmv_model *m = fmv_xport_model(xport);
	fmv_cl_display d;
	fmv_model_display(m, &d);
	fmv_model_state st;
	fmv_model_get_state(m, &st);
	return !d.idle || (st.l64_started && st.l64_cb_frames) ||
	       (disp_valid && (disp_sent[0] & FMV_DISP_ON));
}

int fmv_wanted(void)
{
	// The OSD row, or AMIGACD_FMV=1 in MiSTer.ini, which the test harness
	// uses. A CD32 only: the cartridge is the CD32's.
	return (fmv_module_on() || cfg.amigacd_fmv == 1) &&
	       amigacd_system() == AMIGACD_SYS_CD32 && !fmv_blocked_by_armcpu();
}

static void diag_print();

static void start()
{
	tried = 1;
	int len = 0;
	uint8_t *rom = load_rom(&len);
	if (!rom) {
		printf("FMV: the module is on but no CD32FMV.rom or fmv.rom was found; the board stays off\n");
		return;
	}
	// status[64] stays clear until the session is set up: while it is, the
	// core holds its ring and queue counters at 0 and leaves DDR3 alone
	// (fmv-contract.md v4), so both sides start from zero. A saved config
	// may have brought the bit up with the core; take it down and give the
	// core time to see it.
	user_io_status_set(FMV_STATUS_OPT, 0);
	usleep(2000);
	window = (volatile uint8_t *)shmem_map(FMV_WIN_PHYS, FMV_WIN_SIZE);
	if (!window) {
		printf("FMV: cannot map the DDR3 window at 0x%08x\n", FMV_WIN_PHYS);
		free(rom);
		return;
	}
	xport = fmv_xport_new(window, fmv_log, 0);
	fmv_xport_set_cmd_at_event(xport, !cfg.amigacd_fmv_slowcmd);
	printf("FMV: CL450 commands accepted %s\n", cfg.amigacd_fmv_slowcmd
	       ? "at the next model advance (AMIGACD_FMV_SLOWCMD=1, the old path)"
	       : "at their HOST_newcmd event");
	int ok = fmv_xport_start(xport, rom, (size_t)len, crc32_compute(rom, len));
	free(rom);
	if (!ok) {
		fmv_xport_free(xport);
		xport = 0;
		shmem_unmap((void *)window, FMV_WIN_SIZE);
		window = 0;
		return;
	}
	picture_start();
	user_io_status_set(FMV_STATUS_OPT, 1);
	last_us = now_us();
	diag_print();              // the core's side as the session starts
	printf("FMV: board on. It is configured at the next Amiga reset; Zorro II fast RAM "
	       "is masked out while it is on (fmv-contract.md)\n");
}

// The core's FMV diag window over SPI (hps_ext 0xFA00). It does not go
// through DDR3, so it still answers when the DDR3 path is what failed.
static void diag_print()
{
	uint16_t w[44];
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(0xFA00);
	for (int i = 0; i < 44; i++)
		w[i] = (uint16_t)spi_w(0);
	DisableIO();
	char line[2400];
	static int off_said;
	if (!fmv_diag_format(w, 44, line, sizeof line)) {
		// No window (an older core) or one built without its taps: say so
		// once, not every ten seconds
		if (!off_said) {
			off_said = 1;
			printf("%s\n", line);
		}
		return;
	}
	uint32_t r[5];
	fpga_sdr_status(r);
	printf("%s; HPS: dramsts %08x fpgaportrst %08x protportdefault %08x brgmodrst %08x "
	       "fpgaintf %08x\n", line, r[0], r[1], r[2], r[3], r[4]);
	static int dead_said;
	if (fmv_diag_port_dead(w, 44) && !dead_said) {
		dead_said = 1;
		printf("FMV: *** the core's DDR3 port is not answering reads (the HPS takes them and "
		       "returns no data). The FMV board cannot work like this. Power-cycle the MiSTer: "
		       "a core reload or a port reset did not clear it on 2026-11-02 ***\n");
	}
}

void fmv_shutdown(void)
{
	picture_stop();
	if (xport) {
		fmv_xport_stop(xport);
		fmv_xport_free(xport);
		xport = 0;
	}
	if (window) {
		shmem_unmap((void *)window, FMV_WIN_SIZE);
		window = 0;
	}
	tried = 0;
	diag_shown = 0;
}

// Diagnosis-only modes. The board stays off -- status[64] clear, no window
// mapped, no ROM -- and the core's FMV/DDR3 diag window is printed once a
// second for the first ten seconds after the core starts, then every ten
// seconds. They ask about the core's DDR3 port with the FMV side idle (gate
// 1, 2026-11-02 night: the HPS f2sdram port wedged after a handful of A2065
// commands, before FMV ever ran).
//   2  just watch.
//   3  at 3 s, pulse the f2sdram port reset (fpga_sdr_port_pulse) with the
//      core running. Run 2026-11-02 ~22:35: it clears DDRAM_BUSY, but the
//      core's arbiters still wait for the lost beat, so no traffic follows.
//   4  at 3 s, hold the core reset, pulse the port reset, release the core
//      reset. Both sides start again with the PLL long locked. If A2065
//      traffic then keeps flowing, the port was in a bad state from the
//      bridge enable at load (do_bridge(1) runs as soon as the bitstream is
//      in, perhaps before clk_114 is stable); if it wedges again after a
//      few grants, something recurring kills it.
static void diag_only_poll()
{
	static uint64_t first_us;
	static int pulsed;
	uint64_t t = now_us();
	if (!first_us) {
		first_us = t;
		user_io_status_set(FMV_STATUS_OPT, 0);
		printf("FMV: AMIGACD_FMV=%d, diagnosis only: the board stays off%s\n", cfg.amigacd_fmv,
		       cfg.amigacd_fmv == 3 ? "; port reset pulse at 3 s" :
		       cfg.amigacd_fmv == 4 ? "; core reset + port reset pulse at 3 s" : "");
		// The protection rules, once (only valid ones, data bit 2)
		uint32_t r[60];
		fpga_sdr_rules(r, 20);
		int shown = 0;
		for (int i = 0; i < 20; i++) {
			if (!(r[3 * i + 2] & 4))
				continue;
			printf("FMV diag-only: SDR protection rule %d: addr %08x id %08x data %08x\n", i,
			       r[3 * i], r[3 * i + 1], r[3 * i + 2]);
			shown++;
		}
		if (!shown)
			printf("FMV diag-only: no valid SDR protection rule (all 20 have data bit 2 clear)\n");
	}
	if (cfg.amigacd_fmv >= 3 && !pulsed && t - first_us >= 3000000) {
		pulsed = 1;
		if (cfg.amigacd_fmv == 3) {
			printf("FMV diag-only: pulsing the f2sdram port reset (fpgaportrst 0, then 0x3FFF)\n");
			fpga_sdr_port_pulse();
		} else {
			printf("FMV diag-only: core reset on, f2sdram port reset pulse, core reset off\n");
			fpga_core_reset_with_port_pulse();
		}
		diag_print();
	}
	uint64_t period = (t - first_us < 10000000) ? 1000000 : 10000000;
	if (diag_shown == 0 || t - diag_last_us >= period) {
		diag_last_us = t;
		diag_shown++;
		printf("FMV diag-only, %llu ms after start: ", (unsigned long long)((t - first_us) / 1000));
		diag_print();
	}
}

// Once per activity line: the stream's rate against the wall clock, and
// what Main's side looked like meanwhile. The model only schedules what has
// arrived, so its late counts cannot show a stream fed too slowly; this can.
// Run v26 found the intro at 2/3 speed with every other counter clean.
static void rate_line(uint64_t t)
{
	static uint64_t t0;
	static uint32_t shown0, frames0, events0, near0, cmds0, lo0, hi0;
	fmv_decoder_stats ds = {};
	if (decoder) fmv_decoder_get_stats(decoder, &ds);
	fmv_audio_stats as = {};
	if (audio) fmv_audio_get_stats(audio, &as);
	fmv_xport_stats xs;
	fmv_xport_get_stats(xport, &xs);
	fmv_model_state ms;
	fmv_model_get_state(fmv_xport_model(xport), &ms);
	if (t0 && t > t0) {
		double sec = (double)(t - t0) / 1e6;
		double pic = (ds.presented - shown0) / sec, aud = (as.ticks - frames0) / sec;
		double pic_want = ms.cl_frame_period_us ? 1e6 / ms.cl_frame_period_us : 0;
		double aud_want = ms.l64_sample_rate ? ms.l64_sample_rate / 1152.0 : 0;
		printf("FMV rate: %.1f pictures/s (the sequence: %.1f), %.1f audio frames/s (the stream: %.1f); "
		       "Main: %u polls, longest gap %llu ms, %.1f %% busy, %.0f events/s, "
		       "%u drains near full, ring backlog max %u of %u\n",
		       pic, pic_want, aud, aud_want, win_polls, (unsigned long long)(win_gap_max / 1000),
		       100.0 * (double)win_busy_us / ((double)(t - t0)), (xs.events - events0) / sec,
		       xs.near_full - near0, xs.backlog_max, (unsigned)FMV_EV_ENTRIES);
		// The 68k's wait on HOST_newcmd (run v30's suspect): per command,
		// and as a share of the second, between the two bounds
		// fmv_transport.cpp note_commands gives.
		uint32_t nc = xs.cmd_waits - cmds0;
		double lo = xs.cmd_wait_lo_us - lo0, hi = xs.cmd_wait_hi_us - hi0;
		printf("FMV rate: CL450 commands %.1f/s (%s); HOST_newcmd wait %.0f-%.0f us a command, "
		       "longest at least %u us; %.1f-%.1f %% of the second\n",
		       nc / sec, cfg.amigacd_fmv_slowcmd ? "old path" : "at the event",
		       nc ? lo / nc : 0.0, nc ? hi / nc : 0.0, xs.cmd_wait_max_us,
		       100.0 * lo / (sec * 1e6), 100.0 * hi / (sec * 1e6));
	}
	t0 = t;
	shown0 = ds.presented;
	frames0 = as.ticks;
	events0 = xs.events;
	near0 = xs.near_full;
	cmds0 = xs.cmd_waits;
	lo0 = xs.cmd_wait_lo_us;
	hi0 = xs.cmd_wait_hi_us;
	fmv_xport_clear_max(xport);
	win_polls = 0;
	win_gap_max = 0;
	win_busy_us = 0;
}

static void pc_trace(uint64_t t)
{
	// Debug build only: the 68k's PC ring from the diag window, about 300 times
	// a second, to /tmp/pc.log, to see what runs just before the self-reset of
	// run v33. Needs an FMV_DIAG core.
	static FILE *f;
	static uint64_t last, flushed;
	if (t - last < 3000)
		return;
	last = t;
	if (!f) {
		f = fopen("/tmp/pc.log", "w");
		if (!f)
			return;
	}
	uint16_t w[44];
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(0xFA00);
	for (int i = 0; i < 44; i++)
		w[i] = (uint16_t)spi_w(0);
	DisableIO();
	char line[2400];
	if (!fmv_diag_format(w, 44, line, sizeof line))
		return;
	const char *a = strstr(line, "Amiga: INTENA");
	if (a)
		fprintf(f, "%llu %s
", (unsigned long long)(t / 1000), a + 7);
	if (t - flushed > 200000) {
		flushed = t;
		fflush(f);
	}
}

void fmv_poll(void)
{
	if (is_minimig() && cfg.amigacd_fmv >= 2) {
		diag_only_poll();
		return;
	}
	if (!is_minimig() || !fmv_wanted()) {
		static int said_arm;
		if (is_minimig() && (fmv_module_on() || cfg.amigacd_fmv == 1) &&
		    fmv_blocked_by_armcpu() && !said_arm) {
			said_arm = 1;
			printf("FMV: the module is on, but so is the ARM CPU, and both use ARM core 0; "
			       "the board stays off\n");
		}
		if (xport)
			fmv_shutdown();
		tried = 0;          // switched on again (the ROM added, say): try again
		return;
	}
	if (!xport) {
		if (!tried)
			start();
		if (!xport)
			return;
	}
	uint64_t t = now_us();
	uint64_t dt = t - last_us;
	last_us = t;
	// How often Main gets here, and how long a pass takes. The 68k waits on
	// Main whenever the event ring is full (fmv_xport_stats.near_full), so
	// these say whether Main is what paces a stream (run v26).
	win_polls++;
	pc_trace(t);
	if (dt > win_gap_max)
		win_gap_max = dt;
	// A long stall (an OSD file browser, a blocking load) is not replayed as
	// one jump: the chips' schedules would fire a burst of interrupts.
	if (dt > 100000)
		dt = 100000;
	fmv_xport_poll(xport, (uint32_t)dt);
	if (decoder)
		fmv_decoder_poll(decoder);
	disp_update();
	audio_poll();
	cdda_accumulate(t);
	win_busy_us += now_us() - t;

	// A stalled queue: three snapshots of the core's side, a second apart,
	// so the log says which counters move and which state it sits in.
	if (fmv_xport_stalled(xport) && diag_shown < 3 && t - diag_last_us >= 1000000) {
		diag_last_us = t;
		diag_shown++;
		diag_print();
	}

	// While the board is on: every 10 s, the core's side and what the 68k
	// has done to the board, including the polls the log leaves out. Gate 1
	// (2026-11-03) went quiet after SetThreshold, and a sleeping 68k, a
	// polling one and one held on a board access look alike in the log.
	static uint64_t act_last_us;
	if (!fmv_xport_stalled(xport) && t - act_last_us >= 10000000) {
		act_last_us = t;
		char a[768];
		fmv_model_activity(fmv_xport_model(xport), a, sizeof a);
		printf("%s\n", a);
		rate_line(t);
		if (decoder) {
			fmv_decoder_stats ds;
			fmv_decoder_get_stats(decoder, &ds);
			printf("FMV decoder: %u bytes in, %u sequences (%ux%u), %u pictures, %u shown, "
			       "%u late (ticks with none new), %u bytes dropped, %u flushes; buffer %d\n",
			       ds.bytes_in, ds.sequences, ds.width, ds.height, ds.pictures, ds.presented,
			       ds.late, ds.dropped_bytes, ds.flushes, ds.shown);
		}
		if (audio) {
			fmv_audio_stats as;
			fmv_audio_get_stats(audio, &as);
			// The FIFO's own underrun and starve counts since the session
			// began are the phase 4 gate's measure (both 0). Polls blocked
			// "FIFO full" are back-pressure, Main offering a sector before
			// the FIFO has room, and say little on their own.
			char fifo[80] = "FIFO counters n/a";
			if (cdda_base)
				snprintf(fifo, sizeof fifo, "FIFO underruns %u, starves %u (session totals)",
				         cdda_under_total, cdda_starve_total);
			printf("FMV audio: %u frames in, %u decoded, %u bad, %u played (%u missing, %u late, "
			       "%u muted, %u not 44.1 kHz), %u sectors to the CDDA FIFO, %s, polls blocked "
			       "%u FIFO full / %u CD audio, %u samples dropped, %u flushes\n",
			       as.frames_in, as.decoded, as.bad, as.ticks, as.missing, as.late, as.muted,
			       as.wrong_rate, as.sectors, fifo, audio_full, audio_cdda, as.dropped, as.flushes);
		}
		diag_print();
	}
}
