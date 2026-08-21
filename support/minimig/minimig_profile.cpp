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
