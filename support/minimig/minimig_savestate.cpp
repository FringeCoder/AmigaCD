// Save state payload format helpers.
//
// Deliberately free of MiSTer headers so tests/Makefile can build it with the
// host compiler. Only <stdint.h>, <string.h> and our own headers.

#include <stdint.h>
#include <string.h>

#include "minimig_savestate.h"

bool ss_header_parse(const uint32_t *win, uint32_t win_words, ss_header_t *out)
{
	if (!win || !out) return false;
	if (win_words < 8) return false;

	ss_header_t h;
	h.counter    = win[0];
	h.length     = win[1];
	h.magic      = win[2];
	h.version    = win[3];
	h.core_words = win[4];
	h.host_words = win[5];
	h.core_crc32 = win[6];
	h.host_crc32 = win[7];

	if (h.magic != SS_MAGIC) return false;
	if (h.version != SS_VERSION) return false;

	// length counts everything after word 1.
	if (h.length < SS_HEADER_TAIL_WORDS) return false;
	if (h.length > win_words - 2) return false;
	if (h.core_words + h.host_words != h.length - SS_HEADER_TAIL_WORDS) return false;

	// Core payload starts at word 8 and must fit.
	if (8u + h.core_words + h.host_words > win_words) return false;

	if (crc32_compute((const uint8_t *)&win[8], (int)(h.core_words * 4)) != h.core_crc32)
		return false;

	*out = h;
	return true;
}

uint32_t ss_host_block_offset(const ss_header_t *h)
{
	return 8u + h->core_words;
}

void ss_header_set_host_block(uint32_t *win, uint32_t host_words, uint32_t host_crc)
{
	win[5] = host_words;
	win[7] = host_crc;
	win[1] = SS_HEADER_TAIL_WORDS + win[4] + host_words;
}
