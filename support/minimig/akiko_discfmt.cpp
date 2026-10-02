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

#include <string.h>

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

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int akiko_pvd_root_extent(const uint8_t *pvd, uint32_t *out_lba, uint32_t *out_len)
{
	if (!pvd) return 0;

	const uint8_t *rec = pvd + 156;
	if (rec[0] < 34) return 0;              // shorter than the fixed part

	uint32_t lba = le32(rec + 2);
	uint32_t len = le32(rec + 10);
	if (!lba || !len) return 0;
	if (len % 2048u) return 0;              // a directory is whole sectors

	if (out_lba) *out_lba = lba;
	if (out_len) *out_len = len;
	return 1;
}

int akiko_iso_dir_next(const uint8_t *buf, size_t len, size_t *off,
                       char *name, size_t namecap, int *is_dir)
{
	if (!buf || !off || !name || namecap == 0) return 0;

	while (*off + 33 <= len) {
		uint8_t rlen = buf[*off];

		if (rlen == 0) {
			// End of the records in this logical block, not of the directory.
			size_t next = (*off / 2048u + 1) * 2048u;
			if (next <= *off || next >= len) return 0;
			*off = next;
			continue;
		}

		if (rlen < 33 || *off + rlen > len) return 0;   // malformed; stop

		uint8_t nlen = buf[*off + 32];
		if (33u + nlen > rlen) return 0;

		const uint8_t *id = buf + *off + 33;
		if (is_dir) *is_dir = (buf[*off + 25] & 0x02) ? 1 : 0;

		// The two special entries are a single 0x00 or 0x01 byte, not names:
		// "." for self and ".." for parent. Reported as those strings so a
		// caller listing a directory sees what every other tool shows.
		if (nlen == 1 && (id[0] == 0x00 || id[0] == 0x01)) {
			const char *special = (id[0] == 0x00) ? "." : "..";
			size_t n = strlen(special);
			if (n > namecap - 1) n = namecap - 1;
			memcpy(name, special, n);
			name[n] = 0;
			*off += rlen;
			return 1;
		}

		size_t n = nlen;
		// Strip the ";1" version suffix files carry and directories do not.
		for (size_t i = 0; i + 1 < n; i++) {
			if (id[i] == ';') { n = i; break; }
		}
		if (n > namecap - 1) n = namecap - 1;
		memcpy(name, id, n);
		name[n] = 0;

		*off += rlen;
		return 1;
	}

	return 0;
}
