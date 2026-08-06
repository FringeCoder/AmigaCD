#include <linux/input.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../cfg.h"
#include "../../menu.h"
#include "../../input.h"
#include "../minimig/minimig_config.h"
#include "snac_psx.h"

static uint16_t pad[2];
static uint8_t  id[2];
static uint32_t axes[2];

// Defined below; snac_psx_poll() calls it and C++ needs the declaration first.
static void snac_osd_keys(void);

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

	snac_osd_keys();
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
