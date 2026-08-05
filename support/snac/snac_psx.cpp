#include <stdio.h>
#include <string.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../cfg.h"
#include "../../menu.h"
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

// Task 8 fills this in; an empty body keeps this task's build clean and lets the
// poll be verified on hardware before any key injection exists.
static void snac_osd_keys(void)
{
}
