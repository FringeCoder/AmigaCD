// The per-paint check that a menu page's cursor agrees with its menumask.
//
// The interesting part of this is not the `selected != 1` comparison, it is the
// three silences: a pass that painted nothing, a page with no selectable rows,
// and a cursor parked off the mask. Each of them fires on ordinary menu use, so
// getting one wrong does not produce a missed bug -- it produces a console line
// on every menu open, which is how a diagnostic gets deleted.
//
// FALSIFIED against these mutations -- each was applied to the DUT and the named
// check failed:
//   - `writes <= 0` -> `writes < 0`          -> "a pass that painted nothing" fails
//   - dropping the cursor-on-mask guard      -> "a cursor parked off the mask" fails
//   - `menusub >= 64` -> `menusub > 64`      -> "a menusub past the mask width" fails
//   - `1 << menusub` (int, not uint64_t)     -> "the mask is tested at 63 bits wide"
//   - `selected != 1` -> `selected > 1`      -> "a menumask bit no row draws" fails
//   - `selected != 1` -> `selected < 1`      -> "two rows sharing a menusub" fails
//   - `selected != 1` -> `0`                 -> both of the above fail

#include <stdio.h>

#include "../osd_rowguard.h"

static int failures = 0;

static void check(const char *name, int got, int want)
{
	if (!!got == !!want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %d, want %d\n", name, got, want); failures++; }
}

int main(void)
{
	// --- the two bugs this exists to catch -------------------------------
	// Both are the real shape of the MENU_MINIMIG_CHIPSET1 defect: sixteen
	// selectable rows, the cursor on row 11, and the page's row literals not
	// matching. Two rows at k == 11 invert together; a mask bit no row draws
	// inverts nothing.
	check("two rows sharing a menusub is reported",
	      osd_rowguard_warn(16, 2, 11, 0xFFFF), 1);
	check("a menumask bit no row draws is reported",
	      osd_rowguard_warn(16, 0, 11, 0xFFFF), 1);
	check("three rows sharing a menusub is reported",
	      osd_rowguard_warn(16, 3, 11, 0xFFFF), 1);

	// And the page that agrees with itself says nothing, on every row of it.
	for (unsigned k = 0; k < 16; k++)
	{
		if (osd_rowguard_warn(16, 1, k, 0xFFFF))
		{
			printf("FAIL a consistent page is silent at menusub %u\n", k);
			failures++;
		}
	}
	printf("PASS a consistent page is silent on every one of its rows\n");

	// --- the three silences ----------------------------------------------
	// A page's ...2 state handles keys and paints nothing. Every such pass would
	// otherwise report 0 selected out of 0 rows.
	check("a pass that painted nothing is silent",
	      osd_rowguard_warn(0, 0, 3, 0xFFFF), 0);
	check("and a negative count is treated the same way",
	      osd_rowguard_warn(-1, 0, 3, 0xFFFF), 0);

	// Info screens and the file browser paint rows and have no menumask; the
	// browser draws its own cursor outside this scheme entirely. These two pass
	// through the cursor-on-mask test rather than a guard of their own -- an
	// explicit `if (!menumask)` was written first and removed, because no check
	// here could tell the two versions apart. The checks stay: they pin the
	// behaviour whatever shape the implementation takes.
	check("a page with no selectable rows is silent",
	      osd_rowguard_warn(16, 0, 0, 0), 0);
	check("even when it inverted several rows",
	      osd_rowguard_warn(16, 4, 0, 0), 0);

	// A page can set menusub before the mask machinery moves it onto a
	// selectable row. Nothing inverted is correct during those passes.
	check("a cursor parked off the mask is silent",
	      osd_rowguard_warn(16, 0, 4, 0x000F), 0);
	check("and so is one past the top of the mask",
	      osd_rowguard_warn(16, 0, 31, 0x000F), 0);

	// A cursor off the mask with something inverted anyway is not this check's
	// business either -- it would mean a row inverting for a reason other than
	// the cursor, which the exemption in menu.cpp covers.
	check("a cursor off the mask with a row inverted is still silent",
	      osd_rowguard_warn(16, 1, 4, 0x000F), 0);

	// --- the mask is 64 bits wide, and the shift has to be too -------------
	// menumask is a uint64_t. `1 << menusub` with an int 1 is undefined from 31
	// up and zero from 32 up, which would silence the guard on any page using the
	// high bits -- the AmigaCD pages use up to bit 15 today, so this is a trap
	// waiting rather than a live bug.
	check("the mask is tested at 63 bits wide",
	      osd_rowguard_warn(16, 2, 63, (uint64_t)1 << 63), 1);
	check("and a set bit at 32 is a live cursor",
	      osd_rowguard_warn(16, 0, 32, (uint64_t)1 << 32), 1);
	check("while a clear bit at 32 is not",
	      osd_rowguard_warn(16, 0, 32, (uint64_t)1 << 33), 0);

	// 64 and up cannot be tested against a 64-bit mask at all. Silent, because
	// the honest reading of a cursor outside every page's row set is not "this
	// page's row literals are wrong".
	check("a menusub past the mask width is silent",
	      osd_rowguard_warn(16, 0, 64, ~(uint64_t)0), 0);
	check("and so is one far past it",
	      osd_rowguard_warn(16, 0, 4000000000u, ~(uint64_t)0), 0);

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
