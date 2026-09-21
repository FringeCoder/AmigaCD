// What the screen-position row on the Video & Audio page says, and the key the
// stored positions are filed under.
//
// Extracted from minimig_config.cpp so it can be unit-tested. Both functions
// there were pure already -- one formats three variables into a string, the
// other packs three numbers into a uint32_t -- but they sat in a file that
// includes hardware.h, so nothing in tests/ could link them. cd_savename.cpp,
// rom_crypt.cpp and mgl_rbf.cpp were split out for the same reason.
//
// Deliberately free of MiSTer headers: <stdint.h>, <stddef.h> and the C library
// only. The video mode arrives as parameters rather than being read from the
// statics minimig_adjust_vsize() fills, which is the whole point -- a test can
// state a mode instead of needing an FPGA to report one.

#ifndef VADJUST_INFO_H
#define VADJUST_INFO_H

#include <stdint.h>
#include <stddef.h>

// The longest string vadjust_info_text() can produce, NUL included.
//
// "65535x65535: not adjusted yet" is 29 characters, and the no-mode string is
// 28. Nothing bounds hsize and vsize below 65535 -- they come off the wire from
// UIO_GET_VMODE as uint16_t -- so the bound is stated at the values the types
// allow, not at the ones an Amiga produces. The caller in menu.cpp prints
// "  %.29s" into a 32-column OSD line, so the column budget is the tighter of
// the two limits and this constant exists to be compared against it.
#define VADJUST_INFO_MAXLEN 30

// Describe the video mode now showing and whether it has a saved position.
//
//   known - the core has reported a mode at all (UIO_GET_VMODE answered)
//   found - that mode has a stored screen position
//
// Writes one of three strings. Returns non-zero only when a position is stored
// for the mode now showing, which is what the caller reports upward -- and it
// is 0 whenever !known, because an unreported mode cannot have been adjusted no
// matter what `found` is left over saying from a previous mode.
//
// Truncates rather than overflowing if outsz is short, and writes nothing at
// all (returning 0) for a NULL or non-positive buffer.
int vadjust_info_text(int known, int found, unsigned hsize, unsigned vsize,
                      char *out, size_t outsz);

// The key a screen position is filed under in <core>_vadjust.dat.
//
// hsize in bits 0..11, vsize in bits 12..23, the video mode byte in 24..31.
//
// THIS IS A FILE FORMAT. Every card with a *_vadjust.dat on it has positions
// filed under keys this expression produced, so the bit layout is fixed by
// compatibility. In particular hsize and vsize are NOT masked to 12 bits, even
// though a value over 4095 would carry into the next field: masking them would
// be a different key for the same mode, which loses every stored position
// rather than fixing anything. If that ever needs correcting it needs a
// migration, not an edit here.
//
// The mask on the mode byte is field documentation, not logic: a 32-bit key
// discards those bits in the shift whether they are masked first or not, which
// is why no test can distinguish the two. It is kept because the next person to
// read the layout should see where the field ends.
//
// 0 means "no mode" to both callers, which use it to skip the lookup entirely.
uint32_t vadjust_mode_key(unsigned hsize, unsigned vsize, unsigned vmode);

#endif
