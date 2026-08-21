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
#define SS_VERSION  0x00010002u   // major 1, minor 2

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

// Validates magic, version, bounds and the core payload CRC. Returns false and
// leaves *out untouched on any failure. A window the host memset and then
// partially filled from a truncated file will fail here rather than restore.
bool ss_header_parse(const uint32_t *win, uint32_t win_words, ss_header_t *out);

// Word offset of the host block: immediately after the core payload.
uint32_t ss_host_block_offset(const ss_header_t *h);

// Records a host block that has already been written at ss_host_block_offset(),
// updating host_words, host_crc32 and length.
void ss_header_set_host_block(uint32_t *win, uint32_t host_words, uint32_t host_crc);

#endif
