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
	uint32_t win[SS_MAX_WORDS];
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

	// A truncated file arrives as a zero tail because the host memsets the
	// window before reading (user_io.cpp:1998). That must not parse.
	memset(win, 0, sizeof(win));
	check("all-zero window rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	ss_header_set_host_block(win, 2, 0x12345678);
	check("host block sets host_words", win[5] == 2);
	check("host block sets host_crc32", win[7] == 0x12345678);
	check("host block extends length", win[1] == 6 + 3 + 2);

	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
