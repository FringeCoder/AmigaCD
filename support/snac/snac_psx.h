#ifndef SNAC_PSX_H
#define SNAC_PSX_H

// PSX controllers on the SNAC user port.
//
// The core reads the pads and publishes their state; this reads that state and
// turns it into OSD navigation. Buttons reach the emulated machine in RTL, not
// through here -- see docs/superpowers/specs/2026-08-04-psx-snac-design.md.

// Button bits, matching rtl/snac_psx.v's vector exactly.
#define SNAC_RIGHT   0x0001
#define SNAC_LEFT    0x0002
#define SNAC_DOWN    0x0004
#define SNAC_UP      0x0008
#define SNAC_CROSS   0x0010
#define SNAC_CIRCLE  0x0020
#define SNAC_SQUARE  0x0040
#define SNAC_TRI     0x0080
#define SNAC_L1      0x0100
#define SNAC_R1      0x0200
#define SNAC_START   0x0400
#define SNAC_SELECT  0x0800
#define SNAC_L2      0x1000
#define SNAC_R2      0x2000
#define SNAC_L3      0x4000
#define SNAC_R3      0x8000

// Non-zero when this core is one we read SNAC pads from, and the user asked for it.
int  snac_psx_enabled(void);

// Read pad state and drive the OSD from it. Called once per user_io_poll().
void snac_psx_poll(void);

#endif
