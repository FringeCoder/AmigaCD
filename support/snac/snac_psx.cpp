#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../cfg.h"
#include "../../menu.h"
#include "../../hardware.h"
#include "snac_psx.h"

static uint16_t pad[2];
static uint8_t  id[2];

// Defined below; snac_psx_poll() calls it and C++ needs the declaration first.
static void snac_osd_keys(void);

int snac_psx_enabled(void)
{
	// Gated on core identity, never on probing the bus: a UIO read to a core
	// with no decode for it returns whatever the bus floats to, and a false
	// positive means phantom menu keys.
	if (!cfg.snac_psx) return 0;
	return is_menu();
}

void snac_psx_poll(void)
{
	if (!snac_psx_enabled()) return;

	// All seven words must be read even though the axes are unused here: the
	// core streams a fixed-length reply, and a short read leaves the next
	// transaction misaligned. Plan 2 is what gives the axes a consumer.
	spi_uio_cmd_cont(UIO_MM2_SNACRD);
	uint16_t status = spi_w(0);
	pad[0] = spi_w(0);
	spi_w(0);              // axes0 low  -- Plan 2
	spi_w(0);              // axes0 high -- Plan 2
	pad[1] = spi_w(0);
	spi_w(0);              // axes1 low  -- Plan 2
	spi_w(0);              // axes1 high -- Plan 2
	DisableIO();

	id[0] = status & 0xFF;
	id[1] = status >> 8;

	// A port with nothing in it reports id 0 and must contribute no buttons,
	// so an unplugged pad cannot hold a menu key down.
	if (!id[0]) pad[0] = 0;
	if (!id[1]) pad[1] = 0;

	snac_osd_keys();
}

// We are not an evdev device, so nothing gives us key repeat -- the repeat a
// keyboard gets comes from EV_REP, which never reaches us. These match how the
// OSD feels under a keyboard.
#define SNAC_REPEAT_DELAY_MS  500
#define SNAC_REPEAT_RATE_MS   100

static uint16_t      prev_pad;
static unsigned long repeat_at;
static uint16_t      repeat_btn;

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
	uint16_t pressed = now & ~prev_pad;

	// Start+Select opens the OSD, and must not also register as Start.
	if ((now & (SNAC_START | SNAC_SELECT)) == (SNAC_START | SNAC_SELECT))
	{
		if (pressed & (SNAC_START | SNAC_SELECT)) menu_key_set(KEY_F12);
		prev_pad = now;
		return;
	}

	for (unsigned i = 0; i < sizeof(osd_keys) / sizeof(osd_keys[0]); i++)
	{
		if (pressed & osd_keys[i].btn)
		{
			menu_key_set(osd_keys[i].key);

			// Only directions repeat. Fixing repeat_btn/repeat_at here for every
			// key -- including Cross/Circle -- would let a button tap clobber a
			// still-held direction's repeat state, stranding it un-repeating
			// until the direction itself was released and pressed again.
			if (osd_keys[i].btn & (SNAC_UP | SNAC_DOWN | SNAC_LEFT | SNAC_RIGHT))
			{
				repeat_btn = osd_keys[i].btn;
				repeat_at  = GetTimer(SNAC_REPEAT_DELAY_MS);
			}
		}
	}

	uint16_t dirs = now & (SNAC_UP | SNAC_DOWN | SNAC_LEFT | SNAC_RIGHT);

	// The repeat target went away. If another direction is still held -- the
	// other half of a diagonal, or the same direction on the other pad -- hand
	// the repeat to it instead of dropping it outright. Only clear repeat_btn
	// when no direction at all remains held.
	if (repeat_btn && !(now & repeat_btn))
	{
		repeat_btn = 0;
		if (dirs)
		{
			repeat_btn = dirs & (~dirs + 1);   // lowest still-held direction
			repeat_at  = GetTimer(SNAC_REPEAT_DELAY_MS);
		}
	}

	// Held direction: repeat until released. Buttons deliberately do not repeat.
	if (repeat_btn && (now & repeat_btn) && (repeat_btn & (SNAC_UP | SNAC_DOWN | SNAC_LEFT | SNAC_RIGHT)))
	{
		if (CheckTimer(repeat_at))
		{
			for (unsigned i = 0; i < sizeof(osd_keys) / sizeof(osd_keys[0]); i++)
			{
				if (repeat_btn == osd_keys[i].btn) menu_key_set(osd_keys[i].key);
			}
			repeat_at = GetTimer(SNAC_REPEAT_RATE_MS);
		}
	}

	prev_pad = now;
}
