// How status[63:32] is put together for the AmigaCD core. Dependency-free,
// so tests/test_minimig_status.cpp can check it on a host.
#pragma once

// The bits of status[63:32] that the extended config owns: status[50:32],
// the highest being the 0x40000 toggle in the Amiga video/audio menu.
// status[63:51] belongs to cur_status -- the AmigaCD core's save-state and
// test-pattern requests, and the A2065 mode. See user_io_status_set().
#define MINIMIG_EXTCFG_STATUS_MASK 0x0007FFFFu

// Drive sound volume (upstream e836aea). It is extended config bits 20:19,
// where upstream's core reads it as status[52:51]; on this core those are the
// save state request and slot, so it is sent at status[60:59] instead -- bit
// 27 of status[63:32]. AmigaCD.sv's drive_sounds instance reads it there.
#define MINIMIG_DSND_EXT_SHIFT     19
#define MINIMIG_DSND_STATUS_SHIFT  27

// The cartridge is the Action Replay (upstream b57370d, MINIMIG_EXTCFG_AR). It
// is extended config bit 23, where upstream's core reads it as status[55]; on
// this core that is the screen test pattern, so it is sent at status[63] --
// bit 31 of status[63:32], the last bit free on both sides ([62:61] is the CD
// sector rate, [58:56] the A2065 mode). AmigaCD.sv's cart_ar reads it there.
#define MINIMIG_AR_EXT_SHIFT       23
#define MINIMIG_AR_STATUS_SHIFT    31

// status[63:32] from cur_status's copy of it and the extended config the
// core should see (minimig_core_extcfg(): the volume already zeroed when
// drivesounds.bin was not found). The extended config's own bits come from
// ext, the volume goes to [60:59], the cartridge type to [63], and everything
// else is cur_status's.
static inline unsigned int minimig_status_compose(unsigned int cur_hi, unsigned int ext)
{
	const unsigned int dmask = 3u << MINIMIG_DSND_STATUS_SHIFT;
	const unsigned int dsnd  = (ext >> MINIMIG_DSND_EXT_SHIFT) & 3u;
	const unsigned int amask = 1u << MINIMIG_AR_STATUS_SHIFT;
	const unsigned int ar    = (ext >> MINIMIG_AR_EXT_SHIFT) & 1u;
	return (cur_hi & ~MINIMIG_EXTCFG_STATUS_MASK & ~dmask & ~amask)
	     | (ext & MINIMIG_EXTCFG_STATUS_MASK)
	     | (dsnd << MINIMIG_DSND_STATUS_SHIFT)
	     | (ar << MINIMIG_AR_STATUS_SHIFT);
}
