#include <stdint.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../input.h"
#include "amiga_lightpen.h"

// Where on the Amiga raster a fully-deflected pointer lands.
//
// VHPOSR returns hpos[8:1], so the horizontal axis is 8 bits at 280 ns per
// step; vpos counts scanlines, 0..312 on PAL. The values below are the visible
// window, not the whole raster -- a gun cannot see blanking, so mapping onto
// the full counter range would waste most of the travel on lines and columns
// that are never displayed.
//
// THESE ARE STARTING POINTS, NOT MEASUREMENTS. They have never been fitted
// against a real display. If the pointer tracks with a constant offset or the
// wrong gain, these four numbers are what to change -- and being here in
// userspace is the reason that does not need a 22-minute Quartus run.
#define LPEN_HPOS_MIN   0x20
#define LPEN_HPOS_MAX   0xD8
#define LPEN_VPOS_MIN   0x2C
#define LPEN_VPOS_MAX   0x128

static int map_range(int v, int vmin, int vmax, int omin, int omax)
{
	// Integer maths on purpose: this runs per input event, and the whole range
	// is a few hundred units wide, so rounding is worth less than predictability.
	int span = vmax - vmin;
	if (span <= 0) return omin;
	int out = omin + ((v - vmin) * (omax - omin)) / span;
	if (out < omin) out = omin;
	if (out > omax) out = omax;
	return out;
}

void amiga_lightpen_abs(int x, int y, int xmin, int xmax, int ymin, int ymax)
{
	// Off-screen is a real state, not an error. Leaving the latch alone means
	// the Amiga keeps reading the last position the gun actually saw, which is
	// what a physical light pen does when it loses the raster.
	if (x < xmin || x > xmax) return;
	if (y < ymin || y > ymax) return;

	int hpos = map_range(x, xmin, xmax, LPEN_HPOS_MIN, LPEN_HPOS_MAX);
	int vpos = map_range(y, ymin, ymax, LPEN_VPOS_MIN, LPEN_VPOS_MAX);

	// Vertical first, then horizontal -- the horizontal write is the one the
	// Amiga side reads against, so this ordering makes a torn pair observable
	// only as a one-poll-old position, never as a position from two different
	// places on the screen.
	spi_uio_cmd16(UIO_MM2_LPENV, (uint16_t)vpos);
	spi_uio_cmd16(UIO_MM2_LPENH, (uint16_t)hpos);
}

int amiga_lightpen_usb_active(void)
{
	// No config switch for this, deliberately. A fourth on-disk AmigaCD.cfg
	// layout costs another migration branch, and that trap has already been
	// available three times; a connected CRT light gun on an AmigaCD core is
	// unambiguous enough to act on. The SNAC gun keeps its own OSD mode because
	// that one shares the user port and genuinely needs arbitrating.
	return is_amigacd() && input_has_lightgun();
}
