// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module, Main's side (docs/fmv-plan.md). Switched on by the
// Settings page's "FMV module" row (minimig_config.fmv_module, .mgl key
// fmv_module), on a CD32 only, and not with the ARM CPU, which needs core 0
// too. AMIGACD_FMV=1 in MiSTer.ini also switches it on (the test harness
// uses it). Needs
// the cartridge ROM as CD32FMV.rom or fmv.rom: beside the Kickstart the
// config loaded, in HomeDir() (games/Amiga), or in the root folder. With
// either missing, no board appears on the Amiga. Its log is on stdout, which
// is /dev/null unless MiSTer.ini has DEBUG=1 or 2 (cfg.cpp). AMIGACD_FMV=2
// leaves the board off and only prints the core's diag window, to check the
// DDR3 port with the FMV side idle; 3 and 4 add a port reset experiment at
// 3 s (fmv.cpp, diag_only_poll).
#pragma once

// status[64]: the core's FMV front end on (AmigaCD.sv, rtl/fmv). Above
// status[63:32], which this core has no free bit in (minimig_status.h).
#define FMV_STATUS_OPT "[64]"

// Called from the poll loop, and from ide_wait_req() as a2065_poll() is: if
// Main sat in an IDE wait while the 68k streamed MPEG data, the event ring
// would fill, the FPGA would hold the 68k, and neither side would move.
void fmv_poll(void);

// The core is being unloaded or replaced: lower ALIVE and unmap.
void fmv_shutdown(void);

// Whether the board is to be on: the OSD row (or AMIGACD_FMV=1), a CD32,
// and no ARM CPU. Save states are refused while it is (menu.cpp,
// minimig_ss_blocked): the MPEG decoders' state lives on the ARM.
int fmv_wanted(void);
// Whether the ARM CPU is what keeps it off.
int fmv_blocked_by_armcpu(void);
// Whether CD32FMV.rom or fmv.rom is where the board would look for it.
int fmv_rom_found(void);
