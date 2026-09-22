// Native unit tests for the save state payload format.
// Built by tests/Makefile with the host compiler, not the ARM cross-compiler.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../support/minimig/minimig_savestate.h"

static int failures = 0;

static void check(const char *name, bool ok)
{
	if (ok) printf("PASS %s\n", name);
	else { printf("FAIL %s\n", name); failures++; }
}

// Build a minimal well-formed window: 3 words of core payload, no host block.
static void build_window(uint32_t *win, uint32_t core_words)
{
	memset(win, 0, SS_MAX_WORDS * 4);
	win[0] = 0x00000001;
	win[1] = 6 + core_words;
	win[2] = SS_MAGIC;
	win[3] = SS_VERSION;
	win[4] = core_words;
	win[5] = 0;
	win[6] = crc32_compute((const uint8_t *)&win[8], core_words * 4);
	win[7] = 0;
	for (uint32_t i = 0; i < core_words; i++) win[8 + i] = 0xA5A50000 + i;
	// CRC must be taken after the payload is filled.
	win[6] = crc32_compute((const uint8_t *)&win[8], core_words * 4);
}

int main(void)
{
	// Static, not automatic: the window is 4 MB and the default thread stack
	// on the Windows dev machine is 1 MB, so an automatic one overflows the
	// stack before main() runs a single check. Linux CI never noticed.
	static uint32_t win[SS_MAX_WORDS];
	ss_header_t h;

	build_window(win, 3);
	check("valid header parses", ss_header_parse(win, SS_MAX_WORDS, &h));
	check("core_words read back", h.core_words == 3);
	check("host block follows core payload", ss_host_block_offset(&h) == 11);

	build_window(win, 3);
	win[2] = 0xDEADBEEF;
	check("bad magic rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	win[3] = 0x00020000;
	check("unknown version rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	win[9] ^= 1;
	check("corrupt payload rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	win[1] = SS_MAX_WORDS + 100;
	check("length past window rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	// core_words + host_words must not be allowed to wrap a uint32_t. Here
	// core_words = 0xFFFFFFF9 and host_words = 10 wrap to 3, which matches
	// length - SS_HEADER_TAIL_WORDS (9 - 6) and slips under win_words in the
	// naive additive bounds checks. core_crc32 = 0 matches what the buggy CRC
	// call produces for a negative cast length, so only the overflow guard
	// itself can catch this.
	build_window(win, 3);
	win[1] = 9;
	win[4] = 0xFFFFFFF9u;
	win[5] = 10;
	win[6] = 0;
	check("overflowing core_words+host_words rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	// A truncated file arrives as a zero tail because the host memsets the
	// window before reading (user_io.cpp:1998). That must not parse.
	memset(win, 0, sizeof(win));
	check("all-zero window rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	// The host block. It carries the CD drive position, which the core cannot
	// capture, so a file written without one has to keep restoring exactly as
	// it always did rather than being refused or misread.
	build_window(win, 3);
	check("no host block reads as absent",
	      ss_read_host_block(win, (ss_header_parse(win, SS_MAX_WORDS, &h), &h), 0) == 0);

	build_window(win, 3);
	{
		const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03 };
		check("host block written", ss_write_host_block(win, SS_MAX_WORDS,
		                                                payload, sizeof(payload)));
		check("window still parses", ss_header_parse(win, SS_MAX_WORDS, &h));

		uint32_t got = 0;
		const void *hb = ss_read_host_block(win, &h, &got);
		check("host block read back", hb != 0);
		// Rounded up to whole words, because every length in the header counts
		// words -- 7 bytes occupies 8 and the padding must be deterministic or
		// the CRC would differ for identical state.
		check("host block padded to words", got == 8);
		check("host block contents survive", hb && !memcmp(hb, payload, sizeof(payload)));

		// A corrupt host block must read as absent, not as data: the drive
		// being told to seek somewhere plausible and wrong is worse than it
		// being told nothing.
		uint32_t off = ss_host_block_offset(&h);
		win[off] ^= 0xFFu;
		check("corrupt host block rejected", ss_read_host_block(win, &h, 0) == 0);
	}

	build_window(win, 3);
	ss_header_set_host_block(win, 2, 0x12345678);
	check("host block sets host_words", win[5] == 2);
	check("host block sets host_crc32", win[7] == 0x12345678);
	check("host block extends length", win[1] == 6 + 3 + 2);

	// ---- the 1.3 -> 1.4 upgrade -------------------------------------------
	//
	// A 1.3 image with a core payload and a host block. The mask goes in at
	// SS_MASK_OFFSET_13 with every bit set, everything after it moves up eight
	// words, the header's length, version, core_words and core_crc32 follow,
	// and the host block is byte-for-byte where the new header says it is.
	{
		const uint32_t core13 = SS_MASK_OFFSET_13 + 40;   // past the insert point
		const uint32_t hostw  = 5;
		build_window(win, core13);
		win[3] = SS_VERSION_13;
		win[5] = hostw;
		win[1] = 6 + core13 + hostw;
		for (uint32_t i = 0; i < hostw; i++) win[8 + core13 + i] = 0x48420000 + i;
		win[7] = crc32_compute((const uint8_t *)&win[8 + core13], hostw * 4);
		win[6] = crc32_compute((const uint8_t *)&win[8], core13 * 4);

		uint32_t before_at = win[8 + SS_MASK_OFFSET_13];           // first word to be displaced
		uint32_t last_core = win[8 + core13 - 1];

		check("1.3 is refused as-is", !ss_header_parse(win, SS_MAX_WORDS, &h));
		check("upgrade accepts a valid 1.3", ss_upgrade_13_to_14(win, SS_MAX_WORDS) == 1);
		check("upgraded header parses", ss_header_parse(win, SS_MAX_WORDS, &h));
		check("version is 1.4", h.version == SS_VERSION);
		check("core grew by the mask", h.core_words == core13 + SS_MASK_WORDS);
		check("length grew by the mask", h.length == 6 + core13 + SS_MASK_WORDS + hostw);
		check("mask word 0 is all ones", win[8 + SS_MASK_OFFSET_13] == 0xFFFFFFFFu);
		check("mask word 7 is all ones", win[8 + SS_MASK_OFFSET_13 + 7] == 0xFFFFFFFFu);
		check("word before the insert is untouched", win[8 + SS_MASK_OFFSET_13 - 1] == 0xA5A50000 + SS_MASK_OFFSET_13 - 1);
		check("displaced word moved up eight", win[8 + SS_MASK_OFFSET_13 + 8] == before_at);
		check("last core word moved up eight", win[8 + core13 + 8 - 1] == last_core);
		check("host block found at the new offset", ss_host_block_offset(&h) == 8 + core13 + SS_MASK_WORDS);
		check("host block content moved intact", win[ss_host_block_offset(&h)] == 0x48420000u);
		check("host crc still matches",
		      crc32_compute((const uint8_t *)&win[ss_host_block_offset(&h)], hostw * 4) == h.host_crc32);
		check("a second upgrade is a no-op", ss_upgrade_13_to_14(win, SS_MAX_WORDS) == 0);
		check("still parses after the no-op", ss_header_parse(win, SS_MAX_WORDS, &h));

		// Refusals leave the image alone.
		build_window(win, core13); win[3] = SS_VERSION_13; win[9] ^= 1;
		check("corrupt 1.3 is not upgraded", ss_upgrade_13_to_14(win, SS_MAX_WORDS) == 0);
		check("and its version is untouched", win[3] == SS_VERSION_13);
		build_window(win, core13); win[3] = SS_VERSION_13;
		check("no room for the mask is refused", ss_upgrade_13_to_14(win, 8 + core13 + 3) == 0);
		build_window(win, SS_MASK_OFFSET_13 - 1); win[3] = SS_VERSION_13;
		win[6] = crc32_compute((const uint8_t *)&win[8], (SS_MASK_OFFSET_13 - 1) * 4);
		check("a payload shorter than the insert point is refused", ss_upgrade_13_to_14(win, SS_MAX_WORDS) == 0);

		// The insert point is a number this test pins on purpose: it is
		// KICK_META + STATE_WORDS + 128, and STATE_WORDS follows `SS_STATE_W
		// in rtl/ss_state.vh. If that width changes, this fails first.
		check("mask offset is 1 + 66 + 128", SS_MASK_OFFSET_13 == 195u);
	}

	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
