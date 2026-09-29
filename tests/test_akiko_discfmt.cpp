// SPDX-License-Identifier: GPL-3.0-or-later
//
// MSF/LBA arithmetic, BCD, and ISO9660 descriptor parsing.
//
// These were untestable until akiko_discfmt.cpp split them out of
// akiko_cd32.cpp, which cannot be compiled on the development machine. Disc
// geometry is not a hypothetical risk here: the Akiko TOC size cap was sized to
// the largest disc seen, held for two, and broke on a third. MSF-to-LBA carries
// the 150-frame pre-gap, which is the classic place for an off-by-one to hide and
// shift every seek on the disc by a constant.
//
// Where shipped behaviour is surprising -- the unsigned wrap below 00:02:00, BCD
// conversion not validating its nibbles -- these tests pin it down as it is
// rather than asserting what it arguably ought to be. Changing behaviour and
// changing its tests in the same commit proves nothing.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../support/minimig/akiko_discfmt.h"

static int failures = 0;

static void eq_u32(const char *name, uint32_t got, uint32_t want)
{
	if (got != want) {
		printf("FAIL %s: got %u want %u\n", name, got, want);
		failures++;
	}
}

static void eq_int(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got %d want %d\n", name, got, want);
		failures++;
	}
}

static void eq_str(const char *name, const char *got, const char *want)
{
	if (strcmp(got, want) != 0) {
		printf("FAIL %s: got \"%s\" want \"%s\"\n", name, got, want);
		failures++;
	}
}

// A descriptor with the given identifier at offset 40, in a raw sector whose user
// area starts at `user_off`.
static void make_pvd(uint8_t *raw, int user_off, const char *ident, int identlen)
{
	memset(raw, 0, 2352);
	uint8_t *u = raw + user_off;
	u[0] = 1;                      // type 1, primary
	memcpy(u + 1, "CD001", 5);
	u[6] = 1;                      // version
	memset(u + 40, ' ', 32);
	if (identlen > 32) identlen = 32;
	memcpy(u + 40, ident, identlen);
}

int main(void)
{
	// ------------------------------------------------------------ MSF -> LBA
	{
		// The pre-gap boundary. 00:02:00 is the first addressable block, LBA 0 --
		// an off-by-one here shifts every seek on the disc by one frame.
		eq_u32("msf 00:02:00 is LBA 0", akiko_msf_to_lba(0, 2, 0), 0);
		eq_u32("msf 00:02:01 is LBA 1", akiko_msf_to_lba(0, 2, 1), 1);

		// 75 frames to the second, 60 seconds to the minute.
		eq_u32("msf 00:03:00", akiko_msf_to_lba(0, 3, 0), 75);
		eq_u32("msf 01:00:00", akiko_msf_to_lba(1, 0, 0), 60u * 75u - 150u);
		eq_u32("msf 01:02:00", akiko_msf_to_lba(1, 2, 0), 60u * 75u);

		// A full 74-minute disc, and the 79:59:74 a CD-R can reach.
		eq_u32("msf 74:00:00", akiko_msf_to_lba(74, 0, 0), 74u * 60u * 75u - 150u);
		eq_u32("msf 79:59:74", akiko_msf_to_lba(79, 59, 74),
		       ((79u * 60u + 59u) * 75u + 74u) - 150u);

		// Below the pre-gap the subtraction is unsigned and wraps. Pinned down as
		// shipped, not asserted as desirable: a TOC should never carry such a
		// value, and if one ever does, a huge LBA is more visible than a clamp.
		eq_u32("msf 00:00:00 wraps (shipped behaviour)",
		       akiko_msf_to_lba(0, 0, 0), 0xFFFFFF6Au);
		eq_u32("msf 00:01:74 wraps by one",
		       akiko_msf_to_lba(0, 1, 74), 0xFFFFFFFFu);
	}

	// ----------------------------------------------------------------- BCD
	{
		eq_int("bcd 0x00", akiko_bcd_to_bin(0x00), 0);
		eq_int("bcd 0x09", akiko_bcd_to_bin(0x09), 9);
		eq_int("bcd 0x10", akiko_bcd_to_bin(0x10), 10);
		eq_int("bcd 0x59", akiko_bcd_to_bin(0x59), 59);
		eq_int("bcd 0x99", akiko_bcd_to_bin(0x99), 99);

		eq_int("bin 0 -> bcd", akiko_bin_to_bcd(0), 0x00);
		eq_int("bin 9 -> bcd", akiko_bin_to_bcd(9), 0x09);
		eq_int("bin 10 -> bcd", akiko_bin_to_bcd(10), 0x10);
		eq_int("bin 74 -> bcd", akiko_bin_to_bcd(74), 0x74);
		eq_int("bin 99 -> bcd", akiko_bin_to_bcd(99), 0x99);

		// Round trip across every value a minute, second or frame field can hold.
		for (int v = 0; v <= 99; v++) {
			uint8_t back = akiko_bcd_to_bin(akiko_bin_to_bcd((uint8_t)v));
			if (back != v) {
				printf("FAIL bcd round trip at %d: got %u\n", v, back);
				failures++;
				break;
			}
		}

		// Not validated, by design: a nibble above 9 runs past 99 rather than
		// being rejected. Shipped behaviour, pinned so a future "fix" is a
		// deliberate change and not a silent one.
		eq_int("bcd 0x0a is not rejected", akiko_bcd_to_bin(0x0a), 10);
		eq_int("bcd 0xff runs past 99", akiko_bcd_to_bin(0xff), 165);
	}

	// ------------------------------------------------- PVD user-area offset
	{
		uint8_t raw[2352];

		make_pvd(raw, 16, "TEST", 4);
		eq_int("mode 1 descriptor at 16", akiko_pvd_user_offset(raw), 16);

		make_pvd(raw, 24, "TEST", 4);
		eq_int("mode 2 descriptor at 24", akiko_pvd_user_offset(raw), 24);

		// Neither offset holds one.
		memset(raw, 0, sizeof(raw));
		eq_int("no descriptor", akiko_pvd_user_offset(raw), -1);

		// Right identifier, wrong type.
		make_pvd(raw, 16, "TEST", 4);
		raw[16] = 2;
		eq_int("type 2 rejected", akiko_pvd_user_offset(raw), -1);

		// Right type, wrong identifier.
		make_pvd(raw, 16, "TEST", 4);
		memcpy(raw + 17, "CD002", 5);
		eq_int("wrong identifier rejected", akiko_pvd_user_offset(raw), -1);

		// Right type and identifier, wrong version.
		make_pvd(raw, 16, "TEST", 4);
		raw[22] = 2;
		eq_int("version 2 rejected", akiko_pvd_user_offset(raw), -1);

		// 16 is checked before 24, so a disc with both takes 16.
		make_pvd(raw, 16, "AT16", 4);
		uint8_t *u24 = raw + 24;
		u24[0] = 1; memcpy(u24 + 1, "CD001", 5); u24[6] = 1;
		eq_int("16 wins over 24", akiko_pvd_user_offset(raw), 16);
	}

	// ------------------------------------------------------------- PVD label
	{
		uint8_t raw[2352];
		char out[64];

		make_pvd(raw, 16, "CHUCK ROCK", 10);
		akiko_pvd_label(raw + 16, out, sizeof(out));
		eq_str("label read", out, "CHUCK ROCK");

		// Trailing spaces are trimmed; interior ones are not.
		make_pvd(raw, 16, "A B            ", 15);
		akiko_pvd_label(raw + 16, out, sizeof(out));
		eq_str("trailing spaces trimmed, interior kept", out, "A B");

		// A full 32-character identifier with nothing to trim.
		make_pvd(raw, 16, "0123456789ABCDEF0123456789ABCDEF", 32);
		akiko_pvd_label(raw + 16, out, sizeof(out));
		eq_str("full 32 chars", out, "0123456789ABCDEF0123456789ABCDEF");

		// All spaces trims to nothing.
		make_pvd(raw, 16, "", 0);
		akiko_pvd_label(raw + 16, out, sizeof(out));
		eq_str("blank label", out, "");

		// Non-printables become spaces rather than being dropped, so the label
		// still lines up with what other tools show for the same disc.
		make_pvd(raw, 16, "AB", 2);
		raw[16 + 40 + 1] = 0x01;
		akiko_pvd_label(raw + 16, out, sizeof(out));
		eq_str("control byte becomes a space", out, "A ");

		// A NUL inside the field is trimmed as padding when trailing.
		make_pvd(raw, 16, "OK", 2);
		raw[16 + 40 + 2] = 0x00;
		akiko_pvd_label(raw + 16, out, sizeof(out));
		eq_str("NUL padding trimmed", out, "OK");

		// A short output buffer truncates rather than overruns.
		make_pvd(raw, 16, "0123456789", 10);
		char small[5];
		akiko_pvd_label(raw + 16, small, sizeof(small));
		eq_str("truncates to the buffer", small, "0123");
	}

	if (failures) {
		printf("test_akiko_discfmt: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_akiko_discfmt: OK\n");
	return 0;
}
