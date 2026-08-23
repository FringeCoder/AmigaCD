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

	// core_words and host_words are raw, untrusted uint32_t values straight
	// from the file. Adding two of them can wrap a uint32_t, and a wrapped
	// sum would slip past every check below (and later alias the host block
	// onto unrelated memory via ss_host_block_offset()). Do the bounds
	// arithmetic in 64 bits so no attainable pair of values can wrap.
	uint64_t core_words = h.core_words;
	uint64_t host_words = h.host_words;

	// length counts everything after word 1.
	if (h.length < SS_HEADER_TAIL_WORDS) return false;
	if (h.length > win_words - 2) return false;
	if (core_words + host_words != (uint64_t)h.length - SS_HEADER_TAIL_WORDS) return false;

	// Core payload starts at word 8 and must fit.
	if (8u + core_words + host_words > win_words) return false;

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

bool ss_write_host_block(uint32_t *win, uint32_t win_words,
                         const void *data, uint32_t bytes)
{
	ss_header_t h;
	if (!ss_header_parse(win, win_words, &h)) return false;

	const uint32_t words = (bytes + 3u) / 4u;
	const uint32_t off   = ss_host_block_offset(&h);
	if ((uint64_t)off + words > win_words) return false;

	// Zero first, then copy: the last word is usually partial and the padding
	// has to be deterministic or the CRC changes for the same state.
	memset(&win[off], 0, words * 4u);
	memcpy(&win[off], data, bytes);
	ss_header_set_host_block(win, words,
	                         crc32_compute((const uint8_t *)&win[off], (int)(words * 4u)));
	return true;
}

const void *ss_read_host_block(const uint32_t *win, const ss_header_t *h,
                               uint32_t *bytes_out)
{
	if (!h->host_words) return 0;

	const uint32_t off = ss_host_block_offset(h);
	if (crc32_compute((const uint8_t *)&win[off], (int)(h->host_words * 4u))
	    != h->host_crc32) return 0;

	if (bytes_out) *bytes_out = h->host_words * 4u;
	return &win[off];
}
