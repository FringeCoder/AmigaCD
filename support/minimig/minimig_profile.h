#ifndef MINIMIG_PROFILE_H
#define MINIMIG_PROFILE_H

#include <stdint.h>

// AmigaCD machine profile: the parts of the config that are properties of the
// machine rather than user preferences.
//
// Chip RAM is one of those. A CD32 has 2 MB and a CDTV has 1 MB; neither is
// adjustable on real hardware, and the AmigaCD OSD deliberately offers no row
// for it (FastRAM and D-Cache are the adjustable ones). But the profile was
// only ever applied when no config file existed, so any saved config -- the
// generic Minimig default of 0x11, or a CDTV config at 0x01, both 1 MB chip --
// was loaded verbatim and left in place while CD32 discs were mounted.
//
// Kept free of MiSTer headers so it can be unit-tested with the host compiler;
// minimig_config.h pulls in file_io.h and thence <unistd.h>.

// The machines this core can be. The value is not persisted directly -- it is
// derived from the saved chipset/cpu bits by amigacd_system() -- so the order
// here is free to change.
enum {
	AMIGACD_SYS_CD32  = 0,
	AMIGACD_SYS_CDTV  = 1,
	AMIGACD_SYS_A500  = 2,
	AMIGACD_SYS_A1200 = 3,
};

// How many machines the System row cycles through. Here rather than at the
// menu, so adding one is a single edit.
#define AMIGACD_SYS_COUNT 4

// Returns `memory` with the RAM fields forced to what the machine actually
// has. Chip RAM is bits [1:0], where 0 == 512K, 1 == 1M, 3 == 2M: a CD32 has
// 2M, a CDTV 1M, an A500 512K. The A500 also gets the 512K trapdoor expansion
// in SlowRAM [3:2], because a bare 512K machine cannot run much and every real
// A500 that mattered had one.
//
// FastRAM [5:4] and [7] and HRTmon [6] are preserved for every system,
// including the A1200: those are the user's to choose, and this function runs
// on every config load, so forcing one here would quietly undo the FastRAM row
// each time the config was read back. The A1200 profile seeds 8 MB when the
// user selects the machine -- see amigacd_apply_system -- and it stays whatever
// they set it to afterwards.
uint8_t amigacd_profile_memory(uint8_t memory, int sys);

#endif
