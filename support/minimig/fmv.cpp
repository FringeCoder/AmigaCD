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
#include <unistd.h>

#include "../../cfg.h"
#include "../../file_io.h"
#include "../../shmem.h"
#include "../../spi.h"
#include "../../user_io.h"
#include "fmv_transport.h"
#include "minimig_config.h"
#include "minimig_crc32.h"

static fmv_xport *xport;
static volatile uint8_t *window;
static uint64_t last_us;
static int tried;
static int diag_shown;      // stall snapshots printed this session
static uint64_t diag_last_us;           // one attempt per core load: a missing ROM is not retried every poll

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
// Looked for beside the Kickstart this config actually loaded (CD32.rom may
// live in games/AmigaCD32 while HomeDir() is games/Amiga, as on the owner's
// card, 2026-11-02), then in HomeDir(), then in the root folder.
static uint8_t *load_rom(int *len)
{
	static const char *names[] = { "CD32FMV.rom", "fmv.rom" };
	char dirs[3][1024];
	int nd = 0;
	const char *ks = minimig_config.kickstart;
	const char *slash = strrchr(ks, '/');
	if (slash && slash > ks) {
		snprintf(dirs[nd], sizeof dirs[nd], "%.*s", (int)(slash - ks), ks);
		nd++;
	}
	snprintf(dirs[nd++], sizeof dirs[0], "%s", HomeDir());
	dirs[nd++][0] = 0;
	char path[1100];
	for (int d = 0; d < nd; d++) {
		for (const char *n : names) {
			if (dirs[d][0])
				snprintf(path, sizeof path, "%s/%s", dirs[d], n);
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
	printf("FMV: looked for CD32FMV.rom / fmv.rom in %s%s%s and the root folder\n",
	       nd > 2 ? dirs[0] : "", nd > 2 ? ", " : "", HomeDir());
	return 0;
}

static void diag_print();

static void start()
{
	tried = 1;
	int len = 0;
	uint8_t *rom = load_rom(&len);
	if (!rom) {
		printf("FMV: AMIGACD_FMV=1 but no CD32FMV.rom or fmv.rom found; the board stays off\n");
		return;
	}
	// status[64] stays clear until the session is set up: while it is, the
	// core holds its ring and queue counters at 0 and leaves DDR3 alone
	// (fmv-contract.md v4), so both sides start from zero. A saved config
	// may have brought the bit up with the core; take it down and give the
	// core time to see it.
	user_io_status_set(FMV_STATUS_OPT, 0);
	usleep(2000);
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
	diag_print();              // the core's side as the session starts
	printf("FMV: board on. It is configured at the next Amiga reset; Zorro II fast RAM "
	       "is masked out while it is on (fmv-contract.md)\n");
}

// The core's FMV diag window over SPI (hps_ext 0xFA00). It does not go
// through DDR3, so it still answers when the DDR3 path is what failed.
static void diag_print()
{
	uint16_t w[22];
	EnableIO();
	spi8(UIO_DMA_READ);
	spi32_w(0xFA00);
	for (int i = 0; i < 22; i++)
		w[i] = (uint16_t)spi_w(0);
	DisableIO();
	char line[1200];
	fmv_diag_format(w, 22, line, sizeof line);
	printf("%s\n", line);
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
	diag_shown = 0;
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

	// A stalled queue: three snapshots of the core's side, a second apart,
	// so the log says which counters move and which state it sits in.
	if (fmv_xport_stalled(xport) && diag_shown < 3 && t - diag_last_us >= 1000000) {
		diag_last_us = t;
		diag_shown++;
		diag_print();
	}
}
