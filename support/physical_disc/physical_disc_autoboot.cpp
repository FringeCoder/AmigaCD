// Drive watcher for the armed physical CD-ROM drive.
//
// One state machine serves both auto-boot-on-insert and hot swap, because they
// are the same problem seen from different states: something changed in the
// tray, make the mount match it.
//
//     ARMED_IDLE  --disc appears-->  SETTLING  --mount ok-->  MOUNTED
//         ^                              |                       |
//         |                          mount fails                 |
//         |                              v                       |
//         +----------------------  (retry, then latch)           |
//         |                                                      |
//         +--------------------  disc disappears  <--------------+
//
// Deliberately, the *only* thing this file does about the Amiga is mount and
// unmount. It never resets the core and never talks to the bridges. That is
// because akiko_cd32_poll() already contains a carefully-worked-out mediachange
// path, keyed on the cd_is_mounted() edge, which distinguishes a cold boot from
// a hot swap via `had_prior_unmount` and picks the right notification for each.
// Unmounting on eject and mounting on insert produces exactly the edges that
// code was written to consume, so a disc swap now takes the hot-swap branch it
// always had -- rather than us bolting a second, competing reset path beside it.
//
// The same applies to CDTV: cdtv_cd_set_cd_path() runs off the same mount, sets
// cd_media and re-arms the STCH inject.

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>       // CDSL_CURRENT expands via INT_MAX on this kernel
#include <linux/cdrom.h>

#include "../../user_io.h"
#include "../../menu.h"
#include "../../ide.h"
#include "../../hardware.h"
#include "../minimig/minimig_config.h"
#include "physical_disc.h"
#include "physical_disc_autoboot.h"

// Slow enough to be free, fast enough that inserting a disc feels responsive.
#define PHYSICAL_DISC_AUTOBOOT_PERIOD_MS 2000

// A freshly inserted disc reports CDS_DISC_OK before its TOC is readable, so
// wait one extra tick before mounting. Upstream's launch code burns an 8-second
// busy-loop for the same reason; a tick is cheaper and we can simply retry.
#define PHYSICAL_DISC_AUTOBOOT_SETTLE_TICKS 1

// Give up after this many consecutive failed mounts on one disc, so an
// unreadable disc does not re-spin the drive every 2 seconds forever. Cleared
// when the disc is removed, so taking it out and putting it back always retries.
#define PHYSICAL_DISC_AUTOBOOT_MAX_FAILS 5

enum autoboot_state
{
	AB_IDLE = 0,     // armed, nothing mounted, no disc seen
	AB_SETTLING,     // disc seen, letting it spin up
	AB_MOUNTED,      // mounted; watching for the disc to leave
	AB_FAILED,       // gave up on this disc; waits for it to be removed
};

static int           ab_state      = AB_IDLE;
static int           ab_settle     = 0;
static int           ab_fails      = 0;
static unsigned long ab_next_poll  = 0;   // GetTimer/CheckTimer are unsigned long
static int           ab_have_timer = 0;

void physical_disc_autoboot_reset(void)
{
	if (ab_state == AB_FAILED) ab_state = AB_IDLE;
	ab_fails = 0;
	ab_settle = 0;
	ab_have_timer = 0;
}

// True when hardfile[0] is the physical drive rather than an image or nothing.
static int autoboot_armed(void)
{
	return !strcmp(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL);
}

static int autoboot_mounted(void)
{
	return ide_inst[0].drive[0].phys ? 1 : 0;
}

// Is there a disc in the drive?
//
// When mounted, physical_disc already holds the device open, so this is one
// ioctl on an existing fd. When not mounted nothing is open, so probe with our
// own O_NONBLOCK handle and close it again -- deliberately not leaving the
// drive open while idle, which would keep it spun up and defeat the acoustic
// management the module goes to some trouble over.
//
// Returns 1 present, 0 absent, -1 no drive at all.
static int autoboot_disc_present(void)
{
	if (autoboot_mounted()) return physical_disc_disc_present() ? 1 : 0;

	int seen_drive = 0;
	for (int i = 0; i < 8; i++)
	{
		char path[32];
		snprintf(path, sizeof(path), "/dev/sr%d", i);

		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		seen_drive = 1;

		// CDROM_DRIVE_STATUS queries the drive rather than reading media, so it
		// is a sub-millisecond kernel round trip, not something that can stall
		// user_io_poll() the way a sector read can.
		int st = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
		close(fd);

		if (st == CDS_DISC_OK) return 1;
	}
	return seen_drive ? 0 : -1;
}

void physical_disc_autoboot_poll(void)
{
	// Cheapest possible early-out: one strcmp on the common path, and nothing
	// at all for non-Minimig cores.
	if (!is_amigacd() || !autoboot_armed())
	{
		ab_state = autoboot_mounted() ? AB_MOUNTED : AB_IDLE;
		return;
	}

	// Mounted by some other route -- the OSD row, or a saved config restored at
	// core load -- so adopt that state rather than trying to mount again.
	if (ab_state != AB_MOUNTED && autoboot_mounted())
	{
		ab_state = AB_MOUNTED;
		ab_fails = 0;
	}

	if (!ab_have_timer || CheckTimer(ab_next_poll))
	{
		ab_next_poll = GetTimer(PHYSICAL_DISC_AUTOBOOT_PERIOD_MS);
		ab_have_timer = 1;
	}
	else
	{
		return;
	}

	int present = autoboot_disc_present();
	if (present < 0) return;               // no optical drive attached at all

	switch (ab_state)
	{
	case AB_IDLE:
		if (present)
		{
			ab_settle = PHYSICAL_DISC_AUTOBOOT_SETTLE_TICKS;
			ab_state = AB_SETTLING;
		}
		break;

	case AB_SETTLING:
		if (!present)
		{
			ab_state = AB_IDLE;            // taken out again before we got there
			break;
		}
		if (ab_settle > 0) { ab_settle--; break; }

		// Same call the OSD "Real CD" row makes, so there is one mount path in
		// the tree rather than two that can drift apart.
		if (ide_open(0, PHYSICAL_DISC_SENTINEL))
		{
			ab_fails = 0;
			ab_state = AB_MOUNTED;
			Info("Disc inserted", 1500);
		}
		else if (++ab_fails >= PHYSICAL_DISC_AUTOBOOT_MAX_FAILS)
		{
			ab_state = AB_FAILED;
			Info("Disc could not be read", 3000);
		}
		break;

	case AB_MOUNTED:
		if (!present)
		{
			// Drop the mount so cd_is_mounted() goes false. That is the edge the
			// akiko bridge needs to tell the BIOS the disc has gone, and it also
			// sets had_prior_unmount, which routes the *next* insert down the
			// hot-swap branch instead of the cold-boot one.
			ide_open(0, "");
			ab_state = AB_IDLE;
			ab_fails = 0;
			Info("Disc removed", 1500);
		}
		break;

	case AB_FAILED:
		// Latched. Only taking the disc out clears it, so a disc the drive
		// cannot read does not get retried every couple of seconds forever.
		if (!present)
		{
			ab_state = AB_IDLE;
			ab_fails = 0;
		}
		break;
	}
}
