// SPDX-License-Identifier: GPL-3.0-or-later
//
// CD disc-format arithmetic and ISO9660 descriptor parsing: the rest of the pure
// part of the Akiko driver.
//
// Split out of akiko_cd32.cpp for the same reason akiko_framing.cpp and
// akiko_pace.cpp were: that file includes ide_cdrom.h, which reaches
// linux/cdrom.h, so it cannot be compiled on the development machine and nothing
// in it can be unit tested. These five functions are arithmetic and buffer
// parsing with no I/O at all.
//
// Worth testing rather than assumed correct: disc geometry has already produced a
// bug that bit three times in this project. The TOC size cap was sized to the
// largest disc seen, held for two, and broke on a third; it is now derived from
// the real ceiling. MSF-to-LBA in particular carries the 150-frame pre-gap, which
// is the classic place for an off-by-one to hide and silently shift every seek.

#ifndef AKIKO_DISCFMT_H
#define AKIKO_DISCFMT_H

#include <stddef.h>
#include <stdint.h>

// Minutes/seconds/frames to a logical block address, with the standard two
// second (150 frame) pre-gap subtracted.
//
// Note the subtraction is unsigned and is NOT clamped: anything below 00:02:00
// wraps. That is the shipped behaviour and the tests pin it down rather than
// quietly changing it -- a TOC should never contain such a value, and if one ever
// does the wrap is worth seeing rather than silently absorbing.
uint32_t akiko_msf_to_lba(uint8_t m, uint8_t s, uint8_t f);

// Packed BCD to binary and back. bcd_to_bin does not validate: a nibble above 9
// produces a value above 99, matching what the shipped code does with whatever
// the drive hands over.
uint8_t akiko_bcd_to_bin(uint8_t b);
uint8_t akiko_bin_to_bcd(uint8_t v);

// Where the ISO9660 primary volume descriptor's user area starts within a raw
// sector: 16 for a 2048-byte mode 1 sector, 24 when the 8-byte mode 2 subheader
// is present. Returns -1 when neither offset holds a type 1 "CD001" version 1
// descriptor.
int akiko_pvd_user_offset(const uint8_t *raw);

// Copy the 32-byte volume identifier at offset 40 of a descriptor into out,
// trailing spaces and NULs trimmed. Non-printable bytes become spaces rather than
// being dropped, so a mangled label still lines up column-for-column with what
// other tools show for the same disc.
void akiko_pvd_label(const uint8_t *pvd, char *out, size_t cap);

#endif // AKIKO_DISCFMT_H
