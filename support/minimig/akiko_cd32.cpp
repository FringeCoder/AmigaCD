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
#include <strings.h>   // strcasecmp, for discnames.cfg key matching
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
#include "../../fpga_io.h"     // fpga_spi_fast / fpga_spi_fast_block_write
#include "../../user_io.h"
#include "../../ide.h"
#include "../../ide_cdrom.h"
#include "../../hardware.h"   // GetTimer / CheckTimer
#include "../../menu.h"       // menu_present() — OSD-open save trigger
#include "../chd/mister_chd.h" // mister_chd_read_sector for CDDA pump
#include "../physical_disc/physical_disc.h" // physical CD-ROM drive media
#include "akiko_pace.h"                    // sector delivery floor
#include "minimig_config.h"   // minimig_reset() — used to unblock BIOS stuck
                              // on no-CD splash after a boot-with-no-CD-then-mount.
#include "akiko_cd32.h"

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

// PBX sector size (raw Mode-1/Mode-2 frame).
#define AKIKO_SECTOR_BYTES 2352

// Ext-bus block write shaping: how many words go out per burst, and the gap
// between bursts. Transport shaping only -- the sector delivery RATE LIMIT is
// AKIKO_SECTOR_PERIOD_US in akiko_pace.h, not these.
//
// That distinction is new, and it is the point of this pair of comments. These
// two constants were the rate limit until 2026-09-14, by accident: usleep(10)
// costs a scheduler wakeup (~92 us measured on the MiSTer), not 10 us, so the
// 18 gaps in a 1176-word sector were spending ~1.7 ms while the constants read
// as 180 us. Widening the chunks or shrinking the gap would have removed the
// protection with nothing to say so. They are kept at the values #1277 tested
// because that is the shape that is field-proven, but the timing no longer
// depends on them.
#define AKIKO_FILL_CHUNK_WORDS 64
#define AKIKO_FILL_GAP_US      10

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

typedef enum {
	CD_MEDIA_UNKNOWN = -1,
	CD_MEDIA_ABSENT  = 0,
	CD_MEDIA_PRESENT = 1
} cd_media_level_t;

static cd_media_level_t cd_media_level = CD_MEDIA_UNKNOWN;
static bool     cd_media_push_pending = false;
static bool     cd_media_event        = false;
// Mounting a disc while one is already in swaps it with no removal in between,
// which no physical drive can do - the tray has to open. A guest is told
// present, then present again, and that is not a media change, so AmigaOS
// keeps serving the old volume from cache and the new disc never appears.
// Owe the guest the absent edge first.
static bool     cd_media_swap_absent  = false;
static uint32_t cd_media_swap_ready_ms = 0;
// The tray cannot open and shut in the same instant, and the removal has to
// reach CDFileSystem before the arrival lands or the two collapse back into
// no change at all.
#define AKIKO_SWAP_SETTLE_MS 700

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
// Physical discs arrive from cdrom_parse as "physdisc/<identity>". Everything
// else is an image path.
#define AKIKO_PHYSDISC_PREFIX "physdisc/"
#define AKIKO_PHYSDISC_PREFIX_LEN (sizeof(AKIKO_PHYSDISC_PREFIX) - 1)

// Optional user-supplied "identity = friendly name" map, one per line, '#' or
// ';' comments, whitespace around either side ignored. Exists because CD32
// discs routinely do not carry their own title: Simon the Sorcerer's official
// pressing has volume ID "CD32_TEST", every other ISO9660 name field empty, and
// a root directory of numbered .out chunks. There is nothing on the disc to
// read a real name from, so the only honest source is the person holding it.
#define AKIKO_DISCNAMES_FILE AKIKO_NVRAM_DIR "/discnames.cfg"

// Reduce a user-supplied name to something safe to put in a filename. Spaces
// survive -- they are legal and legibility is the whole point -- but path
// separators, control characters and the usual shell-hostile punctuation go.
// Runs of whitespace collapse to one; leading/trailing space and dots are
// trimmed, so the result can never be "", "." or "..".
static int sanitize_disc_name(const char *src, char *out, size_t outsz)
{
	size_t n = 0;
	int pending_space = 0;

	for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < outsz; p++)
	{
		unsigned char c = *p;
		if (c == ' ' || c == '\t') { if (n) pending_space = 1; continue; }
		if (c < 0x20 || c > 0x7E) continue;
		if (strchr("/\\:*?\"<>|", (char)c)) continue;
		if (pending_space && n + 2 < outsz) out[n++] = ' ';
		pending_space = 0;
		out[n++] = (char)c;
	}

	while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
	out[n] = 0;
	return (int)n;
}

// Returns 1 and fills out[] when discnames.cfg maps this identity to a name.
// Absent file, absent entry and a name that sanitises to nothing all mean "no
// override", so the caller falls back to the identity itself.
static int lookup_disc_name(const char *identity, char *out, size_t outsz)
{
	if (!identity || !*identity || !out || outsz < 2) return 0;
	out[0] = 0;

	FILE *f = fopen(AKIKO_DISCNAMES_FILE, "r");
	if (!f) return 0;

	char line[512];
	int found = 0;

	while (!found && fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#' || *p == ';' || *p == '\r' || *p == '\n') continue;

		char *eq = strchr(p, '=');
		if (!eq) continue;

		char *key_end = eq;
		while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
		*key_end = 0;
		if (strcasecmp(p, identity)) continue;

		char *val = eq + 1;
		while (*val == ' ' || *val == '\t') val++;
		char *nl = strpbrk(val, "\r\n");
		if (nl) *nl = 0;

		found = sanitize_disc_name(val, out, outsz) > 0;
	}

	fclose(f);
	return found;
}

static void compute_save_path(const char *cd_path, char *out, size_t outsz)
{
	// A physical disc names its save after the disc itself rather than a hash
	// of it. <identity> is already filesystem-safe -- physical_disc_save_name()
	// runs it through sanitize_name(), which keeps only [0-9A-Za-z-.], collapses
	// separators to '_' and strips trailing punctuation, and falls back to a
	// TOC-derived UUID when a label sanitises to nothing. So the file lands as
	// "ISSUE7.nvr" rather than "cd32-8fd2874f328829b5.nvr".
	//
	// Two reasons that is worth the special case: the saves directory becomes
	// readable at a glance, and a disc re-burned from the same source keeps its
	// save, since matching volume labels produce the same filename.
	if (!strncmp(cd_path, AKIKO_PHYSDISC_PREFIX, AKIKO_PHYSDISC_PREFIX_LEN))
	{
		const char *identity = cd_path + AKIKO_PHYSDISC_PREFIX_LEN;

		// discnames.cfg wins when it has an entry, because the identity is
		// often not a name anyone would recognise.
		char friendly[128];
		if (lookup_disc_name(identity, friendly, sizeof(friendly)))
		{
			snprintf(out, outsz, "%s/%s.nvr", AKIKO_NVRAM_DIR, friendly);
			return;
		}

		snprintf(out, outsz, "%s/%s.nvr", AKIKO_NVRAM_DIR, identity);
		return;
	}

	const char *base = strrchr(cd_path, '/');
	base = base ? base + 1 : cd_path;
	uint64_t h = fnv1a_64(base);
	snprintf(out, outsz, "%s/cd32-%016" PRIx64 ".nvr", AKIKO_NVRAM_DIR, h);
}

// One-shot rename of a physical disc's save from the old hashed name to the
// disc-named one. The hash was taken over exactly the identity now used
// verbatim, so the old path is reconstructible and the mapping is unambiguous.
//
// Renames rather than copies: leaving the hashed file behind would strand a
// real save somewhere the code no longer looks. Silent no-op when there is
// nothing to migrate, which is every case after the first mount of each disc.
static void migrate_physdisc_save_name(const char *cd_path, const char *new_path)
{
	if (strncmp(cd_path, AKIKO_PHYSDISC_PREFIX, AKIKO_PHYSDISC_PREFIX_LEN)) return;
	if (!new_path[0]) return;

	struct stat st;
	if (!stat(new_path, &st)) return;          // already on the new name

	const char *identity = cd_path + AKIKO_PHYSDISC_PREFIX_LEN;
	char cand[256];

	// Two hops are possible, and a disc can need either. The identity-named
	// file exists when discnames.cfg gained an entry after the disc had already
	// been played; the hashed file exists from before saves were named at all.
	// Try the nearer one first.
	snprintf(cand, sizeof(cand), "%s/%s.nvr", AKIKO_NVRAM_DIR, identity);
	if (strcmp(cand, new_path) && !stat(cand, &st))
	{
		if (!rename(cand, new_path))
		{
			akiko_diag("[akiko] save renamed %s -> %s", cand, new_path);
			return;
		}
		akiko_diag("[akiko] save rename %s -> %s FAILED (errno %d) — "
		           "the old file is left alone", cand, new_path, errno);
		return;
	}

	uint64_t h = fnv1a_64(identity);
	snprintf(cand, sizeof(cand), "%s/cd32-%016" PRIx64 ".nvr", AKIKO_NVRAM_DIR, h);
	if (stat(cand, &st)) return;               // no old save for this disc

	if (!rename(cand, new_path))
	{
		akiko_diag("[akiko] save renamed %s -> %s", cand, new_path);
	}
	else
	{
		akiko_diag("[akiko] save rename %s -> %s FAILED (errno %d) — "
		           "the old file is left alone", cand, new_path, errno);
	}
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

// "Is real CD media inserted?" -- matches WinUAE's `cdrom_disk != NULL` semantic.
// A placeholder drive (cfg=2, no filename) has present=0/cd=1 with chd_f=NULL,
// f=NULL -- that's "drive ready, no media", which WinUAE treats as no-disk for
// cmd_multi/pause/unpause. Checking present+cd alone would let MULTI fall into
// the scan-TOC branch and then PLAY AUDIO with a junk seekpos, trapping BIOS
// in the UNPAUSE polling loop and never letting it render the splash.
static bool cd_is_mounted(void)
{
	if (minimig_cd_drive_get(0)) return true;
	for (int p = 0; p < 2; p++) {
		for (int d = 0; d < 2; d++) {
			drive_t *drv = &ide_inst[p].drive[d];
			if (drv->cd && (drv->chd_f || drv->f || drv->phys)) {
				return true;
			}
		}
	}
	return false;
}

// Find the first CD drive that has real media. NULL if no drive has media.
//
// The dedicated CD32 slot comes first, then the IDE drives. Upstream mounts
// through the slot; a physical disc and anything mounted through hardfile[0]
// still land on ide_inst, and that is what autoboot, the per-game NVRAM key
// and the physical-disc read path all key off.
static drive_t *cd_find_drive(void)
{
	drive_t *slot = minimig_cd_drive_get(0);
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

// ---------------------------------------------------------------------------
// Disc identity for the OSD. Declared in akiko_cd32.h.
//
// The label is CACHED, and that is not an optimisation. Reading it costs a seek
// to LBA 16, the OSD redraws its rows on every refresh, and on a physical drive
// a seek during CD audio is something you can hear. So it is read at most once
// per mounted disc, on the first ask after the TOC exists, and the result --
// including "there is no label" -- is kept until the disc changes.
//
// Everything else (the track counts) is already in memory and is recomputed on
// every call, so a TOC that arrives late is not stuck behind a cache.
static char cd_label[64] = {};
static bool cd_label_known = false;

static void cd_label_forget(void)
{
	cd_label[0] = 0;
	cd_label_known = false;
}

// Where an ISO9660 volume descriptor's user data starts inside a RAW frame.
//
// We read raw because it is the one call that works for a CHD, a cue+bin and a
// physical drive alike. A raw frame carries 16 bytes of header before MODE1
// user data and 24 before MODE2 form 1. Rather than decide from the mode byte,
// look for the descriptor itself at each offset: that tests the thing we
// actually need, and a disc whose LBA 16 is not a volume descriptor at all
// then reports no label instead of 32 bytes of whatever was lying there.
static int pvd_user_offset(const uint8_t *raw)
{
	static const int cand[2] = { 16, 24 };
	for (int i = 0; i < 2; i++) {
		const uint8_t *u = raw + cand[i];
		// Type 1 (primary), the standard identifier, version 1.
		if (u[0] == 1 && !memcmp(u + 1, "CD001", 5) && u[6] == 1) return cand[i];
	}
	return -1;
}

// Copy an ISO9660 volume identifier out of a descriptor: 32 bytes at offset 40,
// space padded. Non-printable bytes become spaces rather than being dropped, so
// a mangled label still lines up with what other tools show for the same disc.
static void cd_label_from_pvd(const uint8_t *pvd)
{
	char lbl[33];
	memcpy(lbl, pvd + 40, 32);
	lbl[32] = 0;

	int end = 32;
	while (end > 0 && (lbl[end - 1] == ' ' || lbl[end - 1] == 0)) end--;
	lbl[end] = 0;

	for (int i = 0; i < end; i++) {
		if ((uint8_t)lbl[i] < 0x20 || (uint8_t)lbl[i] > 0x7E) lbl[i] = ' ';
	}

	snprintf(cd_label, sizeof(cd_label), "%s", lbl);
}

int akiko_cd32_disc_info(char *label, int labelsz, int *tracks, int *audio)
{
	if (label && labelsz > 0) label[0] = 0;
	if (tracks) *tracks = 0;
	if (audio)  *audio  = 0;

	drive_t *drv = cd_find_drive();
	if (!drv) return 0;

	// track_cnt includes the lead-out entry ide_cdrom.cpp appends after the
	// real tracks (cdrom_parse), which is not a track anything was written to.
	int real = drv->track_cnt > 0 ? drv->track_cnt - 1 : 0;
	const int max_tracks = (int)(sizeof(drv->track) / sizeof(drv->track[0]));
	if (real > max_tracks) real = max_tracks;

	if (tracks) *tracks = real;
	if (audio) {
		int n = 0;
		// attr & 0x40 is a data track; everything else is CDDA. Same test the
		// CDDA fetch uses, so the count cannot disagree with what will play.
		for (int i = 0; i < real; i++) if (!(drv->track[i].attr & 0x40)) n++;
		*audio = n;
	}

	if (!cd_label_known && real > 0) {
		cd_label_known = true;          // one attempt per disc, pass or fail
		if (drv->phys) {
			// The physical layer already knows how to find the descriptor on a
			// mixed-mode disc (its first DATA track, not LBA 16 of the disc),
			// so use it rather than a second, worse copy of that logic.
			char lbl[64];
			if (physical_disc_disc_label(lbl, sizeof(lbl)) > 0)
				snprintf(cd_label, sizeof(cd_label), "%s", lbl);
		}
		else {
			uint8_t raw[2352];
			if (!cdrom_read_raw_sector(drv, 16, raw)) {
				int off = pvd_user_offset(raw);
				if (off >= 0) cd_label_from_pvd(raw + off);
			}
		}
	}

	if (label && labelsz > 0) snprintf(label, labelsz, "%s", cd_label);
	return 1;
}

// Read the level the poll last announced to the guest, not the mount state.
// Between a swap's removal edge and its arrival there IS a drive with media,
// and telling the guest so at that moment collapses the swap back into no
// change at all.
static inline bool cd_media_present(void)
{
	return cd_media_level == CD_MEDIA_PRESENT;
}

// Read the bridge status word. Mirrors ide_check() but kept private to avoid
// clobbering the IDE poll's read of the same word.
static uint16_t akiko_read_status(void)
{
	uint16_t res;
	EnableIO();
	res = spi_w(AKIKO_STATUS_CMD);
	if (!res) res = spi_w(0);
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
	akiko_wait_status_bit(AKIKO_STATUS_RX_BUSY, true, 200);

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
	cd_media_push_pending = true;
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
	r[1] = cd_media_present() ? 0x00 : (CH_ERR_NODISK | cd_door);
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
	if (!cd_media_present()) {
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
	akiko_dbg("PAUSE (playing=%d, present=%d)\n", cd_playing, cd_media_present());
}

// 0x03 — UNPAUSE. WinUAE akiko.cpp:1027-1044. Same checkerr() pattern.
static void cmd_unpause(const uint8_t *cmd)
{
	uint8_t r[2];
	r[0] = cmd[0];
	if (!cd_media_present()) {
		r[1] = CH_ERR_NODISK | cd_door;
	} else {
		r[1] = (cd_playing ? CDS_PLAYING : 0) | cd_door;
	}
	cd_paused = 0;
	akiko_send_response(r, 2);
	akiko_dbg("UNPAUSE (playing=%d, present=%d)\n", cd_playing, cd_media_present());
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

	if (!cd_media_present()) {
		r[1] = 0x01;
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

			// Phase 3 item 7: point physical_disc's background prefetch lane
			// at the LBA the BIOS is about to stream. This is the whole reason
			// the worker thread exists -- without a hint its lane_cursor never
			// moves ahead of where we already read, so it prefetches nothing
			// and every sector request becomes a synchronous SG_IO burst
			// inside user_io_poll(). No-op on CHD.
			if (drv2 && drv2->phys) physical_disc_seek_hint((int)s_lba);
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
static void akiko_ext_block_write(uint16_t addr, const uint8_t *buf, int bytes)
{
	static uint16_t words[AKIKO_SECTOR_BYTES / 2];
	memcpy(words, buf, bytes);

	int total = bytes / 2;

	EnableIO();
	fpga_spi_fast(UIO_DMA_WRITE);
	fpga_spi_fast(addr);
	fpga_spi_fast(0);
	int off = 0;
	while (off < total) {
		int n = total - off < AKIKO_FILL_CHUNK_WORDS ? total - off : AKIKO_FILL_CHUNK_WORDS;
		fpga_spi_fast_block_write(words + off, n);
		off += n;
		if (off < total) usleep(AKIKO_FILL_GAP_US);
	}
	DisableIO();
}

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
// AmigaCD.sv asserts sd_rd[0] with sd_lba=0, hps_io reads the file and
// streams every byte on sd_buff_dout / sd_buff_wr / sd_buff_addr, and
// AmigaCD.sv pipes that straight into akiko_nvram.load_we. Sidesteps
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

	uint8_t nvr[AKIKO_NVRAM_BYTES];
	FILE *f = fopen(path, "rb");
	if (!f) {
		akiko_diag("[akiko] NVR load FAIL: fopen(%s) errno=%d", path, errno);
		cd_save_load_failed = true;
		return false;
	}
	size_t got = fread(nvr, 1, sizeof(nvr), f);
	fclose(f);
	if (got != sizeof(nvr)) {
		akiko_diag("[akiko] NVR load FAIL: read %u of %d bytes from %s",
		           (unsigned)got, AKIKO_NVRAM_BYTES, path);
		cd_save_load_failed = true;
		return false;
	}

	akiko_ext_block_write(AKIKO_NVRAM_ADDR, nvr, AKIKO_NVRAM_BYTES);

	akiko_diag("[akiko] NVR loaded (%d bytes from %s) — verify in 100 ms",
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

// Push one sector to the akiko sector_buffer as 1176 16-bit words over the
// ext bus. The old hps_io disk-slot path (UIO_SECTOR_RD on a dedicated VDNUM
// slot, ~0.95 ms/sector) is gone: akiko.v takes words now, and the word-wide
// push is both faster and does not burn a virtual disk slot.
static void akiko_push_sector(const uint8_t *buf)
{
	// Hold each sector to AKIKO_SECTOR_PERIOD_US from the start of the last
	// one. Measured from start to start rather than slept after the write, so
	// the time a CHD miss or a physical-drive seek already spent counts towards
	// the interval instead of being added to it -- a slow sector is never made
	// slower by this.
	static int64_t prev_us = -1;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int64_t now_us = (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;

	const int64_t wait_us = akiko_pace_delay_us(prev_us, now_us, AKIKO_SECTOR_PERIOD_US);
	if (wait_us > 0) {
		usleep((useconds_t)wait_us);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now_us = (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
	}
	prev_us = now_us;

	akiko_ext_block_write(AKIKO_SECTOR_ADDR, buf, AKIKO_SECTOR_BYTES);
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
	if (!drv || !buf2352) return false;
	if (!drv->chd_f && !drv->phys && !drv->f) return false;

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

	if (!track) return false;
	// attr & 0x40 == 0 → audio (CDDA). Refuse to play data tracks as audio.
	if (track->attr & 0x40) return false;
	if (track->sectorSize != AKIKO_CDDA_BYTES) return false;

	if (drv->phys) {
		if (physical_disc_read_sector((int)lba, buf2352, NULL)) return false;
		// READ CD (flags 0x10) returns audio in disc order, i.e.
		// little-endian per 16-bit sample. CHD stores it big-endian, and
		// akiko_push_audio_sector byte-swaps unconditionally on the way to
		// the FIFO. Normalise to CHD order here so the push path stays
		// source-agnostic. Getting this wrong is immediately audible:
		// white noise, not silence.
		uint16_t *s = (uint16_t *)buf2352;
		for (int i = 0; i < AKIKO_CDDA_BYTES / 2; i++) s[i] = bswap_16(s[i]);
		return true;
	}

	if (drv->chd_f) {
		return cdrom_read_raw_sector(drv, lba, buf2352) == 0;
	}

	// Raw cue+bin. The track file holds audio in disc order, so it needs the
	// same swap the physical path does, just done in place afterwards.
	track_t *read_track = track;
	if (index0 && track->number > 1) {
		read_track = &drv->track[track->number - 2];
	}
	if (!cdrom_read_track_raw(read_track, lba, buf2352, AKIKO_CDDA_BYTES)) return false;

	int16_t *samples = (int16_t *)buf2352;
	for (int i = 0; i < AKIKO_CDDA_BYTES / 2; i++) {
		samples[i] = (int16_t)bswap_16((uint16_t)samples[i]);
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
	// Phase 3 item 8: a physical drive bypasses this cache entirely.
	//
	// On a miss, akiko_prefetch_fill reads AKIKO_PREFETCH_SECTORS (128)
	// sectors synchronously. Against a CHD that is cheap -- decompress a few
	// hunks out of the page cache. Against a real drive, each ring miss in
	// physical_disc costs a blocking SG_IO of SYNC_IO_BURST (8) sectors with
	// a 3-second timeout, so one cold window is ~16 blocking round trips
	// executed inside user_io_poll(), to satisfy a request for ONE sector.
	// That is the freeze described under "Blocking I/O in the main loop" in
	// docs/integration-plan.md, not a throughput nicety.
	//
	// physical_disc already caches: a 4096-sector two-lane ring (8x this
	// window cache) with a background worker filling ahead of lane_cursor,
	// aimed by physical_disc_seek_hint above. Stacking a second, synchronous,
	// 128-sector cache on top of an asynchronous one only converts prefetched
	// sectors back into blocking reads.
	//
	// -2 rather than -1 on failure: -1 means "the whole batch failed" and
	// there is no batch here. -2 is the caller's single-bad-sector path,
	// which holds sec_req and lets BIOS retry -- the right behaviour for a
	// disc that is merely dirty. Both land in the same bounded-retry logic.
	if (drv->phys) {
		return (cdrom_read_raw_sector(drv, lba, out_buf) == 0) ? 0 : -2;
	}

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
static bool akiko_handle_sec_req(void)
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
		return false;
	}

	drive_t *drv = cd_find_drive();
	if (!drv) {
		akiko_dbg("sec_req but no CD drive (counter=%u)\n", counter);
		return false;
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
		return false;
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
		return true;
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
			return false;
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
		if (fail_count < MAX_FAIL_RETRIES) return false;
		akiko_diag("[akiko] sec_req lba=%u retry cap reached — push silence, advance", lba);
		fail_lba = UINT32_MAX;
		memset(buf, 0, sizeof(buf));
		akiko_push_sector(buf);
		return true;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts2);

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
	return true;
}

// -----------------------------------------------------------------------------
// Bus trace drain (debug)
// -----------------------------------------------------------------------------

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
	cd_media_event = true;
	// Before the same-slot early return below. That return asks whether the
	// SAVE FILE changed; this cache is about the disc in the drive. A remount
	// of the same image hashes to the same save and returns early, and the
	// label should still be re-read -- it costs one seek, once.
	cd_label_forget();

	char new_path[256] = {0};
	if (path && *path) {
		compute_save_path(path, new_path, sizeof(new_path));
		// Before the same-slot check below, so a disc mounted for the first
		// time after this change picks up its existing save rather than
		// starting from the .mif baseline.
		migrate_physdisc_save_name(path, new_path);
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
	cd_media_level   = CD_MEDIA_UNKNOWN;
	cd_media_push_pending = false;
	cd_media_event   = false;
	cd_media_swap_absent   = false;
	cd_media_swap_ready_ms = 0;
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

// Is the core actually configured as a CD32? From MiSTer-devel/Main_MiSTer
// 7d88019 (Kasper Olesen), the only functional line in the whole upstreaming of
// this file that we did not already have -- see
// docs/mister-upstream-merge-assessment.md.
//
// 68020 + AGA + a removable CD slot. Checked against every AmigaCD profile:
// CD32 stays true with D-Cache or the 14 MHz throttle toggled (both live in
// other bits of minimig_config.cpu), and CDTV is false because it is a 68000.
//
// Note hardfile[0].cfg stays 2 with no disc mounted -- ejecting clears the
// filename, not the slot type -- so this does not disturb autoboot, which
// depends on the poll continuing to run before anything is inserted.
// Upstream's dedicated CD32 drive slot is one way to say "this is a CD32"; the
// CPU/chipset/hardfile test below is the other, and is what every AmigaCD
// profile uses. Either is enough.
static inline bool cd32_active(void)
{
	if (minimig_config.cd32_drive.cfg != 0) return true;
	return (minimig_config.cpu & 0x03) == 3
	    && ((minimig_config.chipset >> 2) & 7) == 6
	    && minimig_config.hardfile[0].cfg == 2;
}

void akiko_cd32_poll(void)
{
	const bool mounted = cd32_active() && cd_is_mounted();
	const cd_media_level_t level = mounted ? CD_MEDIA_PRESENT : CD_MEDIA_ABSENT;

	if (level != cd_media_level || cd_media_event) {
		const bool swap = cd_media_event && (level == CD_MEDIA_PRESENT)
		                  && (cd_initialized >= 2);
		cd_media_event   = false;
		cd_data_lba_base = -1;
		akiko_prefetch_invalidate();
		cd_cdda_lba_next = -1;
		cd_cdda_lba_end  = -1;
		cd_cdda_drv      = NULL;
		toc_point_count  = 0;
		toc_push_idx     = -1;
		cd_media_level   = level;
		cd_media_push_pending = true;
		if (swap) cd_media_swap_absent = true;
		if (level == CD_MEDIA_PRESENT) akiko_build_toc();
		akiko_diag("[akiko] media level -> %s (cd_init=%u, announce pending)",
		           (level == CD_MEDIA_PRESENT) ? "present" : "absent",
		           cd_initialized);
	}

	if (!cd32_active()) return;

	// Phase 32.6 P3 (rolled back): tried replacing the floor with a SEC_REQ-
	// drop barrier inside akiko_handle_sec_req, but CF then re-armed PLAY at
	// 250 Hz (15k arms in 60s vs ~1 Hz baseline) and stalled at black screen.
	// The framed RX/TX barriers (rx_busy, req-clear) are sufficient on their
	// own only because their state bit is observable; the sec channel has
	// no analogous bit (sec_done is internal to the bridge), so a small
	// floor remains the simplest correct synchronisation. 20µs was the
	// original known-good value.
	usleep(20);

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

#if AKIKO_CD32_DEBUG
	// While we don't think we're mounted, periodically dump the underlying
	// ide_inst flags so we can see what state the IDE subsystem is in.
	// This burns one log line per second until something flips.
	static int unmounted_dump_throttle = 0;
	if (!mounted && (unmounted_dump_throttle++ % 60) == 0 && unmounted_dump_throttle < 5*60) {
		akiko_diag(
			"[akiko] no drive with media (slot=%p)",
			(void*)minimig_cd_drive_get(0)
		);
	}
#endif

	// Status poll first, so the unsolicited pushes below (auto-init, the
	// media-status edge, the TOC drip) can be gated on the FPGA's rx_busy
	// bit: when it is set the RX engine still has a queued or in-flight
	// response in result_buffer, and pushing now would overwrite it. This is
	// the mirror of WinUAE's cdrom_can_return_data().
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

	if (cd_initialized == 0 && rx_idle) {
		if (cd_media_present()) {
			uint8_t r[2];
			r[0] = 0x0a;
			r[1] = 0x01;
			akiko_send_response(r, 2);
			akiko_diag("[akiko] auto-init media-status push: opcode=0x0a status=0x01");
		}
		cd_initialized = 1;
		return;
	}

	// The removal half of a swap, sent before the arrival even though a disc is
	// mounted, so cd_media_present() is not the source of the status here.
	if (cd_media_swap_absent && cd_initialized >= 2 && rx_idle) {
		uint8_t r[2];
		r[0] = 0x0a;                                         // CDS_MEDIA_STATUS opcode
		r[1] = 0x00;                                         // media absent
		akiko_send_response(r, 2);
		cd_media_swap_absent   = false;
		cd_media_swap_ready_ms = (uint32_t)GetTimer(AKIKO_SWAP_SETTLE_MS);
		akiko_diag("[akiko] swap: media-status push opcode=0x0a status=0x00 "
		           "(removal edge, arrival held %u ms)", AKIKO_SWAP_SETTLE_MS);
		return;
	}

	if (cd_media_push_pending && cd_initialized >= 2 && rx_idle
	    && (!cd_media_swap_ready_ms || CheckTimer(cd_media_swap_ready_ms))) {
		cd_media_swap_ready_ms = 0;
		uint8_t r[2];
		r[0] = 0x0a;
		r[1] = cd_media_present() ? 0x01 : 0x00;
		akiko_send_response(r, 2);
		cd_media_push_pending = false;
		akiko_build_toc();
		akiko_diag("[akiko] media-status push: opcode=0x0a status=0x%02x", r[1]);
		return;
	}

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
		if (akiko_handle_sec_req()) return;
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

// ---------------------------------------------------------------------------
// Save state, host side. See akiko_cd32.h for why this exists at all.
// ---------------------------------------------------------------------------

// Versioned and fixed-layout. Every field is a fixed-width type and the struct
// is written and read as raw bytes, so a build that changes the layout has to
// change the version with it -- akiko_cd32_state_load refuses an unknown one
// rather than reinterpreting the bytes, because a drive told to seek to a
// plausible wrong place is worse than one told nothing.
#define AKIKO_SS_MAGIC   0x414B5353u   // "AKSS"
#define AKIKO_SS_VERSION 1u

typedef struct
{
	uint32_t magic;
	uint32_t version;

	// Auto-init handshake. 0 = drive cold, 1 = media status sent, 2 = the BIOS
	// has issued INFO and the drive is up. Restoring this is what stops the
	// restored machine re-running an init the game finished long ago.
	uint8_t  initialized;
	uint8_t  paused;
	uint8_t  playing;
	uint8_t  door;
	uint8_t  led_state;
	uint8_t  speed;            // 1x or 2x, as cmd_multi last set it
	uint8_t  pad0;
	uint8_t  pad1;

	uint32_t play_start_lba;
	uint32_t play_end_lba;

	// The data read position. cd_data_lba_base pairs with the FPGA's
	// cdrom_sector_counter, which travels in the core payload: userspace serves
	// base + counter, so the two only mean anything together.
	int32_t  data_lba_base;

	// CDDA. q_pos and q_end survive a natural end so SUBQ keeps answering;
	// lba_next and lba_end are the pump's own cursor.
	int32_t  cdda_q_pos;
	int32_t  cdda_q_end;
	int32_t  cdda_lba_next;
	int32_t  cdda_lba_end;

	uint8_t  cdda_active;      // whether cd_cdda_drv was non-NULL
	uint8_t  last_mounted;
	uint8_t  pad2;
	uint8_t  pad3;
} akiko_ss_state_t;

int akiko_cd32_state_size(void)
{
	return (int)sizeof(akiko_ss_state_t);
}

void akiko_cd32_state_save(void *buf)
{
	akiko_ss_state_t s;
	memset(&s, 0, sizeof(s));
	s.magic          = AKIKO_SS_MAGIC;
	s.version        = AKIKO_SS_VERSION;
	s.initialized    = cd_initialized;
	s.paused         = cd_paused;
	s.playing        = cd_playing;
	s.door           = cd_door;
	s.led_state      = cd_led_state;
	s.speed          = (uint8_t)cdrom_speed;
	s.play_start_lba = cd_play_start_lba;
	s.play_end_lba   = cd_play_end_lba;
	s.data_lba_base  = cd_data_lba_base;
	s.cdda_q_pos     = cd_cdda_q_pos;
	s.cdda_q_end     = cd_cdda_q_end;
	s.cdda_lba_next  = cd_cdda_lba_next;
	s.cdda_lba_end   = cd_cdda_lba_end;
	s.cdda_active    = (cd_cdda_drv != NULL) ? 1 : 0;
	// The field predates upstream's media-level state machine, which replaced
	// the cd_last_mounted flag it used to carry. cd_media_level means the same
	// thing -- what the guest was last told -- so the on-disk .ss format stays
	// as it was.
	s.last_mounted   = (cd_media_level == CD_MEDIA_PRESENT) ? 1 : 0;
	memcpy(buf, &s, sizeof(s));
}

bool akiko_cd32_state_load(const void *buf, int len)
{
	akiko_ss_state_t s;
	if (len < (int)sizeof(s)) return false;
	memcpy(&s, buf, sizeof(s));
	if (s.magic != AKIKO_SS_MAGIC || s.version != AKIKO_SS_VERSION) return false;

	cd_initialized    = s.initialized;
	cd_paused         = s.paused;
	cd_playing        = s.playing;
	cd_door           = s.door;
	cd_led_state      = s.led_state;
	cdrom_speed       = s.speed ? s.speed : 1;
	cd_play_start_lba = s.play_start_lba;
	cd_play_end_lba   = s.play_end_lba;
	cd_data_lba_base  = s.data_lba_base;
	cd_cdda_q_pos     = s.cdda_q_pos;
	cd_cdda_q_end     = s.cdda_q_end;
	cd_cdda_lba_next  = s.cdda_lba_next;
	cd_cdda_lba_end   = s.cdda_lba_end;
	cd_media_level    = s.last_mounted ? CD_MEDIA_PRESENT : CD_MEDIA_ABSENT;

	// cd_cdda_drv is a POINTER into this run's drive table and cannot be
	// carried across a core reload. It is re-derived instead: the pump only
	// needs to know a drive is there, and cd_find_drive answers that for the
	// disc mounted now. If the user swapped discs between save and restore the
	// LBAs are meaningless anyway and there is nothing better to do than aim
	// them at the disc that is actually in.
	cd_cdda_drv = s.cdda_active ? cd_find_drive() : NULL;
	if (!cd_cdda_drv) {
		cd_cdda_lba_next = -1;
		cd_cdda_lba_end  = -1;
	}

	// The prefetch windows hold sectors from wherever the drive was before the
	// restore. They are a cache of the disc, not state of the machine, so the
	// honest thing is to drop them and let them refill from the restored
	// position -- keeping them risks serving a sector from the old stream.
	akiko_prefetch_invalidate();

	akiko_diag("[akiko] state restored: init=%u base=%d cdda=%d..%d speed=%ux",
	           cd_initialized, cd_data_lba_base,
	           cd_cdda_lba_next, cd_cdda_lba_end, cdrom_speed);
	return true;
}
