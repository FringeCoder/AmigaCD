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
#include <string.h>

#include "../support/minimig/minimig_profile.h"

static int failures = 0;

static void check(const char *name, unsigned got, unsigned want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got 0x%02X, want 0x%02X\n", name, got, want); failures++; }
}

static void check_true(const char *name, int cond)
{
	if (cond) printf("PASS %s\n", name);
	else { printf("FAIL %s\n", name); failures++; }
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

	// An A1200 has 2 MB of chip RAM, like a CD32. Its fast RAM is seeded when
	// the user picks the machine, NOT here: this function runs on every config
	// load, so forcing fast RAM would undo the FastRAM row each time the
	// config was read back. That distinction is the whole reason these two
	// checks exist.
	check("a1200 corrects a CDTV config",
	      amigacd_profile_memory(0x01, 3), 0x03);
	check("a1200 leaves the user's FastRAM choice alone",
	      amigacd_profile_memory(0x13, 3), 0x13);
	check("a1200 does not force FastRAM on a config that has none",
	      amigacd_profile_memory(0x03, 3), 0x03);

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

	// Every machine needs its OWN config file. CD32 used to have no suffix, so
	// its file was the unsuffixed boot config that "Save settings" rewrites --
	// saving on any other machine overwrote CD32's, and the System row then
	// looked frozen because switching to CD32 loaded whatever you had just
	// left. Distinct and non-empty is the whole invariant.
	for (int i = 0; i < AMIGACD_SYS_COUNT; i++)
	{
		const char *a = amigacd_cfg_suffix(i);
		check_true("suffix is not null and not empty", a && a[0]);
		for (int j = i + 1; j < AMIGACD_SYS_COUNT; j++)
		{
			const char *b = amigacd_cfg_suffix(j);
			check_true("suffixes are distinct", a && b && strcmp(a, b));
		}
	}

	// ---- floppy drive count is a machine property -------------------------
	//
	// The bug: amigacd_apply_system forced floppy.drives = 1 for every system,
	// commented "single df0". The field is a count MINUS ONE, so that is two
	// drives -- on CD32 and CDTV, which have none. Kickstart probes what it is
	// offered and spends chip RAM on a trackdisk buffer per drive it finds.
	check("cd32 gets one floppy drive, not two",
	      amigacd_profile_floppy_drives(AMIGACD_SYS_CD32), 0);
	check("cdtv gets one floppy drive, not two",
	      amigacd_profile_floppy_drives(AMIGACD_SYS_CDTV), 0);

	// A500 and A1200 keep two: one internal plus the external DF1 nearly
	// everyone had. A seed, not a clamp -- the user owns it afterwards.
	check("a500 keeps two floppy drives",
	      amigacd_profile_floppy_drives(AMIGACD_SYS_A500), 1);
	check("a1200 keeps two floppy drives",
	      amigacd_profile_floppy_drives(AMIGACD_SYS_A1200), 1);

	// The value is a count minus one packed into two bits by
	// minimig_ConfigFloppy, so anything above 3 would silently truncate.
	for (int sys = 0; sys < AMIGACD_SYS_COUNT; sys++)
		check_true("floppy drive count fits the two-bit field",
		           amigacd_profile_floppy_drives(sys) <= 3);

	// Neither CD machine may claim more drives than a machine that has one.
	check_true("no CD machine offers more drives than an A500",
	           amigacd_profile_floppy_drives(AMIGACD_SYS_CD32)
	             <= amigacd_profile_floppy_drives(AMIGACD_SYS_A500)
	        && amigacd_profile_floppy_drives(AMIGACD_SYS_CDTV)
	             <= amigacd_profile_floppy_drives(AMIGACD_SYS_A500));

	// The on-load correction. Making the drive count a machine property was not
	// enough on its own: amigacd_apply_system() runs only when the user selects
	// a machine or when there is no config at all, so every CD32 config already
	// written kept its stored two drives and the OSD still showed both.
	check("cd32 config load is forced to one drive",
	      amigacd_profile_floppy_drives_onload(1, AMIGACD_SYS_CD32), 0);
	check("cdtv config load is forced to one drive",
	      amigacd_profile_floppy_drives_onload(1, AMIGACD_SYS_CDTV), 0);
	check("cd32 already at one drive is left alone",
	      amigacd_profile_floppy_drives_onload(0, AMIGACD_SYS_CD32), 0);
	check("cd32 with a corrupt count is still clamped",
	      amigacd_profile_floppy_drives_onload(3, AMIGACD_SYS_CD32), 0);

	// Asymmetric on purpose: a preference with an OSD row survives a load.
	check("a500 keeps one drive if that is what the user set",
	      amigacd_profile_floppy_drives_onload(0, AMIGACD_SYS_A500), 0);
	check("a500 keeps two drives",
	      amigacd_profile_floppy_drives_onload(1, AMIGACD_SYS_A500), 1);
	check("a500 keeps four drives",
	      amigacd_profile_floppy_drives_onload(3, AMIGACD_SYS_A500), 3);
	check("a1200 keeps two drives",
	      amigacd_profile_floppy_drives_onload(1, AMIGACD_SYS_A1200), 1);

	// Whatever comes back still has to fit the two-bit field
	// minimig_ConfigFloppy packs it into, for every system and every input.
	for (int sys = 0; sys < AMIGACD_SYS_COUNT; sys++)
		for (uint8_t d = 0; d <= 3; d++)
			check_true("on-load result fits the two-bit field",
			           amigacd_profile_floppy_drives_onload(d, sys) <= 3);

	// And a CD machine can never come out of a load claiming more drives than
	// an A500 seed, whatever the file said.
	for (uint8_t d = 0; d <= 3; d++)
		check_true("no CD machine outlasts an A500 after a load",
		           amigacd_profile_floppy_drives_onload(d, AMIGACD_SYS_CD32)
		             <= amigacd_profile_floppy_drives(AMIGACD_SYS_A500)
		        && amigacd_profile_floppy_drives_onload(d, AMIGACD_SYS_CDTV)
		             <= amigacd_profile_floppy_drives(AMIGACD_SYS_A500));

	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }

	printf("ALL TESTS PASSED\n");
	return 0;
}
