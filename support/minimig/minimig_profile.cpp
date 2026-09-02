// AmigaCD machine profile helpers. See minimig_profile.h for why this file
// carries no MiSTer includes.

#include <stdint.h>

#include "minimig_profile.h"

// Chip RAM field is memory[1:0]; the values match config_memory_chip_msg[] in
// minimig_config.cpp -- 0 = 512K, 1 = 1M, 2 = 1.5M, 3 = 2M.
#define MEM_CHIP_MASK 0x03
#define MEM_CHIP_2M   0x03
#define MEM_CHIP_1M   0x01
#define MEM_CHIP_512K 0x00

// SlowRAM is memory[3:2]: 0 = none, 1 = 512K.
#define MEM_SLOW_MASK 0x0C
#define MEM_SLOW_512K 0x04



uint8_t amigacd_profile_memory(uint8_t memory, int sys)
{
	if (sys == AMIGACD_SYS_A500)
	{
		// 512K chip plus the 512K trapdoor card. SlowRAM is part of the
		// machine here, unlike on the CD machines where it stays the user's.
		return (uint8_t)((memory & ~(MEM_CHIP_MASK | MEM_SLOW_MASK))
		                 | MEM_CHIP_512K | MEM_SLOW_512K);
	}

	uint8_t chip = (sys == AMIGACD_SYS_CDTV) ? MEM_CHIP_1M : MEM_CHIP_2M;
	return (uint8_t)((memory & ~MEM_CHIP_MASK) | chip);
}

uint8_t amigacd_profile_floppy_drives(int sys)
{
	switch (sys)
	{
	case AMIGACD_SYS_CD32:
	case AMIGACD_SYS_CDTV:
		// No floppy drive on either machine. Zero is not expressible, so one.
		return 0;

	case AMIGACD_SYS_A500:
	case AMIGACD_SYS_A1200:
	default:
		// One internal, plus the external DF1 nearly everyone had.
		return 1;
	}
}

uint8_t amigacd_profile_floppy_drives_onload(uint8_t drives, int sys)
{
	switch (sys)
	{
	case AMIGACD_SYS_CD32:
	case AMIGACD_SYS_CDTV:
		// A fact about the machine, so it is forced on every load. See the
		// header for why this is a clamp to one rather than to none.
		return amigacd_profile_floppy_drives(sys);

	case AMIGACD_SYS_A500:
	case AMIGACD_SYS_A1200:
	default:
		// A preference with an OSD row. Seeded when the machine is selected,
		// the user's from then on.
		return drives;
	}
}

const char *amigacd_system_name(int sys)
{
	switch (sys)
	{
		case AMIGACD_SYS_CDTV:  return "CDTV";
		case AMIGACD_SYS_A500:  return "A500";
		case AMIGACD_SYS_A1200: return "A1200";
		default:                return "CD32";
	}
}

const char *amigacd_cfg_suffix(int sys)
{
	return amigacd_system_name(sys);
}
