// Does a page that just painted agree with its own menumask?
//
// A menu page declares which rows the cursor can land on by setting menumask,
// and draws the cursor by passing `menusub == k` as the invert flag at each row,
// with k written out as a literal. Nothing ties the two together, and when they
// disagree the symptom is invisible -- MENU_MINIMIG_CHIPSET1 had two rows at
// k == 11 for as long as FDD Turbo existed, so both drew highlighted and the one
// below could never be toggled.
//
// What is checkable is the consequence: with the cursor on a selectable row,
// exactly one row comes out inverted. The decision lives here, away from
// menu.cpp, for the same reason cd_savename.cpp and vadjust_info.cpp do -- it is
// the part that can be wrong in a way reading does not show, and menu.cpp cannot
// be compiled by tests/. The counting stays in osd.cpp, where the writes happen.
//
// Deliberately free of MiSTer headers: <stdint.h> and nothing else.

#ifndef OSD_ROWGUARD_H
#define OSD_ROWGUARD_H

#include <stdint.h>

// writes   - rows the page painted
// selected - how many of them it painted inverted
// menusub  - where the cursor is
// menumask - bit per row the cursor can land on
//
// Returns non-zero when this paint contradicts the mask and is worth one line on
// the console. Silent when:
//
//   - nothing was painted. A page's ...2 state handles keys and draws nothing,
//     and every pass through HandleUI that is not a paint would otherwise report
//     the same non-verdict.
//   - the cursor is not on a bit the mask has set. A page can set menusub before
//     the mask machinery moves it onto a selectable row, and during those passes
//     nothing being inverted is correct rather than a bug. This also covers a
//     menumask of 0 -- info screens, and the file browser, which draws its own
//     cursor outside this scheme -- since no bit is set for the cursor to be on.
//
// Bits at or above 64 cannot be tested against a 64-bit mask, so a menusub that
// far out is treated as not on the mask rather than as a contradiction. It would
// mean the cursor had left every page's row set, which the mask machinery does
// not do; guessing it is a row-literal bug would be the wrong report.
int osd_rowguard_warn(int writes, int selected, unsigned menusub, uint64_t menumask);

#endif
