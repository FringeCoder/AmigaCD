#include <stdint.h>
#include <stdio.h>
#include <linux/input.h>
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

// ---------------------------------------------------------------- the trigger
//
// Held state per player, in JOY_* bits, OR'd into that port's mask by
// input.cpp after build_joy_mask(). Kept here rather than in input.cpp so the
// SNAC gun can use the same route when its trigger is wired up: both guns then
// press the same button on the same port and behave identically to the guest.
static uint32_t lp_buttons[NUMPLAYERS];

void amiga_lightpen_button(int player, int code, int pressed)
{
	if (player < 0 || player >= NUMPLAYERS) return;

	// Which Amiga button this is. A gun has one trigger that matters and
	// usually one or two auxiliary buttons; the trigger becomes fire, the rest
	// become the second button, which is what an Amiga two-button joystick
	// offers and as much as any light gun title asks for.
	//
	// Three code families are accepted because light guns disagree about which
	// they use: mouse-style BTN_LEFT/RIGHT, joystick-style BTN_TRIGGER/THUMB,
	// and gamepad-style BTN_SOUTH/EAST. A GunCon 2 reports in the gamepad
	// range -- its KEY bitmap on hardware has bits at 0x130 and up -- but the
	// SNAC gun and other USB guns need not agree, and guessing one family
	// wrong costs another build-and-deploy cycle to find out.
	uint32_t bit;
	switch (code)
	{
		case BTN_LEFT:
		case BTN_TRIGGER:
		case BTN_SOUTH:
			bit = JOY_BTN1;
			break;

		case BTN_RIGHT:
		case BTN_MIDDLE:
		case BTN_THUMB:
		case BTN_THUMB2:
		case BTN_EAST:
		case BTN_NORTH:
		case BTN_WEST:
			bit = JOY_BTN2;
			break;

		default:
			// Say so once per code rather than silently dropping it: if a gun
			// turns out to report its trigger somewhere unexpected, this line
			// in the log is the whole diagnosis.
			{
				static int moaned[8];
				static int moans;
				int seen = 0;
				for (int i = 0; i < moans; i++) if (moaned[i] == code) seen = 1;
				if (!seen && moans < (int)(sizeof(moaned) / sizeof(moaned[0])))
				{
					moaned[moans++] = code;
					printf("lightpen: gun button 0x%03X is not routed to the Amiga\n", code);
				}
			}
			return;
	}

	if (pressed) lp_buttons[player] |= bit;
	else         lp_buttons[player] &= ~bit;
}

uint32_t amiga_lightpen_joy_mask(int player)
{
	if (player < 0 || player >= NUMPLAYERS) return 0;
	return lp_buttons[player];
}
