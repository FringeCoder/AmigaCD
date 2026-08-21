// Native unit tests for the AmigaCD machine profile's baked memory fields.
//
// The bug these cover: the CD32 profile (2 MB chip) was applied only when no
// config file existed. Any saved config -- including one carrying the generic
// Minimig default of 0x11, or the CDTV profile's 0x01, both of which are 1 MB
// chip -- was loaded verbatim and never corrected, so CD32 discs ran on a
// machine with half the chip RAM a CD32 has. Titles vary in how much of the
// 2 MB they need, which is why many failed rather than all.

#include <stdint.h>
#include <stdio.h>

#include "../support/minimig/minimig_profile.h"

static int failures = 0;

static void check(const char *name, unsigned got, unsigned want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got 0x%02X, want 0x%02X\n", name, got, want); failures++; }
}

int main(void)
{
	// A CD32 always has 2 MB of chip RAM. Chip size is memory[1:0], where
	// 3 == 2M (config_memory_chip_msg in minimig_config.cpp).
	check("cd32 corrects the generic Minimig default",
	      amigacd_profile_memory(0x11, 0), 0x13);
	check("cd32 corrects a CDTV config",
	      amigacd_profile_memory(0x01, 0), 0x03);
	check("cd32 leaves an already-correct config alone",
	      amigacd_profile_memory(0x03, 0), 0x03);
	check("cd32 corrects 512K",
	      amigacd_profile_memory(0x00, 0), 0x03);
	check("cd32 corrects 1.5M",
	      amigacd_profile_memory(0x02, 0), 0x03);

	// A CDTV has 1 MB of chip RAM: memory[1:0] == 1.
	check("cdtv corrects a CD32 config",
	      amigacd_profile_memory(0x03, 1), 0x01);
	check("cdtv leaves an already-correct config alone",
	      amigacd_profile_memory(0x01, 1), 0x01);
	check("cdtv corrects the generic Minimig default",
	      amigacd_profile_memory(0x11, 1), 0x11);

	// An A500 has 512K chip and the 512K trapdoor card: chip [1:0] == 0 and
	// slow [3:2] == 1. Unlike the CD machines, SlowRAM is part of the profile
	// here rather than the user's choice, so it is forced too.
	check("a500 corrects a CD32 config",
	      amigacd_profile_memory(0x03, 2), 0x04);
	check("a500 corrects a CDTV config",
	      amigacd_profile_memory(0x01, 2), 0x04);
	check("a500 leaves an already-correct config alone",
	      amigacd_profile_memory(0x04, 2), 0x04);
	check("a500 keeps FastRAM and HRTmon",
	      amigacd_profile_memory(0x53, 2), 0x54);
	check("a500 clears slow when the config had more than 512K",
	      amigacd_profile_memory(0x0B, 2), 0x04);

	// Everything outside bits [1:0] is user-adjustable and must survive.
	// FastRAM lives in [5:4] plus bit 7; SlowRAM in [3:2]; bit 6 is HRTmon.
	check("fast RAM preserved",
	      amigacd_profile_memory(0x30, 0), 0x33);
	check("slow RAM preserved",
	      amigacd_profile_memory(0x0C, 0), 0x0F);
	check("hrtmon bit preserved",
	      amigacd_profile_memory(0x40, 0), 0x43);
	check("fast RAM bit 7 preserved",
	      amigacd_profile_memory(0x80, 0), 0x83);
	check("every non-chip bit preserved at once",
	      amigacd_profile_memory(0xFC, 0), 0xFF);
	check("every non-chip bit preserved at once, cdtv",
	      amigacd_profile_memory(0xFC, 1), 0xFD);

	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
