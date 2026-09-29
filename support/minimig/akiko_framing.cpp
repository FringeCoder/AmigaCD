// SPDX-License-Identifier: GPL-3.0-or-later
//
// See akiko_framing.h. Nothing here may gain a dependency beyond stdint: the
// point of the split is that this compiles on the development machine, where
// linux/cdrom.h does not exist.

#include <stdint.h>

#include "akiko_framing.h"

// Byte-exact mirror of WinUAE akiko.cpp. -1 marks an opcode the drive does not
// define; index 4 (MULTI) is the longest the BIOS sends, at 12 bytes plus its
// checksum.
const int akiko_command_lengths[16] = {
	1, 2, 1, 1, 12, 2, 1, 1, 4, 1, 2, -1, -1, -1, -1, -1
};

bool akiko_frame_valid(const uint8_t *buf, int n)
{
	if (n < 1) return false;
	int len = akiko_command_lengths[buf[0] & 0x0f];
	if (len <= 0) return false;
	int total = len + 1;
	if (n < total) return false;
	uint32_t sum = 0;
	for (int i = 0; i < total; i++) sum += buf[i];
	return (sum & 0xff) == 0xff;
}

int akiko_frame_resync(const uint8_t *buf, int n)
{
	if (akiko_frame_valid(buf, n)) return 0;
	for (int skip = 1; skip < n; skip++) {
		if (akiko_frame_valid(&buf[skip], n - skip)) return skip;
	}
	return -1;
}
