#ifndef AMIGA_LIGHTPEN_H
#define AMIGA_LIGHTPEN_H

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

#endif
