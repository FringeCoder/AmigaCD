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

	// CD32PLUS falls through to 2M with the stock CD32 -- an expanded CD32 is
	// still a CD32, and chip RAM is not what the expansions added.
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
	case AMIGACD_SYS_CD32PLUS:
	default:
		// One internal, plus the external DF1 nearly everyone had. For
		// CD32PLUS this is the point of the machine: an SX-1 gave a CD32 a
		// floppy drive, and a second is the same seed the desktops get.
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
	case AMIGACD_SYS_CD32PLUS:
	default:
		// A preference with an OSD row. Seeded when the machine is selected,
		// the user's from then on. CD32PLUS belongs here and not with the CD
		// machines above: its drives are real, so its count is the user's.
		return drives;
	}
}

uint8_t amigacd_profile_floppy_none(int sys)
{
	switch (sys)
	{
	case AMIGACD_SYS_CD32:
	case AMIGACD_SYS_CDTV:
		return 1;

	case AMIGACD_SYS_A500:
	case AMIGACD_SYS_A1200:
	case AMIGACD_SYS_CD32PLUS:
	default:
		return 0;
	}
}

uint8_t amigacd_profile_has_hdd(int sys)
{
	// Narrow on purpose: only the stock CD32 loses the rows. See the header.
	return (sys == AMIGACD_SYS_CD32) ? 0 : 1;
}

const char *amigacd_system_name(int sys)
{
	switch (sys)
	{
		case AMIGACD_SYS_CDTV:     return "CDTV";
		case AMIGACD_SYS_A500:     return "A500";
		case AMIGACD_SYS_A1200:    return "A1200";
		case AMIGACD_SYS_CD32PLUS: return "CD32+";
		default:                   return "CD32";
	}
}

const char *amigacd_cfg_suffix(int sys)
{
	// Was a pass-through to the display name, and stays identical for the four
	// machines that predate CD32+. It is a switch now because the display name
	// gained a '+', and this string becomes a filename -- AmigaCD_CD32+.cfg.
	// Legal on FAT and exFAT, but a config filename is not the place to find
	// out which of the paths it travels through disagrees.
	switch (sys)
	{
		case AMIGACD_SYS_CD32PLUS: return "CD32PLUS";
		default:                   return amigacd_system_name(sys);
	}
}
