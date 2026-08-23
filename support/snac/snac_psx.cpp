#include <linux/input.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../cfg.h"
#include "../../menu.h"
#include "../../input.h"
#include "../minimig/minimig_config.h"
#include "../lightpen/amiga_lightpen.h"
#include "snac_psx.h"

static uint16_t pad[2];
static uint8_t  id[2];
static uint32_t axes[2];

// Defined below; snac_psx_poll() calls them and C++ needs the declarations first.
static void snac_osd_keys(void);
static void snac_mouse(void);
static void snac_lightpen(void);

int snac_psx_enabled(void)
{
	// Gated on core identity, never on probing the bus: a UIO read to a core
	// with no decode for it returns whatever the bus floats to, and a false
	// positive means phantom menu keys.
	if (is_menu()) return cfg.snac_psx;
	// AmigaCD asks the core, not the ini: the modes live in AmigaCD.cfg so they
	// can differ per saved configuration.
	if (is_amigacd()) return minimig_config.userport == mmup_snacpsx;
	return 0;
}

void snac_psx_poll(void)
{
	if (!snac_psx_enabled()) return;

	// All seven words are read every time: the core streams a fixed-length
	// reply, and a short read leaves the next transaction misaligned. Plan 1
	// (Menu core) had no consumer for the axes and discarded them; Task 4
	// (the analog-stick mouse) is what reads axes[] from here on.
	spi_uio_cmd_cont(UIO_MM2_SNACRD);
	uint16_t status   = spi_w(0);
	pad[0]             = spi_w(0);
	uint16_t axes0_lo  = spi_w(0);
	uint16_t axes0_hi  = spi_w(0);
	pad[1]             = spi_w(0);
	uint16_t axes1_lo  = spi_w(0);
	uint16_t axes1_hi  = spi_w(0);
	DisableIO();

	axes[0] = ((uint32_t)axes0_hi << 16) | axes0_lo;
	axes[1] = ((uint32_t)axes1_hi << 16) | axes1_lo;

	id[0] = status & 0xFF;
	id[1] = status >> 8;

	// A port with nothing in it reports id 0 and must contribute no buttons,
	// so an unplugged pad cannot hold a menu key down.
	if (!id[0]) pad[0] = 0;
	if (!id[1]) pad[1] = 0;

	snac_mouse();
	snac_osd_keys();
	snac_lightpen();
}

// Stick deflection is a velocity, not a position: subtract centre, apply a
// deadzone, scale, and accumulate the fraction so slow movement does not
// quantise to zero. The pointer tolerates a frame of lag where buttons do not,
// which is why this half lives here and the buttons live in RTL.
#define SNAC_MOUSE_DEADZONE 24
#define SNAC_MOUSE_DIV      12

static int mouse_frac_x, mouse_frac_y;

static int snac_axis_delta(uint8_t v, int *frac)
{
	int d = (int)v - 128;
	if (d > -SNAC_MOUSE_DEADZONE && d < SNAC_MOUSE_DEADZONE) return 0;
	*frac += d;
	int out = *frac / SNAC_MOUSE_DIV;
	*frac -= out * SNAC_MOUSE_DIV;
	return out;
}

static void snac_mouse(void)
{
	// minimig_config.snac[] is AmigaCD's per-port setting; it is a single
	// global that a core switch does not clear, so without this guard a
	// mouse mode configured under AmigaCD would keep firing user_io_mouse()
	// from a stale value after switching back to the Menu core.
	if (!is_amigacd()) return;

	for (int p = 0; p < 2; p++)
	{
		unsigned char mode = minimig_config.snac[p] & 0x07;
		if (mode != mmsp_mouse && mode != mmsp_pad_mouse) continue;
		if (id[p] != 0x73) continue;   // axes are only valid in analog mode

		// rtl/snac_psx.v's axes0/axes1 comment reads "{LY, LX, RY, RX}, 8 bits
		// each" and the ST_DONE assignment is
		//   axes0 <= { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] };
		// Verilog concatenation puts the first operand in the MSB, so
		// axes[31:24]=LY, axes[23:16]=LX, axes[15:8]=RY, axes[7:0]=RX. The
		// left stick is the pointer.
		uint8_t lx = (axes[p] >> 16) & 0xFF;
		uint8_t ly = (axes[p] >> 24) & 0xFF;

		int dx = snac_axis_delta(lx, &mouse_frac_x);
		int dy = snac_axis_delta(ly, &mouse_frac_y);
		if (dx || dy) user_io_mouse(0, dx, dy, 0);
	}
}

// Calibration used to live here as a pair of offsets. It now lives with the
// raster mapping in support/lightpen/amiga_lightpen.cpp, so the SNAC gun and a
// USB gun are calibrated by the same four numbers instead of drifting apart.
#define SNAC_GUN_OFFSCREEN_X 0x0001
#define SNAC_GUN_OFFSCREEN_Y 0x000A

// rtl/snac_psx.v packs the last four payload bytes into axes the same way
// for every device (it does not know or care which one it is talking to):
//   axes <= { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] }
// For a GunCon those four bytes are, in wire order, X-LSB, X-MSB, Y-LSB,
// Y-MSB (see rtl/snac_psx.v's device-ID comment and the GunCon reply layout
// this task's brief confirms from PSX_MiSTer's joypad_pad.vhd), so
//   axes[7:0]   = rx_byte[5] = X-LSB
//   axes[15:8]  = rx_byte[6] = X-MSB (bit 0 only; X is 9 bits)
//   axes[23:16] = rx_byte[7] = Y-LSB
//   axes[31:24] = rx_byte[8] = Y-MSB (bit 0 only; Y is 9 bits)
// which makes axes[8:0] the 9-bit X and (axes>>16)[8:0] the 9-bit Y -- the
// same masks the brief gave, verified against the actual concatenation
// rather than assumed from it.
static void snac_lightpen(void)
{
	if (!is_amigacd()) return;

	for (int p = 0; p < 2; p++)
	{
		unsigned char mode = minimig_config.snac[p] & 0x07;
		if (mode != mmsp_lightgun) continue;
		if (id[p] != 0x63) continue;   // axes are only meaningful from a GunCon

		uint16_t gx = (uint16_t)(axes[p] & 0x01FF);          // X, 9 bits
		uint16_t gy = (uint16_t)((axes[p] >> 16) & 0x01FF);  // Y, 9 bits

		// Pointing off the screen is a real state, not an error: the Amiga side
		// must simply not latch, leaving the last position alone.
		if (gx == SNAC_GUN_OFFSCREEN_X && gy == SNAC_GUN_OFFSCREEN_Y) continue;

		// A GunCon reports X in 8 MHz dot-clock ticks and Y in scanlines, both
		// 9 bits, so the full-scale range is the device's own range. The mapping
		// onto the Amiga raster lives in one place for every pointing device --
		// see support/lightpen/amiga_lightpen.cpp.
		amiga_lightpen_abs(gx, gy, 0, 0x1FF, 0, 0x1FF);
	}
}

// menu_key_set() latches a level, not an event: menu_key_get() (menu.cpp)
// derives press/release edges itself by diffing against its own previous
// value, and generates its own auto-repeat for the screens that want it
// (e.g. MENU_FILE_SELECT2, the core browser). So this side must mirror the
// keyboard path in user_io.cpp exactly: send the key on the press edge and
// UPSTROKE|key on the release edge, and nothing else. Do NOT add local
// repeat here -- re-sending an unchanged level produces no edge and can
// never be observed by menu_key_get(), and for MENU_FILE_SELECT2 it cannot
// even be stopped once started because the pad can't clear a level that
// framework repeat keeps re-latching to the same value.
static uint16_t prev_pad;

struct snac_keymap { uint16_t btn; unsigned int key; };

static const snac_keymap osd_keys[] = {
	{ SNAC_UP,     KEY_UP    },
	{ SNAC_DOWN,   KEY_DOWN  },
	{ SNAC_LEFT,   KEY_LEFT  },
	{ SNAC_RIGHT,  KEY_RIGHT },
	{ SNAC_CROSS,  KEY_ENTER },
	{ SNAC_CIRCLE, KEY_ESC   },
};

static void snac_osd_keys(void)
{
	// Both pads drive the menu: whichever one is to hand should work.
	uint16_t now = pad[0] | pad[1];

	// Start+Select toggles the OSD. Edge-detect the combo itself (not the
	// individual buttons) so a second combo produces a fresh press edge and
	// can close what the first combo opened.
	const uint16_t combo = SNAC_START | SNAC_SELECT;
	bool combo_now  = (now      & combo) == combo;
	bool combo_prev = (prev_pad & combo) == combo;
	if (combo_now && !combo_prev) menu_key_set(KEY_F12);
	else if (!combo_now && combo_prev) menu_key_set(UPSTROKE | KEY_F12);

	// Mask Start/Select out of the working set and fall through instead of
	// returning: holding the combo must not swallow a press of any other
	// button that lands while it's held.
	uint16_t work      = now      & ~combo;
	uint16_t prev_work = prev_pad & ~combo;
	uint16_t pressed   = work      & ~prev_work;
	uint16_t released  = prev_work & ~work;

	// menu_key is a single latched level (menu_key_set() above, menu.cpp:568-572):
	// only the LAST call made this invocation survives. A same-poll roll across
	// a diagonal (e.g. Right released and Down pressed inside one ~1.6 ms RTL
	// sample) used to interleave presses and releases in table order and could
	// let a release overwrite a press, destroying it with no later edge to
	// re-emit it. Two passes -- all releases, then all presses -- fix that: the
	// surviving level is always a press when one occurred this poll, and a
	// clobbered upstroke is harmless because the framework re-arms repeat off
	// c1 != c2 regardless of which level was seen in between.
	//
	// Residual case this ordering cannot fix: a press lands in RTL sample N and
	// the same button's release lands in sample N+1, with both falling inside
	// the 20 ms debounce window at menu.cpp:580-584 -- the debounce only ever
	// sees the later level. Fixing that needs deferring releases until nothing
	// is held, which is a separate design change; not attempted here.
	for (unsigned i = 0; i < sizeof(osd_keys) / sizeof(osd_keys[0]); i++)
		if (released & osd_keys[i].btn) menu_key_set(UPSTROKE | osd_keys[i].key);

	for (unsigned i = 0; i < sizeof(osd_keys) / sizeof(osd_keys[0]); i++)
		if (pressed  & osd_keys[i].btn) menu_key_set(osd_keys[i].key);

	prev_pad = now;
}
