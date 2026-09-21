// The screen-position row's wording, and the key stored positions are filed
// under.
//
// Both were pure functions sitting in minimig_config.cpp, which includes
// hardware.h, so neither could be linked here and neither was checked by
// anything but reading. What a test can pin is exactly the part that matters:
// that the three states say three different things, that "position saved" is
// never claimed for a mode the core has not reported, and that the file-format
// key keeps the bit layout every *_vadjust.dat on a card was written with.
//
// FALSIFIED against these mutations -- each was applied to the DUT and the
// named check failed:
//   - dropping the !known branch                -> "no mode reported ..." fails
//   - `return found` -> `return 1`              -> "... reports not-stored" fails
//   - returning found without the known guard   -> "an unreported mode never
//                                                   claims a stored position"
//   - hsize and vsize swapped in the text       -> "... names the mode" fails
//   - "position saved"/"not adjusted yet" swapped -> both wording checks fail
//   - vsize shifted by 11 or 13 instead of 12   -> "the key packs ..." fails
//   - the mode byte shifted by 23 instead of 24 -> same
//   - masking hsize to 12 bits                  -> "hsize is not masked" fails
//
// ONE MUTATION THIS CANNOT CATCH, recorded so the coverage is not mistaken for
// complete: dropping the `& 0xFF` from the mode byte changes nothing any check
// can see, because a key is a uint32_t and `vmode << 24` discards the bits the
// mask would have removed anyway. The mask is field documentation rather than
// logic -- it would start mattering only if the key ever widened past 32 bits.
// Untestable by construction, not untested by oversight.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "../support/minimig/vadjust_info.h"

static int failures = 0;

static void check(const char *name, int got, int want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %d, want %d\n", name, got, want); failures++; }
}

static void check_str(const char *name, const char *got, const char *want)
{
	if (!strcmp(got, want)) printf("PASS %s\n", name);
	else
	{
		printf("FAIL %s:\n  got:  %s\n  want: %s\n", name, got, want);
		failures++;
	}
}

static void check_u32(const char *name, uint32_t got, uint32_t want)
{
	if (got == want) printf("PASS %s\n", name);
	else
	{
		printf("FAIL %s: got 0x%08" PRIx32 ", want 0x%08" PRIx32 "\n",
		       name, got, want);
		failures++;
	}
}

int main(void)
{
	char buf[64];

	// --- the three states say three different things ---------------------
	{
		check("no mode reported returns not-stored",
		      vadjust_info_text(0, 0, 0, 0, buf, sizeof(buf)), 0);
		check_str("and says so rather than printing 0x0", buf,
		          "(no video mode reported yet)");

		check("a known mode without a preset reports not-stored",
		      vadjust_info_text(1, 0, 320, 256, buf, sizeof(buf)), 0);
		check_str("and names the mode and the state", buf,
		          "320x256: not adjusted yet");

		check("a known mode with a preset reports stored",
		      vadjust_info_text(1, 1, 640, 512, buf, sizeof(buf)), 1);
		check_str("and names the mode and the state", buf,
		          "640x512: position saved");
	}

	// The mode is printed hsize-first. Asymmetric values, because 320x320
	// would pass with the two swapped -- the mutation this exists to catch.
	{
		vadjust_info_text(1, 1, 1448, 625, buf, sizeof(buf));
		check_str("the text names the mode hsize first", buf,
		          "1448x625: position saved");
	}

	// --- an unreported mode never claims a stored position ---------------
	// vadj_found is a static that outlives a mode change: minimig_adjust_vsize()
	// only clears it when the new mode has no preset, and store_vsize() sets it
	// without consulting `known` at all. So `found` can arrive set while the
	// core has reported nothing, and the row must not answer "saved" for a mode
	// it cannot name.
	{
		check("an unreported mode never claims a stored position",
		      vadjust_info_text(0, 1, 320, 256, buf, sizeof(buf)), 0);
		check_str("and still says no mode was reported", buf,
		          "(no video mode reported yet)");
	}

	// The return value is a flag, not a passthrough: callers compare it
	// against 1, so any non-zero `found` has to come back as exactly 1.
	check("a non-boolean found normalises to 1",
	      vadjust_info_text(1, 7, 320, 256, buf, sizeof(buf)), 1);

	// --- the bound in the header is the real one -------------------------
	// VADJUST_INFO_MAXLEN is compared against the OSD column budget by whoever
	// reads the header, so it has to be true. The widest output is the widest
	// values the wire types allow, not the ones an Amiga produces.
	{
		char wide[VADJUST_INFO_MAXLEN];
		vadjust_info_text(1, 0, 65535, 65535, wide, sizeof(wide));
		check_str("the widest text fits VADJUST_INFO_MAXLEN exactly", wide,
		          "65535x65535: not adjusted yet");
		check("which is 29 characters", (int)strlen(wide), VADJUST_INFO_MAXLEN - 1);

		char nomode[VADJUST_INFO_MAXLEN];
		vadjust_info_text(0, 0, 0, 0, nomode, sizeof(nomode));
		check_str("and the no-mode text fits too", nomode,
		          "(no video mode reported yet)");
	}

	// --- short and absent buffers ----------------------------------------
	{
		char tiny[8];
		memset(tiny, 'x', sizeof(tiny));
		check("a short buffer still reports the state",
		      vadjust_info_text(1, 1, 320, 256, tiny, sizeof(tiny)), 1);
		check_str("and truncates rather than overflowing", tiny, "320x256");

		char one[1] = { 'x' };
		vadjust_info_text(1, 1, 320, 256, one, sizeof(one));
		check("a one-byte buffer gets just the terminator", (int)one[0], 0);

		check("a null buffer is refused",
		      vadjust_info_text(1, 1, 320, 256, NULL, 64), 0);
		check("a zero-size buffer is refused",
		      vadjust_info_text(1, 1, 320, 256, buf, 0), 0);
	}

	// --- the mode key is a file format -----------------------------------
	// hsize in 0..11, vsize in 12..23, the mode byte in 24..31. Values chosen
	// so that every field is distinguishable from the others: 0x123 does not
	// fit in 8 bits, 0x045 does not look like 0x123, and 0x67 sits alone in the
	// top byte.
	{
		check_u32("the key packs hsize, vsize and the mode byte",
		          vadjust_mode_key(0x123, 0x045, 0x67), 0x67045123u);

		// A shift of 11 or 13 instead of 12 collides fields; a single bit in
		// vsize pins the boundary on its own.
		check_u32("vsize starts at bit 12", vadjust_mode_key(0, 1, 0), 0x00001000u);
		check_u32("the mode byte starts at bit 24", vadjust_mode_key(0, 0, 1), 0x01000000u);
		check_u32("hsize starts at bit 0", vadjust_mode_key(1, 0, 0), 0x00000001u);

		// The top of the mode byte must not overflow into nothing: 255 << 24 is
		// 0xFF000000, which does not fit in a signed int. The expression this
		// replaced shifted an int and was undefined at that value; this one is
		// unsigned throughout.
		check_u32("the whole mode byte survives",
		          vadjust_mode_key(0, 0, 0xFF), 0xFF000000u);

		// Anything above 8 bits of mode is dropped, as the original did --
		// though see the note at the top: the shift would drop it regardless,
		// so this pins the behaviour and not the mask.
		check_u32("the mode byte is masked to 8 bits",
		          vadjust_mode_key(0, 0, 0x1FF), 0xFF000000u);

		// hsize and vsize are NOT masked. This is not a nicety -- masking them
		// would produce a different key for the same mode and orphan every
		// position already stored on a card. The check exists so that a future
		// tidy-up of the field widths fails here and reads the header comment.
		check_u32("hsize is not masked to its field width",
		          vadjust_mode_key(0x1000, 0, 0), 0x00001000u);

		// Zero is the sentinel both callers test for before looking anything up.
		check_u32("an all-zero mode is zero", vadjust_mode_key(0, 0, 0), 0u);
	}

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
