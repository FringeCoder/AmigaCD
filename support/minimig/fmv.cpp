// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module, Main's side: the switch, the DDR3 window, the ROM
// file and the clock. Everything else is in fmv_transport.cpp (the contract
// plumbing) and fmv_model.cpp (the chips), which are pure and tested on the
// host. See fmv.h.

#include "fmv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../cfg.h"
#include "../../file_io.h"
#include "../../shmem.h"
#include "../../user_io.h"
#include "fmv_transport.h"
#include "minimig_crc32.h"

static fmv_xport *xport;
static volatile uint8_t *window;
static uint64_t last_us;
static int tried;           // one attempt per core load: a missing ROM is not retried every poll

static uint64_t now_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static void fmv_log(void *, const char *line)
{
	printf("%s\n", line);
}

// The ROM, by the names it goes by: WinUAE's "CD32FMV", and the plain one.
// Beside the CD32 ROMs first, as CD32.rom is found (minimig_config.cpp
// preset_rom_path), then in the root folder.
static uint8_t *load_rom(int *len)
{
	static const char *names[] = { "CD32FMV.rom", "fmv.rom" };
	char path[1024];
	for (int pass = 0; pass < 2; pass++) {
		for (const char *n : names) {
			if (pass == 0)
				snprintf(path, sizeof path, "%s/%s", HomeDir(), n);
			else
				snprintf(path, sizeof path, "%s", n);
			int size = FileLoad(path, 0, 0);
			if (size <= 0)
				continue;
			uint8_t *buf = (uint8_t *)malloc(size);
			if (!buf)
				return 0;
			if (FileLoad(path, buf, size) != size) {
				free(buf);
				continue;
			}
			printf("FMV ROM file: %s\n", path);
			*len = size;
			return buf;
		}
	}
	return 0;
}

static void start()
{
	tried = 1;
	int len = 0;
	uint8_t *rom = load_rom(&len);
	if (!rom) {
		printf("FMV: AMIGACD_FMV=1 but no CD32FMV.rom or fmv.rom found; the board stays off\n");
		return;
	}
	window = (volatile uint8_t *)shmem_map(FMV_WIN_PHYS, FMV_WIN_SIZE);
	if (!window) {
		printf("FMV: cannot map the DDR3 window at 0x%08x\n", FMV_WIN_PHYS);
		free(rom);
		return;
	}
	xport = fmv_xport_new(window, fmv_log, 0);
	int ok = fmv_xport_start(xport, rom, (size_t)len, crc32_compute(rom, len));
	free(rom);
	if (!ok) {
		fmv_xport_free(xport);
		xport = 0;
		shmem_unmap((void *)window, FMV_WIN_SIZE);
		window = 0;
		return;
	}
	user_io_status_set(FMV_STATUS_OPT, 1);
	last_us = now_us();
	printf("FMV: board on. It is configured at the next Amiga reset; Zorro II fast RAM "
	       "is masked out while it is on (fmv-contract.md)\n");
}

void fmv_shutdown(void)
{
	if (xport) {
		fmv_xport_stop(xport);
		fmv_xport_free(xport);
		xport = 0;
	}
	if (window) {
		shmem_unmap((void *)window, FMV_WIN_SIZE);
		window = 0;
	}
	tried = 0;
}

void fmv_poll(void)
{
	if (!is_minimig() || !cfg.amigacd_fmv) {
		if (xport)
			fmv_shutdown();
		return;
	}
	if (!xport) {
		if (!tried)
			start();
		if (!xport)
			return;
	}
	uint64_t t = now_us();
	uint64_t dt = t - last_us;
	last_us = t;
	// A long stall (an OSD file browser, a blocking load) is not replayed as
	// one jump: the chips' schedules would fire a burst of interrupts.
	if (dt > 100000)
		dt = 100000;
	fmv_xport_poll(xport, (uint32_t)dt);
}
