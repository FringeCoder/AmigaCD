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

// The machine's display name, and the config-file suffix that goes with it.
//
// Every machine has a suffix, CD32 included. CD32 used to have none, so its
// file WAS the unsuffixed AmigaCD.cfg that amigacd_cfg_save_current() rewrites
// on every "Save settings" -- and saving while on any other machine therefore
// overwrote CD32's stored config with that machine. The System row then looked
// frozen, because switching to CD32 loaded a copy of wherever you had just
// been. Distinct, non-empty suffixes are the invariant; the test asserts it.
const char *amigacd_system_name(int sys);
const char *amigacd_cfg_suffix(int sys);

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

// How many floppy drives the machine has, as minimig_config.floppy.drives
// wants it: a count MINUS ONE, so 0 is one drive and 1 is two.
//
// This is a machine property in the same sense chip RAM is, and it was not
// being treated as one -- amigacd_apply_system forced the value 1 for every
// system, which is TWO drives, on machines including CD32 and CDTV that have
// none at all. Kickstart probes what it is offered, finds two drives, and
// allocates a trackdisk MFM track buffer per unit. Those buffers live in chip
// RAM because floppy DMA can only reach chip RAM, so the cost lands on the
// 2 MB a CD32 has.
//
// CLAMPED, NOT CHOSEN, for CD32 and CDTV. Neither has a floppy drive, and
// neither the host protocol nor the RTL can say so: minimig_ConfigFloppy packs
// the count into two bits as count-minus-one, and paula_floppy.v hardwires
// drive 0 as always present. One drive is the nearest expressible answer.
// If the RTL ever gains a zero-drive encoding, this function is the only place
// that has to change.
//
// A500 and A1200 get two, which is a seed rather than a clamp -- the same
// reasoning the A500's trapdoor RAM already gets in this file. Both shipped
// with one internal drive, but an external DF1 was near universal and most
// multi-disk software is painful without it. The user owns it after this.
uint8_t amigacd_profile_floppy_drives(int sys);

#endif
