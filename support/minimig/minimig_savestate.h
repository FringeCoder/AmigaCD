#ifndef MINIMIG_SAVESTATE_H
#define MINIMIG_SAVESTATE_H

#include <stdint.h>

#include "minimig_crc32.h"

// Save state payload format, version 1.0. See
// docs/superpowers/specs/2026-08-08-amigacd-savestates-design.md.
//
// The core writes words 0-6 and the core payload; the host appends its own
// block after the core payload and updates words 1, 5 and 7. All values are
// little-endian.

#define SS_MAGIC    0x53534341u   // reads "ACSS" in a byte dump
// 1.1 added Denise's 256-entry colour table between the register shadow and
// chip RAM. A 1.0 file has a different core_words and every section after the
// shadow at a different offset, so it must be refused rather than misread.
// 1.4 added the register shadow's WRITTEN mask: eight words directly after the
// 256 shadow entries, one bit per entry, saying whether the machine ever
// wrote that register. The core replays only entries whose bit is set. A 1.3
// file has no mask; ss_upgrade_13_to_14() below inserts one with every bit
// set, which replays exactly what 1.3 replayed, so the core need only know
// 1.4. That is the only backward step this format has ever taken, and the
// reason it could: the core's payload layout is fixed, so the insert point is
// a constant.
// 1.5 added the 68030's MMU registers to the core's state vector, which grew
// the state section from 66 words to 74. The new fields are the top of the
// vector, so the eight words are the END of the state section, and
// ss_upgrade_14_to_15() inserts them as zeros -- TC zero, translation off,
// which is the machine every 1.4 file describes (1.4 refused the 68030's MMU).
// The core reads only 1.5; the host upgrades 1.3 to 1.4 to 1.5 in turn.
#define SS_VERSION    0x00010005u   // major 1, minor 5
#define SS_VERSION_14 0x00010004u   // the last layout without the MMU
#define SS_VERSION_13 0x00010003u   // the last layout without a mask

// Where the mask goes in a 1.3 payload: after the Kickstart fingerprint (one
// word), the state vector and the 256 shadow entries (two per word). The
// state vector width is `SS_STATE_W in the core's rtl/ss_state.vh: 2093 bits
// at 1.3 and 1.4, 2099 and then 2106 as SFC/DFC and the upper CACR went into
// its padding (no word count change), and 2346 at 1.5 with the MMU. These
// constants describe the layouts they upgrade FROM, so they do not move with
// the core any more; SS_STATE_WORDS is the current one, which does --
// tests/test_savestate_format.cpp pins both.
#define SS_KICK_META_WORDS   1u
#define SS_STATE_WORDS_14    66u              // 1.3 and 1.4: (2106 + 31) / 32
#define SS_STATE_WORDS       74u              // 1.5: (2346 + 31) / 32
#define SS_MMU_WORDS         (SS_STATE_WORDS - SS_STATE_WORDS_14)
#define SS_SHADOW_ENTRIES    256u
#define SS_MASK_WORDS        8u               // 256 bits
#define SS_MASK_OFFSET_13    (SS_KICK_META_WORDS + SS_STATE_WORDS_14 + SS_SHADOW_ENTRIES / 2u)
// Where the MMU words go in a 1.4 payload: the end of its state section.
#define SS_MMU_OFFSET_14     (SS_KICK_META_WORDS + SS_STATE_WORDS_14)

// 12 MB slot / 4 bytes per word. The slot grew from 4 MB when Zorro II fast
// RAM joined the payload: 2 MB chip + 1.5 MB slow + 8 MB fast is 11.5 MB, and
// every slot has to hold the largest case. Must match the core's CONF_STR
// ("SS3C000000:C00000") -- the host reads the size from there at startup, and
// this is only the bound used when validating a file against it.
#define SS_MAX_WORDS (0xC00000u / 4u)

// Words 2..7 inclusive: the part of the header that follows `length`.
#define SS_HEADER_TAIL_WORDS 6u

typedef struct
{
	uint32_t counter;
	uint32_t length;
	uint32_t magic;
	uint32_t version;
	uint32_t core_words;
	uint32_t host_words;
	uint32_t core_crc32;
	uint32_t host_crc32;
} ss_header_t;

// Upgrades a 1.3 image in place to 1.4 by inserting the WRITTEN mask, every
// bit set. `win_words` is the whole window, which must have room for eight
// more words after the file's content. Returns 1 if upgraded, 0 if the image
// was not a valid 1.3 file or would not fit (in which case it is untouched);
// a 1.4 image is left alone and also returns 0.
int ss_upgrade_13_to_14(uint32_t *win, uint32_t win_words);

// Upgrades a 1.4 image in place to 1.5 by inserting the MMU words, all zero,
// at the end of the state section. Same contract as above: 1 if upgraded, 0
// (and untouched) otherwise. Run it after ss_upgrade_13_to_14() so a 1.3 file
// takes both steps.
int ss_upgrade_14_to_15(uint32_t *win, uint32_t win_words);

// Validates magic, version, bounds and the core payload CRC. Returns false and
// leaves *out untouched on any failure. A window the host memset and then
// partially filled from a truncated file will fail here rather than restore.
bool ss_header_parse(const uint32_t *win, uint32_t win_words, ss_header_t *out);

// Word offset of the host block: immediately after the core payload.
uint32_t ss_host_block_offset(const ss_header_t *h);

// Records a host block that has already been written at ss_host_block_offset(),
// updating host_words, host_crc32 and length.
void ss_header_set_host_block(uint32_t *win, uint32_t host_words, uint32_t host_crc);

// Copies `bytes` of host state into the window after the core payload and
// updates the header to match. Rounds up to a whole number of words, zeroing
// the tail, because every length in the header counts words.
//
// Returns false if the block will not fit in win_words, in which case the
// window is left exactly as it was -- a save with no host block is a save that
// restores the machine and not the drive, which is what every state written
// before this existed already did.
bool ss_write_host_block(uint32_t *win, uint32_t win_words,
                         const void *data, uint32_t bytes);

// The reverse: locates the host block and checks its CRC. Returns NULL when
// there is none (host_words == 0, i.e. a file written before host state was
// carried) or when it does not verify.
const void *ss_read_host_block(const uint32_t *win, const ss_header_t *h,
                               uint32_t *bytes_out);

// ---------------------------------------------------------------------------
// When the host half of a restore may be applied.
//
// The drive's saved position (the host block) has to be in place before the
// restored machine asks for its first sector, and must NOT be applied at all
// when the core refuses the file: a refusal releases the Amiga exactly as it
// was, and a drive aimed at the save's position under it loses the read the
// game had in flight. Measured on hardware 2026-10-23 -- Flink, a 1.3 file
// with fast RAM restored without any, FAIL_LENGTH, and the game never ran
// again: every sector request after it found no read armed.
//
// ss_ctrl.v freezes the machine first and validates under the freeze. Every
// load_reject() is in a state before S_L_ST_FETCH; from S_L_ST_FETCH to
// S_L_RELEASE the restore is committed and the machine is still frozen. That
// is the window in which to apply. The host sees it by polling the core's live
// status (minimig_ssdiag.cpp's 0xF600 window) after raising the request.
// ---------------------------------------------------------------------------

// ss_ctrl.v's state numbers, the ones this decision needs. They are the RTL's
// localparams, not ours; minimig_ssdiag.cpp names the rest.
enum
{
	SS_ST_IDLE         = 0,
	SS_ST_L_RD_ISSUE   = 11,
	SS_ST_L_RD_WAIT    = 12,
	SS_ST_L_RD_DATA    = 13,
	SS_ST_L_MAGIC      = 14,
	SS_ST_L_LEN_CAP    = 15,
	SS_ST_L_LEN_CHK    = 16,
	SS_ST_L_CRC_CAP    = 17,
	SS_ST_L_PAY_FETCH  = 18,
	SS_ST_L_PAY_FEED   = 19,
	SS_ST_L_FREEZE     = 20,
	SS_ST_L_ST_FETCH   = 21,
	SS_ST_L_ST_FEED    = 22,
	SS_ST_L_ST_WAIT    = 23,
	SS_ST_L_CH_FETCH   = 24,
	SS_ST_L_CH_ISSUE   = 25,
	SS_ST_L_CH_WAIT    = 26,
	SS_ST_L_RELEASE    = 27,
	SS_ST_KICK_ISSUE   = 28,
	SS_ST_KICK_WAIT    = 29,
	SS_ST_KICK_DRAIN   = 30,
	SS_ST_L_KICK_CHK   = 32,
	SS_ST_L_FLUSH      = 33,
	SS_ST_L_SH_FETCH   = 36,
	SS_ST_L_SH_LOAD    = 37,
	SS_ST_L_REPLAY     = 38,
	SS_ST_L_CLUT_FETCH = 53,
	SS_ST_L_CLUT_LOAD  = 54,
	SS_ST_L_FA_FETCH   = 62,
	SS_ST_L_FA_LOAD    = 63,
};

// One read of the core's live status.
typedef struct
{
	uint8_t  live;        // ss_ctrl's state register
	uint8_t  load_busy;
	uint8_t  load_ok;     // sticky until the next restore arms
	uint8_t  load_fail;   // likewise
	uint8_t  fail_code;   // ss_ctrl's load_fail_code, meaningful with load_fail
	uint16_t seq;         // counts state changes; 16 bits, wraps
} ss_core_obs_t;

typedef struct
{
	uint16_t seq0;        // seq read before the request was raised
	int      seen_busy;
} ss_restore_watch_t;

typedef enum
{
	SS_WATCH_WAIT,        // not decided yet: poll again
	SS_WATCH_APPLY,       // committed: apply the host block now
	SS_WATCH_SKIP,        // refused: leave the drive alone
} ss_watch_t;

void       ss_restore_watch_begin(ss_restore_watch_t *w, uint16_t seq_before);
ss_watch_t ss_restore_watch_step(ss_restore_watch_t *w, ss_core_obs_t o);

// ss_ctrl's FAST_WORDS: the 8 MB of Zorro II fast RAM a payload carries when
// the machine had it. core_words is a fixed part plus this or nothing, and the
// fixed part is well under it.
#define SS_FAST_WORDS 0x200000u
// ss_ctrl's load_fail_code for a payload of the wrong size.
#define SS_FAIL_LENGTH 3u

// The toast for a file the core refused as the wrong length (FAIL_LENGTH),
// from the file's own core_words; 0 when the file says nothing useful. The
// core's text, "Restore: not for this core", sent people looking for a build
// mismatch when the cause was the Fast RAM setting.
const char *ss_length_refusal_text(uint32_t file_core_words);

#endif
