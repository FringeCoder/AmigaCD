#ifndef AMIGA_LIGHTPEN_H
#define AMIGA_LIGHTPEN_H

#include <stdint.h>

// The Amiga light pen, driven from whatever pointing device is to hand.
//
// The core side takes a raster position over two UIO commands and latches it;
// Agnus then serves that position from VPOSR/VHPOSR when a program enables the
// light pen (BPLCON0 bit 3). It does not care where the position came from, so
// this is deliberately source-agnostic:
//
//   - a GunCon on the SNAC user port, read in RTL   (support/snac/snac_psx.cpp)
//   - a GunCon 2 over USB, read by the Linux driver (input.cpp)
//
// Both call amiga_lightpen_abs() with their own coordinate range and this maps
// it onto the Amiga raster.

// Feed an absolute pointer position. x/y are in the device's own units and
// xmin..xmax / ymin..ymax describe that device's calibrated range, so a caller
// never has to know anything about Amiga raster geometry.
//
// Out-of-range values are ignored rather than clamped: for a light gun that
// means the shot was off-screen, and the Amiga must keep reading the last
// on-screen position rather than snap to an edge.
void amiga_lightpen_abs(int x, int y, int xmin, int xmax, int ymin, int ymax);

// Non-zero when a USB light gun should be driving the pen: an AmigaCD core is
// running and a CRT light gun is connected.
int  amiga_lightpen_usb_active(void);

// The trigger.
//
// A light pen is two things to the Amiga and only one of them is the position:
// programs read the raster latch to find out WHERE the pen is, and a joystick
// fire button on the same port to find out whether it has been FIRED. Agnus has
// no notion of a gun button, so there is nothing to send it to -- the trigger
// has to arrive as an ordinary fire button or it does not arrive at all.
//
// It also cannot come through the normal button mapping. A light gun is
// recognised by VID/PID and never gets a .map file, so with no explicit route
// the trigger goes nowhere: aiming works and shooting does nothing, which is
// exactly what a GunCon 2 did on hardware.
//
// `code` is the raw EV_KEY code from the device; the module decides which
// Amiga button it becomes, and which port it lands on, so callers need no
// table and no port argument.
void     amiga_lightpen_button(int code, int pressed);

// The port the light pen occupies, zero-based as build_joy_mask() counts.
int      amiga_lightpen_player(void);

// The buttons currently held, as JOY_* bits, to be OR'd into the port's mask.
// Zero for any player with no gun on it, so this is safe to add unconditionally.
uint32_t amiga_lightpen_joy_mask(int player);

// TEMPORARY diagnostic, to be removed once the light pen is understood on
// hardware. Appends a line to /tmp/lpen.log, capped so it cannot fill /tmp.
//
// This exists because the gun cannot be observed from outside the process at
// all: input.cpp calls ioctl(EVIOCGRAB) on every pooled device whenever the
// OSD is up or input is grabbed, and a grabbed evdev node delivers events to
// the grabbing client and to nobody else. Reading /dev/input/eventN from a
// shell returns zero bytes however the gun is aimed, which looks exactly like
// a gun that is not tracking. Instrumenting from in here is the only way to
// see the coordinates.
void amiga_lightpen_log(const char *fmt, ...);

#endif
