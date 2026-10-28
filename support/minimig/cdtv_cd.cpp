// CDTV native-mode bridge — Main_MiSTer side (M2 phase-1c).
//
// Mirrors WinUAE's CR-511 command_thread (cdtv.cpp:524-695) just far enough to
// unblock the CDTV Welcome splash and let a title boot. Talks to the FPGA
// bridge in rtl/cdtv_hps_bridge.v via UIO class 0xF800 (hps_ext.v:183,
// io_din[15:9] == 7'b1111_100). Polled alongside akiko_cd32_poll from
// user_io.cpp.
//
// Direction model (per cdtv_hps_bridge.v):
//   UIO read  (0x62 + 0xF800): each spi_w(0) pops one byte from cmd_in_fifo
//                              (CPU writes to $E900A1 → bridge enqueued).
//   UIO write (0x61 + 0xF800): each spi_w(byte) pushes into cmd_out_fifo and
//                              pulses STEN so the BIOS interrupt handler
//                              latches the reply byte at $E900A1.
//
// Gating: every entry point checks `minimig_config.chipset & CONFIG_CDTV` and
// returns immediately for non-CDTV cores (Akiko/AGA/CD32 paths are untouched).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include <byteswap.h>     // bswap_16 for CDDA big-endian → host conversion

#include "../../spi.h"
#include "../../fpga_io.h"
#include "../../user_io.h"
#include "../../menu.h"
#include "../../hardware.h"
#include "../../ide.h"
#include "../../ide_cdrom.h"
#include "../chd/mister_chd.h"    // mister_chd_read_sector for CDDA pump
#include "../physical_disc/physical_disc.h" // physical CD-ROM drive media
#include "cdtv_cd.h"
#include "minimig_config.h"

// -----------------------------------------------------------------------------
// Debug
// -----------------------------------------------------------------------------
static void cdtv_diag(const char *fmt, ...)
{
	FILE *f = fopen("/tmp/cdtv_dbg.log", "a");
	if (!f) return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(f, "[%6lu.%06lu] ", (unsigned long)ts.tv_sec, (unsigned long)(ts.tv_nsec / 1000));
	va_list ap; va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
	fclose(f);
}

#define CDTV_DEBUG 1
#if CDTV_DEBUG
	#define cdtv_dbg(fmt, ...) cdtv_diag("[cdtv] " fmt, ##__VA_ARGS__)
#else
	#define cdtv_dbg(...) do {} while (0)
#endif

// -----------------------------------------------------------------------------
// Bridge constants
// -----------------------------------------------------------------------------
#define CDTV_BRIDGE_ADDR   0xF800
// Phase-1b sector-push sub-channel: io_din[5]=1 inside the CDTV class
// (hps_ext.v cdtv_cs_sec). Each spi_w pushes one byte into the bridge's
// 8 KB sector staging FIFO; the bridge's drain FSM master-writes them to
// chip RAM at `acr` via chipdma_arb.
// The same sub-channel is READABLE (UIO 0x62): each read returns the FIFO's
// free space in 32-byte units, rounded down — 255 = empty, 0 = full
// (cdtv_bridge.v sec_space). This is the CDTV analogue of the CD32 path's
// sec_req credit: the push direction has no handshake, so without reading the
// credit first we can overrun the FIFO and cdtv_bridge's overflow guard drops
// the excess bytes SILENTLY. That matters because a save state freezes
// chipdma_arb's bridge grants for the whole ~0.2 s chip RAM dump, during which
// the FIFO cannot drain a single byte.
#define CDTV_SEC_ADDR      0xF820
#define CDTV_SEC_SPACE_UNIT 32       // cdtv_bridge.v: sec_space is sec_free[12:5]
// Upper bound on how long we will wait for room in the sector FIFO before
// giving up and failing the READ. The only thing that legitimately blocks the
// drain for a long stretch is a save state dump (~0.2 s), so 2 s is an order
// of magnitude of headroom — and being bounded is what guarantees the MiSTer
// poll loop can never wedge here no matter what the core is doing.
#define CDTV_SEC_WAIT_MS   2000
// Phase-1e STCH-inject sub-channel: io_din[6]=1 inside the CDTV class
// (hps_ext.v cdtv_cs_stch). Any byte written here pulses cdtv_bridge.stch.
#define CDTV_STCH_ADDR     0xF840
// CDDA audio FIFO sub-channel — shared with akiko_cd32.cpp Phase 33.
// hps_ext.v:191 selects cdda_cs on io_din[15:9] == 7'b1111001 → 0xF200.
// Each spi_w pushes a 16-bit sample (high byte first for L/R alternation).
// FIFO depth backpressure shows in the status word as cdda_req (bit 8).
#define CDTV_CDDA_ADDR     0xF200
#define CDTV_CDDA_BYTES    2352      // raw red-book audio frame
#define CDTV_STATUS_CDDA_REQ (1u << 8) // hps_ext.v:218, cdda_req
#define CDTV_STATUS_CMD    0x63
#define CDTV_STATUS_REQ    (1u << 6)   // hps_ext.v:197 — cdtv_req
#define CDTV_STATUS_REQ    (1u << 6)
#define CDTV_STATUS_NVR_DIRTY (1u << 12)

#define CDTV_NVRAM_ADDR       0xF880
#define CDTV_NVRAM_BYTES      16384
#define CDTV_NVRAM_FILE_BYTES 32768
#define CDTV_NVRAM_DIR        "/media/fat/saves/Minimig"

#define CDTV_CARD_ADDR        0xF8C0
#define CDTV_CARD_BYTES       8192
#define CDTV_STATUS_CARD_DIRTY (1u << 13)

// CR-511 command lengths (input bytes). Indexed by full opcode byte. -1
// means unknown / never accept. Lifted from cdtv.cpp:524-695 by opcode:
//   2: 0x00, 0x80
//   1: 0x81, 0x85, 0x86, 0x88, 0xA2
//   7: all the rest that we accept
// We use a sparse lookup rather than a 256-entry table.
static int cr511_command_length(uint8_t op)
{
	switch (op) {
		case 0x00: case 0x80:                      return 2;
		case 0x81: case 0x85: case 0x86:
		case 0x88: case 0xA2:                      return 1;
		case 0x01: case 0x02: case 0x04: case 0x05:
		case 0x09: case 0x0a: case 0x0b: case 0x82:
		case 0x83: case 0x84: case 0x87: case 0x89:
		case 0x8a: case 0x8b: case 0xa3:           return 7;
		default:                                   return -1;
	}
}

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------
#define CDTV_CMD_MAX  16
static uint8_t  cmd_buf[CDTV_CMD_MAX];
static int      cmd_idx = 0;       // bytes accumulated so far
static int      cmd_need = 0;      // total bytes expected once opcode known

// Static drive/media state (mirrors WinUAE cdtv.cpp:87).
static uint8_t  cd_motor       = 0;
static uint8_t  cd_isready     = 0;    // 1 = "tray just opened / not ready"
static uint8_t  cd_media       = 0;    // 1 = disc present
static uint8_t  cd_playing     = 0;
static uint8_t  cd_finished    = 0;
static uint8_t  cd_error       = 0;
static uint16_t cd_sectorsize  = 2048;

// CD path tracking (from ide_cdrom mount/unmount).
static char     cd_path_active[1024] = {0};

// CDDA streaming pump state (mirrors akiko_cd32.cpp Phase 33). lba_next
// is -1 when idle; otherwise it's the next absolute LBA to read and push
// to UIO 0xF200. lba_end is the exclusive end. cd_paused freezes the pump
// without tearing the working set down.
static int32_t  cdtv_play_lba_next = -1;
static int32_t  cdtv_play_lba_end  = -1;
static drive_t *cdtv_play_drv      = NULL;
static uint8_t  cd_paused          = 0;

// Audio status reported in SUBQ byte 0 (WinUAE cdrom.h AUDIO_STATUS_*):
//   0x11 IN_PROGRESS    — pump actively streaming
//   0x12 PAUSED         — pump frozen via 0x8b
//   0x13 PLAY_COMPLETE  — natural end reached, holds last position
//   0x14 PLAY_ERROR     — read failure / abort
//   0x15 NO_STATUS      — idle (never played / stopped)
// DotC's title-music loop polls 0x87 SUBQ to detect PLAY_COMPLETE and
// re-arm the cue. With the old stub frozen at IN_PROGRESS+00:00:00 the
// loop never closed and the title screen stalled.
static uint8_t  cd_audio_status    = 0x15;

// Last LBA we pumped, preserved across natural-end so SUBQ keeps reporting
// the right position after the pump tears down.
static int32_t  cdtv_last_lba      = 0;

// Phase-1e: STCH inject retry budget. The cdtv_bridge ilatch is wiped while
// CPU is held in reset (minimig.v:474 `reset = sys_reset | ~_cpu_reset_in`),
// so a single STCH pulse fired during BootInit gets dropped. Worse, BIOS
// init may also clear ilatch[2] via TPI register-2 write before unmasking,
// losing any pulse that landed pre-init. Mirror WinUAE's hsync retry pattern
// (cdtv.cpp:1288-1302 — do_stch only fires once `tp_cr & 1` is set, polled
// every hsync) by re-firing STCH every poll-period until we see BIOS issue
// something other than STATUS (0x81), which proves BIOS picked up the STCH.
static int            stch_retries  = 0;
static unsigned long  stch_next_ms  = 0;
#define STCH_RETRY_BUDGET    40       // 40 × 250 ms = 10 s of cover
#define STCH_RETRY_PERIOD_MS 250

// Play-end STCH ack tracking -- WinUAE do_stch parity (cdtv.cpp:1292). The
// status-change interrupt fired at CDDA end-of-play must be retried until the
// BIOS INT2 handler actually TAKES it (reads the TPI AIR register and sees the
// STCH source code 0x04), then stopped. A single pulse races with the constant
// scor/sten interrupt churn and is often lost; a blind heartbeat over-injects
// and re-fires INT2 into the next screen. Retry-until-acked fixes both. The
// ack comes from cdtv_bridge.v's sticky flag, read via cdtv_stch_acked().
static bool           stch_wait_ack = false;   // armed at CDDA play-end
#define STCH_PLAYEND_BUDGET    400             // backstop cap (x period) if never acked
#define STCH_PLAYEND_PERIOD_MS 60              // fast cadence to win the int-churn race

// Upstream's play-START STCH, which is what Defender of the Crown waits on
// before it will leave the park screen. Independent of the play-end retry
// above: one is armed when a play begins, the other when it ends.
static bool           stch_playstart_pending = false;

// A disc swapped straight into a loaded drive owes the guest a removal edge
// before the arrival, and the two cannot land in the same instant.
static bool           cd_swap_pending   = false;
static unsigned long  cd_swap_ready_ms  = 0;
#define CDTV_SWAP_SETTLE_MS 700

// -----------------------------------------------------------------------------
// Helpers -- gate on CDTV mode
// -----------------------------------------------------------------------------
// True for either way of configuring a CDTV: upstream's dedicated drive slot,
// or the chipset bit our own profiles set. Upstream only knows about the
// first, and taking that alone would leave the bridge switched off on every
// AmigaCD CDTV profile.
static inline bool cdtv_active(void)
{
	return (minimig_config.cdtv_drive.cfg != 0)
	    || ((minimig_config.chipset & CONFIG_CDTV) != 0);
}

// The dedicated CDTV slot first, then the IDE drives -- a physical disc and
// anything mounted through hardfile[0] still live there.
static drive_t *cdtv_find_drive(void)
{
	drive_t *slot = minimig_cd_drive_get(1);
	if (slot) return slot;
	for (int p = 0; p < 2; p++) {
		for (int d = 0; d < 2; d++) {
			drive_t *drv = &ide_inst[p].drive[d];
			if (drv->cd && (drv->chd_f || drv->f || drv->phys)) {
				return drv;
			}
		}
	}
	return NULL;
}

// -----------------------------------------------------------------------------
// Bridge transactions
// -----------------------------------------------------------------------------
static uint16_t cdtv_read_status(void)
{
	uint16_t res;
	EnableIO();
	res = spi_w(CDTV_STATUS_CMD);
	if (!res) res = spi_w(0);
	DisableIO();
	return res;
}

// Drain one CR-511 byte from the bridge. cmd_in_pending must be checked
// before calling — we just issue an unconditional read.
static uint8_t cdtv_drain_byte(void)
{
	uint8_t b;
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(CDTV_BRIDGE_ADDR);
	b = (uint8_t)spi_w(0);
	DisableIO();
	return b;
}

// Fire a one-shot STCH pulse via the dedicated UIO sub-channel. The payload
// byte is discarded by the bridge — we just need the uio_wr strobe.
static void cdtv_inject_stch(void)
{
	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(CDTV_STCH_ADDR);
	spi_w(0);
	DisableIO();
	cdtv_dbg("STCH inject");
}

// Poll the STCH ack flag on the STCH sub-channel. cdtv_bridge.v sets it when
// the BIOS INT2 handler reads the TPI AIR register and the active source is
// STCH -- i.e. the status change was delivered AND taken -- and the read
// clears it. Replaces scanning the cdtv_trace ring for the same AIR read,
// which the RTL no longer carries.
static bool cdtv_stch_acked(void)
{
	uint16_t v;
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(CDTV_STCH_ADDR);
	v = (uint16_t)spi_w(0);
	DisableIO();
	return (v & 1) != 0;
}

// Push a reply payload (already including any STCH-trigger byte sequencing the
// caller wants). Each byte pulses STEN inside the bridge (cdtv_bridge.v:637).
static void cdtv_push_reply(const uint8_t *buf, int len)
{
	if (len <= 0) return;
	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(CDTV_BRIDGE_ADDR);
	for (int i = 0; i < len; i++) {
		spi_w(buf[i]);
	}
	DisableIO();

#if CDTV_DEBUG
	char dump[3 * CDTV_CMD_MAX + 4];
	int n = 0;
	int dump_n = (len > CDTV_CMD_MAX) ? CDTV_CMD_MAX : len;
	for (int i = 0; i < dump_n; i++) n += snprintf(dump + n, sizeof(dump) - n, "%02x ", buf[i]);
	cdtv_dbg("TX[%d]: %s%s", len, dump, (len > dump_n) ? "..." : "");
#endif
}

// Read the bridge's sector-FIFO credit and convert it to bytes. The bridge
// reports whole 32-byte units and rounds DOWN, so this never over-promises.
// One read strobe is enough — sec_space is combinational status, exactly like
// akiko_read_sec_counter's 1-byte read on the CD32 sec sub-channel.
static int cdtv_sec_space_bytes(void)
{
	uint16_t v;
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(CDTV_SEC_ADDR);
	v = (uint16_t)spi_w(0);
	DisableIO();
	return (int)(v & 0xff) * CDTV_SEC_SPACE_UNIT;
}

// Stream `len` bytes into the CDTV bridge's sector staging FIFO. The bridge's
// drain FSM (cdtv_bridge.v:4b) pops bytes one-at-a-time and master-writes them
// to chip RAM at acr via chipdma_arb's cdtv port.
//
// Credit-gated: we read the FIFO's free space first and refuse to start the
// burst until the whole sector fits. cdtv_bridge's push path has no
// backpressure — when the FIFO is full it drops bytes without telling anyone —
// so this read is the only thing standing between a stalled drain and silent
// CD data corruption. The stall that motivated it is a save state: the freeze
// ties chipdma_arb's dma_hold high for the entire ~0.2 s chip RAM dump, so the
// drain FSM gets no slots at all and 8 KB of FIFO absorbs only ~53 ms of a 1x
// stream.
//
// Cannot deadlock. The wait is bounded (CDTV_SEC_WAIT_MS) and, more
// fundamentally, nothing the core needs in order to drain the FIFO comes from
// this thread: the drain is FPGA-side (bridge DMAC + INT2 re-arm by the CDTV
// BIOS), and the save state machine is entirely in RTL. Blocking here stalls
// only our own producer, which is the point.
//
// Returns false if the FIFO never made room; the caller fails the READ rather
// than pushing bytes it knows will be thrown away.
static bool cdtv_push_sector(const uint8_t *buf, int len)
{
	if (len <= 0) return true;

	int waited_ms = 0;
	int space = cdtv_sec_space_bytes();

	// Version-skew guard. The credit is produced by core RTL, and the core and
	// this binary live in separate repositories that are deployed
	// independently. Against a core built before the credit existed, hps_ext.v
	// never drives io_dout for this sub-channel and the read returns whatever
	// the previous transaction left behind -- zero when idle. Gating on that
	// would make every sector wait the full timeout and every CDTV data READ
	// fail, turning a working CDTV unusable with a symptom pointing nowhere
	// near this code.
	//
	// A zero credit on the very first read cannot be genuine: the caller only
	// pushes after a command has been accepted, and the FIFO is empty then. So
	// treat "never seen a nonzero credit" as "this core has no credit channel"
	// and stream unthrottled, exactly as before the credit existed. One sector
	// of an 8 KB FIFO is 2352 bytes, so a real FIFO always reports room here.
	static bool credit_channel_seen = false;
	static bool credit_absent_logged = false;
	bool gate = true;
	if (space > 0) credit_channel_seen = true;
	else if (!credit_channel_seen) {
		if (!credit_absent_logged) {
			cdtv_dbg("no sector-FIFO credit from the core -- assuming a core "
			         "without the channel and streaming unthrottled");
			credit_absent_logged = true;
		}
		gate = false;
	}

	while (gate && space < len) {
		if (waited_ms >= CDTV_SEC_WAIT_MS) {
			cdtv_dbg("sector FIFO has no room for %d bytes after %d ms — failing READ",
			         len, waited_ms);
			return false;
		}
		usleep(1000);
		waited_ms++;
		space = cdtv_sec_space_bytes();
	}
	if (waited_ms) cdtv_dbg("sector FIFO throttle: waited %d ms for %d bytes", waited_ms, len);

	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(CDTV_SEC_ADDR);
	for (int i = 0; i < len; i++) {
		spi_w(buf[i]);
	}
	DisableIO();
	return true;
}

// -----------------------------------------------------------------------------
// CDDA streaming — same UIO 0xF200 channel as akiko_cd32.cpp Phase 33.
// rtl/cdda.v is instantiated unconditionally in AmigaCD.sv and mixed into
// the audio output, so we don't need a CDTV-specific FPGA hook.
// -----------------------------------------------------------------------------

// Read one 2352-byte raw audio sector from the CHD into buf. Returns true on
// success. Validates that lba falls inside an AUDIO track (attr & 0x40 == 0
// in the ide_cdrom convention, sectorSize == 2352). Mirrors the CD32 path's
// cd_read_audio_sector — re-implemented here rather than calling
// cdrom_read_raw_sector because that helper zero-fills audio tracks.
static bool cdtv_read_audio_sector(drive_t *drv, uint32_t lba, uint8_t *buf2352)
{
	if (!drv || !buf2352) return false;
	if (!drv->chd_f && !drv->phys && !drv->f) return false;

	track_t *track = NULL;
	int real_tracks = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
	for (int i = 0; i < real_tracks; i++) {
		uint32_t end = drv->track[i].start + drv->track[i].length;
		if (lba >= drv->track[i].start && lba < end) {
			track = &drv->track[i];
			break;
		}
	}
	if (!track) return false;
	if (track->attr & 0x40) return false;
	if (track->sectorSize != CDTV_CDDA_BYTES) return false;

	if (drv->phys) {
		if (physical_disc_read_sector((int)lba, buf2352, NULL)) return false;
		// See the CD32 path: disc order is little-endian, CHD order is
		// big-endian, and cdtv_push_audio_sector swaps unconditionally.
		uint16_t *s = (uint16_t *)buf2352;
		for (int i = 0; i < CDTV_CDDA_BYTES / 2; i++) s[i] = bswap_16(s[i]);
		return true;
	}

	if (drv->chd_f) {
		uint32_t chd_lba = lba + track->chd_offset;
		if (mister_chd_read_sector(drv->chd_f, chd_lba, 0, 0,
		                           CDTV_CDDA_BYTES, buf2352,
		                           drv->chd_hunkbuf, &drv->chd_hunknum)
		    != CHDERR_NONE) {
			return false;
		}
		return true;
	}

	// Raw cue+bin.
	if (!cdrom_read_track_raw(track, lba, buf2352, CDTV_CDDA_BYTES)) return false;

	int16_t *samples = (int16_t *)buf2352;
	for (int i = 0; i < CDTV_CDDA_BYTES / 2; i++) {
		samples[i] = (int16_t)bswap_16((uint16_t)samples[i]);
	}
	return true;
}

static bool cdtv_audio_fifo_ready(void)
{
	return (cdtv_read_status() & CDTV_STATUS_CDDA_REQ) != 0;
}

// 2352 bytes = 588 stereo frames × 2 channels × 2 bytes/sample.
// CHD stores audio big-endian per 16-bit sample; cdda.v wants
// LEFT first then RIGHT per stereo frame.
static void cdtv_push_audio_sector(const uint8_t *buf2352)
{
	const uint16_t *src = (const uint16_t *)buf2352;
	const int nframes = CDTV_CDDA_BYTES / 4;

	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(CDTV_CDDA_ADDR);
	for (int i = 0; i < nframes; i++) {
		uint16_t l = bswap_16(src[2 * i + 0]);
		uint16_t r = bswap_16(src[2 * i + 1]);
		spi_w(l);
		spi_w(r);
	}
	DisableIO();
}

// Push one sector if the FIFO has room. Returns true if it pushed.
static bool cdtv_cdda_pump(void)
{
	if (cdtv_play_lba_next < 0) return false;
	if (cd_paused) return false;
	if (!cdtv_play_drv) {
		cdtv_play_lba_next = -1;
		cdtv_play_lba_end  = -1;
		return false;
	}
	if (!cdtv_audio_fifo_ready()) return false;

	uint8_t buf[CDTV_CDDA_BYTES];
	uint32_t lba = (uint32_t)cdtv_play_lba_next;
	if (!cdtv_read_audio_sector(cdtv_play_drv, lba, buf)) {
		cdtv_dbg("CDDA pump read FAIL at lba=%u — aborting", lba);
		memset(buf, 0, sizeof(buf));
		cdtv_push_audio_sector(buf);
		cdtv_last_lba      = (int32_t)lba;
		cdtv_play_lba_next = -1;
		cdtv_play_lba_end  = -1;
		cdtv_play_drv      = NULL;
		cd_playing         = 0;
		cd_paused          = 0;
		cd_finished        = 1;
		cd_audio_status    = 0x14;       // PLAY_ERROR
		cdtv_inject_stch();
		return true;
	}

	cdtv_push_audio_sector(buf);
	cdtv_last_lba = (int32_t)lba;
	cdtv_play_lba_next++;

	if ((lba & 0xff) == 0) {
		cdtv_dbg("CDDA pump lba=%u (rem=%d)", lba,
		         cdtv_play_lba_end - cdtv_play_lba_next);
	}

	if (cdtv_play_lba_next >= cdtv_play_lba_end) {
		cdtv_dbg("CDDA natural end at lba=%u", lba);
		cdtv_play_lba_next = -1;
		cdtv_play_lba_end  = -1;
		cdtv_play_drv      = NULL;
		cd_playing         = 0;
		cd_paused          = 0;
		cd_finished        = 1;
		cd_audio_status    = 0x13;       // PLAY_COMPLETE
		// WinUAE cdtv.cpp:1212-1219: on cd_audio_finished, mirrors the same
		// state writes AND fires activate_stch=1 — the STCH interrupt is
		// what wakes the BIOS to notice play-end. SUBQ also flips to
		// PLAY_COMPLETE so any future loop-detection logic can re-arm.
		cdtv_inject_stch();
		// WinUAE do_stch parity (cdtv.cpp:1292): retry the play-end STCH until
		// the BIOS INT2 handler TAKES it (cdtv_stch_acked sees the flag),
		// then stop. A single pulse is lost to the scor/sten interrupt churn
		// (HW trace: seg-2/map STCH dropped → map stall); the old blind 60 s
		// heartbeat over-injected → credits stall. Ack-terminated retry fixes
		// both: it lands the pulse, and stops the instant it is consumed.
		stch_wait_ack = true;
		stch_retries  = STCH_PLAYEND_BUDGET;
		stch_next_ms  = GetTimer(STCH_PLAYEND_PERIOD_MS);
	}
	return true;
}

// MSF (binary M:S:F packed as 0xMMSSFF) → LBA, with 150-frame pre-gap.
static uint32_t cdtv_msf_to_lba(uint32_t msf)
{
	uint32_t m = (msf >> 16) & 0xff;
	uint32_t s = (msf >>  8) & 0xff;
	uint32_t f =  msf        & 0xff;
	uint32_t lsn = (m * 60u + s) * 75u + f;
	return (lsn >= 150u) ? (lsn - 150u) : 0u;
}

// -----------------------------------------------------------------------------
// Command interpreter — mirrors cdtv.cpp:524-695 cdrom_command_thread
// -----------------------------------------------------------------------------

// LBA → packed MSF (M in [23:16], S in [15:8], F in [7:0]). The +150 disk
// offset (pregap before track 1) is the caller's responsibility — track-
// relative positions don't add it, disk-relative do.
static uint32_t cdtv_lba2msf(uint32_t lba)
{
	uint32_t m = lba / (60u * 75u);
	uint32_t s = (lba / 75u) % 60u;
	uint32_t f =  lba % 75u;
	return (m << 16) | (s << 8) | f;
}

// Build a SUBQ frame (opcode 0x87). cmd[1] bit 1 selects MSF vs LSN format.
// Mirrors WinUAE cdtv.cpp cdrom_subq() at line 414: byte 0 is audio_status,
// bytes 2/3 are track/index, bytes 5..7 are disk-position MSF or LSN, bytes
// 9..11 are track-relative position. Returns 13. DotC's title-music loop
// polls this every few frames during play and after PLAY_COMPLETE to know
// when to re-arm the cue and to advance its title-screen state machine.
static int cmd_subq(const uint8_t *cmd, uint8_t *out)
{
	bool msf = (cmd[1] & 0x02) != 0;

	memset(out, 0, 13);
	out[0] = cd_audio_status;
	out[1] = 0x10;     // control=0x1 (audio), addr=0x0 (Q-channel mode 1)

	// Pick the LBA to report: live pump position if playing, else last LBA
	// pumped (so PLAY_COMPLETE / PAUSED / ERROR keep reporting the right
	// resting position). Default 0 if we never played.
	int32_t pump_lba = (cdtv_play_lba_next >= 0) ? cdtv_play_lba_next : cdtv_last_lba;
	uint32_t disk_lba = (pump_lba > 0) ? (uint32_t)pump_lba : 0;

	// Resolve which track this LBA falls in, using the same track[] that
	// READ TOC reports. drv->track[last] is the lead-out sentinel.
	drive_t *drv = cdtv_play_drv ? cdtv_play_drv : cdtv_find_drive();
	int track_idx = 0;
	uint32_t track_start = 0;
	if (drv && drv->track_cnt > 1) {
		int real_tracks = drv->track_cnt - 1;
		for (int i = 0; i < real_tracks; i++) {
			uint32_t s_lba = drv->track[i].start;
			uint32_t e_lba = s_lba + drv->track[i].length;
			if (disk_lba >= s_lba && disk_lba < e_lba) {
				track_idx   = i;
				track_start = s_lba;
				break;
			}
			// If we never fell into a real track, last partial track wins.
			track_idx   = i;
			track_start = s_lba;
		}
	}

	out[2] = (uint8_t)(track_idx + 1);   // 1-based track number
	out[3] = 0x01;                       // post-pregap index

	uint32_t track_lba = (disk_lba > track_start) ? (disk_lba - track_start) : 0;
	uint32_t disk_pos  = msf ? cdtv_lba2msf(disk_lba + 150) : disk_lba;
	uint32_t track_pos = msf ? cdtv_lba2msf(track_lba)      : track_lba;

	out[5]  = (disk_pos  >> 16) & 0xff;
	out[6]  = (disk_pos  >>  8) & 0xff;
	out[7]  = (disk_pos       ) & 0xff;
	out[9]  = (track_pos >> 16) & 0xff;
	out[10] = (track_pos >>  8) & 0xff;
	out[11] = (track_pos       ) & 0xff;

	return 13;
}

// READ TOC (0x8a). cmd[2] = track point requested. cmd[1] bit 1 = MSF format.
// Per WinUAE cdtv.cpp:460-486 returns 8 bytes per match, -1 if no match.
static int cmd_read_toc(const uint8_t *cmd, uint8_t *out)
{
	drive_t *drv = cdtv_find_drive();
	if (!drv || drv->track_cnt < 1) {
		cd_error = 1;
		return -1;
	}

	uint8_t want = cmd[2];
	bool msf = (cmd[1] & 0x02) != 0;
	int real_tracks = drv->track_cnt - 1;           // last track entry is lead-out
	if (real_tracks < 1) real_tracks = 1;

	// Resolve `want` to a track index. 0xa0/0xa1/0xa2 are first/last/lead-out
	// pseudo-points; 1..N are real tracks.
	uint32_t lba;
	uint8_t  control = 0x04;                        // data track default
	uint8_t  point   = want;

	if (want == 0xa0) {
		lba = drv->track[0].number;                 // first track number (BCD-free)
		point = 0xa0;
	} else if (want == 0xa1) {
		lba = (uint32_t)real_tracks;
		point = 0xa1;
	} else if (want == 0xa2) {
		// lead-out = total LBA span
		uint32_t end = 0;
		for (int i = 0; i < drv->track_cnt; i++) {
			uint32_t e = drv->track[i].start + drv->track[i].length;
			if (e > end) end = e;
		}
		lba = end;
		point = 0xa2;
	} else if (want >= 1 && want <= 99 && want <= real_tracks) {
		int idx = want - 1;
		lba = drv->track[idx].start;
		// attr bit 0 set = audio in our ide_cdrom convention; data tracks
		// report control=0x04, audio control=0x00.
		if (drv->track[idx].attr & 0x01) control = 0x00;
	} else {
		return -1;
	}

	uint32_t addr;
	if (msf) {
		uint32_t lsn = lba + 150u;
		uint32_t mins = (lsn / 75u) / 60u;
		uint32_t secs = (lsn / 75u) % 60u;
		uint32_t fr   = lsn % 75u;
		addr = (mins << 16) | (secs << 8) | fr;
	} else {
		addr = lba;
	}

	out[0] = 0;
	out[1] = (1u << 4) | control;                   // adr=1, control in low nibble
	out[2] = point;
	out[3] = (uint8_t)real_tracks;
	out[4] = 0;
	out[5] = (uint8_t)(addr >> 16);
	out[6] = (uint8_t)(addr >>  8);
	out[7] = (uint8_t)(addr      );
	cd_finished = 1;
	return 8;
}

// INFO (0x89). Returns first/last track + total MSF size.
static int cmd_info(uint8_t *out)
{
	drive_t *drv = cdtv_find_drive();
	if (!drv || drv->track_cnt < 1) {
		cd_error = 1;
		return -1;
	}

	int real_tracks = drv->track_cnt - 1;
	if (real_tracks < 1) real_tracks = 1;

	uint32_t end = 0;
	for (int i = 0; i < drv->track_cnt; i++) {
		uint32_t e = drv->track[i].start + drv->track[i].length;
		if (e > end) end = e;
	}
	uint32_t lsn = end + 150u;
	uint32_t mins = (lsn / 75u) / 60u;
	uint32_t secs = (lsn / 75u) % 60u;
	uint32_t fr   = lsn % 75u;

	out[0] = 1;                                     // first track
	out[1] = (uint8_t)real_tracks;                  // last track
	out[2] = (uint8_t)mins;
	out[3] = (uint8_t)secs;
	out[4] = (uint8_t)fr;
	cd_finished = 1;
	return 5;
}

// MODE SET (0x84). Per cdtv.cpp:488-498, valid sector sizes are
// 512/1024/2048/2052/2336/2340. Anything else => cd_error.
static void cmd_mode_set(const uint8_t *cmd)
{
	uint16_t sz = (uint16_t)((cmd[2] << 8) | cmd[3]);
	switch (sz) {
		case 512: case 1024: case 2048:
		case 2052: case 2336: case 2340:
			cd_sectorsize = sz;
			cd_finished = 1;
			break;
		default:
			cd_error = 1;
			break;
	}
}

// PLAY (0x09 LSN / 0x0a MSF / 0x0b track). Per WinUAE cdtv.cpp:374-412
// (play_cd) and :328-371 (play_cdtrack):
//   0x09 LSN:   cmd[1..3] = start LSN, cmd[4..6] = LENGTH in LSNs
//   0x0a MSF:   cmd[1..3] = start MSF, cmd[4..6] = end MSF
//   0x0b track: cmd[1]=start trk, cmd[2]=start idx, cmd[3]=end trk, cmd[4]=end idx
// All resolve to s_lba/e_lba and arm the CDDA pump (poll-side cdtv_cdda_pump
// pushes one sector per FIFO-ready tick to UIO 0xF200).
//
// Returns reply byte count (1 byte status). 0x42 = playing + media.
static int cmd_play(const uint8_t *cmd, uint8_t *out)
{
	drive_t *drv = cdtv_find_drive();
	if (!drv || drv->track_cnt < 1) {
		cd_error = 1;
		out[0] = 0x00;
		return -1;
	}

	uint8_t op = cmd[0];
	uint32_t s_lba = 0, e_lba = 0;
	int real_tracks = drv->track_cnt - 1;
	if (real_tracks < 1) real_tracks = 1;

	if (op == 0x0b) {
		int track_start = cmd[1];
		int track_end   = cmd[3];
		if (track_start == 0 && track_end == 0) {
			cdtv_dbg("PLAY TRACK 0,0 — stop");
			cdtv_play_lba_next = -1;
			cdtv_play_lba_end  = -1;
			cdtv_play_drv      = NULL;
			cd_playing = 0;
			cd_paused  = 0;
			cd_motor   = 0;
			cd_error   = 1;
			cd_audio_status = 0x15;
			out[0] = 0x00;
			return 1;
		}
		bool got_start = false;
		uint32_t lead_out = drv->track[real_tracks].start;
		uint32_t s = 0, e = lead_out;
		for (int i = 0; i < real_tracks; i++) {
			int trk = i + 1;
			if (trk == track_start) { s = drv->track[i].start; got_start = true; }
			if (trk == track_end)   { e = drv->track[i].start; }
		}
		if (!got_start) {
			cdtv_dbg("PLAY TRACK %d-%d: illegal start", track_start, track_end);
			cd_error = 1;
			out[0] = 0x00;
			return 1;
		}
		s_lba = s;
		e_lba = e;
	} else {
		uint32_t a = ((uint32_t)cmd[1] << 16) | ((uint32_t)cmd[2] << 8) | cmd[3];
		uint32_t b = ((uint32_t)cmd[4] << 16) | ((uint32_t)cmd[5] << 8) | cmd[6];
		if (op == 0x09) {
			s_lba = a;
			e_lba = a + b;            // length-in-LSNs → end LBA
		} else {                       // 0x0a MSF
			s_lba = cdtv_msf_to_lba(a);
			e_lba = (b < 0x00ffffff) ? cdtv_msf_to_lba(b) : 0xffffffffu;
		}
		uint32_t lead_out = drv->track[real_tracks].start;
		if (e_lba > lead_out) e_lba = lead_out;
	}

	if (s_lba == 0 && e_lba == 0) {
		cdtv_dbg("PLAY stop (0,0)");
		cdtv_play_lba_next = -1;
		cdtv_play_lba_end  = -1;
		cdtv_play_drv      = NULL;
		cd_playing = 0;
		cd_paused  = 0;
		cd_motor   = 0;
		cd_error   = 1;
		cd_audio_status = 0x15;
		out[0] = 0x00;
		return 1;
	}

	// Validate that the start LBA falls in an audio track. If not, ack as
	// playing-with-error so the BIOS state machine moves on without
	// streaming garbage data through the audio mixer.
	bool start_is_audio = false;
	for (int i = 0; i < real_tracks; i++) {
		uint32_t end = drv->track[i].start + drv->track[i].length;
		if (s_lba >= drv->track[i].start && s_lba < end) {
			start_is_audio = !(drv->track[i].attr & 0x40);
			break;
		}
	}

	if (start_is_audio && e_lba > s_lba) {
		// Phase 3 item 7: aim physical_disc's prefetch lane at the audio run
		// about to stream, so the worker fills ahead instead of every CDDA
		// sector faulting in synchronously. lane_of() routes this to the
		// audio lane. No-op on CHD.
		if (drv->phys) physical_disc_seek_hint((int)s_lba);
		cdtv_play_drv      = drv;
		cdtv_play_lba_next = (int32_t)s_lba;
		cdtv_play_lba_end  = (int32_t)e_lba;
		cdtv_last_lba      = (int32_t)s_lba;
		cd_paused = 0;
		cd_audio_status    = 0x11;       // IN_PROGRESS
		cdtv_dbg("PLAY arm op=%02x s_lba=%u e_lba=%u (n=%u)",
		         op, s_lba, e_lba, e_lba - s_lba);
	} else {
		cdtv_play_drv      = NULL;
		cdtv_play_lba_next = -1;
		cdtv_play_lba_end  = -1;
		cd_audio_status    = 0x14;       // PLAY_ERROR (non-audio cue)
		cdtv_dbg("PLAY refuse op=%02x s_lba=%u e_lba=%u (audio=%d)",
		         op, s_lba, e_lba, start_is_audio);
	}

	cd_playing = 1;
	cd_motor   = 1;
	stch_playstart_pending = true;
	out[0] = 0x42;
	return 1;
}

// Dispatch one accumulated command. cmd_buf[0] is opcode, cmd_buf[1..cmd_need-1]
// are payload bytes. Returns the reply bytes (or empty) to send back.
static void cdtv_dispatch(void)
{
	uint8_t op = cmd_buf[0];
	uint8_t reply[CDTV_CMD_MAX];
	int rlen = 0;

	// Any non-STATUS command means BIOS has progressed past the post-mount
	// STATUS-only poll loop — STCH retry budget can be shut off.
	if (op != 0x81 && stch_retries > 0) {
		cdtv_dbg("STCH retry budget cleared (op=%02x)", op);
		stch_retries = 0;
		stch_wait_ack = false;
	}

#if CDTV_DEBUG
	{
		char dump[3 * CDTV_CMD_MAX + 4];
		int n = 0;
		int dump_n = (cmd_idx > CDTV_CMD_MAX) ? CDTV_CMD_MAX : cmd_idx;
		for (int i = 0; i < dump_n; i++) n += snprintf(dump + n, sizeof(dump) - n, "%02x ", cmd_buf[i]);
		cdtv_dbg("RX[%d]: %s", cmd_idx, dump);
	}
#endif

	switch (op) {
		case 0x00:
		case 0x80:
			// Ping / reset ack — fixed 2-byte reply.
			reply[0] = 0xaa;
			reply[1] = 0x55;
			rlen = 2;
			break;

		case 0x01: /* seek */
			cd_finished = 1;
			rlen = 0;
			break;

		case 0x02: { /* read N sectors from LBA into chip RAM @ acr */
			// Command layout per cdtv.cpp:524-695 / CR-511 spec:
			//   s[1..3] = LBA (24-bit, big-endian)
			//   s[4..5] = transfer length in SECTORS (big-endian)
			// Sector size on the wire comes from MODE SET ($84) — defaulting
			// to 2048 (ISO9660 cooked). The BIOS pre-programs WTC = nsec *
			// sec_sz and ACR = chip-RAM dest, then writes DMA START. Our
			// bridge's drain FSM pulls bytes from sec_fifo into chip RAM and
			// fires E_INT/INTS + INT2 once wtc hits 0.
			uint32_t lba   = ((uint32_t)cmd_buf[1] << 16)
			               | ((uint32_t)cmd_buf[2] <<  8)
			               | ((uint32_t)cmd_buf[3]      );
			uint16_t nsec  = ((uint16_t)cmd_buf[4] <<  8) | cmd_buf[5];

			drive_t *drv = cdtv_find_drive();
			// Data path, not audio. Easy to miss when grepping for the CDDA
			// chd_f checks, but CDTV data reads fail without it even once
			// cdtv_find_drive accepts a physical drive.
			if (!drv || (!drv->chd_f && !drv->phys && !drv->f)) {
				cdtv_dbg("READ DATA lba=%u nsec=%u - no drive/media", lba, nsec);
				cd_error    = 1;
				cd_finished = 1;
				rlen        = 0;
				break;
			}

			cdtv_dbg("READ DATA lba=%u nsec=%u sec_sz=%u", lba, nsec, cd_sectorsize);

			// The drive has one pickup: a READ stops the CD audio in progress,
			// spins the motor up and leaves the audio status at NOT SUPPORTED,
			// as WinUAE does (Main_MiSTer 1f55555). Without it the CDDA pump
			// picked the track up again after the read, from where it was.
			if (cd_playing) {
				cdtv_play_lba_next = -1;
				cdtv_play_lba_end  = -1;
				cdtv_play_drv      = NULL;
				cd_playing  = 0;
				cd_paused   = 0;
				cd_finished = 1;
			}
			cd_motor = 1;
			cd_audio_status = 0x00;

			// Phase 3 item 7: hint before the streaming loop below. The loop
			// itself reads lba..lba+nsec synchronously, but the BIOS walks a
			// file sequentially across many READ DATA commands, so pointing
			// the lane here is what lets the worker have the *next* request
			// already in the ring. No-op on CHD.
			if (drv->phys) physical_disc_seek_hint((int)lba);

			uint8_t raw[2352];
			bool    failed = false;
			// Stream one sector per loop iteration. The bridge's staging FIFO
			// is 8 KB, so one 2 KB cooked sector fits with margin.
			for (uint16_t s = 0; s < nsec; s++) {
				if (cdrom_read_raw_sector(drv, lba + s, raw) != 0) {
					cdtv_dbg("READ DATA chd_read failed at lba=%u", lba + s);
					failed = true;
					break;
				}

				const uint8_t *src;
				uint16_t       len;
				// Pick the wanted bytes out of the 2352-byte raw frame per
				// MODE SET. WinUAE cdtv.cpp dispatch in cmd_read_data:
				//   2048: user data only, sync+header stripped (offset 16)
				//   2336: user data + EDC/ECC (offset 16, len 2336)
				//   2352: full raw frame
				//   2052: 2048 + 4 subheader bytes (offset 16, len 2052)
				//   2340: full minus 12 sync bytes (offset 12, len 2340)
				//   512 / 1024: subset of user data starting at offset 16
				switch (cd_sectorsize) {
					case 2048: src = raw + 16; len = 2048; break;
					case 2052: src = raw + 16; len = 2052; break;
					case 2336: src = raw + 16; len = 2336; break;
					case 2340: src = raw + 12; len = 2340; break;
					case 2352: src = raw;      len = 2352; break;
					default:   src = raw + 16; len = cd_sectorsize; break;
				}

				if (!cdtv_push_sector(src, len)) {
					// Credit gate gave up — the bridge FIFO never made room.
					// Report an error rather than push bytes the overflow
					// guard would drop, which would corrupt the transfer
					// invisibly and leave the title reading garbage.
					cdtv_dbg("READ DATA aborted at lba=%u — sector FIFO stalled", lba + s);
					failed = true;
					break;
				}

				// Pace producer ≈ drain rate. Bridge drain FSM pumps ~280 ns/byte
				// → ~575 µs per 2 KB sector. SPI push is ~1.6 µs/byte → 3.3 ms per
				// sector. Bare push edges past drain across many sectors and can
				// fill the 8 KB FIFO. The credit gate in cdtv_push_sector is now
				// what makes that SAFE; this sleep is kept purely as pacing so the
				// timing of a normal read is unchanged from before the gate
				// existed, and so the common case never has to spin on the credit.
				if (s + 1 < nsec) usleep(1000);
			}

			if (failed) cd_error = 1;
			cd_finished = 1;
			rlen = 0;
			break;
		}

		case 0x04: cd_motor = 1; cd_finished = 1; rlen = 0; break;
		case 0x05:
			// Motor off — tear down any in-flight CDDA pump.
			cdtv_play_lba_next = -1;
			cdtv_play_lba_end  = -1;
			cdtv_play_drv      = NULL;
			cd_playing = 0;
			cd_paused  = 0;
			cd_motor   = 0;
			cd_finished = 1;
			cd_audio_status = 0x15;       // NO_STATUS
			rlen = 0;
			break;

		case 0x09: /* play (lsn) */
		case 0x0a: /* play (msf) */
		case 0x0b: /* play track */
			rlen = cmd_play(cmd_buf, reply);
			if (rlen < 0) rlen = 0;
			break;

		case 0x81: {
			// STATUS query — flag bits per cdtv.cpp:582-600.
			uint8_t flag = 0;
			if (!cd_isready) flag |= 1 << 0;             // 01 = drive ready
			if (cd_playing)  flag |= 1 << 2;             // 04
			if (cd_finished) flag |= 1 << 3;             // 08
			if (cd_error)    flag |= 1 << 4;             // 10
			if (cd_motor)    flag |= 1 << 5;             // 20
			if (cd_media)    flag |= 1 << 6;             // 40
			reply[0] = flag;
			rlen = 1;
			cd_finished = 0;
			break;
		}

		case 0x82: {
			// Last error / sense (6 bytes). For M2 phase-1c we report no
			// outstanding error (just enough to satisfy any BIOS query).
			memset(reply, 0, 6);
			if (cd_error) reply[2] |= 1 << 4;
			cd_error   = 0;
			cd_isready = 0;
			rlen = 6;
			cd_finished = 1;
			break;
		}

		case 0x83: {
			static const char *MODEL = "MATSHITA0.96";
			int n = (int)strlen(MODEL);
			memcpy(reply, MODEL, n);
			rlen = n;
			cd_finished = 1;
			break;
		}

		case 0x84:
			cmd_mode_set(cmd_buf);
			rlen = 0;
			break;

		case 0x85:
			reply[0] = (uint8_t)(cd_sectorsize >> 8);
			reply[1] = (uint8_t)(cd_sectorsize     );
			rlen = 2;
			break;

		case 0x86: {
			drive_t *drv = cdtv_find_drive();
			if (!drv || drv->track_cnt < 1) {
				cd_error = 1;
				rlen = 0;          // -1 in WinUAE; we just emit nothing
				break;
			}
			uint32_t end = 0;
			for (int i = 0; i < drv->track_cnt; i++) {
				uint32_t e = drv->track[i].start + drv->track[i].length;
				if (e > end) end = e;
			}
			uint32_t size = end - 1;
			reply[0] = (uint8_t)(size >> 16);
			reply[1] = (uint8_t)(size >>  8);
			reply[2] = (uint8_t)(size      );
			reply[3] = (uint8_t)(cd_sectorsize >> 8);
			reply[4] = (uint8_t)(cd_sectorsize     );
			rlen = 5;
			break;
		}

		case 0x87: rlen = cmd_subq(cmd_buf, reply); break;

		case 0x88:
			memset(reply, 0, 14);
			rlen = 14;
			break;

		case 0x89: {
			int n = cmd_info(reply);
			rlen = (n > 0) ? n : 0;
			break;
		}

		case 0x8a: {
			int n = cmd_read_toc(cmd_buf, reply);
			rlen = (n > 0) ? n : 0;
			break;
		}

		case 0x8b: /* pause / resume */
			// WinUAE cdtv.cpp:667-672: cmd[1] == 0x00 → pause, else resume.
			// Pump state is preserved across pause so resume picks up at the
			// same LBA. cd_playing stays 1 so STATUS poll reflects the
			// suspended-play state machine.
			cd_paused = (cmd_buf[1] == 0x00) ? 1 : 0;
			if (cdtv_play_lba_next >= 0)
				cd_audio_status = cd_paused ? 0x12 : 0x11;
			cdtv_dbg("PAUSE/RESUME cmd1=%02x → paused=%d", cmd_buf[1], cd_paused);
			cd_finished = 1;
			rlen = 0;
			break;

		case 0xa2:
			memset(reply, 0, 4);
			rlen = 4;
			break;

		case 0xa3: /* front panel toggle */
			cd_finished = 1;
			rlen = 0;
			break;

		default:
			cdtv_dbg("unknown opcode %02x", op);
			cd_error = 1;
			rlen = 0;
			break;
	}

	if (rlen > 0) cdtv_push_reply(reply, rlen);

	// Reset for next frame.
	cmd_idx  = 0;
	cmd_need = 0;
}

static char cd_save_path_active[256] = {0};
static bool cd_save_load_pending     = false;
static bool cd_save_dirty_observed   = false;
static bool cd_save_load_failed      = false;

static char card_save_path_active[256] = {0};
static bool card_save_dirty_observed   = false;

static uint32_t nvr_verify_due_at_ms = 0;
static char     nvr_verify_path[256] = {0};

static uint64_t fnv1a_64(const char *s)
{
	uint64_t h = 1469598103934665603ull;
	while (*s) {
		h ^= (uint8_t)*s++;
		h *= 1099511628211ull;
	}
	return h;
}

static void cdtv_compute_save_path(const char *cd_path, char *out, size_t outsz)
{
	const char *base = strrchr(cd_path, '/');
	base = base ? base + 1 : cd_path;
	snprintf(out, outsz, "%s/cdtv-%016" PRIx64 ".nvr", CDTV_NVRAM_DIR, fnv1a_64(base));
}

static void cdtv_compute_card_path(const char *cd_path, char *out, size_t outsz)
{
	const char *base = strrchr(cd_path, '/');
	base = base ? base + 1 : cd_path;
	snprintf(out, outsz, "%s/cdtv-%016" PRIx64 ".card", CDTV_NVRAM_DIR, fnv1a_64(base));
}

static void cdtv_nvram_dump(uint8_t *out)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(CDTV_NVRAM_ADDR);
	for (int i = 0; i < CDTV_NVRAM_BYTES; i++) out[i] = (uint8_t)spi_w(0);
	DisableIO();
}

static void cdtv_nvram_upload(const uint8_t *buf)
{
	static uint16_t words[CDTV_NVRAM_BYTES / 2];
	memcpy(words, buf, CDTV_NVRAM_BYTES);

	EnableIO();
	fpga_spi_fast(UIO_DMA_WRITE);
	fpga_spi_fast(CDTV_NVRAM_ADDR);
	fpga_spi_fast(0);
	fpga_spi_fast_block_write(words, CDTV_NVRAM_BYTES / 2);
	DisableIO();
}

static bool cdtv_nvram_load_from_path(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		cdtv_dbg("NVR load skip: no file at %s (errno=%d)", path, errno);
		return false;
	}
	if (st.st_size < CDTV_NVRAM_BYTES) {
		cdtv_dbg("NVR load skip: %s too small %lld (want >= %d)",
		         path, (long long)st.st_size, CDTV_NVRAM_BYTES);
		return false;
	}

	static uint8_t nvr[CDTV_NVRAM_BYTES];
	FILE *f = fopen(path, "rb");
	if (!f) {
		cdtv_dbg("NVR load FAIL: fopen(%s) errno=%d", path, errno);
		cd_save_load_failed = true;
		return false;
	}
	size_t got = fread(nvr, 1, CDTV_NVRAM_BYTES, f);
	fclose(f);
	if (got != CDTV_NVRAM_BYTES) {
		cdtv_dbg("NVR load FAIL: read %u of %d bytes from %s",
		         (unsigned)got, CDTV_NVRAM_BYTES, path);
		cd_save_load_failed = true;
		return false;
	}

	cdtv_nvram_upload(nvr);

	cd_save_load_failed = false;
	strncpy(nvr_verify_path, path, sizeof(nvr_verify_path) - 1);
	nvr_verify_path[sizeof(nvr_verify_path) - 1] = 0;
	nvr_verify_due_at_ms = (uint32_t)GetTimer(100);
	cdtv_dbg("NVR loaded (%d bytes from %s) - verify in 100 ms", CDTV_NVRAM_BYTES, path);
	return true;
}

static void cdtv_nvram_verify_load_pending(void)
{
	if (!nvr_verify_due_at_ms) return;
	if (!CheckTimer(nvr_verify_due_at_ms)) return;
	nvr_verify_due_at_ms = 0;

	static uint8_t verify[CDTV_NVRAM_BYTES];
	static uint8_t expected[CDTV_NVRAM_BYTES];

	cdtv_nvram_dump(verify);

	FILE *f = fopen(nvr_verify_path, "rb");
	if (!f) {
		cdtv_dbg("NVR verify: fopen(%s) failed errno=%d", nvr_verify_path, errno);
		cd_save_load_failed = true;
		return;
	}
	size_t got = fread(expected, 1, CDTV_NVRAM_BYTES, f);
	fclose(f);
	if (got != CDTV_NVRAM_BYTES) {
		cdtv_dbg("NVR verify: short read %u/%d", (unsigned)got, CDTV_NVRAM_BYTES);
		cd_save_load_failed = true;
		return;
	}
	if (!memcmp(expected, verify, CDTV_NVRAM_BYTES)) {
		cdtv_dbg("NVR loaded %d bytes from %s (verify OK)",
		         CDTV_NVRAM_BYTES, nvr_verify_path);
		cd_save_load_failed = false;
		return;
	}

	int first_bad = -1, mismatches = 0;
	for (int i = 0; i < CDTV_NVRAM_BYTES; i++) {
		if (verify[i] != expected[i]) {
			if (first_bad < 0) first_bad = i;
			mismatches++;
		}
	}
	cdtv_dbg("NVR LOAD VERIFY FAILED: %d/%d mismatched (first @ 0x%04x)",
	         mismatches, CDTV_NVRAM_BYTES, first_bad);
	cd_save_load_failed = true;
}

static bool cdtv_nvram_save_to_path(const char *path)
{
	static uint8_t buf[CDTV_NVRAM_BYTES];
	cdtv_nvram_dump(buf);

	FILE *cur = fopen(path, "rb");
	if (cur) {
		static uint8_t disk_buf[CDTV_NVRAM_BYTES];
		size_t n = fread(disk_buf, 1, CDTV_NVRAM_BYTES, cur);
		fclose(cur);
		if (n == CDTV_NVRAM_BYTES && !memcmp(disk_buf, buf, CDTV_NVRAM_BYTES)) {
			cdtv_dbg("NVR save skipped: identical to %s", path);
			return true;
		}
	}

	if (mkdir(CDTV_NVRAM_DIR, 0755) != 0 && errno != EEXIST) {
		cdtv_dbg("NVR mkdir(%s) failed: errno=%d", CDTV_NVRAM_DIR, errno);
	}

	char tmp_path[300];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		cdtv_dbg("NVR fopen(%s) failed: errno=%d", tmp_path, errno);
		return false;
	}

	uint8_t zero[512] = {0};
	size_t wrote = fwrite(buf, 1, CDTV_NVRAM_BYTES, f);
	for (int i = 0; i < (CDTV_NVRAM_FILE_BYTES - CDTV_NVRAM_BYTES) / (int)sizeof(zero); i++) {
		wrote += fwrite(zero, 1, sizeof(zero), f);
	}
	int flush = fflush(f);
	int fsy   = fsync(fileno(f));
	int cls   = fclose(f);
	if (wrote != CDTV_NVRAM_FILE_BYTES || flush != 0 || fsy != 0 || cls != 0) {
		cdtv_dbg("NVR write incomplete: wrote=%u flush=%d fsync=%d close=%d",
		         (unsigned)wrote, flush, fsy, cls);
		unlink(tmp_path);
		return false;
	}

	if (rename(tmp_path, path) != 0) {
		cdtv_dbg("NVR rename(%s -> %s) failed: errno=%d", tmp_path, path, errno);
		unlink(tmp_path);
		return false;
	}

	cdtv_dbg("NVR saved %d bytes to %s", CDTV_NVRAM_BYTES, path);
	return true;
}

static void cdtv_card_dump(uint8_t *out)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(CDTV_CARD_ADDR);
	for (int i = 0; i < CDTV_CARD_BYTES; i++) out[i] = (uint8_t)spi_w(0);
	DisableIO();
}

static void cdtv_card_upload(const uint8_t *buf)
{
	static uint16_t words[CDTV_CARD_BYTES / 2];
	memcpy(words, buf, CDTV_CARD_BYTES);

	EnableIO();
	fpga_spi_fast(UIO_DMA_WRITE);
	fpga_spi_fast(CDTV_CARD_ADDR);
	fpga_spi_fast(0);
	fpga_spi_fast_block_write(words, CDTV_CARD_BYTES / 2);
	DisableIO();
}

static void cdtv_card_load_from_path(const char *path)
{
	static uint8_t card[CDTV_CARD_BYTES];
	memset(card, 0, sizeof(card));

	FILE *f = path[0] ? fopen(path, "rb") : NULL;
	if (f) {
		size_t got = fread(card, 1, CDTV_CARD_BYTES, f);
		fclose(f);
		cdtv_dbg("CARD loaded %u/%d bytes from %s", (unsigned)got, CDTV_CARD_BYTES, path);
	}
	else {
		cdtv_dbg("CARD blank (no file at %s)", path[0] ? path : "(none)");
	}

	cdtv_card_upload(card);
}

static bool cdtv_card_save_to_path(const char *path)
{
	static uint8_t buf[CDTV_CARD_BYTES];
	cdtv_card_dump(buf);

	if (FILE *cur = fopen(path, "rb")) {
		static uint8_t disk_buf[CDTV_CARD_BYTES];
		size_t n = fread(disk_buf, 1, CDTV_CARD_BYTES, cur);
		fclose(cur);
		if (n == CDTV_CARD_BYTES && !memcmp(disk_buf, buf, CDTV_CARD_BYTES)) {
			card_save_dirty_observed = false;
			return true;
		}
	}

	if (mkdir(CDTV_NVRAM_DIR, 0755) != 0 && errno != EEXIST) {
		cdtv_dbg("CARD mkdir(%s) failed: errno=%d", CDTV_NVRAM_DIR, errno);
		return false;
	}

	FILE *f = fopen(path, "wb");
	if (!f) {
		cdtv_dbg("CARD fopen(%s) failed: errno=%d", path, errno);
		return false;
	}

	size_t wrote = fwrite(buf, 1, CDTV_CARD_BYTES, f);
	int flush = fflush(f);
	int fsy   = fsync(fileno(f));
	int cls   = fclose(f);

	if (wrote != CDTV_CARD_BYTES || flush != 0 || fsy != 0 || cls != 0) {
		cdtv_dbg("CARD save FAILED %s wrote=%u flush=%d fsync=%d close=%d errno=%d",
		         path, (unsigned)wrote, flush, fsy, cls, errno);
		return false;
	}

	card_save_dirty_observed = false;
	cdtv_dbg("CARD saved %d bytes to %s", CDTV_CARD_BYTES, path);
	return true;
}

static bool cdtv_nvram_save_to_disk(void)
{
	if (!cd_save_path_active[0]) return false;
	if (cd_save_load_failed) {
		static bool warned_once = false;
		if (!warned_once) {
			cdtv_dbg("NVR save BLOCKED: load failed for %s - refusing to overwrite "
			         "a real save with a blank NVRAM. Future blocks silent.",
			         cd_save_path_active);
			warned_once = true;
		}
		return false;
	}
	return cdtv_nvram_save_to_path(cd_save_path_active);
}
// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void cdtv_cd_set_cd_path(const char *path)
{
	if (!path) path = "";

	char new_save[sizeof(cd_save_path_active)] = {0};
	if (path[0]) cdtv_compute_save_path(path, new_save, sizeof(new_save));

	char new_card[sizeof(card_save_path_active)] = {0};
	if (path[0]) cdtv_compute_card_path(path, new_card, sizeof(new_card));

	if (strcmp(new_card, card_save_path_active) != 0) {
		if (card_save_path_active[0] && card_save_dirty_observed) {
			cdtv_dbg("set_cd_path: flushing old card %s before swap", card_save_path_active);
			cdtv_card_save_to_path(card_save_path_active);
		}
		strncpy(card_save_path_active, new_card, sizeof(card_save_path_active) - 1);
		card_save_path_active[sizeof(card_save_path_active) - 1] = 0;
		card_save_dirty_observed = false;
	}

	if (strcmp(new_save, cd_save_path_active) != 0) {
		if (cd_save_path_active[0] && cd_save_dirty_observed) {
			cdtv_dbg("set_cd_path: flushing old save %s before swap", cd_save_path_active);
			cdtv_nvram_save_to_path(cd_save_path_active);
		}
		strncpy(cd_save_path_active, new_save, sizeof(cd_save_path_active) - 1);
		cd_save_path_active[sizeof(cd_save_path_active) - 1] = 0;
		cd_save_dirty_observed = false;
		cd_save_load_failed    = false;
		cd_save_load_pending   = (cd_save_path_active[0] != 0);
	}
	bool has_path    = (path[0] != 0);
	bool was_present = (cd_media != 0);
	bool swap        = was_present && has_path && strcmp(cd_path_active, path) != 0;

	strncpy(cd_path_active, path, sizeof(cd_path_active) - 1);
	cd_path_active[sizeof(cd_path_active) - 1] = 0;

	cd_media   = swap ? 0 : (has_path ? 1 : 0);
	cd_isready = 0;
	cd_motor   = 0;
	cd_playing = 0;
	cd_paused  = 0;
	cd_audio_status = 0x15;            // NO_STATUS on disc swap / unmount
	cdtv_last_lba   = 0;
	// Tear down any in-flight CDDA pump — drive may have vanished.
	cdtv_play_lba_next = -1;
	cdtv_play_lba_end  = -1;
	cdtv_play_drv      = NULL;
	// WinUAE pattern: every operation completion sets cd_finished=1 (SEEK,
	// MOTOR ON/OFF, INFO, PAUSE, end-of-DMA all set it). The 0x81 STATUS
	// handler clears it after reading. So the *first* STATUS after a state
	// transition should see bit 3 set, then subsequent polls see it clear —
	// BIOS uses that 1→0 transition as "operation acknowledged, proceed".
	// Without this, our STATUS reply is steady-state 0x41 and BIOS just
	// re-polls forever.
	cd_finished = has_path ? 1 : 0;

	stch_retries = STCH_RETRY_BUDGET;
	stch_next_ms = GetTimer(0);

	if (swap) {
		cd_swap_pending  = true;
		cd_swap_ready_ms = GetTimer(CDTV_SWAP_SETTLE_MS);
	} else {
		cd_swap_pending  = false;
		cd_swap_ready_ms = 0;
	}

	cdtv_dbg("set_cd_path: %s%s", has_path ? path : "(empty)",
	         swap ? " (swap, removal edge synthesized)" : "");
}

void cdtv_cd_init(void)
{
	// Order matters: ide_cdrom.cpp's cdrom_parse runs cdtv_cd_set_cd_path
	// BEFORE minimig_boot.cpp's BootInit() reaches cdtv_cd_init. So this
	// init must preserve any mount state established by the earlier call
	// (cd_media + cd_isready); only the transient command/state machine
	// vars get cleared. Clobbering cd_isready here was the M2 phase-1c bug
	// that made STATUS reply 0x41 ("media present, drive NOT ready"),
	// stalling the CDTV BIOS at the Welcome splash with disc inserted.
	cmd_idx       = 0;
	cmd_need      = 0;
	cd_motor      = 0;
	cd_media      = (cd_path_active[0] != 0) ? 1 : 0;
	// WinUAE never sets cd_isready non-zero (spec §1260: "Effective
	// behaviour: bit 0 of STATUS is always 1"). Our prior cd_isready=cd_media
	// gave STATUS reply 0x40 (bit 0 clear, "invalid status"), after which the
	// CDTV BIOS issued one $81, accepted the 0x40 byte over STEN, then stopped
	// dispatching commands — observed in cdtv_trace_phase1h_v2.log. Match
	// WinUAE: leave cd_isready=0 so STATUS reply is 0x41 when media present.
	cd_isready    = 0;
	cd_playing    = 0;
	cd_paused     = 0;
	cd_audio_status = 0x15;             // NO_STATUS on init
	cdtv_last_lba   = 0;
	cdtv_play_lba_next = -1;
	cdtv_play_lba_end  = -1;
	cdtv_play_drv      = NULL;
	// See cdtv_cd_set_cd_path — first STATUS after media-present must show
	// cd_finished=1 so BIOS sees the 1→0 transition.
	cd_finished   = cd_media ? 1 : 0;
	cd_error      = 0;
	cd_sectorsize = 2048;
	// If a CD is already mounted at init time, re-arm the STCH inject —
	// Minimig core reload may have wiped the TPI ilatch between the
	// previous mount and this fresh BIOS boot.
	if (cd_media) {
		stch_retries = STCH_RETRY_BUDGET;
		stch_next_ms = GetTimer(0);
	}

	cd_swap_pending  = false;
	cd_swap_ready_ms = 0;

	cd_save_dirty_observed   = false;
	cd_save_load_failed      = false;
	card_save_dirty_observed = false;
	if (cd_save_path_active[0]) cd_save_load_pending = true;

	cdtv_dbg("init media=%d isready=%d stch_retries=%d", cd_media, cd_isready, stch_retries);
}

void cdtv_cd_poll(void)
{
	if (!cdtv_active()) return;

	if (cd_swap_pending && CheckTimer(cd_swap_ready_ms)) {
		cd_swap_pending = false;
		cd_media    = 1;
		cd_finished = 1;
		stch_retries = STCH_RETRY_BUDGET;
		stch_next_ms = GetTimer(0);
		cdtv_dbg("swap: settle elapsed, new disc now present");
	}

	if (cd_save_load_pending) {
		cd_save_load_pending = false;
		if (cd_save_path_active[0]) cdtv_nvram_load_from_path(cd_save_path_active);
		cdtv_card_load_from_path(card_save_path_active);
	}

	cdtv_nvram_verify_load_pending();

	{
		static int nvr_menu_was_present = 0;
		const bool nvr_dirty_now = (cdtv_read_status() & CDTV_STATUS_NVR_DIRTY) != 0;
		const int  nvr_menu_now  = menu_present();
		const bool osd_open_edge = nvr_menu_now && !nvr_menu_was_present;
		nvr_menu_was_present     = nvr_menu_now;

		if (nvr_dirty_now) cd_save_dirty_observed = true;

		const bool card_dirty_now = (cdtv_read_status() & CDTV_STATUS_CARD_DIRTY) != 0;
		if (card_dirty_now) card_save_dirty_observed = true;

		if (osd_open_edge && cd_save_dirty_observed) {
			cdtv_dbg("NVR flush: OSD opened with dirty pending");
			cdtv_nvram_save_to_disk();
		}

		if (osd_open_edge && card_save_dirty_observed && card_save_path_active[0]) {
			cdtv_dbg("CARD flush: OSD opened with dirty pending");
			cdtv_card_save_to_path(card_save_path_active);
		}
	}

	if (stch_playstart_pending) {
		stch_playstart_pending = false;
		cdtv_dbg("STCH play-start (playing=%d motor=%d)", cd_playing, cd_motor);
		cdtv_inject_stch();
	}

	// Retry STCH inject on a slow cadence until BIOS progresses past STATUS
	// polling. See stch_retries comment for why a single pulse isn't enough.
	if (stch_retries > 0 && CheckTimer(stch_next_ms)) {
		cdtv_inject_stch();
		stch_retries--;
		// Fast cadence while waiting for a play-end ack (win the int-churn
		// race quickly); slow cadence for the post-mount boot retry.
		stch_next_ms = GetTimer(stch_wait_ack ? STCH_PLAYEND_PERIOD_MS
		                                      : STCH_RETRY_PERIOD_MS);
	}

	// CDDA streaming pump — one sector per FIFO-ready tick. The pump
	// runs alongside command processing; status reads in cdtv_audio_fifo_ready
	// share the same SPI cmd 0x63 the CR-511 dispatch already uses.
	cdtv_cdda_pump();

	// Terminate the play-end STCH retry the moment the BIOS takes it. One SPI
	// read of the sticky bridge flag, and only while a retry is outstanding --
	// the old trace-ring scan cost 9 reads per entry on every poll.
	if (stch_wait_ack && cdtv_stch_acked()) {
		stch_wait_ack = false;
		stch_retries  = 0;
		cdtv_dbg("STCH play-end ACKed (bridge flag) -- retry stopped");
	}

	// Drain whatever cmd bytes the bridge has accumulated this tick. The
	// status word's bit 6 (cdtv_req) tracks ~cmd_in_empty in the bridge,
	// so it stays HIGH as long as there are bytes left. Cap iterations to
	// guard against runaway loops if the FPGA is misbehaving.
	for (int guard = 0; guard < 32; guard++) {
		uint16_t status = cdtv_read_status();
		if (!(status & CDTV_STATUS_REQ)) break;

		uint8_t b = cdtv_drain_byte();

		if (cmd_idx == 0) {
			cmd_need = cr511_command_length(b);
			if (cmd_need < 0) {
				// Unknown opcode — log and silently drop the byte rather
				// than try to accumulate something we don't know the length
				// of (would block the FIFO).
				cdtv_dbg("drop unknown opcode %02x", b);
				cmd_idx = 0;
				cmd_need = 0;
				continue;
			}
		}

		if (cmd_idx < CDTV_CMD_MAX) {
			cmd_buf[cmd_idx++] = b;
		}

		if (cmd_idx >= cmd_need) {
			cdtv_dispatch();
		}
	}
}
