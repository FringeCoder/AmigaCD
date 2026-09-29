// SPDX-License-Identifier: GPL-3.0-or-later
//
// See akiko_discfmt.h. Nothing here may gain a dependency beyond the C library:
// the point of the split is that this compiles on the development machine, where
// linux/cdrom.h does not exist.
//
// Bodies are moved from akiko_cd32.cpp unchanged, so the tests describe shipped
// behaviour rather than a tidied-up version of it.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "akiko_discfmt.h"

uint32_t akiko_msf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
	// Standard CDDA MSF -> LBA conversion with 2-second pre-gap.
	return (((uint32_t)m * 60u + s) * 75u + f) - 150u;
}

uint8_t akiko_bcd_to_bin(uint8_t b)
{
	return (uint8_t)(((b >> 4) * 10u) + (b & 0x0fu));
}

uint8_t akiko_bin_to_bcd(uint8_t v)
{
	return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

int akiko_pvd_user_offset(const uint8_t *raw)
{
	static const int cand[2] = { 16, 24 };
	for (int i = 0; i < 2; i++) {
		const uint8_t *u = raw + cand[i];
		// Type 1 (primary), the standard identifier, version 1.
		if (u[0] == 1 && !memcmp(u + 1, "CD001", 5) && u[6] == 1) return cand[i];
	}
	return -1;
}

void akiko_pvd_label(const uint8_t *pvd, char *out, size_t cap)
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

	snprintf(out, cap, "%s", lbl);
}
