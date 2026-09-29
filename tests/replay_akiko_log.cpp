// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replay a captured Akiko log through the current framing code.
//
// Every hardware finding in this project has, until now, needed the hardware to
// re-examine: the command channel ran on the board, the log came off the board,
// and asking "what would today's code do with that?" meant flashing a binary and
// booting a game. The framing decisions are pure functions over a byte buffer,
// and every line that reports one now carries the bytes it decided on -- so a
// captured hang can be re-run here in milliseconds.
//
// Two uses:
//
//   * As a tool.   replay_akiko_log <log>...  prints, for each frame in the log,
//     what the current code decides and whether that differs from what the build
//     that produced the log did. A log from a hang becomes a regression case.
//
//   * As a test.   Run with no arguments it replays
//     tests/data/akiko-chuckrock-hang.log -- the capture that identified the
//     command-channel desync -- and asserts the current code handles it: the
//     all-zero phantoms rejected, and the BIOS's INFO command recovered from
//     behind the stray byte. If a future change breaks the resync, this fails
//     without anyone switching the machine on.
//
// The parser reads `bytes=` from any line that has one, so it handles the
// 2026-09-26 capture format and the current one alike.
//
// What this covers, and what it does not. Mutation-checked against the fixture:
// disabling the resync fails it, and removing the checksum test fails it. Two
// other mutations do NOT fail it, and that is a property of the capture rather
// than a gap in the assertions -- removing the unknown-opcode guard changes
// nothing because every frame in this log has a known opcode, and making the
// resync scan to the LAST valid offset changes nothing because ea 27 d8 has only
// one. Those two rules are covered by tests/test_akiko_framing.cpp, which builds
// the cases for them. A replay is only ever as thorough as the capture; it is
// here to stop a real, observed hang from regressing, not to replace the unit
// test.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../support/minimig/akiko_framing.h"

#define MAX_BYTES 64

struct Frame {
	int  n;
	uint8_t b[MAX_BYTES];
	char kind[32];      // CMD, CHECKSUM, SHORT, PHANTOM, RESYNC, ...
	int  line;
};

static int failures = 0;

// Pull "bytes=de ad be ef" out of a log line. Returns the count, or -1 if the
// line carries no byte field.
static int parse_bytes(const char *line, uint8_t *out, int cap)
{
	const char *p = strstr(line, "bytes=");
	if (!p) return -1;
	p += 6;
	int n = 0;
	while (n < cap) {
		while (*p == ' ') p++;
		if (!isxdigit((unsigned char)*p)) break;
		char *end = NULL;
		long v = strtol(p, &end, 16);
		if (end == p || v < 0 || v > 0xff) break;
		out[n++] = (uint8_t)v;
		p = end;
	}
	return n;
}

// What the logging build said it did, taken from the line's own wording, so the
// replay can report a DIFFERENCE rather than just an opinion.
static void classify(const char *line, char *out, size_t cap)
{
	const char *kinds[] = {"CHECKSUM FAIL", "SHORT FRAME", "PHANTOM", "RESYNC",
	                       "CMD op=", NULL};
	for (int i = 0; kinds[i]; i++) {
		if (strstr(line, kinds[i])) {
			snprintf(out, cap, "%s", kinds[i]);
			return;
		}
	}
	snprintf(out, cap, "?");
}

static int replay(const char *path, bool assert_recovery)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("FAIL cannot open %s\n", path);
		return 1;
	}

	char line[1024];
	int lineno = 0, frames = 0, recovered = 0, rejected = 0, differs = 0;

	printf("== %s\n", path);
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		if (line[0] == '#' || line[0] == '\n') continue;

		Frame fr;
		fr.n = parse_bytes(line, fr.b, MAX_BYTES);
		if (fr.n <= 0) continue;
		fr.line = lineno;
		classify(line, fr.kind, sizeof(fr.kind));
		frames++;

		bool valid = akiko_frame_valid(fr.b, fr.n);
		int  skip  = akiko_frame_resync(fr.b, fr.n);

		char hex[3 * MAX_BYTES + 1];
		int off = 0;
		for (int i = 0; i < fr.n; i++)
			off += snprintf(hex + off, sizeof(hex) - off, "%02x ", fr.b[i]);
		if (off) hex[off - 1] = '\0';

		const char *now;
		if (valid)         now = "accepted as it stands";
		else if (skip > 0) now = "resynced";
		else               now = "rejected, no frame in it";

		printf("   line %-4d %-14s %-14s -> %s", fr.line, fr.kind, hex, now);
		if (skip > 0) printf(" (skip %d, op 0x%02x)", skip, fr.b[skip] & 0x0f);
		printf("\n");

		// A line the logging build reported as a CHECKSUM FAIL, which today's
		// code recovers, is the interesting case: it is a bug this code fixes.
		if (strstr(fr.kind, "CHECKSUM") && skip > 0) {
			differs++;
			recovered++;
			printf("                 ^ the capturing build answered CH_ERR_CHECKSUM here;"
			       " today's code recovers the frame\n");
		} else if (strstr(fr.kind, "CHECKSUM") && skip < 0 && !valid) {
			rejected++;
		}
	}
	fclose(f);

	printf("   %d frame(s): %d recovered by resync, %d still rejected, "
	       "%d differ from the capturing build\n",
	       frames, recovered, rejected, differs);

	if (assert_recovery) {
		// The capture contains two all-zero phantoms, which must stay rejected,
		// and one ea 27 d8, which must be recovered. Anything else means the
		// framing has regressed against a real hang.
		if (frames < 3) {
			printf("FAIL parsed %d frames, expected at least 3\n", frames);
			failures++;
		}
		if (recovered != 1) {
			printf("FAIL expected exactly 1 frame recovered by resync, got %d\n",
			       recovered);
			failures++;
		}
		if (rejected != 2) {
			printf("FAIL expected 2 phantoms to stay rejected, got %d\n", rejected);
			failures++;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		for (int i = 1; i < argc; i++) replay(argv[i], false);
		return 0;
	}

	// No arguments: the regression case.
	replay("data/akiko-chuckrock-hang.log", true);

	if (failures) {
		printf("replay_akiko_log: %d failure(s)\n", failures);
		return 1;
	}
	printf("replay_akiko_log: OK\n");
	return 0;
}
