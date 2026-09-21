#include <stdint.h>
#include <stdio.h>
#include <linux/input.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../input.h"
#include "../minimig/minimig_config.h"
#include "amiga_lightpen.h"

// Where on the Amiga raster a fully-deflected pointer lands.
//
// VHPOSR returns hpos[8:1], so the horizontal axis is 8 bits at 280 ns per
// step; vpos counts scanlines. The values below are the visible window, not
// the whole raster -- a gun cannot see blanking, so mapping onto the full
// counter range would waste most of the travel on lines that are never
// displayed.
//
// The origins are WinUAE's, read out of its source rather than guessed. It
// converts a screen position to a raster one in drawing.cpp:
//
//     cx = ((lightpen_x + visible_left_border) >> (1 + gfx_resolution)) + 29;
//     cy = (lightpen_y >> linedbl) + minfirstline;
//     if (cx <= 0x18 - 1) out = true;
//
// so the horizontal floor is 0x18 and the vertical origin is minfirstline,
// which is VBLANK_ENDLINE_{PAL,NTSC} - 1 == 25 and 20 (include/custom.h).
// The previous values here were 0x20 and 0x2C -- the vertical one about 19
// lines too far down the screen.
//
// The spans are derived, not quoted: 320 lores pixels at two per hpos step is
// 160 steps, and the visible height is 256 lines on PAL and 200 on NTSC. So
// the ends are as sound as the origins and the middle is arithmetic.
//
// WinUAE also carries a user-tunable lightpen_offset[] pair on top of all
// this, because the true geometry depends on the display. That is the right
// long-term shape here too; these constants are the starting point it would
// be an offset from.
//
// One scaling step on top of WinUAE's numbers: its cx is in CCK units, and so
// is the hpos VHPOSR reports -- but the latch userspace writes is read back as
// lpen_hpos[8:1], because hpos[0] is the CCK phase bit. So the value sent here
// is twice the CCK position, and 0x18 CCK is 0x30 written. Sending WinUAE's
// number unscaled put the gun's whole travel in the left third of the screen.
#define LPEN_HPOS_MIN       0x30    // 0x18 CCK
#define LPEN_HPOS_SPAN      320     // 160 CCK

// "No position to report" -- no gun, or the pointer is off screen. The core
// tests for exactly this value; see the LIGHT PEN LATCH block in
// agnus_beamcounter.v.
#define LPEN_VPOS_NONE      0x7FF

#define LPEN_VPOS_MIN_PAL   25
#define LPEN_VPOS_SPAN_PAL  256
#define LPEN_VPOS_MIN_NTSC  20
#define LPEN_VPOS_SPAN_NTSC 200

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
	// Off screen is a real state, not an error, and it has to be said out loud
	// rather than by staying quiet. The core freezes VPOSR on the latched
	// position, so a stale one left behind has the Amiga believing the pen is
	// still seeing a beam it cannot. Send the sentinel instead and the core
	// falls back to its no-pen behaviour.
	//
	// Once per crossing, not once per event: the pointer sits off screen for
	// long stretches and this is SPI traffic on the input path.
	static int offscreen;
	if (x < xmin || x > xmax || y < ymin || y > ymax)
	{
		if (!offscreen)
		{
			offscreen = 1;
			spi_uio_cmd16(UIO_MM2_LPENV, LPEN_VPOS_NONE);
		}
		return;
	}
	offscreen = 0;

	// PAL and NTSC do not share a vertical origin or a visible height, and the
	// core knows which it is: chipset bit 1 is the NTSC flag.
	const int ntsc  = (minimig_config.chipset & CONFIG_NTSC) != 0;
	const int vmin  = ntsc ? LPEN_VPOS_MIN_NTSC  : LPEN_VPOS_MIN_PAL;
	const int vspan = ntsc ? LPEN_VPOS_SPAN_NTSC : LPEN_VPOS_SPAN_PAL;

	int hpos = map_range(x, xmin, xmax, LPEN_HPOS_MIN, LPEN_HPOS_MIN + LPEN_HPOS_SPAN);
	int vpos = map_range(y, ymin, ymax, vmin, vmin + vspan);

	// Vertical first, then horizontal, and the pair must already be a real one
	// when it gets here -- input.cpp sends on EV_SYN, once per completed report,
	// not on each EV_ABS.
	//
	// An earlier version of this comment argued the ordering made tearing
	// harmless, on the grounds that the core arms only when vpos AND hpos both
	// match the raster, so a mismatched pair would simply fail to match. That
	// reasoning is wrong. A torn pair is not an impossible coordinate, it is
	// somebody else's coordinate: new x with the previous y is a point the beam
	// genuinely sweeps through, so the latch arms there quite happily and then
	// holds that wrong position for the rest of the frame. Requiring both to
	// match rejects nothing.
	//
	// So the atomicity has to come from the caller, and it does. The ordering
	// below is now only about which half is newer if a write is ever lost.
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
// SNAC gun uses the same route: both guns press the same button on the same
// port and behave identically to the guest. The SNAC side was wired up on
// 2026-08-31 -- support/snac/snac_psx.cpp calls in with BTN_TRIGGER and
// BTN_THUMB, which is why this is a shared route and not a USB one.
static uint32_t lp_buttons[NUMPLAYERS];

// Which port the pen is on.
//
// Not the one the user assigned the gun to -- the light pen is wired to a
// fixed port and a program looking for it reads that port's fire button and no
// other. WinUAE says which (inputdevice.cpp):
//
//     static int lightpen_port_number(void)
//     {
//         return (cs_agnusmodel == AGNUSMODEL_A1000 ||
//                 cs_agnusmodel == AGNUSMODEL_VELVET) ? 0 : 1;
//     }
//
// so port 1 on everything this core can be. Following input[].num instead put
// the trigger wherever MiSTer happened to assign the gun, which for a gun on
// player 1 is the wrong port entirely and reaches nothing.
#define LPEN_PLAYER 1

int amiga_lightpen_player(void)
{
	return LPEN_PLAYER;
}

void amiga_lightpen_button(int code, int pressed)
{
	const int player = LPEN_PLAYER;

	// Which Amiga button this is. A gun has one trigger that matters and
	// usually one or two auxiliary buttons; the trigger becomes fire, the rest
	// become the second button, which is what an Amiga two-button joystick
	// offers and as much as any light gun title asks for.
	//
	// Three code families are accepted because light guns disagree about which
	// they use: mouse-style BTN_LEFT/RIGHT, joystick-style BTN_TRIGGER/THUMB,
	// and gamepad-style BTN_SOUTH/EAST.
	//
	// BTN_NORTH is on the fire side because that is measured, not guessed. A
	// throwaway logging build read the codes off the device itself: this
	// kernel's guncon2 driver reports the GunCon 2's trigger as 0x133
	// (BTN_NORTH), forty presses of it, while 0x131 came from a side button and
	// 0x130 -- the gamepad code two rounds of guessing had assumed -- never
	// appeared at all. Reading it off the hardware settled in one pass what
	// guessing had not.
	//
	// So a gun that uses BTN_SOUTH for its trigger will also fire on its
	// BTN_NORTH side button. That is the right way round to be wrong: one
	// spurious fire button costs far less than a trigger that reaches nothing,
	// which is what this cost already.
	uint32_t bit;
	switch (code)
	{
		case BTN_LEFT:
		case BTN_TRIGGER:
		case BTN_SOUTH:
		case BTN_NORTH:
			bit = JOY_BTN1;
			break;

		case BTN_RIGHT:
		case BTN_MIDDLE:
		case BTN_THUMB:
		case BTN_THUMB2:
		case BTN_EAST:
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
