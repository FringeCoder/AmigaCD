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

// One ISO9660 directory record at `rec`. Returns its length, so a caller can
// lay several out back to back the way a real extent does.
// The id bytes take an explicit length because the two special entries are a
// single 0x00 or 0x01 byte, and strlen() of those is 0 -- which built a
// nameless record and failed the first run of these tests.
static uint8_t make_dirrec_id(uint8_t *rec, const void *id, size_t nlen,
                              int is_dir, uint32_t lba)
{
	uint8_t rlen = (uint8_t)(33 + nlen);
	if (rlen & 1) rlen++;                 // records are padded to even lengths

	memset(rec, 0, rlen);
	rec[0] = rlen;
	rec[2] = (uint8_t)(lba & 0xff);
	rec[3] = (uint8_t)((lba >> 8) & 0xff);
	rec[25] = is_dir ? 0x02 : 0x00;
	rec[32] = (uint8_t)nlen;
	memcpy(rec + 33, id, nlen);
	return rlen;
}

static uint8_t make_dirrec(uint8_t *rec, const char *name, int is_dir, uint32_t lba)
{
	return make_dirrec_id(rec, name, strlen(name), is_dir, lba);
}

// A primary volume descriptor whose root record points at `lba` for `len` bytes.
static void make_pvd_root(uint8_t *pvd, uint32_t lba, uint32_t len)
{
	memset(pvd, 0, 2048);
	pvd[0] = 1;
	memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	uint8_t *rec = pvd + 156;
	rec[0] = 34;
	rec[2] = (uint8_t)(lba & 0xff);
	rec[3] = (uint8_t)((lba >> 8) & 0xff);
	rec[10] = (uint8_t)(len & 0xff);
	rec[11] = (uint8_t)((len >> 8) & 0xff);
	rec[12] = (uint8_t)((len >> 16) & 0xff);
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

	// ------------------------------------------------- ISO9660 root extent
	{
		uint8_t pvd[2048];
		uint32_t lba = 0, len = 0;

		make_pvd_root(pvd, 23, 2048);
		eq_int("root extent found", akiko_pvd_root_extent(pvd, &lba, &len), 1);
		eq_u32("root lba", lba, 23);
		eq_u32("root len", len, 2048);

		// A zero extent, a zero length, or a length that is not whole sectors is
		// not a root directory. Reported absent rather than walked, because
		// walking it reads whatever is at sector 0.
		make_pvd_root(pvd, 0, 2048);
		eq_int("zero extent rejected", akiko_pvd_root_extent(pvd, &lba, &len), 0);
		make_pvd_root(pvd, 23, 0);
		eq_int("zero length rejected", akiko_pvd_root_extent(pvd, &lba, &len), 0);
		make_pvd_root(pvd, 23, 1000);
		eq_int("partial sector rejected", akiko_pvd_root_extent(pvd, &lba, &len), 0);

		// A root record shorter than its own fixed part.
		make_pvd_root(pvd, 23, 2048);
		pvd[156] = 20;
		eq_int("short record rejected", akiko_pvd_root_extent(pvd, &lba, &len), 0);
	}

	// --------------------------------------------------- ISO9660 dir walk
	{
		uint8_t dir[4096];
		memset(dir, 0, sizeof(dir));
		size_t w = 0;
		static const uint8_t self_id = 0x00, parent_id = 0x01;
		w += make_dirrec_id(dir + w, &self_id,   1, 1, 23);   // "." (self)
		w += make_dirrec_id(dir + w, &parent_id, 1, 1, 23);   // ".." (parent)
		w += make_dirrec(dir + w, "CD32.TM;1", 0, 30);
		w += make_dirrec(dir + w, "DATA", 1, 31);

		char name[64];
		size_t off = 0;
		int is_dir = -1;

		eq_int("entry 1 present", akiko_iso_dir_next(dir, sizeof(dir), &off, name, sizeof(name), &is_dir), 1);
		eq_str("entry 1 is self", name, ".");
		eq_int("entry 1 is a dir", is_dir, 1);

		eq_int("entry 2 present", akiko_iso_dir_next(dir, sizeof(dir), &off, name, sizeof(name), &is_dir), 1);
		eq_str("entry 2 is parent", name, "..");

		eq_int("entry 3 present", akiko_iso_dir_next(dir, sizeof(dir), &off, name, sizeof(name), &is_dir), 1);
		// The version suffix is stripped: a caller looking for CD32.TM must not
		// have to know whether the master wrote ";1".
		eq_str("version suffix stripped", name, "CD32.TM");
		eq_int("entry 3 is a file", is_dir, 0);

		eq_int("entry 4 present", akiko_iso_dir_next(dir, sizeof(dir), &off, name, sizeof(name), &is_dir), 1);
		eq_str("entry 4 name", name, "DATA");
		eq_int("entry 4 is a dir", is_dir, 1);

		// A zero record length ends the records in THIS 2048-byte block only.
		// Treating it as the end of the directory is how every entry past the
		// first block of a large directory gets missed, so the walk has to cross
		// the boundary and find what is there.
		make_dirrec(dir + 2048, "SECOND.BLK;1", 0, 40);
		eq_int("crosses the block boundary", akiko_iso_dir_next(dir, sizeof(dir), &off, name, sizeof(name), &is_dir), 1);
		eq_str("entry in the second block", name, "SECOND.BLK");

		eq_int("walk ends", akiko_iso_dir_next(dir, sizeof(dir), &off, name, sizeof(name), &is_dir), 0);

		// A record claiming to run past the buffer stops the walk instead of
		// reading off the end.
		uint8_t trunc[64];
		memset(trunc, 0, sizeof(trunc));
		make_dirrec(trunc, "TOOLONG.BIN;1", 0, 50);
		trunc[0] = 200;
		off = 0;
		eq_int("overlong record rejected", akiko_iso_dir_next(trunc, sizeof(trunc), &off, name, sizeof(name), &is_dir), 0);

		// A name longer than the output buffer truncates rather than overruns.
		uint8_t one[128];
		memset(one, 0, sizeof(one));
		make_dirrec(one, "ABCDEFGHIJ;1", 0, 60);
		char small[5];
		off = 0;
		eq_int("long name entry present", akiko_iso_dir_next(one, sizeof(one), &off, small, sizeof(small), &is_dir), 1);
		eq_str("long name truncated", small, "ABCD");
	}

	if (failures) {
		printf("test_akiko_discfmt: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_akiko_discfmt: OK\n");
	return 0;
}
