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

// Returns `memory` with the chip RAM field forced to what the machine actually
// has: bits [1:0], where 3 == 2M (CD32) and 1 == 1M (CDTV). Every other bit --
// SlowRAM [3:2], FastRAM [5:4] and [7], HRTmon [6] -- is preserved, because
// those are the user's to choose.
uint8_t amigacd_profile_memory(uint8_t memory, int cdtv);

#endif
