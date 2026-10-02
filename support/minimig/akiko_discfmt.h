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

// The root directory record inside a primary volume descriptor: its extent
// LBA and its length in bytes. The record sits at offset 156 and ISO9660 stores
// both fields little-endian (there is a big-endian copy too; this reads the LE
// one, which is what every other tool does).
//
// Returns 0 and leaves the outputs alone when the record is unusable: a zero
// extent, a zero length, or a length that is not a whole number of sectors is
// not a root directory and is better reported as absent than walked.
int akiko_pvd_root_extent(const uint8_t *pvd, uint32_t *out_lba, uint32_t *out_len);

// Step over the directory records in `buf`, which holds `len` bytes read from a
// directory extent. `*off` is the cursor and must start at 0.
//
// Writes the entry name into `name` with the ISO9660 ";1" version suffix
// stripped, and sets `*is_dir` from the file-flags directory bit. Returns 1 for
// an entry, 0 at the end.
//
// A zero record length means "no more records in this 2048-byte block" rather
// than end of directory, so the walk advances to the next block boundary and
// keeps going. Treating it as the end is the classic way to miss every entry
// past the first block of a large directory.
int akiko_iso_dir_next(const uint8_t *buf, size_t len, size_t *off,
                       char *name, size_t namecap, int *is_dir);

#endif // AKIKO_DISCFMT_H
