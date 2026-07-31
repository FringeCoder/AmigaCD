#ifndef PHYSICAL_DISC_AUTOBOOT_INCLUDED
#define PHYSICAL_DISC_AUTOBOOT_INCLUDED

// Watch the optical drive while the physical drive is armed, so inserting a
// disc mounts it and removing one unmounts it, without going through the OSD.
//
// Covers two things that used to need a manual "Real CD" selection:
//
//   auto-boot on insert  -- armed with an empty tray, put a disc in, it mounts
//   hot swap             -- change discs mid-session and the Amiga follows
//
// Armed means minimig_config.hardfile[0].filename holds PHYSICAL_DISC_SENTINEL.
// That is the same state the OSD row sets, persists through "Save settings",
// and is cleared by BACKSPACE on the CD row. There is no separate setting.
//
// Call once per Minimig poll. Cheap when idle: a strcmp, and a drive probe at
// most every PHYSICAL_DISC_AUTOBOOT_PERIOD_MS.
void physical_disc_autoboot_poll(void);

// Forget any latched failure and re-probe on the next tick. The OSD calls this
// when the user selects "Real CD" by hand, so an explicit request always gets a
// fresh attempt even after the poller has given up on a bad disc.
void physical_disc_autoboot_reset(void);

#endif
