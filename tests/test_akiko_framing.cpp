// SPDX-License-Identifier: GPL-3.0-or-later
//
// akiko_frame_valid() and akiko_frame_resync(), the rule that decides whether a
// drained command buffer holds a real frame and, if not, where the real one
// starts.
//
// Why this file exists at all: the resync was validated on hardware, over 27
// interleaved boot rounds across three runs, because akiko_cd32.cpp cannot be
// compiled on the development machine -- it includes ide_cdrom.h, which reaches
// linux/cdrom.h. The rule itself is a property of a byte buffer. Splitting it
// into akiko_framing.cpp made it testable in milliseconds. The hardware A/B is
// still the only thing that can say whether the fix changes boot outcomes; it
// should not also have been the thing telling us whether the logic was right.
//
// The central case is the exact byte sequence the hardware logged:
//
//     CHECKSUM FAIL n=3 bytes=ea 27 d8
//
// ea is the orphaned checksum byte of a LED frame (15 00 ea); 27 d8 is the
// BIOS's INFO command sitting behind it, whole and correctly summed.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../support/minimig/akiko_framing.h"

static int failures = 0;

static void check(const char *name, bool got, bool want)
{
	if (got != want) {
		printf("FAIL %s: got %s want %s\n", name, got ? "true" : "false",
		       want ? "true" : "false");
		failures++;
	}
}

static void check_int(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got %d want %d\n", name, got, want);
		failures++;
	}
}

// A frame is opcode + payload + a checksum byte chosen so the whole thing sums
// to 0xff. Built here rather than hard-coded so the tests state intent.
static int make_frame(uint8_t *out, uint8_t op, const uint8_t *payload, int payload_len)
{
	out[0] = op;
	for (int i = 0; i < payload_len; i++) out[1 + i] = payload[i];
	uint32_t sum = op;
	for (int i = 0; i < payload_len; i++) sum += payload[i];
	out[1 + payload_len] = (uint8_t)(0xff - (sum & 0xff));
	return payload_len + 2;
}

int main(void)
{
	// ---------------------------------------------------------------- valid
	{
		// LED, opcode 5, one payload byte. command_lengths[5] is 2, meaning
		// two bytes follow the opcode: payload and checksum.
		uint8_t f[8];
		int n = make_frame(f, 0x15, (const uint8_t[]){0x00}, 1);
		check_int("LED frame length", n, 3);
		check("LED frame is valid", akiko_frame_valid(f, n), true);
		check_int("valid frame needs no resync", akiko_frame_resync(f, n), 0);
	}

	// ------------------------------------------------- the hardware sequence
	{
		// ea 27 d8 -- a stray checksum byte, then the BIOS's INFO command.
		const uint8_t buf[3] = {0xea, 0x27, 0xd8};

		check("ea 27 d8 is not valid as it stands",
		      akiko_frame_valid(buf, 3), false);

		// INFO is opcode 7, command_lengths[7] == 1, so one byte follows: the
		// checksum. 0x27 + 0xd8 == 0xff.
		check("27 d8 alone is valid", akiko_frame_valid(buf + 1, 2), true);

		// The whole point: the resync must find it at offset 1.
		check_int("resync finds INFO behind the stray byte",
		          akiko_frame_resync(buf, 3), 1);
	}

	// --------------------------------------------------------- the phantom
	{
		// Two zero bytes. Opcode 0 has command_lengths[0] == 1, so this LOOKS
		// like a complete two-byte frame -- but 0x00 + 0x00 is not 0xff, so the
		// checksum must reject it. This is the frame the FPGA reports when the
		// TX engine fetches chip RAM the BIOS has not written yet, and treating
		// it as real is what desynchronised the command channel.
		const uint8_t zeros[2] = {0x00, 0x00};
		check("all-zero frame is rejected", akiko_frame_valid(zeros, 2), false);
		check_int("all-zero frame offers no resync target",
		          akiko_frame_resync(zeros, 2), -1);
	}

	// ------------------------------------------------- unknown opcodes
	{
		// Opcodes 11..15 are -1 in the table. They must never be claimed as
		// valid, and never chosen as a resync target: draining one is already a
		// "take whatever is there" path, and claiming it would let the resync
		// land on noise.
		//
		// The second byte is chosen so that op + byte == 0xff. That matters: with
		// a zero payload these frames are rejected by the CHECKSUM, so the test
		// could not tell "rejected because the opcode is unknown" from "rejected
		// because the sum is wrong" -- and a mutation removing the unknown-opcode
		// guard entirely still passed. Made to sum correctly, the guard is the
		// only thing that can reject them.
		for (uint8_t op = 0x0b; op <= 0x0f; op++) {
			uint8_t f[4] = {op, (uint8_t)(0xff - op), 0x00, 0x00};
			char name[80];
			snprintf(name, sizeof(name),
			         "unknown opcode 0x%02x rejected despite a correct sum", op);
			check(name, akiko_frame_valid(f, 2), false);
			// Nor at the longer lengths the drain might hand over.
			snprintf(name, sizeof(name),
			         "unknown opcode 0x%02x rejected at n=4", op);
			check(name, akiko_frame_valid(f, 4), false);
		}

		// A buffer whose only correctly summed "frame" starts with an unknown
		// opcode must resync to nothing rather than to the unknown one. 0x0c +
		// 0xf3 == 0xff, so again only the guard can reject it.
		const uint8_t buf[3] = {0x01, 0x0c, 0xf3};
		check_int("resync does not land on an unknown opcode",
		          akiko_frame_resync(buf, 3), -1);
	}

	// ------------------------------------------------------------- truncation
	{
		// MULTI, opcode 4, is the longest the BIOS sends: 12 bytes follow. A
		// buffer holding only part of one is not valid at any length below the
		// whole, and must not be resynced into by accident.
		uint8_t payload[11];
		for (int i = 0; i < 11; i++) payload[i] = (uint8_t)(0xc1 + i);
		uint8_t f[16];
		int n = make_frame(f, 0x04, payload, 11);
		check_int("MULTI frame length", n, 13);
		check("whole MULTI is valid", akiko_frame_valid(f, n), true);

		for (int short_n = 1; short_n < n; short_n++) {
			char name[64];
			snprintf(name, sizeof(name), "MULTI truncated to %d is invalid", short_n);
			check(name, akiko_frame_valid(f, short_n), false);
		}
	}

	// ------------------------------------------------------------ edge cases
	{
		check("empty buffer is invalid", akiko_frame_valid(NULL, 0), false);
		check_int("empty buffer offers no resync", akiko_frame_resync(NULL, 0), -1);

		// One byte can never be a frame: even the shortest needs a checksum.
		const uint8_t one[1] = {0x07};
		check("single byte is invalid", akiko_frame_valid(one, 1), false);
		check_int("single byte offers no resync", akiko_frame_resync(one, 1), -1);
	}

	// ------------------------------------------- resync takes the FIRST frame
	{
		// Two valid frames back to back behind one stray byte. The resync must
		// stop at the first, not scan past it: the second is the next command
		// and answering it first would reorder the channel.
		uint8_t buf[16];
		buf[0] = 0xea;                                   // stray
		int a = make_frame(buf + 1, 0x27, NULL, 0);      // INFO
		int b = make_frame(buf + 1 + a, 0x15, (const uint8_t[]){0x00}, 1);
		check_int("resync stops at the first frame",
		          akiko_frame_resync(buf, 1 + a + b), 1);
	}

	if (failures) {
		printf("test_akiko_framing: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_akiko_framing: OK\n");
	return 0;
}
