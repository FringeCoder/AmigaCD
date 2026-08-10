// AmigaCD machine profile helpers. See minimig_profile.h for why this file
// carries no MiSTer includes.

#include <stdint.h>

#include "minimig_profile.h"

// Chip RAM field is memory[1:0]; the values match config_memory_chip_msg[] in
// minimig_config.cpp -- 0 = 512K, 1 = 1M, 2 = 1.5M, 3 = 2M.
#define MEM_CHIP_MASK 0x03
#define MEM_CHIP_2M   0x03
#define MEM_CHIP_1M   0x01

uint8_t amigacd_profile_memory(uint8_t memory, int cdtv)
{
	uint8_t chip = cdtv ? MEM_CHIP_1M : MEM_CHIP_2M;
	return (uint8_t)((memory & ~MEM_CHIP_MASK) | chip);
}
