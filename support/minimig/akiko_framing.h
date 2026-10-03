// SPDX-License-Identifier: GPL-3.0-or-later
//
// Akiko command-frame framing: the pure part of the command channel.
//
// Split out of akiko_cd32.cpp so it can be unit tested. That file includes
// ide_cdrom.h and hardware.h, which reach linux/cdrom.h and therefore cannot be
// compiled on the Windows development machine at all -- so every change to this
// logic used to be validated by flashing a core and counting boots. The frame
// resync below was proven that way, over 27 interleaved boot rounds, when the
// rule it implements is a property of a byte buffer and nothing else.
//
// Same reason akiko_pace.cpp is a module of its own.

#ifndef AKIKO_FRAMING_H
#define AKIKO_FRAMING_H

#include <stdint.h>

// Total bytes FOLLOWING the opcode for each of the sixteen opcodes, or -1 where
// the opcode is not one the drive defines. Byte-exact mirror of WinUAE's table.
extern const int akiko_command_lengths[16];

// Is buf[0..n) a complete, correctly summed frame for the opcode it starts with?
//
// A real frame sums to 0xff across opcode, payload and checksum. An unknown
// opcode has no length to check against, so it is never treated as valid --
// draining one is already a "take whatever is there" path, and claiming it here
// would let the resync below land on noise.
bool akiko_frame_valid(const uint8_t *buf, int n);

// How many leading bytes to drop so that the buffer starts on a complete frame.
//
// Returns 0 when it already does, a positive count when dropping that many
// bytes exposes one, and -1 when no offset in the buffer does.
//
// This exists because the TX engine fetches the command stream from chip RAM
// ahead of what the BIOS has written: a drain can consume bytes that are not
// there yet, and what lands afterwards is the tail of one frame in front of the
// next. On hardware that reads
//
//     CHECKSUM FAIL n=3 bytes=ea 27 d8
//
// where ea is the orphaned checksum byte of a LED frame (15 00 ea) and 27 d8 is
// the BIOS's INFO command sitting behind it, whole and correctly summed.
// Answering CH_ERR_CHECKSUM there loses the INFO and the BIOS stops talking,
// which is the Chuck Rock boot hang. A byte skipped this way was never a command
// of its own: it is the remains of one already answered.
int akiko_frame_resync(const uint8_t *buf, int n);

// The checksum byte a frame is missing, or 0 when it is not missing one.
//
// A frame whose only bad byte is a trailing ZERO where the checksum belongs has
// not been corrupted -- it has not finished being written. The 68020 writes the
// checksum last, the TX engine fetches from chip RAM, and a drain can win that
// race, so the whole command lands with a hole at the end. Answering
// CH_ERR_CHECKSUM to that is what ends every stalled Chuck Rock boot measured on
// 2026-10-03.
//
// Returns the value that byte must hold for the frame to sum correctly, or 0
// when the frame is already valid, when the trailing byte is not zero, or when
// the needed value is itself zero (in which case the frame is valid as it
// stands and never gets here). `len` counts the command bytes AND the checksum.
uint8_t akiko_frame_unwritten_checksum(const uint8_t *buf, int len);

#endif // AKIKO_FRAMING_H
