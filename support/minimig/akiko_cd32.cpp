// CD32 Akiko native-mode bridge — Main_MiSTer side (M3 MVP).
//
// Mirrors the WinUAE Akiko CD command interpreter (akiko.cpp @ SHA 2c7f8581)
// in just enough fidelity to boot Cannon Fodder. Talks to the FPGA bridge in
// rtl/akiko_hps_bridge.v via UIO class 0xF400 (io_din[15:9] == 7'b1111_010,
// see hps_ext.v:127). Polled once per frame from user_io.cpp.
//
// Protocol (per hps_ext.v):
//   Status poll: spi_w(0x63) → bit[11] = akiko_req (command framed & ready).
//   Read txn :   spi8(0x62); spi32_w(0xF400); { byte = spi_w(0); }*; DisableIO()
//                ends txn → FPGA pulses cmd_done (releases command framer).
//   Write txn:   spi8(0x61); spi32_w(0xF400); { spi_w(byte); }*; DisableIO()
//                ends txn → FPGA pulses result_done (kicks RX DMA).
// Read and write must NEVER be mixed in the same EnableIO/DisableIO scope.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdbool.h>
#include <sys/stat.h>     // mkdir() for /media/fat/saves/Minimig
#include <errno.h>
#include <unistd.h>       // unlink, fsync, close
#include <fcntl.h>        // open, O_RDONLY, posix_fadvise, POSIX_FADV_WILLNEED
#include <time.h>         // clock_gettime for akiko_diag timestamp
#include <byteswap.h>     // bswap_16 for CDDA big-endian → host conversion

#include "../../spi.h"
#include "../../fpga_io.h"     // fpga_spi_fast_block_write_8 (M5+ SECTOR_RD fast path)
#include "../../user_io.h"
#include "../../ide.h"
#include "../../ide_cdrom.h"
#include "../../hardware.h"   // GetTimer / CheckTimer
#include "../../menu.h"       // menu_present() — OSD-open save trigger
#include "../chd/mister_chd.h" // mister_chd_read_sector for CDDA pump
#include "minimig_config.h"   // minimig_reset() — used to unblock BIOS stuck
                              // on no-CD splash after a boot-with-no-CD-then-mount.
#include "akiko_cd32.h"
#include "chipset_trace.h"    // chipset_trace_drain() — gfx-trio debug
#include "z2_trace.h"         // z2_trace_drain() — 2026-05-27 Z2-hang debug

// -----------------------------------------------------------------------------
// Debug gating
// -----------------------------------------------------------------------------
// Forward declaration so the debug macro can use it before its definition.
static void akiko_diag(const char *fmt, ...);

// Verbose per-command/per-status diag (TX/RX byte dumps, status change
// chatter). Costly: every line is a synchronous fopen/fwrite/fclose to
// tmpfs, and once gameplay starts CF generates 200+ of these per second.
// Off in production. (The poll-loop throttle in akiko_cd32_poll is what
// was historically masked by this logging — see comment there.)
#define AKIKO_CD32_DEBUG 0
#if AKIKO_CD32_DEBUG
	// Route through akiko_diag so output reaches /tmp/akiko_dbg.log instead
	// of stdout, which Minimig redirects/silences after init.
	#define akiko_dbg(fmt, ...) akiko_diag("[akiko] " fmt, ##__VA_ARGS__)
#else
	#define akiko_dbg(...) do { } while (0)
#endif

// Bus-trace ring drain. Independent flag because the per-entry logging
// dwarfs everything else (>90% of log volume during gameplay). HOWEVER —
// the drain itself MUST run every poll regardless: the trace ring in
// akiko_hps_bridge appears to back-pressure the CPU when full, so an
// undrained ring stalls CF on dense bus activity (e.g. C2P traffic
// during early boot — observed: CF stuck at lba 33 vs lba 25k+ with
// drain enabled). When this flag is 0 we still drain, just silently.
#define AKIKO_BUS_TRACE 0

// -----------------------------------------------------------------------------
// WinUAE-derived constants (akiko.cpp)
// -----------------------------------------------------------------------------

// Bridge address class (matches hps_ext.v:127 → akiko_cs).
#define AKIKO_BRIDGE_ADDR  0xF400

// CDDA audio FIFO sub-channel (Phase 33). hps_ext.v:148 selects cdda_cs on
// io_din[15:9] == 7'b1111001 → 0xF200. Each UIO write byte is one half of
// a 16-bit sample word; cdda.v internally pairs up consecutive 16-bit
// writes into stereo frames {right,left} via its LRCK toggle.
// FIFO depth backpressure surfaces in the status word as cdda_req (bit 8):
// HIGH = FIFO has room for at least one full audio sector (cdda.v:79,
// WRITE_REQ <= AVAILABLE_COUNT >= SECTOR_SIZE = 588 stereo samples).
#define AKIKO_CDDA_ADDR        0xF200
#define AKIKO_CDDA_BYTES       2352      // raw red-book audio frame
#define AKIKO_STATUS_CDDA_REQ  (1u << 8) // hps_ext.v:167, cdda_req

// Status-poll cmd byte. Returns one 16-bit word; bit[11] = akiko_req,
// bit[10] = akiko_sec_req (M4 PBX wants a sector pushed).
#define AKIKO_STATUS_CMD             0x63
#define AKIKO_STATUS_REQ             (1u << 11)
#define AKIKO_STATUS_SEC_REQ         (1u << 10)
// Phase 18: bit[9] = akiko_rx_busy = (cdrom_receive_length != 0). Mirror of
// WinUAE's cdrom_can_return_data() gate: when set, the FPGA RX engine still
// has a queued/in-flight response and we must NOT push another frame, or it
// gets dropped (overwritten in result_buffer before the framer drains it).
#define AKIKO_STATUS_RX_BUSY (1u << 9)

// Sub-channel selector inside the 0xF400 class: io_din[8] = 1 selects the
// sector channel (hps_ext.v:135, akiko_cs_sec). 0xF400 | 0x100 = 0xF500.
#define AKIKO_SECTOR_ADDR  0xF500

// Subcode push sub-channel (io_din[4] = 1 -> akiko_cs_subcode in hps_ext.v).
// 0xF400 | 0x10 = 0xF410. Write-only: Main pushes one 96-byte INTERLEAVED P-W
// subchannel block per CD frame during CDDA play; the FPGA DMAs it to
// (addressmisc|0x100) and raises CDINT_SUBCODE (gated on CDFLAG_SUBCODE). See
// research/docs/subcode-streaming-design-2026-05-30.md.
#define AKIKO_SUBCODE_ADDR 0xF410
#define AKIKO_SUB_ENTRY    12     // SUB_ENTRY_SIZE (one subchannel = 12 bytes)
#define AKIKO_SUB_BYTES    96     // SUB_CHANNEL_SIZE (8 channels x 12)

// Phase 32: NVRAM save-dump sub-channel. io_din[6] = 1 selects the NVRAM
// host port (akiko_hps_bridge.v + akiko_nvram.v). 0xF400 | 0x40 = 0xF440.
// Reading streams 1024 bytes (auto-incrementing internal addr counter,
// resets on cs rise). Any write here pulses host_clear_dirty.
#define AKIKO_NVRAM_ADDR              0xF440
#define AKIKO_NVRAM_BYTES             1024
#define AKIKO_NVRAM_DIR               "/media/fat/saves/Minimig"
// Pre-Phase-32.5.1 single-file fallback. Used only when the dirty-debounce
// poll path tries to save and we have no per-game CD path active (e.g. the
// player wrote to EEPROM before any CD got mounted, or the path-setter hook
// hasn't fired yet). After 32.5.1 normal use writes to per-game files
// `cd32-<hash>.nvr` in AKIKO_NVRAM_DIR; legacy `cd32.nvr` is read on first
// load only as a one-shot migration when no per-game file exists.
#define AKIKO_NVRAM_FILE_LEGACY       "/media/fat/saves/Minimig/cd32.nvr"
#define AKIKO_STATUS_NVR_DIRTY        (1u << 7)   // hps_ext bit 7
// Throttle: poll the dirty bit at most once per second; once seen, wait
// this long for the dirty state to stabilize before saving (debounces
// bursty BIOS FlashFile commits, which may take multiple I2C writes).
#define AKIKO_NVRAM_POLL_PERIOD_MS    1000
// 30s debounce: BIOS often re-writes the same FlashFile entries multiple
// times within a few seconds during a save sequence. Idempotent-write
// guard already drops byte-identical re-saves, but we still pay the SPI
// dump cost (~50 ms). Bumping the debounce from 5s reduces dump churn
// from ~12/min worst case to ~2/min, which matters for SD-card wear and
// for the power-cut window during fsync.
#define AKIKO_NVRAM_DIRTY_DEBOUNCE_MS 30000
// Minimum interval between two consecutive saves to disk. Even if the
// BIOS keeps making distinct dirty-burst sequences, we won't write to SD
// faster than once per N ms. Protects against pathological save loops.
#define AKIKO_NVRAM_MIN_SAVE_INTERVAL_MS 60000

// Trace sub-channel: io_din[7] = 1 selects the akiko_bus_trace ring buffer
// (hps_ext.v: akiko_cs_trace). Each entry is 4 bytes:
//   byte 0: bit7 = 1 if write / 0 if read; bits6:0 = addr[7:1] within the
//           akiko window ($B80000-$B800FE in 2-byte stride)
//   byte 1: data[7:0]
//   byte 2: data[15:8]
//   byte 3: 0xFF if entry valid, 0x00 if ring empty (stop draining)
#define AKIKO_TRACE_ADDR  0xF480

// 2026-05-28 DDR peek sub-channel: io_din[5] = 1 selects akiko_ddr_peek's
// ring buffer (bridge→DDR3 writes). 8 bytes per entry:
//   byte 0: {addr[7:1], 1'b0}
//   byte 1: addr[15:8]
//   byte 2: addr[23:16]
//   byte 3: {U, L, 1'b0, addr[28:24]}
//   byte 4: data[7:0]
//   byte 5: data[15:8]
//   byte 6: 0xA5 sentinel (helps debug if drain alignment slips)
//   byte 7: 0xFF if valid, 0x00 if ring empty (stop draining)
#define AKIKO_PEEK_ADDR  0xF420

// DEBUG counter-drain sub-channel (Deep Core garble localization, 2026-06-05).
// UIO class 0xF400 with io_din[3]=1. Drain returns a fixed 13-byte payload:
// [0]=0xC5 magic, then little-endian dbg_fill_sum, dbg_commit_cnt, dbg_commit_sum
// (the FPGA's per-sector fill checksum and authoritative SDRAM chip-commit
// count + byte-sum). See rtl/hps_ext.v + rtl/sdram_ctrl.v.
#define AKIKO_DBG_ADDR   0xF408

// PBX sector size (raw Mode-1/Mode-2 frame).
#define AKIKO_SECTOR_BYTES 2352

// Per-opcode command lengths *excluding* the trailing checksum byte.
// akiko.cpp:1136 — entries < 0 are "reserved/unsupported".
static const int command_lengths[16] = {
	1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 2, -1, -1, -1, -1, -1
};

// Status / error nibbles. Values are byte-exact mirrors of WinUAE
// akiko.cpp:433-449 — getting these wrong is what kept the no-CD splash
// from rendering: BIOS distinguishes "no disc, drive ready" (0xF8|door =
// 0xF9) from "bad command" (0x80) by exact bit pattern. Returning 0x81
// for no-disc (the wrong NODISK value 0x80 | door 0x01) made BIOS think
// the drive was reporting an unknown command and re-poll forever.
#define CH_ERR_OK          0x00
#define CH_ERR_BADCOMMAND  0x80
#define CH_ERR_NODISK      0xf8
#define CH_ERR_CHECKSUM    0x88
#define CDS_ERROR          0x80
#define CDS_PLAYING        0x08
#define CDS_PLAYEND        0x00

// Drive firmware string returned by INFO (opcode 0x07). Exactly 18 chars,
// matches akiko.cpp:176 #define FIRMWAREVERSION "CHINON  O-658-2 24".
#define AKIKO_FIRMWARE     "CHINON  O-658-2 24"

// Max bytes we can possibly drain in a single command frame: largest table
// entry is 12 (opcode 0x04) plus the checksum = 13. Round up to 32 because
// the FPGA's buffer is 32 bytes and we're paranoid about tooling churn.
#define AKIKO_CMD_MAX      32

// -----------------------------------------------------------------------------
// Module state
// -----------------------------------------------------------------------------

// Auto-init handshake (akiko.cpp:1388-1397):
//   0 = drive cold, host hasn't seen media yet.
//   1 = we sent the {0x0a, present} media-status response.
//   2 = host sent INFO (0x07); fully initialised.
static uint8_t  cd_initialized      = 0;
static uint8_t  cd_paused           = 0;
static uint8_t  cd_playing          = 0;       // unused in M3, set by 0x04 (audio)
static uint8_t  cd_led_state        = 0;
// WinUAE akiko.cpp:500 defaults cdrom_door=1 and never clears it. The
// "door" byte is actually a status mask whose bit 0 = CHERR_DISKPRESENT
// (AROS chinon.h:63). With cd_door=0 every INFO/STATUS/STOP/PAUSE/MULTI
// response advertises "no disk present" -- the CD32 BIOS then halts at
// the spinning-CD splash polling CDINTREQ forever, which is exactly what
// we observed. Initial value MUST be 1.
static uint8_t  cd_door             = 1;
static uint32_t cd_play_start_lba   = 0;       // recorded for M4 (real audio/data)
static uint32_t cd_play_end_lba     = 0;

// M4 PBX state: cd_data_lba_base is set when cmd 0x04 is issued in DATA mode
// (cmd[7] bit 7 = 1). On each FPGA sec_req, we read the FPGA's sector_counter
// and fetch LBA = base + counter.
//   -1                  = no data read armed (sec_req pushes zeros to keep PBX flowing)
//   any other negative  = armed at a pre-gap start_msf (e.g. DotC arms at
//                         start_lba=-34 to stream across pre-gap into track 1).
//                         The per-sector push path silences sectors with
//                         (base+counter) < 0 and serves CHD data once the
//                         counter advances enough that the absolute LBA is
//                         non-negative. Earlier code skipped the entire
//                         burst on negative base, which stranded BIOSes
//                         that depend on streaming across pre-gap.
static int32_t  cd_data_lba_base    = -1;

// PBX prefetch cache (mirrors WinUAE akiko.cpp:1589-1658). Without it, every
// sec_req is a fresh CHD seek + hunk decode (~600us+ per sector). With it, a
// cache miss reads 128 sectors in a batch and subsequent reads in that window
// are pure memcpy.
//
// Multi-window LRU. Single-window thrashes on titles that interleave reads
// from 3+ regions (Sim City alternates LBAs 868/1066/1265 and ran at 39%
// hit ratio with one window). 4 windows is the sweet spot: Sim City 99%,
// Microcosm 95%, no regression on sequential readers. Bumping to 8 was
// tested 2026-05-10 — gave +0.7% on Sim City but cost 1-2% on Microcosm/CF
// (more thinly-distributed evictions) and didn't fix streamers like Litil
// Divil (working set >8 regions, mostly read-once). Stay at 4. ~1.18 MiB
// BSS on the host (1 GB DDR3) is negligible.
#define AKIKO_PREFETCH_SECTORS 128
#define AKIKO_PREFETCH_WINDOWS 4

typedef struct {
	int32_t  base_lba;     // first LBA covered, -1 = empty
	uint32_t lru_tick;     // higher = more recently used
	uint8_t  valid[AKIKO_PREFETCH_SECTORS];
	uint8_t  buf[AKIKO_PREFETCH_SECTORS * AKIKO_SECTOR_BYTES];
} prefetch_window_t;

static prefetch_window_t cd_prefetch_windows[AKIKO_PREFETCH_WINDOWS];
static uint32_t cd_prefetch_lru_counter = 0;
static uint32_t cd_prefetch_hits = 0;
static uint32_t cd_prefetch_misses = 0;
static uint32_t cd_prefetch_failed_sectors = 0;

static inline void akiko_prefetch_invalidate(void)
{
	for (int i = 0; i < AKIKO_PREFETCH_WINDOWS; i++) {
		cd_prefetch_windows[i].base_lba = -1;
	}
}

// Returns window index containing lba, or -1 if not cached.
static int akiko_prefetch_find(uint32_t lba)
{
	for (int i = 0; i < AKIKO_PREFETCH_WINDOWS; i++) {
		const prefetch_window_t *w = &cd_prefetch_windows[i];
		if (w->base_lba >= 0
		    && lba >= (uint32_t)w->base_lba
		    && lba <  (uint32_t)w->base_lba + AKIKO_PREFETCH_SECTORS) {
			return i;
		}
	}
	return -1;
}

// Pick eviction target: empty window if any, else oldest by lru_tick.
// Wraparound at 4 billion accesses is benign (one comparison flips at the
// boundary; no correctness impact).
static int akiko_prefetch_pick_evict(void)
{
	int pick = 0;
	for (int i = 0; i < AKIKO_PREFETCH_WINDOWS; i++) {
		if (cd_prefetch_windows[i].base_lba < 0) return i;
		if (cd_prefetch_windows[i].lru_tick < cd_prefetch_windows[pick].lru_tick) {
			pick = i;
		}
	}
	return pick;
}

// Audio-play notification state (mirror of WinUAE cdrom_audiotimeout,
// akiko.cpp:1411-1435). cmd_multi acks PLAY AUDIO synchronously with 0x42
// ("play starting"), but BIOS won't advance past the audio-cued state until
// it sees the asynchronous opcode-0x04 follow-up frame. Values:
//    2,1 -> countdown to "play started" emission (CDS_PLAYING|2)
//   -1   -> stop audio engine (placeholder until Phase 33), advance to -2
//   -2   -> emit "play ended" (CDS_PLAYEND)
//   -3   -> emit "play failed" (CDS_ERROR)
static int8_t   cd_audio_timeout    = 0;

// Phase 32.6 P4: wall-clock deadline for the natural play_ended notification.
// Superseded by Phase 33's position-based pump (cd_cdda_lba_next/end) when a
// CDDA pump is actually running — the pump fires playend_notify when it
// finishes pushing the last sector. Kept as a safety net for failure paths
// where the pump was never armed (e.g. cd_find_drive() returned NULL but the
// caller has already advertised 0x42); 0 = inactive.
static uint32_t cd_audio_play_until_ms = 0;

// Phase 33 — CDDA streaming pump state.
//   cd_cdda_lba_next : next absolute LBA to read from the CHD. -1 = idle.
//   cd_cdda_lba_end  : exclusive end LBA (one past the last sector to push).
//   cd_cdda_drv      : drive captured at PLAY-arm time. Held across the run
//                      so a CD swap mid-play deterministically aborts when
//                      the read fails rather than reading from the new disc.
//                      NULL = idle.
// All three are kept in lockstep — set together in cmd_multi and cleared
// together in cmd_stop / pump natural-end / failure paths.
static int32_t cd_cdda_q_pos    = -1;   // SUBQ: last sector played (survives natural end)
static int32_t cd_cdda_q_end    = -1;   // SUBQ: play-end LBA for WinUAE end-clamp
static int32_t cd_cdda_lba_next = -1;
static int32_t cd_cdda_lba_end  = -1;
static drive_t *cd_cdda_drv     = NULL;

// Last mounted state — used to re-arm the auto-init when a disc is swapped.
static bool     cd_last_mounted     = false;

// Phase 33-E flag: when set, the next akiko_cd32_poll iteration will call
// minimig_reset() (deferred from the mediachange handler so the edge-
// detection code runs to completion first). Used only for the boot-with-
// no-CD-then-mount recovery path; hot-swaps during gameplay never set it.
static bool     g_akiko_pending_minimig_reset = false;

// One-shot: push a fresh media-status frame on the next poll *after* INFO
// has completed. Mimics WinUAE's mediachanged-still-set-after-init quirk
// (akiko.cpp:1399 fires once cd_initialized reaches 2 if mediachanged was
// set at boot). Without this, BIOS gets the LED+INFO acks but never sees
// the second media_status that would unblock its post-init MULTI scan.
static uint8_t  cd_post_info_media_push_pending = 0;

// TOC streaming state. After cmd_info completes we proactively push TOC
// entries (cmd 0x06 / cdrom_return_toc_entry) one per poll until the BIOS
// has seen each point TOC_REPEAT times. WinUAE pushes one per video frame
// (akiko.cpp:1438-1440). Without this, CD32 BIOS sits at the spinning-CD
// splash forever — it never sends MULTI/READ until TOC is known.
//   AKIKO_TOC_MAX_POINTS = 0xA0 + 0xA1 + 0xA2 + up to 99 tracks. The original
//   M5 cap of 16 was sized for Cannon Fodder (2 tracks → 5 points). Bumped
//   to 32 on 2026-05-07 after Fire & Ice (23-track multi-CDDA disc) was
//   observed truncating its TOC to 13 audio points and wedging post-scan-TOC.
//   32 covers everything in the user's collection (Fire & Ice has the most
//   tracks at 23; AKIKO_TOC_REPEAT=3 still keeps the drip under 2s @ 20ms/frame).
#define AKIKO_TOC_REPEAT       3
#define AKIKO_TOC_PUSH_PERIOD_MS 20 // 50 Hz, matches WinUAE PAL framesync (akiko.cpp:1438)
#define AKIKO_TOC_MAX_POINTS   32
static uint8_t  toc_buffer[AKIKO_TOC_MAX_POINTS * 13];
static uint8_t  toc_point_count     = 0;
static int16_t  toc_push_idx        = -1;   // -1 = idle; else next slot in 3x sequence
static uint32_t toc_push_last_ms    = 0;    // wall-clock ms of last push (Phase 33-B)

// Phase 32.5.1: per-game NVRAM. cd_save_path_active is the full path of the
// per-CD save file currently mirrored in FPGA BRAM (`""` when no CD is
// mounted). cd_save_load_pending is set by akiko_cd32_set_cd_path() (CD
// swap) or akiko_cd32_init() (Minimig core reconfig wiped BRAM) and cleared
// by the poll loop after the load actually fires. cd_save_dirty_observed
// tracks whether we've ever seen the FPGA dirty flag asserted since the
// current save was loaded — used to skip the pre-swap flush when the player
// never touched EEPROM (avoids overwriting a real save with the synthesized
// FlashFile init from a different CD).
static char cd_save_path_active[256] = {0};
static bool cd_save_load_pending     = false;
static bool cd_save_dirty_observed   = false;

// Per-slot guard: set true when akiko_nvram_load_from_path's verify pass
// fails for cd_save_path_active. Cleared when set_cd_path swaps to a new
// slot, or when init() runs and the load is re-armed. While set,
// akiko_nvram_save_to_disk refuses to overwrite the on-disk file — BRAM
// is not a faithful mirror of disk after a failed load (BIOS will see
// the .mif baseline and "rebuild" the FlashFile bootstrap, which our
// dirty-detect would otherwise capture and persist over the user's real
// save). See known-issues-deferred.md "NVRAM disk persistence" section.
static bool cd_save_load_failed      = false;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

// FNV-1a 64-bit. Stable across runs/builds, no endian concerns. Used to map
// a CD basename to a 16-hex-char save filename slot.
static uint64_t fnv1a_64(const char *s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	while (*s) {
		h ^= (uint64_t)(uint8_t)*s++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

// Build `<AKIKO_NVRAM_DIR>/cd32-<hash>.nvr` from the basename of `cd_path`.
// We hash the basename (not the full path) so the same CHD in a different
// folder maps to the same save slot. Comparison is case-sensitive — Linux
// FS, and CD32 CHDs are typically named in a stable case anyway.
static void compute_save_path(const char *cd_path, char *out, size_t outsz)
{
	const char *base = strrchr(cd_path, '/');
	base = base ? base + 1 : cd_path;
	uint64_t h = fnv1a_64(base);
	snprintf(out, outsz, "%s/cd32-%016" PRIx64 ".nvr", AKIKO_NVRAM_DIR, h);
}

static inline uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
	// Standard CDDA MSF → LBA conversion with 2-second pre-gap.
	return (((uint32_t)m * 60u + s) * 75u + f) - 150u;
}

static inline uint8_t bcd_to_bin(uint8_t b)
{
	return (uint8_t)(((b >> 4) * 10u) + (b & 0x0fu));
}

static inline uint8_t bin_to_bcd(uint8_t v)
{
	return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

// "Is real CD media inserted?" — matches WinUAE's `cdrom_disk != NULL` semantic.
// A placeholder drive (cfg=2, no filename) has present=0/cd=1 with chd_f=NULL,
// f=NULL — that's "drive ready, no media", which WinUAE treats as no-disk for
// cmd_multi/pause/unpause. Checking present+cd alone would let MULTI fall into
// the scan-TOC branch and then PLAY AUDIO with a junk seekpos, trapping BIOS
// in the UNPAUSE polling loop and never letting it render the splash.
static bool cd_is_mounted(void)
{
	for (int p = 0; p < 2; p++) {
		for (int d = 0; d < 2; d++) {
			drive_t *drv = &ide_inst[p].drive[d];
			if (drv->cd && (drv->chd_f || drv->f)) {
				return true;
			}
		}
	}
	return false;
}

// Find the first CD drive that has real media. NULL if no drive has media.
static drive_t *cd_find_drive(void)
{
	for (int p = 0; p < 2; p++) {
		for (int d = 0; d < 2; d++) {
			drive_t *drv = &ide_inst[p].drive[d];
			if (drv->cd && (drv->chd_f || drv->f)) {
				return drv;
			}
		}
	}
	return NULL;
}

// Read the bridge status word. Mirrors ide_check() but kept private to avoid
// clobbering the IDE poll's read of the same word.
static uint16_t akiko_read_status(void)
{
	uint16_t res;
	EnableIO();
	res = spi_w(AKIKO_STATUS_CMD);
	if (!res) res = (uint8_t)spi_w(0);
	DisableIO();
	return res;
}

// Forward decl: cmd_subq (early in dispatch table) reads the FPGA sector
// counter via the helper defined later in the M4 PBX block.
static uint8_t akiko_read_sec_counter(void);

// Active-poll barrier: read status repeatedly until the masked bit matches
// `want_set`, or `max_iters` reached. Each iteration is a SPI status read
// (~µs), so this is a microsecond-scale wait that completes as soon as the
// FPGA actually shows the expected state — vs a fixed usleep which waits
// the worst case every time. Returns true if the expected state was seen.
static bool akiko_wait_status_bit(uint16_t mask, bool want_set, int max_iters)
{
	for (int i = 0; i < max_iters; i++) {
		uint16_t s = akiko_read_status();
		if (((s & mask) != 0) == want_set) return true;
	}
	return false;
}

// -----------------------------------------------------------------------------
// Bridge transactions
// -----------------------------------------------------------------------------

// Drain up to one framed command from the bridge. Returns the number of bytes
// read. The first byte (opcode) tells us how many bytes the rest of the frame
// is — we read exactly cmd_length+1 (incl. checksum). For unknown opcodes we
// fall back to reading the maximum to drain whatever the FPGA has buffered.
static int akiko_drain_command(uint8_t *buf)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(AKIKO_BRIDGE_ADDR);

	// First byte = opcode. We have to issue at least one read to know how
	// many follow.
	buf[0] = (uint8_t)spi_w(0);

	int op = buf[0] & 0x0f;
	int payload_len = command_lengths[op];   // bytes following the opcode? No:
	                                         // table is *total* command bytes
	                                         // INCLUDING opcode, EXCLUDING chk.
	int total;
	if (payload_len < 0) {
		// Unknown opcode — drain the full FPGA buffer so we don't leave stale
		// bytes around for the next frame. The bridge clamps rd_ptr at 32.
		total = AKIKO_CMD_MAX;
	} else {
		total = payload_len + 1;             // +1 for trailing checksum byte
	}
	if (total > AKIKO_CMD_MAX) total = AKIKO_CMD_MAX;

	for (int i = 1; i < total; i++) {
		buf[i] = (uint8_t)spi_w(0);
	}

	DisableIO();

	// Barrier: wait until the FPGA's TX engine clears its REQ bit. Without
	// this the next status read at top of poll can still show REQ=1 (stale)
	// and we'd re-drain garbage as a phantom command. Bounded to 200 iters
	// (~200µs); typical case is 1-3.
	akiko_wait_status_bit(AKIKO_STATUS_REQ, false, 200);

	return total;
}

// 2026-05-28: drain the akiko_ddr_peek ring (bridge→DDR3 writes). One entry
// is 8 bytes, last byte 0x00 marks ring-empty. Each entry is decoded into
// (addr[28:0], U, L, data) and logged via akiko_diag. Used to verify that
// the bytes userspace pushes via UIO_DMA_WRITE actually land in DDR3 at the
// expected Z2 addresses with the expected data.
#if AKIKO_Z2_TRACE
static int akiko_ddr_peek_drain_and_log(const char *tag)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(AKIKO_PEEK_ADDR);
	int count = 0;
	for (int safety = 0; safety < 64; safety++) {
		uint8_t b[8];
		for (int i = 0; i < 8; i++) b[i] = (uint8_t)spi_w(0);
		if (b[7] == 0x00) break;        // ring empty
		uint32_t addr =  (uint32_t)b[0]
		              | ((uint32_t)b[1] << 8)
		              | ((uint32_t)b[2] << 16)
		              | (((uint32_t)(b[3] & 0x1F)) << 24);
		uint8_t  u    = (b[3] >> 7) & 1;
		uint8_t  l    = (b[3] >> 6) & 1;
		uint16_t data = (uint16_t)b[4] | ((uint16_t)b[5] << 8);
		akiko_diag("[akiko] DDR-peek %s[%d]: addr=$%08x UL=%d%d data=$%04x%s",
		           tag, count, addr, u, l, data,
		           (b[6] != 0xA5) ? " (BAD SENTINEL)" : "");
		count++;
	}
	DisableIO();
	if (count == 0) {
		akiko_diag("[akiko] DDR-peek %s: ring empty (no bridge writes captured)", tag);
	}
	return count;
}
#endif  // AKIKO_Z2_TRACE

// Append the trailing one-byte checksum (akiko.cpp:838-842) and push the whole
// payload to the FPGA via UIO_DMA_WRITE. The FPGA's RX engine DMAs the bytes
// to chip RAM and asserts the CD32 IRQ when result_done strobes.
static void akiko_send_response(const uint8_t *payload, int len)
{
	if (len < 1 || len > AKIKO_CMD_MAX) {
		akiko_dbg("send_response: bad len %d\n", len);
		return;
	}

	uint8_t out[AKIKO_CMD_MAX + 1];
	memcpy(out, payload, (size_t)len);

	uint32_t sum = 0;
	for (int i = 0; i < len; i++) sum += out[i];
	out[len] = (uint8_t)(0xff - (sum & 0xff));

	int total = len + 1;

	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(AKIKO_BRIDGE_ADDR);
	for (int i = 0; i < total; i++) {
		spi_w(out[i]);                       // low 8 bits = byte, high = 0
	}
	DisableIO();

	// Barrier: wait until the FPGA's RX engine registers our payload by
	// raising rx_busy. Without this, the next status read at top of poll
	// can return rx_busy=0 (stale), and the gating logic for auto-init /
	// TOC drip / NVR save would push another response on top of this one.
	// Bounded to 200 iters (~200µs of SPI reads); typical case is 1-3.
	//
#if AKIKO_Z2_TRACE
	// 2026-05-27 Z2 hang diag: count iters and log whether rx_busy was
	// observed. If rx_busy never rose, either the SPI push didn't trigger
	// result_done in akiko_hps_bridge, or the RX engine isn't draining
	// our payload into chip RAM (Z2-mode hang suspect).
	int wait_iters = 0;
	bool rx_seen = false;
	for (; wait_iters < 200; wait_iters++) {
		uint16_t s = akiko_read_status();
		if (s & AKIKO_STATUS_RX_BUSY) { rx_seen = true; break; }
	}
	// 2026-05-27 Z2 hang diag (v2): also poll for rx_busy to CLEAR — proves the
	// FPGA RX engine actually drained the result_buffer into chip RAM via
	// chipdma_arb's bridge-write path. If rx_busy stays set forever, RX is
	// stuck (bridge writes to Z2 cdrx_address not completing).
	int drain_iters = 0;
	bool rx_drained = false;
	if (rx_seen) {
		for (; drain_iters < 50000; drain_iters++) {
			uint16_t s = akiko_read_status();
			if (!(s & AKIKO_STATUS_RX_BUSY)) { rx_drained = true; break; }
		}
	}
	akiko_diag("[akiko] SEND op=0x%02x len=%d rx_busy=%s iters=%d drain=%s drain_iters=%d",
	           out[0] & 0x0F, len, rx_seen ? "set" : "TIMEOUT", wait_iters,
	           rx_drained ? "ok" : "STUCK", drain_iters);

	// 2026-05-28: drain the DDR-peek ring after the RX engine claims it
	// finished writing to Z2. Only log for op=0x07 (INFO) — that's the
	// frame the CD32 BIOS hangs on after parsing the CMD. Other opcodes
	// would just spam the log without informing the hypothesis.
	if ((out[0] & 0x0F) == 0x07) {
		akiko_ddr_peek_drain_and_log("INFO-RX");
	}
#else
	akiko_wait_status_bit(AKIKO_STATUS_RX_BUSY, true, 200);
#endif  // AKIKO_Z2_TRACE

#if AKIKO_CD32_DEBUG
	akiko_dbg("TX %d bytes:", total);
	for (int i = 0; i < total; i++) printf(" %02x", out[i]);
	printf("\n");
#endif
}

// -----------------------------------------------------------------------------
// TOC build & push (akiko.cpp:758-786, 956-978, 1437-1440)
// -----------------------------------------------------------------------------

static void toc_pack_entry(int point, uint8_t control, uint32_t msf_or_track)
{
	if (toc_point_count >= AKIKO_TOC_MAX_POINTS) return;
	uint8_t *d = &toc_buffer[toc_point_count * 13];
	memset(d, 0, 13);
	d[1] = 1u | ((uint8_t)control << 4);              // adr=1, control in high nibble
	d[3] = (point < 100) ? bin_to_bcd((uint8_t)point) : (uint8_t)point;
	if (point == 0xA0 || point == 0xA1) {
		// "MSF" actually carries the first/last track number in the M slot.
		d[8] = bin_to_bcd((uint8_t)msf_or_track);
	} else {
		uint32_t lsn = msf_or_track + 150u;           // standard 2-second pre-gap
		uint32_t mins = (lsn / 75u) / 60u;
		uint32_t secs = (lsn / 75u) % 60u;
		uint32_t fr   = lsn % 75u;
		d[8]  = bin_to_bcd((uint8_t)mins);
		d[9]  = bin_to_bcd((uint8_t)secs);
		d[10] = bin_to_bcd((uint8_t)fr);
	}
	toc_point_count++;
}

static void akiko_build_toc(void)
{
	toc_point_count = 0;
	toc_push_idx    = -1;

	drive_t *drv = cd_find_drive();
	if (!drv || drv->track_cnt < 2) {
		akiko_diag("[akiko] TOC build SKIP (no drive or no tracks)");
		return;
	}

	int real_tracks = drv->track_cnt - 1;             // last entry is lead-out
	if (real_tracks < 1 || real_tracks > 99) {
		akiko_diag("[akiko] TOC build SKIP (real_tracks=%d)", real_tracks);
		return;
	}

	// Phase 24 diag: dump every track's raw fields so we can see what the
	// CHD parser actually populated.
	for (int i = 0; i <= real_tracks; i++) {
		akiko_diag("[akiko] TRACK[%d] num=%u attr=0x%02x start=%u length=%u chd_off=%u",
		           i, drv->track[i].number, drv->track[i].attr,
		           drv->track[i].start, drv->track[i].length,
		           drv->track[i].chd_offset);
	}

	// Use track 1's data/audio attribute for the 0xA0/0xA1/0xA2 entries.
	uint8_t first_ctrl = (drv->track[0].attr & 0x40) ? 0x04 : 0x00;
	toc_pack_entry(0xA0, first_ctrl, 1);
	toc_pack_entry(0xA1, first_ctrl, real_tracks);
	toc_pack_entry(0xA2, first_ctrl, drv->track[real_tracks].start);

	for (int i = 0; i < real_tracks; i++) {
		uint8_t ctrl = (drv->track[i].attr & 0x40) ? 0x04 : 0x00;
		toc_pack_entry(drv->track[i].number, ctrl, drv->track[i].start);
	}

	// Phase 19: do NOT auto-arm the drip. WinUAE's akiko_handler only emits
	// TOC frames when cdrom_toc_counter >= 0, and that counter is set to 0
	// only inside cdrom_command_multi() at line 1096 — i.e. after BIOS issues
	// MULTI cmd 0x04 with the negative-MSF "scan TOC" sentinel. Auto-arming
	// here (and in cmd_info) caused the drip to run forever, BIOS to consume
	// 700k entries, and never frame LED=1/PLAY because it was stuck in TOC-
	// scan mode. We keep the build (it's cheap) but leave toc_push_idx = -1.
	toc_push_last_ms = 0;
	akiko_diag("[akiko] TOC built: %u points (%d real tracks, lead-out lba=%u)",
	           toc_point_count, real_tracks, drv->track[real_tracks].start);
}

// Push one TOC entry frame. Returns true if a frame was sent (caller should
// then `return` from the poll so we don't double-action this tick).
static bool akiko_push_toc_entry(void)
{
	if (toc_push_idx < 0) return false;
	int point_idx = toc_push_idx / AKIKO_TOC_REPEAT;
	// Phase 19: hard-stop after points*REPEAT pushes, matching WinUAE
	// akiko.cpp:974-976. Looping forever kept BIOS in TOC-scan mode and
	// blocked progression to LED=1/PLAY — BIOS treats counter=-1 as "TOC
	// transmission complete" and only then advances state.
	if (point_idx >= toc_point_count) {
		toc_push_idx = -1;
		akiko_diag("[akiko] TOC drip complete (%u points * %d repeat = %d frames)",
		           toc_point_count, AKIKO_TOC_REPEAT,
		           toc_point_count * AKIKO_TOC_REPEAT);
		return false;
	}
	uint8_t r[15];
	memset(r, 0, sizeof(r));
	r[0] = 0x06;                                       // cmd opcode echo
	r[1] = 0x0a;                                       // "unknown but real CD32 sets it"
	memcpy(r + 2, &toc_buffer[point_idx * 13], 13);
	int counter = toc_push_idx;
	// Phase 19: match WinUAE akiko.cpp:971-973 byte-for-byte. WinUAE does
	// NOT take r[7] mod 100 — it lets BCD wrap naturally past 99. Removed
	// the %100 to avoid drifting after counter/75 >= 76.
	r[6] = bin_to_bcd(99);
	r[7] = bin_to_bcd((uint8_t)(24u + (uint32_t)counter / 75u));
	r[8] = bin_to_bcd((uint8_t)((uint32_t)counter % 75u));
	akiko_send_response(r, 15);
	akiko_diag("[akiko] TOC push idx=%d point_idx=%d point=0x%02x ctrl=0x%02x msf=%02x:%02x:%02x",
	           toc_push_idx, point_idx, r[5], (r[3] >> 4) & 0x0f, r[10], r[11], r[12]);
	toc_push_idx++;
	return true;
}

// -----------------------------------------------------------------------------
// Per-opcode command handlers
// -----------------------------------------------------------------------------

// 0x07 — INFO/STATUS. akiko.cpp:940-954.
// Response = 20 bytes: { cmd, door, FIRMWAREVERSION[18] }.
static void cmd_info(const uint8_t *cmd)
{
	uint8_t r[20];
	r[0] = cmd[0];
	r[1] = cd_door;
	memcpy(&r[2], AKIKO_FIRMWARE, 18);       // exactly 18 chars, no NUL
	akiko_send_response(r, 20);
	cd_initialized = 2;
	akiko_dbg("INFO -> initialized=2\n");
	// Build TOC eagerly (cheap), but DON'T start streaming. WinUAE's BIOS
	// asks for TOC via MULTI in PLAY mode with seekpos<0 — see cmd_multi.
	akiko_build_toc();
	// Phase 21: removed auto-arm of TOC drip and post-INFO media push.
	// Subagent diff vs WinUAE akiko.cpp:940-954 (cdrom_command_status / INFO)
	// shows WinUAE does NOTHING after building the TOC — it just bumps
	// cd_initialized to 2 and waits for BIOS's next command. The TOC drip
	// counter is set to 0 in EXACTLY ONE place (akiko.cpp:1096), inside
	// cdrom_command_multi when MULTI 0x04 is issued with seekpos<0 (the
	// "scan TOC" sentinel). Our cmd_multi at line 502 already does this.
	// Phase 19.5's auto-arm sent BIOS 15 unsolicited cmd 0x06 frames it
	// never asked for, which corrupted its rxinx/rxcmp FSM and caused it
	// to go silent after the drip completed. Removing it should let BIOS
	// proceed straight from INFO to MULTI 0x04 (data read of boot sector).
}

// Async play-state notification — WinUAE cdrom_playend_notify (akiko.cpp:
// 1110-1121). Emitted by the poll loop after a MULTI audio command, never
// in synchronous response to a command frame.
//   status  0 = started   -> CDS_PLAYING | 0x02 | door
//   status  1 = ended     -> CDS_PLAYEND | door  (==door since CDS_PLAYEND=0)
//   status -1 = failed    -> CDS_ERROR   | door
static void emit_playend_notify(int status)
{
	uint8_t r[2];
	r[0] = 0x04;
	if (status < 0)        r[1] = CDS_ERROR;
	else if (status == 0)  r[1] = CDS_PLAYING | 0x02;
	else                   r[1] = CDS_PLAYEND;
	r[1] |= cd_door;
	akiko_send_response(r, 2);
	akiko_dbg("PLAYEND_NOTIFY status=%d -> %02x\n", status, r[1]);
}

// 0x01 — STOP. akiko.cpp:989-1003.
static void cmd_stop(const uint8_t *cmd)
{
	uint8_t r[2];
	r[0] = cmd[0];
	r[1] = cd_is_mounted() ? 0x00 : (CH_ERR_NODISK | cd_door);
	cd_playing = 0;
	cd_paused = 0;
	cd_data_lba_base = -1;                       // cancel any data-mode read
	// Phase 33: tear down any in-flight CDDA pump. The FIFO will drain
	// naturally over the next ~13 ms (588 stereo samples remaining at
	// most). No need to flush — RTL just stops getting refills and the
	// AUDIO_L/R outputs go to silence on FIFO empty.
	cd_cdda_lba_next = -1;
	cd_cdda_lba_end  = -1;
	cd_cdda_drv      = NULL;
	// If a play-started ack is still pending from a prior MULTI audio that
	// just got STOPped, swallow it. Conversely, a play-ended timeout (-2/-1)
	// is a legitimate "engine just finished" signal and stays armed so BIOS
	// sees the natural end-of-play frame.
	if (cd_audio_timeout > 0) cd_audio_timeout = 0;
	cd_audio_play_until_ms = 0;                  // P4: cancel pending natural end
	akiko_send_response(r, 2);
	akiko_dbg("STOP\n");
}

// 0x02 — PAUSE. WinUAE akiko.cpp:1005-1024. checkerr() at line 1014 returns
// CH_ERR_NODISK | cdrom_door if no disc. We were returning 0 in that case,
// telling BIOS "drive idle, fine" instead of "no disc, waiting for media".
static void cmd_pause(const uint8_t *cmd)
{
	uint8_t r[2];
	r[0] = cmd[0];
	if (!cd_is_mounted()) {
		r[1] = CH_ERR_NODISK | cd_door;
	} else {
		r[1] = (cd_playing ? CDS_PLAYING : 0) | cd_door;
	}
	// 2026-05-15 WinUAE-parity fix (akiko.cpp:1012): PAUSE stops the TOC
	// drip immediately. CR2 issues PAUSE while the post-cmd-info scan-TOC
	// drip is still pushing frames; without this, we keep firing 0x06-
	// shaped async frames at BIOS during pause, leaving BIOS in a stale
	// "TOC mode" state that prevents it from re-enabling CDFLAG_ENABLE on
	// the next PLAY DATA — the bridge then sits with sec_req silent for
	// 57+ seconds until BIOS times out, by which time the boot has hung.
	toc_push_idx     = -1;
	toc_push_last_ms = 0;
	cd_paused = 1;
	akiko_send_response(r, 2);
	akiko_dbg("PAUSE (playing=%d, mounted=%d)\n", cd_playing, cd_is_mounted());
}

// 0x03 — UNPAUSE. WinUAE akiko.cpp:1027-1044. Same checkerr() pattern.
static void cmd_unpause(const uint8_t *cmd)
{
	uint8_t r[2];
	r[0] = cmd[0];
	if (!cd_is_mounted()) {
		r[1] = CH_ERR_NODISK | cd_door;
	} else {
		r[1] = (cd_playing ? CDS_PLAYING : 0) | cd_door;
	}
	cd_paused = 0;
	akiko_send_response(r, 2);
	akiko_dbg("UNPAUSE (playing=%d, mounted=%d)\n", cd_playing, cd_is_mounted());
}

// 0x04 — PLAY/READ. akiko.cpp:1257ff.
//   cmd[1..3] = start MSF (BCD), cmd[4..6] = end MSF (BCD).
//   cmd[7] bit 7: 1 = data read, 0 = audio play.
//   cmd[8] bit 6: 1 = 2x CD speed, 0 = 1x (WinUAE akiko.cpp:1055).
// We *record* the LBAs but do not actually start audio/data — that's M4.
// cdrom_speed is recorded for future seek-time simulation; we do not currently
// throttle to the requested speed (we deliver faster than 2x already).
static int cdrom_speed = 1;  // 1x or 2x; updated each cmd_multi

static void cmd_multi(const uint8_t *cmd)
{
	uint8_t r[2];
	r[0] = cmd[0];

	int new_speed = (cmd[8] & 0x40) ? 2 : 1;
	if (new_speed != cdrom_speed) {
		akiko_diag("[akiko] cdrom_speed change %dx -> %dx (cmd8=0x%02x)",
		           cdrom_speed, new_speed, cmd[8]);
		cdrom_speed = new_speed;
	}

	if (!cd_is_mounted()) {
		r[1] = 0x01;                         // "no disk" code in the play branch (matches WinUAE akiko.cpp:1059)
		akiko_send_response(r, 2);
		akiko_dbg("PLAY: no disk\n");
		return;
	}

	// WinUAE uses signed msf2lsn — values before pre-gap (MSF 00:00:00)
	// produce a negative LSN which the BIOS uses as the "scan TOC" sentinel
	// in the PLAY branch. Match by checking the raw MSF before the lossy
	// uint subtract.
	uint32_t s_msf_total = (((uint32_t)bcd_to_bin(cmd[1]) * 60u
	                       + bcd_to_bin(cmd[2])) * 75u + bcd_to_bin(cmd[3]));
	bool seek_negative = (s_msf_total < 150u);
	uint32_t s_lba = s_msf_total - 150u;          // lossy when seek_negative
	uint32_t e_lba = msf_to_lba(bcd_to_bin(cmd[4]), bcd_to_bin(cmd[5]), bcd_to_bin(cmd[6]));

	cd_play_start_lba = s_lba;
	cd_play_end_lba   = e_lba;

	// cmd[7] bit 7 = data read. Match WinUAE akiko.cpp:1063 — bit 7 only.
	// Earlier code also accepted bit 6 (0x40) as data_read on a speculative
	// CF observation, but log analysis (63,728 PLAY DATA arms in CF boot)
	// shows zero bit-6-only arms; CF always sets bit 7. Bit 6 is unspecified
	// in the CD32 Akiko surface and accepting it as data_read would
	// misclassify any future bit-6 use (likely a scan/seek flag).
	bool data_read = (cmd[7] & 0x80) != 0;
	if (data_read) {
		// Audio-track refusal. CR2/HQ2/Microcosm 2026-05-08: BIOS issues
		// PLAY DATA on LBAs that fall inside audio tracks (CR2 LBA 37224,
		// HQ2 LBA 25667 in pregap then 49810 in track 2 body, Microcosm
		// 19213-area). Pushing real audio bytes produces a 4 ms garbage-
		// CMD storm; pushing silence with counter advance feeds the BIOS
		// file walker zeros that get re-encoded as a next-extent pointer;
		// holding sec_req deadlocks BIOS.
		//
		// Synchronous refusal with CDS_PLAYEND mirrors WinUAE's natural
		// play-end notification — BIOS sees the play has ended, treats
		// the requested file as truncated, and walks to the next dirent.
		// CDS_ERROR alone makes HQ2 retry the same LBA forever.
		//
		// Pregap classification: 150 frames before a track's body start
		// belong to that track per MMC. Without explicit handling, LBAs
		// in the gap (HQ2 25663-25812) fall through both per-track
		// ranges, prefetch returns silence, BIOS reads zeros as a dirent
		// extent and arms PLAY DATA at a garbage LBA.
		drive_t *drv2 = cd_find_drive();
		bool start_is_audio = false;
		bool start_is_oob   = false;
		// seek_negative => s_msf_total < 150, so s_lba underflowed to a huge
		// uint32 (e.g. msf 00:01:54 → s_lba = 4294967275). The classification
		// below compares as uint32 and would trip the OOB branch on the wrap,
		// silently refusing a legitimate pre-gap PLAY DATA. Skip classification
		// in that case and arm the read with a SIGNED negative cd_data_lba_base;
		// akiko_handle_sec_req's per-sector signed-LBA path skip-pushes the
		// pre-gap sectors and serves real data once the counter crosses zero.
		// (Deep Core 2026-05-09: BIOS issued PLAY DATA at start_lba=4294967275
		// = -21 and the OOB branch refused it silently. Confirmed in log:
		// "PLAY DATA REFUSED oob start_lba=4294967275 (cmd7=0x80) — silent".)
		if (!seek_negative && drv2) {
			int real_tracks2 = drv2->track_cnt > 0 ? drv2->track_cnt - 1 : 0;
			uint32_t lead_out2 = (real_tracks2 > 0) ? drv2->track[real_tracks2].start : 0;
			if (lead_out2 && s_lba >= lead_out2) {
				start_is_oob = true;
			}
			for (int i = 0; i < real_tracks2 && !start_is_oob; i++) {
				uint32_t body_end = drv2->track[i].start + drv2->track[i].length;
				if (s_lba >= drv2->track[i].start && s_lba < body_end) {
					start_is_audio = !(drv2->track[i].attr & 0x40);
					break;
				}
				// Pregap region between this track's body end and next
				// track's start. Belongs to the NEXT track per MMC, so
				// classify by the next track's attr.
				if (i + 1 < real_tracks2 &&
				    s_lba >= body_end && s_lba < drv2->track[i + 1].start) {
					start_is_audio = !(drv2->track[i + 1].attr & 0x40);
					break;
				}
			}
		}
		if (start_is_audio) {
			// AUDIO refusal: CDS_ERROR causes HQ2 BIOS to retry the same
			// LBA forever (~36 k retries observed). CDS_PLAYEND mirrors
			// WinUAE's natural-end notification: BIOS sees "play ended",
			// treats the requested file as truncated/empty, and walks to
			// the next directory entry.
			cd_data_lba_base = -1;
			r[1] = CDS_PLAYEND | cd_door;
			akiko_diag("[akiko] PLAY DATA REFUSED audio start_lba=%u (cmd7=0x%02x)",
			           s_lba, cmd[7]);
		} else if (start_is_oob) {
			// OOB refusal: mirror WinUAE exactly (akiko.cpp:1078-1082 for
			// data plays + 1595 is_valid_data_sector + 1361-1362 skip-push).
			// WinUAE returns 0x02 (play started) synchronously regardless
			// of LBA validity, then declines to push PBX bytes for invalid
			// sectors. BIOS waits for PBX, eventually times out internally,
			// and re-reads the parent dirent rather than walking the
			// corrupt linked list.
			//
			// Earlier attempt: CDS_PLAYEND synchronously. That works for
			// AUDIO but creates a tight walk-loop on OOB — BIOS sees
			// "play ended", abandons, and tries the NEXT corrupt dirent
			// pointer immediately. Fast-CHD boots stay clean (BIOS never
			// goes OOB), but slow-CHD boots burn 9 k+ refusals/120 s
			// without recovery.
			cd_data_lba_base = -1;
			r[1] = 0x02;
			akiko_diag("[akiko] PLAY DATA REFUSED oob start_lba=%u (cmd7=0x%02x) — silent (no PBX, no playend)",
			           s_lba, cmd[7]);
		} else {
			// M4: arm the PBX sector fetcher. cdrom_sector_counter on the FPGA
			// is reset to 0 on CDFLAG_ENABLE rising (akiko.cpp:1973-1976), so
			// LBA = base + counter holds across the full read pass.
			cd_data_lba_base = (int32_t)s_lba;
			// 2026-05-15 WinUAE-parity fix: cmd_multi at akiko.cpp:1054 sets
			// cdrom_paused = 0 unconditionally on entry. Our scan-TOC and
			// audio-play branches do this, but the data branch was missing it.
			// CR2 fails the sequence scan-TOC → PAUSE → PAUSE → PLAY DATA
			// because cd_paused stays 1 from the pause; bridge CDDA pump is
			// gated by it. (The earlier broader patch that also cleared
			// cd_playing/cd_cdda_lba_next/end broke CR2 by tearing down audio
			// state — only cd_paused needs flipping here.)
			cd_paused = 0;
			r[1] = 0x02;
			// 2026-06-05 Deep Core counter-drift diagnostic: log the FPGA
			// cdrom_sector_counter AT ARM TIME. If the BIOS issues successive
			// READ DATA without re-pulsing CDFLAG_ENABLE, the counter is NOT
			// reset between reads, so LBA=base+counter delivers base+leftover
			// (wrong sector) — deterministic, matches the read-#3 divergence.
			akiko_diag("[akiko] PLAY DATA arm: start_lba=%d (cmd7=0x%02x) fpga_ctr@arm=%u",
			           (int32_t)s_lba, cmd[7], akiko_read_sec_counter());
		}
	} else if (seek_negative) {
		// PLAY with seekpos < 0 = "scan TOC" trigger (akiko.cpp:1095-1097).
		// Start streaming TOC entries to the BIOS one frame at a time.
		// Phase 23: WinUAE sets r[1] = 0 for scan-TOC (default from line 1057),
		// not 0x42. The 0x42 "play started" status is reserved for actual
		// audio play (seekpos >= 0, line 1099). Our 0x42 here was telling
		// BIOS "play has started" which conflicts with the TOC scan that
		// follows — likely keeping BIOS in audio-play state machine instead
		// of letting it transition to data-read after the TOC.
		cd_data_lba_base = -1;
		cd_playing = 0;
		cd_paused = 0;
		toc_push_idx = (toc_point_count > 0) ? 0 : -1;
		toc_push_last_ms = 0;
		r[1] = 0x00;                              // scan-TOC: no play-started flag
		akiko_diag("[akiko] MULTI scan-TOC trigger (points=%u)", toc_point_count);
	} else {
		// PLAY AUDIO. Synchronous ack (0x42 = "play starting") first; the
		// poll loop emits the asynchronous "play started" follow-up via the
		// cd_audio_timeout state machine. BIOS gates audio-cued game
		// progress on the async frame — without it, titles that start a
		// CDDA cue (Banshee, Speris Legacy, JP3) hang.
		//
		// Phase 33: arm the CDDA streaming pump. cd_cdda_lba_next/end and
		// cd_cdda_drv form the pump's working set; the poll loop's
		// akiko_cdda_pump() then pushes one sector per FIFO-ready tick and
		// fires playend_notify(1) on natural end via cd_audio_timeout = -1.
		cd_data_lba_base = -1;
		cd_playing = 1;
		cd_paused = 0;
		r[1] = 0x42;
		cd_audio_timeout = 2;

		// Validate: drive present, non-empty range, range falls inside an
		// audio track (cd_read_audio_sector probes the track table on each
		// read; we sanity-check the start LBA up front so an invalid PLAY
		// fails synchronously via cdrom_audiotimeout = -3 → playend_notify
		// (-1), matching WinUAE akiko.cpp:1432-1435).
		drive_t *drv = cd_find_drive();
		bool valid = false;
		if (drv && e_lba > s_lba) {
			// Probe the start LBA only; the rest of the range will be
			// caught by the pump's per-sector validation. This is cheap
			// (no SPI, just a read of the in-memory track table via the
			// same logic cd_read_audio_sector uses).
			int real_tracks = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
			for (int i = 0; i < real_tracks; i++) {
				uint32_t end = drv->track[i].start + drv->track[i].length;
				if (s_lba >= drv->track[i].start && s_lba < end) {
					if (!(drv->track[i].attr & 0x40)) valid = true;
					break;
				}
			}
		}

		if (valid) {
			// Idempotent re-PLAY guard (Liberation/Captive II, Deep Core
			// 2026-05-30): these titles poll PAUSE (op=0x02) then re-issue an
			// IDENTICAL PLAY AUDIO (op=0x04) every ~0.88 s. Unconditionally
			// re-arming cd_cdda_lba_next to s_lba rewound the pump to the track
			// start each poll, so the track never advanced (looping music). If
			// a play is already in progress for the same drive+range and the
			// pump position is still inside [s_lba,e_lba), leave cd_cdda_lba_next
			// alone so playback continues; only re-arm on a new/idle range.
			bool same_range_inflight =
				(cd_cdda_drv == drv) &&
				(cd_cdda_lba_end == (int32_t)e_lba) &&
				(cd_cdda_lba_next >= (int32_t)s_lba) &&
				(cd_cdda_lba_next <  (int32_t)e_lba);
			cd_cdda_drv      = drv;
			if (!same_range_inflight) {
				cd_cdda_lba_next = (int32_t)s_lba;
				cd_cdda_lba_end  = (int32_t)e_lba;
				cd_cdda_q_pos    = (int32_t)s_lba;
			}
			cd_cdda_q_end = (int32_t)e_lba;
			// Wall-clock fallback no longer needed — pump fires playend
			// from natural-end detection on its own. Leave armed at 0.
			cd_audio_play_until_ms = 0;
			if (same_range_inflight) {
				akiko_dbg("CDDA re-PLAY same range s=%u e=%u next=%d kept\n",
				          s_lba, e_lba, cd_cdda_lba_next);
				akiko_diag("[akiko] CDDA re-PLAY same range s_lba=%u e_lba=%u next=%d (position kept)",
				           s_lba, e_lba, cd_cdda_lba_next);
			} else {
				akiko_dbg("CDDA arm s=%u e=%u\n", s_lba, e_lba);
				akiko_diag("[akiko] CDDA arm: s_lba=%u e_lba=%u (drv=%p)",
				           s_lba, e_lba, (void*)drv);
			}
		} else {
			// Bad range or wrong track type — schedule asynchronous
			// failure notify. The synchronous 0x42 ack is already in r[]
			// (sent below); the pump stays idle and the timeout walks
			// 2 → 1 → emit "started", then we override with -3 below to
			// also send the failure frame. Simpler: skip the "started"
			// sequence entirely by going straight to -3.
			cd_cdda_drv      = NULL;
			cd_cdda_lba_next = -1;
			cd_cdda_lba_end  = -1;
			cd_audio_timeout = -3;
			cd_audio_play_until_ms = 0;
			cd_playing       = 0;
			akiko_diag("[akiko] CDDA arm INVALID: drv=%p s_lba=%u e_lba=%u",
			           (void*)drv, s_lba, e_lba);
		}
	}

	akiko_send_response(r, 2);
	akiko_dbg("PLAY %s start=%u(%s) end=%u\n",
		data_read ? "DATA" : "AUDIO", s_lba, seek_negative ? "neg" : "pos", e_lba);
}

// 0x05 — LED control. cmd[1] bit 7 set means "respond with new state".
//   Form A (bit7 set): 2-byte response { cmd, led_state }.
//   Form B (bit7 clr): 1-byte response { cmd } (just an ack).
static void cmd_led(const uint8_t *cmd)
{
	uint8_t r[2];
	r[0] = cmd[0];

	if (cmd[1] & 0x80) {
		cd_led_state = cmd[1] & 0x01;
		r[1] = cd_led_state;
		akiko_send_response(r, 2);
	} else {
		// Phase 21: WinUAE akiko.cpp:917-922 returns 0 here, dispatcher at
		// line 1288 calls set_status(DRIVEXMIT) which is "not used by ROM,
		// PIO mode" (akiko.cpp:416). Net effect = no buffer push, no IRQ.
		// We were pushing 1 byte + checksum = 2 bytes that BIOS read as
		// part of the next response, putting our RX buffer 2 bytes ahead
		// of BIOS's RXCMP tracking. Drop the response entirely.
	}
	akiko_dbg("LED cmd1=%02x state=%d\n", cmd[1], cd_led_state);
}

// 0x06 — SUBQ. WinUAE akiko.cpp:1124-1134 + cd_qcode at 706-755.
// Response layout (15 bytes):
//   r[0]    = cmd echo
//   r[1]    = 0
//   r[2..14] = 13-byte Q-code payload, of which 11 are used:
//     [2+0] reserved
//     [2+1] CtlAdr (control nibble in high, adr=1 in low)
//     [2+2] Track (BCD)
//     [2+3] Index (BCD, always 1 for our purposes)
//     [2+4..6] TrackPos M/S/F (BCD, relative to track start with 2-sec pre-gap)
//     [2+7] reserved
//     [2+8..10] DiskPos M/S/F (BCD, absolute with 2-sec pre-gap)
//     [2+11..12] reserved
// If qcode is invalid (no play / no read), set r[2+2]=0x80 sentinel and
// leave the rest zero — mirrors WinUAE's `qcode_valid==0` early-return.
static void cmd_subq(const uint8_t *cmd)
{
	uint8_t r[15];
	memset(r, 0, sizeof(r));
	r[0] = cmd[0];

	drive_t *drv = cd_find_drive();
	uint32_t cur_lba = 0;
	bool valid = false;

	if (drv) {
		if (cd_data_lba_base >= 0) {
			// Active data read: counter gives offset within the burst.
			cur_lba = (uint32_t)cd_data_lba_base + akiko_read_sec_counter();
			valid = true;
		} else if (cd_playing && cd_cdda_q_pos >= 0) {
			// Audio play (Phase 33): pump tracks the next-to-push LBA.
			// Report the position of the sector we *just* pushed
			// (cd_cdda_lba_next - 1) so qcode advances frame-by-frame
			// across the play range. WinUAE's qcode comes from the audio
			// renderer's frame counter — same effect.
			cur_lba = (uint32_t)cd_cdda_q_pos;
			// WinUAE akiko.cpp:742-750 "end of disc position is not missed":
			// within ~10 frames of the play end, clamp to the exact end so a
			// game polling SUBQ for end-of-audio (DotC) actually sees it.
			if (cd_cdda_q_end > 0 && (cd_cdda_q_end - (int32_t)cur_lba) < 10)
				cur_lba = (uint32_t)cd_cdda_q_end;
			valid = true;
		} else if (cd_playing && cd_play_start_lba > 0) {
			// Pump idle but cd_playing still set (e.g. mid-pump-arm
			// transition). Fall back to recorded start position.
			cur_lba = cd_play_start_lba;
			valid = true;
		}
	}

	if (!valid) {
		r[2 + 2] = 0x80;                    // qcode-invalid sentinel
		akiko_send_response(r, 15);
		return;
	}

	// Locate the track containing cur_lba (linear scan; track_cnt is small).
	int trk_idx = 0;
	int real_tracks = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
	for (int i = 0; i < real_tracks; i++) {
		uint32_t end = (i + 1 < drv->track_cnt) ? drv->track[i + 1].start
		                                        : drv->track[i].start + drv->track[i].length;
		if (cur_lba >= drv->track[i].start && cur_lba < end) {
			trk_idx = i;
			break;
		}
	}

	const track_t &t = drv->track[trk_idx];
	// CtlAdr: track attr already holds the control bits in the high nibble
	// (0x40 = data, 0x00 = audio). adr=1 in low nibble (current Q-mode).
	uint8_t ctl_adr = (uint8_t)((t.attr & 0xf0) | 0x01);

	// MSF positions, with 2-second pre-gap. Saturate trk_lsn at 0 if
	// cur_lba sits before the track start (shouldn't happen in steady
	// state but defensive).
	uint32_t trk_lsn  = (cur_lba >= t.start) ? (cur_lba - t.start) + 150u : 150u;
	uint32_t disk_lsn = cur_lba + 150u;

	uint32_t tm = (trk_lsn / 75u) / 60u;
	uint32_t ts = (trk_lsn / 75u) % 60u;
	uint32_t tf =  trk_lsn % 75u;
	uint32_t dm = (disk_lsn / 75u) / 60u;
	uint32_t ds = (disk_lsn / 75u) % 60u;
	uint32_t df =  disk_lsn % 75u;

	r[2 + 0] = 0;
	r[2 + 1] = ctl_adr;
	r[2 + 2] = bin_to_bcd((uint8_t)t.number);
	r[2 + 3] = bin_to_bcd(1);                       // Index 1
	r[2 + 4] = bin_to_bcd((uint8_t)tm);
	r[2 + 5] = bin_to_bcd((uint8_t)ts);
	r[2 + 6] = bin_to_bcd((uint8_t)tf);
	r[2 + 7] = 0;
	r[2 + 8] = bin_to_bcd((uint8_t)dm);
	r[2 + 9] = bin_to_bcd((uint8_t)ds);
	r[2 + 10] = bin_to_bcd((uint8_t)df);

	akiko_send_response(r, 15);
	akiko_dbg("SUBQ trk=%u lba=%u tpos=%u:%u:%u dpos=%u:%u:%u\n",
	          t.number, cur_lba, tm, ts, tf, dm, ds, df);
}

// Catch-all for unknown / reserved opcodes. akiko.cpp:1161-1191 style.
static void cmd_bad(const uint8_t *cmd, uint8_t err_code)
{
	uint8_t r[2];
	r[0] = (uint8_t)((cmd[0] & 0xf0) | 5);   // top nibble preserved, low = 5
	r[1] = err_code | cd_door;
	akiko_send_response(r, 2);
	akiko_dbg("BAD cmd=%02x err=%02x\n", cmd[0], err_code);
}

// -----------------------------------------------------------------------------
// M4 PBX sector channel
// -----------------------------------------------------------------------------

// Read the FPGA's current cdrom_sector_counter (1-byte read on the sec
// sub-channel, 0xF500). The bridge presents hps_sec_status on every read
// strobe; one word is enough.
static uint8_t akiko_read_sec_counter(void)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(AKIKO_SECTOR_ADDR);
	uint16_t w = spi_w(0);
	DisableIO();
	return (uint8_t)(w & 0xff);
}

// -----------------------------------------------------------------------------
// Phase 32: NVRAM save-dump (UIO 0xF440)
// -----------------------------------------------------------------------------
// Drain all 1024 NVRAM bytes from the FPGA via UIO_DMA_READ on the nvr
// sub-channel. The bridge presents bytes from an internal address counter
// that resets to 0 on cs rising and increments per uio_rd. host_dout has
// 1-cycle BRAM latency; cs is held many cycles before the first read so
// the first byte is valid by then.
static void akiko_nvram_dump(uint8_t *out_1024)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(AKIKO_NVRAM_ADDR);
	for (int i = 0; i < AKIKO_NVRAM_BYTES; i++) {
		out_1024[i] = (uint8_t)spi_w(0);
	}
	DisableIO();
}

// Canonical SD-block load path. user_io_file_mount registers the .nvr
// as virtual disk slot 0; hps_io fires img_mounted, the in-core FSM in
// Minimig.sv asserts sd_rd[0] with sd_lba=0, hps_io reads the file and
// streams every byte on sd_buff_dout / sd_buff_wr / sd_buff_addr, and
// Minimig.sv pipes that straight into akiko_nvram.load_we. Sidesteps
// the gp_out CDC freeze that broke the ioctl_download path on hardware.
//
// Userspace returns immediately after mounting — the actual BRAM fill
// happens asynchronously. Verify deferred to a poll-driven check below
// (akiko_nvram_verify_load_pending), once the FPGA-side FSM has had
// time to walk the LBA.
static uint32_t  nvr_verify_due_at_ms = 0;        // 0 = no verify pending
static char      nvr_verify_path[1024];

static bool akiko_nvram_load_from_path(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		akiko_diag("[akiko] NVR load skip: no file at %s (errno=%d)",
		           path, errno);
		return false;
	}
	if (st.st_size != AKIKO_NVRAM_BYTES) {
		akiko_diag("[akiko] NVR load skip: %s wrong size %lld (want %d)",
		           path, (long long)st.st_size, AKIKO_NVRAM_BYTES);
		return false;
	}

	// user_io_file_mount → UIO_SET_SDINFO + UIO_SET_SDSTAT over SPI.
	// hps_io drops a single-cycle img_mounted[0] pulse with img_size set;
	// Minimig.sv's edge-detect FSM kicks off the LBA-0 read on the next
	// clk_sys edge.
	int rc = user_io_file_mount(path, /*index*/0, /*pre*/0, /*pre_size*/0);
	if (rc != 1) {
		akiko_diag("[akiko] NVR mount FAIL: user_io_file_mount(%s, slot=0) rc=%d",
		           path, rc);
		cd_save_load_failed = true;
		return false;
	}

	// Verify delay: load takes ~10-20 ms over SPI (BLKSZ=3, 1024 B per
	// LBA, 4 user_io_poll iterations to fire the request once the
	// !is_minimig() gate in user_io.cpp was lifted). 100 ms is well
	// past completion while still catching the load before BIOS has
	// done meaningful FlashFile-bootstrap work.
	akiko_diag("[akiko] NVR mounted slot 0 (%d bytes from %s) — verify in 100 ms",
	           AKIKO_NVRAM_BYTES, path);
	cd_save_load_failed = false;
	strncpy(nvr_verify_path, path, sizeof(nvr_verify_path) - 1);
	nvr_verify_path[sizeof(nvr_verify_path) - 1] = '\0';
	nvr_verify_due_at_ms = (uint32_t)GetTimer(100);
	return true;
}

// Called from akiko_cd32_poll. When the deferred-verify timer expires,
// dump the BRAM via the existing save sub-channel and compare to what's
// on disk. Sets cd_save_load_failed if mismatch (which gates the
// dirty-detect → save path so we don't overwrite a real file with a
// broken load).
static void akiko_nvram_verify_load_pending(void)
{
	if (!nvr_verify_due_at_ms) return;
	if (!CheckTimer(nvr_verify_due_at_ms)) return;
	nvr_verify_due_at_ms = 0;

	uint8_t verify[AKIKO_NVRAM_BYTES];
	akiko_nvram_dump(verify);
	uint8_t expected[AKIKO_NVRAM_BYTES];
	FILE *f = fopen(nvr_verify_path, "rb");
	if (!f) {
		akiko_diag("[akiko] NVR verify: fopen(%s) failed errno=%d",
		           nvr_verify_path, errno);
		cd_save_load_failed = true;
		return;
	}
	size_t got = fread(expected, 1, AKIKO_NVRAM_BYTES, f);
	fclose(f);
	if (got != AKIKO_NVRAM_BYTES) {
		akiko_diag("[akiko] NVR verify: short read %zu/%d", got, AKIKO_NVRAM_BYTES);
		cd_save_load_failed = true;
		return;
	}
	if (memcmp(expected, verify, AKIKO_NVRAM_BYTES) == 0) {
		akiko_diag("[akiko] NVR loaded %d bytes from %s (verify OK)",
		           AKIKO_NVRAM_BYTES, nvr_verify_path);
		cd_save_load_failed = false;
		return;
	}
	int first_bad = -1, mismatches = 0;
	for (int i = 0; i < AKIKO_NVRAM_BYTES; i++) {
		if (verify[i] != expected[i]) {
			if (first_bad < 0) first_bad = i;
			mismatches++;
		}
	}
	akiko_diag("[akiko] NVR LOAD VERIFY FAILED: %d/%d mismatched (first @ 0x%03x)",
	           mismatches, AKIKO_NVRAM_BYTES, first_bad);
	int logged = 0;
	for (int i = 0; i < AKIKO_NVRAM_BYTES && logged < 32; i++) {
		if (verify[i] != expected[i]) {
			akiko_diag("[akiko]   @0x%03x: got 0x%02x want 0x%02x",
			           i, verify[i], expected[i]);
			logged++;
		}
	}
	cd_save_load_failed = true;
}

// Atomic save to a specific path: dump → write to .tmp → fsync → rename →
// unlink stale .tmp. Returns true on success. Failure paths log via
// akiko_diag and leave the dirty flag set (next poll re-tries). Note that
// akiko_nvram_dump on the bridge auto-clears the dirty flag at end of the
// read burst, so a successful save is naturally idempotent.
//
// Idempotent-write guard: if the dump matches the file already on disk
// byte-for-byte, skip the rewrite. BIOS often re-writes the same value
// (e.g. an entry-accessed flag that was already set), and we don't want
// to churn the SD card or update mtime for a no-op change.
static bool akiko_nvram_save_to_path(const char *path)
{
	uint8_t buf[AKIKO_NVRAM_BYTES];
	akiko_nvram_dump(buf);

	// Idempotent-write guard + bootstrap-overwrite guard. Read current file
	// (if any) and compare.
	//
	// (a) Idempotent: if dump matches disk byte-for-byte, skip rewrite.
	//     BIOS often re-writes the same value (entry-accessed flag etc.) and
	//     we don't want to churn the SD card or update mtime for a no-op.
	//
	// (b) Bootstrap-overwrite: if dump has ZERO meaningful content past the
	//     FlashFile root header (bytes 25..1023 all zero) AND the disk file
	//     has data there, refuse the write. This catches the destructive
	//     cascade observed 2026-05-04: BIOS rebuilds the empty FlashFile
	//     bootstrap (bytes 0..24 only) from scratch when it can't recognize
	//     the existing NVRAM contents → dirty-detect captures the rebuild
	//     → save would silently overwrite the user's real save with the
	//     bootstrap. Real games leave entry data in the 25..1023 region.
	{
		FILE *cur = fopen(path, "rb");
		if (cur) {
			uint8_t disk_buf[AKIKO_NVRAM_BYTES];
			size_t n = fread(disk_buf, 1, AKIKO_NVRAM_BYTES, cur);
			fclose(cur);
			if (n == AKIKO_NVRAM_BYTES) {
				if (memcmp(disk_buf, buf, AKIKO_NVRAM_BYTES) == 0) {
					akiko_diag("[akiko] NVR save skipped: identical to %s", path);
					return true;
				}
				// Bootstrap-overwrite check
				int buf_entry_nz  = 0;
				int disk_entry_nz = 0;
				for (int i = 25; i < AKIKO_NVRAM_BYTES; i++) {
					if (buf[i])      buf_entry_nz++;
					if (disk_buf[i]) disk_entry_nz++;
				}
				if (buf_entry_nz == 0 && disk_entry_nz > 0) {
					akiko_diag("[akiko] NVR save BLOCKED: about to overwrite "
					           "%s (%d entry bytes) with empty FlashFile bootstrap "
					           "(0 entry bytes) — destructive cascade detected, "
					           "refusing.",
					           path, disk_entry_nz);
					return false;
				}
			}
		}
	}

	// Ensure target directory exists. mkdir is fine if it already does
	// (EEXIST). Other errors surface during fopen.
	if (mkdir(AKIKO_NVRAM_DIR, 0755) != 0 && errno != EEXIST) {
		akiko_diag("[akiko] NVR mkdir(%s) failed: errno=%d", AKIKO_NVRAM_DIR, errno);
		// fall through — fopen below will report the real failure
	}

	char tmp_path[300];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	FILE *f = fopen(tmp_path, "wb");
	if (!f) {
		akiko_diag("[akiko] NVR fopen(%s) failed: errno=%d", tmp_path, errno);
		return false;
	}
	size_t wrote = fwrite(buf, 1, AKIKO_NVRAM_BYTES, f);
	int    flush = fflush(f);
	int    fsy   = fsync(fileno(f));
	int    cls   = fclose(f);
	if (wrote != AKIKO_NVRAM_BYTES || flush != 0 || fsy != 0 || cls != 0) {
		akiko_diag("[akiko] NVR write incomplete: wrote=%zu flush=%d fsync=%d close=%d",
		           wrote, flush, fsy, cls);
		unlink(tmp_path);
		return false;
	}

	if (rename(tmp_path, path) != 0) {
		akiko_diag("[akiko] NVR rename(%s -> %s) failed: errno=%d",
		           tmp_path, path, errno);
		unlink(tmp_path);
		return false;
	}

	akiko_diag("[akiko] NVR saved %d bytes to %s", AKIKO_NVRAM_BYTES, path);
	return true;
}

// Save wrapper used by the dirty-debounce path in poll. Resolves to the
// per-game save filename if a CD path is active; otherwise falls back to
// the legacy single-file slot so writes that happen with no CD context
// (shouldn't normally occur, but cheap insurance) aren't silently dropped.
//
// Load-failure guard: if the most recent load_from_path verify failed for
// this slot, BRAM is the .mif baseline rather than a faithful mirror of
// disk. Any BIOS write into that baseline is BIOS recreating its
// FlashFile bootstrap, NOT a real user save — persisting it would silently
// overwrite the user's real on-disk save with the empty bootstrap
// (observed 2026-05-04, confirmed via byte diff against pre-reset save).
// Refuse the write until set_cd_path or init re-arms the load.
static bool akiko_nvram_save_to_disk(void)
{
	if (cd_save_load_failed && cd_save_path_active[0]) {
		static bool warned_once = false;
		if (!warned_once) {
			akiko_diag("[akiko] NVR save BLOCKED: load failed for %s — refusing to "
			           "overwrite real save with BIOS bootstrap. Future blocks silent.",
			           cd_save_path_active);
			warned_once = true;
		}
		return false;
	}
	const char *target = (cd_save_path_active[0])
		? cd_save_path_active
		: AKIKO_NVRAM_FILE_LEGACY;
	return akiko_nvram_save_to_path(target);
}

// Push 2352 bytes to the akiko sector_buffer. Two paths coexist:
//
//   Default: UIO_SECTOR_RD on slot AKIKO_SEC_SLOT, then
//   fpga_spi_fast_block_write_8(buf, 2352). hps_io's b_wr<<1 pipeline drives
//   sd_buff_wr / sd_buff_addr / sd_buff_dout per byte at SPI clock without
//   needing ACK round-trips. akiko.v captures via sd_ack[AKIKO_SEC_SLOT] gate;
//   sector_ready latches when sd_buff_addr hits 2351. Measured ~0.95 ms/sector
//   (hardware A/B, 2026-05-07).
//
//   AKIKO_SLOW_PUSH=1 (escape hatch): UIO_DMA_WRITE on 0xF500 (sec sub-channel
//   of 0xF400). Per-byte SSPI_ACK round-trip → ~2.3 ms/sector. Bridge ends the
//   transfer with hps_sec_done; akiko.v latches sector_ready iff every byte
//   landed (sec_wr_ptr == 2352). Kept as a fallback in case a future regression
//   needs to A/B against the slow path without rebuilding.
//
// The fast path needs the matching RTL (Minimig.sv VDNUM=2, akiko_sec_dma_*
// wiring) — without it, sd_ack[1] never asserts and bytes go nowhere. Bundled
// shipped RBFs (May 2026 onward) include this RTL.
#define AKIKO_SEC_SLOT 1   // hps_io disk slot wired to akiko.v's hps_sec_dma_*
static const bool g_akiko_fast_push = (getenv("AKIKO_SLOW_PUSH") == nullptr);

static void akiko_push_sector(const uint8_t *buf)
{
	if (g_akiko_fast_push) {
		EnableIO();
		spi_w(UIO_SECTOR_RD | (AKIKO_SEC_SLOT << 8));
		fpga_spi_fast_block_write_8(buf, AKIKO_SECTOR_BYTES);
		DisableIO();
	} else {
		EnableIO();
		spi8(UIO_DMA_WRITE);
		spi32_w(AKIKO_SECTOR_ADDR);
		for (int i = 0; i < AKIKO_SECTOR_BYTES; i++) {
			spi_w(buf[i]);
		}
		DisableIO();
	}
}

// -----------------------------------------------------------------------------
// Phase 33 — CDDA streaming (UIO 0xF200, FIFO into rtl/cdda.v)
// -----------------------------------------------------------------------------

// Read one 2352-byte raw audio sector from the CHD into buf. Returns true on
// success, false on any failure (no drive, no chd handle, LBA outside any
// track, LBA inside a non-audio track, CHD read error). Failure leaves buf
// untouched; the caller is expected to push silence.
//
// The CHD parser tags audio tracks with sector_size = 2352 and
// attr & 0x40 == 0 (mister_chd.cpp:135-141). For audio sectors the CHD
// stores the raw 2352 bytes verbatim (big-endian per 16-bit sample, the
// canonical red-book layout). We fetch with mister_chd_read_sector using
// the per-track chd_offset because CHD pads each track to 4-sector
// boundaries — see ide_cdrom.cpp:1143 for the same pattern in the data
// path. Re-implemented here rather than calling cdrom_read_raw_sector
// because that helper (a) zero-fills audio tracks (attr != 0 path
// at line 1865-1898 of ide_cdrom.cpp is for data) and (b) we want to
// validate "is this actually audio" before reading.
static bool cd_read_audio_sector(drive_t *drv, uint32_t lba, uint8_t *buf2352)
{
	if (!drv || !drv->chd_f || !buf2352) return false;

	bool index0 = false;
	track_t *track = NULL;
	int real_tracks = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
	for (int i = 0; i < real_tracks; i++) {
		uint32_t end = drv->track[i].start + drv->track[i].length;
		if (lba >= drv->track[i].start && lba < end) {
			track = &drv->track[i];
			break;
		}
		// inside the index-0 pregap of the next track (lba < its start
		// but covered by the previous track's data): we don't try to be
		// clever for CDDA, just clamp to the owning track if any.
		if (i + 1 < real_tracks && lba < drv->track[i + 1].start &&
		    lba >= end) {
			track = &drv->track[i];
			index0 = true;
			break;
		}
	}
	(void)index0;

	if (!track) return false;
	// attr & 0x40 == 0 → audio (CDDA). Refuse to play data tracks as audio.
	if (track->attr & 0x40) return false;
	if (track->sectorSize != AKIKO_CDDA_BYTES) return false;

	uint32_t chd_lba = lba + track->chd_offset;
	if (mister_chd_read_sector(drv->chd_f, chd_lba, 0, 0,
	                           AKIKO_CDDA_BYTES, buf2352,
	                           drv->chd_hunkbuf, &drv->chd_hunknum)
	    != CHDERR_NONE) {
		return false;
	}
	return true;
}

// Read the bridge status word and check the cdda_req bit. HIGH means the
// FIFO has room for at least one full sector and we can safely push 2352
// bytes without backpressure stalls on the SPI side.
static bool akiko_audio_fifo_ready(void)
{
	return (akiko_read_status() & AKIKO_STATUS_CDDA_REQ) != 0;
}

// Push 2352 bytes of raw audio to the CDDA FIFO via UIO_DMA_WRITE on the
// cdda sub-channel (0xF200). 2352 bytes = 588 stereo frames × 2 channels
// × 2 bytes/sample. CD audio is stored big-endian per 16-bit sample
// (high byte first); after byte-swapping to host order each 16-bit value
// is one channel sample.
//
// Bridge wiring (hps_ext.v:143, 229-235):
//   cdda_dout <= io_din;           // 16-bit, full word from each spi_w
//   cdda_wr   <= cdda_cs;          // pulses HIGH for one cycle per spi_w
//                                  // when cmd == 0x61 and byte_cnt >= 3
//
// cdda.v then walks LRCK on each rising edge of WRITE (cdda_wr):
//   - First spi_w  → DATA = DIN (left sample)
//   - Second spi_w → BUFFER[WRITE_ADDR] = {DIN, DATA} = {right, left}
//                    (AUDIO_L drives BUFFER_Q[15:0], AUDIO_R drives [31:16])
//
// So we send the LEFT sample as the first 16-bit word, then RIGHT, etc.
// Each spi_w transmits the full 16-bit sample value (the FPGA's io_din is
// 16 bits wide).
static void akiko_push_audio_sector(const uint8_t *buf2352)
{
	const uint16_t *src = (const uint16_t *)buf2352;
	const int nframes = AKIKO_CDDA_BYTES / 4;   // 588 stereo frames

	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(AKIKO_CDDA_ADDR);
	for (int i = 0; i < nframes; i++) {
		uint16_t l = bswap_16(src[2 * i + 0]);  // CHD big-endian → host
		uint16_t r = bswap_16(src[2 * i + 1]);
		spi_w(l);                                // LRCK 0 → DATA = left
		spi_w(r);                                // LRCK 1 → BUFFER = {right,left}
	}
	DisableIO();
}

// Build the deinterleaved 96-byte subchannel for `lba`: all zero except the
// Q channel (bytes 12..23), per WinUAE getsub regenerate
// (blkdev_cdimage.cpp:336). Track-relative MSF carries no pre-gap; absolute
// MSF includes the 2-second (150-frame) pre-gap, matching lsn2msf().
static void build_subcode_deint(drive_t *drv, uint32_t lba, uint8_t out[AKIKO_SUB_BYTES])
{
	memset(out, 0, AKIKO_SUB_BYTES);
	if (!drv) return;

	int trk_idx = 0;
	int real_tracks = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
	for (int i = 0; i < real_tracks; i++) {
		uint32_t end = (i + 1 < drv->track_cnt) ? drv->track[i + 1].start
		                                        : drv->track[i].start + drv->track[i].length;
		if (lba >= drv->track[i].start && lba < end) { trk_idx = i; break; }
	}
	const track_t &t = drv->track[trk_idx];

	uint8_t *q = out + AKIKO_SUB_ENTRY;       // Q channel at byte offset 12
	q[0] = (uint8_t)((t.attr & 0xf0) | 0x01); // ctrl nibble | adr=1
	q[1] = bin_to_bcd((uint8_t)t.number);     // track number (1-based)
	q[2] = bin_to_bcd(1);                     // index 1

	uint32_t rel = (lba >= t.start) ? (lba - t.start) : 0u; // track-rel, no pregap
	q[3] = bin_to_bcd((uint8_t)((rel / 75u) / 60u));
	q[4] = bin_to_bcd((uint8_t)((rel / 75u) % 60u));
	q[5] = bin_to_bcd((uint8_t)( rel % 75u));
	q[6] = 0;

	uint32_t ab = lba + 150u;                 // absolute, +2s pregap (lsn2msf)
	q[7] = bin_to_bcd((uint8_t)((ab / 75u) / 60u));
	q[8] = bin_to_bcd((uint8_t)((ab / 75u) % 60u));
	q[9] = bin_to_bcd((uint8_t)( ab % 75u));
	// q[10],q[11] = CRC, left zero (WinUAE leaves them 0 on the regen path)
}

// WinUAE sub_to_interleaved (blkdev_cdimage.cpp:280): deinterleaved 8x12
// channel layout -> bit-interleaved 96-byte raw subchannel.
static void sub_to_interleaved(const uint8_t *s, uint8_t *d)
{
	for (int i = 0; i < AKIKO_SUB_BYTES; i++) {
		int dmask = 0x80;
		int smask = 1 << (7 - (i & 7));
		d[i] = 0;
		for (int j = 0; j < 8; j++) {
			if (s[(i / 8) + j * AKIKO_SUB_ENTRY] & smask) d[i] |= dmask;
			dmask >>= 1;
		}
	}
}

// Push one 96-byte INTERLEAVED subchannel block over the subcode UIO channel.
// The FPGA stages it and (if CDFLAG_SUBCODE is set) DMAs it to the game's
// subcode buffer + raises CDINT_SUBCODE. Mirrors akiko_push_sector's slow path.
static void akiko_push_subcode(const uint8_t *d96)
{
	EnableIO();
	spi8(UIO_DMA_WRITE);
	spi32_w(AKIKO_SUBCODE_ADDR);
	for (int i = 0; i < AKIKO_SUB_BYTES; i++) spi_w(d96[i]);
	DisableIO();
}

// Pump tick: if a CDDA play is armed and the FIFO has room, push exactly
// one sector. Returns true if it pushed (caller treats as one bridge
// action consumed and returns from poll). Handles natural end-of-play
// by emitting playend_notify(1) via the cd_audio_timeout state machine.
static bool akiko_cdda_pump(void)
{
	if (cd_cdda_lba_next < 0) return false;
	if (cd_paused) return false;
	if (!cd_cdda_drv) {
		// Drive vanished — abort the pump rather than spin trying to
		// re-acquire (we'd risk reading from a swapped CD).
		cd_cdda_lba_next = -1;
		cd_cdda_lba_end  = -1;
		cd_audio_timeout = -3;
		akiko_diag("[akiko] CDDA pump abort: drive gone");
		return false;
	}
	if (!akiko_audio_fifo_ready()) return false;

	uint8_t buf[AKIKO_CDDA_BYTES];
	uint32_t lba = (uint32_t)cd_cdda_lba_next;
	if (!cd_read_audio_sector(cd_cdda_drv, lba, buf)) {
		// Read failure — push silence so the FIFO doesn't underrun at the
		// boundary, but tear down so we don't loop here forever. WinUAE
		// emits playend_notify(-1) on producer error (akiko.cpp:1432-1435).
		akiko_diag("[akiko] CDDA pump read FAIL at lba=%u, aborting", lba);
		memset(buf, 0, sizeof(buf));
		akiko_push_audio_sector(buf);
		cd_cdda_lba_next = -1;
		cd_cdda_lba_end  = -1;
		cd_cdda_drv      = NULL;
		cd_audio_timeout = -3;
		return true;
	}

	akiko_push_audio_sector(buf);
	cd_cdda_lba_next++;
	cd_cdda_q_pos = (int32_t)lba;          // SUBQ reports the sector just played

	// CDDA position heartbeat: stream the Q-subchannel for the sector we just
	// played. The FPGA only DMAs/IRQs it when the game set CDFLAG_SUBCODE, so
	// non-subcode titles are unaffected. Without this, games that arm the
	// SUBCODE interrupt (Liberation, Deep Core) re-cue the music every ~0.9 s.
	{
		uint8_t sub_deint[AKIKO_SUB_BYTES];
		uint8_t sub_intlv[AKIKO_SUB_BYTES];
		build_subcode_deint(cd_cdda_drv, lba, sub_deint);
		sub_to_interleaved(sub_deint, sub_intlv);
		akiko_push_subcode(sub_intlv);
	}

	if ((lba & 0xff) == 0) {
		int32_t remaining = cd_cdda_lba_end - cd_cdda_lba_next;
		(void)remaining;
		akiko_dbg("CDDA pump pushed lba=%u (rem=%d)\n", lba, remaining);
	}

	if (cd_cdda_lba_next >= cd_cdda_lba_end) {
		akiko_dbg("CDDA done at lba=%u\n", lba);
		akiko_diag("[akiko] CDDA pump natural end at lba=%u", lba);
		// WinUAE cd_qcode parity: report the exact play-end so a SUBQ-polling
		// game (DotC) sees end-of-audio; q_end kept for the clamp, cd_playing
		// stays 1 so cmd_subq still answers.
		cd_cdda_q_pos    = cd_cdda_q_end;
		cd_cdda_lba_next = -1;
		cd_cdda_lba_end  = -1;
		cd_cdda_drv      = NULL;
		// Advance the play-state machine through -1 → -2 → emit
		// playend_notify(1). cd_audio_play_until_ms wall-clock fallback
		// is no longer needed since we just hit natural end.
		cd_audio_play_until_ms = 0;
		cd_audio_timeout = -1;
	}
	return true;
}

// Refill window win_idx starting at base_lba. Reads up to AKIKO_PREFETCH_SECTORS
// sectors; per-sector failures mark that slot invalid. Returns number of sectors
// successfully read; 0 means the entire batch failed.
//
// Validity is binary (1=valid, 0=read-failed). WinUAE uses a decay counter
// (init=3, decrement on access) as part of its eviction policy, but with our
// LRU-by-window approach decay would silently break BIOS retry patterns:
// after 3 accesses to the same LBA the entry would go invalid and stall the
// read. Keep validity binary; eviction is per-window via lru_tick.
static int akiko_prefetch_fill(drive_t *drv, int win_idx, uint32_t base_lba)
{
	prefetch_window_t *w = &cd_prefetch_windows[win_idx];
	w->base_lba = (int32_t)base_lba;
	int filled = 0;
	for (int i = 0; i < AKIKO_PREFETCH_SECTORS; i++) {
		uint32_t lba = base_lba + i;
		uint8_t *slot = &w->buf[i * AKIKO_SECTOR_BYTES];
		if (cdrom_read_raw_sector(drv, lba, slot) == 0) {
			w->valid[i] = 1;
			filled++;
		} else {
			w->valid[i] = 0;
			cd_prefetch_failed_sectors++;
		}
	}
	return filled;
}

// Returns 0 on cache hit (buf populated), -1 on miss-and-refill-failed, or
// -2 on cached-as-bad. -2 = sector failed to read; caller should skip the PBX
// push so the FPGA holds sec_req and BIOS retries on the next poll.
//
// On miss, evict the LRU window and refill it. The fill base is aligned to a
// 128-sector boundary so cache windows are predictable and short backwards
// seeks (BIOS often re-reads PVD/dirent after data passes) stay in-window.
static int akiko_prefetch_get(drive_t *drv, uint32_t lba, uint8_t *out_buf)
{
	int win = akiko_prefetch_find(lba);
	if (win < 0) {
		cd_prefetch_misses++;
		win = akiko_prefetch_pick_evict();
		uint32_t base = (lba / AKIKO_PREFETCH_SECTORS) * AKIKO_PREFETCH_SECTORS;
		if (akiko_prefetch_fill(drv, win, base) == 0) {
			akiko_diag("[akiko] prefetch fill TOTAL FAIL win=%d base=%u (req lba=%u)",
			           win, base, lba);
			return -1;
		}
		akiko_diag("[akiko] prefetch refill win=%d base=%u (req lba=%u hits=%u misses=%u failed=%u)",
		           win, base, lba, cd_prefetch_hits, cd_prefetch_misses,
		           cd_prefetch_failed_sectors);
	} else {
		cd_prefetch_hits++;
	}
	prefetch_window_t *w = &cd_prefetch_windows[win];
	w->lru_tick = ++cd_prefetch_lru_counter;
	int idx = (int)(lba - (uint32_t)w->base_lba);
	if (w->valid[idx] == 0) {
		// In-window but flagged bad. Try a single-sector re-read for this
		// LBA only — transient CHD errors shouldn't permanently poison the
		// cache slot.
		uint8_t *slot = &w->buf[idx * AKIKO_SECTOR_BYTES];
		if (cdrom_read_raw_sector(drv, lba, slot) == 0) {
			w->valid[idx] = 1;
		} else {
			return -2;
		}
	}
	memcpy(out_buf, &w->buf[idx * AKIKO_SECTOR_BYTES], AKIKO_SECTOR_BYTES);
	return 0;
}

// DEBUG counter readback (Deep Core garble localization, 2026-06-05). Reads the
// 13-byte drain on AKIKO_DBG_ADDR and parses the 0xC5-magic-prefixed payload
// (robust to a 1-byte SPI read-alignment slip). Returns false if the magic
// isn't found — e.g. the running RBF predates the debug channel.
static bool akiko_read_dbg_counters(uint32_t *fill, uint32_t *cnt, uint32_t *sum)
{
	uint8_t raw[24];
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(AKIKO_DBG_ADDR);
	for (int i = 0; i < (int)sizeof(raw); i++) raw[i] = (uint8_t)spi_w(0);
	DisableIO();
	for (int i = 0; i + 12 < (int)sizeof(raw); i++) {
		if (raw[i] == 0xC5) {
			const uint8_t *p = &raw[i + 1];
			*fill = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
			*cnt  = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
			*sum  = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
			return true;
		}
	}
	return false;
}

// DEBUG per-sector commit verifier (Deep Core, 2026-06-05). The FPGA's fill and
// commit shadows latch one sector behind (fill at sector_ready, commit at
// PBX_FIN), so each call reads them for the PREVIOUS real sector pushed and
// compares to the expectation stashed for it, then stashes THIS sector's
// expectation. Decisive split:
//   cnt   != 2352            -> PBX DATA byte(s) never committed to chip SDRAM:
//                               a downstream drop the arbiter's self-ack hides.
//   fill  != raw-sum         -> fast fill captured the sector wrong (M1).
//   commit_core != data-sum  -> wrong byte values reached chip RAM (M2 data).
//   all OK but game WILD      -> corruption is in the CPU read / coherency path.
// commit_core removes the modeled PBX byte-0..3 transform (0,0,0,counter&31)
// from the FPGA commit sum so it can be compared to the raw data region.
static void akiko_dbg_check_and_stash(const uint8_t *buf, uint8_t counter, uint32_t lba)
{
	static bool     have_prev = false;
	static uint32_t prev_fill = 0, prev_commit_core = 0, prev_lba = 0;
	static uint8_t  prev_counter = 0;
	static uint32_t dbg_seq = 0;

	uint32_t fill = 0, cnt = 0, sum = 0;
	bool ok = akiko_read_dbg_counters(&fill, &cnt, &sum);
	if (ok && have_prev) {
		long long commit_core = (long long)sum - (long long)(prev_counter & 0x1f);
		bool fill_bad   = (fill != prev_fill);
		bool cnt_bad    = (cnt  != AKIKO_SECTOR_BYTES);
		bool commit_bad = (commit_core != (long long)prev_commit_core);
		bool bad = fill_bad || cnt_bad || commit_bad;
		if (bad || (dbg_seq & 0xff) == 0) {
			akiko_diag("[akiko][dbg] lba=%u fill exp=%u got=%u%s | commit cnt=%u(exp %u)%s "
			           "sum=%u core=%lld exp=%u%s%s",
			           prev_lba, prev_fill, fill, fill_bad ? " FILL_MISMATCH" : "",
			           cnt, AKIKO_SECTOR_BYTES, cnt_bad ? " CNT_DROP" : "",
			           sum, commit_core, prev_commit_core, commit_bad ? " COMMIT_MISMATCH" : "",
			           bad ? "" : " OK");
		}
		dbg_seq++;
	} else if (!ok) {
		static bool warned = false;
		if (!warned) {
			akiko_diag("[akiko][dbg] counter-drain magic not found (RBF lacks the debug channel?)");
			warned = true;
		}
	}

	uint32_t f = 0, c = 0;
	for (int i = 0; i < AKIKO_SECTOR_BYTES; i++) f += buf[i];
	for (int i = 4; i < AKIKO_SECTOR_BYTES; i++) c += buf[i];
	prev_fill = f; prev_commit_core = c; prev_counter = counter; prev_lba = lba;
	have_prev = true;
}

// Service one akiko_sec_req. Reads the engine's current sector_counter,
// fetches LBA = base + counter from the CD image, and pushes the raw 2352-byte
// frame back. On any error (no disc, no data read armed, image read failure)
// we still push 2352 zeros so the engine doesn't stall — the PBX cycle will
// land but the resulting sector will fail Kickstart's data-checksum (returning
// junk is preferable to deadlocking the CD32 boot path on a transient).
//
// Phase 32.6: bucketed timing instrumentation. WinUAE 6.0.3 reaches CF "GO
// FOR IT" in ~40s; we're 5-15x slower. Three phases per sec_req can swallow
// time: (a) sector_counter SPI read, (b) cdrom_read_raw_sector (CHD seek +
// hunk decode), (c) push_sector (2352 byte-by-byte SPI w/ ACK). Sum µs spent
// in each across N sectors and emit one log line per bucket so we can see
// where the bottleneck actually lives before optimizing.
static void akiko_handle_sec_req(void)
{
	struct timespec ts0, ts1, ts2, ts3;
	static int64_t phase_a_us = 0, phase_b_us = 0, phase_c_us = 0;
	static uint32_t bucket_count = 0;
	const uint32_t BUCKET_SIZE = 256;

	// Phase 33-C diagnostic: log sec_req gap > 50 ms with the lba context.
	// If sec_req keeps firing during the BIOS "stall", bottleneck is elsewhere
	// (CHD read or push). If sec_req goes silent, BIOS has stopped writing
	// cdrom_pbx and the gap is BIOS-internal — pointing at a missing
	// completion notification on our side.
	static uint32_t last_sec_req_ms = 0;
	uint32_t now_ms = (uint32_t)GetTimer(0);
	if (last_sec_req_ms != 0 && (now_ms - last_sec_req_ms) >= 50) {
		akiko_diag("[akiko] sec_req GAP %ums (lba_base=%d)",
		           now_ms - last_sec_req_ms, cd_data_lba_base);
	}
	last_sec_req_ms = now_ms;

	clock_gettime(CLOCK_MONOTONIC, &ts0);
	uint8_t counter = akiko_read_sec_counter();
	clock_gettime(CLOCK_MONOTONIC, &ts1);

	uint8_t buf[AKIKO_SECTOR_BYTES];

	if (cd_data_lba_base == -1) {
		akiko_dbg("sec_req but no data read armed (counter=%u)\n", counter);
		memset(buf, 0, sizeof(buf));
		akiko_push_sector(buf);
		return;
	}

	drive_t *drv = cd_find_drive();
	if (!drv) {
		akiko_dbg("sec_req but no CD drive (counter=%u)\n", counter);
		memset(buf, 0, sizeof(buf));
		akiko_push_sector(buf);
		return;
	}

	// Per-sector pre-gap check. Earlier code skipped the entire burst when
	// cd_data_lba_base < 0, which broke DotC: its BIOS arms PLAY DATA at
	// start_lba=-34 (typical CDTV/CD32 boot pattern of streaming across the
	// pre-gap into track 1), then expects real sector data once the counter
	// advances past 34. Per-burst skip stranded BIOS forever waiting on the
	// first real sector. Per-sector handling silences only the genuinely-
	// negative LBAs and serves CHD data once we cross zero.
	int32_t signed_lba = cd_data_lba_base + (int32_t)counter;
	// 2026-06-05 Deep Core counter-drift diagnostic: trace the first sec_reqs
	// so we can see whether `counter` resets to 0 per READ DATA arm or keeps
	// climbing across arms (=> base+counter delivers the wrong sector).
	{
		static uint32_t dbg_n = 0;
		if (dbg_n < 60) {
			akiko_diag("[akiko] DBG sec_req#%u base=%d counter=%u -> lba=%d",
			           dbg_n, cd_data_lba_base, counter, signed_lba);
			dbg_n++;
		}
	}
	if (signed_lba < 0) {
		memset(buf, 0, sizeof(buf));
		akiko_push_sector(buf);
		return;
	}
	uint32_t lba = (uint32_t)signed_lba;

	// Out-of-bounds guard. BIOS occasionally arms PLAY DATA past lead-out
	// (CF arms start_lba=51216 with lead-out=39909, DotC has hit lba=286166
	// against lead-out=115153). Without this guard, the prefetch refill fails,
	// the cache marks the sector invalid, and we return rc=-2 forever — BIOS
	// keeps re-asserting sec_req, we keep skip-pushing, the core hangs on a
	// black screen. Push silence + advance so BIOS gets *something* and can
	// move on, matching how WinUAE handles a read past the end of media.
	//
	// Defensive: when lead_out==0 (track table empty during a CHD swap window)
	// we previously fell through to a guaranteed-failing CHD read and the
	// ~5000Hz retry storm. Treat unknown geometry as "hold and silence" too —
	// the bounded-retry path below catches the residual case where the OOB
	// arm sneaks through with a stale-but-larger lead_out from a prior CHD.
	int real_tracks = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
	uint32_t lead_out = (real_tracks > 0) ? drv->track[real_tracks].start : 0;
	if (!lead_out || lba >= lead_out) {
		static uint32_t last_oob_lba = 0;
		if (lba != last_oob_lba) {
			akiko_diag("[akiko] sec_req lba=%u %s lead_out=%u — push silence, advance",
			           lba, lead_out ? ">=" : "(no track table)", lead_out);
			last_oob_lba = lba;
		}
		memset(buf, 0, sizeof(buf));
		akiko_push_sector(buf);
		return;
	}

	// Audio-track guard. WinUAE akiko.cpp:1322,1361-1362,1610-1614 sets
	// sector_buffer_info[i]=0 for non-data sectors via is_valid_data_sector
	// (control & 0x0c != 4). cdrom_run_read then SKIPS the PBX push (inc=0)
	// and holds sec_req.
	//
	// Without a guard, BIOS reads raw audio bytes from PBX, treats them as
	// ISO9660 directory data, and uses noise as the next PLAY DATA start
	// MSF — producing the CR2/HQ2/Microcosm 4-ms retry storm with invalid
	// BCD bytes (e.g. 0x7C/0x61). 2026-05-08 trace diff: CR2 boots fine
	// until LBA 37222 (track 2 audio); next CMD has start=0x7C5240E.
	//
	// Pure hold (no push, no IRQ) deadlocks BIOS — it never times out,
	// just waits forever for CDINT_PBX. WinUAE survives this somehow but
	// our 180s capture confirms our BIOS image does not. Solution: hold
	// for a short window (~500 ms) then synthesise a "play ended" frame
	// so BIOS receives the same CDINT_DRIVERECV completion it would get
	// at lead-out, drops CDFLAG_ENABLE, and moves on. Mirrors the
	// audiotimeout=-1 -> playend_notify(1) path WinUAE uses for the audio
	// command end-of-stream.
	{
		track_t *track = NULL;
		for (int i = 0; i < real_tracks; i++) {
			uint32_t end = drv->track[i].start + drv->track[i].length;
			if (lba >= drv->track[i].start && lba < end) {
				track = &drv->track[i];
				break;
			}
		}
		if (track && !(track->attr & 0x40)) {
			static uint32_t held_audio_lba = UINT32_MAX;
			static uint32_t held_count = 0;
			const uint32_t HOLD_MAX = 50;
			if (lba != held_audio_lba) {
				held_audio_lba = lba;
				held_count = 0;
				akiko_diag("[akiko] sec_req lba=%u in audio track (attr=0x%02x) — hold",
				           lba, track->attr);
			}
			held_count++;
			if (held_count >= HOLD_MAX) {
				akiko_diag("[akiko] audio-hold cap reached at lba=%u — emit playend_notify, disarm",
				           lba);
				cd_data_lba_base = -1;
				emit_playend_notify(1);
				held_audio_lba = UINT32_MAX;
				held_count = 0;
			}
			return;
		}
	}

	int rc = akiko_prefetch_get(drv, lba, buf);
	if (rc != 0) {
		// rc == -1: the entire 128-sector batch read failed (CHD/SPI broken)
		// rc == -2: this individual sector was marked invalid in the cache
		// Bounded retry: WinUAE-style inc=0 hold lets BIOS retry on transients,
		// but if the read keeps failing we'd otherwise spin at ~5000 Hz forever
		// (observed: lba=51216 retry storms when the OOB guard misses because
		// lead_out=0 during a CHD swap, and lba=543450 from junk PLAY DATA
		// arms during core-load transitions). After MAX_FAIL_RETRIES on the
		// same LBA, push silence + advance to unblock BIOS, matching how the
		// OOB guard recovers.
		static const unsigned MAX_FAIL_RETRIES = 8;
		static uint32_t fail_lba = UINT32_MAX;
		static unsigned fail_count = 0;
		if (lba != fail_lba) { fail_lba = lba; fail_count = 0; }
		fail_count++;
		akiko_diag("[akiko] sec_req lba=%u %s (retry %u/%u)", lba,
		           (rc == -1) ? "prefetch FAIL" : "cached invalid",
		           fail_count, MAX_FAIL_RETRIES);
		if (fail_count < MAX_FAIL_RETRIES) return;
		akiko_diag("[akiko] sec_req lba=%u retry cap reached — push silence, advance", lba);
		fail_lba = UINT32_MAX;
		memset(buf, 0, sizeof(buf));
		akiko_push_sector(buf);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts2);

	// DEBUG (Deep Core, 2026-06-05): verify the PREVIOUS sector's chip-RAM
	// commit before pushing this one, and stash this sector's expectation.
	akiko_dbg_check_and_stash(buf, counter, lba);

	akiko_push_sector(buf);
	clock_gettime(CLOCK_MONOTONIC, &ts3);

	// Signed math throughout — when tv_nsec wraps across a second, the diff
	// is genuinely negative until the tv_sec component compensates. Casting
	// to unsigned mid-computation produces 2^64-class garbage values.
	#define TS_DELTA_US(a, b) \
		(((int64_t)(b).tv_sec  - (int64_t)(a).tv_sec ) * 1000000LL + \
		 ((int64_t)(b).tv_nsec - (int64_t)(a).tv_nsec) / 1000LL)
	phase_a_us += TS_DELTA_US(ts0, ts1);
	phase_b_us += TS_DELTA_US(ts1, ts2);
	phase_c_us += TS_DELTA_US(ts2, ts3);
	#undef TS_DELTA_US
	bucket_count++;

	if (bucket_count >= BUCKET_SIZE) {
		akiko_diag("[akiko] sec_req timing N=%u: ctr_read=%lld chd_read=%lld push=%lld (us total) "
		           "= %lld/%lld/%lld us avg, last lba=%u",
		           bucket_count,
		           (long long)phase_a_us,
		           (long long)phase_b_us,
		           (long long)phase_c_us,
		           (long long)(phase_a_us / bucket_count),
		           (long long)(phase_b_us / bucket_count),
		           (long long)(phase_c_us / bucket_count),
		           lba);
		phase_a_us = phase_b_us = phase_c_us = 0;
		bucket_count = 0;
	}
}

// -----------------------------------------------------------------------------
// Bus trace drain (debug)
// -----------------------------------------------------------------------------

// Drain the akiko_bus_trace ring buffer. ALWAYS COMPILED IN — the ring
// back-pressures the CPU when full, and an undrained ring stalls CF on
// dense bus activity (root cause of "stuck at lba 33"). Per-entry logging
// is gated on AKIKO_BUS_TRACE because at >90% of log volume during
// gameplay it dominates I/O cost; the bytes are still consumed off the
// ring, just discarded silently.
static void akiko_drain_trace(void)
{
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(AKIKO_TRACE_ADDR);

	// Cap at 128 entries (ring depth, matches akiko_bus_trace.v) so a runaway
	// loop can't lock us up.
	for (int i = 0; i < 128; i++) {
		uint8_t b0 = (uint8_t)spi_w(0);  // {wr, addr[6:0]}
		uint8_t b1 = (uint8_t)spi_w(0);  // data[7:0]
		uint8_t b2 = (uint8_t)spi_w(0);  // data[15:8]
		uint8_t b3 = (uint8_t)spi_w(0);  // 0xFF valid, 0x00 empty

		if (b3 == 0) break;              // ring drained

		// Phase 33-C: filtered trace — log writes to control registers
		// ($00-$27) so we can see CDFLAG_ENABLE toggles, PBX writes, INTREQ
		// acks during the sec_req stall. Excludes high-volume cmd buffer
		// ($18-$1B) and data-DMA addresses to keep log overhead manageable.
		//
		// v28: also emit READ entries (bit7=0). The RTL already filters
		// reads to control-register addresses AND only inside the post-CMD
		// arm window, so userspace can emit them all without flooding.
		uint8_t  reg_addr = (b0 & 0x7F) << 1;          // word address within $B80000
		bool     is_wr    = (b0 & 0x80) != 0;
		bool     is_ctrl  = (reg_addr <= 0x27);        // control registers
		bool     is_cmd_buf = (reg_addr >= 0x18 && reg_addr <= 0x1B); // very chatty
		uint32_t addr = 0xB80000u | reg_addr;
		uint16_t data = (uint16_t)b1 | ((uint16_t)b2 << 8);
		if (is_wr) {
			if (is_ctrl && !is_cmd_buf) {
				akiko_diag("[trace] W $%06X = 0x%04X", addr, data);
			}
		} else {
			// All read entries that escape the RTL filter are interesting.
			akiko_diag("[trace] R $%06X -> 0x%04X", addr, data);
		}
	}

	DisableIO();
}

// -----------------------------------------------------------------------------
// Top-level poll
// -----------------------------------------------------------------------------

// Phase 32.5.1 entrypoint: register the currently-mounted CD image's path
// so we can pick the right per-game save slot. Called from
// ide_cdrom.cpp::cdrom_parse on every mount/unmount. Empty string =
// unmount. Safe to call before akiko_cd32_init() — state is plain static
// memory and the actual SPI load is deferred to the poll loop.
//
// On a CD swap (different basename hash) we synchronously flush any dirty
// EEPROM contents to the OLD save file before swapping, so `mount A → write
// → mount B → write → mount A` preserves both saves correctly. The flush
// is skipped when we never observed dirty for the old slot — that avoids
// overwriting a real save with the synthesized FlashFile init from a
// different CD when the player just browsed past one image without
// running it.
void akiko_cd32_set_cd_path(const char *path)
{
	char new_path[256] = {0};
	if (path && *path) {
		compute_save_path(path, new_path, sizeof(new_path));
	}

	if (strcmp(new_path, cd_save_path_active) == 0) {
		// Same hash slot (or both empty) — nothing to do. Either the
		// user remounted the same CD or this is a pre-init no-op.
		return;
	}

	// Pre-warm Linux page cache for the boot region of the CHD. The PBX
	// prefetch covers ~99% of in-game accesses, but each cache miss still
	// costs a USB read (~30 MB/s on USB 2.0). User-observed: a core reset
	// after first boot makes subsequent boots dramatically faster — that's
	// the kernel page cache, not our cache.
	//
	// 16 MB is enough to cover ISO9660 PVD/path/dirent (LBAs 16-32) plus
	// the typical CD32 boot executable region (~20-50 MB into the disc
	// for most titles). Whole-file readahead (size=0) was tested 2026-05-10
	// but caused a long black-screen stall on Sim City because the kernel
	// readahead saturated USB bandwidth during the BIOS init phase. The
	// 16 MB cap finishes in ~500ms on USB 2.0 — fast enough that it
	// doesn't compete with the BIOS's own reads. Far-LBA streamers
	// (Microcosm: deep video data) still benefit because their boot+
	// menu region is in the warmed window; their late streaming hits
	// the bridge cache normally.
	if (path && *path) {
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			posix_fadvise(fd, 0, 16 * 1024 * 1024, POSIX_FADV_WILLNEED);
			close(fd);
			akiko_diag("[akiko] set_cd_path: fadvise WILLNEED 16MB on %s", path);
		}
	}

	// Flush old save before swap, only if there's a chance writes happened
	// since the last load (the dirty-debounce poll path may have already
	// caught a write in flight; either way, dirty-observed gates this).
	if (cd_save_path_active[0] && cd_save_dirty_observed) {
		akiko_diag("[akiko] set_cd_path: flushing old save %s before swap",
		           cd_save_path_active);
		akiko_nvram_save_to_path(cd_save_path_active);
	}

	// Swap to the new slot.
	strncpy(cd_save_path_active, new_path, sizeof(cd_save_path_active) - 1);
	cd_save_path_active[sizeof(cd_save_path_active) - 1] = 0;
	cd_save_dirty_observed = false;
	// Reset load-failed status — the new slot will set it from its own
	// load_from_path verify result. Without this, a previous slot's failure
	// would block writeback for the new slot too.
	cd_save_load_failed = false;

	if (!cd_save_path_active[0]) {
		cd_save_load_pending = false;
		akiko_diag("[akiko] set_cd_path: CD unmounted, no save file active");
		return;
	}

	// Arm deferred load. The poll path's first-mounted handler will load
	// the file into BRAM after the SPI_RST_USR pulse has finished wiping
	// it but before BIOS issues its first EEPROM I2C read. Sync load from
	// here is unsafe — it both gets wiped by the post-set_cd_path reset
	// AND its 1024-byte spi_w stream perturbs bridge state in a way that
	// breaks CF boot.
	cd_save_load_pending = true;
	akiko_diag("[akiko] set_cd_path: cd=%s -> save=%s (deferred load armed)",
	           path, cd_save_path_active);
}

void akiko_cd32_init(void)
{
	cd_initialized   = 0;
	cd_paused        = 0;
	cd_playing       = 0;
	cd_led_state     = 0;
	cd_door          = 1;                          // see static initializer above
	cd_play_start_lba = 0;
	cd_play_end_lba   = 0;
	cd_data_lba_base = -1;
	cd_audio_timeout = 0;
	cd_audio_play_until_ms = 0;
	cd_cdda_lba_next = -1;
	cd_cdda_lba_end  = -1;
	cd_cdda_drv      = NULL;
	cd_last_mounted  = false;
	toc_point_count  = 0;
	toc_push_idx     = -1;
	toc_push_last_ms = 0;

	// Cross-core data corruption guard: the 128-sector PBX prefetch buffer
	// is plain static memory, so its contents survive load_core. Without
	// invalidation, DotC asking for an LBA already cached from CF would
	// receive CF's bytes. The post-info media-push pending flag is the
	// same story — a stale "1" here would fire a spurious mediachange
	// push on the very first poll of the new core.
	akiko_prefetch_invalidate();
	cd_post_info_media_push_pending = 0;

	// Phase 32.5.1: a Minimig core reset wipes FPGA BRAM. If we already
	// have an active per-game save file (set by cdrom_parse before or
	// after this init), schedule a reload so soft `load_core` of Minimig
	// behaves the same as a hardware reset for save persistence. Without
	// this, the user sees "save vanished" after every core-reload loop.
	cd_save_dirty_observed = false;
	// The pending load might succeed this time (host_we Quartus issue is
	// nondeterministic across power cycles per past observations); clear
	// the failed flag so a successful load isn't blocked, and a failed
	// load can re-set it.
	cd_save_load_failed = false;
	if (cd_save_path_active[0]) {
		cd_save_load_pending = true;
		akiko_diag("[akiko] init: scheduling reload of %s after BRAM wipe",
		           cd_save_path_active);
	}

	akiko_dbg("init\n");
}

// Write a one-line diagnostic to /tmp/akiko_dbg.log, bypassing libc stdio
// buffering. Cheap enough for an event-driven loop. Always compiled in.
static void akiko_diag(const char *fmt, ...)
{
	FILE *f = fopen("/tmp/akiko_dbg.log", "a");
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

void akiko_cd32_poll(void)
{
	// Phase 32.6 P3 (rolled back): tried replacing the floor with a SEC_REQ-
	// drop barrier inside akiko_handle_sec_req, but CF then re-armed PLAY at
	// 250 Hz (15k arms in 60s vs ~1 Hz baseline) and stalled at black screen.
	// The framed RX/TX barriers (rx_busy, req-clear) are sufficient on their
	// own only because their state bit is observable; the sec channel has
	// no analogous bit (sec_done is internal to the bridge), so a small
	// floor remains the simplest correct synchronisation. 20µs was the
	// original known-good value.
	usleep(20);

	bool mounted = cd_is_mounted();

	// Phase 33-E: deferred minimig_reset for "BIOS stuck on no-CD splash"
	// recovery. Set by the mediachange handler below when the unmount→mount
	// edge fires with cd_initialized>=2 and no prior eject in this session.
	// Fire at the top of the NEXT poll so the edge-detection code runs to
	// completion first (TOC rebuild + state bookkeeping). Safe because the
	// previous poll always exits through send_response's
	// `akiko_wait_status_bit` barrier — no UIO_DMA_WRITE is mid-flight.
	if (g_akiko_pending_minimig_reset) {
		g_akiko_pending_minimig_reset = false;
		akiko_diag("[akiko] mediachange: BIOS was stuck on no-CD splash, "
		           "triggering minimig_reset() (Phase 33-E)");
		minimig_reset();
		return;  // Don't continue this poll — minimig_reset re-init'd everything.
	}

	// Heartbeat: prove poll loop reached us at all. Writes to a dedicated
	// log file so it survives any stdout redirection MiSTer does after init.
	// Phase 32.5.1: NVRAM load is no longer driven from here — it's
	// scheduled by akiko_cd32_set_cd_path() (CD mount/swap) and
	// akiko_cd32_init() (post-reconfig BRAM wipe), and serviced below
	// once `mounted` is true. That way each Minimig load_core re-loads,
	// not just hardware boots, and per-game saves work correctly.
	static bool first_poll = true;
	if (first_poll) {
		first_poll = false;
		akiko_diag("[akiko] poll alive (first call) mounted=%d", mounted);
	}

	// Phase 32.5.1 deferred load. Triggered by set_cd_path() or init().
	// Only fires once mounted=1 so we don't load before the CHD is open
	// (early load would still work — it's purely a BRAM write — but the
	// log line is more useful when correlated with the mount event).
	if (cd_save_load_pending && mounted) {
		cd_save_load_pending = false;
		if (cd_save_path_active[0]) {
			// One-shot legacy migration: if the per-game file doesn't
			// exist but the legacy single-file save does, load the
			// legacy data into the per-game slot for this CD. Useful
			// for the user's existing cd32.nvr that pre-dates the
			// per-game scheme.
			struct stat pst;
			if (stat(cd_save_path_active, &pst) != 0 &&
			    stat(AKIKO_NVRAM_FILE_LEGACY, &pst) == 0 &&
			    pst.st_size == AKIKO_NVRAM_BYTES) {
				akiko_diag("[akiko] migrating legacy save %s -> %s",
				           AKIKO_NVRAM_FILE_LEGACY, cd_save_path_active);
				akiko_nvram_load_from_path(AKIKO_NVRAM_FILE_LEGACY);
			} else {
				akiko_nvram_load_from_path(cd_save_path_active);
			}
		}
	}

	// SD-block load is asynchronous (img_mounted edge → FPGA FSM walks
	// LBA 0). akiko_nvram_load_from_path schedules a verify ~250 ms out;
	// service it here once the deadline passes.
	akiko_nvram_verify_load_pending();

	// Always drain — the ring back-pressures the CPU when full. Per-entry
	// logging is gated inside the function on AKIKO_BUS_TRACE.
	akiko_drain_trace();

	// Chipset bus trace (gfx-trio investigation). Always-on drain; when the
	// agnus.v CHIPSET_TRACE gate is 0 this is a single empty-sentinel poll.
	chipset_trace_drain();

	// 2026-05-27 Z2-hang trace ring drain. Gated on AKIKO_Z2_TRACE (z2_trace.h);
	// release builds compile this out so /tmp/z2_trace.csv is never created and
	// the per-poll UIO drain disappears. Re-enable alongside the RTL ring
	// (build_minimig.ps1 -Z2Trace) when investigating Z2 fast-RAM hangs.
#if AKIKO_Z2_TRACE
	z2_trace_drain();
#endif

#if AKIKO_CD32_DEBUG
	// While we don't think we're mounted, periodically dump the underlying
	// ide_inst flags so we can see what state the IDE subsystem is in.
	// This burns one log line per second until something flips.
	static int unmounted_dump_throttle = 0;
	if (!mounted && (unmounted_dump_throttle++ % 60) == 0 && unmounted_dump_throttle < 5*60) {
		akiko_diag(
			"[akiko] ide_inst dump: "
			"[0,0]p=%d/c=%d/chd=%p/f=%p  [0,1]p=%d/c=%d/chd=%p/f=%p",
			ide_inst[0].drive[0].present, ide_inst[0].drive[0].cd,
			(void*)ide_inst[0].drive[0].chd_f, (void*)ide_inst[0].drive[0].f,
			ide_inst[0].drive[1].present, ide_inst[0].drive[1].cd,
			(void*)ide_inst[0].drive[1].chd_f, (void*)ide_inst[0].drive[1].f
		);
	}
#endif

	// Mediachange handling. Mirror WinUAE akiko.cpp:1399-1408: insert AFTER
	// INFO has run (cd_initialized>=2) goes through the dedicated mediachange
	// path that pushes 0x0a/0x01 + rebuilds TOC WITHOUT regressing FSM state.
	// Insert BEFORE INFO (cd_initialized<2, e.g. cold boot with CD) goes
	// through the cold-boot auto-init path below. Eject queues a 0x0a/0x00
	// push so BIOS sees the disc has gone away.
	//
	// Phase 33-D (2026-05-04): the prior version reset cd_initialized=0 on
	// EVERY mount edge, which broke the boot-with-no-CD-then-mount scenario:
	// BIOS issues INFO during no-CD state (cd_initialized -> 2), mount drops
	// it to 0, auto-init bumps it to 1, but BIOS never re-issues INFO so we
	// never reach 2 again — TOC drip (gated on cd_initialized==2) stays dead
	// and BIOS sits on no-disc screen forever. WinUAE keeps cd_initialized=2
	// across mediachange and the mediachange push alone is enough.
	static bool media_absent_push_pending = false;
	static bool mediachanged_push_pending = false;
	// Phase 33-E (2026-05-07): when the user boots with no CD, BIOS issues
	// 0x07 INFO during the no-disc state (cd_initialized -> 2) then sits
	// indefinitely on the spinning-CD splash. A subsequent insert produces a
	// mediachange push (0x0a/0x01) that BIOS ignores — empirically verified
	// (107-line akiko log, no CMDs after the push). The only thing that
	// unblocks BIOS is a CPU reset (LCtrl+LAlt+RAlt or OSD->Reset). We
	// distinguish "no-CD-boot then mount" (BIOS stuck) from "hot-swap during
	// gameplay" (BIOS handles the push) by tracking whether an eject was
	// observed earlier in this session.
	static bool had_prior_unmount = false;
	if (mounted != cd_last_mounted) {
		cd_data_lba_base = -1;               // any in-progress read is stale
		akiko_prefetch_invalidate();         // cache may be from previous disc
		// Phase 33: a CD swap or eject must abort any in-flight CDDA
		// pump — the captured cd_cdda_drv pointer would now reference a
		// closed CHD or read from the new disc otherwise.
		cd_cdda_lba_next = -1;
		cd_cdda_lba_end  = -1;
		cd_cdda_drv      = NULL;
		toc_point_count  = 0;
		toc_push_idx     = -1;
		if (!mounted) {
			// Just ejected — arm the absent push (gated on rx_idle below).
			// Preserve cd_initialized so the next insert routes through the
			// mediachange path (matches WinUAE: cd_initialized only resets
			// in akiko_reset, never on mediachange).
			media_absent_push_pending = true;
			mediachanged_push_pending = false;
			had_prior_unmount = true;
		} else {
			// Just inserted. Rebuild TOC eagerly so it's fresh when BIOS
			// queries (mirror WinUAE get_cdrom_toc() in mediachange branch
			// at akiko.cpp:1403; WinUAE calls it twice — first try may fail,
			// safe to do up front then again in the push branch below).
			media_absent_push_pending = false;
			akiko_build_toc();
			if (cd_initialized >= 2) {
				if (had_prior_unmount) {
					// Hot-swap during gameplay: BIOS already handled an eject
					// in this session, so the mediachange push is sufficient
					// (mirrors WinUAE's behaviour for the same case).
					mediachanged_push_pending = true;
				} else {
					// Boot-with-no-CD-then-mount (Phase 33-E): BIOS booted
					// past INFO without ever seeing a CD. Pushing 0x0a/0x01
					// is empirically a no-op — BIOS ignores it. Defer a
					// minimig_reset to fire on the NEXT poll iteration; that
					// re-runs ApplyConfiguration with the CHD already open
					// via the prior cdrom_parse, so the post-reset boot has
					// mounted=1 from t=0 and the cold-boot auto-init path
					// takes over normally.
					g_akiko_pending_minimig_reset = true;
					mediachanged_push_pending = false;
				}
			}
			// Else (cd_initialized<2): cold-boot auto-init path will fire.
		}
		cd_last_mounted = mounted;
		akiko_diag("[akiko] media change -> mounted=%d cd_init=%u (absent=%d, mediachanged=%d)",
		           mounted, cd_initialized,
		           media_absent_push_pending, mediachanged_push_pending);
	}

	// 0. Status poll moved up so unsolicited pushes (auto-init, post-INFO,
	// TOC drip) can be gated on FPGA rx_busy. Phase 18: bit[9] = rx_busy =
	// (cdrom_receive_length != 0). When set, the RX engine still has a
	// queued/in-flight response in result_buffer; pushing now would
	// overwrite it and lose data. Mirror of WinUAE's
	// cdrom_can_return_data() gate.
	uint16_t status = akiko_read_status();
#if AKIKO_CD32_DEBUG
	static uint16_t last_status = 0xffff;
	static int status_log_count = 0;
	if (status != last_status && status_log_count < 200) {
		akiko_diag("[akiko] status=0x%04x (was 0x%04x)", status, last_status);
		last_status = status;
		status_log_count++;
	}
#endif
	const bool rx_idle = !(status & AKIKO_STATUS_RX_BUSY);

	// 1. Auto-init: WinUAE akiko.cpp:1388-1392 pushes a media-status frame
	// when cd_initialized == 0 && media-present. BIOS uses this to learn
	// "media is here" before issuing LED/INFO. Phase 16: re-enabled now
	// that Phase 14 NVRAM and Phase 15 partial-RX delivery are working.
	// Without this, our trace shows BIOS doing LED+INFO drains via the
	// partial-RX path (RXCMP bumped 4 → 30) but never issuing PLAY/MULTI:
	// it's still in "expecting media-status" mode after INFO.
	if (mounted && cd_initialized == 0 && rx_idle) {
		uint8_t r[2];
		r[0] = 0x0a;                                         // CDS_MEDIA_STATUS opcode
		r[1] = 0x01;                                         // media present
		akiko_send_response(r, 2);
		cd_initialized = 1;
		akiko_diag("[akiko] auto-init media-status push: opcode=0x0a status=0x01 (cd_initialized=1)");
	}

	// Phase 33-D: mediachange push. Fired on insert AFTER INFO has run
	// (cd_initialized>=2). Mirror of WinUAE akiko.cpp:1399-1408 — pushes
	// 0x0a/0x01 + rebuilds TOC twice, WITHOUT resetting cd_initialized so
	// the TOC drip can fire when BIOS responds with MULTI 0x04.
	if (mediachanged_push_pending && mounted && cd_initialized >= 2 && rx_idle) {
		uint8_t r[2];
		r[0] = 0x0a;
		r[1] = 0x01;
		akiko_send_response(r, 2);
		mediachanged_push_pending = false;
		akiko_build_toc();                                  // WinUAE: "do not remove! first try may fail"
		akiko_diag("[akiko] mediachange push: opcode=0x0a status=0x01 (TOC rebuilt 2x)");
	}

	// Phase 32.6: pending eject notification. Fired once per mount->unmount
	// edge; cleared on send. rx_idle gating same as auto-init.
	if (media_absent_push_pending && !mounted && rx_idle) {
		uint8_t r[2];
		r[0] = 0x0a;                                         // CDS_MEDIA_STATUS opcode
		r[1] = 0x00;                                         // media absent
		akiko_send_response(r, 2);
		media_absent_push_pending = false;
		akiko_diag("[akiko] eject media-status push: opcode=0x0a status=0x00");
	}

	// Phase 20: post-INFO media-status push DISABLED (was Phase 15-19.x).
	// Subagent diff vs WinUAE akiko.cpp:1399-1407 shows the post-INFO push
	// is GATED on `mediachanged == 1`, which the auto-init push at
	// cd_initialized==0 already CONSUMED (akiko.cpp:1390-1392). On a cold
	// boot WinUAE therefore fires post-INFO ZERO times. Our unconditional
	// `cd_post_info_media_push_pending=1` in cmd_info was sending a
	// duplicate/spurious frame after BIOS had already accepted INFO,
	// likely racing the BIOS state machine and preventing it from
	// advancing to LED(1)/PLAY/MULTI. Drop it entirely to match WinUAE's
	// cold-boot behavior; only the auto-init push above should fire.
	(void)cd_post_info_media_push_pending;

	// 1.5 Auto-TOC drip: re-enabled in Phase 17, gated on rx_idle in Phase 18.
	// BIOS RXCMP after INFO+post-INFO goes 4,6,7,...,15,30 — the +15 jump from
	// RXCMP=15 to 30 indicates BIOS is waiting for a 15-byte TOC frame to land
	// at offset 30. WinUAE pushes one TOC entry per video frame
	// (akiko.cpp:1438-1440). Throttle accumulates only when the engine is
	// idle — pushing into a busy buffer would silently drop the entry.
	if (cd_initialized == 2 && toc_push_idx >= 0 && rx_idle) {
		// Phase 33-B (2026-05-04): wall-clock 50 Hz pacing — matches
		// WinUAE's framesync-driven cdrom_return_toc_entry()
		// (akiko.cpp:1438-1440, ~50 Hz PAL). Prior /1200 poll-count
		// throttle assumed a ~60 kHz poll rate but the real rate under
		// load is ~6.8 kHz, giving 175 ms/frame and (because the drip-
		// complete detection requires the same throttle to elapse one
		// more time after the last push) a measured 52-second stall
		// before BIOS resumed. Wall-clock pacing is rate-independent.
		uint32_t now_ms = (uint32_t)GetTimer(0);
		if ((now_ms - toc_push_last_ms) >= AKIKO_TOC_PUSH_PERIOD_MS) {
			toc_push_last_ms = now_ms;
			akiko_push_toc_entry();
		}
	}

	// Phase 32: NVRAM persistence. RTL latches nvr_dirty whenever BIOS
	// writes a byte through the I2C path; userspace dumps + saves once
	// the dirty state has held steady for the debounce window (BIOS
	// FlashFile commits are bursty — 16+ writes back-to-back). Gate the
	// dump itself on rx_idle so a save doesn't interleave with a queued
	// MULTI response, but the timestamp tracking runs unconditionally.
	{
		static uint32_t nvr_dirty_seen_at    = 0;  // 0 = not currently dirty
		static uint32_t nvr_last_save_at     = 0;  // wall-clock of last successful save
		static int      nvr_menu_was_present = 0;  // for OSD-open rising-edge detect
		const bool nvr_dirty_now  = (status & AKIKO_STATUS_NVR_DIRTY) != 0;
		const int  nvr_menu_now   = menu_present();
		// OSD-open rising edge: user popped open the OSD menu. Mirrors
		// arcade core pattern at menu.cpp:2217 (UIO_CHK_UPLOAD-driven
		// arcade_nvm_save). Treats OSD-open as "user is about to swap CD,
		// reset, or power down" — flush any pending dirty save now while
		// we still have the data and user attention. Saves the player from
		// the throttle window (which can lose 30s of progress on quick
		// reset).
		const bool osd_open_edge  = nvr_menu_now && !nvr_menu_was_present;
		nvr_menu_was_present      = nvr_menu_now;

		if (nvr_dirty_now) {
			cd_save_dirty_observed = true;     // arms set_cd_path flush
			uint32_t now_ms = (uint32_t)GetTimer(0);
			if (!nvr_dirty_seen_at) {
				nvr_dirty_seen_at = now_ms;
				akiko_diag("[akiko] NVR dirty observed at t=%u", now_ms);
			}
			// Fire on EITHER OSD-open-with-dirty (preferred — explicit
			// user intent, matches arcade pattern) OR the 30s debounce
			// expiring as a safety net for sessions where the user never
			// reopens OSD before power-off.
			const bool throttle_expired = rx_idle &&
				(now_ms - nvr_dirty_seen_at) >= AKIKO_NVRAM_DIRTY_DEBOUNCE_MS;
			const bool fire = osd_open_edge || throttle_expired;
			// Min-interval cooldown still applies — protects against
			// pathological save loops regardless of trigger.
			const bool cooldown_clear = !nvr_last_save_at ||
				(now_ms - nvr_last_save_at) >= AKIKO_NVRAM_MIN_SAVE_INTERVAL_MS;
			if (fire && cooldown_clear) {
				if (osd_open_edge) {
					akiko_diag("[akiko] NVR flush: OSD opened with dirty pending");
				}
				// Phase 32.5: bridge auto-clears dirty at end of the
				// READ burst inside save_to_disk's nvram_dump, so no
				// explicit clear-dirty SPI write is needed (and would
				// in fact corrupt byte 0 under the new write-as-load
				// bridge semantics).
				akiko_nvram_save_to_disk();
				nvr_dirty_seen_at = 0;
				nvr_last_save_at  = now_ms;
			}
		} else {
			// Flag dropped (RTL reset, or we just cleared it). Re-arm.
			nvr_dirty_seen_at = 0;
		}
	}

	// Audio-play notification state machine (mirrors WinUAE akiko_handler,
	// akiko.cpp:1411-1435). Only steps when the RX engine is idle so we
	// never overwrite an in-flight reply with the play-state frame.
	// Decrements through 2->1 then emits "play started"; advances -1->-2
	// then emits "play ended"; emits "play failed" on -3.
	if (cd_audio_timeout != 0 && rx_idle) {
		if (cd_audio_timeout > 1) {
			cd_audio_timeout--;
		} else if (cd_audio_timeout == 1) {
			emit_playend_notify(0);
			cd_audio_timeout = 0;
		} else if (cd_audio_timeout == -1) {
			// Phase 33 will tear down the CDDA pump here. For now we
			// just advance to the play-ended emission.
			cd_audio_timeout = -2;
		} else if (cd_audio_timeout == -2) {
			emit_playend_notify(1);
			cd_playing = 0;
			cd_audio_timeout = 0;
			cd_audio_play_until_ms = 0;
		} else if (cd_audio_timeout == -3) {
			emit_playend_notify(-1);
			cd_playing = 0;
			cd_audio_timeout = 0;
			cd_audio_play_until_ms = 0;
		}
		return;                              // one bridge action per poll
	}

	// Phase 32.6 P4: natural play_ended emission after the wall-clock track
	// duration. cmd_multi audio branch arms cd_audio_play_until_ms; we fire
	// the cdrom_playend_notify(1) frame once that deadline passes so games
	// gating on "audio finished" advance correctly. Skipped while the
	// existing cd_audio_timeout state machine is mid-transition (its
	// negative-branch path already handles play_ended for explicit stops).
	if (cd_audio_play_until_ms != 0 && cd_playing && rx_idle &&
	    cd_audio_timeout == 0) {
		uint32_t now_ms = (uint32_t)GetTimer(0);
		if ((int32_t)(now_ms - cd_audio_play_until_ms) >= 0) {
			emit_playend_notify(1);
			cd_playing = 0;
			cd_audio_play_until_ms = 0;
			akiko_diag("[akiko] PLAY AUDIO natural end at t=%u", now_ms);
			return;
		}
	}

	// Phase 33: CDDA pump. Push exactly one audio sector per poll if the
	// FIFO has room (status bit 8 = cdda_req asserted). The pump writes to
	// the cdda sub-channel (0xF200) which is independent of the bridge's
	// RX engine, so no rx_idle gate is needed. Runs even when paused (it
	// will see cd_paused=1 and decline) so the FIFO keeps draining naturally
	// during a pause; resuming starts pushing again from cd_cdda_lba_next.
	// On natural end, the pump arms cd_audio_timeout=-1 so the existing
	// state machine emits playend_notify(1) on a subsequent poll.
	if (akiko_cdda_pump()) {
		return;                              // one bridge action per poll
	}

	if (status & AKIKO_STATUS_SEC_REQ) {
		akiko_handle_sec_req();
		return;                              // one bridge action per poll
	}

	if (!(status & AKIKO_STATUS_REQ)) return;

	// 3. Drain the framed command.
	uint8_t cmd[AKIKO_CMD_MAX];
	int     n = akiko_drain_command(cmd);
	if (n < 1) {
		akiko_dbg("drain returned 0 bytes\n");
		return;
	}

#if AKIKO_CD32_DEBUG
	akiko_dbg("RX %d bytes:", n);
	for (int i = 0; i < n; i++) printf(" %02x", cmd[i]);
	printf("\n");
#endif

	int op = cmd[0] & 0x0f;
	int expected_len = command_lengths[op];

	// 4. Validate input checksum (akiko.cpp:1161-1191): sum of all bytes incl.
	//    checksum byte must equal 0xff. Only meaningful for known opcodes.
	if (expected_len > 0) {
		int chk_total = expected_len + 1;    // bytes summed = cmd + chk
		if (n < chk_total) {
			akiko_dbg("short frame: n=%d expected=%d\n", n, chk_total);
			cmd_bad(cmd, CH_ERR_CHECKSUM);
			return;
		}
		uint32_t sum = 0;
		for (int i = 0; i < chk_total; i++) sum += cmd[i];
		if ((sum & 0xff) != 0xff) {
			akiko_dbg("checksum FAIL sum=%02x\n", sum & 0xff);
			cmd_bad(cmd, CH_ERR_CHECKSUM);
			return;
		}
	}

	// Diagnostic cmd trace (low volume — skip the very chatty LED 0x05).
	// Helps correlate sec_req with CF's command stream when debugging
	// stuck-after-game-data-reached state.
	if (op != 0x05) {
		char hex[3 * AKIKO_CMD_MAX + 1];
		int  off = 0;
		for (int i = 0; i < n && i < 16; i++) {
			off += snprintf(hex + off, sizeof(hex) - off, "%02x ", cmd[i]);
		}
		if (off > 0) hex[off - 1] = '\0';
		akiko_diag("[akiko] CMD op=0x%02x n=%d bytes=%s", op, n, hex);
	}

	// 5. Dispatch.
	switch (op) {
		case 0x00: {
			// Phase 33-A WinUAE-parity: NOP / echo (akiko.cpp:1259-1262).
			// WinUAE returns a 1-byte response equal to the opcode itself
			// (the leading nibble is 0x0 — the responding side overwrites
			// it during framing). We previously fell through to cmd_bad
			// here which sent a 2-byte CH_ERR_BADCOMMAND, which BIOS may
			// have misinterpreted as a real error frame for the previous
			// command.
			uint8_t r[1];
			r[0] = cmd[0];
			akiko_send_response(r, 1);
			akiko_dbg("CMD 0x00 echo\n");
			break;
		}
		case 0x01: cmd_stop(cmd);    break;
		case 0x02: cmd_pause(cmd);   break;
		case 0x03: cmd_unpause(cmd); break;
		case 0x04: cmd_multi(cmd);   break;
		case 0x05: cmd_led(cmd);     break;
		case 0x06: cmd_subq(cmd);    break;
		case 0x07: cmd_info(cmd);    break;
		case 0x08:
		case 0x09:
		case 0x0a:
			// WinUAE-parity (akiko.cpp:1284-1290): these opcodes have
			// valid frame lengths in command_lengths[] (4, 1, 2) so the
			// framer consumes them, but the dispatcher returns len=0 and
			// sends no response payload. Previously we fell through to
			// cmd_bad which emitted a CH_ERR_BADCOMMAND frame — wrong vs
			// WinUAE and could confuse BIOS into treating a legitimately-
			// silent command as a bad-command error for the prior op.
			akiko_dbg("CMD 0x%02x consumed (no response per WinUAE parity)\n", op);
			break;
		default:
			cmd_bad(cmd, CH_ERR_BADCOMMAND);
			break;
	}
}
