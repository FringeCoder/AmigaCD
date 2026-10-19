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
#define SS_VERSION    0x00010004u   // major 1, minor 4
#define SS_VERSION_13 0x00010003u   // the last layout without a mask

// Where the mask goes in a 1.3 payload: after the Kickstart fingerprint (one
// word), the state vector and the 256 shadow entries (two per word). The
// state vector width is `SS_STATE_W in the core's rtl/ss_state.vh, 2093 bits
// as of 1.4 and 2099 since SFC/DFC went into its padding (2026-10-18, same
// word count, no version change), and this constant has to move with it --
// tests/test_savestate_format.cpp pins it, and the core's ss_ctrl_tb pins the
// same number from the RTL side.
#define SS_KICK_META_WORDS   1u
#define SS_STATE_WORDS       66u              // (2099 + 31) / 32
#define SS_SHADOW_ENTRIES    256u
#define SS_MASK_WORDS        8u               // 256 bits
#define SS_MASK_OFFSET_13    (SS_KICK_META_WORDS + SS_STATE_WORDS + SS_SHADOW_ENTRIES / 2u)

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

#endif
