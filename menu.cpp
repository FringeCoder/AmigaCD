/*
Copyright 2005, 2006, 2007 Dennis van Weeren
Copyright 2008, 2009 Jakub Bednarski

This file is part of Minimig

Minimig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

Minimig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// 2009-11-14   - OSD labels changed
// 2009-12-15   - added display of directory name extensions
// 2010-01-09   - support for variable number of tracks
// 2016-06-01   - improvements to 8-bit menu

#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdbool.h>
#include <stdio.h>
#include <sched.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <libgen.h>
#include <bluetooth.h>
#include <hci.h>
#include <hci_lib.h>

#include "file_io.h"
#include "osd.h"
#include "hardware.h"
#include "menu.h"
#include "user_io.h"
#include "debug.h"
#include "fpga_io.h"
#include "cfg.h"
#include "input.h"
#include "battery.h"
#include "cheats.h"
#include "game_docs.h"
#include "video.h"
#include "audio.h"
#include "joymapping.h"
#include "recent.h"
#include "support.h"
#include "bootcore.h"
#include "ide.h"
#include "support/physical_disc/physical_disc.h"
#include "support/physical_disc/physical_disc_autoboot.h"
#include "support/minimig/minimig_ssdiag.h"
#include "support/minimig/akiko_cd32.h"        // akiko_cd32_disc_info for the CD row
#include "support/minimig/mgl_rbf.h"             // rewriting an .mgl's <rbf> in place
#include "support/minimig/mgl_cfg.h"             // the settings block in an .mgl
#include "profiling.h"
#include "str_util.h"
#include "osd_rowguard.h"                       // the per-paint cursor/menumask guard
#include "autofire.h"

/*menu states*/
enum MENU
{
	MENU_NONE1,
	MENU_NONE2,
	MENU_INFO,

	MENU_SYSTEM1,
	MENU_SYSTEM2,
	MENU_COMMON1,
	MENU_COMMON2,
	MENU_MISC1,
	MENU_MISC2,

	MENU_SELECT_INI1,
	MENU_SELECT_INI2,

	MENU_FILE_SELECT1,
	MENU_FILE_SELECT2,
	MENU_CORE_FILE_SELECTED1,
	MENU_CORE_FILE_SELECTED2,
	MENU_CORE_FILE_CANCELED,
	MENU_RECENT1,
	MENU_RECENT2,
	MENU_RECENT3,
	MENU_RECENT4,
	MENU_ABOUT1,
	MENU_ABOUT2,
	MENU_RESET1,
	MENU_RESET2,
	MENU_UNLOCK1,
	MENU_UNLOCK2,
	MENU_UNLOCK3,

	MENU_PROHIB_BTPAIR1,
	MENU_JOYSYSMAP,
	MENU_JOYDIGMAP,
	MENU_JOYDIGMAP1,
	MENU_JOYDIGMAP2,
	MENU_JOYDIGMAP3,
	MENU_JOYDIGMAP4,
	MENU_JOYRESET,
	MENU_JOYRESET1,
	MENU_JOYKBDMAP,
	MENU_JOYKBDMAP1,
	MENU_KBDMAP,
	MENU_KBDMAP1,
	MENU_KBDMAP2,
	MENU_BTPAIR,
	MENU_BTPAIR2,
	MENU_LGCAL,
	MENU_LGCAL1,
	MENU_LGCAL2,
	MENU_PROHIB_BTPAIR2,

	MENU_SCRIPTS_PRE,
	MENU_SCRIPTS_PRE1,
	MENU_SCRIPTS,
	MENU_SCRIPTS1,
	MENU_SCRIPTS_FB,
	MENU_SCRIPTS_FB2,

	MENU_DOC_FILE_SELECTED,
	MENU_DOC_FILE_SELECTED_2,
	MENU_DOC_NO_FBTERM,
	MENU_DOC_NO_FBTERM2,

	MENU_CHEATS1,
	MENU_CHEATS2,

	MENU_UART1,
	MENU_UART2,
	MENU_UART3,
	MENU_UART4,
	MENU_BAUD1,
	MENU_BAUD2,

	MENU_SFONT_FILE_SELECTED,

	MENU_VIDEOPROC1,
	MENU_VIDEOPROC2,
	MENU_COEFF_FILE_SELECTED,
	MENU_GAMMA_FILE_SELECTED,
	MENU_SMASK_FILE_SELECTED,
	MENU_PRESET_FILE_SELECTED,

	MENU_AFILTER_FILE_SELECTED,

	// Generic
	MENU_GENERIC_MAIN1,
	MENU_GENERIC_MAIN2,
	MENU_GENERIC_MAIN3,
	MENU_GENERIC_FILE_SELECTED,
	MENU_GENERIC_IMAGE_SELECTED,
	MENU_CORES1,
	MENU_CORES2,
	MENU_SAVE_CHECK,
	MENU_SAVE_WAIT,

	// Arcade
	MENU_ARCADE_DIP1,
	MENU_ARCADE_DIP2,

	// Minimig
	MENU_MINIMIG_MAIN1,
	MENU_MINIMIG_MAIN2,
	MENU_MINIMIG_VIDEO1,
	MENU_MINIMIG_VIDEO2,
	MENU_MINIMIG_CHIPSET1,
	MENU_MINIMIG_CHIPSET2,
	MENU_MINIMIG_DISK1,
	MENU_MINIMIG_DISK2,
	MENU_MINIMIG_HDFFILE_SELECTED,
	MENU_MINIMIG_CD32FILE_SELECTED,
	MENU_MINIMIG_CDTVFILE_SELECTED,
	MENU_MINIMIG_CD32FILE_START,
	MENU_MINIMIG_CDTVFILE_START,
	MENU_MINIMIG_ADFFILE_SELECTED,
	MENU_MINIMIG_ROMFILE_SELECTED,
	MENU_MINIMIG_EXTROMFILE_SELECTED,
	MENU_MINIMIG_LOADCONFIG1,
	MENU_MINIMIG_LOADCONFIG2,
	MENU_MINIMIG_SAVECONFIG1,
	MENU_MINIMIG_SAVECONFIG2,
	MENU_MINIMIG_PRESET1,
	MENU_MINIMIG_PRESET2,

	// AmigaCD — simplified "console" Minimig variant
	MENU_AMIGACD_MAIN1,
	MENU_AMIGACD_MAIN2,
	MENU_AMIGACD_SETTINGS1,
	MENU_AMIGACD_SETTINGS2,
	MENU_AMIGACD_ADVANCED1,
	MENU_AMIGACD_DRIVES1,
	MENU_AMIGACD_DRIVES2,
	MENU_AMIGACD_ADVANCED2,
	MENU_AMIGACD_NVRCLEAR1,
	MENU_AMIGACD_NVRCLEAR2,
	MENU_AMIGACD_CONFIGS1,
	MENU_AMIGACD_CONFIGS2,
	MENU_AMIGACD_DEFAULTS1,
	MENU_AMIGACD_DEFAULTS2,
	MENU_AMIGACD_RESET1,
	MENU_AMIGACD_RESET2,
	MENU_AMIGACD_CDFILE_SELECTED,
	MENU_AMIGACD_HDFFILE_SELECTED,
	MENU_AMIGACD_ADFFILE_SELECTED,
	MENU_AMIGACD_ROMFILE_SELECTED,
	MENU_AMIGACD_EXTROMFILE_SELECTED,

	// Atari ST
	MENU_ST_MAIN1,
	MENU_ST_MAIN2,
	MENU_ST_SYSTEM1,
	MENU_ST_SYSTEM2,
	MENU_ST_FDD_FILE_SELECTED,
	MENU_ST_HDD_FILE_SELECTED,
	MENU_ST_SYSTEM_FILE_SELECTED,
	MENU_ST_LOAD_CONFIG1,
	MENU_ST_LOAD_CONFIG2,
	MENU_ST_SAVE_CONFIG1,
	MENU_ST_SAVE_CONFIG2,

	// Archie
	MENU_ARCHIE_MAIN1,
	MENU_ARCHIE_MAIN2,
	MENU_ARCHIE_MAIN_FILE_SELECTED,

	// MT32-pi
	MENU_MT32PI_MAIN1,
	MENU_MT32PI_MAIN2,

	//Advanced Button Map
	MENU_ADVANCED_MAP_LIST1,
	MENU_ADVANCED_MAP_LIST2,
	MENU_ADVANCED_MAP_EDIT1,
	MENU_ADVANCED_MAP_EDIT2,
	MENU_ADVANCED_MAP_EDIT3,
	MENU_ADVANCED_MAP_EDIT4,
	MENU_ADVANCED_MAP_CAPTURE1,
	MENU_ADVANCED_MAP_KEYCAPTURE1,
	// Atari 8bit cartridge type selection
	MENU_ATARI8BIT_CART1,
	MENU_ATARI8BIT_CART2,

	// Not a state. The count, for the per-state "warned once" table the row
	// guard in HandleUI keeps. Must stay last.
	MENU_STATE_COUNT,
};

static uint32_t menustate = MENU_NONE1;
static uint32_t parentstate;
static uint32_t menusub = 0;
static uint32_t menusub_last = 0; //for when we allocate it dynamically and need to know last row
static uint64_t menumask = 0; // Used to determine which rows are selectable...
static uint32_t menu_timer = 0;
static uint32_t menu_save_timer = 0;
static uint32_t load_addr = 0;
static int32_t  bt_timer = 0;
static bool     bt_present = false;
static bool     bt_pairing = false;

static bool osd_unlocked = 1;
static char osd_code_entry[32];
static uint32_t osd_lock_timer = 0;


extern const char *version;

const char *config_tos_wrprot[] = { "None", "A:", "B:", "A: and B:" };

const char *config_scanlines_msg[] = { "Off", "HQ2x", "CRT 25%" , "CRT 50%" , "CRT 75%" };
const char *config_blank_msg[] = { "Blank", "Blank+" };
const char *config_dither_msg[] = { "off", "SPT", "RND", "S+R" };
const char *config_autofire_msg[] = { "        AUTOFIRE OFF", "        AUTOFIRE FAST", "        AUTOFIRE MEDIUM", "        AUTOFIRE SLOW" };
// Which df unit the AmigaCD floppy browser was opened for, so
// MENU_AMIGACD_ADFFILE_SELECTED inserts into the row that asked.
static int amigacd_fdd_unit = 0;
// Which hardfile slot the Hard Disk browser is filling: 1 for Hard Disk 0
// (IDE0 slave) and 2 for Hard Disk 1 (IDE1 master). The browser returns on
// its own menustate long after the row that opened it has gone, the same
// reason amigacd_fdd_unit above exists.
static int amigacd_hdf_slot = 1;

// Which recent-files list each AmigaCD row owns.
//
// One per row, because the list a row offers is also the list the file
// browser's ` key opens and the list "Clear the List" empties. Two rows
// sharing one list is not a cosmetic problem: picking an entry in a recent
// list jumps straight to that row's fs_MenuSelect (menu.cpp, MENU_RECENT2),
// so an HDF offered in the CD row's list was mounted as a CD image -- cfg=2,
// ide_open(0, ...) -- and an ISO offered in a Hard Disk row became a fixed
// disk. Both rows wrote 500 and both read 500, so that was every session.
//
// 0 and 1 are df0 and df1, which is what the floppy rows already read
// (recent_init(fu)); only the write was wrong, pinned at 0, so df1's list
// could never fill and df0's collected both drives' disks.
//
// 501 is CD images, which is what the full Minimig tree already calls its
// CD32 list (MENU_MINIMIG_MAIN2). The two trees share a namespace whenever
// /media/fat/amigacd_force puts a Minimig-named core into this OSD -- the
// file is <core name>_recent_<idx>.cfg -- so agreeing on the meaning is
// better than picking a free number. 502 is that tree's CDTV list and is
// left to it; 503 and 504 are new.
#define AMIGACD_RECENT_DF(u)    (u)              // df0 -> 0, df1 -> 1
#define AMIGACD_RECENT_CD       501
#define AMIGACD_RECENT_HDF(s)   (502 + (s))      // hardfile 1 -> 503, 2 -> 504

const char *config_joystick_mode[] = { "Digital", "Analog", "CD32", "Analog" };
const char *config_button_turbo_msg[] = { "OFF", "FAST", "MEDIUM", "SLOW" };
const char *config_button_turbo_choice_msg[] = { "A only", "B only", "A & B" };
const char *joy_button_map[] = { "RIGHT", "LEFT", "DOWN", "UP", "BUTTON A", "BUTTON B", "BUTTON X", "BUTTON Y", "BUTTON L", "BUTTON R", "SELECT", "START", "KBD TOGGLE", "MENU", "    Stick 1: Tilt RIGHT", "    Stick 1: Tilt DOWN", "   Mouse emu X: Tilt RIGHT", "   Mouse emu Y: Tilt DOWN" };
const char *joy_ana_map[] = { "    DPAD test: Press RIGHT", "    DPAD test: Press DOWN", "   Stick 1 Test: Tilt RIGHT", "   Stick 1 Test: Tilt DOWN", "   Stick 2 Test: Tilt RIGHT", "   Stick 2 Test: Tilt DOWN" };
const char *config_stereo_msg[] = { "0%", "25%", "50%", "100%" };
const char *config_dsnd_msg[] = { "Off", "Quiet", "Normal", "Loud" };
const char *config_uart_msg[] = { "      None", "       PPP", "   Console", "      MIDI", "     Modem", "UDP", "SNI"};
const char *config_midilink_mode[] = {"Local", "Local", "  USB", "  UDP", "-----", "-----", "  USB" };
const char *config_afilter_msg[] = { "Internal","Custom" };
const char *config_smask_msg[] = { "None", "1x", "2x", "1x Rotated", "2x Rotated" };
const char *config_scale[] = { "Normal", "V-Integer", "HV-Integer-", "HV-Integer+", "HV-Integer", "???", "???", "???" };

// Save state slots. Four, matching the four DDR3 windows process_ss() maps at
// the core's advertised base (AmigaCD.sv CONF_STR "SS3C000000:C00000"), and
// matching the four .ss1-.ss4 files FileGenerateSavestatePath() names. Labelled
// 1-4 for the user; the wire value in status[53:52] is 0-3.
static const char *config_ss_slot_msg[] = { "1", "2", "3", "4" };

// CD sector delivery rate, mm_cdSpeed. The multipliers are what they measure
// against a 1x drive's 75 sectors/s: the shipped floor of one sector every
// 1700 us is 588/s, near enough 8x and not a rate any real machine produces;
// a CD32 is a 2x unit at 150/s. Labelled by what they are rather than "Fast"
// and "Slow", because the slow one is the accurate one.
const char *config_cd_speed_msg[] = { "Fast (~8x)", "CD32 (2x)", "Drive (asked)" };

// Save states are refused in two configurations. The OSD greys the rows out so
// the user is told, rather than pressing SELECT and having nothing happen:
//
//  - Z3 fast RAM. The savestate windows start at DDR3 0x3C000000, which is
//    inside the region ddram_ctrl.v hands to Z3 fast RAM, so the two cannot
//    coexist. Moving the windows does not help: Z3_1 covers the whole of
//    0x30000000-0x3FFFFFFF. Z2 fast RAM is a different matter -- it sits at
//    0x30000000-0x307FFFFF, clear of the windows, and IS carried in the save
//    state. Fast RAM index >= 4 means a Z3 board (cpu_wrapper.v ac_memcard[2]
//    selects the 128/256 MB Z3 autoconfig entries). The index is decoded here
//    exactly as the FastRAM row itself decodes it.
//  - 68000. cpu_wrapper exports TG68K state only -- fx68k has no equivalent --
//    so AmigaCD.sv gates the request on `ss_supported = |cpucfg`, and userio.v
//    takes cpucfg from the low two bits of the CPU config byte, i.e.
//    minimig_config.cpu & 3. Without this the request would be silently
//    swallowed in RTL.
// Name the save state after the mounted disc.
//
// hardfile[0].filename cannot be used directly: for a physical disc it holds
// PHYSICAL_DISC_SENTINEL, "*PHYSICAL_DISC*". That has no '.', which used to
// segfault FileGenerateSavestatePath outright, and even with that fixed the
// '*' is not a legal exfat filename character, so the state could never be
// written. Use the same identity per-game NVRAM already uses -- the ISO9660
// volume label, or a TOC-geometry UUID for discs without one.
static const char *amigacd_ss_name();

// ---------------------------------------------------------------------------
// System row: show the choice now, apply it when the stepping stops
// ---------------------------------------------------------------------------
// Stepping CD32 -> CDTV -> A500 -> A1200 used to do the whole switch on every
// press: write the leaving machine's config out, read the arriving one back,
// resolve its ROMs, and re-open the CD image. Three presses to cross the list
// meant three of those, so the row answered in its own time rather than in the
// user's -- and two thirds of that work was for machines being passed through.
//
// So the press only moves a pending value, and the work happens once the row
// has been still for AMIGACD_SYS_APPLY_MS. Same end state, one apply.
//
// The pending value is not a preview of something that might not happen: it is
// committed on the timer, and committed immediately if the user leaves the row,
// the page or the OSD first. There is no path that shows a machine in the row
// and leaves a different one running -- that would be worse than being slow.
#define AMIGACD_SYS_APPLY_MS 500

static int      sys_pending = -1;      // -1 = nothing armed
static uint32_t sys_pending_timer = 0;

// The work itself, unchanged from when it ran inside the row handler -- only
// when it runs has changed.
static void amigacd_system_apply(int sys)
{
	int old_sys = amigacd_system();

	// Each machine keeps its own settings. The machine being left is written
	// out first, then the one arriving is read back if it has ever been
	// configured -- so the discs, hard disks, RAM and ROM of both survive a
	// round trip through the row.
	if (!amigacd_switch_system(old_sys, sys))
	{
		// First time on this machine: build it from the profile. A missing
		// default ROM is worth saying out loud -- the machine still boots, but
		// as whatever Amiga the old ROM makes it, and the only other clue is
		// the Main ROM row two menus away.
		if (!amigacd_apply_system(sys))
		{
			const char *mname = 0;
			amigacd_default_rom_names(sys, &mname, 0);
			char msg[128];
			snprintf(msg, sizeof(msg), "%s ROM %s not found",
			         amigacd_system_name(sys), mname);
			Info(msg, 2500);
		}

		// The A1200 has no CD drive, so a disc mounted under a previous system
		// has to go with it -- otherwise the row keeps naming a disc that
		// nothing can read.
		if (sys == AMIGACD_SYS_A1200)
		{
			minimig_config.hardfile[0].filename[0] = 0;
			ide_open(0, "");
		}
		// Refresh the active bridge with the mounted CD, if any.
		else if (minimig_config.hardfile[0].filename[0])
			ide_open(0, minimig_config.hardfile[0].filename);
	}
}

// What the row should say: the pending choice while one is armed, otherwise the
// machine actually running.
static int amigacd_system_shown()
{
	return (sys_pending >= 0) ? sys_pending : amigacd_system();
}

// Commit now, whatever the timer says. Called when the timer expires and from
// every path that leaves the row. Returns 1 if a machine was actually switched,
// which is the caller's cue to redraw: the switch changes which rows the page
// has, not just what this one says.
static int amigacd_system_flush()
{
	if (sys_pending < 0) return 0;

	int sys = sys_pending;
	sys_pending = -1;
	sys_pending_timer = 0;

	if (sys == amigacd_system()) return 0;   // stepped all the way back round

	amigacd_system_apply(sys);
	return 1;
}

// Arm or re-arm. Each press pushes the deadline out, so holding a direction
// walks the list at the menu's own repeat rate and applies once at the end.
static void amigacd_system_arm(int sys)
{
	sys_pending = sys;
	sys_pending_timer = GetTimer(AMIGACD_SYS_APPLY_MS);
	// GetTimer can return 0, and 0 is this code's "nothing armed". The bluetooth
	// icon timer above guards the same way for the same reason; without it the
	// apply would wait for the user to leave the row instead of for the timer.
	if (!sys_pending_timer) sys_pending_timer = 1;
}

// Called from the page's input state on every pass, which is what makes the
// deadline a deadline rather than something only the next keypress notices.
static int amigacd_system_tick()
{
	if (sys_pending >= 0 && sys_pending_timer && CheckTimer(sys_pending_timer))
		return amigacd_system_flush();

	return 0;
}

// Full-screen preview of what a save slot holds, shown while the cursor sits on
// a Save slot row. The picture is the frame process_ss() captured at save time;
// see ss_thumb_path_for_slot().
//
// Driven from the page DRAW rather than from the keypress, so it follows the
// cursor without every path that can move it having to remember. The draw runs
// on entering the page, on every cursor move and on every slot change, which is
// exactly the set of moments the preview should change.
//
// It cannot follow the cursor OFF the page that way, because another page's
// draw knows nothing about this one -- HandleUI() turns it off centrally for
// any state that is not one of the two pages carrying these rows.
static void amigacd_ss_preview(int on, int slot)
{
	static int shown_slot = -1;

	if (!on)
	{
		if (shown_slot >= 0)
		{
			shown_slot = -1;
			video_fb_preview_off();
		}
		return;
	}

	if (shown_slot == slot) return;

	char thumb[1024];
	if (!ss_thumb_path_for_slot(amigacd_ss_name(), slot, thumb, sizeof(thumb))
	    || !FileExists(thumb)
	    || !video_fb_preview(getFullPath(thumb)))
	{
		// No picture for this slot -- an empty slot, or one saved before
		// thumbnails existed. Leave the core's own video up rather than
		// showing black or the previous slot's picture, which would both
		// read as "this slot holds that".
		amigacd_ss_preview(0, 0);
		return;
	}

	shown_slot = slot;
}

// What is actually in a save state slot, for the Save slot row.
//
// The row used to read "Save slot: 2" and nothing else. The thumbnail preview
// beside it only appears when a .png exists, so an empty slot, a slot saved
// before thumbnails existed and a slot belonging to media that is not mounted
// all looked identical -- four numbers with nothing behind them, which is not
// a choice anyone can make. The file is already named by
// FileGenerateSavestatePath(); one stat says whether it is there and when it
// was written.
//
// The longest thing it writes is a timestamp, "16 Sep 21:35", at 12
// characters. " Save slot: N  " is 15, so the row tops out at 27 of the 32
// columns a line has.
static void amigacd_ss_slot_label(int slot, char *out, size_t outsz)
{
	if (!out || outsz < 2) return;
	out[0] = 0;

	// No disc and no HDF means there is nothing to name a save after, so the
	// slots do not refer to anything yet. Saying so here is the same refusal
	// minimig_ss_request_save() gives on the press, one step earlier.
	const char *media = amigacd_ss_name();
	if (!media || !media[0])
	{
		snprintf(out, outsz, "(no media)");
		return;
	}

	// Side effect worth knowing about: this also mkdir()s the savestates
	// directory. It is silent, it is the same call the thumbnail preview
	// already makes, and mkdir on an existing directory is a cheap EEXIST.
	char path[1024];
	FileGenerateSavestatePath(media, path, (slot & 3) + 1);

	struct stat64 *st = getPathStat(path);
	if (!st || !st->st_size)
	{
		snprintf(out, outsz, "(empty)");
		return;
	}

	// Day, month and time rather than a full date: the year is noise for a
	// file you wrote this week, and the line has no room for it.
	time_t t = (time_t)st->st_mtime;
	struct tm tm = *localtime(&t);
	strftime(out, outsz, "%d %b %H:%M", &tm);
}

static const char *amigacd_ss_name()
{
	static char name[256];
	const char *f = minimig_config.hardfile[0].filename;

	// No CD: name the state after a hard disk instead. The A1200 profile has
	// no CD drive at all, and its software lives in the HDF -- without this a
	// machine that can be saved has nothing to name the file after, and
	// process_ss is never armed, so the save is captured into DDR3 and never
	// reaches the card.
	//
	// Both hard disks, in row order. This consulted hardfile[1] alone and
	// stopped, so a machine with only Hard Disk 1 filled -- the IDE1 master,
	// which is a perfectly good place to keep a Workbench install -- had media
	// mounted, could be saved, and was refused for want of a name.
	if (!f[0])
	{
		if (minimig_config.hardfile[1].filename[0]) return minimig_config.hardfile[1].filename;
		return minimig_config.hardfile[2].filename;
	}

	if (strcmp(f, PHYSICAL_DISC_SENTINEL)) return f;   // an image path: use it

	if (physical_disc_save_name(PHYSICAL_DISC_DISC_NONE, name, sizeof(name)) &&
	    name[0])
	{
		return name;
	}

	// No label and no geometry to hash: still needs a name a file can carry.
	strcpy(name, "PhysicalDisc");
	return name;
}

// Whether the CPU byte can have Zorro III fast RAM: the 68030 and the
// unthrottled 68020. The 68EC020 (68020 with bit 5, the stock-speed throttle)
// has 24-bit addresses, and cpu_wrapper folds a Z3 size down to 8 MB of Z2
// for it, as it does for the 68000 and 68010. Upstream e836aea's expression,
// named, because the menus spell it out in five places.
static int minimig_fast_z3(unsigned char cpu)
{
	return ((cpu & 0x23) != 0x23) && (cpu & 2);
}

static int minimig_ss_blocked()
{
	int fastram = ((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5);
	// The 68030 is allowed, with or without its MMU, since format 1.5 put the
	// MMU registers and the rest of CACR in the state (see ss_supported in
	// AmigaCD.sv). The 68000 (fx68k) and Zorro III fast RAM stay refused.
	int cpu = minimig_config.cpu & 0x03;
	return (minimig_fast_z3(minimig_config.cpu) && fastram >= 4) || (cpu == 0);
}

// Ask the core for a save, from either menu.
//
// Both save rows used to raise status[51] themselves, and only the AmigaCD one
// armed process_ss() first. The other did not, so a save pressed from the
// Minimig System page was captured into the DDR3 window by the core, announced
// as "Save state: saved" -- that toast is the CORE's, and it means the capture
// finished, not that anything reached the card -- and then never written,
// because the host poller only writes when the window's counter word differs
// from the last one it saw and nothing had reset that. Measured on hardware:
// core says saved at t=2453, no "Saving the state" from the writer ever
// follows, no file on the card, and the next restore quietly loads the
// PREVIOUS save and looks like it worked.
//
// Arming is also what resets ss_cnt[] to 0xFFFFFFFF, so it is not optional
// even when the poller is already enabled from an earlier save this session.
//
// Returns 0 without asking the core for anything when there is no name to save
// under, because a capture nothing can write is worse than a refusal: it looks
// like it worked.
static int minimig_ss_request_save()
{
	const char *name = amigacd_ss_name();
	if (!name || !name[0])
	{
		Info("No disc or HDF - cannot name the save", 2500);
		return 0;
	}

	// Idempotent: re-maps the same four windows, reloads them from the file on
	// disk and re-stamps the counter word.
	process_ss(name, 1);

	// The core reports "saved" long before the file exists -- the poller writes
	// it. Ask that poller to hurry, so a reset straight after the toast cannot
	// leave the previous save on the card pretending to be this one.
	ss_expect_save();

	// The core edge-detects this in RTL (AmigaCD.sv ss_save_edge), so a
	// set/clear pair is one save and a bit left high cannot re-arm. Clearing it
	// again is housekeeping on the persisted status word, not a safety
	// property.
	user_io_status_set("[53:52]", minimig_config.ss_slot & 3);
	user_io_status_set("[51]", 1);
	user_io_status_set("[51]", 0);
	return 1;
}

// Ask the core for a restore, from either the menu row or the hotkey.
//
// Lifted out of the Settings row so the two cannot drift: a hotkey that armed
// process_ss() differently from the row would be a second, subtly different
// restore path, and the whole reason this refuses rather than firing blind is
// that the difference is invisible until it succeeds at the wrong thing.
//
// The caller closes the OSD if it has one open. This does not touch menustate:
// the hotkey path has no menu to close, and Info() below needs menustate at or
// below MENU_INFO to draw at all.
//
// Returns 0 without asking the core for anything when there is no name to
// restore from.
static int minimig_ss_request_restore()
{
	const char *ss_name = amigacd_ss_name();
	if (!ss_name || !ss_name[0])
	{
		// Refuse rather than fire the edge at an unloaded window.
		//
		// The save path already did this; the load path did not, and the
		// asymmetry was the bug. The name comes from the mounted media --
		// amigacd_ss_name() is the CD, or the HDF when there is no CD -- so
		// with nothing mounted it is empty, process_ss() was skipped, and
		// status[54] was set anyway.
		//
		// process_ss() is what maps the four DDR3 windows, ZEROES them and
		// reads the .ss files in. Skipping it does not leave the window empty,
		// it leaves it untouched -- so the core validated whatever DDR3
		// happened to hold. Usually that fails SS_MAGIC and the machine is left
		// alone, which is why this was not noticed. But mount a game, save or
		// restore, unmount it, then press restore: the previous payload is
		// still resident, passes magic, CRC and the Kickstart check, and the
		// core restores a machine belonging to media that is not in the drive.
		// It does not fail -- it succeeds at the wrong thing, which is the
		// worse of the two.
		//
		// The Z3 and 68000 refusals grey the row out so the user is told rather
		// than left pressing SELECT at nothing; this one cannot be known until
		// the press, so it toasts instead.
		Info("No disc or HDF - nothing to restore", 2500, 0, 0, 0, 1);
		return 0;
	}

	// This is not housekeeping, it is the load. process_ss() maps the four DDR3
	// windows AND reads the four .ss files off disk into them. Nothing else on
	// this core ever calls it but the save path, so without this the window the
	// core is about to validate holds whatever was there, and a restore after a
	// power cycle -- the whole point of the feature -- could never work.
	//
	// It re-reads all four slots every press, ~16 MB. That is the cost of
	// always validating the freshest file on disk rather than a stale window,
	// on a keypress the user made deliberately. It also resets the poller's
	// counters, so it cannot itself trigger a spurious write back to disk.
	process_ss(ss_name, 1);

	// status[54], edge-detected in RTL (AmigaCD.sv ss_load_edge) exactly as
	// status[51] is. A level here would restore over and over -- ss_ctrl
	// re-arms out of idle on any cycle load_req is high -- and the bit being
	// cleared by userspace must not be what stands between the user and a
	// machine that never runs again. The set/clear pair below is housekeeping
	// on the persisted status word, not the safety property.
	//
	// Everything after this is the core's: it validates the window with the
	// Amiga still running, refuses without freezing if any gate fails, and
	// reports the outcome as an OSD toast either way (AmigaCD.sv ss_info_*).
	user_io_status_set("[53:52]", minimig_config.ss_slot & 3);
	// The drive travels beside the payload rather than in it: where the head is
	// and what was playing are userspace state the core cannot see. Applied
	// here so the restored machine's first sector request is served from the
	// position it was saved at.
	ss_apply_host_state(minimig_config.ss_slot & 3);
	user_io_status_set("[54]", 1);
	user_io_status_set("[54]", 0);
	return 1;
}

// Save states without opening the OSD. Called from user_io_kbd(), which is the
// only place a keypress is visible while the OSD is CLOSED -- HandleUI's key
// switch never runs then, because menu_key_set() is only called when the OSD is
// up. The screenshot hotkey lives in the same chain for the same reason.
//
// Returns 1 when it acted, which is also what tells the caller the key was
// consumed. It always returns 1 for this core, refusals included: a hotkey that
// silently passed Ctrl+Alt+F1 to the Amiga on a machine that cannot save states
// would be worse than one that says why.
int amigacd_ss_hotkey(int action)
{
	if (!is_amigacd()) return 0;

	if (minimig_ss_blocked())
	{
		// The same two conditions that grey the Settings rows out. There they
		// are visible before the press; here there is nothing to grey.
		Info("Save states need 020 and no Z3 RAM", 2500, 0, 0, 0, 1);
		return 1;
	}

	switch (action)
	{
	case SS_HOTKEY_SLOT:
	{
		int slot = (minimig_config.ss_slot + 1) & 3;
		minimig_config.ss_slot = (unsigned char)slot;
		char msg[32];
		sprintf(msg, "Save slot %d", slot + 1);
		Info(msg, 1500);
		return 1;
	}

	case SS_HOTKEY_SAVE:
		minimig_ss_request_save();
		return 1;

	case SS_HOTKEY_RESTORE:
		minimig_ss_request_restore();
		return 1;
	}

	return 1;
}

#define DPAD_NAMES 4
#define DPAD_BUTTON_NAMES 12  //DPAD_NAMES + 6 buttons + start/select

#define script_line_length 1024
#define script_lines 50
static FILE *script_pipe;
static int script_file;
static char script_command[script_line_length];
static int script_line;
static char script_output[script_lines][script_line_length];
static char script_line_output[script_line_length];
static bool script_finished;

// one screen width
static const char* HELPTEXT_SPACER = "                                ";
static char helptext_custom[1024];

enum HelpText_Message
{
	HELPTEXT_NONE, HELPTEXT_CUSTOM, HELPTEXT_MAIN, HELPTEXT_HARDFILE, HELPTEXT_CHIPSET, HELPTEXT_MEMORY, HELPTEXT_EJECT, HELPTEXT_CLEAR,
	HELPTEXT_SSHOTKEY,
	// Same buffer as HELPTEXT_CUSTOM, different entry purely so it can have a
	// different timeout. See helptext_timeouts[].
	HELPTEXT_ROWHINT
};

static const char *helptexts[] =
{
	0,
	helptext_custom,
	"                                Welcome to MiSTer! Use the cursor keys to navigate the menus. Use space bar or enter to select an item. Press Esc or F12 to exit the menus. Joystick emulation on the numeric keypad can be toggled with the numlock or scrlock key, while pressing Ctrl-Alt-0 (numeric keypad) toggles autofire mode.",
	"                                Minimig can emulate an A600/A1200 IDE harddisk interface. The emulation can make use of Minimig-style hardfiles (complete disk images) or UAE-style hardfiles (filesystem images with no partition table).",
	"                                Minimig's processor core can emulate a 68000 (cycle accuracy as A500/A600), a 68EC020 at A1200 speed, or a 68030 with or without MMU (maximum performance), all with transparent cache. The 68030 has no FPU, and needs a core that offers it.",
	"                                Minimig can make use of up to 2 megabytes of Chip RAM, up to 1.5 megabytes of Slow RAM (A500 Trapdoor RAM), and up to 384 megabytes of Fast RAM (8MB max for 68000 mode). To use the HRTmon feature you will need a file on the SD card named hrtmon.rom.",
	"                                Backspace key (or B-hold + A on gamepad) to unmount",
	"                                Backspace key (or B-hold + A on gamepad) to clear stored option. You have to reload the core to be able to use default value.",
	// The hotkeys have nowhere else to be announced: this page is exactly full
	// at sixteen lines, and a hotkey nobody is told about is not a feature.
	"                                Save states also work with this menu closed, which is the point of them: Ctrl+Alt+F1 saves to the selected slot, Ctrl+Alt+F2 restores it, Ctrl+Alt+F3 steps to the next slot. The Amiga never sees those keys. Clear NVRAM erases the CD32 saved games for the disc in the drive, which is not the same thing as a save state.",
	// HELPTEXT_ROWHINT. The same buffer HELPTEXT_CUSTOM points at: what makes
	// it a separate entry is the timeout below.
	helptext_custom,
};

// One per helptexts[] entry, and they must stay the same length: helptext_idx
// indexes both.
//
// Ten seconds is the upstream value and the right one for a page-level message:
// it is there for someone who has stopped and is stuck. A per-ROW hint is a
// different thing -- it answers "what is this one?" while the cursor is moving
// -- and at ten seconds it would never be seen, because every keypress resets
// this timer. Hence the separate entry with a short one.
//
// 1500 ms is a deliberate middle. Shorter and the bottom line animates in and
// out while scrolling past rows; longer and it stops answering the question
// anyone is actually asking.
static const uint32_t helptext_timeouts[] =
{
	10000,
	10000,
	10000,
	10000,
	10000,
	10000,
	10000,
	10000,
	10000,
	1500        // HELPTEXT_ROWHINT
};

// helptext_idx indexes both, and nothing else makes them agree -- adding a
// message and forgetting its timeout reads a uint32_t past the end of this
// array, which is a bad way to find out.
static_assert(sizeof(helptexts) / sizeof(helptexts[0])
              == sizeof(helptext_timeouts) / sizeof(helptext_timeouts[0]),
              "helptexts[] and helptext_timeouts[] must have one entry each per HelpText_Message");

static const char *info_top = "\x80\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x82";
static const char *info_bottom = "\x85\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x84";

// file selection menu variables
static char fs_pFileExt[13] = "xxx";
static uint32_t fs_ExtLen = 0;
static uint32_t fs_Options;
static uint32_t fs_MenuSelect;
static uint32_t fs_MenuCancel;
static int fs_RecentIdx = -1;

// Which recent list the browser is currently working with.
//
// fs_RecentIdx is what the caller of SelectFile asked for; the rest is the
// guess for callers that asked for nothing, which is where a core with more
// than one kind of mountable file ends up reading somebody else's list.
//
// It takes ioctl_index as an argument because that is a static local of
// HandleUI, not a file-scope one.
static int recent_idx_active(int ioctl_index)
{
	if (fs_RecentIdx >= 0) return fs_RecentIdx;
	return (fs_Options & SCANO_CORES)  ? -1
	     : (fs_Options & SCANO_UMOUNT) ? ioctl_index + 500
	                                   : ioctl_index;
}

// Open a recent list straight from a menu row, without going through the file
// browser. SelectFile is what normally sets fs_RecentIdx, so a row that skips
// it left the previous browse's index in place -- and "Clear the List" then
// emptied whatever that was.
static int recent_open(int idx)
{
	fs_RecentIdx = idx;
	return recent_init(idx);
}

// Leaving the shared Audio & Video page.
//
// It has always returned to MENU_MINIMIG_MAIN1 row 9, which is the row the
// full OSD links to it from. The console variant reaches it from its own
// Settings page instead, and MENU_MINIMIG_MAIN1 redirects to
// MENU_AMIGACD_MAIN1 whose row 10 is Save settings -- so the page would have
// dropped the user on a different page, on an unrelated row, one keypress away
// from overwriting their config. Each variant names its own way back.
//
// A function because the page has two exits, SELECT on Back and the back/left
// keys, and an exit that agrees with the other only by copy is one edit away
// from not agreeing.
// Leaving the shared Load config page. Same problem, same shape as
// amigacd_video_back(): it has always returned to MENU_MINIMIG_MAIN1 row 12,
// which on the console variant redirects to MENU_AMIGACD_MAIN1 where row 12 is
// exit. Landing on exit after backing out of a config list is not what
// backing out means.

// Step df<fu>'s external-drive assignment. 0 means "use an image", 1..4 name a
// drive on the MiSTer Floppy board.
//
// A helper because there are two ways in now: LEFT/RIGHT on the main page's
// floppy rows, which is the shortcut, and the explicit rows on the Drives page,
// which is where it is findable. Two copies of a walk with this many skip
// conditions is two copies that drift.
static void amigacd_cycle_extdrive(int fu, int forward)
{
	// driveMask is which external drives physically exist. An IBM/PC cable only
	// carries two, so skip 3 and 4 rather than offer drives that cannot answer.
	uint16_t mask16 = spi_uio_cmd16(UIO_GET_OSDMASK, 0);
	uint8_t  driveMask = 0x0F;
	if ((mask16 & 64) && (mask16 & 128)) driveMask &= 0x03;

	if (df[fu].status & DSK_INSERTED)   // an image was mounted; eject it
	{
		FileClose(&df[fu].file);
		if (df[fu].fluxFile) df[fu].fluxFile->closeFile();
	}

	// Skip anything absent or already claimed by the other unit; fall back to
	// image after a full lap.
	bool inUse = false;
	int  loops = 0;
	do {
		int e = minimig_config.floppy.extDrives[fu];
		if (forward) e = (e + 1) % 5;
		else         e = e ? (e - 1) : 4;
		minimig_config.floppy.extDrives[fu] = (unsigned char)e;

		inUse = false;
		if (e)
		{
			if (!(driveMask & (1 << (e - 1)))) inUse = true;
			else for (int d = 0; d < 4; d++)
				if (d != fu && minimig_config.floppy.extDrives[d] == e) inUse = true;
		}
		loops++;
	} while (inUse && loops <= 5);
	if (inUse) minimig_config.floppy.extDrives[fu] = 0;

	df[fu].status = 0;
	minimig_ConfigFloppyExt(minimig_config.floppy.extDrives[0], minimig_config.floppy.extDrives[1],
	                        minimig_config.floppy.extDrives[2], minimig_config.floppy.extDrives[3]);
}

// How an external-drive assignment reads. Shared by the main page's floppy rows
// and the Drives page so the two cannot describe the same setting differently.
//
// Letters vs digits track the detected drive type, matching Rob's rendering --
// A/B for IBM/PC, 0/1 for Shugart, and 0/A when the board has not been detected
// and we do not yet know which.
static void amigacd_extdrive_name(int ext, uint16_t osdMask, char *out, size_t outsz)
{
	if (ext < 1 || ext > 4) { snprintf(out, outsz, "off"); return; }

	// 1/2 are the pair a Shugart or IBM cable exposes and get the detected-type
	// label; 3/4 only exist on a 4-drive setup.
	if (ext > 2) { snprintf(out, outsz, "External Drive %s", (ext == 3) ? "2" : "3"); return; }

	const char *n = (osdMask & 64) ? ((osdMask & 128) ? ((ext == 1) ? "A" : "B")
	                                                  : ((ext == 1) ? "0" : "1"))
	                               : ((ext == 1) ? "0/A" : "1/B");
	snprintf(out, outsz, "External Drive %s", n);
}

// Rows a menumask selects. Every mask below is a contiguous run from bit 0,
// so this is simply the row count; written as a popcount because that stays
// correct if a page ever grows a gap.
static constexpr unsigned rowhint_rows(uint32_t mask)
{
	unsigned rows = 0;
	while (mask) { rows += (mask & 1u); mask >>= 1; }
	return rows;
}

// Each of these is stated twice: once as the page's menumask, once as the
// length its tips table has to have. Naming them lets a static_assert compare
// the two. A tips table is indexed by menusub, so it has to have exactly one
// entry per selectable row -- and menumask is where "how many rows" is
// actually stated. Without the assert, adding a row and forgetting its hint
// shows the row after it the wrong text, or nothing, and neither says
// anything is wrong.
#define AMIGACD_MAIN_MENUMASK     0x1FFFu
#define AMIGACD_SETTINGS_MENUMASK 0xFFFu
#define AMIGACD_CONFIGS_MENUMASK  0x0Fu
#define AMIGACD_ADV_MENUMASK_SNAC 0x7FFu
#define AMIGACD_ADV_MENUMASK      0x1FFu
#define AMIGACD_DRIVES_MENUMASK   0x0Fu

// The hint for the row the cursor is on, in the scrolling line at the bottom.
//
// The OSD has always had one helptext per PAGE, shown after ten seconds of no
// input. That is the right shape for "I am stuck": it waits until you have
// stopped. It is the wrong shape for "what does this row do?", which is asked
// while the cursor is moving -- and every keypress resets that timer, so a
// page-level message is never seen mid-navigation at all.
//
// So the text is composed per row here and shown on the short HELPTEXT_ROWHINT
// timer instead. Park on a row for a moment and it tells you what the row does
// and, where there is one, the key that does it without the menu.
//
// The core name and build date lead every hint rather than living on their own
// line, which is how they survived the page's helptext becoming per-row: they
// are the answer to "is this the build I just copied?", which is worth having
// on screen whichever row you are on.
//
//   tips  one string per selectable row, in menusub order
//   n     how many
//   sub   the current menusub
//
// A row with no entry -- or an index past the end, which is what a page whose
// rows are conditional can produce -- falls back to the name alone. Silent,
// because a missing hint is a gap in the writing and not a fault the user can
// do anything about.
// Returns the helptext index to use, so the call site is one statement --
// helptext_idx is a static local of HandleUI and cannot be set from here, and
// composing the text without selecting it would be a thing to forget.
static int amigacd_rowhint(const char *const *tips, unsigned n, uint32_t sub)
{
	snprintf(helptext_custom, sizeof(helptext_custom), "%s%s", HELPTEXT_SPACER, OsdCoreNameGet());

	if (tips && sub < n && tips[sub] && tips[sub][0])
	{
		strncat(helptext_custom, "  -  ", sizeof(helptext_custom) - strlen(helptext_custom) - 1);
		strncat(helptext_custom, tips[sub], sizeof(helptext_custom) - strlen(helptext_custom) - 1);
	}

	return HELPTEXT_ROWHINT;
}

// Write a .mgl that launches this core with the disc that is mounted now.
//
// docs/mgl.md has always described writing one by hand, and warned about the
// part that bites: <rbf> names a dated core, so every file naming it stops
// working at the next deploy. The OSD does not have to guess -- get_rbf_dir()
// and get_rbf_name() are the core that is RUNNING -- so a file written here is
// correct by construction, and correct again after the next deploy if it is
// written again.
//
// Returns one of MGL_WROTE / MGL_UPDATED / MGL_CURRENT, or 0 on a refusal, and
// leaves the file's root-relative path in out[].
enum { MGL_FAILED = 0, MGL_WROTE, MGL_UPDATED, MGL_CURRENT };
// The image a .mgl is written beside and named after: the CD when there is
// one, else the first mounted floppy -- an A500 or A1200 game is floppies
// only, and refusing those left them the one machine with no .mgl at all.
// Null when there is neither, or when the CD is the physical drive: a .mgl
// cannot name a disc that is not a file, and its settings block would carry
// the sentinel into a launch that has no drive.
static const char *amigacd_mgl_anchor()
{
	const char *disc = minimig_config.hardfile[0].filename;
	if (disc[0]) return strcmp(disc, PHYSICAL_DISC_SENTINEL) ? disc : 0;
	for (int i = 0; i < 4; i++)
		if ((df[i].status & DSK_INSERTED) && df[i].name[0]) return df[i].name;
	return 0;
}

static int amigacd_write_mgl(char *out, size_t outsz)
{
	const char *disc = amigacd_mgl_anchor();
	if (!disc) return 0;

	const char *rbf = get_rbf_name();
	if (!rbf[0]) return 0;

	// <rbf> is a path under the storage root with no extension.
	char stem[512];
	snprintf(stem, sizeof(stem), "%s", rbf);
	char *dot = strrchr(stem, '.');
	if (dot) *dot = 0;

	char rbfdir[512];
	snprintf(rbfdir, sizeof(rbfdir), "%s", get_rbf_dir());

	// The disc path, written in the form mgl_selpath() resolves back to this
	// same file: relative to the core's home directory when it lives under it,
	// and absolute when it does not. Not the root-relative form it is stored
	// in -- that one would be read back as HomeDir()/<it> and miss.
	char home[512];
	snprintf(home, sizeof(home), "%s", HomeDir());
	const size_t hlen = strlen(home);

	char discpath[1024];
	if (hlen && !strncmp(disc, home, hlen) && disc[hlen] == '/')
		snprintf(discpath, sizeof(discpath), "%s", disc + hlen + 1);
	else
		snprintf(discpath, sizeof(discpath), "%s", getFullPath(disc));

	// Beside the disc and named after it, which is the one place someone will
	// think to look for it. Moving it into _Console/ afterwards puts it on the
	// main menu's front page; nothing here depends on where it ends up.
	snprintf(out, outsz, "%s", disc);
	// An image inside a .zip gets its .mgl beside the zip: nothing can be
	// written into one.
	char *zip = strcasestr(out, ".zip/");
	if (zip) zip[4] = 0;
	char *ext = strrchr(out, '.');
	if (ext && !strchr(ext, '/')) *ext = 0;
	strncat(out, ".mgl", outsz - strlen(out) - 1);

	char rbfval[1024];
	snprintf(rbfval, sizeof(rbfval), "%s%s%s", rbfdir, rbfdir[0] ? "/" : "", stem);

	// getFullPath returns a pointer into a shared static, so it is copied
	// before anything else can call it.
	char full[1024];
	snprintf(full, sizeof(full), "%s", getFullPath(out));

	// Every setting, the disc among them, goes into the file's
	// <amigacd_config> block (support/minimig/mgl_cfg.h). Launching the file
	// then gives this machine exactly, whatever the startup config says --
	// and with the disc in the drive at power-on, as on a real CD32. That
	// matters beyond convenience: the late mount a <file index="4"> makes is
	// the route on which Banshee and Super Stardust run out of chip RAM, on a
	// real CD32 as well as here. So no CD <file> line is written, and one
	// already in the file is removed, or it would remount the disc late.
	static char block[48 * 1024];
	if (minimig_mgl_config_block(block, sizeof(block), "\t") < 0) return MGL_FAILED;
	(void)discpath;

	// An existing file is EDITED, not replaced.
	//
	// Replacing it is what this used to do, and it threw away whatever the user
	// had added -- a <reset/>, a second <file> for a save volume, a comment --
	// silently, on a keypress whose whole purpose is to fix a path after a
	// deploy. Which is exactly when someone would press it on a file they had
	// tuned by hand. Only <rbf>, the settings block and the CD line change.
	FILE *ex = fopen(full, "r");
	if (ex)
	{
		static char xml[64 * 1024];
		size_t n = fread(xml, 1, sizeof(xml) - 1, ex);
		const int truncated = !feof(ex);
		fclose(ex);
		xml[n] = 0;

		// A file too big for the buffer is left alone rather than written back
		// short.
		if (truncated) return MGL_FAILED;

		// No <rbf> element means the file is not one of ours, and editing a
		// file we do not understand is worse than declining.
		if (!strstr(xml, "<rbf>")) return MGL_FAILED;

		int changed = mgl_rewrite_rbf(xml, sizeof(xml), rbfval) > 0;
		changed |= mgl_remove_file_index(xml, 4) > 0;
		// Floppy lines go the way of the CD line: the settings block now
		// carries df0/df1 and mounts them at power-on, and a <file> line --
		// which this row used to write -- would remount one two seconds into
		// the boot, or after a swap bring the old disk back.
		changed |= mgl_remove_file_index(xml, 0) > 0;
		changed |= mgl_remove_file_index(xml, 1) > 0;
		const int put = mglcfg_put_block(xml, sizeof(xml), block);
		if (put < 0) return MGL_FAILED;
		changed |= put;
		if (!changed) return MGL_CURRENT;

		FILE *w = fopen(full, "w");
		if (!w) return MGL_FAILED;
		const size_t len = strlen(xml);
		int ok = (fwrite(xml, 1, len, w) == len) && !ferror(w);
		if (fclose(w)) ok = 0;
		return ok ? MGL_UPDATED : MGL_FAILED;
	}

	FILE *f = fopen(full, "w");
	if (!f) return MGL_FAILED;

	fprintf(f, "<mistergamedescription>\n");
	fprintf(f, "\t<rbf>%s</rbf>\n", rbfval);
	// No floppy <file> lines: df0 and df1 are in the settings block, with
	// the hard disks and the CD, and mounted at power-on.
	fputs(block, f);
	fprintf(f, "</mistergamedescription>\n");

	int ok = !ferror(f);
	if (fclose(f)) ok = 0;
	return ok ? MGL_WROTE : MGL_FAILED;
}

// Is an optical drive attached at all?
//
// physical_disc_open() answers this properly, but by opening the device --
// worker thread, ring buffer, speed cap -- which is not something a menu draw
// can do on every refresh. The device nodes are the cheap half of the same
// question, and they are the half that separates "no drive" from "a drive with
// an empty tray". Those two produced the same row text and the same toast, and
// they are different problems: one is a cable, the other is a disc.
//
// /dev/sr0../dev/sr7 is the range physical_disc.cpp's find_drive() scans, so
// the two agree on what counts as a drive. An already-open device is checked
// first, since a preferred device set from elsewhere need not be in that range.
static int amigacd_optical_drive_present()
{
	const char *open_dev = physical_disc_device();
	if (open_dev && open_dev[0]) return 1;

	for (int i = 0; i < 8; i++)
	{
		char dev[16];
		struct stat st;
		snprintf(dev, sizeof(dev), "/dev/sr%d", i);
		if (!stat(dev, &st)) return 1;
	}
	return 0;
}

static void amigacd_loadcfg_back()
{
	if (is_amigacd())
	{
		menustate = MENU_AMIGACD_CONFIGS1;
		menusub = 0;                       // the Load a config row
	}
	else
	{
		menustate = MENU_MINIMIG_MAIN1;
		menusub = 12;                      // the full OSD's link to this page
	}
}

// And leaving the shared Save config page, which had no route from this OSD at
// all until now. Same shape as the two above: its own row on the Configs page
// rather than MENU_MINIMIG_MAIN1 row 13, which on this variant redirects to
// MENU_AMIGACD_MAIN1 where row 13 is exit.
static void amigacd_savecfg_back()
{
	if (is_amigacd())
	{
		menustate = MENU_AMIGACD_CONFIGS1;
		menusub = 1;                       // the Save to a config slot row
	}
	else
	{
		menustate = MENU_MINIMIG_MAIN1;
		menusub = 13;                      // the full OSD's link to this page
	}
}

static void amigacd_video_back()
{
	if (is_amigacd())
	{
		menustate = MENU_AMIGACD_SETTINGS1;
		menusub = 10;                      // the Video & Audio row
	}
	else
	{
		menustate = MENU_MINIMIG_MAIN1;
		menusub = 9;                       // the full OSD's link to this page
	}
}

// Put the current MGL item's file into selPath, the way MENU_MINIMIG_* has
// always resolved it: an absolute path is used as given, "../" resolves from
// the storage root, anything else from the core's home directory.
//
// A helper rather than a fourth copy, because the AmigaCD tree has three mount
// handlers that need it and a resolution rule that differs between them by a
// line is a .mgl that works for a floppy and not for a disc.
static void mgl_selpath(char *selPath, size_t selPathSize)
{
	const char *p = mgl_get()->item[mgl_get()->current].path;

	if (p[0] == '/') snprintf(selPath, selPathSize, "%s", p);
	else if (p[0] == '.' && p[1] == '.') snprintf(selPath, selPathSize, "%s/%s", getRootDir(), p);
	else
	{
		snprintf(selPath, selPathSize, "%s/%s", HomeDir(), p);

		// Two other ways of writing a relative path are in use, and both used
		// to resolve to <home>/<path>, miss, and mount nothing without a word
		// -- the drive just read "* no disk *", which looks like the .mgl
		// having been ignored:
		//
		//   * from the SD root ("games/Amiga/x.adf"), which is what a script
		//     writes and what scripts/vamigats.sh wrote; and
		//   * from games/Amiga, Minimig's home, which is how every .mgl written
		//     for Minimig names its ADF -- and MENU_AMIGACD_MAIN1 keeps indices
		//     0..3 as floppies precisely so those files work here.
		//
		// The home directory still wins when the file is there.
		if (!FileExists(selPath, 0))
		{
			static const char *const prefix[] = { "", "games/Amiga/" };
			char alt[1024];
			for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
			{
				snprintf(alt, sizeof(alt), "%s%s", prefix[i], p);
				if (FileExists(alt, 0))
				{
					snprintf(selPath, selPathSize, "%s", alt);
					break;
				}
			}
		}
	}

	if (!FileExists(selPath, 0))
	{
		printf("MGL: %s not found\n", selPath);
		Info("MGL: file not found", 4000);
	}

	// Same /tmp/ breadcrumbs the other MGL paths leave, so a script watching
	// them sees an AmigaCD launch the way it sees every other core's.
	if (cfg.log_file_entry)
	{
		const char *fname = strrchr(selPath, '/');
		MakeFile("/tmp/FULLPATH", selPath);
		MakeFile("/tmp/CURRENTPATH", fname ? fname + 1 : selPath);
		MakeFile("/tmp/FILESELECT", "selected");
	}
}

static advancedButtonMap abm_edit_map = {};
static advancedButtonMap *abm_edit_ptr = NULL;
static int abm_dev_num = 0;

static char* GetExt(char *ext)
{
	static char extlist[32];
	char *p = extlist;

	while (*ext) {
		strcpy(p, ",");
		strncat(p, ext, 3);
		while (*(p + strlen(p) - 1) == ' ') *(p + strlen(p) - 1) = 0;
		if (strlen(ext) <= 3) break;
		ext += 3;
		p += strlen(p);
	}

	return extlist + 1;
}

static char SelectedDir[1024] = {};
static char SelectedLabel[1024] = {};

static char Selected_F[16][1024] = {};
static char Selected_S[16][1024] = {};
static char Selected_tmp[1024] = {};
static char Selected_CD32[1024] = {};
static char Selected_CDTV[1024] = {};

void StoreIdx_F(int idx, const char *path)
{
	strcpy(Selected_F[idx], path);
}

void StoreIdx_S(int idx, const char *path)
{
	strcpy(Selected_S[idx], path);
}

static char selPath[1024] = {};

static void ResolveExistingCorePath(char *path)
{
	while (path[0] && !FileExists(path) && !PathIsDir(path))
	{
		char *p = strrchr(path, '/');
		if (!p)
		{
			path[0] = 0;
			break;
		}
		*p = 0;
	}
}

static int changeDir(char *dir)
{
	char curdir[128];
	memset(curdir, 0, sizeof(curdir));
	if(!dir || !strcmp(dir, ".."))
	{
		if (!strlen(selPath))
		{
			return 0;
		}

		char *p = strrchr(selPath, '/');
		if (p)
		{
			*p = 0;
			strncpy(curdir, p + 1, sizeof(curdir) - 1);
		}
		else
		{
			strncpy(curdir, selPath, sizeof(curdir) - 1);
			selPath[0] = 0;
		}
	}
	else
	{
		if (strlen(selPath) + strlen(dir) > sizeof(selPath) - 100)
		{
			return 0;
		}

		if (strlen(selPath)) strcat(selPath, "/");
		strcat(selPath, dir);
	}

	ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, fs_Options);
	if(curdir[0])
	{
		ScanDirectory(selPath, SCANF_SET_ITEM, curdir, fs_Options);
	}
	return 1;
}

static const char *home_dir = NULL;
static char filter[256] = {};
static unsigned long filter_typing_timer = 0;

// this function displays file selection menu
void SelectFile(const char* path, const char* pFileExt, int Options, unsigned char MenuSelect, unsigned char MenuCancel, int recent_idx)
{
	static char tmp[1024];
	printf("pFileExt = %s\n", pFileExt);
	filter_typing_timer = 0;
	filter[0] = 0;

	strncpy(selPath, path, sizeof(selPath) - 1);
	selPath[sizeof(selPath) - 1] = 0;

	if (Options & SCANO_CORES)
	{
		strcpy(selPath, get_rbf_dir());
		if (strlen(get_rbf_name()))
		{
			if(strlen(selPath)) strcat(selPath, "/");
			strcat(selPath, get_rbf_name());
		}
		ResolveExistingCorePath(selPath);
		pFileExt = "RBFMRAMGL";
		home_dir = NULL;
	}
	else if (Options & SCANO_TXT)
	{
		if(pFileExt == 0) pFileExt = "TXT";
		home_dir = NULL;
	}
	else
	{
		const char *home = is_menu() ? "Scripts" : user_io_get_core_path(
			(is_pce() && !strncasecmp(pFileExt, "CUE", 3)) ? PCECD_DIR :
			(is_neogeo() && !strncasecmp(pFileExt, "CUE", 3)) ? NEOCD_DIR :
			NULL, 1);
		home_dir = strrchr(home, '/');
		if (home_dir) home_dir++;
		else home_dir = home;

		if (Options & SCANO_SAVES)
		{
			snprintf(tmp, sizeof(tmp), "%s/%s", SAVE_DIR, CoreName2);
			home = tmp;
		}

		if (strncasecmp(home, selPath, strlen(home)) || !strcasecmp(home, selPath) || (!FileExists(selPath) && !PathIsDir(selPath)))
		{
			Options &= ~SCANO_NOENTER;
			strcpy(selPath, home);
		}
	}

	ScanDirectory(selPath, SCANF_INIT, pFileExt, Options);
	AdjustDirectory(selPath);

	strcpy(fs_pFileExt, pFileExt);
	fs_ExtLen = strlen(fs_pFileExt);
	fs_Options = Options & ~SCANO_NOENTER;
	fs_MenuSelect = MenuSelect;
	fs_MenuCancel = MenuCancel;
	fs_RecentIdx = recent_idx;

	menustate = MENU_FILE_SELECT1;
}

#define STD_EXIT       "            exit"
#define STD_BACK       "            back"
#define STD_SPACE_EXIT "        SPACE to exit"
#define STD_COMBO_EXIT "      Ctrl+ESC to exit"

// conversion table of Amiga keyboard scan codes to ASCII codes
static const uint8_t keycode_table[128] =
{
	0,'1','2','3','4','5','6','7','8','9','0',  0,  0,  0,  0,  0,
	'Q','W','E','R','T','Y','U','I','O','P',  0,  0,  0,  0,  0,  0,
	'A','S','D','F','G','H','J','K','L',  0,  0,  0,  0,  0,  0,  0,
	0,'Z','X','C','V','B','N','M',  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static uint8_t GetASCIIKey(uint32_t keycode)
{
	if (keycode & UPSTROKE)
		return 0;

	return keycode_table[get_amiga_code(keycode & 0xFFFF) & 0x7F];
}

static bool ignore_osd_release = false;
static int select_ini = 0;

void SelectINI()
{
	select_ini = 1;
}

void build_advanced_map_code_str(uint16_t *abm_codes, size_t abm_size, char *code_str, size_t code_size, int center_size = 0)
{
	char str[128] = {};
	if(abm_codes[0])
	{
		strncat(str, "[", code_size);
		for (unsigned int i = 0; i < abm_size/sizeof(uint16_t); i++)
		{
			char cs[64] = {};
			if (!abm_codes[i]) break;
			if (abm_codes[i] < 256) //keyboard
			{
				sprintfz(cs, "k%X", abm_codes[i]);
			} else {
				get_button_name_for_code((abm_codes[i] & 0x7FFF), abm_dev_num, cs, sizeof(cs));
			}
			strncat(cs, ",", sizeof(cs)-1);
			strncat(str, cs, code_size);
		}
		int code_len = strlen(str);
		if (str[code_len-1] == ',') str[code_len-1] = 0;
		strcat(str, "]");
	}
	if (center_size)
	{
		snprintf(code_str, code_size, "%*s%*s", (int)(center_size+strlen(str)/2), str, (int)(center_size+strlen(str)/2), " ");
	} else {
		snprintf(code_str, code_size, "%s", str);
	}
}


/* the Atari core handles OSD keys competely inside the core */
static uint32_t menu_key = 0;

void menu_key_set(unsigned int c)
{
	//printf("OSD enqueue: %x\n", c);
	menu_key = c;
}

// get key status
static int hold_cnt = 0;
static uint32_t menu_key_get(void)
{
	static uint32_t prev_key = 0;
	static unsigned long db_time = 0;
	if (prev_key != menu_key || !db_time)
	{
		prev_key = menu_key;
		db_time = GetTimer(20);
	}

	uint32_t c = 0;
	if (CheckTimer(db_time))
	{
		static uint32_t c2;
		static unsigned long repeat;
		uint32_t c1;

		c1 = menu_key;
		c = 0;
		if (c1 != c2)
		{
			c = c1;
			hold_cnt = 1;
		}
		c2 = c1;

		// generate repeat "key-pressed" events
		if ((c1 & UPSTROKE) || (!c1))
		{
			hold_cnt = 0;
			repeat = GetTimer(REPEATDELAY);
		}
		else if (CheckTimer(repeat))
		{
			repeat = GetTimer(REPEATRATE);
			if (GetASCIIKey(c1) || menustate == MENU_FILE_SELECT2 || ((menustate == MENU_COMMON2) && (menusub == 17)) || ((menustate == MENU_SYSTEM2) && (menusub == 5)))
			{
				c = c1;
				hold_cnt++;
			}
		}
	}

	// currently no key pressed
	if (!c && !select_ini)
	{
		static unsigned long longpress = 0, longpress_consumed = 0;
		static unsigned char last_but = 0;
		unsigned char but = user_io_menu_button();

		if (but && !last_but) longpress = GetTimer(3000);
		if (but && CheckTimer(longpress) && !longpress_consumed)
		{
			longpress_consumed = 1;
			if (menustate == MENU_SCRIPTS1) c = KEY_BACKSPACE;
			else if(menustate < MENU_PROHIB_BTPAIR1 || menustate > MENU_PROHIB_BTPAIR2) menustate = MENU_BTPAIR;
		}

		if (!but && last_but && !longpress_consumed) c = KEY_F12 | UPSTROKE;

		if (!but) longpress_consumed = 0;
		last_but = but;
	}

	if (!c)
	{
		static unsigned long longpress = 0, longpress_consumed = 0;
		static unsigned char last_but = 0;
		unsigned char but = user_io_user_button();

		if (user_io_osd_is_visible())
		{
			if (but && !last_but) longpress = GetTimer(1500);
			if (but && CheckTimer(longpress) && !longpress_consumed)
			{
				longpress_consumed = 1;
				if (is_menu())
				{
					if (menustate == MENU_SYSTEM2 || menustate == MENU_FILE_SELECT2) menustate = MENU_JOYSYSMAP;
				}
				else if (get_map_vid() || get_map_pid())
				{
					menustate = MENU_JOYRESET;
				}
			}

			if (!but && last_but && !longpress_consumed)
			{
				if (select_ini)
				{
					c = KEY_ESC | UPSTROKE;
				}
				else if (get_map_vid() || get_map_pid())
				{
					send_map_cmd(KEY_ALTERASE);
				}
			}
		}

		if (!but) longpress_consumed = 0;
		last_but = but;
	}

	return(c);
}

static char* getNet(int spec)
{
	int netType = 0;
	struct ifaddrs *ifaddr, *ifa, *ifae = 0, *ifaw = 0;
	static char host[NI_MAXHOST];

	if (getifaddrs(&ifaddr) == -1)
	{
		printf("getifaddrs: error\n");
		return NULL;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_addr == NULL) continue;
		if (!memcmp(ifa->ifa_addr->sa_data, "\x00\x00\xa9\xfe", 4)) continue; // 169.254.x.x

		if ((strcmp(ifa->ifa_name, "eth0") == 0)     && (ifa->ifa_addr->sa_family == AF_INET)) ifae = ifa;
		if ((strncmp(ifa->ifa_name, "wlan", 4) == 0) && (ifa->ifa_addr->sa_family == AF_INET)) ifaw = ifa;
	}

	ifa = 0;
	netType = 0;
	if (ifae && (!spec || spec == 1))
	{
		ifa = ifae;
		netType = 1;
	}

	if (ifaw && (!spec || spec == 2))
	{
		ifa = ifaw;
		netType = 2;
	}

	if (spec && ifa)
	{
		strcpy(host, "IP: ");
		getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host + strlen(host), NI_MAXHOST - strlen(host), NULL, 0, NI_NUMERICHOST);
	}

	freeifaddrs(ifaddr);
	return spec ? (ifa ? host : 0) : (char*)netType;
}

static long sysinfo_timer;
static void infowrite(int pos, const char* txt)
{
	char str[40];
	memset(str, 0x20, 29);
	int len = strlen(txt);
	if (len > 27) len = 27;
	if(len) strncpy(str + 1+ ((27-len)/2), txt, len);
	str[0] = 0x83;
	str[28] = 0x83;
	str[29] = 0;
	OsdWrite(pos, str, 0, 0);
}

static void printSysInfo()
{
	if (!sysinfo_timer || CheckTimer(sysinfo_timer))
	{
		sysinfo_timer = GetTimer(2000);
		struct battery_data_t bat;
		int hasbat = getBattery(0, &bat);
		int n = 2;
		static int flip = 0;

		char str[40];
		OsdWrite(n++, info_top, 0, 0);

		int j = 0;
		char *net;
		net = getNet(1);
		if (net)
		{
			sprintf(str, "\x1c %s", net);
			infowrite(n++, str);
			j++;
		}
		net = getNet(2);
		if (net)
		{
			sprintf(str, "\x1d %s", net);
			infowrite(n++, str);
			j++;
		}
		if (!j) infowrite(n++, "No network");
		if (j<2) infowrite(n++, "");

		flip = (flip + 1) & 3;
		if (hasbat && (flip & 2))
		{
			infowrite(n++, "");

			sprintf(str, "\x1F ");
			if (bat.capacity == -1) strcat(str, "n/a");
			else sprintf(str + strlen(str), "%d%%", bat.capacity);
			if (bat.current != -1) sprintf(str + strlen(str), " %dmAh", bat.current);
			if (bat.voltage != -1) sprintf(str + strlen(str), " %d.%dV", bat.voltage / 1000, (bat.voltage / 100) % 10);

			infowrite(n++, str);

			str[0] = 0;
			if (bat.load_current > 0)
			{
				sprintf(str + strlen(str), " \x12 %dmA", bat.load_current);
				if (bat.time != -1)
				{
					if (bat.time < 90) sprintf(str + strlen(str), ", ETA: %dm", bat.time);
					else sprintf(str + strlen(str), ", ETA: %dh%02dm", bat.time / 60, bat.time % 60);
				}
			}
			else if (bat.load_current < -1)
			{
				sprintf(str + strlen(str), " \x13 %dmA", -bat.load_current);
				if (bat.time != -1)
				{
					if (bat.time < 90) sprintf(str + strlen(str), ", ETA: %dm", bat.time);
					else sprintf(str + strlen(str), ", ETA: %dh%02dm", bat.time / 60, bat.time % 60);
				}
			}
			else
			{
				strcat(str, "Not charging");
			}
			infowrite(n++, str);
		}
		else
		{
			video_mode_adjust(true);
			infowrite(n++, "");
			video_core_description(str, 40);
			infowrite(n++, str);
			video_scaler_description(str, 40);
			infowrite(n++, str);
		}
		OsdWrite(n++, info_bottom, 0, 0);
	}
}

static int  firstmenu = 0;
static int  adjvisible;

static void MenuWrite(unsigned char n, const char *s = "", unsigned char invert = 0, unsigned char stipple = 0, int arrow = 0)
{
	int row = n - firstmenu;

	if (row < 0)
	{
		if (invert) adjvisible = row;
		return;
	}

	if (row >= OsdGetSize())
	{
		if (invert) adjvisible = row - OsdGetSize() + 1;
		return;
	}

	OsdSetArrow(arrow);
	OsdWriteOffset(row, s, invert, stipple, 0, (row == 0 && firstmenu) ? 17 : (row == (OsdGetSize()-1) && !arrow) ? 16 : 0, 0);
}

const char* get_rbf_name_bootcore(char *str)
{
	if (!strlen(cfg.bootcore)) return "";
	char *p = strrchr(str, '/');
	if (!p) return str;

	char *spl = strrchr(p + 1, '.');
	if (spl && (!strcmp(spl, ".rbf") || !strcmp(spl, ".mra") || !strcmp(spl, ".mgl")))
	{
		*spl = 0;
	}
	else
	{
		return NULL;
	}
	return p + 1;
}


static void vga_nag()
{
	if (video_fb_state() && !(cfg.fb_terminal == 2 && !cfg.vga_scaler && !cfg.direct_video))
	{
		EnableOsd_on(OSD_VGA);
		OsdSetSize(16);
		OsdSetTitle("Information");
		int n = 0;
		OsdWrite(n++);
		OsdWrite(n++);
		OsdWrite(n++, "  If you see this, then you");
		OsdWrite(n++, "  need to modify MiSTer.ini");
		OsdWrite(n++);
		OsdWrite(n++, " Either disable framebuffer:");
		OsdWrite(n++, "       fb_terminal=0");
		OsdWrite(n++);
		OsdWrite(n++, "   or size it for CRT/VGA:");
		OsdWrite(n++, "       fb_terminal=2");
		OsdWrite(n++);
		OsdWrite(n++, "   or enable scaler on VGA:");
		OsdWrite(n++, "       vga_scaler=1");
		for (; n < OsdGetSize(); n++) OsdWrite(n);
		OsdUpdate();
		OsdEnable(OSD_MSG);
		EnableOsd_on(OSD_HDMI);
	}

	OsdDisable();
	EnableOsd_on(OSD_ALL);
}

void process_addon(char *ext, uint8_t idx)
{
	static char name[1024];

	while (*ext && *ext != ',') ext++;
	if (*ext) ext++;
	if (!*ext) return;

	printf("addons: %s\n", ext);

	int i = 0;
	while (1)
	{
		char *fname = name;
		strcpy(name, selPath);
		char *p = strrchr(name, '.');
		if (!p) p = name + strlen(name);
		*p++ = '.';

		substrcpy(p, ext, i);
		if (!strlen(p)) return;
		if (*p == '!')
		{
			*p = 0;
			char *bs = strrchr(name, '/');
			if (!bs)
			{
				fname = p + 1;
			}
			else
			{
				strcpy(bs + 1, p + 1);
			}
		}

		printf("Trying: %s\n", fname);
		user_io_file_tx_a(fname, ((i+1) << 8) | idx);
		i++;
	}
}

static int get_arc(const char *str)
{
	int arc = 0;
	if (!strcmp(str, "[ARC1]")) arc = 1;
	else if(!strcmp(str, "[ARC2]")) arc = 2;
	else return 0;

	uint32_t x = 0, y = 0;
	if (sscanf(cfg.custom_aspect_ratio[arc - 1], "%u:%u", &x, &y) != 2 || x < 1 || x > 4095 || y < 1 || y > 4095) arc = -1;

	return arc;
}

static int get_ar_name(int ar, char *str)
{
	switch (ar)
	{
	case 0:
		strcat(str, "Original");
		break;

	case 1:
		strcat(str, "Full Screen");
		break;

	case 2:
		if (get_arc("[ARC1]") <= 0)
		{
			strcat(str, "Original");
			ar = 0;
		}
		else
		{
			strcat(str, cfg.custom_aspect_ratio[0]);
		}
		break;

	case 3:
		if (get_arc("[ARC2]") <= 0)
		{
			strcat(str, "Original");
			ar = 0;
		}
		else
		{
			strcat(str, cfg.custom_aspect_ratio[1]);
		}
		break;
	}

	return ar;
}

static int next_ar(int ar, int minus)
{
	if (minus)
	{
		ar = (ar - 1) & 3;
		while (1)
		{
			if (ar == 3 && get_arc("[ARC2]") > 0 && get_arc("[ARC1]") > 0) break;
			if (ar == 2 && get_arc("[ARC1]") > 0) break;
			if (ar < 2) break;
			ar--;
		}
	}
	else
	{
		ar = (ar + 1) & 3;
		if (ar == 3 && get_arc("[ARC2]") <= 0) ar = 0;
		if (ar == 2 && get_arc("[ARC1]") <= 0) ar = 0;
	}

	return ar;
}

static int joymap_first = 0;

static int gun_x = 0;
static int gun_y = 0;
static int gun_ok = 0;
static int gun_side = 0;
static int gun_idx = 0;
static int32_t gun_pos[4] = {};
static int page = 0;

static void menu_button_name(int button, char *buf, size_t bsize)
{
	switch(button)
	{
	        case 0:
	                strncpy(buf, "Right", bsize);
	                break;
	        case 1:
	                strncpy(buf, "Left", bsize);
	                break;
	        case 2:
	                strncpy(buf, "Down", bsize);
	                break;
	        case 3:
	                strncpy(buf, "Up", bsize);
	                break;
					case -1:
									strncpy(buf, "(None)", bsize);
									break;
	        default:
	                if ((button-4 < joy_bcount) && joy_bnames[button-4][0])
	                {
	                        strncpy(buf, joy_bnames[button-4], bsize);
	                } else {
	                        snprintf(buf, bsize, "%d", button-4);
	                }
	}
}


static void menu_parse_buttons()
{
	if (is_minimig())
	{
		joy_bcount = 7;
		strcpy(joy_bnames[0], "A(Red/Fire)");
		strcpy(joy_bnames[1], "B(Blue)");
		strcpy(joy_bnames[2], "C(Yellow)");
		strcpy(joy_bnames[3], "D(Green)");
		strcpy(joy_bnames[4], "RT");
		strcpy(joy_bnames[5], "LT");
		strcpy(joy_bnames[6], "Pause");
	}
	else
	{
		parse_buttons();
  }
}


void build_advanced_map_core_btn_str(advancedButtonMap *abm, char *dest_str, size_t dest_size)
{
	int first_btn = -1;
	int btn_count = 0;
	for(unsigned int i = 0; i < sizeof(abm->button_mask)*8; i++)
	{
		if (abm->button_mask & 1<<i)
		{
			btn_count++;
			if (first_btn == -1) first_btn = i;
		}
	}
	if (btn_count > 1) snprintf(dest_str, dest_size, "(%d)", btn_count);
	else menu_button_name(first_btn, dest_str, dest_size);
}


void build_advanced_map_summary(advancedButtonMap *abm, char *dest_str, size_t dest_size)
{
	char input_str[128] = {};
	char output_str[128] = {};
	build_advanced_map_code_str(abm->input_codes, sizeof(abm->input_codes), input_str, sizeof(input_str));
	if (abm->output_codes[0])
	{
		build_advanced_map_code_str(abm->output_codes, sizeof(abm->output_codes), output_str, sizeof(output_str));
	} else {
		build_advanced_map_core_btn_str(abm, output_str, sizeof(output_str));
	}
	snprintf(dest_str, dest_size, "%s->%s", input_str, output_str);
}

static void *close_pipe_async(void *arg)
{
	pclose((FILE *)arg);
	return NULL;
}

void HandleUI(void)
{
	PROFILE_FUNCTION();

	// Does the page that just painted agree with its own menumask?
	//
	// docs/state-todo.md U1. A page declares how many rows the cursor can land
	// on by setting menumask, and then draws the cursor by passing `menusub == k`
	// as the invert flag at each row, with k written out as a literal. Nothing
	// tied the two together. When they disagreed the symptom was invisible:
	// MENU_MINIMIG_CHIPSET1 had two rows at k == 11 for as long as FDD Turbo
	// existed, so both drew highlighted and the one below could never be
	// toggled, and the only statement of that page's row count was a prose
	// comment -- which was wrong.
	//
	// A static_assert cannot see this. The literals are spread over sixty lines
	// of drawing code and only the invert FLAG reaches OsdWrite(), not the k it
	// came from, so there is nothing for the compiler to count. What is checkable
	// is the consequence: with the cursor on a selectable row, exactly one row
	// comes out inverted. Two rows sharing a k give two; a bit set in menumask
	// that no row draws gives none.
	//
	// Checked here, at the top of the pass AFTER the one that painted, because a
	// page paints in its ...1 state and then hands off to its ...2 state, and
	// this is the first point where the totals are complete. menumask and menusub
	// still hold what the painting pass set; nothing between the two touches
	// them.
	//
	// Console-only and once per state, same as osd_check_line() below it in
	// osd.cpp: this is a claim about our own code being self-consistent, not
	// something to put in front of a player mid-game.
	{
		static uint8_t  row_warned[MENU_STATE_COUNT] = {};
		static uint32_t tally_state = MENU_NONE1;

		const int writes = OsdRowTallyWrites();
		const int sel = OsdRowTallySelected();

		// The verdict itself is in osd_rowguard.cpp, which tests/ can link and
		// tests/test_osd_rowguard.cpp falsifies. What stays here is the part a
		// test cannot reach: the tally, and warning once per state.
		if (osd_rowguard_warn(writes, sel, menusub, menumask) &&
		    tally_state < MENU_STATE_COUNT && !row_warned[tally_state])
		{
			row_warned[tally_state] = 1;
			printf("OSD: menu state %u painted %d rows, %d of them selected, with "
			       "menusub %u set in menumask 0x%llx -- expected exactly one. "
			       "Two rows sharing a menusub, or a menumask bit no row draws.\n",
			       (unsigned)tally_state, writes, sel, (unsigned)menusub,
			       (unsigned long long)menumask);
		}

		tally_state = menustate;
		OsdRowTallyReset();
	}

	// A pending System change belongs to the AmigaCD main page. Any other state
	// means the row is gone -- moved off, paged away, OSD closed by something
	// other than the menu key -- and the choice the row was showing has to be
	// honoured rather than dropped.
	if (menustate != MENU_AMIGACD_MAIN1 && menustate != MENU_AMIGACD_MAIN2)
	{
		amigacd_system_flush();
	}

	// The save state preview replaces the core's live video, so it must not be
	// able to outlive the two pages that own it. Their draws turn it on and off
	// as the cursor moves within a page; this is what turns it off when the
	// cursor leaves one -- including the ways that do not go through a row at
	// all, such as closing the OSD. Cheap: a no-op unless a preview is up.
	if (menustate != MENU_MINIMIG_CHIPSET1 && menustate != MENU_MINIMIG_CHIPSET2 &&
	    menustate != MENU_AMIGACD_SETTINGS1 && menustate != MENU_AMIGACD_SETTINGS2)
	{
		amigacd_ss_preview(0, 0);
	}

	if (bt_timer >= 0)
	{
		if (!bt_timer) bt_timer = (int32_t)GetTimer(6000);
		else if (CheckTimer((uint32_t)bt_timer))
		{
			bt_timer = -1;
			if (hci_get_route(0) < 0)
			{
				// Some BT dongles get stuck after boot.
				// Kicking of USB port usually make it work.
				printf("*** reset bt ***\n");
				system("/bin/bluetoothd hcireset &");
			}
		}
	}

	{
		static unsigned long bt_icon_timer = 0;
		if (!bt_pairing && (!bt_icon_timer || CheckTimer(bt_icon_timer)))
		{
			bt_present = (hci_get_route(0) >= 0);
			bt_icon_timer = GetTimer(3000);
			if (!bt_icon_timer) bt_icon_timer = 1;
		}
	}

	switch (user_io_core_type())
	{
	case CORE_TYPE_8BIT:
		break;

	default:
		// No UI in unknown cores.
		return;
	}

	static char opensave;
	static char ioctl_index;
	char *p;
	static char s[256];
	unsigned char m = 0, up, down, select, menu, back, right, left, plus, minus, recent;
	char enable;
	static int reboot_req = 0;
	static uint32_t helptext_timer;
	static int helptext_idx = 0;
	static int helptext_idx_old = 0;
	static char helpstate = 0;
	static char flag;
	static int cr = 0;
	static uint32_t cheatsub = 0;
	static uint8_t card_cid[32];
	static uint32_t hdmask = 0;
	static pid_t ttypid = 0;
	static int ttystatus = 0;
	static int has_fb_terminal = 0;
	static unsigned long flash_timer = 0;
	static int flash_state = 0;
	static uint32_t dip_submenu;
	static uint32_t manual_submenu;
	static int need_reset = 0;
	static int flat = 0;
	static int menusub_parent = 0;
	static char title[32] = {};
	static uint32_t saved_menustate = 0;
	static char addon[1024];
	static int store_name;
	static int vfilter_type;
	static int old_volume = 0;
	static uint32_t lock_pass_timeout = 0;
	static uint32_t menu_timeout = 0;

	static char	cp_MenuCancel;

	uint32_t c = 0;

	mgl_struct *mgl = mgl_get();

	/*
	static int old_state = -1;
	static int old_current = -1;
	static int old_done = -1;
	static uint32_t old_menustate = -1;

	if ((old_state != mgl->state) || (old_current != mgl->current) || (old_done != mgl->done) || (old_menustate != menustate))
	{
		printf("*** MGL menustate=%d, count=%d current=%d state=%d action=%d done=%d\n", menustate, mgl->count, mgl->current, mgl->state, mgl->item[mgl->current].action, mgl->done);
		old_state = mgl->state;
		old_current = mgl->current;
		old_done = mgl->done;
		old_menustate = menustate;
	}
	*/

	if (!mgl->done)
	{
		switch (mgl->state)
		{
		case 0:
			if (CheckTimer(mgl->timer))
			{
				mgl->state = (mgl->item[mgl->current].action == MGL_ACTION_LOAD) ? 1 : 4;
			}
			break;

		case 3:
			mgl->state = 0;
			mgl->current++;
			if (mgl->current < mgl->count) mgl->timer = GetTimer(mgl->item[mgl->current].delay * 1000);
			else mgl->done = 1;
			break;

		case 4:
			user_io_set_kbd_reset(1);
			user_io_send_buttons(1);
			mgl->timer = GetTimer(mgl->item[mgl->current].hold ? (mgl->item[mgl->current].hold * 1000) : 100);
			mgl->state = 5;
			break;

		case 5:
			if (CheckTimer(mgl->timer))
			{
				user_io_set_kbd_reset(0);
				user_io_send_buttons(1);
				mgl->state = 3;
			}
			break;
		}
	}
	else
	{
		// get user control codes
		c = menu_key_get();
	}

	// decode and set events
	menu = false;
	back = false;
	select = false;
	up = false;
	down = false;
	left = false;
	right = false;
	plus = false;
	minus = false;
	recent = false;

	if (c && cfg.bootcore[0] != '\0') cfg.bootcore[0] = '\0';

	if (!select_ini && is_menu() && cfg.osd_timeout >= 5)
	{
		static int menu_visible = 1;
		static unsigned long timeout = 0;
		static unsigned long off_timeout = 0;
		static uint32_t wake_release = 0;
		if (!video_fb_state() && cfg.fb_terminal)
		{
			if (c == wake_release)
			{
				wake_release = 0;
				c = 0;
			}

			if (timeout && CheckTimer(timeout))
			{
				timeout = 0;
				if (menu_visible > 0)
				{
					menu_visible = 0;
					video_menu_bg(user_io_status_get("[3:1]"), 1);
					OsdMenuCtl(0);
				}
				else if (!menu_visible)
				{
					menu_visible--;
					video_menu_bg(user_io_status_get("[3:1]"), 2);
					off_timeout = cfg.video_off ? GetTimer(cfg.video_off * 1000) : 0;
				}
			}

			if (off_timeout && CheckTimer(off_timeout) && menu_visible < 0)
			{
				off_timeout = 0;
				video_menu_bg(user_io_status_get("[3:1]"), cfg.video_off_logo ? 4 : 3);
				if (cfg.video_off_logo) off_timeout = GetTimer(10000);
			}

			if (c || menustate != MENU_FILE_SELECT2)
			{
				timeout = 0;
				if (menu_visible <= 0)
				{
					wake_release = c | UPSTROKE;
					c = 0;
					menu_visible = 1;
					video_menu_bg(user_io_status_get("[3:1]"));
					OsdMenuCtl(1);
					off_timeout = 0;
				}
			}

			if (!timeout)
			{
				timeout = GetTimer(cfg.osd_timeout * 1000);
			}
		}
		else
		{
			timeout = 0;
			menu_visible = 1;
		}
	}

	//prevent OSD control while script is executing on framebuffer
	if ((!video_fb_state() || video_chvt(0) != 2) && !select_ini)
	{
		switch (c)
		{
		case KEY_F12:
			if (user_io_osd_is_visible())
			{
				menu = true;
				ignore_osd_release = true;
			}
			break;
		case KEY_F12 | UPSTROKE:
			if (!ignore_osd_release)
				menu = true;
			ignore_osd_release = false;
			if(video_fb_state()) video_menu_bg(user_io_status_get("[3:1]"));
			video_fb_enable(0);
			break;
		case KEY_F1:
			if (is_menu() && cfg.fb_terminal)
			{
				user_io_status_set("[3:1]", user_io_status_get("[3:1]") + 1);
				user_io_status_save(user_io_create_config_name());
				video_menu_bg(user_io_status_get("[3:1]"));
			}
			break;

		case KEY_F11:
			if (user_io_osd_is_visible() && (menustate != MENU_SCRIPTS1 || script_finished))
			{
				menustate = MENU_BTPAIR;
			}
			break;

		case KEY_F10:
			if (input_has_lightgun())
			{
				menustate = MENU_LGCAL;
			}
			break;

		case KEY_F9:
			if ((is_menu() || ((get_key_mod() & (LALT | RALT)) && (get_key_mod() & (LCTRL | RCTRL))) || has_fb_terminal) && cfg.fb_terminal)
			{
				video_chvt(1);
				video_fb_enable(!video_fb_state());
				if (video_fb_state())
				{
					menustate = MENU_NONE1;
					has_fb_terminal = 1;
				}
			}
			break;

		case KEY_F7: // added: F7 activates joystick map if OSD is visible, or in menu core
			if (menustate != MENU_SCRIPTS1 || script_finished)
			{
				menustate = MENU_JOYDIGMAP;
			}
			break;

			// Within the menu the esc key acts as the menu key. problem:
			// if the menu is left with a press of ESC, then the follwing
			// break code for the ESC key when the key is released will
			// reach the core which never saw the make code. Simple solution:
			// react on break code instead of make code
		case KEY_ESC | UPSTROKE:
			if (menustate != MENU_NONE2) menu = true;
			break;
		case KEY_BACK | UPSTROKE:
			if (saved_menustate || !osd_unlocked) back = true;
			else menu = true;
			break;
		case KEY_BACKSPACE | UPSTROKE:
			if (saved_menustate || !osd_unlocked) back = true;
			break;
		case KEY_ENTER:
		case KEY_SPACE:
		case KEY_KPENTER:
			select = true;
			break;
		case KEY_UP:
			up = true;
			break;
		case KEY_DOWN:
			down = true;
			break;
		case KEY_LEFT:
			left = true;
			break;
		case KEY_RIGHT:
			right = true;
			break;
		case KEY_KPPLUS:
		case KEY_EQUAL: // =/+
			plus = true;
			break;
		case KEY_KPMINUS:
		case KEY_MINUS: // -/_
			minus = true;
			break;
		case KEY_GRAVE:
			recent = true;
			break;
		}
	}

	if (select_ini)
	{
		DISKLED_ON;

		switch (c)
		{
		case KEY_ESC | UPSTROKE:
			menu = true;
			break;
		case KEY_UP:
			up = true;
			break;
		case KEY_DOWN:
			down = true;
			break;
		case KEY_LEFT:
			left = true;
			break;
		case KEY_RIGHT:
			right = true;
			break;
		}
	}

	if (menu || select || up || down || left || right || plus || minus || (helptext_idx_old != helptext_idx))
	{
		helptext_idx_old = helptext_idx;
		if (helpstate) OsdWrite(OsdGetSize()-1, STD_EXIT, (menumask - ((1 << (menusub + 1)) - 1)) <= 0, 0); // Redraw the Exit line...
		helpstate = 0;
		helptext_timer = GetTimer(helptext_timeouts[helptext_idx]);
	}

	if (helptext_idx)
	{
		if (helpstate<9)
		{
			if (CheckTimer(helptext_timer))
			{
				helptext_timer = GetTimer(32);
				OsdShiftDown(OsdGetSize() - 1);
				++helpstate;
			}
		}
		else if (helpstate == 9)
		{
			ScrollReset(1);
			++helpstate;
		}
		else
		{
			ScrollText(OsdGetSize() - 1, helptexts[helptext_idx], 0, 0, 0, 0, 1);
		}
	}

	// Standardised menu up/down.
	// The screen should set menumask, bit 0 to make the top line selectable, bit 1 for the 2nd line, etc.
	// (Lines in this context don't have to correspond to rows on the OSD.)
	// Also set parentstate to the appropriate menustate.
	if (menumask)
	{
		if (down)
		{
			if((menumask >= ((uint64_t)1 << (menusub + 1))))	// Any active entries left?
			{
				do
				{
					menusub++;
				} while ((menumask & ((uint64_t)1 << menusub)) == 0);
			} else {
				menusub = 0; // jump to first item
				while ((menumask & ((uint64_t)1 << menusub )) == 0) menusub++;
			}

			menustate = parentstate;
		}

		if (up)
		{
			if (menusub > 0)
			{
				do
				{
					--menusub;
				} while (menusub != 0 && (menumask & ((uint64_t)1 << menusub)) == 0);
				if (menusub == 0 && (menumask & 1) == 0) { //If the first menu entry is disabled...
					while ((menumask & ((uint64_t)(~0) << (menusub + 1))) != 0) menusub++;
					//Go to to last item
				}
			} else {
				do
				{
					menusub++;
				} while ((menumask & ((uint64_t)(~0) << (menusub + 1))) != 0); // jump to last item
			}
			menustate = parentstate;
		}
	}

	// Ensure we clear out the file-selector-visible file on select or cancel
	if (cfg.log_file_entry)
	{
		if (menustate == fs_MenuSelect)
			MakeFile("/tmp/FILESELECT", "selected");
		else if (menustate == fs_MenuCancel)
			MakeFile("/tmp/FILESELECT", "cancelled");
	}

	switch (menustate)
	{
	case MENU_NONE1:
	case MENU_NONE2:
	case MENU_INFO:
		break;

	default:
		saved_menustate = 0;
		break;
	}

	if (osd_lock_timer == 0) osd_lock_timer = GetTimer(10 * 1000);

	switch (menustate)
	{
	case MENU_NONE1:
	case MENU_NONE2:
	case MENU_INFO:
		if (!cfg.osd_lock[0] || is_menu() || !mgl->done) osd_unlocked = 1;
		else if (cfg.osd_lock_time && CheckTimer(osd_lock_timer)) osd_unlocked = 0;
		break;

	case MENU_UNLOCK1:
	case MENU_UNLOCK2:
	case MENU_UNLOCK3:
		break;

	default:
		osd_unlocked = 1;
		osd_lock_timer = GetTimer(cfg.osd_lock_time * 1000);
		break;
	}

	// Switch to current menu screen
	switch (menustate)
	{
		/******************************************************************/
		/* no menu selected                                               */
		/******************************************************************/
	case MENU_NONE1:
		helptext_idx = 0;
		menumask = 0;
		menustate = MENU_NONE2;
		firstmenu = 0;
		vga_nag();
		OsdSetSize(8);
		break;

	case MENU_INFO:
		if (CheckTimer(menu_timer)) menustate = MENU_NONE1;
		// fall through

	case MENU_NONE2:
		if (menu && !osd_unlocked)
		{
			menustate = MENU_UNLOCK1;
			osd_code_entry[0] = 0;
		}
		else if (menu || (is_menu() && !video_fb_state()) || (menustate == MENU_NONE2 && !mgl->done && mgl->state == 1))
		{
			OsdSetSize(16);
			menusub = 0;
			if(!is_menu() && (get_key_mod() & (LALT | RALT))) //Alt+Menu
			{
				menustate = MENU_CORES1;
			}
			else if (saved_menustate)
			{
				menustate = saved_menustate;
			}
			else if (is_st()) menustate = MENU_ST_MAIN1;
			else if (is_archie()) menustate = MENU_ARCHIE_MAIN1;
			else {
				if (select_ini)
				{
					menustate = MENU_SELECT_INI1;
				}
				else if (is_menu())
				{
					menusub = 6;
					SelectFile("", 0, SCANO_CORES, MENU_CORE_FILE_SELECTED1, MENU_SYSTEM1);
				}
				else if (is_minimig())
				{
					menustate = MENU_MINIMIG_MAIN1;
				}
				else
				{
					if ((get_key_mod() & (LGUI | RGUI)) && !is_f12_mod_needed() && has_menu()) //Win+Menu
					{
						menustate = MENU_COMMON1;
					}
					else
					{
						parentstate = MENU_NONE1;
						menustate = MENU_GENERIC_MAIN1;
					}
				}
			}
			OsdClear();
			if (!mgl->done) OsdDisable();
			else OsdEnable(DISABLE_KEYBOARD);
			if (mgl->state == 1) mgl->state = 2;
		}
		break;

	case MENU_SELECT_INI1:
		{
			OsdSetTitle("Select INI");
			int flag = 1;
			for (int i = 1; i < 4; i++) if (FileExists(cfg_get_name(i))) flag |= 1 << i;
			for(int i = 0; i<16; i++) OsdWrite(m++);
			m = 4;
			OsdWrite(m++, "  Use dpad/cursor to select");
			m += 2;
			sprintf(s, "            %s", cfg_get_label(0));
			OsdWrite(m++, s);
			m++;
			strcpy(s, "      ");
			if (!(flag & 2)) strcat(s, "\xb");
			strcat(s, cfg_get_label(1));
			if (!(flag & 2)) strcat(s, "\xb");
			strcat(s, "        ");
			if (!(flag & 8)) strcat(s, "\xb");
			strcat(s, cfg_get_label(3));
			if (!(flag & 8)) strcat(s, "\xb");
			OsdWrite(m++, s);
			m++;
			strcpy(s, "            ");
			if (!(flag & 4)) strcat(s, "\xb");
			strcat(s, cfg_get_label(2));
			if (!(flag & 4)) strcat(s, "\xb");
			OsdWrite(m++, s);
			m++;
			parentstate = MENU_SELECT_INI1;
			menustate = MENU_SELECT_INI2;
		}
		break;

	case MENU_SELECT_INI2:
		if (menu || up)
		{
			menustate = MENU_NONE1;
			select_ini = 0;
		}
		else if (left && FileExists(cfg_get_name(1)))
		{
			user_io_set_ini(1);
		}
		else if (down && FileExists(cfg_get_name(2)))
		{
			user_io_set_ini(2);
		}
		else if (right && FileExists(cfg_get_name(3)))
		{
			user_io_set_ini(3);
		}
		break;

		/******************************************************************/
		/* archimedes main menu                                           */
		/******************************************************************/

	case MENU_ARCHIE_MAIN1:
		OsdSetTitle(CoreName, OSD_ARROW_RIGHT | OSD_ARROW_LEFT);

		m = 0;
		menumask = 0x1fff;

		strcpy(s, " Floppy 0: ");
		strncat(s, get_image_name(0) ? get_image_name(0) : "* no disk *",27);
		OsdWrite(m++, s, menusub == 0);

		strcpy(s, " Floppy 1: ");
		strncat(s, get_image_name(1) ? get_image_name(1) : "* no disk *", 27);
		OsdWrite(m++, s, menusub == 1);

		OsdWrite(m++);

		strcpy(s, " HDD 0: ");
		strncat(s, archie_get_hdd_name(0) ? archie_get_hdd_name(0) : "* no disk *", 27);
		OsdWrite(m++, s, menusub == 2);

		strcpy(s, " HDD 1: ");
		strncat(s, archie_get_hdd_name(1) ? archie_get_hdd_name(1) : "* no disk *", 27);
		OsdWrite(m++, s, menusub == 3);

		OsdWrite(m++);

		strcpy(s, " OS ROM: ");
		strcat(s, archie_get_rom_name());
		OsdWrite(m++, s, menusub == 4);

		OsdWrite(m++);

		strcpy(s, " Aspect Ratio:    ");
		archie_set_ar(get_ar_name(archie_get_ar(), s));
		OsdWrite(m++, s, menusub == 5);

		strcpy(s, " Scale:           ");
		strcat(s, config_scale[archie_get_scale()]);
		OsdWrite(m++, s, menusub == 6);

		strcpy(s, " Refresh Rate:    ");
		strcat(s, archie_get_60() ? "Variable" : "60Hz");
		OsdWrite(m++, s, menusub == 7);

		sprintf(s, " Stereo Mix:      %s", config_stereo_msg[archie_get_amix()]);
		OsdWrite(m++, s, menusub == 8);

		strcpy(s, " 25MHz Audio Fix: ");
		strcat(s, archie_get_afix() ? "Enable" : "Disable");
		OsdWrite(m++, s, menusub == 9);

		sprintf(s, " Swap Joysticks:  %s", user_io_get_joyswap() ? "Yes" : "No");
		OsdWrite(m++, s, menusub == 10);
		sprintf(s, " Swap Btn 2/3:    %s", archie_get_mswap() ? "Yes" : "No");
		OsdWrite(m++, s, menusub == 11);

		while(m<15) OsdWrite(m++);

		OsdWrite(15, STD_EXIT, menusub == 12, 0);
		menustate = MENU_ARCHIE_MAIN2;
		parentstate = MENU_ARCHIE_MAIN1;

		// set helptext with core display on top of basic info
		sprintf(helptext_custom, HELPTEXT_SPACER);
		strcat(helptext_custom, OsdCoreNameGet());
		strcat(helptext_custom, helptexts[HELPTEXT_MAIN]);
		helptext_idx = HELPTEXT_CUSTOM;
		break;

	case MENU_ARCHIE_MAIN2:
		// menu key closes menu
		if (menu) menustate = MENU_NONE1;
		if (recent && (menusub <= 3))
		{
			fs_Options = SCANO_DIR | SCANO_UMOUNT;
			fs_MenuSelect = MENU_ARCHIE_MAIN_FILE_SELECTED;
			fs_MenuCancel = MENU_ARCHIE_MAIN1;
			strcpy(fs_pFileExt, (menusub <= 1) ? "ADF" : "HDF");
			if (recent_init((menusub <= 1) ? 500 : 501)) menustate = MENU_RECENT1;
		}

		if (select || plus || minus)
		{
			switch (menusub)
			{
			case 0:  // Floppy 0
			case 1:  // Floppy 1
				if (select)
				{
					ioctl_index = 0;
					SelectFile(Selected_S[menusub], "ADF", SCANO_DIR | SCANO_UMOUNT, MENU_ARCHIE_MAIN_FILE_SELECTED, MENU_ARCHIE_MAIN1);
				}
				break;

			case 2:  // HDD 0
			case 3:  // HDD 1
				if (select)
				{
					ioctl_index = 1;
					SelectFile(Selected_S[menusub], "HDF", SCANO_DIR | SCANO_UMOUNT, MENU_ARCHIE_MAIN_FILE_SELECTED, MENU_ARCHIE_MAIN1);
				}
				break;

			case 4:  // Load ROM
				if (select)
				{
					ioctl_index = 0;
					SelectFile(Selected_F[menusub], "ROM", 0, MENU_ARCHIE_MAIN_FILE_SELECTED, MENU_ARCHIE_MAIN1);
				}
				break;

			case 5:
				archie_set_ar(next_ar(archie_get_ar(), minus));
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 6:
				archie_set_scale(archie_get_scale() + (minus ? -1 : 1));
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 7:
				archie_set_60(!archie_get_60());
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 8:
				archie_set_amix(archie_get_amix() + (minus ? -1 : 1));
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 9:
				archie_set_afix(!archie_get_afix());
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 10:
				user_io_set_joyswap(!user_io_get_joyswap());
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 11:
				archie_set_mswap(!archie_get_mswap());
				menustate = MENU_ARCHIE_MAIN1;
				break;

			case 12:  // Exit
				if (select) menustate = MENU_NONE1;
				break;
			}
		}

		if (right)
		{
			menustate = MENU_COMMON1;
			menusub = 0;
		}
		else if (left)
		{
			menustate = MENU_MISC1;
			menusub = 3;
		}
		break;

	case MENU_ARCHIE_MAIN_FILE_SELECTED:
		menustate = MENU_ARCHIE_MAIN1;
		if (menusub <= 1)
		{
			memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
			recent_update(SelectedDir, Selected_F[menusub], SelectedLabel, 500);
			user_io_file_mount(selPath, menusub);
		}
		else if (menusub <= 3)
		{
			memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
			recent_update(SelectedDir, Selected_F[menusub], SelectedLabel, 501);
			archie_hdd_mount(selPath, menusub - 2);
		}
		else if (menusub == 4)
		{
			memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
			archie_set_rom(selPath);
			menustate = MENU_NONE1;
		}
		break;

	case MENU_GENERIC_MAIN1:
	case MENU_CORES1:
		parentstate = menustate;
		menustate = MENU_SAVE_CHECK;
		break;

	case MENU_GENERIC_MAIN2: {
		hdmask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);
		user_io_read_confstr();
		uint32_t s_entry = 0;
		int entry = 0;
		while(1)
		{
			if (!menusub) firstmenu = 0;

			adjvisible = 0;
			entry = 0;
			uint32_t selentry = 0;
			menumask = 0;

			OsdSetTitle(page ? title : user_io_get_core_name());

			dip_submenu = -1;
			manual_submenu = -1;

			int last_space = 0;

			// add options as requested by core
			int i = 2;
			do
			{
				char* pos;

				p = user_io_get_confstr(i++);
				//printf("Option %d: %s\n", i-1, p);

				if (p)
				{
					int h = 0, d = 0, inpage = !page;

					if (!strncmp(p, "DEFMRA,", 7))
					{
					}
					else if (!strcmp(p, "DIP"))
					{
						h = page;
						if (!h && arcade_sw()->dip_num)
						{
							dip_submenu = selentry;
							MenuWrite(entry, " DIP Switches              \x16", menusub == selentry, 0);
							entry++;
							selentry++;
							menumask = (menumask << 1) | 1;
						}
						continue;
					}
					else
					{
						//Hide or Disable flag (small letter - opposite action)
						while ((p[0] == 'H' || p[0] == 'D' || p[0] == 'h' || p[0] == 'd') && strlen(p) > 2)
						{
							int flg = (hdmask & (1 << user_io_hd_mask(p + 1))) ? 1 : 0;
							if (p[0] == 'H') h |= flg;
							if (p[0] == 'h') h |= (flg ^ 1);
							if (p[0] == 'D') d |= flg;
							if (p[0] == 'd') d |= (flg ^ 1);
							p += 2;
						}

						if (p[0] == 'P')
						{
							int n = p[1] - '0';
							if (p[2] != ',')
							{
								if (page && page == n) inpage = 1;
								if (!page && n && !flat) inpage = 0;
								p += 2;

								if (flat && !page && p[0] == '-') inpage = 0;
							}
							else if (flat && !page && !last_space)
							{
								MenuWrite(entry, "", 0, d);
								entry++;
							}
						}
					}

					last_space = 0;

					if (!h && inpage)
					{
						if (p[0] == 'P')
						{
							if (flat)
							{
								strcpy(s, " \x16 ");
								substrcpy(s + 3, p, 1);

								int len = strlen(s);
								while (len < 28) s[len++] = ' ';
								s[28] = 0;
							}
							else
							{
								strcpy(s, " ");
								substrcpy(s + 1, p, 1);

								int len = strlen(s);
								while (len < 27) s[len++] = ' ';
								s[27] = 0x16;
								s[28] = 0;
							}

							MenuWrite(entry, s, menusub == selentry, d);

							// add bit in menu mask
							menumask = (menumask << 1) | 1;
							entry++;
							selentry++;
						}

						// check for 'F'ile or 'S'D image strings
						if ((p[0] == 'F') || (p[0] == 'S'))
						{
							if (p[0] == 'S') s_entry = selentry;
							substrcpy(s, p, 2);
							int idx = 1;

							if (p[idx] == 'S') idx++;
							if (p[idx] == 'C') idx++;

							int num = (p[idx] >= '0' && p[idx] <= '9') ? p[idx] - '0' : 0;
							if (!mgl->done && num == mgl->item[mgl->current].index && (p[0] == mgl->item[mgl->current].type)) mgl->item[mgl->current].submenu = selentry;

							if (is_x86() && x86_get_image_name(num))
							{
								strcpy(s, " ");
								substrcpy(s + 1, p, 2);
								strcat(s, " ");
								strcat(s, x86_get_image_name(num));
							}
							else if (is_pcxt() && (p[0] == 'S') && x86_get_image_name(num))
							{
								strcpy(s, " ");
								substrcpy(s + 1, p, 2);
								strcat(s, " ");
								strcat(s, x86_get_image_name(num));
							}
							else if (is_pcxt() && (p[0] == 'F'))
							{
								strcpy(s, " ");
								substrcpy(s + 1, p, 2);
								static char str[1024];
								sprintf(str, "%s.f%c", user_io_get_core_name(), p[idx]);
								if (FileLoadConfig(str, str, sizeof(str)) && str[0])
								{
									strcat(s, " ");
									strcat(s, GetNameFromPath(str));
								}
								else
								{
									strcat(s, " *.");
									pos = s + strlen(s);
									substrcpy(pos, p, 1);
									strcpy(pos, GetExt(pos));
								}
							}
							else if ((p[0] == 'S') && get_image_name(num))
							{
								strcpy(s, "\x16");
								strncat(s, get_image_name(num), sizeof(s) - strlen(s) - 1);
							}
							else
							{
								if (strlen(s))
								{
									strcpy(s, " ");
									substrcpy(s + 1, p, 2);
									strcat(s, " *.");
								}
								else
								{
									if (p[0] == 'F') strcpy(s, " Load *.");
									else             strcpy(s, " Mount *.");
								}
								pos = s + strlen(s);
								substrcpy(pos, p, 1);
								strcpy(pos, GetExt(pos));
							}
							MenuWrite(entry, s, menusub == selentry, d);

							// add bit in menu mask
							menumask = (menumask << 1) | 1;
							entry++;
							selentry++;
						}

						// check for 'C'heats
						if (p[0] == 'C')
						{
							if (game_docs_manual_available())
							{
								manual_submenu = selentry;
								MenuWrite(entry, " Manual", menusub == selentry, 0);

								// add bit in menu mask
								menumask = (menumask << 1) | 1;
								entry++;
								selentry++;
							}

							substrcpy(s, p, 1);
							if (strlen(s))
							{
								strcpy(s, " ");
								substrcpy(s + 1, p, 1);
							}
							else
							{
								strcpy(s, " Cheats");
							}
							MenuWrite(entry, s, menusub == selentry, !cheats_available() || d);

							// add bit in menu mask
							menumask = (menumask << 1) | 1;
							entry++;
							selentry++;
						}

						// check for 'T'oggle and 'R'eset (toggle and then close menu) strings
						if ((p[0] == 'T') || (p[0] == 'R') || (p[0] == 't') || (p[0] == 'r'))
						{

							s[0] = ' ';
							substrcpy(s + 1, p, 1);
							MenuWrite(entry, s, menusub == selentry, d);

							// add bit in menu mask
							menumask = (menumask << 1) | 1;
							entry++;
							selentry++;
						}

						// check for 'O'ption strings
						if ((p[0] == 'O') || (p[0] == 'o'))
						{
							//option handled by HPS
							if (p[1] == 'X') p++;
							uint32_t x = user_io_status_get(p + 1, p[0] == 'o');

							// get currently active option
							substrcpy(s, p, 2 + x);
							int l = strlen(s);
							int arc = get_arc(s);
							if (!l || arc < 0)
							{
								// option's index is outside of available values.
								// reset to 0.
								x = 0;
								//user_io_status(setStatus(p, status, x), 0xffffffff);
								substrcpy(s, p, 2 + x);
								l = strlen(s);
								arc = get_arc(s);
							}

							if (arc > 0) l = strlen(cfg.custom_aspect_ratio[arc - 1]);

							s[0] = ' ';
							substrcpy(s + 1, p, 1);

							char *end = s + strlen(s) - 1;
							while ((end > s + 1) && (*end == ' ')) end--;
							*(end + 1) = 0;

							int len = strlen(s);
							if (len+l > 27) len = 27-l;
							s[len++] = ':';
							s[len] = 0;

							l = 28 - l - strlen(s);
							while (l--) strcat(s, " ");

							if (arc > 0) strcpy(s + strlen(s), cfg.custom_aspect_ratio[arc - 1]);
							else substrcpy(s + strlen(s), p, 2 + x);

							MenuWrite(entry, s, menusub == selentry, d);

							// add bit in menu mask
							menumask = (menumask << 1) | 1;
							entry++;
							selentry++;
						}

						// delimiter, text
						if (p[0] == '-')
						{
							s[0] = ' ';
							s[1] = 0;
							substrcpy(s + 1, p, 1);
							last_space = (strlen(s) == 1);
							MenuWrite(entry, s, 0, d);
							entry++;
						}
					}
				}
			} while (p);

			if (!entry) break;

			for (; entry < OsdGetSize() - 1; entry++) MenuWrite(entry, "", 0, 0);

			// exit row
			if (!page)
			{
				MenuWrite(entry, STD_EXIT, menusub == selentry, 0, OSD_ARROW_RIGHT | OSD_ARROW_LEFT);
			}
			else
			{
				MenuWrite(entry, STD_BACK, menusub == selentry, 0, 0);
			}
			menusub_last = selentry;
			menumask = (menumask << 1) | 1;

			if (parentstate == MENU_NONE1 && is_pce() && pcecd_using_cd() && menusub != s_entry)
			{
				menusub = s_entry;
				continue;
			}

			if (!adjvisible) break;
			firstmenu += adjvisible;
		}

		if (!entry)
		{
			if (page) page = 0;
			else menustate = MENU_COMMON1;
			menusub = 0;
			break;
		}

		parentstate = menustate;
		menustate = MENU_GENERIC_MAIN3;

		// set helptext with core display on top of basic info
		sprintf(helptext_custom, HELPTEXT_SPACER);
		strcat(helptext_custom, OsdCoreNameGet());
		if (is_arcade())
		{
			strcat(helptext_custom, " (");
			strcat(helptext_custom, user_io_get_core_name(1));
			strcat(helptext_custom, ")");
		}
		strcat(helptext_custom, helptexts[HELPTEXT_MAIN]);
		helptext_idx = HELPTEXT_CUSTOM;

	} break;

	case MENU_CORES2:
		SelectFile("", 0, SCANO_CORES, MENU_CORE_FILE_SELECTED1, MENU_NONE1);
		break;

	case MENU_SAVE_CHECK:
		menumask = 0;
		if (is_arcade() && spi_uio_cmd(UIO_CHK_UPLOAD))
		{
			menu_save_timer = GetTimer(500);
			arcade_nvm_save();
		}

		if (menu_save_timer && !CheckTimer(menu_save_timer))
		{
			for (int i = 0; i < 16; i++) OsdWrite(m++);
			OsdWrite(8, "          Saving...");
			menustate = MENU_SAVE_WAIT;
			break;
		}

		menustate = parentstate+1;
		break;

	case MENU_SAVE_WAIT:
		if (menu)
		{
			menu_save_timer = 0;
			menustate = MENU_NONE1;
		}
		else if (menu_save_timer && CheckTimer(menu_save_timer))
		{
			menu_save_timer = 0;
			menustate = parentstate + 1;
		}
		break;

	case MENU_GENERIC_MAIN3:
		saved_menustate = MENU_GENERIC_MAIN1;

		// F/S option not found -> deactivate mgl.
		if (!mgl->done && mgl->item[mgl->current].submenu < 0)
		{
			menustate = MENU_NONE1;
			mgl->state = 3;
		}

		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if(back || (left && page) || (menusub == menusub_last && select))
		{
			if(!page) menustate = MENU_NONE1;
			else
			{
				firstmenu = 0;
				menustate = MENU_GENERIC_MAIN1;
				menusub = menusub_parent;
				page = 0;
			}
		}
		else if (select || recent || minus || plus || !mgl->done)
		{
			if (!mgl->done)
			{
				menusub = mgl->item[mgl->current].submenu;
				select = 1;
			}

			if (manual_submenu == menusub)
			{
				if (select)
				{
					snprintf(selPath, sizeof(selPath), "%s", game_docs_get_manual());
					menustate = MENU_DOC_FILE_SELECTED;
				}
			}
			else if (dip_submenu == menusub && select)
			{
				menustate = MENU_ARCADE_DIP1;
				menusub = 0;
			}
			else
			{
				static char ext[256];
				int h = 0, d = 0, inpage = !page;
				uint32_t entry = 0;
				int i = 2;

				p = 0;
				addon[0] = 0;

				while (1)
				{
					p = user_io_get_confstr(i++);
					if (!p) break;

					h = 0;
					d = 0;
					inpage = !page;

					if (!strcmp(p, "DIP")) h = page || !arcade_sw()->dip_num;
					else if (strncmp(p, "DEFMRA,", 7))
					{
						//Hide or Disable flag
						while ((p[0] == 'H' || p[0] == 'D' || p[0] == 'h' || p[0] == 'd') && strlen(p) > 2)
						{
							int flg = (hdmask & (1 << user_io_hd_mask(p + 1))) ? 1 : 0;
							if (p[0] == 'H') h |= flg;
							if (p[0] == 'h') h |= (flg ^ 1);
							if (p[0] == 'D') d |= flg;
							if (p[0] == 'd') d |= (flg ^ 1);
							p += 2;
						}
					}

					if (p[0] == 'P')
					{
						int n = p[1] - '0';
						if (p[2] != ',')
						{
							if (page && page == n) inpage = 1;
							if (!page && n && !flat) inpage = 0;
							p += 2;
						}
					}

					if (!inpage || h || p[0] < 'A') continue;

					// supplement files
					if (p[0] == 'f')
					{
						strcpy(addon, p);
						continue;
					}

					if (p[0] == 'C' && game_docs_manual_available())
					{
						if (entry == menusub) break;
						entry++;
					}

					if (entry == menusub) break;
					entry++;

					if (p[0] == 'F' || p[0] == 'S') addon[0] = 0;
				}

				if (p && !d)
				{
					if (p[0] == 'F' && (select || recent))
					{
						store_name = 0;
						opensave = 0;
						ioctl_index = menusub + 1;
						int idx = 1;

						if (p[idx] == 'S')
						{
							opensave = 1;
							idx++;
						}

						if (p[idx] == 'C')
						{
							store_name = 1;
							idx++;
						}

						if (p[idx] >= '0' && p[idx] <= '9') ioctl_index = p[idx] - '0';
						substrcpy(ext, p, 1);
						if (is_gba() && FileExists(user_io_make_filepath(HomeDir(), "goomba.rom"))) strcat(ext, "GB GBC");
						while (strlen(ext) % 3) strcat(ext, " ");

						fs_Options = SCANO_DIR | (is_neogeo() ? SCANO_NEOGEO | SCANO_NOENTER : 0) | (store_name ? SCANO_CLEAR : 0) | (is_atari5200() || (is_atari800() && (ioctl_index == 8 || ioctl_index == 9)) ? SCANO_UMOUNT : 0);
						fs_MenuSelect = MENU_GENERIC_FILE_SELECTED;
						fs_MenuCancel = MENU_GENERIC_MAIN1;
						strcpy(fs_pFileExt, ext);

						load_addr = 0;
						if (substrcpy(s, p, 3))
						{
							load_addr = strtoul(s, NULL, 16);
							if (load_addr < 0x20000000 || load_addr >= 0x40000000)
							{
								printf("Loading address 0x%X is outside the supported range! Using normal load.\n", load_addr);
								load_addr = 0;
							}
						}

						if (!mgl->done) menustate = MENU_GENERIC_FILE_SELECTED;
						else if (select) SelectFile(Selected_F[ioctl_index & 15], ext, fs_Options, fs_MenuSelect, fs_MenuCancel);
						else if (recent_init(ioctl_index)) menustate = MENU_RECENT1;
					}
					else if (p[0] == 'S' && (select || recent))
					{
						store_name = 0;
						int idx = 1;

						if (p[idx] == 'C')
						{
							store_name = 1;
							idx++;
						}

						ioctl_index = 0;
						if ((p[idx] >= '0' && p[idx] <= '9') || is_x86() || is_pcxt()) ioctl_index = p[idx] - '0';
						substrcpy(ext, p, 1);
						while (strlen(ext) % 3) strcat(ext, " ");

						fs_Options = SCANO_DIR | SCANO_UMOUNT;
						fs_MenuSelect = MENU_GENERIC_IMAGE_SELECTED;
						fs_MenuCancel = MENU_GENERIC_MAIN1;
						strcpy(fs_pFileExt, ext);

						memcpy(Selected_tmp, Selected_S[(int)ioctl_index], sizeof(Selected_tmp));
						if (is_x86() || is_pcxt()) strcpy(Selected_tmp, x86_get_image_path(ioctl_index));
						if (is_psx() && (ioctl_index == 2 || ioctl_index == 3)) fs_Options |= SCANO_SAVES;
						if (is_saturn() && (ioctl_index == 1)) fs_Options |= SCANO_SAVES;

						if ((is_saturn() && !(fs_Options & SCANO_SAVES)) || is_pce() || is_megacd() || is_3do() || is_x86() || is_cdi() || (is_psx() && !(fs_Options & SCANO_SAVES)) || is_neogeo())
						{
							//look for CHD too
							if (!strcasestr(ext, "CHD"))
							{
								strcat(fs_pFileExt, "CHD");
								strcat(ext, "CHD");
							}

							int num = ScanDirectory(Selected_tmp, SCANF_INIT, fs_pFileExt, 0);
							memcpy(Selected_tmp, Selected_S[(int)ioctl_index], sizeof(Selected_tmp));

							if (num == 1)
							{
								fs_Options |= SCANO_NOENTER;
								char *p = strrchr(Selected_tmp, '/');
								if (p) *p = 0;
							}

							fs_Options |= SCANO_NOZIP;
						}

						if (is_psx()) fs_Options |= SCANO_NOZIP;

						if (!mgl->done) menustate = MENU_GENERIC_IMAGE_SELECTED;
						else if (select) SelectFile(Selected_tmp, ext, fs_Options, fs_MenuSelect, fs_MenuCancel);
						else if (recent_init(ioctl_index + 500)) menustate = MENU_RECENT1;
					}
					else if (select || minus || plus)
					{
						if (p[0] == 'P' && select)
						{
							page = p[1] - '0';
							if (page < 1 || page > 9) page = 0;
							menusub_parent = menusub;
							substrcpy(title, p, 1);
							menustate = MENU_GENERIC_MAIN1;
							menusub = 0;
						}
						else if (p[0] == 'C' && cheats_available() && select)
						{
							menustate = MENU_CHEATS1;
							cheatsub = menusub;
							menusub = 0;
						}
						else if ((p[0] == 'O') || (p[0] == 'o'))
						{
							int ex = (p[0] == 'o');

							int byarm = 0;
							if (p[1] == 'X')
							{
								byarm = 1;
								p++;
							}

							uint32_t x = user_io_status_get(p + 1, ex);
							x = minus ? (x - 1) : (x + 1);
							uint32_t mask = user_io_status_mask(p + 1);
							x &= mask;

							if (byarm && is_x86() && p[1] == '2') x86_set_fdd_boot(!(x & 1));

							// check if next value available
							if (minus)
							{
								while(1)
								{
									substrcpy(s, p, 2 + x);
									if (strlen(s) && get_arc(s) >= 0) break;
									x = (x - 1) & mask;
								}
							}
							else
							{
								substrcpy(s, p, 2 + x);
								if (!strlen(s) || get_arc(s) < 0) x = 0;
							}

							user_io_status_set(p + 1, x, ex);

							if (is_x86() && p[1] == 'A')
							{
								int mode = GetUARTMode();
								if (mode != 0)
								{
									SetUARTMode(0);
									SetUARTMode(mode);
								}
							}
							menustate = MENU_GENERIC_MAIN1;
						}
						else if (((p[0] == 'T') || (p[0] == 'R') || (p[0] == 't') || (p[0] == 'r')) && select)
						{
							int bit;
							int ex = (p[0] == 't') || (p[0] == 'r');
							if (user_io_status_bits(p + 1, &bit, 0, ex) == 1)
							{
								const char *opt = p + 1;
								if (!bit && (is_x86() || is_pcxt()))
								{
									x86_init();
									ResetUART();
									menustate = MENU_NONE1;
								}
								else
								{
									if (!bit && is_uneon())
									{
										x86_ide_set();
										menustate = MENU_NONE1;
									}

									if (is_megacd())
									{
										if (!bit) mcd_set_image(0, "");
										if (bit == 1)
										{
											mcd_reset();
											opt = "[0]";
										}
									}

									if (is_pce() && !bit) pcecd_reset();
									if (is_saturn() && !bit) saturn_reset();
									if (is_n64() && !bit) n64_reset();
									if (is_psx() && !bit) psx_reset();
									if (is_atari800() && !bit) atari800_reset();
									if (is_atari5200() && !bit) atari5200_reset();
									if (is_3do() && !bit) p3do_reset();

									user_io_status_set(opt, 1, ex);
									if (is_n64() && !ex && !strcmp(opt, "[41]")) n64_save_dd_disk();
									user_io_status_set(opt, 0, ex);

									menustate = MENU_GENERIC_MAIN1;
									if (p[0] == 'R' || p[0] == 'r') menustate = MENU_NONE1;
								}
							}
						}
					}
					else if (recent)
					{
						flat = !flat;
						page = 0;
						menustate = MENU_GENERIC_MAIN1;
						menusub = 0;
					}
				}
			}
		}
		else if (right && !page)
		{
			menustate = MENU_COMMON1;
			menusub = 0;
		}
		else if (left)
		{
			menustate = MENU_MISC1;
			menusub = 3;
		}
		else if(spi_uio_cmd16(UIO_GET_OSDMASK, 0) != hdmask)
		{
			menustate = MENU_GENERIC_MAIN1;
		}

		break;

	case MENU_GENERIC_FILE_SELECTED:
		{
			if (!mgl->done)
			{
				if(mgl->item[mgl->current].path[0] == '/') snprintf(selPath, sizeof(selPath), "%s", mgl->item[mgl->current].path);
				else snprintf(selPath, sizeof(selPath), "%s/%s", HomeDir(), mgl->item[mgl->current].path);

				// Update /tmp/ files to reflect the actual file being loaded by MGL
				if (cfg.log_file_entry)
				{
					const char *fname = strrchr(selPath, '/');
						MakeFile("/tmp/FULLPATH", selPath);
					MakeFile("/tmp/CURRENTPATH", fname ? fname + 1 : selPath);
					MakeFile("/tmp/FILESELECT", "selected");
				}
			}

			if(!(selPath[0] && (is_atari5200() || (is_atari800() && (ioctl_index == 8 || ioctl_index == 9))))) MenuHide();
			printf("File selected: %s\n", selPath);
			memcpy(Selected_F[ioctl_index & 15], selPath, sizeof(Selected_F[ioctl_index & 15]));

			if (mgl->done && selPath[0]) recent_update(SelectedDir, Selected_F[ioctl_index & 15], SelectedLabel, ioctl_index);

			if (store_name)
			{
				char str[64];
				sprintf(str, "%s.f%d", user_io_get_core_name(), ioctl_index);
				FileSaveConfig(str, selPath, sizeof(selPath));
			}

			if (selPath[0])
			{

				char idx = user_io_ext_idx(selPath, fs_pFileExt) << 6 | ioctl_index;
				if (addon[0] == 'f' && addon[1] != '1') process_addon(addon, idx);

				if (fs_Options & SCANO_NEOGEO)
				{
					neocd_set_en(0);
					neogeo_romset_tx(selPath, 0);
				}
				else
				{
					if (is_pce())
					{
						pcecd_set_image(0, "");
						pcecd_reset();
					}
					if (!store_name) user_io_store_filename(selPath);
					if (is_n64())
					{
						uint32_t n64_crc;
						if (!n64_rom_tx(selPath, idx, load_addr, n64_crc))
                Info("failed to load ROM");
						else if (!store_name)
						{
							game_docs_init(selPath, n64_crc);
							if (user_io_use_cheats()) cheats_init(selPath, n64_crc);
						}
					}
					else if (is_c64() || is_c128())
					{
						c64_open_file(selPath, idx);
					}
					else if (is_atari800() || is_atari5200())
					{
						if(is_atari800() && ioctl_index != 8 && ioctl_index != 9)
						{
							atari800_open_bios_file(selPath, idx);
						}
						else
						{
							int a8bit_cart_matches = is_atari5200() ? atari5200_check_cartridge_file(selPath, idx): atari800_check_cartridge_file(selPath, idx);
							if(a8bit_cart_matches <= 1) MenuHide();
							if(a8bit_cart_matches == 1)
							{
								if(is_atari5200())
								{
									atari5200_open_cartridge_file(selPath, 0);
								}
								else
								{
									atari800_open_cartridge_file(selPath, 0);
								}
							}
							else if(a8bit_cart_matches > 1 && mgl->done)
							{
								menustate = MENU_ATARI8BIT_CART1;
								menusub = 0;
							}
							else if(mgl->done)
							{
								Info("Unsupported cartridge type!", 2000);
							}
						}
					}
					else
					{
						user_io_file_tx(selPath, idx, opensave, 0, 0, load_addr);
						if (!store_name)
						{
							game_docs_init(selPath, user_io_get_file_crc());
							if (user_io_use_cheats()) cheats_init(selPath, user_io_get_file_crc());
						}
					}
				}

				if (addon[0] == 'f' && addon[1] == '1') process_addon(addon, idx);
			}
			else if(is_atari800() && (ioctl_index == 8 || ioctl_index == 9))
			{
				atari800_umount_cartridge(ioctl_index == 9);
			}
			else if(is_atari5200())
			{
				atari5200_umount_cartridge();
			}

			mgl->state = 3;
		}
		break;

	case MENU_GENERIC_IMAGE_SELECTED:
		{
			if (!mgl->done)
			{
				if (mgl->item[mgl->current].path[0] == '/') snprintf(selPath, sizeof(selPath), "%s", mgl->item[mgl->current].path);
				else snprintf(selPath, sizeof(selPath), "%s/%s", HomeDir(((is_pce() && !strncasecmp(fs_pFileExt, "CUE", 3)) ? PCECD_DIR : NULL)), mgl->item[mgl->current].path);

				// Update /tmp/ files to reflect the actual image being loaded by MGL
				if (cfg.log_file_entry)
				{
					const char *fname = strrchr(selPath, '/');
						MakeFile("/tmp/FULLPATH", selPath);
					MakeFile("/tmp/CURRENTPATH", fname ? fname + 1 : selPath);
					MakeFile("/tmp/FILESELECT", "selected");
				}
			}

			if (store_name)
			{
				char str[64];
				sprintf(str, "%s.s%d", user_io_get_core_name(), ioctl_index);
				FileSaveConfig(str, selPath, sizeof(selPath));
			}

			menustate = MENU_GENERIC_MAIN1;
			if (selPath[0] && !is_x86() && !is_pcxt()) MenuHide();

			printf("Image selected: %s\n", selPath);
			memcpy(Selected_S[(int)ioctl_index], selPath, sizeof(Selected_S[(int)ioctl_index]));
			if (mgl->done) recent_update(SelectedDir, Selected_S[(int)ioctl_index], SelectedLabel, ioctl_index + 500);

			char idx = user_io_ext_idx(selPath, fs_pFileExt) << 6 | ioctl_index;
			if (addon[0] == 'f' && addon[1] != '1') process_addon(addon, idx);

			else if (is_x86() || is_pcxt() || (is_uneon() && idx >= 2))
			{
				x86_set_image(ioctl_index, selPath);
			}
			else if (is_megacd())
			{
				mcd_set_image(ioctl_index, selPath);
				game_docs_init(selPath, 0);
			}
			else if (is_pce())
			{
				pcecd_set_image(ioctl_index, selPath);
				game_docs_init(selPath, 0);
				cheats_init(selPath, 0);
			}
			else if (is_psx() && ioctl_index == 1)
			{
				psx_mount_cd(user_io_ext_idx(selPath, fs_pFileExt) << 6 | (menusub + 1), ioctl_index, selPath);
				game_docs_init(selPath, 0);
				cheats_init(selPath, 0);
			}
			else if (is_cdi())
			{
				cdi_mount_cd(ioctl_index, selPath);
			}
			else if (is_saturn())
			{
				if (!ioctl_index)
				{
					saturn_set_image(ioctl_index, selPath);
				} else {
					saturn_mount_save(selPath);
				}
			}
			else if (is_neogeo())
			{
				neocd_set_en(1);
				neocd_set_image(selPath);
			}
			else if (is_atari800())
			{
				atari800_set_image(user_io_ext_idx(selPath, fs_pFileExt), ioctl_index, selPath);
			}
			else if (is_3do())
			{
				p3do_set_image(ioctl_index, selPath);
			}
			else
			{
				user_io_set_index(user_io_ext_idx(selPath, fs_pFileExt) << 6 | (menusub + 1));
				user_io_file_mount(selPath, ioctl_index);
			}

			if (addon[0] == 'f' && addon[1] == '1') process_addon(addon, idx);

			mgl->state = 3;
			if (!mgl->done) MenuHide();
		}
		break;

	case MENU_COMMON1:
		{
			OsdSetSize(16);
			helptext_idx = 0;
			reboot_req = 0;

			OsdSetTitle("System", 0);
			menustate = MENU_COMMON2;
			parentstate = MENU_COMMON1;
			int n;

			while(1)
			{
				n = 0;
				menumask = 0x7805d;

				if (!menusub) firstmenu = 0;
				adjvisible = 0;

				MenuWrite(n++, " Core                      \x16", menusub == 0, 0);
				MenuWrite(n++);

				if (cfg.osd_lock[0] && !cfg.osd_lock_time)
				{
					MenuWrite(n++, " Lock OSD", menusub == 1, 0);
					MenuWrite(n++);
					menumask |= 2;
				}

				sprintf(s, " Define %s buttons         ", is_menu() ? "System" : user_io_get_core_name());
				s[27] = '\x16';
				s[28] = 0;
				MenuWrite(n++, s, menusub == 2, 0);
				MenuWrite(n++, " Button/Key remap          \x16", menusub == 3, 0);
				MenuWrite(n++, " Reset player assignment", menusub == 4, 0);

				if (user_io_get_uart_mode())
				{
					menumask |= 0x20;
					MenuWrite(n++);
					int mode = GetUARTMode();
					const char *p = config_uart_msg[mode];
					while (*p == ' ') p++;
					sprintf(s, " UART mode (%s)            ",p);
					s[27] = '\x16';
					s[28] = 0;
					MenuWrite(n++, s, menusub == 5);
				}

				MenuWrite(n++);
				MenuWrite(n++, " Video processing          \x16", menusub==6);

				if (audio_filter_en() >= 0)
				{
					MenuWrite(n++);
					menumask |= 0x600;
					sprintf(s, " Audio filter - %s", config_afilter_msg[audio_filter_en() ? 1 : 0]);
					MenuWrite(n++, s, menusub == 9);

					memset(s, 0, sizeof(s));
					s[0] = ' ';
					if (strlen(audio_get_filter(1))) strncpy(s + 1, audio_get_filter(1), 25);
					else strcpy(s, " < none >");

					while (strlen(s) < 26) strcat(s, " ");
					strcat(s, " \x16 ");

					MenuWrite(n++, s, menusub == 10, !audio_filter_en() || !S_ISDIR(getFileType(AFILTER_DIR)));
				}

				if (!is_minimig() && !is_st())
				{
					menumask |= 0x6000;
					MenuWrite(n++);
					MenuWrite(n++, " Reset settings", menusub == 13, is_archie());
					MenuWrite(n++, " Save settings", menusub == 14, 0);
				}

				MenuWrite(n++);
				MenuWrite(n++, " Help                      \x16", menusub == 15);
				MenuWrite(n++, " About",                          menusub == 16);

				MenuWrite(n++);
				cr = n;
				MenuWrite(n++, " Reboot (hold \x16 cold reboot)", menusub == 17);

				while(n < OsdGetSize() - 1) MenuWrite(n++);
				MenuWrite(n++, STD_EXIT, menusub == 18, 0, OSD_ARROW_LEFT);
				sysinfo_timer = 0;

				if (!adjvisible) break;
				firstmenu += adjvisible;
			}

		}
		break;

	case MENU_COMMON2:
		if (menu)
		{
			menustate = MENU_NONE1;
		}

		if (recent && menusub == 0)
		{
			fs_Options = SCANO_CORES;
			fs_MenuSelect = MENU_CORE_FILE_SELECTED1;
			fs_MenuCancel = MENU_COMMON1;

			if (recent_init(-1)) menustate = MENU_RECENT1;
			break;
		}

		if (select)
		{
			switch (menusub)
			{
			case 0:
				SelectFile("", 0, SCANO_CORES, MENU_CORE_FILE_SELECTED1, MENU_COMMON1);
				menusub = 0;
				break;

			case 1:
				osd_unlocked = 0;
				menustate = MENU_NONE1;
				break;

			case 2:
				if (is_minimig())
				{
					joy_bcount = 7;
					strcpy(joy_bnames[0], "A(Red/Fire)");
					strcpy(joy_bnames[1], "B(Blue)");
					strcpy(joy_bnames[2], "C(Yellow)");
					strcpy(joy_bnames[3], "D(Green)");
					strcpy(joy_bnames[4], "RT");
					strcpy(joy_bnames[5], "LT");
					strcpy(joy_bnames[6], "Pause");
				}
				else
				{
					parse_buttons();
				}
				start_map_setting(joy_bcount ? joy_bcount+4 : 8);
				menustate = MENU_JOYDIGMAP;
				menusub = 0;
				joymap_first = 1;
				break;

			case 3:
				menustate = MENU_JOYKBDMAP;
				menusub = 0;
				break;

			case 4:
				reset_players();
				menustate = MENU_NONE1;
				break;

			case 5:
				{
					menustate = MENU_UART1;
					menusub = 0;
				}
				break;

			case 6:
				{
					menustate = MENU_VIDEOPROC1;
					menusub = 0;
				}
				break;

			case 9:
				audio_set_filter_en(audio_filter_en() ? 0 : 1);
				menustate = MENU_COMMON1;
				break;

			case 10:
				if (audio_filter_en())
				{
					snprintf(Selected_tmp, sizeof(Selected_tmp), AFILTER_DIR"/%s", audio_get_filter(0));
					if (!FileExists(Selected_tmp)) snprintf(Selected_tmp, sizeof(Selected_tmp), AFILTER_DIR);
					SelectFile(Selected_tmp, 0, SCANO_DIR | SCANO_TXT, MENU_AFILTER_FILE_SELECTED, MENU_COMMON1);
				}
				break;

			case 13:
				if (!is_archie())
				{
					menustate = MENU_RESET1;
					menusub = 1;
				}
				break;

			case 14:
				// Save settings
				menustate = MENU_GENERIC_MAIN1;
				menusub = 0;

				if (is_archie())
				{
					archie_save_config();
					menustate = MENU_ARCHIE_MAIN1;
				}
				else
				{
					char *filename = user_io_create_config_name(1);
					printf("Saving config to %s\n", filename);
					user_io_status_save(filename);
					if (is_x86() || is_pcxt()) x86_config_save();
					if (is_arcade()) arcade_nvm_save();
				}
				break;

			case 15:
				snprintf(Selected_tmp, sizeof(Selected_tmp), "%s", user_io_get_core_name());
				if (!findDocsDir(Selected_tmp, sizeof(Selected_tmp))) {
					FileCreatePath(DOCS_DIR);
					snprintf(Selected_tmp, sizeof(Selected_tmp), DOCS_DIR "/%s", user_io_get_core_name());
					FileCreatePath(Selected_tmp);
				}
				SelectFile(Selected_tmp, "PDFTXTMD ", SCANO_DIR | SCANO_TXT, MENU_DOC_FILE_SELECTED, MENU_COMMON1);
				break;

			case 16:
				menustate = MENU_ABOUT1;
				menusub = 0;
				break;

			case 17:
				{
					reboot_req = 1;

					int off = hold_cnt / 3;
					if (off > 5) reboot(1);

					sprintf(s, " Cold Reboot");
					p = s + 5 - off;
					MenuWrite(cr, p, 1, 0);
				}
				break;

			default:
				menustate = MENU_NONE1;
				break;
			}
		}
		else if (left)
		{
			// go back to core requesting this menu
			switch (user_io_core_type())
			{
			case CORE_TYPE_8BIT:
				if (is_minimig())
				{
					menusub = 0;
					menustate = MENU_MINIMIG_MAIN1;
				}
				else if (is_archie())
				{
					menusub = 0;
					menustate = MENU_ARCHIE_MAIN1;
				}
				else if (is_st())
				{
					menusub = 0;
					menustate = MENU_ST_MAIN1;
				}
				else
				{
					menusub = 0;
					menustate = MENU_GENERIC_MAIN1;
				}
				break;
			}
		}
		else if (minus || plus)
		{
			if (menusub == 10 && audio_filter_en())
			{
				const char *newfile = flist_GetPrevNext(AFILTER_DIR, audio_get_filter(0), "TXT", plus);
				audio_set_filter(newfile ? newfile : "");
				menustate = MENU_COMMON1;
			}
		}

		if(!hold_cnt && reboot_req) fpga_load_rbf("menu.rbf");
		break;

	case MENU_VIDEOPROC1:
		helptext_idx = 0;
		menumask = 0x7FFF;
		OsdSetTitle("Video Processing");
		menustate = MENU_VIDEOPROC2;
		parentstate = MENU_VIDEOPROC1;

		while (1)
		{
			int n = 0;
			if (!menusub) firstmenu = 0;
			adjvisible = 0;

			MenuWrite(n++, " Load preset", menusub == 0);
			MenuWrite(n++);

			sprintf(s, video_get_scaler_flt(VFILTER_HORZ) ?  " Horz filter: From file" : " Video filter: NearNeighbour");
			MenuWrite(n++, s, menusub == 1, cfg.direct_video);
			strcpy(s, " ");
			if (strlen(video_get_scaler_coeff(VFILTER_HORZ))) strncat(s, video_get_scaler_coeff(VFILTER_HORZ), 25);
			else strcpy(s, " < none >");
			while (strlen(s) < 26) strcat(s, " ");
			strcat(s, " \x16 ");
			MenuWrite(n++, s, menusub == 2, !video_get_scaler_flt(VFILTER_HORZ) || !S_ISDIR(getFileType(COEFF_DIR)));


			MenuWrite(n++);
			sprintf(s, " Vert filter: %s", video_get_scaler_flt(VFILTER_VERT) ? "From file" : "Same as Horz");
			MenuWrite(n++, s, menusub == 3, cfg.direct_video || !video_get_scaler_flt(VFILTER_HORZ));
			strcpy(s, " ");
			if (strlen(video_get_scaler_coeff(VFILTER_VERT))) strncat(s, video_get_scaler_coeff(VFILTER_VERT), 25);
			else strcpy(s, " < none >");
			while (strlen(s) < 26) strcat(s, " ");
			strcat(s, " \x16 ");
			MenuWrite(n++, s, menusub == 4, !video_get_scaler_flt(VFILTER_VERT) || !video_get_scaler_flt(VFILTER_HORZ) || !S_ISDIR(getFileType(COEFF_DIR)) || cfg.direct_video);

			MenuWrite(n++);
			sprintf(s, " Scan filter: %s", video_get_scaler_flt(VFILTER_SCAN) ? "From file" : "Same as Vert");
			MenuWrite(n++, s, menusub == 5, cfg.direct_video || !video_get_scaler_flt(VFILTER_HORZ));
			strcpy(s, " ");
			if (strlen(video_get_scaler_coeff(VFILTER_SCAN))) strncat(s, video_get_scaler_coeff(VFILTER_SCAN), 25);
			else strcpy(s, " < none >");
			while (strlen(s) < 26) strcat(s, " ");
			strcat(s, " \x16 ");
			MenuWrite(n++, s, menusub == 6, !video_get_scaler_flt(VFILTER_SCAN) || !video_get_scaler_flt(VFILTER_HORZ) || !S_ISDIR(getFileType(COEFF_DIR)) || cfg.direct_video);

			MenuWrite(n++);
			sprintf(s, " Intl filter: %s", video_get_scaler_flt(VFILTER_ILACE) ? "From file" : "Same as Horz");
			MenuWrite(n++, s, menusub == 7, cfg.direct_video || !video_get_scaler_flt(VFILTER_HORZ));
			strcpy(s, " ");
			if (strlen(video_get_scaler_coeff(VFILTER_ILACE))) strncat(s, video_get_scaler_coeff(VFILTER_ILACE), 25);
			else strcpy(s, " < none >");
			while (strlen(s) < 26) strcat(s, " ");
			strcat(s, " \x16 ");
			MenuWrite(n++, s, menusub == 8, !video_get_scaler_flt(VFILTER_ILACE) || !video_get_scaler_flt(VFILTER_HORZ) || !S_ISDIR(getFileType(COEFF_DIR)) || cfg.direct_video);


			MenuWrite(n++);
			sprintf(s, " Gamma correction - %s", (video_get_gamma_en() > 0) ? "On" : "Off");
			MenuWrite(n++, s, menusub == 9, video_get_gamma_en() < 0);
			strcpy(s, " ");
			if (strlen(video_get_gamma_curve())) strncat(s, video_get_gamma_curve(), 25);
			else strcpy(s, " < none >");
			while (strlen(s) < 26) strcat(s, " ");
			strcat(s, " \x16 ");
			MenuWrite(n++, s, menusub == 10, (video_get_gamma_en() <= 0) || !S_ISDIR(getFileType(GAMMA_DIR)));

			MenuWrite(n++);
			sprintf(s, " Shadow Mask - %s", (video_get_shadow_mask_mode() < 0) ? config_smask_msg[0] : config_smask_msg[video_get_shadow_mask_mode()]);
			MenuWrite(n++, s, menusub == 11, video_get_shadow_mask_mode() < 0);
			strcpy(s, " ");
			if (strlen(video_get_shadow_mask())) strncat(s, video_get_shadow_mask(), 25);
			else strcpy(s, " < none >");
			while (strlen(s) < 26) strcat(s, " ");
			strcat(s, " \x16 ");
			MenuWrite(n++, s, menusub == 12, (video_get_shadow_mask_mode() <= 0) || !S_ISDIR(getFileType(SMASK_DIR)));

			MenuWrite(n++);
			MenuWrite(n++, " Reset to Defaults", menusub == 13);

			MenuWrite(n++);
			MenuWrite(n++, STD_BACK, menusub == 14);

			if (!adjvisible) break;
			firstmenu += adjvisible;
		}
		break;

	case MENU_VIDEOPROC2:
		if (menu || left)
		{
			menusub = 8;
			menustate = MENU_COMMON1;
			break;
		}

		if ((select || recent) && menusub == 0)
		{
			fs_Options = SCANO_DIR | SCANO_TXT;
			fs_MenuSelect = MENU_PRESET_FILE_SELECTED;
			fs_MenuCancel = parentstate;
			strcpy(fs_pFileExt, "INI");
			if (!FileExists(Selected_F[15])) snprintf(Selected_F[15], sizeof(Selected_F[15]), PRESET_DIR);
			if (select) SelectFile(Selected_F[15], fs_pFileExt, fs_Options, fs_MenuSelect, fs_MenuCancel, 15);
			else if (recent_init(15)) menustate = MENU_RECENT1;
			break;
		}

		if (plus || minus)
		{
			if (menusub == 9)
			{
				video_set_shadow_mask_mode(video_get_shadow_mask_mode() + (plus ? 1 : -1));
			}

			switch (menusub)
			{
			case 2:
			case 4:
			case 6:
			case 8:
				vfilter_type = (menusub == 2) ? VFILTER_HORZ : (menusub == 4) ? VFILTER_VERT : (menusub == 6) ? VFILTER_SCAN : VFILTER_ILACE;
				if(video_get_scaler_flt(VFILTER_HORZ) && video_get_scaler_flt(vfilter_type))
				{
					const char *newfile = flist_GetPrevNext(COEFF_DIR, video_get_scaler_coeff(vfilter_type, 0), "TXT", plus);
					video_set_scaler_coeff(vfilter_type, newfile ? newfile : "");
				}
				break;

			case 10:
				if(video_get_gamma_en() > 0)
				{
					const char *newfile = flist_GetPrevNext(GAMMA_DIR, video_get_gamma_curve(0), "TXT", plus);
					video_set_gamma_curve(newfile ? newfile : "");
				}
				break;

			case 12:
				if (video_get_shadow_mask_mode() > 0)
				{
					const char *newfile = flist_GetPrevNext(SMASK_DIR, video_get_shadow_mask(0), "TXT", plus);
					video_set_shadow_mask(newfile ? newfile : "");
				}
				break;
			}

			menustate = parentstate;
			break;
		}

		if (select)
		{
			switch (menusub)
			{
			case 1:
				if (!cfg.direct_video)
				{
					video_set_scaler_flt(VFILTER_HORZ, video_get_scaler_flt(VFILTER_HORZ) ? 0 : 1);
					menustate = parentstate;
				}
				break;

			case 2:
			case 4:
			case 6:
			case 8:
				vfilter_type = (menusub == 2) ? VFILTER_HORZ : (menusub == 4) ? VFILTER_VERT : (menusub == 6) ? VFILTER_SCAN : VFILTER_ILACE;
				if (video_get_scaler_flt(VFILTER_HORZ))
				{
					snprintf(Selected_tmp, sizeof(Selected_tmp), COEFF_DIR"/%s", video_get_scaler_coeff(vfilter_type, 0));
					if (!FileExists(Selected_tmp)) snprintf(Selected_tmp, sizeof(Selected_tmp), COEFF_DIR);
					SelectFile(Selected_tmp, 0, SCANO_DIR | SCANO_TXT, MENU_COEFF_FILE_SELECTED, parentstate);
				}
				break;

			case 3:
			case 5:
			case 7:
				if (!cfg.direct_video && video_get_scaler_flt(VFILTER_HORZ))
				{
					vfilter_type = (menusub == 3) ? VFILTER_VERT : (menusub == 5) ? VFILTER_SCAN : VFILTER_ILACE;
					video_set_scaler_flt(vfilter_type, video_get_scaler_flt(vfilter_type) ? 0 : 1);
					menustate = parentstate;
				}
				break;

			case 9:
				if (video_get_gamma_en() >= 0) video_set_gamma_en(video_get_gamma_en() ? 0 : 1);
				menustate = parentstate;
				break;

			case 10:
				if (video_get_gamma_en() > 0)
				{
					snprintf(Selected_tmp, sizeof(Selected_tmp), GAMMA_DIR"/%s", video_get_gamma_curve(0));
					if (!FileExists(Selected_tmp)) snprintf(Selected_tmp, sizeof(Selected_tmp), GAMMA_DIR);
					SelectFile(Selected_tmp, 0, SCANO_DIR | SCANO_TXT, MENU_GAMMA_FILE_SELECTED, parentstate);
				}
				break;

			case 11:
				if (video_get_shadow_mask_mode() >= 0) video_set_shadow_mask_mode(video_get_shadow_mask_mode() + 1);
				menustate = parentstate;
				break;

			case 12:
				if (video_get_shadow_mask_mode() > 0)
				{
					snprintf(Selected_tmp, sizeof(Selected_tmp), SMASK_DIR"/%s", video_get_shadow_mask(0));
					if (!FileExists(Selected_tmp)) snprintf(Selected_tmp, sizeof(Selected_tmp), SMASK_DIR);
					SelectFile(Selected_tmp, 0, SCANO_DIR | SCANO_TXT, MENU_SMASK_FILE_SELECTED, parentstate);
				}
				break;

			case 13:
				video_cfg_reset();
				menustate = parentstate;
				break;

			case 14:
				menusub = 6;
				menustate = MENU_COMMON1;
				break;
			}
		}
		break;

	case MENU_DOC_FILE_SELECTED:
		if (cfg.fb_terminal)
		{
			memcpy(Selected_tmp, selPath, sizeof(Selected_tmp));
			static char cmd[1024 * 2];
			const char *path = getFullPath(selPath);
			menustate = MENU_DOC_FILE_SELECTED_2;
			video_chvt(2);
			video_fb_enable(1);
			vga_nag();
			// check file type
			const char *ext = "";
                        if (strlen(path) > 4) ext = path + strlen(path) - 4;
			static char binary[1024*2];
			printf("extension: [%s]\n",ext);
			strcpy(binary,"/media/fat/linux/pdfviewer --cache_size=1");
			if (!strcasecmp(ext,".pdf")) {
				sprintf(binary,"/media/fat/linux/pdfviewer --zoom_to_fit --cache_size=1 \"%s\"",path);
			} else if (!strcasecmp(ext,".txt")) {
				sprintf(binary,"less \"%s\"",path);
			} else if (!strcasecmp(ext+1,".md")) {
				sprintf(binary,"/media/fat/linux/glow --style dark  \"%s\" | less -R",path);
			}

			sprintf(cmd, "#!/bin/bash\nexport LC_ALL=en_US.UTF-8\nexport HOME=/root\nexport LESSKEY=/media/fat/linux/lesskey\n cd $(dirname \"%s\")\n%s \nEXITSTATUS=$? \necho \"Press any key to continue\"\nexit $EXITSTATUS\n", path, binary  );
			printf("CMD [%s]\n",cmd);
			unlink("/tmp/script");
			FileSave("/tmp/script", cmd, strlen(cmd));
			ttypid = fork();
			ttystatus = 0;
			if (!ttypid)
			{
				cpu_set_t set;
				CPU_ZERO(&set);
				CPU_SET(0, &set);
				sched_setaffinity(0, sizeof(set), &set);
				setsid();
				execl("/sbin/agetty", "/sbin/agetty",  "-a", "root", "-l", "/tmp/script", "--nohostname", "-L", "tty2", "linux", NULL);
				exit(1); //should never be reached
			}
		} else {
			menustate = MENU_DOC_NO_FBTERM;
		}

		break;

	case MENU_DOC_FILE_SELECTED_2:
		if (ttypid)
		{
			if (waitpid(ttypid, &ttystatus, WNOHANG) > 0)
			{
				ttypid = 0;
				user_io_osd_key_enable(1);
			}
		}
		else
		{
			if ((WIFEXITED(ttystatus) && !WEXITSTATUS(ttystatus)) || (c & UPSTROKE))
			{
				video_menu_bg(user_io_status_get("[3:1]"));
				video_fb_enable(0);
				menustate = MENU_NONE1;
				menusub = 3;
				OsdClear();
				OsdEnable(DISABLE_KEYBOARD);
			}
		}
		break;

		case MENU_DOC_NO_FBTERM:
			{
				int n = 0;
				menustate = MENU_DOC_NO_FBTERM2;
				OsdSetSize(18);
				OsdSetTitle("Help");
				OsdWrite(n++, "");
				OsdWrite(n++, "");
				OsdWrite(n++, "");
				OsdWrite(n++, "");
				OsdWrite(n++, "");
				OsdWrite(n++, "");
				OsdWrite(n++,"        Help requires");
				OsdWrite(n++,"        fb_terminal=1");
				for (; n < OsdGetSize(); n++) OsdWrite(n);
				MenuWrite(n, STD_EXIT, true, 0);
			}
		break;

		case MENU_DOC_NO_FBTERM2:
			if (select)
			{
				menustate = MENU_NONE1;
				menusub = 3;
			}
		break;


	case MENU_ARCADE_DIP1:
		helptext_idx = 0;
		menumask = 0;
		OsdSetTitle("DIP Switches");
		menustate = MENU_ARCADE_DIP2;
		parentstate = MENU_ARCADE_DIP1;

		while (1)
		{
			int entry = 0;
			if (!menusub) firstmenu = 0;

			adjvisible = 0;
			uint32_t selentry = 0;
			menumask = 0;

			sw_struct *sw = arcade_sw();

			int n = (sw->dip_num < OsdGetSize() - 1) ? (OsdGetSize() - 1 - sw->dip_num) / 2 : 0;
			for (; entry < n; entry++) MenuWrite(entry);

			for (int i = 0; i < sw->dip_num; i++)
			{
				uint64_t status = sw->dip_cur & sw->dip[i].mask;
				int m = 0;
				for (int n = 0; n < sw->dip[i].num; n++)
				{
					if (status == sw->dip[i].val[n])
					{
						m = n;
						break;
					}
				}

				char l = strlen(sw->dip[i].id[m]);
				s[0] = ' ';
				strcpy(s + 1, sw->dip[i].name);

				char *end = s + strlen(s) - 1;
				while ((end > s + 1) && (*end == ' ')) end--;
				*(end + 1) = 0;

				strcat(s, ":");
				l = 28 - l - strlen(s);
				while (l--) strcat(s, " ");

				strcat(s, sw->dip[i].id[m]);

				MenuWrite(entry, s, menusub == selentry);

				menumask = (menumask << 1) | 1;
				entry++;
				selentry++;
			};

			for (; entry < OsdGetSize() - 1; entry++) MenuWrite(entry, "", 0, 0);

			MenuWrite(entry, "       Reset to apply", menusub == selentry);
			menusub_last = selentry;
			menumask = (menumask << 1) | 1;

			if (!adjvisible) break;
			firstmenu += adjvisible;
		}
		break;

	case MENU_ARCADE_DIP2:
		if (menu || left)
		{
			menustate = MENU_GENERIC_MAIN1;
			menusub = dip_submenu;
			arcade_sw_save();
		}

		if (select)
		{
			if (menusub == menusub_last)
			{
				arcade_sw_save();
				user_io_status_set("[0]", 1);
				user_io_status_set("[0]", 0);
				menustate = MENU_NONE1;
			}
			else
			{
				sw_struct *sw = arcade_sw();
				uint64_t status = sw->dip_cur & sw->dip[menusub].mask;
				int m = 0;
				for (int n = 0; n < sw->dip[menusub].num; n++)
				{
					if (status == sw->dip[menusub].val[n])
					{
						m = n;
						break;
					}
				}

				m = (m + 1) % sw->dip[menusub].num;
				sw->dip_cur = (sw->dip_cur & ~sw->dip[menusub].mask) | sw->dip[menusub].val[m];
				menustate = MENU_ARCADE_DIP1;
				arcade_sw_send();
			}
		}
		break;

	case MENU_UART1:
		{
			helptext_idx = 0;
			menumask = 0x181;

			OsdSetTitle("UART Mode");
			menustate = MENU_UART2;
			parentstate = MENU_UART1;

			int mode = GetUARTMode();
			int midilink = GetMidiLinkMode();

			m = 0;
			OsdWrite(m++);
			sprintf(s, " Connection:      %s", config_uart_msg[mode]);
			OsdWrite(m++, s, menusub == 0, 0);

			OsdWrite(m++);
			if (mode == 4)
			{
				sprintf(s, " Link:            %s", (midilink == 6) ? "USB Serial" : (midilink == 5) ? "       UDP" : "       TCP");
				OsdWrite(m++, s, menusub == 1);
				menumask |= 2;
			}

			if (mode == 3)
			{
				sprintf(s, " MidiLink:             %s", config_midilink_mode[midilink]);
				OsdWrite(m++, s, menusub == 2);

				if (midilink < 2)
				{
					sprintf(s, " Type:                %s", midilink ? "  MUNT" : "FSYNTH");
					OsdWrite(m++, s, menusub == 3);

					OsdWrite(m++);
					OsdWrite(m++, " Change Soundfont          \x16", menusub == 4, midilink);
					menumask |= 0x18;
				}
				OsdWrite(m++);

				menumask |= 0x4;
			}

			if (mode)
			{
				strcpy(s, " Baud                      \x16");
				sprintf(s + 6, "(%s)", GetUARTbaud_label(GetUARTMode()));
				s[strlen(s)] = ' ';
				OsdWrite(m++, s, menusub == 5, !mode);

				OsdWrite(m++);
				OsdWrite(m++, " Reset UART connection", menusub == 6, mode ? 0 : 1);

				menumask |= 0x60;
			}

			OsdWrite(m++, " Save", menusub == 7);

			for (; m < 15; m++) OsdWrite(m);
			OsdWrite(15, STD_EXIT, menusub == 8);
		}
		break;

	case MENU_UART2:
		if (menu || left)
		{
			menustate = MENU_COMMON1;
			menusub = 5;
			break;
		}

		if (select || minus || plus)
		{
			switch (menusub)
			{
			case 0:
				menusub = GetUARTMode();
				menustate = MENU_UART3;
				break;

			case 1:
				{
					int mode = GetUARTMode();
					int midilink = GetMidiLinkMode();
					SetUARTMode(0);
					if (minus)
					{
						if (midilink <= 4) midilink = 6;
						else midilink--;
					}
					else
					{
						if (midilink >= 6) midilink = 4;
						else midilink++;
					}
					SetMidiLinkMode(midilink);
					SetUARTMode(mode);
					menustate = MENU_UART1;
				}
				break;

			case 2:
				{
					int mode = GetUARTMode();
					int midilink = GetMidiLinkMode();
					SetUARTMode(0);

					if (minus)
					{
						if (midilink < 2 || midilink > 3) midilink = 3;
						else if (midilink == 3)
						{
							struct stat filestat;
							midilink = (!stat("/dev/midi1", &filestat) || !stat("/dev/ttyUSB0", &filestat)) ? 2 : 0;
						}
						else midilink = 0;
					}
					else
					{
						if (midilink < 2)
						{
							struct stat filestat;
							midilink = (!stat("/dev/midi1", &filestat) || !stat("/dev/ttyUSB0", &filestat)) ? 2 : 3;
						}
						else if (midilink == 2) midilink = 3;
						else midilink = 0;
					}

					SetMidiLinkMode(midilink);
					SetUARTMode(mode);
					menustate = MENU_UART1;
				}
				break;

			case 3:
				{
					int mode = GetUARTMode();
					int midilink = GetMidiLinkMode();
					SetUARTMode(0);
					SetMidiLinkMode(midilink ? 0 : 1);
					SetUARTMode(mode);
					menustate = MENU_UART1;
				}
				break;

			case 4:
				if(select && GetMidiLinkMode() == 0)
				{
					sprintf(Selected_tmp, GetMidiLinkSoundfont());
					SelectFile(Selected_tmp, "SF2", SCANO_DIR | SCANO_TXT, MENU_SFONT_FILE_SELECTED, MENU_UART1);
				}
				break;

			case 5:
				if (select)
				{
					menustate = MENU_BAUD1;
					menusub = GetUARTbaud_idx(GetUARTMode());
				}
				break;

			case 6:
				if (select)
				{
					ResetUART();
					menustate = MENU_COMMON1;
					menusub = 5;
				}
				break;

			case 7:
				if (select)
				{
					int mode = GetUARTMode() | (GetMidiLinkMode() << 8);
					sprintf(s, "uartmode.%s", user_io_get_core_name());
					FileSaveConfig(s, &mode, 4);
					uint32_t speeds[3];
					speeds[0] = GetUARTbaud(1);
					speeds[1] = GetUARTbaud(3);
					speeds[2] = GetUARTbaud(4);
					sprintf(s, "uartspeed.%s", user_io_get_core_name());
					FileSaveConfig(s, speeds, sizeof(speeds));
					menustate = MENU_COMMON1;
					menusub = 5;
				}
				break;

			default:
				menustate = MENU_NONE1;
				break;
			}
		}
		break;

	case MENU_UART3:
        {
            helptext_idx = 0;
            menumask = 0x00;
            OsdSetTitle("UART MODE");
            menustate = MENU_UART4;
            parentstate = MENU_UART3;

            uint32_t max = (sizeof(config_uart_msg) / sizeof(config_uart_msg[0]));
			m = 0;

			int mode = GetUARTMode();

			// UDP uartmode is not selectable through the menu
			bool udp_enabled = mode == 5;
			// SNI is only selectable if playing SNES and snid is present
			bool sni_enabled = (mode == 6) || (is_snes() && FileExists("/media/fat/snid"));

			uint32_t skipped = !udp_enabled + !sni_enabled;;

            for (uint32_t i = 0; i < 15; i++)
            {
				// Skip drawing unselectable entries
				while ((!udp_enabled && m == 5) || (!sni_enabled && m == 6))
				{
					m++;
				}
				if((i >= (14-(max-skipped))/2) && (m < max))
                {
                    menumask |= 1 << m;
                    const char * uart_msg = config_uart_msg[m];
                    while (*uart_msg == ' ') {uart_msg++;}//skip spaces
                    sprintf(s, "         %s", uart_msg);
                    OsdWrite(i, s, menusub == m, 0);
					m++;
                }
				else
				{
					OsdWrite(i);
				}
            }
            menumask |= 0x10000;
            OsdWrite(15, STD_EXIT, menusub == 16);
        }
        break;

    case MENU_UART4:
        {
			if (menu)
			{
				menustate = MENU_UART1;
				menusub = 0;
			}
			else if (select)
			{
				if (menusub != 16)
				{
					uint32_t max = (sizeof(config_uart_msg) / sizeof(config_uart_msg[0]));
					if (menusub < max)
					{
						int midilink = GetMidiLinkMode();
						if (menusub == 4) midilink = (midilink == 2) ? 6 : 4;
						if (menusub == 3) midilink = (midilink == 6) ? 2 : (midilink > 3) ? 0 : midilink;
						SetMidiLinkMode(midilink);
						SetUARTMode(menusub);
					}
				}
				menustate = MENU_UART1;
				menusub = 0;
			}
		}
		break;

	case MENU_SFONT_FILE_SELECTED:
		{
			printf("MENU_SFONT_FILE_SELECTED --> '%s'\n", selPath);
			snprintf(Selected_tmp, sizeof(Selected_tmp), "/sbin/mlinkutil FSSFONT /media/fat/\"%s\"", selPath);
			system(Selected_tmp);
			AdjustDirectory(selPath);
			// MENU_FILE_SELECT1 to file select OSD
			menustate = MENU_UART1; //MENU_FILE_SELECT1;
		}
		break;

	case MENU_BAUD1:
		{
			helptext_idx = 0;
			OsdSetTitle("UART Baud Rate");
			menustate = MENU_BAUD2;
			parentstate = MENU_BAUD1;

			m = 0;
			menumask = 0;
			int mode = GetUARTMode();
			const uint32_t *bauds = GetUARTbauds(mode);
			for (uint32_t i = 0; i < 13; i++)
			{
				if (!bauds[i]) break;
				menumask |= 1 << i;
				m = i;
			}

			uint32_t start = (16 - m)/2;
			uint32_t k = 0;
			while (k < start) OsdWrite(k++);

			for (uint32_t i = 0; i < 13; i++)
			{
				if (!bauds[i]) break;

				sprintf(s, " %s", GetUARTbaud_label(mode, i));
				OsdWrite(k++, s, menusub == i, 0);
			}

			while (k < 15) OsdWrite(k++);
			m++;
			menumask |= 1 << m;
			OsdWrite(15, STD_EXIT, menusub == m);
		}
		break;

	case MENU_BAUD2:
		{
			if (menu)
			{
				menustate = MENU_UART1;
				menusub = 5;
				break;
			}
			else if (select)
			{
				const uint32_t *bauds = GetUARTbauds(GetUARTMode());
				for (uint32_t i = 0; i < 13; i++)
				{
					if (!bauds[i]) break;
					if (menusub == i)
					{
						ValidateUARTbaud(GetUARTMode(), bauds[i]);
						if (GetUARTMode() >= 3)
						{
							sprintf(s, "/sbin/mlinkutil BAUD %d", GetUARTbaud(GetUARTMode()));
							system(s);
						}
						else
						{
							int mode = GetUARTMode();
							SetUARTMode(0);
							SetUARTMode(mode);
						}
					}
				}
				menusub = 5;
				menustate = MENU_UART1;
			}
		}
		break;

	case MENU_AFILTER_FILE_SELECTED:
		{
			char *p = strcasestr(selPath, AFILTER_DIR"/");
			if (!p) audio_set_filter(selPath);
			else
			{
				p += strlen(AFILTER_DIR);
				while (*p == '/') p++;
				audio_set_filter(p);
			}
			menustate = MENU_COMMON1;
		}
		break;

	case MENU_COEFF_FILE_SELECTED:
		{
			char *p = strcasestr(selPath, COEFF_DIR"/");
			if (!p) video_set_scaler_coeff(vfilter_type, selPath);
			else
			{
				p += strlen(COEFF_DIR);
				while (*p == '/') p++;
				video_set_scaler_coeff(vfilter_type, p);
			}
			menustate = MENU_VIDEOPROC1;
		}
		break;

	case MENU_GAMMA_FILE_SELECTED:
		{
			char *p = strcasestr(selPath, GAMMA_DIR"/");
			if (!p) video_set_gamma_curve(selPath);
			else
			{
				p += strlen(GAMMA_DIR);
				while (*p == '/') p++;
				video_set_gamma_curve(p);
			}
			menustate = MENU_VIDEOPROC1;
		}
		break;

	case MENU_SMASK_FILE_SELECTED:
		{
			char *p = strcasestr(selPath, SMASK_DIR"/");
			if (!p) video_set_shadow_mask(selPath);
			else
			{
				p += strlen(SMASK_DIR);
				while (*p == '/') p++;
				video_set_shadow_mask(p);
			}
			menustate = MENU_VIDEOPROC1;
		}
		break;

	case MENU_PRESET_FILE_SELECTED:
		memcpy(Selected_F[15], selPath, sizeof(Selected_F[15]));
		recent_update(SelectedDir, selPath, SelectedLabel, 15);
		video_loadPreset(selPath, true);
		menustate = MENU_VIDEOPROC1;
		break;

	case MENU_MISC1:
		OsdSetSize(16);
		helptext_idx = 0;
		menumask = 0xF;
		menustate = MENU_MISC2;
		sysinfo_timer = 0; // force refresh
		OsdSetTitle("Misc. Options", OSD_ARROW_RIGHT);

		if (parentstate != MENU_MISC1)
		{
			for (int i = 0; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
			flag = 1;
			for (int i = 1; i < 4; i++) if (FileExists(cfg_get_name(i))) flag |= 1 << i;
			flag |= altcfg() << 4;
			menusub = 3;
		}
		parentstate = MENU_MISC1;

		OsdWrite(1, "         Information");

		if (menusub != 0) flag = (flag & 0xF) | (altcfg() << 4);
		strcpy(s, " Config:");
		m = 0;
		for (int i = 0; i < 4; i++)
		{
			int en = flag & (1 << i);
			if (i == (flag >> 4) && en) strcat(s, "\xc");
			strcat(s, " ");
			if (m) strcat(s, "\xc ");
			m = (i == (flag >> 4) && en);
			if (!en) strcat(s, "\xb");
			strcat(s, cfg_get_label(i));
			if (!en) strcat(s, "\xb");
		}
		strcat(s, " ");
		if (m) strcat(s, "\xc");
		OsdWrite(10, s, menusub == 0);

		m = get_core_volume();
		{
			strcpy(s, "     Core Volume: ");
			if (audio_filter_en() >= 0) s[4] = 0x1b;
			memset(s + strlen(s), 0, 11);
			char *bar = s + strlen(s);
			memset(bar, 0x8C, 8);
			memset(bar, 0x7f, 8 - (m & 7));
			if (m & 0x60) bar[8] = '+';
			if (m & 0x40) bar[9] = '+';
		}
		OsdWrite(12, s, menusub == 1);

		old_volume = get_volume();
		strcpy(s, "   Global Volume: ");
		if (old_volume & 0x10)
		{
			strcat(s, "< Mute >");
		}
		else
		{
			memset(s+strlen(s), 0, 10);
			char *bar = s + strlen(s);
			int vol = (audio_filter_en() < 0) ? get_core_volume() : 0;
			memset(bar, 0x8C, 8 - vol);
			memset(bar, 0x7f, 8 - vol - old_volume);
		}
		OsdWrite(13, s, menusub == 2);

		OsdWrite(15, STD_EXIT, menusub == 3, 0, OSD_ARROW_RIGHT);
		break;

	case MENU_MISC2:
		printSysInfo();
		if ((select && menusub == 3) || menu)
		{
			menustate = MENU_NONE1;
			break;
		}
		else if (menusub == 0 && (right || left || minus || plus || select))
		{
			uint8_t i = flag >> 4;
			if (select)
			{
				user_io_set_ini(i);
			}
			else
			{
				do
				{
					if (right || plus) i = (i + 1) & 3;
					else i = (i - 1) & 3;
				} while (!(flag & (1 << i)));

				flag = (flag & 0xF) | (i << 4);
			}
			menustate = MENU_MISC1;
		}
		else if(menusub == 1 && (right || left || minus || plus))
		{
			set_core_volume((right || plus) ? 1 : -1);
			menustate = MENU_MISC1;
		}
		else if (menusub == 2 && (right || left || minus || plus || select))
		{
			set_volume((right || plus) ? 1 : (left || minus) ? -1 : 0);
			menustate = MENU_MISC1;
		}
		else if (right)
		{
			// go back to core requesting this menu
			switch (user_io_core_type())
			{
			case CORE_TYPE_8BIT:
				if (is_menu())
				{
					menusub = 4;
					menustate = MENU_SYSTEM1;
				}
				else if (is_minimig())
				{
					menusub = 0;
					menustate = MENU_MINIMIG_MAIN1;
				}
				else if (is_archie())
				{
					menusub = 0;
					menustate = MENU_ARCHIE_MAIN1;
				}
				else if (is_st())
				{
					menusub = 0;
					menustate = MENU_ST_MAIN1;
				}
				else
				{
					menusub = 0;
					menustate = MENU_GENERIC_MAIN1;
				}
				break;
			}
		}

		m = get_volume();
		if (old_volume != m)
		{
			old_volume = m;
			menustate = MENU_MISC1;
		}
		break;

	case MENU_JOYRESET:
		OsdWrite(3);
		OsdWrite(4, "       Reset to default");
		OsdWrite(5);
		menustate = MENU_JOYRESET1;
		break;

	case MENU_JOYRESET1:
		if (!user_io_user_button())
		{
			finish_map_setting(2);
			menustate = MENU_COMMON1;
			menusub = 1;
		}
		break;

	case MENU_JOYDIGMAP:
		helptext_idx = 0;
		menumask = 1;
		OsdSetTitle("Define buttons", 0);
		menustate = MENU_JOYDIGMAP1;
		parentstate = MENU_JOYDIGMAP;
		flash_timer = 0;
		flash_state = 0;
		for (int i = 0; i < OsdGetSize(); i++) OsdWrite(i);
		if (is_menu())
		{
			OsdWrite(8, "          Esc \x16 Cancel");
			OsdWrite(9, "        Enter \x16 Finish");
		}
		else
		{
			OsdWrite(8, "    Menu-hold \x16 Cancel");
			OsdWrite(9, "        Enter \x16 Finish");
		}
		break;

	case MENU_JOYDIGMAP1:
		{
			int line_info = 0;
			if (get_map_clear())
			{
				OsdWrite(3);
				OsdWrite(4, "          Clearing");
				OsdWrite(5);
				joymap_first = 1;
				break;
			}

			if (get_map_cancel())
			{
				OsdWrite(3);
				OsdWrite(4, "          Canceling");
				OsdWrite(5);
				break;
			}

			if (is_menu() && !get_map_button()) OsdWrite(7);

			const char* p = 0;
			if (get_map_button() < 0)
			{
				strcpy(s, joy_ana_map[get_map_button() + 6]);
				OsdWrite(7, "   Space/User \x16 Skip");
			}
			else if (get_map_button() < DPAD_NAMES)
			{
				p = joy_button_map[get_map_button()];
			}
			else if (joy_bcount)
			{
				p = joy_bnames[get_map_button() - DPAD_NAMES];
				if (is_menu())
				{
					if (!get_map_type()) joy_bcount = 17;
					if (get_map_button() == SYS_BTN_OSD_KTGL)
					{
						p = joy_button_map[DPAD_BUTTON_NAMES + get_map_type()];
						if (get_map_type())
						{
							OsdWrite(12, "   (can use 2-button combo)");
							line_info = 1;
						}
					}
				}
			}
			else
			{
				p = (get_map_button() < DPAD_BUTTON_NAMES) ? joy_button_map[get_map_button()] : joy_button_map[DPAD_BUTTON_NAMES + get_map_type()];
			}

			if (get_map_button() >= 0)
			{
				if (is_menu() && get_map_button() > SYS_BTN_CNT_ESC)
				{
					strcpy(s, joy_button_map[(get_map_button() - SYS_BTN_CNT_ESC - 1) + DPAD_BUTTON_NAMES + 2]);
				}
				else
				{
					s[0] = 0;
					int len = (30 - (strlen(p) + 7)) / 2;
					while (len > 0)
					{
						strcat(s, " ");
						len--;
					}

					strcat(s, "Press: ");
					strcat(s, p);
				}
			}

			OsdWrite(3, s, 0, 0);
			OsdWrite(4);

			if(is_menu() && joy_bcount && get_map_button() >= SYS_BTN_RIGHT && get_map_button() <= SYS_BTN_START)
			{
				// draw an on-screen gamepad to help with central button mapping
				if (!flash_timer || CheckTimer(flash_timer))
				{
					flash_timer = GetTimer(100);
					if (flash_state)
					{
						switch (get_map_button())
						{
							case SYS_BTN_L:      OsdWrite(10, "  \x86   \x88               \x86 R \x88  "); break;
							case SYS_BTN_R:      OsdWrite(10, "  \x86 L \x88               \x86   \x88  "); break;
							case SYS_BTN_UP:     OsdWrite(12, " \x83                     X   \x83");        break;
							case SYS_BTN_X:      OsdWrite(12, " \x83   U                     \x83");        break;
							case SYS_BTN_A:      OsdWrite(13, " \x83 L \x1b R  Sel Start  Y     \x83");     break;
							case SYS_BTN_Y:      OsdWrite(13, " \x83 L \x1b R  Sel Start      A \x83");     break;
							case SYS_BTN_LEFT:   OsdWrite(13, " \x83   \x1b R  Sel Start  Y   A \x83");     break;
							case SYS_BTN_RIGHT:  OsdWrite(13, " \x83 L \x1b    Sel Start  Y   A \x83");     break;
							case SYS_BTN_SELECT: OsdWrite(13, " \x83 L \x1b R      Start  Y   A \x83");     break;
							case SYS_BTN_START:  OsdWrite(13, " \x83 L \x1b R  Sel        Y   A \x83");     break;
							case SYS_BTN_DOWN:   OsdWrite(14, " \x83       \x86\x81\x81\x81\x81\x81\x81\x81\x81\x81\x88   B   \x83"); break;
							case SYS_BTN_B:      OsdWrite(14, " \x83   D   \x86\x81\x81\x81\x81\x81\x81\x81\x81\x81\x88       \x83"); break;
						}
					}
					else
					{
						OsdWrite(10, "  \x86 L \x88               \x86 R \x88  ");
						OsdWrite(11, " \x86\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x88");
						OsdWrite(12, " \x83   U                 X   \x83");
						OsdWrite(13, " \x83 L \x1b R  Sel Start  Y   A \x83");
						OsdWrite(14, " \x83   D   \x86\x81\x81\x81\x81\x81\x81\x81\x81\x81\x88   B   \x83");
						OsdWrite(15, " \x8b\x81\x81\x81\x81\x81\x81\x81\x8a         \x8b\x81\x81\x81\x81\x81\x81\x81\x8a");
					}
					flash_state = !flash_state;
				}
			}
			else
			{
				if(flash_timer)
				{
					//clear all gamepad gfx
					OsdWrite(10);
					OsdWrite(11);
					OsdWrite(12);
					OsdWrite(13);
					OsdWrite(14);
					OsdWrite(15);
					flash_timer = 0;
				}

				if (!line_info) OsdWrite(12);
			}

			if (get_map_vid() || get_map_pid())
			{
				if (!is_menu() && get_map_type() && !has_default_map() && !get_map_set())
				{
					for (int i = 0; i < OsdGetSize(); i++) OsdWrite(i);
					OsdWrite(6, "   You need to define this");
					OsdWrite(7, " joystick in Menu core first");
					OsdWrite(9, "      Press ESC/Enter");
					finish_map_setting(1);
					menustate = MENU_JOYDIGMAP2;
				}
				else
				{
					sprintf(s, "   %s ID: %04x:%04x", get_map_type() ? "Joystick" : "Keyboard", get_map_vid(), get_map_pid());
					if (get_map_button() > 0 || !joymap_first)
					{
						OsdWrite(7, (!get_map_type()) ? "         User \x16 Undefine" :
							is_menu() ? "   User/Space \x16 Undefine" : "    User/Menu \x16 Undefine");

						if (!get_map_type()) OsdWrite(9);
					}
					OsdWrite(5, s);
					if (!is_menu()) OsdWrite(10, "          F12 \x16 Clear all");
				}
			}

			if (!is_menu() && (get_map_button() >= (joy_bcount ? joy_bcount + 4 : 8) || (select & get_map_vid() & get_map_pid())) && joymap_first && get_map_type())
			{
				finish_map_setting(0);
				menustate = MENU_JOYDIGMAP3;
				menusub = 0;
			}
			else if (select || menu || get_map_button() >= (joy_bcount ? joy_bcount + 4 : 8))
			{
				finish_map_setting(menu);
				if (is_menu())
				{
					menustate = MENU_SYSTEM1;
					menusub = 2;
				}
				else
				{
					menustate = MENU_COMMON1;
					menusub = 2;
				}
			}
		}
		break;

	case MENU_JOYDIGMAP2:
		if (select || menu)
		{
			menustate = MENU_COMMON1;
			menusub = 2;
		}
		break;

	case MENU_JOYDIGMAP3:
		for (int i = 0; i < OsdGetSize(); i++) OsdWrite(i);
		m = 6;
		menumask = 3;
		OsdWrite(m++, "    Do you want to setup");
		OsdWrite(m++, "    alternative buttons?");
		OsdWrite(m++, "           No", menusub == 0);
		OsdWrite(m++, "           Yes", menusub == 1);
		parentstate = menustate;
		menustate = MENU_JOYDIGMAP4;
		break;

	case MENU_JOYDIGMAP4:
		if (menu)
		{
			menustate = MENU_COMMON1;
			menusub = 2;
			break;
		}
		else if (select)
		{
			switch (menusub)
			{
			case 0:
				menustate = MENU_COMMON1;
				menusub = 2;
				break;

			case 1:
				start_map_setting(joy_bcount ? joy_bcount + 4 : 8, 1);
				menustate = MENU_JOYDIGMAP;
				menusub = 0;
				joymap_first = 0;
				break;
			}
		}
		break;

	case MENU_JOYKBDMAP:
	{
		helptext_idx = 0;
		menumask = 1;
		menustate = MENU_JOYKBDMAP1;
		parentstate = MENU_JOYKBDMAP;

		memset(&abm_edit_map, 0, sizeof(abm_edit_map));
		start_map_setting(3, 0, &abm_edit_map);

		OsdSetTitle("Button/Key remap", 0);
		for (int i = 0; i < 5; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(5, info_top, 0, 0);
		infowrite(6, "Supported mapping:");
		infowrite( 7, "");
		infowrite( 8, " Button(s)/Key(s) -> Key(s)");
		infowrite( 9, "Button(s) -> Button(s)");
		infowrite(10, "");
		if (abm_edit_map.input_codes[0] && abm_edit_map.input_codes[0] <= 256)
		{
			infowrite(11, "    F12 \x16 Advanced ");
			infowrite(12, "  Esc \x16 Clear ");
			infowrite(13, "Enter \x16 Finish ");
		} else {
	    infowrite(11, "     OK-hold \x16 Advanced  ");
		  infowrite(12, "     Menu \x16 Finish ");
		  infowrite(13, "Menu-hold \x16 Clear  ");
		}
		OsdWrite(14, info_bottom, 0, 0);
		OsdWrite(OsdGetSize() - 1, "           Cancel", menusub == 0, 0);
		break;
		}

		case MENU_JOYKBDMAP1:
			{
				int map_clear = get_map_cancel();
				abm_dev_num = get_map_dev();

				if (get_map_finish() || map_clear)
				{
					OsdWrite(1);
					OsdWrite(2,  (map_clear) ? "          Clearing" : "          Finishing");
					OsdWrite(3);
					OsdWrite(OsdGetSize() - 1);
					OsdUpdate();
					finish_map_setting(map_clear);
					menustate = MENU_COMMON1;
					menusub = 3;
					sleep(1);
				} else if (get_map_advance()) {
					menustate = MENU_ADVANCED_MAP_LIST1;
					menusub = 0;
					finish_map_setting(0);
				} else if (get_map_set() == 2) {
					bool is_kbd = abm_edit_map.input_codes[0] && abm_edit_map.input_codes[0] <= 256;
					if (is_kbd)
					{
						OsdWrite(1, "     Press key(s) to map to", 0, 0);
						OsdWrite(2, "        on a keyboard", 0, 0);
					} else {
						OsdWrite(1, "   Press button(s) to map to", 0, 0);
						OsdWrite(2, "        on the same pad", 0, 0);
						OsdWrite(3, "    or key(s) on a keyboard", 0, 0);
					}
					char str[128] = {};
					build_advanced_map_code_str(abm_edit_map.output_codes, sizeof(abm_edit_map.output_codes), str, sizeof(str), 14);
					OsdWrite(is_kbd ? 3 : 4, str, 0, 0);
				} else {
					 char str[128] = {};
					 build_advanced_map_code_str(abm_edit_map.input_codes, sizeof(abm_edit_map.input_codes), str, sizeof(str), 14);
					 OsdWrite(1, "       Press button(s) ", 0, 0);
					 OsdWrite(2, "     or key(s) to change", 0, 0);
					 OsdWrite(3, str, 0, 0);
					 OsdWrite(4, "", 0, 0);
					 OsdWrite(OsdGetSize() - 1, " Esc \x16 Clear, Enter \x16 Finish", menusub == 0, 0);
				}
				break;
			}
	case MENU_ABOUT1:
		OsdSetSize(16);
		menumask = 0;
		helptext_idx = 0;
		OsdSetTitle("About", 0);
		menustate = MENU_ABOUT2;
		parentstate = MENU_ABOUT1;
		StarsInit();
		ScrollReset();
		for (int i = 5; i < OsdGetSize(); i++) OsdWrite(i, "", 0, 0);
		break;

	case MENU_ABOUT2:
		StarsUpdate();
		m = 0;
		while (m < 10) OsdDrawLogo(m++);
		OsdWrite(m++, "     www.MiSTerFPGA.org", 0, 0, 1);
		OsdWrite(m++, "", 0, 0, 1);
		sprintf(s, "       MiSTer v%s", version + 5);
		OsdWrite(m++, s, 0, 0, 1);

		s[0] = 0;
		{
			int len = strlen(OsdCoreNameGet());
			if (len > 30) len = 30;
			int sp = (30 - len) / 2;
			for (int i = 0; i < sp; i++) strcat(s, " ");
			char *s2 = s + strlen(s);
			char *s3 = OsdCoreNameGet();
			for (int i = 0; i < len; i++) *s2++ = *s3++;
			*s2++ = 0;
		}
		OsdWrite(m++, s, 0, 0, 1);
		OsdWrite(m++, "", 0, 0, 1);
		ScrollText(m++, "                                 MiSTer by Alexey Melnikov, based on MiST by Till Harbaum, Minimig by Dennis van Weeren and other projects. MiSTer hardware and software is distributed under the terms of the GNU General Public License version 3. MiSTer FPGA cores are the work of their respective authors under individual licensing. Go to www.MiSTerFPGA.org for more details.", 0, 0, 0, 0);

		if (menu | select | left)
		{
			menustate = MENU_COMMON1;
			menusub = 16;
		}
		break;

	case MENU_UNLOCK1:
		OsdSetSize(8);
		menumask = 0;
		helptext_idx = 0;
		menustate = MENU_UNLOCK2;
		parentstate = MENU_UNLOCK1;
		OsdEnable(DISABLE_KEYBOARD);
		OsdSetTitle("Locked", 0);
		for (int r = 0; r < OsdGetSize(); r++) OsdWrite(r);
		{
			if (strlen(osd_code_entry) >= strlen(cfg.osd_lock))
			{
				lock_pass_timeout = GetTimer(200);
				menustate = MENU_UNLOCK3;
			}

			int maxlen = strlen(cfg.osd_lock);
			int count = strlen(osd_code_entry);

			m = 2;
			OsdWrite(m++, "      Enter unlock code", 0, 0, 1);
			OsdWrite(m++, "", 0, 0, 1);
			OsdWrite(m++, "", 0, 0, 1);

			int i;
			for (i = 0; i < (29 - maxlen) / 2; i++) s[i] = ' ';
			for (int j = 0; j < maxlen; j++, i++) s[i] = j < count ? 0x7F : 0x8C;

			s[i] = '\0';
			OsdWrite(m++, s, 0, 0, 1);
			OsdUpdate();
		}
		break;

	case MENU_UNLOCK2:
		{
			const char *append = "";
			if (up) append = "U";
			else if (down) append = "D";
			else if (left) append = "L";
			else if (right) append = "R";
			else if (select) append = "A";
			else if (back) append = "B";
			else if (menu) menustate = MENU_NONE1;

			if (append[0])
			{
				strcat(osd_code_entry, append);
				menustate = MENU_UNLOCK1;
			}
		}
		break;

	case MENU_UNLOCK3:
		if (CheckTimer(lock_pass_timeout))
		{
			if (!strcmp(osd_code_entry, cfg.osd_lock))
			{
				osd_unlocked = 1;
				osd_lock_timer = GetTimer(cfg.osd_lock_time * 1000);
				menustate = MENU_NONE2;
				menu_key_set(KEY_F12);
			}
			else
			{
				menustate = MENU_UNLOCK1;
				osd_code_entry[0] = 0;
			}
		}
		break;

		/******************************************************************/
		/* st main menu                                                 */
		/******************************************************************/

	case MENU_ST_MAIN1:
		OsdSetSize(16);
		menumask = 0xeff;
		OsdSetTitle("AtariST", 0);
		firstmenu = 0;
		m = 0;

		for (uint32_t i = 0; i < 2; i++)
		{
			snprintf(s, 29, " %c: %s%s", 'A' + i, (tos_system_ctrl() & (TOS_CONTROL_FDC_WR_PROT_A << i)) ? "\x17" : "", tos_get_disk_name(i));
			OsdWrite(m++, s, menusub == i, 0);
		}
		strcpy(s, " Write protect:  ");
		strcat(s, config_tos_wrprot[(tos_system_ctrl() >> 6) & 3]);
		OsdWrite(m++, s, menusub == 2, 0);
		OsdWrite(m++);

		snprintf(s, 29, " Cart: %s", tos_get_cartridge_name());
		MenuWrite(m++, s, menusub == 3, !!(tos_system_ctrl() & TOS_CONTROL_DONGLE));
		MenuWrite(m++);

		snprintf(s, 29, " Joysticks swap: %s", user_io_get_joyswap() ? "Yes" : "No");
		OsdWrite(m++, s, menusub == 4);
		OsdWrite(m++);

		OsdWrite(m++, " Modify config             \x16", menusub == 5);
		OsdWrite(m++, " Load config               \x16", menusub == 6);
		OsdWrite(m++, " Save config               \x16", menusub == 7);

		if (spi_uio_cmd16(UIO_GET_OSDMASK, 0) & 1)
		{
			menumask |= 0x100;
			OsdWrite(m++, " MT32-pi                   \x16", menusub == 8);
		}

		OsdWrite(m++);
		OsdWrite(m++, " Reset", menusub == 9);
		OsdWrite(m++, " Cold Boot", menusub == 10);

		for (; m < OsdGetSize()-1; m++) OsdWrite(m);
		OsdWrite(15, STD_EXIT, menusub == 11, 0, OSD_ARROW_RIGHT | OSD_ARROW_LEFT);

		menustate = MENU_ST_MAIN2;
		parentstate = MENU_ST_MAIN1;
		break;

	case MENU_ST_MAIN2:
		// menu key closes menu
		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (right)
		{
			menustate = MENU_COMMON1;
			menusub = 0;
		}
		else if (left)
		{
			menustate = MENU_MISC1;
			menusub = 3;
		}
		else if (menusub <= 1 && (select || recent))
		{
			if (tos_disk_is_inserted(menusub))
			{
				tos_insert_disk(menusub, "");
				menustate = MENU_ST_MAIN1;
			}
			else
			{
				fs_Options = SCANO_DIR;
				fs_MenuSelect = MENU_ST_FDD_FILE_SELECTED;
				fs_MenuCancel = MENU_ST_MAIN1;
				strcpy(fs_pFileExt, "ST");
				if (select) SelectFile(Selected_F[menusub], "ST", fs_Options, fs_MenuSelect, fs_MenuCancel, menusub);
				else if (recent_init(menusub)) menustate = MENU_RECENT1;
			}
		}
		else if (select || minus || plus)
		{
			switch (menusub)
			{
			case 2:
				// remove current write protect bits and increase by one
				tos_update_sysctrl((tos_system_ctrl() & ~(TOS_CONTROL_FDC_WR_PROT_A | TOS_CONTROL_FDC_WR_PROT_B))
					| (((((tos_system_ctrl() >> 6) & 3) + (minus ? -1 : 1)) & 3) << 6));
				menustate = MENU_ST_MAIN1;
				break;

			case 3:
				if (!(tos_system_ctrl() & TOS_CONTROL_DONGLE))
				{
					if (tos_cartridge_is_inserted())
					{
						tos_load_cartridge("");
						menustate = MENU_ST_MAIN1;
					}
					else
					{
						fs_Options = SCANO_DIR;
						fs_MenuSelect = MENU_ST_SYSTEM_FILE_SELECTED;
						fs_MenuCancel = MENU_ST_MAIN1;
						strcpy(fs_pFileExt, "STC");
						if (select) SelectFile(Selected_F[menusub], fs_pFileExt, fs_Options, fs_MenuSelect, fs_MenuCancel, menusub);
						else if (recent_init(menusub)) menustate = MENU_RECENT1;
					}
				}
				break;

			case 4:
				if (select)
				{
					user_io_set_joyswap(!user_io_get_joyswap());
					menustate = MENU_ST_MAIN1;
				}
				break;

			case 5:  // System submenu
				if (select)
				{
					menustate = MENU_ST_SYSTEM1;
					need_reset = 0;
					menusub = 0;
				}
				break;

			case 6:  // Load config
				if (select)
				{
					menustate = MENU_ST_LOAD_CONFIG1;
					menusub = 0;
				}
				break;

			case 7:  // Save config
				if (select)
				{
					menustate = MENU_ST_SAVE_CONFIG1;
					menusub = 0;
				}
				break;

			case 8:
				if (select)
				{
					menustate = MENU_MT32PI_MAIN1;
					menusub = 0;
				}
				break;

			case 9:  // Reset
				if (select)
				{
					tos_reset(0);
					menustate = MENU_NONE1;
				}
				break;

			case 10:  // Cold Boot
				if (select)
				{
					tos_insert_disk(0, "");
					tos_insert_disk(1, "");
					tos_reset(1);
					menustate = MENU_NONE1;
				}
				break;

			case 11:  // Exit
				if (select)
				{
					menustate = MENU_NONE1;
				}
				break;
			}
		}
		break;

	case MENU_ST_FDD_FILE_SELECTED:
		memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
		recent_update(SelectedDir, selPath, SelectedLabel, menusub);
		tos_insert_disk(menusub, selPath);
		menustate = MENU_ST_MAIN1;
		break;

	case MENU_ST_SYSTEM1:
		menumask = 0x1fffb;
		OsdSetTitle("Config", 0);
		helptext_idx = 0;

		while (1)
		{
			if (!menusub) firstmenu = 0;
			adjvisible = 0;
			m = 0;

			for (uint32_t i = 0; i < 2; i++)
			{
				snprintf(s, 29, " HDD%d: %s", i, tos_get_disk_name(2 + i));
				MenuWrite(m++, s, menusub == i);
			}
			MenuWrite(m++);

			strcpy(s, " Memory:     ");
			strcat(s, tos_mem[(tos_system_ctrl() >> 1) & 7]);
			MenuWrite(m++, s, menusub == 3);

			snprintf(s, 29, " TOS:        %s", tos_get_image_name());
			MenuWrite(m++, s, menusub == 4);

			strcpy(s, " Chipset:    ");
			// extract  TOS_CONTROL_STE and  TOS_CONTROL_MSTE bits
			strcat(s, tos_chipset[(tos_system_ctrl() >> 23) & 3]);
			MenuWrite(m++, s, menusub == 5);
			MenuWrite(m++);

			// Blitter is always present in >= STE
			enable = (tos_system_ctrl() & (TOS_CONTROL_STE | TOS_CONTROL_MSTE)) ? 1 : 0;
			strcpy(s, " Blitter:    ");
			strcat(s, ((tos_system_ctrl() & TOS_CONTROL_BLITTER) || enable) ? "On" : "Off");
			MenuWrite(m++, s, menusub == 6, enable);

			// Viking card can only be enabled with max 8MB RAM
			enable = (tos_system_ctrl() & 0xe) <= TOS_MEMCONFIG_8M;
			strcpy(s, " Viking:     ");
			strcat(s, ((tos_system_ctrl() & TOS_CONTROL_VIKING) && enable) ? "On" : "Off");
			MenuWrite(m++, s, menusub == 7, enable ? 0 : 1);

			strcpy(s, " Aspect:     ");
			tos_set_ar(get_ar_name(tos_get_ar(), s));
			MenuWrite(m++, s, menusub == 8);

			strcpy(s, " Screen:     ");
			if (tos_system_ctrl() & TOS_CONTROL_VIDEO_COLOR) strcat(s, "Color");
			else                                             strcat(s, "Mono");
			MenuWrite(m++, s, menusub == 9);

			strcpy(s, " Mono 60Hz:  ");
			if (tos_system_ctrl() & TOS_CONTROL_MDE60) strcat(s, "On");
			else                                       strcat(s, "Off");
			MenuWrite(m++, s, menusub == 10);

			strcpy(s, " Video Crop: ");
			if (tos_system_ctrl() & TOS_CONTROL_BORDER) strcat(s, (tos_get_extctrl() & 0x400) ? "Visible 216p(5x)" : "Visible");
			else                                        strcat(s, "Full");
			MenuWrite(m++, s, menusub == 11);

			strcpy(s, " Scale:      ");
			strcat(s, config_scale[(tos_get_extctrl() >> 11) & 3]);
			MenuWrite(m++, s, menusub == 12);

			strcpy(s, " Scanlines:  ");
			strcat(s, tos_scanlines[(tos_system_ctrl() >> 20) & 3]);
			MenuWrite(m++, s, menusub == 13);
			MenuWrite(m++);

			strcpy(s, " Dongle:     ");
			if (tos_system_ctrl() & TOS_CONTROL_DONGLE) strcat(s, "Cubase");
			else                                        strcat(s, "None");
			MenuWrite(m++, s, menusub == 14);
			MenuWrite(m++);

			strcpy(s, " YM-Audio:   ");
			strcat(s, tos_stereo[(tos_system_ctrl() & TOS_CONTROL_STEREO) ? 1 : 0]);
			MenuWrite(m++, s, menusub == 15);
			MenuWrite(m++);

			MenuWrite(m++, STD_BACK, menusub == 16);

			if (!adjvisible) break;
			firstmenu += adjvisible;
		}

		parentstate = menustate;
		menustate = MENU_ST_SYSTEM2;
		break;

	case MENU_ST_SYSTEM2:
		saved_menustate = MENU_ST_SYSTEM1;

		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left)
		{
			menustate = MENU_ST_MAIN1;
			menusub = 5;
			if (need_reset)
			{
				tos_reset(1);
				saved_menustate = 0;
			}
		}
		else if (menusub <= 2 && (select || recent))
		{
			if (menusub <= 1)
			{
				fs_Options = SCANO_DIR | SCANO_UMOUNT;
				fs_MenuSelect = MENU_ST_HDD_FILE_SELECTED;
				fs_MenuCancel = MENU_ST_SYSTEM1;
				strcpy(fs_pFileExt, "VHD");
				if (select) SelectFile(Selected_S[menusub], "VHD", fs_Options, fs_MenuSelect, fs_MenuCancel, menusub + 500);
				else if (recent_init(menusub + 500)) menustate = MENU_RECENT1;
			}
		}
		else if (select || plus || minus)
		{
			switch (menusub)
			{
			case 3:
				{
					// RAM
					int mem = (tos_system_ctrl() >> 1) & 7;   // current memory config
					if (minus)
					{
						mem--;
						if (mem < 0) mem = 5;
					}
					else
					{
						mem++;
						if (mem > 5) mem = 0;
					}
					tos_update_sysctrl((tos_system_ctrl() & ~0x0e) | (mem << 1));
					need_reset = 1;
					menustate = MENU_ST_SYSTEM1;
				}
				break;

			case 4:  // TOS
				if (select) SelectFile(Selected_F[menusub], "IMG", SCANO_DIR, MENU_ST_SYSTEM_FILE_SELECTED, MENU_ST_SYSTEM1);
				break;

			case 5:
				{
					int chipset = (((tos_system_ctrl() >> 23) + (minus ? -1 : 1)) & 3);
					tos_update_sysctrl((tos_system_ctrl() & ~(TOS_CONTROL_STE | TOS_CONTROL_MSTE)) | (chipset << 23));
					menustate = MENU_ST_SYSTEM1;
				}
				break;

			case 6:
				if (!(tos_system_ctrl() & TOS_CONTROL_STE))
				{
					tos_update_sysctrl(tos_system_ctrl() ^ TOS_CONTROL_BLITTER);
					menustate = MENU_ST_SYSTEM1;
				}
				break;

			case 7:
				// viking/sm194
				tos_update_sysctrl(tos_system_ctrl() ^ TOS_CONTROL_VIKING);
				menustate = MENU_ST_SYSTEM1;
				break;

			case 8:
				tos_set_ar(next_ar(tos_get_ar(), minus));
				tos_update_sysctrl(tos_system_ctrl());
				menustate = MENU_ST_SYSTEM1;
				break;

			case 9:
				tos_update_sysctrl(tos_system_ctrl() ^ TOS_CONTROL_VIDEO_COLOR);
				menustate = MENU_ST_SYSTEM1;
				break;

			case 10:
				tos_update_sysctrl(tos_system_ctrl() ^ TOS_CONTROL_MDE60);
				menustate = MENU_ST_SYSTEM1;
				break;

			case 11:
				{
					int mode = ((tos_system_ctrl() & TOS_CONTROL_BORDER) ? 1 : 0) | ((tos_get_extctrl() & 0x400) ? 2 : 0);
					if (minus)
					{
						mode = (mode == 0) ? 3 : (mode == 3) ? 1 : 0;
					}
					else
					{
						mode = (mode == 0) ? 1 : (mode == 1) ? 3 : 0;
					}

					tos_update_sysctrl((mode & 1) ? (tos_system_ctrl() | TOS_CONTROL_BORDER) : (tos_system_ctrl() & ~TOS_CONTROL_BORDER));
					tos_set_extctrl((mode & 2) ? (tos_get_extctrl() | 0x400) : (tos_get_extctrl() & ~0x400));
					menustate = MENU_ST_SYSTEM1;
				}
				break;

			case 12:
				{
					int mode = ((tos_get_extctrl() >> 11) + (minus ? -1 : 1)) & 3;
					tos_set_extctrl((tos_get_extctrl() & ~0x1800) | (mode << 11));
					menustate = MENU_ST_SYSTEM1;
				}
				break;

			case 13:
				{
					// next scanline state
					int scan = ((tos_system_ctrl() >> 20) + (minus ? -1 : 1)) & 3;
					tos_update_sysctrl((tos_system_ctrl() & ~TOS_CONTROL_SCANLINES) | (scan << 20));
					menustate = MENU_ST_SYSTEM1;
				}
				break;

			case 14:
				tos_update_sysctrl(tos_system_ctrl() ^ TOS_CONTROL_DONGLE);
				menustate = MENU_ST_SYSTEM1;
				break;

			case 15:
				tos_update_sysctrl(tos_system_ctrl() ^ TOS_CONTROL_STEREO);
				menustate = MENU_ST_SYSTEM1;
				break;


			case 16:
				menustate = MENU_ST_MAIN1;
				menusub = 5;
				if (need_reset)
				{
					tos_reset(1);
					saved_menustate = 0;
				}
				break;
			}
		}
		break;

	case MENU_ST_HDD_FILE_SELECTED:
		printf("Insert image for disk %d\n", menusub);
		memcpy(Selected_S[menusub], selPath, sizeof(Selected_S[menusub]));
		recent_update(SelectedDir, selPath, SelectedLabel, menusub + 500);
		tos_insert_disk(menusub+2, selPath);
		menustate = MENU_ST_SYSTEM1;
		break;

	case MENU_ST_SYSTEM_FILE_SELECTED: // file successfully selected
		if (menusub == 4)
		{
			memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
			tos_upload(selPath);
			menustate = MENU_ST_SYSTEM1;
		}

		if (menusub == 3)
		{
			memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
			recent_update(SelectedDir, selPath, SelectedLabel, menusub);
			tos_load_cartridge(selPath);
			menustate = MENU_ST_MAIN1;
		}
		break;

	case MENU_ST_LOAD_CONFIG1:
		helptext_idx = 0;
		OsdSetTitle("Load Config", 0);

		if (parentstate != menustate)	// First run?
		{
			parentstate = menustate;
			menumask = 0x201;
			for (uint32_t i = 1; i < 9; i++) if (tos_config_exists(i)) menumask |= 1<<i;
		}

		m = 0;
		OsdWrite(m++);
		OsdWrite(m++, " Startup config:");
		for (uint32_t i = 0; i < 9; i++)
		{
			snprintf(s, 29, "  %s", (menumask & (1 << i)) ? tos_get_cfg_string(i) : "");
			OsdWrite(m++, s, menusub == i, !(menumask & (1<<i)));
			if (!i)
			{
				OsdWrite(m++);
				OsdWrite(m++, " Other configs:");
			}
		}

		for (; m < OsdGetSize() - 1; m++) OsdWrite(m);
		OsdWrite(15, STD_BACK, menusub == 9, 0);

		menustate = MENU_ST_LOAD_CONFIG2;
		break;

	case MENU_ST_LOAD_CONFIG2:
		if (menu || left)
		{
			menustate = MENU_ST_MAIN1;
			menusub = 6;
		}

		if (select)
		{
			if (menusub < 9)
			{
				tos_config_load(menusub);
				tos_upload(NULL);
				menustate = MENU_NONE1;
			}
			else
			{
				menustate = MENU_ST_MAIN1;
				menusub = 6;
			}
		}
		break;

	case MENU_ST_SAVE_CONFIG1:
		helptext_idx = 0;
		OsdSetTitle("Save Config", 0);

		parentstate = menustate;
		menumask = 0x3FF;

		m = 0;
		OsdWrite(m++);
		OsdWrite(m++, " Startup config:");
		for (uint32_t i = 0; i < 9; i++)
		{
			snprintf(s, 29, "  %s", tos_get_cfg_string(i));
			OsdWrite(m++, s, menusub == i, !(menumask & (1 << i)));
			if (!i)
			{
				OsdWrite(m++);
				OsdWrite(m++, " Other configs:");
			}
		}

		for (; m < OsdGetSize() - 1; m++) OsdWrite(m);
		OsdWrite(15, STD_BACK, menusub == 9, 0);

		menustate = MENU_ST_SAVE_CONFIG2;
		break;

	case MENU_ST_SAVE_CONFIG2:
		if (menu || left)
		{
			menustate = MENU_ST_MAIN1;
			menusub = 7;
		}

		if (select)
		{
			if (menusub < 9)
			{
				tos_config_save(menusub);
				menustate = MENU_NONE1;
			}
			else
			{
				menustate = MENU_ST_MAIN1;
				menusub = 7;
			}
		}
		break;


	case MENU_MT32PI_MAIN1:
		{
			parentstate = menustate;
			OsdSetTitle("MT32-pi");
			menumask = 0x7F;
			uint32_t mt32_cfg = is_minimig() ? minimig_get_extcfg() : tos_get_extctrl();

			m = 0;
			OsdWrite(m++);
			strcpy(s, " Use MT32-pi:            ");
			strcat(s, (mt32_cfg & 0x2) ? " No" : "Yes");
			OsdWrite(m++, s, menusub == 0);

			strcpy(s, " Show Info: ");
			switch ((mt32_cfg >> 8) & 3)
			{
			case 0:
				strcat(s, "              No");
				break;

			case 1:
				strcat(s, "             Yes");
				break;

			case 2:
				strcat(s, "  LCD-On(non-FB)");
				break;

			case 3:
				strcat(s, "LCD-Auto(non-FB)");
				break;
			}
			OsdWrite(m++, s, menusub == 1);

			OsdWrite(m++);
			OsdWrite(m++, " Default Config:");

			strcpy(s, " Synth:           ");
			strcat(s, (mt32_cfg & 0x4) ? "FluidSynth" : "      Munt");
			OsdWrite(m++, s, menusub == 2);

			strcpy(s, " Munt ROM:          ");
			switch ((mt32_cfg >> 3) & 3)
			{
			case 0:
				strcat(s, "MT-32 v1");
				break;

			case 1:
				strcat(s, "MT-32 v2");
				break;

			case 2:
				strcat(s, "  CM-32L");
				break;

			case 3:
				strcat(s, " Unknown");
				break;
			}
			OsdWrite(m++, s, menusub == 3);

			sprintf(s, " SoundFont:                %d", (mt32_cfg >> 5) & 7);
			OsdWrite(m++, s, menusub == 4);

			OsdWrite(m++);
			strcpy(s, " Current Config: ");
			hdmask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);
			if (((hdmask >> 1) & 3) == 1)
			{
				switch ((hdmask >> 3) & 3)
				{
				case 0:
					strcat(s, "   MT-32 v1");
					break;

				case 1:
					strcat(s, "   MT-32 v2");
					break;

				case 2:
					strcat(s, "     CM-32L");
					break;

				default:
					strcat(s, "    Unknown");
					break;
				}
			}
			else if (((hdmask >> 1) & 3) == 2)
			{
				sprintf(s + strlen(s), "SoundFont %d", (hdmask >> 3) & 7);
			}
			else
			{
				strcat(s, "    Unknown");
			}
			OsdWrite(m++, s);

			OsdWrite(m++);
			OsdWrite(m++, " Reset Hanging Notes", menusub == 5);

			while (m < 15) OsdWrite(m++);
			OsdWrite(15, STD_BACK, menusub == 6, 0);

			menustate = MENU_MT32PI_MAIN2;
		}
		break;


	case MENU_MT32PI_MAIN2:
		if (menu || back || left || (select && menusub == 6))
		{
			// is_minimig() is true for the console variant too, so without this
			// the page returned to MENU_MINIMIG_MAIN1 row 10 -- which redirects
			// to MENU_AMIGACD_MAIN1, where row 10 is Configs. A different page
			// and an unrelated row, the same mistake the Audio & Video and
			// config pages made before they were given a variant-aware return.
			if (is_amigacd())
			{
				menustate = MENU_AMIGACD_ADVANCED1;
				menusub = 1;                  // the MT32-pi settings row
			}
			else if (is_minimig())
			{
				menustate = MENU_MINIMIG_MAIN1;
				menusub = 10;
			}
			else
			{
				menustate = MENU_ST_MAIN1;
				menusub = 8;
			}
		}
		else if (select || plus || minus)
		{
			uint32_t mt32_cfg = is_minimig() ? minimig_get_extcfg() : tos_get_extctrl();

			switch (menusub)
			{
			case 0:
				mt32_cfg ^= 0x2;
				menustate = MENU_MT32PI_MAIN1;
				break;

			case 1:
				m = (mt32_cfg >> 8) & 3;
				m = (m + (minus ? -1 : 1)) & 3;
				mt32_cfg = (mt32_cfg & ~0x300) | (m<<8);
				menustate = MENU_MT32PI_MAIN1;
				break;

			case 2:
				mt32_cfg ^= 0x4;
				menustate = MENU_MT32PI_MAIN1;
				break;

			case 3:
				m = (mt32_cfg >> 3) & 3;
				if (minus)
				{
					m = (m - 1) & 3;
					if (m == 3) m = 2;
				}
				else
				{
					m = (m + 1) & 3;
					if (m == 3) m = 0;
				}
				mt32_cfg = (mt32_cfg & ~0x18) | (m << 3);
				menustate = MENU_MT32PI_MAIN1;
				break;

			case 4:
				m = (mt32_cfg >> 5) & 7;
				m = (m + (minus ? -1 : 1)) & 7;
				mt32_cfg = (mt32_cfg & ~0xE0) | (m << 5);
				menustate = MENU_MT32PI_MAIN1;
				break;

			case 5:
				if (select)
				{
					if (is_minimig()) minimig_set_extcfg(mt32_cfg | 1);
					else tos_set_extctrl(mt32_cfg | 1);
					mt32_cfg &= ~1;
					menustate = MENU_MT32PI_MAIN1;
				}
				break;
			}

			if (is_minimig()) minimig_set_extcfg(mt32_cfg);
			else tos_set_extctrl(mt32_cfg);
		}
		else if (spi_uio_cmd16(UIO_GET_OSDMASK, 0) != hdmask)
		{
			menustate = MENU_MT32PI_MAIN1;
		}
		break;

		/******************************************************************/
		/* file selection menu                                            */
		/******************************************************************/
	case MENU_FILE_SELECT1:
		helptext_idx = (fs_Options & SCANO_UMOUNT) ? HELPTEXT_EJECT : (fs_Options & SCANO_CLEAR) ? HELPTEXT_CLEAR : 0;
		OsdSetTitle((fs_Options & SCANO_CORES) ? "Cores" : "Select", 0);
		PrintDirectory(hold_cnt<2);
		menustate = MENU_FILE_SELECT2;
		if (cfg.log_file_entry && flist_nDirEntries())
		{
			//Write out paths infos for external integration
			MakeFile("/tmp/CURRENTPATH", flist_SelectedItem()->altname);
			MakeFile("/tmp/FULLPATH", selPath);
			MakeFile("/tmp/FILESELECT", "active");
		}
		break;

	case MENU_FILE_SELECT2:
		menumask = 0;

		if (c == KEY_BACKSPACE && (fs_Options & (SCANO_UMOUNT | SCANO_CLEAR)) && !strlen(filter))
		{
			for (int i = 0; i < OsdGetSize(); i++) OsdWrite(i, "", 0, 0);
			if (fs_Options & SCANO_CLEAR)
			{
				int i = (OsdGetSize() / 2) - 2;
				OsdWrite(i++, "     Clearing the option");
				OsdWrite(i++);
				OsdWrite(i++, " You have to reload the core");
				OsdWrite(i++, "    to use default value.");
				OsdUpdate();
				sleep(2);
			}
			else
			{
				OsdWrite(OsdGetSize() / 2, "    Unmounting the image", 0, 0);
				OsdUpdate();
				sleep(1);
			}
			input_poll(0);
			menu_key_set(0);
			selPath[0] = 0;
			menustate = fs_MenuSelect;
			helptext_idx = 0;
			break;
		}

		if (menu)
		{
			if (flist_nDirEntries() && flist_SelectedItem()->de.d_type != DT_DIR)
			{
				SelectedDir[0] = 0;
				if (strlen(selPath))
				{
					strcpy(SelectedDir, selPath);
					strcat(selPath, "/");
				}
				strcat(selPath, flist_SelectedItem()->de.d_name);
			}

			if (!strcasecmp(fs_pFileExt, "RBF")) selPath[0] = 0;
			menustate = fs_MenuCancel;
			helptext_idx = 0;
		}

		if (recent && recent_init(recent_idx_active(ioctl_index)))
		{
			menustate = MENU_RECENT1;
		}

		if (c == KEY_BACKSPACE)
		{
			filter[0] = 0;
			filter_typing_timer = 0;
			ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, fs_Options);
			menustate = MENU_FILE_SELECT1;
		}

		if (flist_nDirEntries())
		{
			if (!helpstate || ((flist_iSelectedEntry() - flist_iFirstEntry() + 1) < OsdGetSize())) ScrollLongName(); // scrolls file name if longer than display line

			if (c == KEY_HOME || c == KEY_TAB)
			{
				filter_typing_timer = 0;
				ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
				select = (c == KEY_TAB && flist_SelectedItem()->de.d_type == DT_DIR && !strcmp(flist_SelectedItem()->de.d_name, ".."));
			}

			if (c == KEY_END)
			{
				filter_typing_timer = 0;
				ScanDirectory(selPath, SCANF_END, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
			}

			if ((c == KEY_PAGEUP) || (c == KEY_LEFT))
			{
				filter_typing_timer = 0;
				ScanDirectory(selPath, SCANF_PREV_PAGE, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
			}

			if ((c == KEY_PAGEDOWN) || (c == KEY_RIGHT))
			{
				filter_typing_timer = 0;
				ScanDirectory(selPath, SCANF_NEXT_PAGE, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
			}

			if (down) // scroll down one entry
			{
				filter_typing_timer = 0;
				ScanDirectory(selPath, SCANF_NEXT, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
			}

			if (up) // scroll up one entry
			{
				filter_typing_timer = 0;
				ScanDirectory(selPath, SCANF_PREV, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
			}

			if (plus || minus)
			{
				ScanDirectory(selPath, plus ? SCANF_NEXT_CHAR : SCANF_PREV_CHAR, fs_pFileExt, fs_Options);
				menustate = MENU_FILE_SELECT1;
			}

			{
				char i;
				if ((i = GetASCIIKey(c)) > 1)
				{
					int filter_len = strlen(filter);
					if (CheckTimer(filter_typing_timer))
					{
						filter[0] = i;
						filter[1] = 0;

						// You need both ScanDirectory calls here: the first
						// call "clears" the filter, the second one scrolls to
						// the right place in the list
						ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, fs_Options);
						ScanDirectory(selPath, i, fs_pFileExt, fs_Options);
					}
					else if (filter_len < 255)
					{
						filter[filter_len++] = i;
						filter[filter_len] = 0;
						ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, fs_Options, NULL, filter);
					}

					filter_typing_timer = GetTimer(2000);
					printf("filter is: %s\n", filter);

					menustate = MENU_FILE_SELECT1;
				}
			}

			if (select)
			{
				static char name[256];
				char type = flist_SelectedItem()->de.d_type;
				memcpy(name, flist_SelectedItem()->de.d_name, sizeof(name));

				if ((fs_Options & SCANO_UMOUNT) && (is_megacd() || is_pce() || is_cdi() || is_neogeo() || (is_psx() && !(fs_Options & SCANO_SAVES)) || is_saturn() || is_3do()) && type == DT_DIR && strcmp(flist_SelectedItem()->de.d_name, ".."))
				{
					int len = strlen(selPath);
					strcat(selPath, "/");
					strcat(selPath, name);
					int num = ScanDirectory(selPath, SCANF_INIT, fs_pFileExt, 0);
					if (num != 1) selPath[len] = 0;
					else
					{
						type = flist_SelectedItem()->de.d_type;
						memcpy(name, flist_SelectedItem()->de.d_name, sizeof(name));
					}
				}

				if (type == DT_DIR)
				{
					changeDir(name);
					menustate = MENU_FILE_SELECT1;
				}
				else
				{
					if (flist_nDirEntries())
					{
						SelectedDir[0] = 0;
						if (strlen(selPath))
						{
							strcpy(SelectedDir, selPath);
							strcat(selPath, "/");
						}
						strcpy(SelectedLabel, flist_SelectedItem()->altname);
						if (fs_Options & SCANO_CORES)
						{
							int len = strlen(SelectedLabel);
							if (SelectedLabel[len - 4] == '.') SelectedLabel[len - 4] = 0;
							char *p = strstr(SelectedLabel, "_20");
							if (p) *p = 0;
						}
						strcat(selPath, name);
						menustate = fs_MenuSelect;
						helptext_idx = 0;
					}
				}
			}
		}

		if (c & UPSTROKE) PrintDirectory(1);
		break;

		/******************************************************************/
		/* cheats menu                                                    */
		/******************************************************************/
	case MENU_CHEATS1:
		helptext_idx = 0;
		sprintf(s, "Cheats (%d)", cheats_loaded());
		OsdSetTitle(s);
		cheats_print();
		menustate = MENU_CHEATS2;
		parentstate = menustate;
		break;

	case MENU_CHEATS2:
		menumask = 0;

		if (menu)
		{
			menustate = MENU_GENERIC_MAIN1;
			menusub = cheatsub;
			break;
		}

		cheats_scroll_name();

		if (c == KEY_HOME)
		{
			cheats_scan(SCANF_INIT);
			menustate = MENU_CHEATS1;
		}

		if (c == KEY_END)
		{
			cheats_scan(SCANF_END);
			menustate = MENU_CHEATS1;
		}

		if ((c == KEY_PAGEUP) || (c == KEY_LEFT))
		{
			cheats_scan(SCANF_PREV_PAGE);
			menustate = MENU_CHEATS1;
		}

		if ((c == KEY_PAGEDOWN) || (c == KEY_RIGHT))
		{
			cheats_scan(SCANF_NEXT_PAGE);
			menustate = MENU_CHEATS1;
		}

		if (down) // scroll down one entry
		{
			cheats_scan(SCANF_NEXT);
			menustate = MENU_CHEATS1;
		}

		if (up) // scroll up one entry
		{
			cheats_scan(SCANF_PREV);
			menustate = MENU_CHEATS1;
		}

		if (select)
		{
			cheats_toggle();
			menustate = MENU_CHEATS1;
		}
		break;

		/******************************************************************/
		/* last rom menu                                                    */
		/******************************************************************/
	case MENU_RECENT1:
		helptext_idx = 0;
		OsdSetTitle((fs_Options & SCANO_CORES) ? "Recent Cores" : "Recent Files");
		recent_print();
		menustate = MENU_RECENT2;
		parentstate = menustate;
		break;

	case MENU_RECENT2:
		menumask = 0;

		if (menu || recent)
		{
			menustate = fs_MenuCancel;
			if (is_menu()) menustate = MENU_FILE_SELECT1;
			break;
		}

		recent_scroll_name();

		if (c == KEY_HOME)
		{
			recent_scan(SCANF_INIT);
			menustate = MENU_RECENT1;
		}

		if (c == KEY_END)
		{
			recent_scan(SCANF_END);
			menustate = MENU_RECENT1;
		}

		if ((c == KEY_PAGEUP) || (c == KEY_LEFT))
		{
			recent_scan(SCANF_PREV_PAGE);
			menustate = MENU_RECENT1;
		}

		if ((c == KEY_PAGEDOWN) || (c == KEY_RIGHT))
		{
			recent_scan(SCANF_NEXT_PAGE);
			menustate = MENU_RECENT1;
		}

		if (c == KEY_BACKSPACE)
		{
			menusub_last = menusub;
			menusub = 0;
			menustate = MENU_RECENT3;
			break;
		}

		if (down) // scroll down one entry
		{
			recent_scan(SCANF_NEXT);
			menustate = MENU_RECENT1;
		}

		if (up) // scroll up one entry
		{
			recent_scan(SCANF_PREV);
			menustate = MENU_RECENT1;
		}

		if (select)
		{
			menustate = recent_select(SelectedDir, selPath, SelectedLabel) ? (enum MENU)fs_MenuSelect : MENU_RECENT1;
		}
		break;

	case MENU_RECENT3:
		menumask = 0x03;
		parentstate = menustate;
		m = 0;
		OsdWrite(m++);
		OsdWrite(m++);
		OsdWrite(m++);
		OsdWrite(m++);
		OsdWrite(m++);
		OsdWrite(m++);
		OsdWrite(m++, "        Clear the List?");
		OsdWrite(m++);
		OsdWrite(m++, "             No", menusub == 0);
		OsdWrite(m++, "             Yes", menusub == 1);
		while(m < OsdGetSize()) OsdWrite(m++);
		menustate = MENU_RECENT4;
		break;

	case MENU_RECENT4:
		if (select && menusub == 1)
		{
			for (int i = 0; i < OsdGetSize(); i++) OsdWrite(i, "", 0, 0);
			OsdWrite(OsdGetSize() / 2, "    Clearing the recents", 0, 0);
			OsdUpdate();
			sleep(1);
			// Was the same expression as the ` key above but without the
			// fs_RecentIdx arm, so a caller that named its list explicitly --
			// every AmigaCD row, presets, the ST drives -- cleared a different
			// list from the one it was showing.
			recent_clear(recent_idx_active(ioctl_index));
			menustate = fs_MenuCancel;
			menusub = menusub_last;
			if (is_menu()) menustate = MENU_FILE_SELECT1;

		}
		else if (select || menu || back)
		{
			menustate = fs_MenuCancel;
			menusub = menusub_last;
			if (is_menu()) menustate = MENU_FILE_SELECT1;
		}
		break;

		/******************************************************************/
		/* reset menu                                                     */
		/******************************************************************/
	case MENU_RESET1:
		m = 0;
		if (is_minimig()) m = 1;
		helptext_idx = 0;
		OsdSetTitle("Reset", 0);
		menumask = 0x03;	// Yes / No
		parentstate = menustate;

		OsdWrite(0, "", 0, 0);
		OsdWrite(1, m ? "       Reset Minimig?" : "       Reset settings?", 0, 0);
		OsdWrite(2, "", 0, 0);
		OsdWrite(3, "             yes", menusub == 0, 0);
		OsdWrite(4, "             no", menusub == 1, 0);
		OsdWrite(5, "", 0, 0);
		OsdWrite(6, "", 0, 0);
		for (int i = 7; i < OsdGetSize(); i++) OsdWrite(i, "", 0, 0);

		menustate = MENU_RESET2;
		break;

	case MENU_RESET2:
		m = 0;
		if (is_minimig()) m = 1;

		if (select && menusub == 0)
		{
			if (m)
			{
				menustate = MENU_NONE1;
				minimig_reset();
			}
			else
			{
				user_io_status_reset();
				char *filename = user_io_create_config_name(1);
				printf("Saving config to %s\n", filename);
				user_io_status_save(filename);
				menustate = MENU_GENERIC_MAIN1;
				if (arcade_sw()->dip_num)
				{
					arcade_sw()->dip_cur = arcade_sw()->dip_def;
					arcade_sw_send();
					user_io_status_set("[0]", 1);
					user_io_status_set("[0]", 0);
					arcade_sw_save();
				}
				menustate = MENU_NONE1;
				menusub = 0;
			}
		}

		if (menu || (select && (menusub == 1))) // exit menu
		{
			menustate = MENU_COMMON1;
			menusub = 11;
		}
		break;

		/******************************************************************/
		/* minimig main menu                                              */
		/******************************************************************/
	case MENU_MINIMIG_MAIN1:
		// AmigaCD console variant: render the simplified menu instead of the
		// full Minimig top level. This is the single external entry point for
		// the Minimig top level (OSD open / MGL load); AmigaCD states never set
		// MENU_MINIMIG_MAIN1, so the redirect only fires on a fresh entry.
		if (is_amigacd()) { menustate = MENU_AMIGACD_MAIN1; break; }

		menumask = 0x1EF0;
		OsdSetTitle("Minimig", OSD_ARROW_RIGHT | OSD_ARROW_LEFT);
		helptext_idx = HELPTEXT_MAIN;

		while (1)
		{
			if (!menusub) firstmenu = 0;
			adjvisible = 0;
			// floppy drive info
			// We display a line for each drive that's active
			// in the config file, but grey out any that the FPGA doesn't think are active.
			// We also print a help text in place of the last drive if it's inactive.
			for (int i = 0; i < 4; i++)
			{
				if (i == minimig_config.floppy.drives + 1) MenuWrite(i, " KP +/- to add/remove drives", 0, 1);
				else
				{
					strcpy(s, " dfx: ");
					s[3] = i + '0';
					if (i <= drives)
					{
						menumask |= (1 << i);	// Make enabled drives selectable

						if (df[i].status & DSK_INSERTED) // floppy disk is inserted
						{
							char *p;
							if ((p = strrchr(df[i].name, '/')))
							{
								p++;
							}
							else
							{
								p = df[i].name;
							}

							int len = strlen(p);
							if (len > 22) len = 21;
							strncpy(&s[6], p, len);
							s[6 + len] = ' ';
							s[6 + len + 1] = 0;
							s[6 + len + 2] = 0;
							if (!(df[i].status & DSK_WRITABLE)) s[6 + len + 1] = '\x17'; // padlock icon for write-protected disks
						}
						else // no floppy disk
						{
							strcat(s, "* no disk *");
						}
					}
					else if (i <= minimig_config.floppy.drives)
					{
						strcat(s, "* active after reset *");
					}
					else
					{
						strcpy(s, "");
					}
					MenuWrite(i, s, menusub == (uint32_t)i, (i > drives) || (i > minimig_config.floppy.drives));
				}
			}

			m = 4;
			if (is_minimig() == 2)
			{
				if (minimig_cfg_available(CONFIG_PRESET_CD32)) menumask |= 0x10;
				if (minimig_cfg_available(CONFIG_PRESET_CDTV)) menumask |= 0x20;

				MenuWrite(m++, " Start CD32 Game", menusub == 4, !(menumask & 0x10));
				MenuWrite(m++, " Start CDTV Game", menusub == 5, !(menumask & 0x20));
				MenuWrite(m++);
			}

			strcpy(s,      " Joystick Swap:          ");
			strcat(s, (minimig_config.autofire & 0x8) ? " On" : "Off");
			MenuWrite(m++, s, menusub == 6, 0);
			MenuWrite(m++),

			MenuWrite(m++, " CD & HDD                  \x16", menusub == 7, 0);
			MenuWrite(m++, " System                    \x16", menusub == 8, 0);
			MenuWrite(m++, " Audio & Video             \x16", menusub == 9, 0);
			if (spi_uio_cmd16(UIO_GET_OSDMASK, 0) & 1)
			{
				menumask |= 0x400;
				MenuWrite(m++, " MT32-pi                   \x16", menusub == 10);
			}

			MenuWrite(m++);
			MenuWrite(m++, " Load preset               \x16", menusub == 11, 0);
			MenuWrite(m++, " Load configuration        \x16", menusub == 12, 0);
			MenuWrite(m++, " Save configuration        \x16", menusub == 13, 0);
			MenuWrite(m++);

			while (m < 14) MenuWrite(m++);
			MenuWrite(m++, " Reset", menusub == 14, 0);
			MenuWrite(m, STD_EXIT, menusub == 15, 0);

			if (!adjvisible) break;
			firstmenu += adjvisible;
		}

		menustate = MENU_MINIMIG_MAIN2;
		parentstate = MENU_MINIMIG_MAIN1;

		if (!mgl->done)
		{
			if (mgl->item[mgl->current].index < 4)
			{
				menusub = mgl->item[mgl->current].index;
				menustate = MENU_MINIMIG_ADFFILE_SELECTED;
				break;
			}
			else if (mgl->item[mgl->current].index == 4 && (menumask & 0x10))
			{
				menustate = MENU_MINIMIG_CD32FILE_START;
				break;
			}
			else if (mgl->item[mgl->current].index == 5 && (menumask & 0x20))
			{
				menustate = MENU_MINIMIG_CDTVFILE_START;
				break;
			}
			else
			{
				mgl->state = 3;
			}
		}
		break;

	case MENU_MINIMIG_MAIN2:
		if (menu) menustate = MENU_NONE1;
		else if (plus && (minimig_config.floppy.drives < 3) && menusub < 4)
		{
			minimig_config.floppy.drives++;
			minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
			menustate = MENU_MINIMIG_MAIN1;
		}
		else if (minus && (minimig_config.floppy.drives > 0) && menusub < 4)
		{
			minimig_config.floppy.drives--;
			minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
			menustate = MENU_MINIMIG_MAIN1;
		}
		else if (select || recent || minus || plus)
		{
			if (menusub < 4)
			{
				ioctl_index = 0;
				if (df[menusub].status & DSK_INSERTED) // eject selected floppy
				{
					df[menusub].status = 0;
					FileClose(&df[menusub].file);
					menustate = MENU_MINIMIG_MAIN1;
				}
				else
				{
					df[menusub].status = 0;
					fs_Options = SCANO_DIR;
					fs_MenuSelect = MENU_MINIMIG_ADFFILE_SELECTED;
					fs_MenuCancel = MENU_MINIMIG_MAIN1;
					strcpy(fs_pFileExt, "ADFDMS");
					if (select) SelectFile(Selected_F[menusub], "ADFDMS", fs_Options, fs_MenuSelect, fs_MenuCancel, 0);
					else if (recent_init(0)) menustate = MENU_RECENT1;
				}
			}
			else if (menusub == 4 && (menumask & 0x10))
			{
				if (select || recent)
				{
					fs_Options = SCANO_DIR | SCANO_UMOUNT;
					fs_MenuSelect = MENU_MINIMIG_CD32FILE_START;
					fs_MenuCancel = MENU_MINIMIG_MAIN1;
					strcpy(fs_pFileExt, "ISOCUECHDIMG");
					if (select)
					{
						if (!Selected_CD32[0]) memcpy(Selected_CD32, minimig_config.cd32_drive.filename, sizeof(Selected_CD32));
						SelectFile(Selected_CD32, fs_pFileExt, fs_Options, fs_MenuSelect, fs_MenuCancel, 501);
					}
					else if (recent_init(501)) menustate = MENU_RECENT1;
					break;
				}
			}
			else if (menusub == 5 && (menumask & 0x20))
			{
				if (select || recent)
				{
					fs_Options = SCANO_DIR | SCANO_UMOUNT;
					fs_MenuSelect = MENU_MINIMIG_CDTVFILE_START;
					fs_MenuCancel = MENU_MINIMIG_MAIN1;
					strcpy(fs_pFileExt, "ISOCUECHDIMG");
					if (select)
					{
						if (!Selected_CDTV[0]) memcpy(Selected_CDTV, minimig_config.cdtv_drive.filename, sizeof(Selected_CDTV));
						SelectFile(Selected_CDTV, fs_pFileExt, fs_Options, fs_MenuSelect, fs_MenuCancel, 502);
					}
					else if (recent_init(502)) menustate = MENU_RECENT1;
					break;
				}
			}
			else if (menusub == 6)
			{
				minimig_config.autofire ^= 0x8;
				menustate = MENU_MINIMIG_CHIPSET1;
				minimig_ConfigAutofire(minimig_config.autofire, 0x8);
				menustate = MENU_MINIMIG_MAIN1;
			}
			else if (select)
			{
				if (menusub == 7)
				{
					menustate = MENU_MINIMIG_DISK1;
					menusub = 0;
				}
				else if (menusub == 8)
				{
					menustate = MENU_MINIMIG_CHIPSET1;
					menusub = 0;
				}
				else if (menusub == 9)
				{
					menustate = MENU_MINIMIG_VIDEO1;
					menusub = 0;
				}
				else if (menusub == 10)
				{
					menusub = 0;
					menustate = MENU_MT32PI_MAIN1;
				}
				else if (menusub == 11)
				{
					menusub = 0;
					menustate = MENU_MINIMIG_PRESET1;
				}
				else if (menusub == 12)
				{
					menusub = 0;
					menustate = MENU_MINIMIG_LOADCONFIG1;
				}
				else if (menusub == 13)
				{
					menusub = 0;
					menustate = MENU_MINIMIG_SAVECONFIG1;
				}
				else if (menusub == 14)
				{
					menustate = MENU_NONE1;
					minimig_reset();
				}
				else if (menusub == 15)
				{
					menustate = MENU_NONE1;
				}
			}
		}
		else if (c == KEY_BACKSPACE) // eject all floppies
		{
			for (int i = 0; i <= drives; i++) df[i].status = 0;
			menustate = MENU_MINIMIG_MAIN1;
		}
		else if (right)
		{
			menustate = MENU_COMMON1;
			menusub = 0;
		}
		else if (left)
		{
			menustate = MENU_MISC1;
			menusub = 3;
		}
		break;

	case MENU_MINIMIG_ADFFILE_SELECTED:
		if (!mgl->done)
		{
			// MGL <file path="">: absolute uses path; "../" resolves from storage root; else HomeDir
			const char *p = mgl->item[mgl->current].path;
			if (p[0] == '/') snprintf(selPath, sizeof(selPath), "%s", p);
			else if (p[0] == '.' && p[1] == '.') snprintf(selPath, sizeof(selPath), "%s/%s", getRootDir(), p);
			else snprintf(selPath, sizeof(selPath), "%s/%s", HomeDir(), p);
			// Update /tmp/ files to reflect the actual file being loaded by MGL
			if (cfg.log_file_entry)
			{
				const char *fname = strrchr(selPath, '/');
				MakeFile("/tmp/FULLPATH", selPath);
				MakeFile("/tmp/CURRENTPATH", fname ? fname + 1 : selPath);
				MakeFile("/tmp/FILESELECT", "selected");
			}
		}
		memcpy(Selected_F[menusub], selPath, sizeof(Selected_F[menusub]));
		if (mgl->done) recent_update(SelectedDir, selPath, SelectedLabel, 0);
		InsertFloppy(&df[menusub], selPath);
		if (menusub < drives) menusub++;
		menustate = MENU_MINIMIG_MAIN1;
		if (!mgl->done)
		{
			mgl->state = 3;
			menustate = MENU_NONE1;
		}
		break;

	case MENU_MINIMIG_LOADCONFIG1:
		helptext_idx = 0;
		if (parentstate != menustate) menumask = 0x400;

		parentstate = menustate;
		OsdSetTitle("Load config", 0);

		m = 0;
		OsdWrite(m++, "", 0, 0);
		OsdWrite(m++, " Startup config:");
		for (uint i = 0; i < 10; i++)
		{
			const char *info = minimig_get_cfg_info(i, menusub != i);
			static char name[128];

			if (info)
			{
				menumask |= 1 << i;
				sprintf(name, "  %s", strlen(info) ? info : "NO INFO");
				char *p = strchr(name, '\n');
				if (p) *p = 0;
				OsdWrite(m++, name, menusub == i);

				if (menusub == i && p)
				{
					// The selected config's second info line is drawn inverted
					// too, so it reads as part of the same highlighted block.
					// That makes two inverted rows on purpose, which is the one
					// page in the tree where the guard at the top of HandleUI
					// would be wrong -- hence the exemption rather than a
					// changed rendering.
					OsdRowTallyExempt();
					sprintf(name, "  %s", strchr(info, '\n') + 1);
					OsdWrite(m++, name, 1, !(menumask & (1 << i)));
				}
			}

			if (!i)
			{
				OsdWrite(m++, "", 0, 0);
				m = 4;
				OsdWrite(m++, " Other configs:");
			}
		}

		while (m < OsdGetSize() - 1) OsdWrite(m++);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 10, 0);

		menustate = MENU_MINIMIG_LOADCONFIG2;
		break;

	case MENU_MINIMIG_LOADCONFIG2:
		if (down)
		{
			if (menusub < 9) menusub++;
			menustate = MENU_MINIMIG_LOADCONFIG1;
		}
		else if (select)
		{
			if (menusub < 10)
			{
				OsdDisable();
				minimig_cfg_load(menusub);
				menustate = MENU_NONE1;
			}
			else
			{
				amigacd_loadcfg_back();
			}
		}
		if (menu || left)
		{
			amigacd_loadcfg_back();
		}
		break;

	case MENU_MINIMIG_SAVECONFIG1:
		helptext_idx = 0;
		menumask = 0x7ff;
		parentstate = menustate;
		OsdSetTitle("Save config", 0);

		m = 0;
		OsdWrite(m++, "", 0, 0);
		OsdWrite(m++, " Startup config:");
		for (uint i = 0; i < 10; i++)
		{
			const char *info = minimig_get_cfg_info(i, menusub != i);
			static char name[128];

			if (info)
			{
				sprintf(name, "  %s", strlen(info) ? info : "NO INFO");
				char *p = strchr(name, '\n');
				if (p) *p = 0;
				OsdWrite(m++, name, menusub == i);
				if (menusub == i && p)
				{
					sprintf(name, "  %s", strchr(info, '\n') + 1);
					OsdWrite(m++, name, 1);
				}
			}
			else
			{
				OsdWrite(m++, "  < EMPTY >", menusub == i);
			}

			if (!i)
			{
				OsdWrite(m++, "", 0, 0);
				m = 4;
				OsdWrite(m++, " Other configs:");
			}
		}

		while (m < OsdGetSize() - 1) OsdWrite(m++);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 10, 0);

		menustate = MENU_MINIMIG_SAVECONFIG2;
		break;

	case MENU_MINIMIG_SAVECONFIG2:
		if (select)
		{
			int fastcfg = ((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5);
			sprintf(minimig_config.info, "%s/%s/%s%s %s%s%s%s%s%s\n",
				config_cpu_msg[minimig_config.cpu & 0x03] + 2,
				config_chipset_msg[(minimig_config.chipset >> 2) & 7],
				minimig_config.chipset & CONFIG_NTSC ? "N" : "P",
				((minimig_config.ide_cfg & 1) && (minimig_config.hardfile[0].cfg ||
					minimig_config.hardfile[1].cfg ||
					minimig_config.hardfile[2].cfg ||
					minimig_config.hardfile[3].cfg)) ? "/HD" : "",
				config_memory_chip_msg[minimig_config.memory & 0x03],
				fastcfg ? "+" : "",
				fastcfg ? config_memory_fast_msg[minimig_fast_z3(minimig_config.cpu)][fastcfg] : "",
				((minimig_config.memory >> 2) & 0x03) ? "+" : "",
				((minimig_config.memory >> 2) & 0x03) ? config_memory_slow_msg[(minimig_config.memory >> 2) & 0x03] : "",
				(minimig_config.memory & 0x40) ? " HRT" : ""
			);

			char *p = strrchr(minimig_config.kickstart, '/');
			if (!p) p = minimig_config.kickstart;
			else p++;

			strncat(minimig_config.info, p, sizeof(minimig_config.info) - strlen(minimig_config.info) - 1);
			minimig_config.info[sizeof(minimig_config.info) - 1] = 0;

			if (menusub<10)
			{
				minimig_cfg_save(menusub);
				minimig_mgl_save();
			}
			amigacd_savecfg_back();
		}
		else
		if (menu || left) // exit menu
		{
			amigacd_savecfg_back();
		}
		break;

	case MENU_MINIMIG_PRESET1:
		helptext_idx = 0;
		menumask = 0x100;
		parentstate = menustate;
		OsdSetTitle("Load preset", 0);

		if (minimig_cfg_available(CONFIG_PRESET_A500)) menumask |= 1;
		if (minimig_cfg_available(CONFIG_PRESET_A600)) menumask |= 2;
		if (minimig_cfg_available(CONFIG_PRESET_A1200)) menumask |= 4;
		if (minimig_cfg_available(CONFIG_PRESET_CDTV)) menumask |= 8;
		if (minimig_cfg_available(CONFIG_PRESET_CD32)) menumask |= 16;

		m = 0;
		OsdWrite(m++, "", 0, 0);
		OsdWrite(m++, " A500  OCS ChipRAM 512KB", menusub == 0, !(menumask & 1));
		OsdWrite(m++, " A600  ECS ChipRAM 1MB", menusub == 1, !(menumask & 2));
		OsdWrite(m++, " A1200 AGA ChipRAM 2MB", menusub == 2, !(menumask & 4));
		OsdWrite(m++, "", 0, 0);
		OsdWrite(m++, " CDTV  ECS ChipRAM 1MB", menusub == 3, !(menumask & 8));
		OsdWrite(m++, " CD32  AGA ChipRAM 2MB", menusub == 4, !(menumask & 16));

		while (m < OsdGetSize() - 1) OsdWrite(m++);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 8, 0);
		menustate = MENU_MINIMIG_PRESET2;
		break;

	case MENU_MINIMIG_PRESET2:
		if (select)
		{
			if (menusub < 8)
			{
				switch (menusub)
				{
				case 0:
					minimig_cfg_set(CONFIG_PRESET_A500);
					break;

				case 1:
					minimig_cfg_set(CONFIG_PRESET_A600);
					break;

				case 2:
					minimig_cfg_set(CONFIG_PRESET_A1200);
					break;

				case 3:
					minimig_cfg_set(CONFIG_PRESET_CDTV);
					break;

				case 4:
					minimig_cfg_set(CONFIG_PRESET_CD32);
					break;
				}
				minimig_reset();
				menustate = MENU_MINIMIG_MAIN1;
				menusub = 0;
			}
			else if (menusub == 8)
			{
				menustate = MENU_MINIMIG_MAIN1;
				menusub = 11;
			}
		}
		if (menu || left)
		{
			menustate = MENU_MINIMIG_MAIN1;
			menusub = 11;
		}
		break;

	case MENU_MINIMIG_CHIPSET1:
		helptext_idx = HELPTEXT_CHIPSET;
		// 16 selectable rows: 0..11 from this fork (FDD Turbo is 11), 12..13
		// save states, 14 = upstream's A2065 Ethernet row (menusub 9 upstream,
		// renumbered twice on merge), 15 = Back.
		//
		// This said fifteen rows with the save states at 11..12, which missed
		// that FDD Turbo below is an eleventh fork row and already held 11. Two
		// rows drew as selected whenever menusub was 11, and CHIPSET2 read 11
		// as the save-slot stepper, so FDD Turbo could be highlighted and never
		// toggled. The row count on this page is stated here and nowhere else,
		// which is why a miscount here was invisible.
		menumask = 0xFFFF;
		OsdSetTitle("System");
		parentstate = menustate;

		// *** THIS PAGE IS EXACTLY FULL ***
		// The core's OSD is 16 lines (sys/osd.v OSD_HEIGHT 64 at highres, and
		// OsdUpdate() only transmits OsdGetSize() == 16 of them), Back occupies
		// the last one, so there are 15 lines for 14 rows. The blank separators
		// this page used to carry between groups are gone: with them, m ran to
		// 17 even BEFORE the save state rows, which put the Ethernet row on
		// line 16 where OsdUpdate never sent it and let Back overwrite the
		// blank on line 15. That row has been invisible since this fork added
		// D-Cache and CPU Speed. Anything added here from now on has to
		// displace something.
		m = 0;
		strcpy(s, " CPU      : ");
		// CPU byte (minimig_config.cpu), since upstream e836aea:
		//   bits 1:0  00 68000, 01 68010, 10 68030, 11 68020
		//   bit  5    68020: the stock-speed throttle (68EC020, A1200 speed,
		//             24-bit addresses), i.e. CPU Turbo Off.
		//             68030: no MMU (upstream 60a868f; the clocks are gone).
		//   bit  4    was this fork's D-Cache; the core no longer reads it.
		// Upstream drops the unthrottled 68020 and moves such configs to the
		// 68030 no MMU (0x22, since 60a868f). This fork keeps it -- it is CPU Turbo On, the
		// default since 4b4f604 -- and the RTL still runs it (cpucfg 011).
		strcat(s, ((minimig_config.cpu & 0x23) == 0x23) ? "68EC020" : config_cpu_msg[minimig_config.cpu & 0x03]);
		OsdWrite(m++, s, menusub == 0, 0);
		// The 68030's MMU, on the line its clock had (upstream 60a868f left
		// the 68030 one clock and made "no MMU" bit 5). Upstream offers it as
		// a fifth CPU, "68030 no MMU"; here it is a row of its own, the way
		// bit 5 is CPU Turbo on the 68020, which keeps every row below where
		// it was on a page with no line to spare. Greyed for every other CPU.
		strcpy(s, " CPU MMU  : ");
		strcat(s, ((minimig_config.cpu & 0x03) != 0x02) ? "-" : (minimig_config.cpu & 0x20) ? "Off" : "On");
		OsdWrite(m++, s, menusub == 1, (minimig_config.cpu & 0x03) != 0x02);
		// CPU CFG bit 5 read the other way up: set means the stock-speed throttle
		// is engaged, so Turbo On is the bit CLEAR. Phrased as Turbo because that
		// is the question being asked -- "faster than the real machine?" -- rather
		// than as a clock rate the core does not literally produce.
		//
		// "CPU Turbo" and not just "Turbo": FDD Turbo is a few rows down on this
		// same page, and two rows both called Turbo is a support question waiting
		// to happen.
		//
		// The 68020 only. On the 68030 bit 5 is the MMU row above.
		strcpy(s, " CPU Turbo: ");
		strcat(s, ((minimig_config.cpu & 0x03) != 0x03) ? "-" : (minimig_config.cpu & 0x20) ? "Off" : "On");
		OsdWrite(m++, s, menusub == 2, (minimig_config.cpu & 0x03) != 0x03);
		strcpy(s, " Chipset   : ");
		strcat(s, config_chipset_msg[(minimig_config.chipset >> 2) & 7]);
		OsdWrite(m++, s, menusub == 3, 0);
		strcpy(s, " ChipRAM   : ");
		strcat(s, config_memory_chip_msg[minimig_config.memory & 0x03]);
		OsdWrite(m++, s, menusub == 4, 0);
		strcpy(s, " FastRAM   : ");
		strcat(s, config_memory_fast_msg[minimig_fast_z3(minimig_config.cpu)][((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5)]);
		OsdWrite(m++, s, menusub == 5, 0);
		strcpy(s, " SlowRAM   : ");
		strcat(s, config_memory_slow_msg[(minimig_config.memory >> 2) & 0x03]);
		OsdWrite(m++, s, menusub == 6, 0);

		strcpy(s, " Joystick  : ");
		strcat(s, config_joystick_mode[(minimig_config.autofire & 6) >> 1]);
		OsdWrite(m++, s, menusub == 7, 0);

		strcpy(s, " ROM    : ");
		{
			char *path = HomeDir();
			int len = strlen(path);
			char *name = minimig_config.kickstart;
			if (!strncasecmp(name, path, len))  name += len + 1;
			strncat(s, name, 20);
		}

		OsdWrite(m++, s, menusub == 8, 0);
		strcpy(s, " Ext.ROM: ");
		{
			char *path = HomeDir();
			int len = strlen(path);
			const char *name = minimig_get_extrom();
			if (!name[0]) {
				strcat(s, "<none>");
			} else {
				if (!strncasecmp(name, path, len)) name += len + 1;
				strncat(s, name, 20);
			}
		}
		OsdWrite(m++, s, menusub == 9, 0);
		strcpy(s, " HRTmon : ");
		strcat(s, (minimig_config.memory & 0x40) ? "enabled " : "disabled");
		OsdWrite(m++, s, menusub == 10, 0);
		sprintf(s, " FDD Turbo: %s", minimig_config.floppy.speed ? "On" : "Off");
		OsdWrite(m++, s, menusub == 11, 0);

		// Save states. The core is the active party in the protocol: these rows
		// only pick a slot and raise a request bit, the capture itself is
		// entirely in RTL and the host's process_ss() poller notices the
		// finished window within a second and writes it to disk.
		//
		// Both rows grey out together -- see minimig_ss_blocked() for the two
		// reasons. A greyed row is still reachable with the cursor (same as the
		// D-Cache row above), so the handlers re-test the condition.
		{
			int ss_blocked = minimig_ss_blocked();

			strcpy(s, " Save slot : ");
			strcat(s, config_ss_slot_msg[minimig_config.ss_slot & 3]);
			OsdWrite(m++, s, menusub == 12, ss_blocked);

			amigacd_ss_preview(menusub == 12 && !ss_blocked, minimig_config.ss_slot & 3);

			OsdWrite(m++, " Save state", menusub == 13, ss_blocked);
		}

		strcpy(s, " Ethernet : ");
		strcat(s, a2065_iface_msg(a2065_get_iface()));
		// Upstream renders this at menusub 9; this fork has five extra rows
		// ahead of it, so it lands at 14 and Back moves to 15.
		OsdWrite(m++, s, menusub == 14, 0);

		for (int i = m; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 15, 0);

		menustate = MENU_MINIMIG_CHIPSET2;
		break;

	case MENU_MINIMIG_CHIPSET2:
		saved_menustate = MENU_MINIMIG_CHIPSET1;

		if (select || minus || plus)
		{
			if (menusub == 0)
			{
				// 68000 -> 68010 -> 68020 -> 68030. Bits 2-4 (turbo chip/kick,
				// the old D-Cache bit) are left alone. Bit 5 belongs to the
				// Turbo row on the 68020 and to the MMU row on the 68030, so
				// it is cleared on every change of CPU, with the old clock
				// bits 7:6: a 68020 is entered Turbo On, this fork's default,
				// and a 68030 with its MMU, as upstream's step order enters it.
				static const unsigned char cpu_order[4] = { 0, 1, 3, 2 };
				int cpu = minimig_config.cpu & 3;
				int at = (cpu == 0) ? 0 : (cpu == 1) ? 1 : (cpu == 3) ? 2 : 3;
				cpu = cpu_order[(at + (minus ? 3 : 1)) & 3];

				menustate = MENU_MINIMIG_CHIPSET1;
				minimig_config.cpu = (minimig_config.cpu & 0x1c) | cpu;
				minimig_ConfigCPU(minimig_config.cpu);
			}
			else if (menusub == 1 && (minimig_config.cpu & 0x3) == 0x2)
			{
				// 68030 MMU (CPU CFG bit 5): set is "68030 no MMU", whose MMU
				// instructions take the F-line exception as on a 68020.
				minimig_config.cpu ^= 0x20;
				menustate = MENU_MINIMIG_CHIPSET1;
				minimig_ConfigCPU(minimig_config.cpu);
			}
			else if (menusub == 2 && (minimig_config.cpu & 0x3) == 0x3)
			{
				// Toggle stock-speed gate (CPU CFG bit 5): 68EC020 at A1200
				// speed when set, the unthrottled 68020 when clear.
				minimig_config.cpu ^= 0x20;
				menustate = MENU_MINIMIG_CHIPSET1;
				minimig_ConfigCPU(minimig_config.cpu);
			}
			else if (menusub == 3)
			{
				if (minus)
				{
					switch (minimig_config.chipset & 0x1c)
					{
					case (CONFIG_AGA | CONFIG_ECS):
						minimig_config.chipset = (minimig_config.chipset & 3) | CONFIG_ECS;
						break;
					case CONFIG_ECS:
						minimig_config.chipset = (minimig_config.chipset & 3) | CONFIG_A1000;
						break;
					case CONFIG_A1000:
						minimig_config.chipset = (minimig_config.chipset & 3) | 0;
						break;
					case 0:
						minimig_config.chipset = (minimig_config.chipset & 3) | CONFIG_AGA | CONFIG_ECS;
						break;
					}
				}
				else
				{
					switch (minimig_config.chipset & 0x1c)
					{
					case 0:
						minimig_config.chipset = (minimig_config.chipset & 3) | CONFIG_A1000;
						break;
					case CONFIG_A1000:
						minimig_config.chipset = (minimig_config.chipset & 3) | CONFIG_ECS;
						break;
					case CONFIG_ECS:
						minimig_config.chipset = (minimig_config.chipset & 3) | CONFIG_AGA | CONFIG_ECS;
						break;
					case (CONFIG_AGA | CONFIG_ECS):
						minimig_config.chipset = (minimig_config.chipset & 3) | 0;
						break;
					}
				}

				minimig_ConfigChipset(&minimig_config);
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 4)
			{
				minimig_config.memory = ((minimig_config.memory + (minus ? -1 : 1)) & 0x03) | (minimig_config.memory & ~0x03);
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 5)
			{
				int c = (((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5));
				// Upstream 60a868f's order: none 2M 4M 8M 256M 384M, the Zorro III
				// sizes only where minimig_fast_z3() allows them.
				if (minus)
				{
					c--;
					if (c < 0) c = 5;
					if (!minimig_fast_z3(minimig_config.cpu) && c > 3) c = 3;
				}
				else
				{
					c++;
					if (c > 5) c = 0;
					if (!minimig_fast_z3(minimig_config.cpu) && c > 3) c = 0;
				}
				minimig_config.memory = ((c << 4) & 0x30) | ((c << 5) & 0x80) | (minimig_config.memory & ~0xB0);
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 6)
			{
				minimig_config.memory = ((minimig_config.memory + (minus ? -4 : 4)) & 0x0C) | (minimig_config.memory & ~0x0C);
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 7)
			{
				uint8_t x = (minimig_config.autofire & 6) >> 1;
				if (minus)
				{
					x = (x - 1) & 3;
					if (x == 3) x = 2;
				}
				else
				{
					x = (x + 1) & 3;
					if (x == 3) x = 0;
				}

				minimig_config.autofire = (minimig_config.autofire & ~6) | (x << 1);
				menustate = MENU_MINIMIG_CHIPSET1;
				minimig_ConfigAutofire(minimig_config.autofire, 6);
			}
			else if (menusub == 8 && select)
			{
				ioctl_index = 1;
				strcpy(Selected_F[4], minimig_config.kickstart);
				SelectFile(Selected_F[4], "ROM", SCANO_DIR, MENU_MINIMIG_ROMFILE_SELECTED, MENU_MINIMIG_CHIPSET1);
			}
			else if (menusub == 9)
			{
				if (minus)
				{
					// Clear Ext.ROM pairing.
					minimig_set_extrom((char *)"");
					menustate = MENU_MINIMIG_CHIPSET1;
				}
				else if (select)
				{
					ioctl_index = 1;
					strcpy(Selected_F[5], minimig_config.kickstart + strlen(minimig_config.kickstart) + 1);
					SelectFile(Selected_F[5], "ROM", SCANO_DIR | SCANO_UMOUNT, MENU_MINIMIG_EXTROMFILE_SELECTED, MENU_MINIMIG_CHIPSET1);
				}
			}
			else if (menusub == 10)
			{
				minimig_config.memory ^= 0x40;
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 11)
			{
				// FDD Turbo. It had no handler at all: this index was the save
				// slot's, so the row rendered and did nothing.
				minimig_config.floppy.speed ^= 1;
				minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 12 && !minimig_ss_blocked())
			{
				// Save slot 1..4, stored 0..3. Kept in minimig_config so it
				// survives "Save settings"; it is NOT pushed to status[53:52]
				// here. The core samples slot_base continuously for the whole
				// ~0.2 s dump, so writing the slot only immediately before the
				// request (menusub 12 below) makes it impossible for a slot
				// change to redirect a save that is already in flight.
				int slot = minimig_config.ss_slot & 3;
				slot = minus ? (slot + 3) & 3 : (slot + 1) & 3;
				minimig_config.ss_slot = (unsigned char)slot;
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 13 && select && !minimig_ss_blocked())
			{
				// Everything this needs -- arming the writer included -- is in
				// the shared helper. This row used to do the raise on its own
				// and skip the arm, which is how a save could be announced and
				// never written.
				minimig_ss_request_save();
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			else if (menusub == 14)
			{
				// A2065 ethernet: OFF -> eth0 -> eth1 -> macvlan -> tap0,
				// skipping anything this box cannot support (no second NIC,
				// no tun driver, no macvlan). OFF and eth0 always qualify, so
				// the walk always lands somewhere.
				int m2 = a2065_get_iface();
				for (int i = 0; i < A2065_MODES; i++)
				{
					m2 = minus ? (m2 + A2065_MODES - 1) % A2065_MODES
					           : (m2 + 1) % A2065_MODES;
					if (a2065_mode_available(m2)) break;
				}
				a2065_set_iface(m2);
				menustate = MENU_MINIMIG_CHIPSET1;
			}
			// Back. Upstream puts the A2065 row at menusub 9 and Back at 10;
			// this fork has five extra rows ahead of them, so both shift up.
			else if (menusub == 15)
			{
				menustate = MENU_MINIMIG_MAIN1;
				menusub = 8;
			}
		}

		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left)
		{
			menustate = MENU_MINIMIG_MAIN1;
			menusub = 8;
		}
		break;

	case MENU_MINIMIG_ROMFILE_SELECTED:
		memcpy(Selected_F[4], selPath, sizeof(Selected_F[4]));
		minimig_set_kickstart(selPath);
		menustate = MENU_MINIMIG_CHIPSET1;
		break;

	case MENU_MINIMIG_EXTROMFILE_SELECTED:
		memcpy(Selected_F[5], selPath, sizeof(Selected_F[5]));
		minimig_set_extrom(selPath);
		menustate = MENU_MINIMIG_CHIPSET1;
		break;

	// ===================== AmigaCD console variant (simplified menu) =========
	// Top level: CD / Real CD / Floppy x2 / Hard Disk / System / Settings /
	// Advanced / Save / Reset / Exit.
	// ChipRAM/CPU/chipset/HDD/video/config complexity is hidden; Settings exposes
	// only FastRAM, D-Cache (020 only) and Main/Ext ROM browse. See
	// research/docs/amiga-console-design-2026-06-13.md.
	case MENU_AMIGACD_MAIN1:
	{
		// LEFT/RIGHT arrows page out to the stock MiSTer common menus (System:
		// load other cores + Reboot-to-menu; Misc: volume/config/info) -- only
		// the core menu itself is reskinned, the MiSTer menu stays reachable.
		OsdSetTitle("AmigaCD", OSD_ARROW_LEFT | OSD_ARROW_RIGHT);

		// What the row under the cursor does, and the key that does it without
		// the menu. See amigacd_rowhint().
		//
		// This replaces one page-level line that named the eject binding and
		// the save state hotkeys together. That line had to list every shortcut
		// on the page at once, because it could not know which row you were on;
		// these say the one that applies. The eject binding in particular is
		// contextual again, which is how it read back when it had a row.
		//
		// The core name and build date still lead every hint -- see
		// amigacd_rowhint() for why they are not on a line of their own.
		static const char *const tips[] =
		{
			/* 0 CD        */ "Mount a CD image: ISO, CUE or CHD. Backspace, or A+B on a pad, ejects it. The ` key lists the discs you have used before.",
			/* 1 Real CD   */ "Use a CD-ROM drive plugged into the MiSTer instead of an image. Select it again after changing the disc to re-read the TOC.",
			/* 2 Floppy 0  */ "Mount an ADF, DMS, SCP or IPF in df0. Backspace or A+B ejects. Left and right assign a real drive on the MiSTer Floppy board instead.",
			/* 3 Floppy 1  */ "Mount an ADF, DMS, SCP or IPF in df1. Backspace or A+B ejects. Left and right assign a real drive on the MiSTer Floppy board instead.",
			/* 4 Hard Disk */ "Mount an HDF as the IDE0 slave. Turns the IDE controller on if it was off. Backspace or A+B ejects.",
			/* 5 Hard Disk */ "Mount an HDF as the IDE1 master: the second volume software like AmigaVision keeps its settings on. Backspace or A+B ejects.",
			/* 6 System    */ "The machine: CD32, CDTV, A500 or A1200. Each keeps its own settings, so switching away and back does not lose them.",
			/* 7 Settings  */ "RAM, ROMs, save states, CD speed, per-game NVRAM, and the Amiga's own video and audio.",
			/* 8 Advanced  */ "The user port - MT32-pi, MiSTer Floppy or a PSX SNAC adapter - plus Ethernet, Fast-IDE, and writing an .mgl launcher.",
			/* 9 Configs   */ "Load a saved config, or save this one to any of ten slots.",
			/*10 Save      */ "Write these settings, including what is mounted, as the startup config. Loaded again the next time the core starts.",
			/*11 Reset     */ "Restart the Amiga; asks first. Ctrl+LAlt+RAlt does it straight away, without the menu and without asking.",
			/*12 exit      */ "Close the menu. Esc or F12 does the same from anywhere.",
		};
		static_assert(sizeof(tips) / sizeof(tips[0]) == rowhint_rows(AMIGACD_MAIN_MENUMASK),
			"AmigaCD main page: one hint per selectable row");
		helptext_idx = amigacd_rowhint(tips, sizeof(tips) / sizeof(tips[0]), menusub);
		// CD, Real CD, DF0, DF1, HD0, HD1, System, Settings, Advanced,
		// Configs, Save, Reset, Exit -- thirteen rows. Write .mgl was the fourteenth
		// and is on the Advanced page now: it is a one-off setup step, not
		// something reached for while playing, and the everyday page is
		// shorter for its going.
		menumask = AMIGACD_MAIN_MENUMASK;

		// The A1200 profile has no CD drive at all, so its two rows are dead.
		// Greyed out rather than removed: the row positions are shared with
		// every other system and menusub indices below them would all shift,
		// and a row that vanishes reads as a bug where a greyed one reads as
		// "not on this machine".
		const int amigacd_has_cd = (amigacd_system() != AMIGACD_SYS_A1200);
		if (!amigacd_has_cd) menumask &= ~0x003;

		// A stock CD32 has no hard disk interface at all -- Akiko gives it the
		// CD, and the IDE and Gayle decodes belong to hardware it never
		// shipped with. The rows are masked as well as greyed so the selection
		// cannot land on something that cannot work; they keep their positions
		// because every menusub below would shift otherwise, which is the same
		// reason the A1200's dead CD rows are kept.
		const int amigacd_has_hdd = amigacd_profile_has_hdd(amigacd_system());
		if (!amigacd_has_hdd) menumask &= ~0x030;
		parentstate = menustate;

		// mister_floppy_status rides in the core's OSD mask: bit 6 board
		// detected, bit 7 IBM/PC vs Shugart, bit 8 cable straight vs crossed.
		uint16_t osdMask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);

		// What is actually on the mounted disc, for the detail line under the
		// CD row. Two rips of the same CD32 title -- data only, and the full
		// disc with its music -- mount identically and look identical here,
		// and the difference only turns up as silence once you are playing.
		// The track counts settle it before that.
		//
		// Drawn whenever there is a disc rather than only when the cursor is
		// on the row: a line that appears and disappears as the cursor moves
		// makes the rows below it jump, and the answer is worth seeing without
		// having to go and look for it.
		char cd_label[64] = {};
		int  cd_tracks = 0, cd_audio = 0;
		const int cd_detail = amigacd_has_cd
		                   && akiko_cd32_disc_info(cd_label, sizeof(cd_label), &cd_tracks, &cd_audio)
		                   && cd_tracks > 0;

		m = 0;
		OsdWrite(m++, "", 0, 0);

		// CD slot = hardfile[0]. The sentinel means "the physical drive is
		// selected", which is not the same as "a disc is mounted" -- a saved
		// config restores the sentinel even if the tray was empty at core
		// load, and ide_open then returns 0. Render the real mount state
		// (drive[0].phys) so the row cannot claim a disc that is not there.
		strcpy(s, " CD       : ");
		if (!amigacd_has_cd) strcat(s, "* no drive *");
		else if (!strcmp(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL))
		{
			strcat(s, ide_inst[0].drive[0].phys ? "* physical drive *" : "* no disc *");
		}
		else if (minimig_config.hardfile[0].filename[0])
		{
			char *name = minimig_config.hardfile[0].filename;
			char *p = strrchr(name, '/');
			strncat(s, p ? p + 1 : name, 24);
		}
		else strcat(s, "* no disc *");
		OsdWrite(m++, s, menusub == 0, !amigacd_has_cd);

		if (cd_detail)
		{
			// The OSD line is 32 characters (OSDLINELEN is 256 pixels at 8 per
			// character), so the counts get their space first and the label
			// takes what is left. The label is the part that can be any
			// length, and it is the part you can do without: the counts are
			// why this line exists.
			char counts[24];
			if (cd_audio) snprintf(counts, sizeof(counts), "%d trk  %d CDDA", cd_tracks, cd_audio);
			else          snprintf(counts, sizeof(counts), "%d trk  no CDDA", cd_tracks);

			int budget = 32 - 3 - 2 - (int)strlen(counts);
			if (cd_label[0] && budget > 0) sprintf(s, "   %.*s  %s", budget, cd_label, counts);
			else                           sprintf(s, "   %s", counts);
			OsdWrite(m++, s, 0, 1);
		}

		// Physical CD-ROM drive attached to the MiSTer. Mounting parks the
		// sentinel in hardfile[0].filename, which is what ide_open matches on.
		// Re-selecting while mounted re-reads the TOC, i.e. disc swap.
		{
			// Probed once and used twice, for the text and for the greying:
			// the probe is eight stat() calls and this row redraws constantly.
			const int have_drive = amigacd_has_cd && amigacd_optical_drive_present();

			strcpy(s, " Real CD  : ");
			if (!amigacd_has_cd) strcat(s, "-");
			else if (ide_inst[0].drive[0].phys) strcat(s, "re-read TOC");
			// "mount drive" on a machine with no drive attached invites a press
			// that cannot work. Say so instead, and grey it -- there is nothing
			// to mount until a drive appears. SELECT still answers, because
			// greying here is presentation and not a lock.
			else if (!have_drive) strcat(s, "* no drive found *");
			else strcat(s, "mount drive");
			OsdWrite(m++, s, menusub == 1, !have_drive && !ide_inst[0].drive[0].phys);
		}

		// df0 and df1. The core has always presented two drives -- floppy.drives
		// is a count-minus-one and amigacd_apply_system sets it to 1 -- but the
		// OSD only ever drew one row, so df1 was live with no way to fill it.
		// extDrives[] is sized 4 for the eventual Amiga maximum.
		for (int fu = 0; fu < 2; fu++)
		{
			sprintf(s, " Floppy %d : ", fu);
			if (df[fu].status & DSK_INSERTED)
			{
				char *p = strrchr(df[fu].name, '/');
				strncat(s, p ? p + 1 : df[fu].name, 18);
				// Flux images are read-only by design (InsertFloppy omits
				// DSK_WRITABLE for them), so the padlock is expected there.
				if (df[fu].status & DSK_FLUXMODE)    strcat(s, " \x1B");
				if (!(df[fu].status & DSK_WRITABLE)) strcat(s, " \x17");
			}
			else
			{
				// No image: show the external drive assignment instead, through
				// the same namer the Drives page uses.
				int ext = (minimig_config.userport == mmup_misterfloppy)
				        ? minimig_config.floppy.extDrives[fu] : 0;
				if (ext >= 1 && ext <= 4)
				{
					char en[32];
					amigacd_extdrive_name(ext, osdMask, en, sizeof(en));
					strcat(s, en);
				}
				else strcat(s, "* no disk *");
			}
			// Grey a drive the machine does not have, rather than dropping the
			// row: positions are shared across systems and every menusub below
			// would shift, which is the same reason the A1200's CD rows stay.
			// A CD32 or a CDTV has no floppy drive at all, so both rows grey
			// rather than only the second. The rows stay present because row
			// positions are shared across systems and every menusub below
			// would shift otherwise -- the same reason the A1200's dead CD
			// rows are kept.
			OsdWrite(m++, s, menusub == (uint32_t)(2 + fu),
			         amigacd_profile_floppy_none(amigacd_system())
			         || fu > minimig_config.floppy.drives);
		}

		// Two hard disks. HD0 is hardfile[1], the IDE0 slave, so it sits beside
		// the CD slot (hardfile[0]) rather than displacing it -- a machine can
		// boot from an HDF with a disc still mounted. HD1 is hardfile[2], the
		// IDE1 master.
		//
		// Two rather than one because a single HDF is not how the software that
		// wants them ships. AmigaVision keeps its saves and settings on a second
		// volume, and with nowhere to mount it the system boots and then forgets
		// everything on reset. ApplyConfiguration already opens all four IDE
		// units, so the rows were the only part missing.
		for (int hu = 0; hu < 2; hu++)
		{
			const int hslot = hu + 1;    // hardfile[1] and hardfile[2]
			sprintf(s, " Hard Disk %d: ", hu);
			if (minimig_config.hardfile[hslot].cfg && minimig_config.hardfile[hslot].filename[0])
			{
				char *hname = minimig_config.hardfile[hslot].filename;
				char *hp = strrchr(hname, '/');
				strncat(s, hp ? hp + 1 : hname, 22);
			}
			else strcat(s, amigacd_has_hdd ? "* none *" : "-");
			OsdWrite(m++, s, menusub == (uint32_t)(4 + hu), !amigacd_has_hdd);
		}

		// The detail line above is paid for here rather than out of the page's
		// height. Sixteen lines with Reset and exit pinned to the last two
		// leaves exactly one spare, which the hint line at the bottom uses;
		// without this the hint would silently stop fitting whenever a disc
		// was mounted, which is precisely when it is wanted.
		if (!cd_detail) OsdWrite(m++, "", 0, 0);

		strcpy(s, " System   : ");
		strcat(s, amigacd_system_name(amigacd_system_shown()));
		OsdWrite(m++, s, menusub == 6, 0);

		// The blank that used to sit here paid for the Load settings row below.
		// Sixteen lines with Reset and exit pinned to the last two leaves one
		// spare, and the hint line has it -- the hints being the only place the
		// Backspace/A+B binding is written down, they do not lose a fight with
		// a blank line.
		OsdWrite(m++, " Settings                 \x16", menusub == 7, 0);
		// Hardware a real CD32/CDTV never shipped with. Kept off the Settings
		// page so the everyday options stay a short list.
		OsdWrite(m++, " Advanced                 \x16", menusub == 8, 0);
		// The three submenus sit together, then the one-press actions. Configs
		// was "Load settings", which opened the config list directly. It is a
		// submenu so that SAVING to a slot has somewhere to live: the list
		// offers ten configs and Save settings below only ever writes slot 0,
		// so a slot could be loaded that nothing in this OSD could write.
		OsdWrite(m++, " Configs                  \x16", menusub == 9, 0);
		// Keeps its own row and its single press. It is the one of the two
		// that gets used, and putting it behind a submenu to tidy the page
		// would be paying for the tidying with the common case.
		OsdWrite(m++, " Save settings", menusub == 10, 0);

		for (int i = m; i < OsdGetSize() - 2; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(OsdGetSize() - 2, " Reset", menusub == 11, 0);
		OsdWrite(OsdGetSize() - 1, STD_EXIT, menusub == 12, 0);

		menustate = MENU_AMIGACD_MAIN2;

		// A .mgl launched this core. MENU_MINIMIG_MAIN1 has had this since
		// floppies, but it jumps straight here for the console variant and
		// breaks before reaching it, so an .mgl mounted nothing at all on
		// AmigaCD -- the OSD opened invisibly and sat there until the item's
		// delay ran out. That is the standard way every other MiSTer CD core
		// is launched: a game list, a favourites folder, bootcore.
		//
		// index picks the drive, and 0..3 keep meaning floppies so an .mgl
		// written for Minimig still works:
		//
		//   0, 1   df0, df1        (2, 3 are drives this machine does not have)
		//   4      the CD
		//   5, 6   Hard Disk 0, Hard Disk 1
		//
		// type is not checked, matching MENU_MINIMIG_MAIN1: every .mgl in the
		// wild writes type="f" and refusing the others would only break files
		// that work today.
		//
		// Nothing here resets the machine. The .mgl format has <reset/> for
		// that, so it stays the author's decision -- and a CD does not need it
		// anyway: the akiko bridge sees cd_is_mounted() go false -> true and
		// handles the TOC rebuild and its own deferred reset itself.
		if (!mgl->done)
		{
			const int idx = mgl->item[mgl->current].index;
			if (idx == 0 || idx == 1)
			{
				amigacd_fdd_unit = idx;
				menustate = MENU_AMIGACD_ADFFILE_SELECTED;
			}
			else if (idx == 4 && amigacd_has_cd)
			{
				menustate = MENU_AMIGACD_CDFILE_SELECTED;
			}
			else if (idx == 5 || idx == 6)
			{
				amigacd_hdf_slot = idx - 4;     // 5 -> hardfile[1], 6 -> hardfile[2]
				menustate = MENU_AMIGACD_HDFFILE_SELECTED;
			}
			else
			{
				// An index this machine has no drive for -- either an index
				// with no drive behind it at all, or the CD on the A1200
				// profile, which has no drive. Skip the item rather than
				// stall: state 3 advances to the next one, so the rest of the
				// .mgl still runs and a typo costs one mount, not the launch.
				printf("MGL: AmigaCD (%s) has no drive at index %d, skipping\n",
				       amigacd_system_name(amigacd_system()), idx);
				mgl->state = 3;
				menustate = MENU_NONE1;
			}
		}
		break;
	}

	case MENU_AMIGACD_MAIN2:
		// The deadline has to be checked by something that runs, not by the
		// next keypress -- the whole point is that there may not be one. A
		// switch that lands here redraws, because it changes which rows the
		// page has: the A1200 has no CD row.
		if (amigacd_system_tick())
		{
			menustate = MENU_AMIGACD_MAIN1;
			break;
		}

		if (menu)
		{
			// Leaving with a change still pending applies it now. The row has
			// been showing that machine; walking away from it must not leave a
			// different one running.
			amigacd_system_flush();
			menustate = MENU_NONE1;
		}
		else if (c == KEY_BACKSPACE && menusub == 0) // eject CD
		{
			minimig_config.hardfile[0].filename[0] = 0;
			ide_open(0, "");
			menustate = MENU_AMIGACD_MAIN1;
		}
		// left/right normally page out to the stock MiSTer menus, but on the
		// floppy rows they choose the external drive. Let those through here;
		// the else-if chain then never reaches the paging branches.
		else if (select || recent || minus || plus || ((left || right) && (menusub == 2 || menusub == 3)))
		{
			if (menusub == 0) // CD: always (re)select a disc image
			{
				if (select || recent)
				{
					fs_Options = SCANO_DIR | SCANO_UMOUNT;
					fs_MenuSelect = MENU_AMIGACD_CDFILE_SELECTED;
					fs_MenuCancel = MENU_AMIGACD_MAIN1;
					strcpy(fs_pFileExt, "ISOCUECHDIMG");
					if (select)
					{
						// Never seed the browser's start path with the physical
						// drive sentinel -- it is not a path and SelectFile
						// would try to open it as a directory.
						if (!Selected_S[0][0]
						    && strcmp(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL))
							memcpy(Selected_S[0], minimig_config.hardfile[0].filename, sizeof(Selected_S[0]));
						SelectFile(Selected_S[0], fs_pFileExt, fs_Options, fs_MenuSelect,
						           fs_MenuCancel, AMIGACD_RECENT_CD);
					}
					else if (recent_open(AMIGACD_RECENT_CD)) menustate = MENU_RECENT1;
				}
			}
			else if (menusub == 1) // Real CD: mount the physical drive
			{
				if (select)
				{
					// Swapping in from a mounted CD image is invisible to the
					// Akiko bridge. akiko_cd32_poll acts only on a
					// cd_is_mounted() edge, and image -> physical is
					// mounted -> mounted, so nothing fires: no akiko_build_toc(),
					// no mediachange push, and the BIOS carries on serving the
					// previous disc's geometry. Both ide_open calls below run
					// inside this one HandleUI pass, so the poll loop never
					// observes the gap between them either.
					//
					// Unmount first so the image is torn down through the normal
					// path, then mount the drive, then restart the Amiga so it
					// boots from it. A saved config carrying a CHD in
					// hardfile[0] is the usual way to land here.
					// Asking for it by hand always earns a fresh attempt, even
					// if the watcher has latched off on an unreadable disc.
					physical_disc_autoboot_reset();

					bool had_image = minimig_config.hardfile[0].filename[0]
					              && strcmp(minimig_config.hardfile[0].filename,
					                        PHYSICAL_DISC_SENTINEL);
					// Physical -> physical is edge-less for the same reason, so a
					// disc swap needs the reset too. There are no multi-disc CD32
					// titles, so re-selecting Real CD always means "boot what is in
					// the tray now" rather than "continue with a second disc".
					bool had_phys  = ide_inst[0].drive[0].phys;
					bool need_reset = had_image || had_phys;

					if (had_image)
					{
						minimig_config.hardfile[0].filename[0] = 0;
						ide_open(0, "");
					}

					strcpy(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL);
					minimig_config.hardfile[0].cfg = 2;
					if (ide_open(0, minimig_config.hardfile[0].filename))
					{
						if (need_reset)
						{
							// minimig_reset re-runs ApplyConfiguration ->
							// hdd_open -> ide_open, which re-reads the TOC
							// without re-spinning the drive: physical_disc_open
							// returns early when the same device is already open.
							Info("Real CD mounted - resetting", 1500);
							menustate = MENU_NONE1;
							minimig_reset();
						}
						else
						{
							// Cold mount with nothing previously loaded. Here the
							// bridge does see cd_is_mounted() go false -> true and
							// handles it itself: TOC rebuild, plus its own
							// deferred reset for the boot-with-no-CD case
							// (phase 33-E).
							Info("Physical CD mounted", 1500);
							menustate = MENU_AMIGACD_MAIN1;
						}
					}
					else
					{
						// Leave the sentinel in place. It means "the physical
						// drive is selected", not "a disc is mounted", and the
						// CD row above renders the difference. Clearing it here
						// would make selecting the drive with an empty tray
						// impossible, and that is exactly the state you want to
						// be able to sit in before putting a disc in.
						// Two different failures reached this line. A drive
						// with an empty tray is a disc problem; no drive at all
						// is not, and telling someone to check the disc when
						// there is nowhere to put one wastes their time.
						if (!amigacd_optical_drive_present())
							Info("No CD-ROM drive found", 2500);
						else
							Info("No readable disc", 2000);
						menustate = MENU_AMIGACD_MAIN1;
					}
				}
			}
			else if (menusub == 2 || menusub == 3) // Floppy df0 / df1
			{
				int fu = menusub - 2;
				if (left || right || minus || plus)
				{
					// The shortcut. LEFT/RIGHT is the binding that matters:
					// minus/plus are the keyboard -/+ keys only, so on a gamepad
					// this was unreachable. Kept alongside the explicit rows on
					// the Drives page -- removing a working shortcut to reward
					// the new page would be a regression for anyone using it.
					amigacd_cycle_extdrive(fu, right || plus);
					menustate = MENU_AMIGACD_MAIN1;
				}
				// SELECT and the recent key both open a chooser, whether or not
				// a disk is already in. Upstream ejects instead when one is
				// (menu.cpp:6860), and this page inherited that: with a disk in
				// df0 the recent key ejected it rather than listing the disks
				// you had used -- which is the one moment that list is worth
				// having, since swapping to disk 2 of a four-disk game is what
				// it is for. Two presses to swap, and the second one could not
				// reach the list at all.
				//
				// Ejecting is not lost: Backspace and A+B still do it, on this
				// row and on every other mountable row, which is what the row's
				// hint says and what the CD row above has always done.
				else if (select || recent)
				{
					// The external-drive assignment is dropped where the image
					// arrives, not here. Clearing it before a browser that can
					// be cancelled loses the assignment for a press the user
					// took back -- the same mistake as clearing df[fu].status
					// here, and it was sitting in the same three lines.
					ioctl_index = 0;
					amigacd_fdd_unit = fu;
					// The disk in the drive stays in it until another is
					// chosen. Clearing status here would eject on a browser
					// CANCEL, which now matters: this branch is reachable with
					// a disk mounted. InsertFloppy sets status itself.
					fs_Options = SCANO_DIR;
					fs_MenuSelect = MENU_AMIGACD_ADFFILE_SELECTED;
					fs_MenuCancel = MENU_AMIGACD_MAIN1;
					// IPF only when the CAPS decoder actually loaded, matching
					// what upstream does -- otherwise the browser offers a format
					// that cannot open. DMS needs nothing loaded: it is unpacked
					// to an ADF at insert (support/minimig/dms.cpp).
					const char *fext = caps_init() ? "ADFDMSSCPIPF" : "ADFDMSSCP";
					strcpy(fs_pFileExt, fext);
					if (select) SelectFile(Selected_F[fu], fext, fs_Options, fs_MenuSelect,
					                       fs_MenuCancel, AMIGACD_RECENT_DF(fu));
					else if (recent_open(AMIGACD_RECENT_DF(fu))) menustate = MENU_RECENT1;
				}
			}
			else if (menusub == 4 || menusub == 5) // Hard Disk 0 / 1: select an HDF
			{
				if (select || recent)
				{
					// Which row is being filled has to survive the file
					// browser, which comes back on its own menustate long
					// after this branch has returned.
					amigacd_hdf_slot = (menusub - 4) + 1;   // hardfile[1] or [2]
					fs_Options = SCANO_DIR | SCANO_UMOUNT;
					fs_MenuSelect = MENU_AMIGACD_HDFFILE_SELECTED;
					fs_MenuCancel = MENU_AMIGACD_MAIN1;
					strcpy(fs_pFileExt, "HDFVHDIMGDSK");
					if (select)
					{
						if (!Selected_S[amigacd_hdf_slot][0])
							memcpy(Selected_S[amigacd_hdf_slot],
							       minimig_config.hardfile[amigacd_hdf_slot].filename,
							       sizeof(Selected_S[amigacd_hdf_slot]));
						SelectFile(Selected_S[amigacd_hdf_slot], fs_pFileExt, fs_Options,
						           fs_MenuSelect, fs_MenuCancel,
						           AMIGACD_RECENT_HDF(amigacd_hdf_slot));
					}
					else if (recent_open(AMIGACD_RECENT_HDF(amigacd_hdf_slot))) menustate = MENU_RECENT1;
				}
			}
			else if (menusub == 6) // System: cycle CD32 -> CDTV -> A500 -> A1200
			{
				if (select || minus || plus)
				{
					// minus steps backwards so the row behaves like every
					// other multi-value row in this menu.
					//
					// Stepping from the SHOWN machine, not the running one, so
					// a second press while an apply is pending moves on from
					// what the row says rather than jumping back.
					int old_sys = amigacd_system_shown();
					int sys = minus ? (old_sys + AMIGACD_SYS_COUNT - 1) % AMIGACD_SYS_COUNT
					                : (old_sys + 1) % AMIGACD_SYS_COUNT;

					// The work is deferred -- see amigacd_system_arm(). Three
					// presses to cross the list should cost one switch, not
					// three, and the two machines passed through do not need
					// their configs written and read at all.
					amigacd_system_arm(sys);
					menustate = MENU_AMIGACD_MAIN1;
				}
			}
			else if (menusub == 7 && select) // Settings submenu
			{
				menusub = 0;
				menustate = MENU_AMIGACD_SETTINGS1;
			}
			else if (menusub == 8 && select) // Advanced submenu
			{
				menusub = 0;
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == 10 && select) // Save settings -> default config slot 0
			{
				// Persists the whole minimig_config: mounted CD (hardfile[0]),
				// the two hard disks, RAM, CPU/chipset (System profile),
				// D-Cache and ROMs. minimig_cfg_load(0) restores it on the next
				// core load, and ApplyConfiguration()->hdd_open() re-mounts
				// everything. Also updates this machine's own file, so the
				// System row hands back these settings rather than older ones.
				amigacd_cfg_save_current();
				minimig_mgl_save();
				Info("Settings saved", 1500);
				menustate = MENU_AMIGACD_MAIN1;
			}
			else if (menusub == 9 && select) // Configs
			{
				menustate = MENU_AMIGACD_CONFIGS1;
				menusub = 0;
			}
			else if (menusub == 11 && select) // Reset
			{
				// Asks first. The row is pinned second from the bottom, one
				// row above exit, and what it throws away is everything since
				// the last save state -- on a machine whose save states are
				// refused outright on two of its four profiles.
				//
				// The keyboard reset is deliberately NOT routed through here.
				// Ctrl+LAlt+RAlt is a three-key chord nobody presses by
				// accident, and it is the binding you reach for when the Amiga
				// is wedged and this menu is the thing that will not open.
				menustate = MENU_AMIGACD_RESET1;
				menusub = 0;                 // land on No
			}
			else if (menusub == 12 && select) // Exit
			{
				menustate = MENU_NONE1;
			}
		}
		else if (c == KEY_BACKSPACE && (menusub == 4 || menusub == 5)) // eject a hard disk
		{
			const int hslot = (menusub - 4) + 1;
			minimig_config.hardfile[hslot].filename[0] = 0;
			minimig_config.hardfile[hslot].cfg = 0;
			ide_open(hslot, "");
			menustate = MENU_AMIGACD_MAIN1;
		}
		else if (c == KEY_BACKSPACE && (menusub == 2 || menusub == 3)) // eject floppy
		{
			int fu = menusub - 2;
			df[fu].status = 0;
			FileClose(&df[fu].file);
			menustate = MENU_AMIGACD_MAIN1;
		}
		else if (right) // -> stock MiSTer "System" menu (load cores / reboot)
		{
			menustate = MENU_COMMON1;
			menusub = 0;
		}
		else if (left) // -> stock MiSTer "Misc. Options" menu (volume / info)
		{
			menustate = MENU_MISC1;
			menusub = 3;
		}
		break;

	case MENU_AMIGACD_SETTINGS1:
	{
		OsdSetTitle("Settings", 0);
		{
			// The save state hotkeys were announced on this page's own
			// helptext, which had to name all three at once. Each now sits on
			// the row it belongs to.
			static const char *const tips[] =
			{
				/* 0 FastRAM  */ "Zorro II and Zorro III fast RAM. Z3 and 68000 both stop save states working, and grey those rows out when selected.",
				/* 1 CPU      */ "The machine's 68020, or a 68030 at full speed, with or without its MMU. No FPU. Save states work on both.",
				/* 2 CPU Turbo*/ "Run the 68020 at full speed rather than throttled to the stock rate. Only available when the CPU is an 020.",
				/* 3 Main ROM */ "The Kickstart. Amiga Forever ROMs work as they are, encrypted, as long as ROM.KEY sits beside them.",
				/* 4 Ext ROM  */ "The extended ROM, which a CD32 needs to boot a disc. Minus clears the pairing.",
				/* 5 Save slot*/ "Which of four save state slots to use. Shows when each was written, or that it is empty. Ctrl+Alt+F3 steps it without the menu.",
				/* 6 Save     */ "Capture the machine into the selected slot. Ctrl+Alt+F1 does it without opening the menu, which is the point of it.",
				/* 7 Restore  */ "Put the selected slot back. Ctrl+Alt+F2 does it without the menu. Refused when there is no disc or HDF to name the save after.",
				/* 8 CD speed */ "Fast is the rate this core has always used. CD32 (2x) is what the real machine does: four times the loading, four times the margin.",
				/* 9 Clear NVR*/ "Erase the CD32 saved games for the disc in the drive. Not a save state. Asks first, and the machine needs a reset to see it.",
				/*10 Video/A  */ "The Amiga's own video and audio: PAL or NTSC, stereo mix, the LED filter, Paula output, and screen centring.",
				/*11 back     */ "Back to the main page.",
			};
			static_assert(sizeof(tips) / sizeof(tips[0]) == rowhint_rows(AMIGACD_SETTINGS_MENUMASK),
				"AmigaCD settings page: one hint per selectable row");
			helptext_idx = amigacd_rowhint(tips, sizeof(tips) / sizeof(tips[0]), menusub);
		}
		// FastRAM(0), CPU(1), CPU Turbo(2), Main ROM(3), Ext ROM(4),
		// Save slot(5), Save state(6), Restore state(7), CD speed(8),
		// Clear NVRAM(9), Video & Audio(10), Back(11) -- twelve rows, 0xFFF.
		//
		// The merge that brought CPU Turbo in from develop left this page
		// half-renumbered: the render drew Save slot on the same index as Ext
		// ROM, and the handler had two menusub == 7 branches, Restore state
		// and CD speed, so CD speed could never be changed. Every index below
		// Turbo is +1 from the layout without it, all the way through.
		menumask = AMIGACD_SETTINGS_MENUMASK;
		parentstate = menustate;

		m = 0;
		OsdWrite(m++, "", 0, 0);

		strcpy(s, " FastRAM  : ");
		strcat(s, config_memory_fast_msg[minimig_fast_z3(minimig_config.cpu)][((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5)]);
		OsdWrite(m++, s, menusub == 0, 0);

		// CPU, on the line D-Cache had: the core no longer reads that bit
		// (upstream 26eb46a uses the data cache where the real CPU has one).
		// The machine's own 68020, or a 68030 accelerator -- the SX-32 Pro put
		// one in a CD32, the Blizzards in an A1200 -- with or without its MMU
		// (upstream 60a868f: one clock, the maximum). The 68000 machines
		// (A500, CDTV) keep their 68000; greyed for them.
		strcpy(s, " CPU      : ");
		if ((minimig_config.cpu & 0x23) == 0x22) strcat(s, "68030 no MMU");
		else strcat(s, ((minimig_config.cpu & 0x23) == 0x23) ? "68EC020" : config_cpu_msg[minimig_config.cpu & 0x03]);
		OsdWrite(m++, s, menusub == 1, !(minimig_config.cpu & 0x2));

		// CPU Turbo = CPU byte bit5 read the other way up: the bit SET engages
		// the stock-speed throttle, so Turbo On is the bit clear. The 68020
		// only: on the 68030 bit 5 is "no MMU", which the CPU row above sets.
		strcpy(s, " CPU Turbo: ");
		strcat(s, ((minimig_config.cpu & 0x03) != 0x03) ? "-" : (minimig_config.cpu & 0x20) ? "OFF" : "ON");
		OsdWrite(m++, s, menusub == 2, (minimig_config.cpu & 0x03) != 0x03);

		// No blank between the CPU rows and the ROMs. Row budget: this page
		// draws fifteen lines before Back on line 15 -- eleven selectable, the
		// NVRAM status line, and three spacers. A fourth spacer put Video &
		// Audio on line 15, where Back was drawn over it: the cursor still
		// stopped there, on nothing, and SELECT opened the video page from a
		// row nobody could see. Count the lines before adding one.
		strcpy(s, " Main ROM : ");
		{
			const char *root = getRootDir();
			int rlen = strlen(root);
			char *name = minimig_config.kickstart;
			if (!strncasecmp(name, root, rlen) && name[rlen] == '/') name += rlen + 1;
			if (!name[0]) strcat(s, "<none>");
			else strncat(s, name, 22);
		}
		OsdWrite(m++, s, menusub == 3, 0);

		strcpy(s, " Ext ROM  : ");
		{
			const char *name = minimig_get_extrom();
			if (!name[0]) strcat(s, "<none>");
			else
			{
				const char *root = getRootDir();
				int rlen = strlen(root);
				const char *disp = name;
				if (!strncasecmp(name, root, rlen) && name[rlen] == '/') disp += rlen + 1;
				strncat(s, disp, 22);
			}
		}
		OsdWrite(m++, s, menusub == 4, 0);

		// Save states. These rows were originally added to MENU_MINIMIG_CHIPSET1,
		// which the AmigaCD console variant never reaches -- menu.cpp:5859 sends
		// it straight to MENU_AMIGACD_MAIN1 and its own menu tree. The rows
		// existed but were unreachable on the only variant that wanted them.
		//
		// All three grey out together on minimig_ss_blocked(): a restore is
		// refused in exactly the configurations a save is (see that function),
		// and the core enforces the same lockout in RTL regardless.
		OsdWrite(m++, "", 0, 0);
		{
			int ss_blocked = minimig_ss_blocked();

			{
				char slotinfo[24];
				amigacd_ss_slot_label(minimig_config.ss_slot & 3, slotinfo, sizeof(slotinfo));
				sprintf(s, " Save slot: %s  %s",
				        config_ss_slot_msg[minimig_config.ss_slot & 3], slotinfo);
			}
			OsdWrite(m++, s, menusub == 5, ss_blocked);

			amigacd_ss_preview(menusub == 5 && !ss_blocked, minimig_config.ss_slot & 3);

			OsdWrite(m++, " Save state", menusub == 6, ss_blocked);
			OsdWrite(m++, " Restore state", menusub == 7, ss_blocked);
		}

		// How fast sectors reach the machine. Grouped with the NVRAM rows
		// below rather than with the CPU rows above because it is about the
		// drive, and greyed out on the A1200 profile, which has no drive at
		// all -- same reasoning as the CD rows on the main page.
		OsdWrite(m++, "", 0, 0);
		{
			const int has_cd = (amigacd_system() != AMIGACD_SYS_A1200);
			// Range-checked rather than trusted: this byte arrives from a file,
			// and the row has to render something rather than read past the
			// end of the table if the file is old, hand-edited or truncated.
			const int spd = (minimig_config.cd_speed < mmcd_count)
			              ? (int)minimig_config.cd_speed : (int)mmcd_fast;
			strcpy(s, " CD speed : ");
			strcat(s, config_cd_speed_msg[spd]);
			OsdWrite(m++, s, menusub == 8, !has_cd);
		}

		// Per-game NVRAM. A CD32 save lives in a file named after a hash of
		// the disc, in a directory nobody browses, and nothing anywhere said
		// which one was live -- so "my save vanished" had no answer short of
		// ssh. Worse, when a load fails its verify pass the writes stop
		// happening on purpose (see cd_save_load_failed in akiko_cd32.cpp) and
		// the player is never told; that is what the "not saving!" marker is.
		{
			char nvr_path[256];
			int  nvr_exists = 0, nvr_failed = 0;
			const int have_nvr = akiko_cd32_nvram_info(nvr_path, sizeof(nvr_path),
			                                           &nvr_exists, &nvr_failed);

			strcpy(s, " NVRAM    : ");
			if (!have_nvr) strcat(s, "* no disc *");
			else if (nvr_failed) strcat(s, "NOT SAVING - load failed");
			else
			{
				// The basename is the identifying part and the directory is
				// always the same one, so the path is worth nothing here and
				// the 32-column line has no room for it either.
				const char *base = strrchr(nvr_path, '/');
				base = base ? base + 1 : nvr_path;
				strncat(s, base, 20);
				if (!nvr_exists) strcat(s, " (new)");
			}
			OsdWrite(m++, s, 0, 1);

			// Greyed rather than hidden when there is nothing to clear: the row
			// keeps its position, and a row that is there and dim says "not
			// now" where a row that vanishes says "you imagined it".
			OsdWrite(m++, " Clear NVRAM", menusub == 9, !have_nvr);
		}

		// The Amiga's own video and audio: TV standard, stereo mix, the LED
		// filter, Paula output, screen centring.
		//
		// That page has existed all along and this core could not reach it.
		// MENU_MINIMIG_VIDEO1 is routed from one place -- MENU_MINIMIG_MAIN2's
		// row 9 -- and the console variant jumps to MENU_AMIGACD_MAIN1 at the
		// head of MENU_MINIMIG_MAIN1 and breaks first, so every one of those
		// settings was stranded. TV Standard was reachable only by holding F1
		// or F2 while the core came up (minimig_cfg_load), and the stereo mix
		// -- the one an Amiga on headphones actually needs -- not at all.
		//
		// Same root cause as the .mgl gap, and as the CD32 pad mode, which was
		// patched by deriving it from the system profile rather than by giving
		// the OSD a way in (minimig_config.cpp, "never shows that row"). This
		// is the way in.
		OsdWrite(m++, " Video & Audio            \x16", menusub == 10, 0);

		for (int i = m; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 11, 0);

		menustate = MENU_AMIGACD_SETTINGS2;
		break;
	}

	case MENU_AMIGACD_SETTINGS2:
		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left)
		{
			menustate = MENU_AMIGACD_MAIN1;
			// The Settings row, which is 7 -- CD, Real CD, two floppies, two
			// hard disks, System, Settings. This said 4, which was right
			// before the two Hard Disk rows went in and has been landing the
			// cursor on Hard Disk 0 ever since.
			menusub = 7;
		}
		else if (select || minus || plus)
		{
			if (menusub == 0) // FastRAM cycle (mirrors CHIPSET2 menusub 5)
			{
				// Upstream 60a868f's order: none 2M 4M 8M 256M 384M.
				int fc = (((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5));
				if (minus)
				{
					fc--;
					if (fc < 0) fc = 5;
					if (!minimig_fast_z3(minimig_config.cpu) && fc > 3) fc = 3;
				}
				else
				{
					fc++;
					if (fc > 5) fc = 0;
					if (!minimig_fast_z3(minimig_config.cpu) && fc > 3) fc = 0;
				}
				minimig_config.memory = ((fc << 4) & 0x30) | ((fc << 5) & 0x80) | (minimig_config.memory & ~0xB0);
				menustate = MENU_AMIGACD_SETTINGS1;
			}
			else if (menusub == 1) // CPU: 68020, 68030, 68030 no MMU
			{
				// For the 68020 machines only; the A500 and CDTV keep their
				// 68000. The 68020 is entered Turbo On, this fork's default;
				// the 68030 with and without its MMU are upstream 60a868f's
				// last two steps, in their order.
				if (minimig_config.cpu & 0x2)
				{
					static const unsigned char steps[3] = { 0x03, 0x02, 0x22 };
					int at = ((minimig_config.cpu & 3) == 3) ? 0 :
					         (minimig_config.cpu & 0x20) ? 2 : 1;
					at = (at + (minus ? 2 : 1)) % 3;
					minimig_config.cpu = (minimig_config.cpu & 0x1c) | steps[at];
					minimig_ConfigCPU(minimig_config.cpu);
				}
				menustate = MENU_AMIGACD_SETTINGS1;
			}
			else if (menusub == 2) // CPU Turbo (CPU bit5 inverted; 020 only)
			{
				if ((minimig_config.cpu & 0x3) == 0x3)
				{
					minimig_config.cpu ^= 0x20;
					minimig_ConfigCPU(minimig_config.cpu);
				}
				menustate = MENU_AMIGACD_SETTINGS1;
			}
			else if (menusub == 3 && select) // Main ROM
			{
				ioctl_index = 1;
				SelectFile(Selected_F[4], "ROM", SCANO_DIR, MENU_AMIGACD_ROMFILE_SELECTED, MENU_AMIGACD_SETTINGS1);
			}
			else if (menusub == 4) // Ext ROM ('-' clears the pairing)
			{
				if (minus)
				{
					minimig_set_extrom((char *)"");
					menustate = MENU_AMIGACD_SETTINGS1;
				}
				else if (select)
				{
					ioctl_index = 1;
					SelectFile(Selected_F[5], "ROM", SCANO_DIR | SCANO_UMOUNT, MENU_AMIGACD_EXTROMFILE_SELECTED, MENU_AMIGACD_SETTINGS1);
				}
			}
			else if (menusub == 5 && !minimig_ss_blocked()) // Save slot 1-4
			{
				int slot = minimig_config.ss_slot & 3;
				slot = minus ? (slot + 3) & 3 : (slot + 1) & 3;
				minimig_config.ss_slot = (unsigned char)slot;
				menustate = MENU_AMIGACD_SETTINGS1;
			}
			else if (menusub == 6 && select && !minimig_ss_blocked()) // Save state
			{
				// Arming the host poller happens here and nowhere earlier.
				//
				// process_ss() only ever armed itself from
				// user_io_file_tx(..., opensave) or the arcade MRA path.
				// Minimig uploads its Kickstart by a different route and
				// reaches neither, so the poller's enabled flag stayed 0 and a
				// state the core wrote into its DDR3 window was never
				// persisted. Arming from the CD mount path instead was tried
				// and the core would not boot -- that path runs during core
				// init. The user's explicit save press is the latest possible
				// moment, when everything is up.
				//
				// See minimig_ss_request_save(): it arms, asks, and refuses
				// rather than pressing on when there is no name to save under.
				minimig_ss_request_save();
				// Close the OSD rather than returning to the Settings page.
				//
				// The outcome arrives as a toast ~250 ms later, and Info()
				// draws only while menustate <= MENU_INFO (== 2). Staying on
				// this page leaves menustate at MENU_AMIGACD_SETTINGS/MAIN,
				// far above it, so every toast this feature produces was
				// silently discarded -- measured on hardware: "Save state:
				// saved" and "Save state: FAILED" both logged as DROPPED at
				// menustate=98, and the video-mode toasts with them.
				//
				// Closing also matches what the press means. The user asked
				// the machine to do something and wants to watch it happen,
				// not sit in a menu while it does.
				menustate = MENU_NONE1;
			}
			else if (menusub == 7 && select && !minimig_ss_blocked()) // Restore state
			{
				// This is not housekeeping, it is the load. process_ss() maps
				// the four DDR3 windows AND reads the four .ss files off disk
				// into them (user_io.cpp:1988-2028). Nothing else on this core
				// ever calls it -- the save row above is the only other caller
				// -- so without this the window the core is about to validate
				// holds whatever was there, and a restore after a power cycle,
				// which is the whole point of the feature, could never work.
				//
				// It re-reads all four slots every press, ~16 MB. That is the
				// cost of always validating the freshest file on disk rather
				// than a stale window, on a keypress the user made deliberately.
				// It also resets the poller's counters, so it cannot itself
				// trigger a spurious write back to disk.
				// Close the OSD BEFORE the load, not after it.
				//
				// process_ss() now draws a progress bar while it reads the four
				// slots, and ProgressMessage -> InfoMessage is behind the same
				// menustate <= MENU_INFO gate as every other message here. Left
				// on this page (menustate 98) the bar would be dropped exactly
				// the way the save state toasts were, and the user would be
				// back to watching a frozen menu for several seconds.
				menustate = MENU_NONE1;

				// The load itself, and its refusal when there is nothing
				// mounted to name a save after, both live in
				// minimig_ss_request_restore() -- shared with the Ctrl+Alt+F2
				// hotkey so the two paths cannot drift. Closing the OSD above
				// is also what lets its Info() and process_ss()'s progress bar
				// draw at all.
				minimig_ss_request_restore();
			}
			else if (menusub == 8) // CD speed
			{
				// -/+ and SELECT all cycle it: two values, so there is nothing
				// for a direction to mean, and SELECT is what a controller can
				// reach without the keyboard.
				if (amigacd_system() != AMIGACD_SYS_A1200)
				{
					// CYCLE, do not toggle. This flipped between two values, so
					// when a third was added the row could not reach it: pressing
					// it from "Drive (asked)" went to "CD32 (2x)" and then just
					// alternated, and the new mode was unselectable from the OSD
					// even though the label for it was right there.
					minimig_config.cd_speed =
						(unsigned char)((minimig_config.cd_speed + 1) % mmcd_count);
				}
				menustate = MENU_AMIGACD_SETTINGS1;
			}
			else if (menusub == 9 && select) // Clear NVRAM
			{
				// Confirmed, because this deletes somebody's saves. The row is
				// greyed when there is nothing to clear, but a greyed row still
				// takes SELECT, so re-check rather than trusting the drawing.
				if (akiko_cd32_nvram_info(0, 0, 0, 0))
				{
					menusub = 0;   // default the confirm page to "No"
					menustate = MENU_AMIGACD_NVRCLEAR1;
				}
			}
			else if (menusub == 10 && select) // Video & Audio
			{
				menustate = MENU_MINIMIG_VIDEO1;
				menusub = 0;
			}
			else if (menusub == 11 && select) // Back
			{
				menustate = MENU_AMIGACD_MAIN1;
				menusub = 7;   // Settings row -- 5 was Hard Disk 1, see above
			}
		}
		break;

	// Loading and saving whole configurations.
	//
	// Its own page rather than two more rows on a main page that is exactly
	// full, and rather than the single Load row it replaces. That row opened
	// the shared list straight away, which was fine until you noticed the list
	// offers ten slots and Save settings only ever writes the first -- so a
	// slot could be loaded that nothing here could put anything in.
	case MENU_AMIGACD_CONFIGS1:
	{
		OsdSetTitle("Configs", 0);
		{
			static const char *const tips[] =
			{
				/* 0 Load     */ "Load one of the saved configs. Restarts the Amiga: the load re-uploads the Kickstart and resets the CPU.",
				/* 1 Save slot*/ "Save these settings to one of ten slots. The first is the startup config, which is what Save settings on the main page writes.",
				/* 2 Defaults */ "Put this machine back to the settings it starts with. Asks first. Leaves the discs and hard disks mounted, and does not save.",
				/* 3 back     */ "Back to the main page.",
			};
			static_assert(sizeof(tips) / sizeof(tips[0]) == rowhint_rows(AMIGACD_CONFIGS_MENUMASK),
				"AmigaCD configs page: one hint per selectable row");
			helptext_idx = amigacd_rowhint(tips, sizeof(tips) / sizeof(tips[0]), menusub);
		}
		menumask = AMIGACD_CONFIGS_MENUMASK;  // Load(0), Save(1), Defaults(2), Back(3)
		parentstate = menustate;

		m = 0;
		OsdWrite(m++, "", 0, 0);
		// (resets) because it does: minimig_cfg_load runs ApplyConfiguration,
		// which halts the CPU, resets it and re-uploads the Kickstart.
		OsdWrite(m++, " Load a config   (resets)", menusub == 0, 0);
		OsdWrite(m++, " Save to a config slot", menusub == 1, 0);

		// The way back out of a config that boots to a black screen.
		// amigacd_apply_system() writes a machine's defaults, but it only ever
		// ran on a SWITCH -- and switching away and back loads that machine's
		// saved config, not its defaults. So a saved config with a bad ROM
		// pairing or RAM the game will not take could only be undone by
		// deleting the file over SSH, from a machine that may well be the one
		// that will not boot.
		sprintf(s, " Reset %s to defaults", amigacd_system_name(amigacd_system_shown()));
		OsdWrite(m++, s, menusub == 2, 0);

		OsdWrite(m++, "", 0, 0);
		OsdWrite(m++, " Save settings on the main", 0, 1);
		OsdWrite(m++, " page writes the startup", 0, 1);
		OsdWrite(m++, " config, which is the first", 0, 1);
		OsdWrite(m++, " slot in both lists.", 0, 1);

		for (int i = m; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 3, 0);

		menustate = MENU_AMIGACD_CONFIGS2;
		break;
	}

	case MENU_AMIGACD_CONFIGS2:
		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left || (select && menusub == 3))
		{
			menustate = MENU_AMIGACD_MAIN1;
			menusub = 9;                 // the Configs row
		}
		else if (select)
		{
			if (menusub == 0)
			{
				menustate = MENU_MINIMIG_LOADCONFIG1;
				menusub = 0;
			}
			else if (menusub == 1)
			{
				menustate = MENU_MINIMIG_SAVECONFIG1;
				menusub = 0;
			}
			else if (menusub == 2)
			{
				// Land on No. This one resets the machine.
				menustate = MENU_AMIGACD_DEFAULTS1;
				menusub = 0;
			}
		}
		break;

	// Confirming the Reset row. Same shape as the two pages below it, and the
	// same argument: no undo on the other side.
	case MENU_AMIGACD_RESET1:
	{
		menumask = 0x03;
		parentstate = menustate;

		m = 0;
		while (m < 4) OsdWrite(m++);
		sprintf(s, "      Restart the %s?", amigacd_system_name(amigacd_system()));
		OsdWrite(m++, s);
		OsdWrite(m++);
		// Name the save state slot only when one could actually be taken.
		// Telling an A500 player to save first, when AmigaCD.sv refuses save
		// states on a 68000, is advice that cannot be followed.
		if (!minimig_ss_blocked())
		{
			OsdWrite(m++, "  Anything since the last", 0, 1);
			OsdWrite(m++, "  save state is lost.", 0, 1);
		}
		else
		{
			OsdWrite(m++, "  Anything unsaved is lost,", 0, 1);
			OsdWrite(m++, "  and this machine cannot", 0, 1);
			OsdWrite(m++, "  take a save state.", 0, 1);
		}
		OsdWrite(m++);
		OsdWrite(m++, "             No", menusub == 0);
		OsdWrite(m++, "             Yes", menusub == 1);
		while (m < OsdGetSize()) OsdWrite(m++);

		menustate = MENU_AMIGACD_RESET2;
		break;
	}

	case MENU_AMIGACD_RESET2:
		if (select && menusub == 1)
		{
			menustate = MENU_NONE1;
			minimig_reset();
		}
		else if (select || menu || back || left)
		{
			menustate = MENU_AMIGACD_MAIN1;
			menusub = 11;                // the Reset row
		}
		break;

	// The MiSTer Floppy board: which real drive each df unit uses, and turbo.
	//
	// Exists because the external-drive assignment had no row at all. It was a
	// LEFT/RIGHT binding on the main page's floppy rows -- the least
	// discoverable control in this OSD, and the per-row hints only made it
	// described rather than visible. FDD Turbo came here from Advanced to sit
	// with it; the main page keeps its mount rows, because mounting an image is
	// the common case and does not belong behind a submenu.
	case MENU_AMIGACD_DRIVES1:
	{
		OsdSetTitle("Drives", 0);

		const int mf = (minimig_config.userport == mmup_misterfloppy);
		uint16_t osdMask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);

		{
			static const char *const tips[] =
			{
				/* 0 Ext df0  */ "Which real drive on the MiSTer Floppy board df0 uses. Off means df0 uses a mounted image instead. Left and right on the main page's Floppy 0 row does the same.",
				/* 1 Ext df1  */ "Which real drive df1 uses. A drive already taken by df0 is skipped, and an IBM/PC cable only carries two.",
				/* 2 FDD Turbo*/ "Load floppies faster than the real drive could. Off is authentic; a few disks that time themselves against the drive need it off.",
				/* 3 back     */ "Back to Advanced.",
			};
			static_assert(sizeof(tips) / sizeof(tips[0]) == rowhint_rows(AMIGACD_DRIVES_MENUMASK),
				"AmigaCD drives page: one hint per selectable row");
			helptext_idx = amigacd_rowhint(tips, sizeof(tips) / sizeof(tips[0]), menusub);
		}
		menumask = AMIGACD_DRIVES_MENUMASK;   // Ext df0(0), Ext df1(1), Turbo(2), Back(3)
		parentstate = menustate;

		m = 0;
		OsdWrite(m++, "", 0, 0);

		// Greyed when the user port is not the floppy board, for the same
		// reason the main page greys the assignment out of its rows: the
		// setting is stored either way but nothing is listening.
		for (int fu = 0; fu < 2; fu++)
		{
			char en[32];
			amigacd_extdrive_name(mf ? minimig_config.floppy.extDrives[fu] : 0,
			                      osdMask, en, sizeof(en));
			sprintf(s, " df%d drive : %.18s", fu, en);
			OsdWrite(m++, s, menusub == (uint32_t)fu, !mf);
		}

		OsdWrite(m++, "", 0, 0);
		sprintf(s, " FDD Turbo : %s", minimig_config.floppy.speed ? "On" : "Off");
		OsdWrite(m++, s, menusub == 2, 0);

		// What is actually in each unit, including whether it can be written.
		// Informational: the mount rows are on the main page and duplicating
		// them here would give two places to change one thing.
		OsdWrite(m++, "", 0, 0);
		for (int fu = 0; fu < 2; fu++)
		{
			strcpy(s, "  ");
			if (df[fu].status & DSK_INSERTED)
			{
				char *p = strrchr(df[fu].name, '/');
				char nm[32] = {};
				snprintf(nm, sizeof(nm), "%s", p ? p + 1 : df[fu].name);
				sprintf(s, "  df%d %.20s", fu, nm);
				// Flux images are read-only by design (InsertFloppy omits
				// DSK_WRITABLE), so the padlock is expected on SCP and IPF --
				// and on DMS, which is unpacked to a copy that is not kept.
				if (df[fu].status & DSK_FLUXMODE)    strcat(s, " \x1B");
				if (!(df[fu].status & DSK_WRITABLE)) strcat(s, " \x17");
			}
			else sprintf(s, "  df%d empty", fu);
			OsdWrite(m++, s, 0, 1);
		}

		if (!mf)
		{
			OsdWrite(m++, "", 0, 0);
			OsdWrite(m++, "  Set the User Port to MiSTer", 0, 1);
			OsdWrite(m++, "  Floppy to use real drives.", 0, 1);
		}

		for (int i = m; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 3, 0);

		menustate = MENU_AMIGACD_DRIVES2;
		break;
	}

	case MENU_AMIGACD_DRIVES2:
		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left || (select && menusub == 3))
		{
			menustate = MENU_AMIGACD_ADVANCED1;
			// The Drives row on Advanced, which moves with SNAC tenancy the
			// same way every row below the port rows does -- and moved once
			// more when CD Mount Delay went in above it, and again for the CD32
			// audio buffer.
			menusub = (minimig_config.userport == mmup_snacpsx) ? 8 : 6;
		}
		else if (select || right || minus || plus)
		{
			if (menusub == 0 || menusub == 1)
			{
				// Re-checked rather than trusting the greying, which is
				// presentation and not a lock.
				if (minimig_config.userport == mmup_misterfloppy)
				{
					amigacd_cycle_extdrive(menusub, !minus);
					menustate = MENU_AMIGACD_DRIVES1;
				}
			}
			else if (menusub == 2)
			{
				// minimig_ConfigFloppy carries drives and speed together, so the
				// drive count has to be passed through unchanged -- the same
				// call the two stranded upstream pages make.
				minimig_config.floppy.speed ^= 1;
				minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
				menustate = MENU_AMIGACD_DRIVES1;
			}
		}
		break;

	// Confirming a "Reset <machine> to defaults". Shaped like the Clear NVRAM
	// page below, and confirmed for the same reason: what is on the other side
	// of it cannot be undone. Settings are not saved games, but a config you
	// spent an evening arriving at is no less gone.
	case MENU_AMIGACD_DEFAULTS1:
	{
		menumask = 0x03;
		parentstate = menustate;

		m = 0;
		while (m < 3) OsdWrite(m++);
		sprintf(s, "   Reset %s settings?", amigacd_system_name(amigacd_system()));
		OsdWrite(m++, s);
		OsdWrite(m++);
		// Say what survives, not just what goes. "Reset to defaults" reads as
		// "lose everything" unless the exceptions are on the page, and the two
		// that matter are the mounted media -- which this does not touch -- and
		// the saved config, which is only rewritten if Save settings is pressed
		// afterwards.
		OsdWrite(m++, "  RAM, ROMs, CPU and IDE go", 0, 1);
		OsdWrite(m++, "  back to this machine's own", 0, 1);
		OsdWrite(m++, "  defaults, and it restarts.", 0, 1);
		OsdWrite(m++);
		OsdWrite(m++, "  Discs and hard disks stay", 0, 1);
		OsdWrite(m++, "  mounted. Nothing is saved", 0, 1);
		OsdWrite(m++, "  until you Save settings.", 0, 1);
		OsdWrite(m++);
		OsdWrite(m++, "             No", menusub == 0);
		OsdWrite(m++, "             Yes", menusub == 1);
		while (m < OsdGetSize()) OsdWrite(m++);

		menustate = MENU_AMIGACD_DEFAULTS2;
		break;
	}

	case MENU_AMIGACD_DEFAULTS2:
		if (select && menusub == 1)
		{
			// The machine actually running, not amigacd_system_shown(): a
			// pending switch is committed before this page can be reached (the
			// main page flushes on every exit), and defaulting to the machine
			// on the row rather than the one in the core is how you reset the
			// wrong one.
			const int sys = amigacd_system();

			// Same call the first-time-on-this-machine path makes, and the same
			// report when a default ROM is missing -- the machine still comes
			// up, as whatever Amiga the old ROM makes it, and nothing else on
			// screen would say so.
			if (!amigacd_apply_system(sys))
			{
				const char *mname = 0;
				amigacd_default_rom_names(sys, &mname, 0);
				char msg[128];
				snprintf(msg, sizeof(msg), "%s ROM %s not found",
				         amigacd_system_name(sys), mname);
				Info(msg, 2500);
			}

			// apply_system pushes CPU, chipset and floppy itself, but memory
			// and the ROMs only take effect through ApplyConfiguration, which
			// is what minimig_reset() runs. Without this the page would claim
			// defaults the running machine does not have.
			menustate = MENU_NONE1;
			minimig_reset();
		}
		else if (select || menu || back || left)
		{
			menustate = MENU_AMIGACD_CONFIGS1;
			menusub = 2;
		}
		break;

	// Confirming a Clear NVRAM. Its own page rather than a press-twice, because
	// the thing on the other side of it is a player's saved games and there is
	// no undo. Shaped like MENU_RECENT3/4, which confirms the other destructive
	// action in this OSD.
	case MENU_AMIGACD_NVRCLEAR1:
	{
		menumask = 0x03;
		parentstate = menustate;

		char nvr_path[256];
		int  nvr_exists = 0;
		const int have_nvr = akiko_cd32_nvram_info(nvr_path, sizeof(nvr_path), &nvr_exists, 0);
		const char *base = "";
		if (have_nvr)
		{
			const char *p = strrchr(nvr_path, '/');
			base = p ? p + 1 : nvr_path;
		}

		m = 0;
		while (m < 4) OsdWrite(m++);
		OsdWrite(m++, "      Erase saved games?");
		OsdWrite(m++);
		// Name the file. "Erase saved games?" with no object is a question
		// about the machine; with the filename it is a question about a disc,
		// which is what it actually is.
		sprintf(s, "  %.28s", base);
		OsdWrite(m++, s, 0, 1);
		if (!nvr_exists) OsdWrite(m++, "  (nothing saved yet)", 0, 1);
		OsdWrite(m++);
		OsdWrite(m++, "             No", menusub == 0);
		OsdWrite(m++, "             Yes", menusub == 1);
		while (m < OsdGetSize()) OsdWrite(m++);

		menustate = MENU_AMIGACD_NVRCLEAR2;
		break;
	}

	case MENU_AMIGACD_NVRCLEAR2:
		if (select && menusub == 1)
		{
			if (akiko_cd32_nvram_clear())
			{
				// No toast on success. The row this returns to now reads
				// "(new)", and that is not a near-enough description of the
				// result -- it is the result: the disc has no save, which is
				// the state a disc that was never saved on is in.
				menustate = MENU_AMIGACD_SETTINGS1;
				menusub = 9;                 // the Clear NVRAM row
			}
			else
			{
				// A failure leaves the row exactly as it was, so it has to be
				// said. Closing the OSD first is what makes it render: Info()
				// is dropped unless menustate <= MENU_INFO (== 2), which is
				// the gate that used to swallow every save state toast.
				menustate = MENU_NONE1;
				Info("NVRAM clear FAILED - see log", 3000);
			}
		}
		else if (select || menu || back || left)
		{
			menustate = MENU_AMIGACD_SETTINGS1;
			menusub = 9;                 // the Clear NVRAM row
		}
		break;

	// Expansions a real CD32/CDTV never had. Separate from Settings so the
	// everyday page stays short, and so anything added here is understood as
	// deviating from the authentic machine rather than configuring it.
	case MENU_AMIGACD_ADVANCED1:
	{
		OsdSetTitle("Advanced", 0);

		int snac = (minimig_config.userport == mmup_snacpsx);
		{
			// Two tables, because the two SNAC port rows only exist while SNAC
			// is the tenant and everything below them shifts when it is. The
			// same reason the Ethernet and Back row numbers are computed from
			// that flag rather than written down once.
			static const char *const tips_snac[] =
			{
				/* 0 User Port*/ "Who owns the user port: MT32-pi, a MiSTer Floppy board, or a PSX SNAC adapter. They share every pin, so only one at a time.",
				/* 1 MT32-pi  */ "Synth, Munt ROM and soundfont for the MT32-pi on the user port. Only selectable while MT32-pi owns it.",
				/* 2 Port 1   */ "What the pad in SNAC port 1 is: off, pad, mouse, both at once, or a light gun.",
				/* 3 Port 2   */ "What the pad in SNAC port 2 is: off, pad, mouse, both at once, or a light gun.",
				/* 4 Ethernet */ "The A2065 Zorro II Ethernet card. Off by default; the FPGA half is there either way.",
				/* 5 CD delay */ "How long a disc already in the drive is held back at power-on. 8 s is measured; Off announces it at once, which fragments chip RAM and breaks Flink.",
				/* 6 Audio buf*/ "How much CD audio is buffered. Normal (46 ms) is closest to the real drive. Large (186 ms) rides out longer MiSTer stalls; audio runs on that long after a pause.",
				/* 7 Fast-IDE */ "Faster IDE transfers on a 68020. Not available on a CDTV, on a 68000, or with the IDE controller off.",
				/* 8 Drives   */ "The MiSTer Floppy board: which real drive each df unit uses, and floppy turbo.",
				/* 9 Write mgl*/ "Write an .mgl beside the mounted CD, or beside the floppy when there is no CD, so the MiSTer menu can launch straight into this core with it and any floppies. Names the core running now.",
				/*10 back     */ "Back to the main page.",
			};
			static const char *const tips_plain[] =
			{
				tips_snac[0],
				tips_snac[1],
				tips_snac[4],
				tips_snac[5],
				tips_snac[6],
				tips_snac[7],
				tips_snac[8],
				tips_snac[9],
				tips_snac[10],
			};
			static_assert(sizeof(tips_snac) / sizeof(tips_snac[0]) == rowhint_rows(AMIGACD_ADV_MENUMASK_SNAC),
				"AmigaCD advanced page with SNAC: one hint per selectable row");
			static_assert(sizeof(tips_plain) / sizeof(tips_plain[0]) == rowhint_rows(AMIGACD_ADV_MENUMASK),
				"AmigaCD advanced page without SNAC: one hint per selectable row");
			helptext_idx = snac
				? amigacd_rowhint(tips_snac, sizeof(tips_snac) / sizeof(tips_snac[0]), menusub)
				: amigacd_rowhint(tips_plain, sizeof(tips_plain) / sizeof(tips_plain[0]), menusub);
		}
		// User Port(0), MT32-pi(1)[, SNAC Port 1(2), Port 2(3)], Ethernet,
		// CD Mount Delay, CD32 audio buffer, Fast-IDE, Drives, Write .mgl, Back
		menumask = snac ? AMIGACD_ADV_MENUMASK_SNAC : AMIGACD_ADV_MENUMASK;
		parentstate = menustate;
		uint16_t osdMask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);

		m = 0;
		OsdWrite(m++, "", 0, 0);

		// The SNAC user port is shared with MT32-pi and MiSTer Floppy:
		// AmigaCD.sv muxes USER_OUT on this and resets mt32pi when it changes,
		// so the three tenants are mutually exclusive by construction.
		strcpy(s, " User Port: ");
		strcat(s, snac                                           ? "PSX SNAC" :
		          (minimig_config.userport == mmup_misterfloppy) ? "MiSTer Floppy" : "MT32-Pi");
		OsdWrite(m++, s, menusub == 0, 0);

		// Board detection, straight from mister_floppy_status in AmigaCD.sv:
		// bit 6 detected, bit 7 IBM/PC vs Shugart, bit 8 cable straight vs
		// crossed. Informational, so drawn unselectable.
		if (minimig_config.userport == mmup_misterfloppy)
		{
			if (osdMask & 64)
			{
				strcpy(s, "   ");
				strcat(s, (osdMask & 256) ? "=" : "X");
				strcat(s, (osdMask & 128) ? " Detected (IBM/PC)" : " Detected (Shugart)");
			}
			else strcpy(s, "   Not Detected");
			OsdWrite(m++, s, 0, 1);
		}

		// Per-port SNAC mode. Pad and mouse are independent signals in
		// userio.v, so both at once is deliberate: mouse in port 1, joystick
		// in port 2, the classic Amiga arrangement.
		// The MT32-pi page, which this OSD could not reach.
		//
		// The row above SELECTS MT32-Pi as the user port's tenant. Its settings
		// -- synth, Munt ROM, soundfont, what it shows on screen -- live on
		// MENU_MT32PI_MAIN1, and that page is routed from MENU_MINIMIG_MAIN2
		// and from the Atari ST tree, neither of which this variant reaches.
		// So the OSD could turn a device on and then offer no way to set it up.
		//
		// Greyed rather than hidden when another tenant is selected: the row
		// explains what the User Port row above it leads to, which is worth
		// seeing before the switch rather than only after.
		OsdWrite(m++, " MT32-pi settings         \x16", menusub == 1,
		         minimig_config.userport != mmup_mp32pi);

		if (snac)
		{
			// Sized to the full 3-bit field the RTL reads, not just the enum's
			// defined range: a hand-edited or corrupted config reaches here
			// through the raw memcpy in the full-size load path, and an
			// out-of-range value must render as text rather than read past the end.
			static const char *modes[8] = { "Off", "Pad", "Mouse", "Pad + Mouse", "Light Gun",
			                                "Invalid", "Invalid", "Invalid" };
			for (int p = 0; p < 2; p++)
			{
				sprintf(s, "   Port %d  : %s", p + 1, modes[minimig_config.snac[p] & 0x07]);
				OsdWrite(m++, s, menusub == (uint32_t)(2 + p), 0);
			}
		}

		OsdWrite(m++, "", 0, 0);

		// A2065 ZorroII Ethernet. Mirrors the row on the full Minimig Chipset
		// page -- same a2065_* calls, same cycle order -- so the two cannot
		// drift apart in behaviour.
		strcpy(s, " Ethernet : ");
		strcat(s, a2065_iface_msg(a2065_get_iface()));
		OsdWrite(m++, s, menusub == (uint32_t)(snac ? 4 : 2), 0);

		OsdWrite(m++, "", 0, 0);

		// How long the first CD media announcement is held at cold boot.
		// Presenting a disc that is already there at power-on is not what the
		// hardware does, and a CD32 title that wants most of chip RAM in one
		// allocation fails because of it. 8 s is the measured value: 6 fails.
		// Off restores the old immediate announce.
		strcpy(s, " CD Mount Delay : ");
		if (!minimig_config.cd_mount_delay) strcat(s, "Off");
		else
		{
			char d[8];
			sprintf(d, "%u s", minimig_config.cd_mount_delay);
			strcat(s, d);
		}
		OsdWrite(m++, s, menusub == (uint32_t)(snac ? 5 : 3), 0);

		// How much CD audio the FPGA buffers ahead of the speaker. Here and
		// not on Settings because the large setting is a deviation from the
		// machine: a real drive buffers nothing. Under the mount delay because
		// both are about the drive, and without a spacer of its own because
		// this page has exactly one line to spare (see the row budget below).
		// Greyed off a CD32: the CDTV feeds the same FIFO but never sets it.
		strcpy(s, " CD32 audio buffer: ");
		strcat(s, (amigacd_cd_audio_buffer() == mmab_large) ? "Large" : "Normal");
		OsdWrite(m++, s, menusub == (uint32_t)(snac ? 6 : 4),
		         amigacd_system() != AMIGACD_SYS_CD32);

		// Fast-IDE, which lives on MENU_MINIMIG_DISK1 -- a page this variant
		// cannot open, and one that would drag in four IDE slot rows the CD and
		// Hard Disk rows already cover. So it gets a row here rather than a
		// route there.
		//
		// The bit is INVERTED: ide_cfg & 0x20 clear means on. Read straight off
		// the Drives page rather than reasoned about, because a setting whose
		// sense is backwards is exactly the one to get wrong from memory.
		//
		// Greyed on the same three conditions that page uses: no IDE, a CDTV
		// (whose bridge is not IDE at all), or a CPU that is not an 020 -- the
		// transfer path this speeds up only exists on the 020.
		OsdWrite(m++, "", 0, 0);
		{
			const int cdtv_on = (minimig_config.chipset & CONFIG_CDTV) != 0;
			const int ide_on  = (minimig_config.ide_cfg & 1) != 0;
			const int ok      = ide_on && !cdtv_on && (minimig_config.cpu & 2);

			strcpy(s, " Fast-IDE : ");
			if (cdtv_on)      strcat(s, "N/A (CDTV)");
			else if (!ide_on) strcat(s, "N/A (no IDE)");
			else if (!(minimig_config.cpu & 2)) strcat(s, "N/A (68000)");
			else strcat(s, (minimig_config.ide_cfg & 0x20) ? "Off" : "On");
			OsdWrite(m++, s, menusub == (uint32_t)(snac ? 7 : 5), !ok);
		}

		// FDD Turbo moved to the Drives page, which is where the rest of the
		// floppy hardware now lives. This row is the way in.
		OsdWrite(m++, " Drives                   \x16", menusub == (uint32_t)(snac ? 8 : 6), 0);

		// Writes a .mgl beside the mounted disc so the MiSTer menu can launch
		// straight into this core with it. It started as a + / - binding
		// announced only in the scrolling helptext, which nobody found; then
		// a row on the main page, where it sat between Configs and Reset as
		// the one thing there that is not reached for while playing. A
		// launcher is set up once per disc, which is this page's job.
		//
		// Greyed with no disc IMAGE, which includes the physical drive being
		// selected: a disc in a real tray has no path to put in a file.
		//
		// Row budget: with SNAC as the tenant this page now fills every line
		// before Back on line 15 -- this row is on line 14. Count that layout,
		// not the one on screen, before adding anything here: one more line
		// puts this row on the last line, where Back is drawn over it.
		OsdWrite(m++, "", 0, 0);
		{
			const int can_mgl = amigacd_mgl_anchor() != 0;
			OsdWrite(m++, " Write .mgl for this disc", menusub == (uint32_t)(snac ? 9 : 7), !can_mgl);
		}

		for (int i = m; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == (uint32_t)(snac ? 10 : 8), 0);

		menustate = MENU_AMIGACD_ADVANCED2;
		break;
	}

	case MENU_AMIGACD_ADVANCED2:
		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left)
		{
			menustate = MENU_AMIGACD_MAIN1;
			menusub = 8;   // Advanced row -- 6 was the System row, see SETTINGS2
		}
		else if (select || minus || plus)
		{
			// Two SNAC port rows only exist while SNAC is the tenant, so
			// Ethernet and Back slide down the menusub list when it is.
			int      snac     = (minimig_config.userport == mmup_snacpsx);
			uint32_t eth_row  = snac ? 4 : 2;
			uint32_t cd_row   = snac ? 5 : 3;
			uint32_t cdda_row = snac ? 6 : 4;   // CD32 audio buffer
			uint32_t ide_row  = snac ? 7 : 5;
			uint32_t fdd_row  = snac ? 8 : 6;   // the Drives submenu row
			uint32_t mgl_row  = snac ? 9 : 7;
			uint32_t back_row = snac ? 10 : 8;

			if (menusub == 0) // User Port: MT32-pi -> MiSTer Floppy -> PSX SNAC
			{
				minimig_config.userport = (mm_userportMode)((minimig_config.userport + 1) % 3);
				minimig_ConfigSnac();
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == 1 && select) // MT32-pi settings
			{
				// Re-checked rather than trusting the greying, which is
				// presentation and not a lock. Opening the page for a device
				// that is not the current tenant would show settings that
				// cannot take effect.
				if (minimig_config.userport == mmup_mp32pi)
				{
					menustate = MENU_MT32PI_MAIN1;
					menusub = 0;
				}
			}
			else if (snac && (menusub == 2 || menusub == 3)) // SNAC port 1/2 mode
			{
				uint32_t p = menusub - 2;
				minimig_config.snac[p] = (minimig_config.snac[p] + 1) % 5;
				minimig_ConfigSnac();
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == eth_row) // Ethernet interface
			{
				// OFF -> eth0 -> eth1 -> macvlan -> tap0, skipping anything this
				// box cannot support (no second NIC, no tun driver, no macvlan).
				// OFF and eth0 always qualify, so the walk always lands.
				int m2 = a2065_get_iface();
				for (int i = 0; i < A2065_MODES; i++)
				{
					m2 = minus ? (m2 + A2065_MODES - 1) % A2065_MODES
					           : (m2 + 1) % A2065_MODES;
					if (a2065_mode_available(m2)) break;
				}
				a2065_set_iface(m2);
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == cd_row) // CD mount delay, 0 (Off) .. AMIGACD_CD_DELAY_MAX
			{
				int d = minimig_config.cd_mount_delay;
				if (d > AMIGACD_CD_DELAY_MAX) d = AMIGACD_CD_DELAY_DEFAULT;
				d = minus ? (d + AMIGACD_CD_DELAY_MAX) % (AMIGACD_CD_DELAY_MAX + 1)
				          : (d + 1) % (AMIGACD_CD_DELAY_MAX + 1);
				minimig_config.cd_mount_delay = (unsigned char)d;
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == cdda_row) // CD32 audio buffer: Normal <-> Large
			{
				// Two values, so -/+ and SELECT all just flip it. Takes effect
				// at the next PLAY: the FIFO is resized as a play starts, never
				// under one. Re-checked rather than trusting the greying.
				if (amigacd_system() == AMIGACD_SYS_CD32)
				{
					minimig_config.cd_audio_buffer =
						(amigacd_cd_audio_buffer() == mmab_large) ? mmab_normal : mmab_large;
				}
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == ide_row && select) // Fast-IDE
			{
				// The same three conditions the row greys on, re-checked:
				// greying is presentation, and toggling a bit that has no
				// effect on this machine would still persist into the config.
				const int cdtv_on = (minimig_config.chipset & CONFIG_CDTV) != 0;
				const int ide_on  = (minimig_config.ide_cfg & 1) != 0;
				if (ide_on && !cdtv_on && (minimig_config.cpu & 2))
				{
					minimig_config.ide_cfg ^= 0x20;
					menustate = MENU_AMIGACD_ADVANCED1;
				}
			}
			else if (menusub == fdd_row && select) // Drives
			{
				menustate = MENU_AMIGACD_DRIVES1;
				menusub = 0;
			}
			else if (menusub == mgl_row && select) // Write .mgl for this disc
			{
				char mgl[1024];
				const int res = amigacd_write_mgl(mgl, sizeof(mgl));
				if (res)
				{
					// Name the file rather than saying "done": it is written
					// beside the disc, not somewhere this row could imply, and
					// the name is how it will be found again. And say WHICH of
					// the three happened -- "updated" and "wrote" are different
					// promises about a file the user may have edited.
					const char *base = strrchr(mgl, '/');
					char msg[64];
					snprintf(msg, sizeof(msg), "%s %s",
					         (res == MGL_WROTE)   ? "Wrote" :
					         (res == MGL_UPDATED) ? "Updated" : "Already current:",
					         base ? base + 1 : mgl);
					Info(msg, 2500);
				}
				else
				{
					// The row is greyed without a disc image, but a greyed row
					// still takes SELECT, and a write can fail on a full or
					// read-only card with a disc present. One message covers
					// the refusal; the log carries the rest.
					Info("Could not write a .mgl", 2500);
				}
				menustate = MENU_AMIGACD_ADVANCED1;
			}
			else if (menusub == back_row && select) // Back
			{
				menustate = MENU_AMIGACD_MAIN1;
				// The Advanced row is 8. This said 6 -- the System row -- which
				// is the same two-out error the back/left exit above had, from
				// the same cause: both numbers were written before the two Hard
				// Disk rows went in. That one was corrected; this one is the
				// SELECT-on-Back path and was missed.
				menusub = 8;
			}
		}
		break;

	case MENU_AMIGACD_CDFILE_SELECTED:
		// Reached either from the file browser, with selPath already set, or
		// from a .mgl, which supplies it. Everything below is then identical,
		// which is the point: a disc mounted by an .mgl is mounted by the same
		// four lines as one picked by hand.
		if (!mgl->done) mgl_selpath(selPath, sizeof(selPath));
		memcpy(Selected_S[0], selPath, sizeof(Selected_S[0]));
		// Not recorded as "recent" when an .mgl put it there, matching what
		// every other MGL path in this file does: the list is of things you
		// chose in this OSD.
		if (mgl->done) recent_update(SelectedDir, selPath, SelectedLabel, AMIGACD_RECENT_CD);
		{
			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.hardfile[0].filename) - 1) len = sizeof(minimig_config.hardfile[0].filename) - 1;
			memcpy(minimig_config.hardfile[0].filename, selPath, len);
			minimig_config.hardfile[0].filename[len] = 0;
		}
		minimig_config.hardfile[0].cfg = 2;
		ide_open(0, minimig_config.hardfile[0].filename);
		// And the CD32 slot, which is the drive akiko actually reads.
		//
		// cd_find_drive() prefers minimig_cd_drive_get(0) over the IDE drives,
		// and ApplyConfiguration fills that slot from this same hardfile[0]
		// entry -- but only on a reset. Without this line the row text and the
		// per-game save slot changed while the Amiga kept reading the previous
		// disc: picking Simon the Sorcerer II with Chuck Rock loaded showed
		// "CD : simon2-lf.cue" above "CHUCKROCK 1 trk no CDDA" and served Chuck
		// Rock's TOC and root directory for it.
		//
		// Same shape as the asymmetry that keeps biting here: applied on the
		// interactive path and not the load path, or the reverse.
		minimig_cd_drive_open(0, minimig_config.hardfile[0].filename);
		menusub = 0;
		menustate = MENU_AMIGACD_MAIN1;
		if (!mgl->done)
		{
			mgl->state = 3;             // on to the next .mgl item
			menustate = MENU_NONE1;     // and do not draw a menu nobody opened
		}
		break;

	case MENU_AMIGACD_HDFFILE_SELECTED:
		// amigacd_hdf_slot was set by the row that opened the browser: 1 for
		// Hard Disk 0 (IDE0 slave) and 2 for Hard Disk 1 (IDE1 master).
		if (!mgl->done) mgl_selpath(selPath, sizeof(selPath));
		memcpy(Selected_S[amigacd_hdf_slot], selPath, sizeof(Selected_S[0]));
		if (mgl->done) recent_update(SelectedDir, selPath, SelectedLabel, AMIGACD_RECENT_HDF(amigacd_hdf_slot));
		{
			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.hardfile[amigacd_hdf_slot].filename) - 1)
				len = sizeof(minimig_config.hardfile[amigacd_hdf_slot].filename) - 1;
			memcpy(minimig_config.hardfile[amigacd_hdf_slot].filename, selPath, len);
			minimig_config.hardfile[amigacd_hdf_slot].filename[len] = 0;
		}
		minimig_config.hardfile[amigacd_hdf_slot].cfg = 1;   // fixed disk, not a removable slot
		// The drive is invisible unless the IDE controller is on. A CD32
		// profile already has it; CDTV and A500 do not, and mounting an HDF is
		// an unambiguous request for one -- an A590 sidecar in A500 terms.
		minimig_config.ide_cfg |= 1;
		ide_open(amigacd_hdf_slot, minimig_config.hardfile[amigacd_hdf_slot].filename);
		menusub = 4 + (amigacd_hdf_slot - 1);
		menustate = MENU_AMIGACD_MAIN1;
		if (!mgl->done)
		{
			mgl->state = 3;
			menustate = MENU_NONE1;
		}
		break;

	case MENU_AMIGACD_ADFFILE_SELECTED:
		if (!mgl->done) mgl_selpath(selPath, sizeof(selPath));
		memcpy(Selected_F[amigacd_fdd_unit], selPath, sizeof(Selected_F[0]));
		if (mgl->done) recent_update(SelectedDir, selPath, SelectedLabel, AMIGACD_RECENT_DF(amigacd_fdd_unit));
		// Swapping a disk lands here with the old one still open. InsertFloppy
		// calls FileOpenEx straight onto drive->file without closing what was
		// there, so the outgoing descriptor has to be closed here -- it used to
		// be closed by the eject-on-select branch that a swap was forced
		// through. FileClose on a closed file is a no-op, so the first insert
		// into an empty drive is unaffected.
		if (df[amigacd_fdd_unit].status & DSK_INSERTED) FileClose(&df[amigacd_fdd_unit].file);
		// A chosen image overrides any external-drive assignment on this unit.
		// Here rather than at the row, so a cancelled browse keeps it: the two
		// cannot both drive one unit, and it is the image that wins.
		if (minimig_config.floppy.extDrives[amigacd_fdd_unit])
		{
			minimig_config.floppy.extDrives[amigacd_fdd_unit] = 0;
			minimig_ConfigFloppyExt(minimig_config.floppy.extDrives[0], minimig_config.floppy.extDrives[1],
			                        minimig_config.floppy.extDrives[2], minimig_config.floppy.extDrives[3]);
		}
		InsertFloppy(&df[amigacd_fdd_unit], selPath);
		menusub = 2 + amigacd_fdd_unit;  // back to the row that opened the browser
		menustate = MENU_AMIGACD_MAIN1;
		if (!mgl->done)
		{
			mgl->state = 3;
			menustate = MENU_NONE1;
		}
		break;

	case MENU_AMIGACD_ROMFILE_SELECTED:
		memcpy(Selected_F[4], selPath, sizeof(Selected_F[4]));
		minimig_set_kickstart(selPath);
		menustate = MENU_AMIGACD_SETTINGS1;
		break;

	case MENU_AMIGACD_EXTROMFILE_SELECTED:
		memcpy(Selected_F[5], selPath, sizeof(Selected_F[5]));
		minimig_set_extrom(selPath);
		menustate = MENU_AMIGACD_SETTINGS1;
		break;
	// =================== end AmigaCD console variant =========================

	case MENU_MINIMIG_DISK1:
		helptext_idx = HELPTEXT_HARDFILE;
		OsdSetTitle("Drives");

		m = 0;
		parentstate = menustate;
		{
			int cdtv_on = (minimig_config.chipset & CONFIG_CDTV) ? 1 : 0;
			int ide_on  = (minimig_config.ide_cfg & 1) ? 1 : 0;
			int io_on   = ide_on || cdtv_on;

			menumask = 0xC01;
			if (ide_on)  menumask |= 0x002;  // Fast-IDE row (IDE only)
			if (io_on)   menumask |= 0x154;  // slot type rows (IDE or CDTV)
			OsdWrite(m++, "", 0, 0);

			const char *iomode = cdtv_on ? "CDTV" : (ide_on ? "IDE " : "OFF ");
			strcpy(s, " I/O controller    : ");
			strcat(s, iomode);
			OsdWrite(m++, s, menusub == 0, 0);

			strcpy(s, " Fast-IDE (68020)  : ");
			if (cdtv_on) strcat(s, "N/A");
			else         strcat(s, (minimig_config.ide_cfg & 0x20) ? "Off" : "On ");
			OsdWrite(m++, s, menusub == 1, cdtv_on || !ide_on || !(minimig_config.cpu & 2));
			if (!(minimig_config.cpu & 2)) menumask &= ~2;
			OsdWrite(m++);

			uint n = 2, t = 8;
			for (uint i = 0; i < 4; i++)
			{
				if (cdtv_on)
				{
					static const char *cdtv_slot_label[4] = {
						" CD0           : ",
						" HD0  (SCSI)   : ",
						" HD1  (SCSI)   : ",
						" HD2  (SCSI)   : "
					};
					strcpy(s, cdtv_slot_label[i]);
				}
				else
				{
					strcpy(s, (i & 2) ? " Sec. " : " Pri. ");
					strcat(s, (i & 1) ? " Slave: " : "Master: ");
				}
				strcat(s, (minimig_config.hardfile[i].cfg == 2) ? "Removable/CD" : minimig_config.hardfile[i].cfg ? "Fixed/HDD" : "Disabled");
				OsdWrite(m++, s, io_on ? (menusub == n++) : 0, !io_on);
				if (minimig_config.hardfile[i].filename[0])
				{
					strcpy(s, "                                ");
					char *path = HomeDir();
					int len = strlen(path);
					char *name = minimig_config.hardfile[i].filename;
					if (!strncasecmp(name, path, len))  name += len + 1;
					strncpy(&s[3], name, 25);
				}
				else
				{
					strcpy(s, "   ** not selected **");
				}
				enable = io_on && minimig_config.hardfile[i].cfg;
				if (enable) menumask |= t;	// Make hardfile selectable
				OsdWrite(m++, s, menusub == n++, enable == 0);
				t <<= 2;
				if(i == 1) OsdWrite(m++);
			}
		}

		OsdWrite(m++);
		sprintf(s, " Floppy Disk Turbo : %s", minimig_config.floppy.speed ? "On" : "Off");
		OsdWrite(m++, s, menusub == 10, 0);
		OsdWrite(m++);

		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 11, 0);
		menustate = MENU_MINIMIG_DISK2;
		break;

	case MENU_MINIMIG_DISK2:
		saved_menustate = MENU_MINIMIG_DISK1;

		if (select || recent || minus || plus)
		{
			if (menusub == 0)
			{
				if (select || minus || plus)
				{
					// 3-state I/O controller cycle: OFF -> IDE -> CDTV -> OFF.
					// Orchestrates both ide_cfg bit 0 (IDE on) and chipset
					// CONFIG_CDTV (CDTV personality). These are mutually
					// exclusive: real CDTV has no IDE, and IDE-mode keeps
					// the chipset out of CDTV bus layout.
					int cur;
					if (minimig_config.chipset & CONFIG_CDTV)   cur = 2;
					else if (minimig_config.ide_cfg & 1)        cur = 1;
					else                                        cur = 0;
					int next = minus ? ((cur + 2) % 3) : ((cur + 1) % 3);
					if (next == 0)
					{
						minimig_config.ide_cfg &= ~1;
						minimig_config.chipset &= ~CONFIG_CDTV;
					}
					else if (next == 1)
					{
						minimig_config.ide_cfg |= 1;
						minimig_config.chipset &= ~CONFIG_CDTV;
					}
					else
					{
						minimig_config.ide_cfg &= ~1;
						minimig_config.chipset |= CONFIG_CDTV;
					}
					minimig_ConfigChipset(&minimig_config);
					menustate = MENU_MINIMIG_DISK1;
				}
			}
			else if (menusub == 1)
			{
				if (select && !(minimig_config.chipset & CONFIG_CDTV))
				{
					minimig_config.ide_cfg ^= 0x20;
					menustate = MENU_MINIMIG_DISK1;
				}
			}
			else if (menusub < 10)
			{
				if (!(menusub & 1))
				{
					if (select || minus || plus)
					{
						int idx = (menusub - 2) / 2;
						if (minus)
						{
							if (!minimig_config.hardfile[idx].cfg) minimig_config.hardfile[idx].cfg = 2;
							else minimig_config.hardfile[idx].cfg--;
						}
						else
						{
							minimig_config.hardfile[idx].cfg++;
							if (minimig_config.hardfile[idx].cfg > 2) minimig_config.hardfile[idx].cfg = 0;
						}
						menustate = MENU_MINIMIG_DISK1;
					}
				}
				else if(select || recent)
				{
					fs_Options = SCANO_DIR | SCANO_UMOUNT;
					fs_MenuSelect = MENU_MINIMIG_HDFFILE_SELECTED;
					fs_MenuCancel = MENU_MINIMIG_DISK1;
					int idx = (menusub - 3) / 2;
					strcpy(fs_pFileExt, (minimig_config.hardfile[idx].cfg == 2) ? "ISOCUECHDIMG" : "HDFVHDIMGDSK");
					if (select)
					{
						if (!Selected_S[idx][0]) memcpy(Selected_S[idx], minimig_config.hardfile[idx].filename, sizeof(Selected_S[idx]));
						SelectFile(Selected_S[idx], fs_pFileExt, fs_Options, fs_MenuSelect, fs_MenuCancel);
					}
					else if (recent_init(500)) menustate = MENU_RECENT1;
				}
			}
			else if (menusub == 10 && select) // return to previous menu
			{
				minimig_config.floppy.speed ^= 1;
				minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
				menustate = MENU_MINIMIG_DISK1;
			}
			else if (menusub == 11 && select) // return to previous menu
			{
				menustate = MENU_MINIMIG_MAIN1;
				menusub = 5;
			}
		}

		if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left)
		{
			menustate = MENU_MINIMIG_MAIN1;
			menusub = 5;
		}
		break;
	case MENU_MINIMIG_HDFFILE_SELECTED:
		{
			memcpy(Selected_S[(menusub - 7) / 2], selPath, sizeof(Selected_S[(menusub - 2) / 2]));
			recent_update(SelectedDir, selPath, SelectedLabel, 500);
			int num = (menusub - 7) / 2;
			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.hardfile[num].filename) - 1) len = sizeof(minimig_config.hardfile[num].filename) - 1;
			if(len) memcpy(minimig_config.hardfile[num].filename, selPath, len);
			minimig_config.hardfile[num].filename[len] = 0;

			if (ide_is_placeholder(num))
			{
				ide_open(num, minimig_config.hardfile[num].filename);
			}

			menustate = MENU_MINIMIG_DISK1;
		}
		break;

	case MENU_MINIMIG_CD32FILE_SELECTED:
		{
			memcpy(Selected_CD32, selPath, sizeof(Selected_CD32));
			recent_update(SelectedDir, selPath, SelectedLabel, 501);

			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.cd32_drive.filename) - 1) len = sizeof(minimig_config.cd32_drive.filename) - 1;
			if (len) memcpy(minimig_config.cd32_drive.filename, selPath, len);
			minimig_config.cd32_drive.filename[len] = 0;
			minimig_cd_drive_open(0, minimig_config.cd32_drive.filename);
			menustate = MENU_MINIMIG_DISK1;
		}
		break;

	case MENU_MINIMIG_CD32FILE_START:
		{
			if (!mgl->done)
			{
				const char *p = mgl->item[mgl->current].path;
				if (p[0] == '/') snprintf(selPath, sizeof(selPath), "%s", p);
				else if (p[0] == '.' && p[1] == '.') snprintf(selPath, sizeof(selPath), "%s/%s", getRootDir(), p);
				else snprintf(selPath, sizeof(selPath), "%s/%s", HomeDir(), p);
			}

			if (!selPath[0])
			{
				minimig_config.cd32_drive.filename[0] = 0;
				minimig_cd_drive_open(0, "");
				menustate = MENU_MINIMIG_MAIN1;

				break;
			}
			memcpy(Selected_CD32, selPath, sizeof(Selected_CD32));
			if (mgl->done) recent_update(SelectedDir, selPath, SelectedLabel, 501);

			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.cd32_drive.filename) - 1) len = sizeof(minimig_config.cd32_drive.filename) - 1;
			if (len) memcpy(minimig_config.cd32_drive.filename, selPath, len);
			minimig_config.cd32_drive.filename[len] = 0;
			minimig_cfg_set(CONFIG_PRESET_CD32);
			minimig_cd_drive_open(0, minimig_config.cd32_drive.filename);
			menustate = MENU_NONE1;
			if (!mgl->done) mgl->state = 3;
			minimig_reset();
		}
		break;

	case MENU_MINIMIG_CDTVFILE_SELECTED:
		{
			memcpy(Selected_CDTV, selPath, sizeof(Selected_CDTV));
			recent_update(SelectedDir, selPath, SelectedLabel, 502);

			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.cdtv_drive.filename) - 1) len = sizeof(minimig_config.cdtv_drive.filename) - 1;
			if (len) memcpy(minimig_config.cdtv_drive.filename, selPath, len);
			minimig_config.cdtv_drive.filename[len] = 0;
			minimig_cd_drive_open(1, minimig_config.cdtv_drive.filename);
			menustate = MENU_MINIMIG_DISK1;
		}
		break;

	case MENU_MINIMIG_CDTVFILE_START:
		{
			if (!mgl->done)
			{
				const char *p = mgl->item[mgl->current].path;
				if (p[0] == '/') snprintf(selPath, sizeof(selPath), "%s", p);
				else if (p[0] == '.' && p[1] == '.') snprintf(selPath, sizeof(selPath), "%s/%s", getRootDir(), p);
				else snprintf(selPath, sizeof(selPath), "%s/%s", HomeDir(), p);
			}

			if (!selPath[0])
			{
				minimig_config.cdtv_drive.filename[0] = 0;
				minimig_cd_drive_open(1, "");
				menustate = MENU_MINIMIG_MAIN1;

				break;
			}
			memcpy(Selected_CDTV, selPath, sizeof(Selected_CDTV));
			if (mgl->done) recent_update(SelectedDir, selPath, SelectedLabel, 502);

			uint len = strlen(selPath);
			if (len > sizeof(minimig_config.cdtv_drive.filename) - 1) len = sizeof(minimig_config.cdtv_drive.filename) - 1;
			if (len) memcpy(minimig_config.cdtv_drive.filename, selPath, len);
			minimig_config.cdtv_drive.filename[len] = 0;
			minimig_cfg_set(CONFIG_PRESET_CDTV);
			minimig_cd_drive_open(1, minimig_config.cdtv_drive.filename);
			menustate = MENU_NONE1;
			if (!mgl->done) mgl->state = 3;
			minimig_reset();
		}
		break;

	case MENU_MINIMIG_VIDEO1:
		menumask = 0x3fff;
		parentstate = menustate;
		helptext_idx = 0; // helptexts[HELPTEXT_VIDEO];

		m = 0;
		OsdSetTitle("Audio & Video");

		// The page is full: Drive Sounds (upstream e836aea) took the blank that
		// stood under TV Standard, as the per-mode position line further down
		// took the one above Adjust. Fifteen lines and Back.
		strcpy(s, " TV Standard   : ");
		strcat(s, minimig_config.chipset & CONFIG_NTSC ? "NTSC" : "PAL");
		OsdWrite(m++, s, menusub == 0, 0);
		strcpy(s, " Scandoubler FX: ");
		strcat(s, config_scanlines_msg[minimig_config.scanlines & 7]);
		OsdWrite(m++, s, menusub == 1, 0);
		strcpy(s, " Video area by : ");
		strcat(s, config_blank_msg[(minimig_config.scanlines >> 6) & 3]);
		OsdWrite(m++, s, menusub == 2, 0);
		strcpy(s, " Aspect Ratio  : ");
		minimig_config.scanlines = (get_ar_name((minimig_config.scanlines >> 4) & 3, s) << 4) | (minimig_config.scanlines & ~0x30);
		OsdWrite(m++, s, menusub == 3, 0);
		strcpy(s, " Pixel Clock   : ");
		strcat(s, (minimig_get_extcfg() & 0x400) ? "Adaptive" : "28MHz");
		OsdWrite(m++, s, menusub == 4, 0);
		strcpy(s, " Scaling       : ");
		strcat(s,config_scale[(minimig_get_extcfg() >> 11) & 7]);
		OsdWrite(m++, s, menusub == 5, 0);
		strcpy(s, " RTG Upscaling : ");
		strcat(s, (minimig_get_extcfg() & 0x4000) ? "HV-Integer" : "Normal");
		OsdWrite(m++, s, menusub == 6, 0);

		OsdWrite(m++);
		strcpy(s, " Stereo mix    : ");
		strcat(s, config_stereo_msg[minimig_config.audio & 3]);
		OsdWrite(m++, s, menusub == 7, 0);
		strcpy(s, " Audio Filter  : ");
		strcat(s, (~minimig_get_extcfg() & 0x10000) ? "Auto(LED)" : (minimig_get_extcfg() & 0x8000) ? "On" : "Off");
		OsdWrite(m++, s, menusub == 8, 0);
		strcpy(s, " Model         : ");
		strcat(s, (minimig_get_extcfg() & 0x20000) ? "A1200" : "A500");
		OsdWrite(m++, s, menusub == 9, 0);
		strcpy(s, " Paula Output  : ");
		strcat(s, (minimig_get_extcfg() & 0x40000) ? "PWM" : "Normal");
		OsdWrite(m++, s, menusub == 10, 0);
		strcpy(s, " Drive Sounds  : ");
		strcat(s, minimig_dsnd_loaded() ? config_dsnd_msg[(minimig_get_extcfg() >> 19) & 3] : "No file");
		OsdWrite(m++, s, menusub == 11, 0);

		// The separator that was here is spent on this instead. The page is
		// exactly full -- fifteen lines and Back -- so the line had to come from
		// somewhere, and a blank is worth less than the one fact this row does
		// not otherwise tell you: the stored position is PER VIDEO MODE.
		//
		// Without it, adjusting the picture on a CD32 boot screen and then
		// finding a game off-centre reads as the adjustment not having saved.
		// It did save -- for the mode the boot screen was in. See
		// minimig_vadjust_info().
		{
			char vinfo[40];
			minimig_vadjust_info(vinfo, sizeof(vinfo));
			sprintf(s, "  %.29s", vinfo);
			OsdWrite(m++, s, 0, 1);
		}
		OsdWrite(m++, minimig_get_adjust() ? " Finish screen adjustment" : " Adjust screen position (grid)", menusub == 12, 0);
		for (; m < OsdGetSize() - 1; m++) OsdWrite(m);
		OsdWrite(OsdGetSize() - 1, STD_BACK, menusub == 13, 0);

		menustate = MENU_MINIMIG_VIDEO2;
		break;

	case MENU_MINIMIG_VIDEO2:
		saved_menustate = MENU_MINIMIG_VIDEO1;
		if (select || minus || plus)
		{
			menustate = MENU_MINIMIG_VIDEO1;
			switch(menusub)
			{
			case 0:
				minimig_config.chipset ^= CONFIG_NTSC;
				minimig_ConfigChipset(&minimig_config);
				break;

			case 1:
				{
					int scanlines = minimig_config.scanlines & 7;
					if (minus)
					{
						scanlines--;
						if (scanlines < 0) scanlines = 4;
					}
					else
					{
						scanlines++;
						if (scanlines > 4) scanlines = 0;
					}
					minimig_config.scanlines = scanlines | (minimig_config.scanlines & 0xf8);
					minimig_ConfigVideo(minimig_config.scanlines);
				}
				break;

			case 2:
				minimig_config.scanlines &= ~0x80;
				minimig_config.scanlines ^= 0x40;
				minimig_ConfigVideo(minimig_config.scanlines);
				break;

			case 3:
				minimig_config.scanlines = (next_ar((minimig_config.scanlines >> 4) & 3, minus) << 4) | (minimig_config.scanlines & ~0x30);
				minimig_ConfigVideo(minimig_config.scanlines);
				break;

			case 4:
				minimig_set_extcfg(minimig_get_extcfg() ^ 0x400);
				break;

			case 5:
				{
					int mode = (minimig_get_extcfg() >> 11) & 7;
					if (minus) mode = (mode <= 0) ? 4 : (mode - 1);
					else mode = (mode >= 4) ? 0 : (mode + 1);
					minimig_set_extcfg((minimig_get_extcfg() & ~0x3800) | (mode << 11));
				}
				break;

			case 6:
				minimig_set_extcfg(minimig_get_extcfg() ^ 0x4000);
				break;

			case 7:
				minimig_config.audio = (minimig_config.audio + (minus ? -1 : 1)) & 3;
				minimig_ConfigAudio(minimig_config.audio);
				break;

			case 8:
				{
					int mode = (minimig_get_extcfg() >> 15) & 3;
					if (minus) mode = (mode == 2) ? 0 : (mode - 1);
					else mode = (mode == 0) ? 2 : (mode + 1);
					minimig_set_extcfg((minimig_get_extcfg() & ~0x18000) | ((mode & 3) << 15));
				}
				break;

			case 9:
				minimig_set_extcfg(minimig_get_extcfg() ^ 0x20000);
				break;

			case 10:
				minimig_set_extcfg(minimig_get_extcfg() ^ 0x40000);
				break;

			case 11:
				if (minimig_dsnd_loaded())
				{
					int mode = (minimig_get_extcfg() >> 19) & 3;
					mode = (mode + (minus ? 3 : 1)) & 3;
					minimig_set_extcfg((minimig_get_extcfg() & ~0x180000) | (mode << 19));
				}
				break;

			case 12:
				if (select)
				{
					menustate = MENU_NONE1;
					minimig_set_adjust(minimig_get_adjust() ? 0 : 1);
				}
				break;

			case 13:
				if (select) amigacd_video_back();
				break;
			}
		}
		else if (menu)
		{
			menustate = MENU_NONE1;
		}
		else if (back || left)
		{
			amigacd_video_back();
		}
		break;

		/******************************************************************/
		/* system menu */
		/******************************************************************/
	case MENU_SYSTEM1:
		if (video_fb_state())
		{
			menustate = MENU_NONE1;
			break;
		}

		OsdSetSize(16);
		helptext_idx = 0;
		parentstate = menustate;

		m = 0;
		OsdSetTitle("System Settings", OSD_ARROW_LEFT);
		menumask = 0x7F;

		OsdWrite(m++);
		sprintf(s, "       MiSTer v%s", version + 5);
		{
			char str[8] = {};
			FILE *f = fopen("/MiSTer.version", "r");
			if (f)
			{
				if (fread(str, 6, 1, f)) sprintf(s, " MiSTer v%s,  OS v%s", version + 5, str);
				fclose(f);
			}
		}

		OsdWrite(m++, s);

		{
			uint64_t avail = 0;
			struct statvfs buf;
			memset(&buf, 0, sizeof(buf));
			if (!statvfs(getRootDir(), &buf)) avail = buf.f_bsize * buf.f_bavail;
			if(avail < (10ull*1024*1024*1024)) sprintf(s, "   Available space: %llumb", avail / (1024 * 1024));
			else sprintf(s, "   Available space: %llugb", avail / (1024 * 1024 * 1024));
			OsdWrite(m+2, s, 0, 0);
		}

		OsdWrite(m++, "");
		if (getStorage(0))
		{
			OsdWrite(m++, "        Storage: USB");
			m++;
			OsdWrite(m++, "      Switch to SD card", menusub == 0);
		}
		else
		{
			if (getStorage(1))
			{
				OsdWrite(m++, " No USB found, using SD card");
				m++;
				OsdWrite(m++, "      Switch to SD card", menusub == 0);
			}
			else
			{
				OsdWrite(m++, "      Storage: SD card");
				m++;
				OsdWrite(m++, "        Switch to USB", menusub == 0, !isUSBMounted());
			}
		}
		OsdWrite(m++, "");
		OsdWrite(m++, " Remap keyboard            \x16", menusub == 1);
		OsdWrite(m++, " Define joystick buttons   \x16", menusub == 2);
		OsdWrite(m++, " Scripts                   \x16", menusub == 3);
		OsdWrite(m++, " Help                      \x16", menusub == 4);
		OsdWrite(m++, "");
		cr = m;
		OsdWrite(m++, " Reboot (hold \x16 cold reboot)", menusub == 5);
		sysinfo_timer = 0;

		reboot_req = 0;

		while(m < OsdGetSize()-1) OsdWrite(m++, "");
		OsdWrite(15, STD_EXIT, menusub == 6);
		menustate = MENU_SYSTEM2;
		break;

	case MENU_SYSTEM2:
		if (menu)
		{
			SelectFile("", 0, SCANO_CORES, MENU_CORE_FILE_SELECTED1, MENU_SYSTEM1);
			break;
		}
		else if (select)
		{
			switch (menusub)
			{
			case 0:
				if (getStorage(1) || isUSBMounted()) setStorage(!getStorage(1));
				break;

			case 1:
				start_map_setting(0);
				menustate = MENU_KBDMAP;
				menusub = 0;
				break;

			case 2:
				menustate = MENU_JOYSYSMAP;
				break;

			case 3:
				{
					uint8_t confirm[32] = {};
					int match = 0;
					int fd = open("/sys/block/mmcblk0/device/cid", O_RDONLY);
					if (fd >= 0)
					{
						int ret = read(fd, card_cid, 32);
						close(fd);
						if (ret == 32)
						{
							if (FileLoadConfig("script_confirm", confirm, 32))
							{
								match = !memcmp(card_cid, confirm, 32);
							}
						}
					}

					if (match) SelectFile(Selected_F[0], "SH", SCANO_DIR, MENU_SCRIPTS_FB, MENU_SYSTEM1);
					else
					{
						menustate = MENU_SCRIPTS_PRE;
						menusub = 0;
					}
				}
				break;

			case 4:
				strcpy(Selected_tmp, DOCS_DIR);
				FileCreatePath(Selected_tmp);
				SelectFile(Selected_tmp, "PDFTXTMD ", SCANO_DIR | SCANO_TXT, MENU_DOC_FILE_SELECTED, MENU_NONE1);
				break;

			case 5:
				{
					reboot_req = 1;

					int off = hold_cnt / 3;
					if (off > 5) reboot(1);

					sprintf(s, " Cold Reboot");
					p = s + 5 - off;
					MenuWrite(cr, p, 1, 0);
				}
				break;

			case 6:
				menustate = MENU_NONE1;
				break;
			}
		}
		else if (left)
		{
			menustate = MENU_MISC1;
		}

		if (!hold_cnt && reboot_req) fpga_load_rbf("menu.rbf");
		break;

	case MENU_JOYSYSMAP:
		strcpy(joy_bnames[SYS_BTN_A - DPAD_NAMES], "A");
		strcpy(joy_bnames[SYS_BTN_B - DPAD_NAMES], "B");
		strcpy(joy_bnames[SYS_BTN_X - DPAD_NAMES], "X");
		strcpy(joy_bnames[SYS_BTN_Y - DPAD_NAMES], "Y");
		strcpy(joy_bnames[SYS_BTN_L - DPAD_NAMES], "L");
		strcpy(joy_bnames[SYS_BTN_R - DPAD_NAMES], "R");
		strcpy(joy_bnames[SYS_BTN_SELECT - DPAD_NAMES], "Select");
		strcpy(joy_bnames[SYS_BTN_START - DPAD_NAMES], "Start");
		strcpy(joy_bnames[SYS_MS_RIGHT - DPAD_NAMES], "Mouse Move RIGHT");
		strcpy(joy_bnames[SYS_MS_LEFT - DPAD_NAMES], "Mouse Move LEFT");
		strcpy(joy_bnames[SYS_MS_DOWN - DPAD_NAMES], "Mouse Move DOWN");
		strcpy(joy_bnames[SYS_MS_UP - DPAD_NAMES], "Mouse Move UP");
		strcpy(joy_bnames[SYS_MS_BTN_L - DPAD_NAMES], "Mouse Btn Left");
		strcpy(joy_bnames[SYS_MS_BTN_R - DPAD_NAMES], "Mouse Btn Right");
		strcpy(joy_bnames[SYS_MS_BTN_M - DPAD_NAMES], "Mouse Btn Middle");
		strcpy(joy_bnames[SYS_MS_BTN_EMU - DPAD_NAMES], "Mouse Emu/Sniper");
		strcpy(joy_bnames[SYS_BTN_OSD_KTGL - DPAD_NAMES], "Menu");
		strcpy(joy_bnames[SYS_BTN_CNT_OK - DPAD_NAMES], "Menu: OK");
		strcpy(joy_bnames[SYS_BTN_CNT_ESC - DPAD_NAMES], "Menu: Back");
		joy_bcount = 20 + 1; //buttons + OSD/KTGL button
		start_map_setting(joy_bcount + 6); // + dpad + Analog X/Y
		menustate = MENU_JOYDIGMAP;
		menusub = 0;
		break;

	case MENU_LGCAL:
		helptext_idx = 0;
		OsdSetTitle("Lightgun Calibration", 0);
		for (int i = 0; i < OsdGetSize(); i++) OsdWrite(i);
		OsdWrite(9,  "     Point to the edge of");
		OsdWrite(10, "   screen and press trigger");
		OsdWrite(11, "         to confirm");
		OsdWrite(OsdGetSize() - 1, "           Cancel", menusub == 0, 0);
		gun_ok = 0;
		gun_side = 0;
		gun_y = 0;
		gun_x = 0;
		memset(gun_pos, 0, sizeof(gun_pos));
		menustate = MENU_LGCAL1;
		menusub = 0;
		break;

	case MENU_LGCAL1:
		{
			static int state = 0;
			static uint32_t blink = 0;
			if (!blink || CheckTimer(blink))
			{
				blink = GetTimer(300);
				state = !state;
			}

			m = !state;
		}

		if (gun_side < 4) gun_pos[gun_side] = (gun_side < 2) ? gun_y : gun_x;
		sprintf(s, "           %c%04d%c", (gun_side == 0 && m) ? 17 : 32, (gun_side == 0) ? gun_y : gun_pos[0], (gun_side == 0 && m) ? 16 : 32);
		OsdWrite(0, s);
		sprintf(s, "%c%04d%c                 %c%04d%c", (gun_side == 2 && m) ? 17 : 32, (gun_side == 2) ? gun_x : gun_pos[2], (gun_side == 2 && m) ? 16 : 32,
		                                                (gun_side == 3 && m) ? 17 : 32, (gun_side == 3) ? gun_x : gun_pos[3], (gun_side == 3 && m) ? 16 : 32);
		OsdWrite(7, s);
		sprintf(s, "           %c%04d%c", (gun_side == 1 && m) ? 17 : 32, (gun_side == 1) ? gun_y : gun_pos[1], (gun_side == 1 && m) ? 16 : 32);
		OsdWrite(13, s);
		if (menu || select) menustate = MENU_NONE1;

		if (gun_ok == 1)
		{
			gun_ok = 0;
			gun_side++;
		}

		if (gun_ok == 2)
		{
			gun_ok = 0;
			if (gun_side == 4)
			{
				input_lightgun_save(gun_idx, gun_pos);
				menustate = MENU_NONE1;
			}
		}
		break;

	case MENU_SCRIPTS_PRE:
		OsdSetTitle("Warning!!!", 0);
		helptext_idx = 0;
		menumask = 7;
		m = 0;
		OsdWrite(m++);
		OsdWrite(m++, "         Attention:");
		OsdWrite(m++, " This is dangerous operation!");
		OsdWrite(m++);
		OsdWrite(m++, " Script has control over the");
		OsdWrite(m++, " whole system and may damage");
		OsdWrite(m++, " the files or settings, then");
		OsdWrite(m++, " MiSTer won't boot, so you");
		OsdWrite(m++, " will have to re-format the");
		OsdWrite(m++, " SD card and fill with files");
		OsdWrite(m++, " in order to use it again.");
		OsdWrite(m++);
		OsdWrite(m++, "  Do you want to continue?");
		OsdWrite(m++, "            No", menusub == 0);
		OsdWrite(m++, "            Yes", menusub == 1);
		OsdWrite(m++, "  Yes, and don't ask again", menusub == 2);
		menustate = MENU_SCRIPTS_PRE1;
		parentstate = MENU_SCRIPTS_PRE;
		break;

	case MENU_SCRIPTS_PRE1:
		if (menu) menustate = MENU_SYSTEM1;
		else if (select)
		{
			switch (menusub)
			{
			case 0:
				menustate = MENU_SYSTEM1;
				break;

			case 2:
				FileSaveConfig("script_confirm", card_cid, 32);
				// fall through

			case 1:
				SelectFile(Selected_F[0], "SH", SCANO_DIR, MENU_SCRIPTS_FB, MENU_SYSTEM1);
				break;
			}
		}
		break;

	case MENU_SCRIPTS_FB:
		if (cfg.fb_terminal)
		{
			memcpy(Selected_F[0], selPath, sizeof(Selected_F[0]));
			static char cmd[1024 * 2];
			const char *path = getFullPath(selPath);
			menustate = MENU_SCRIPTS_FB2;
			video_chvt(2);
			video_fb_enable(1);
			vga_nag();
			sprintf(cmd, "#!/bin/bash\nexport LC_ALL=en_US.UTF-8\nexport HOME=/root\ncd $(dirname %s)\n%s\necho \"Press any key to continue\"\n", path, path);

			unlink("/tmp/script");
			FileSave("/tmp/script", cmd, strlen(cmd));
			ttystatus = 0;
			ttypid = fork();
			if (!ttypid)
			{
				execl("/sbin/agetty", "/sbin/agetty", "-a", "root", "-l", "/tmp/script", "--nohostname", "-L", "tty2", "linux", NULL);
				exit(0); //should never be reached
			}
		}
		else
		{
			menustate = MENU_SCRIPTS;
		}
		break;

	case MENU_SCRIPTS_FB2:
		if (ttypid)
		{
			if (waitpid(ttypid, 0, WNOHANG) > 0)
			{
				ttypid = 0;
				user_io_osd_key_enable(1);
			}
		}
		else
		{
			if (c & UPSTROKE)
			{
				video_menu_bg(user_io_status_get("[3:1]"));
				video_fb_enable(0);
				menustate = MENU_SYSTEM1;
				menusub = 3;
				OsdClear();
				OsdEnable(DISABLE_KEYBOARD);
			}
		}
		break;

	case MENU_BTPAIR2:
		if (select || menu)
		{
			menustate = MENU_NONE1;
		}
		break;

	case MENU_BTPAIR:
		OsdSetSize(16);
		OsdEnable(DISABLE_KEYBOARD);
		parentstate = MENU_BTPAIR;
		OsdSetTitle("BT Pairing");
		if (hci_get_route(0) < 0)
		{
			helptext_idx = 0;
			menumask = 1;
			menusub = 0;
			for (int i = 0; i < OsdGetSize() - 1; i++) OsdWrite(i);
			OsdWrite(7, "    No Bluetooth available");
			OsdWrite(OsdGetSize() - 1, STD_EXIT, menusub == 0);
			menustate = MENU_BTPAIR2;
			break;
		}
		//fall through

	case MENU_SCRIPTS:
		helptext_idx = 0;
		menumask = 0;
		menusub = 0;
		if(parentstate != MENU_BTPAIR) OsdSetTitle(flist_SelectedItem()->de.d_name);
		menustate = MENU_SCRIPTS1;
		if (parentstate != MENU_BTPAIR) parentstate = MENU_SCRIPTS;
		for (int i = 0; i < OsdGetSize() - 1; i++) OsdWrite(i);
		OsdWrite(OsdGetSize() - 1, (parentstate == MENU_BTPAIR) ? "           Finish" : "           Cancel", menusub == 0, 0);
		for (int i = 0; i < script_lines; i++) strcpy(script_output[i], "");
		script_line=0;
		script_finished = false;
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(0, &set);
		CPU_SET(1, &set);
		sched_setaffinity(0, sizeof(set), &set);
		if (parentstate == MENU_BTPAIR)
		{
			bt_pairing = true;
			OsdUpdate();
			if(cfg.bt_reset_before_pair) system("hciconfig hci0 reset");
			script_pipe = popen("/usr/sbin/btpair", "r");
		}
		else
		{
			script_pipe = popen(getFullPath(selPath), "r");
		}
		script_file = fileno(script_pipe);
		fcntl(script_file, F_SETFL, O_NONBLOCK);
		break;

	case MENU_SCRIPTS1:
		if (!script_finished)
		{
			if (!feof(script_pipe)) {
				if (fgets(script_line_output, script_line_length, script_pipe) != NULL)
				{
					script_line_output[strcspn(script_line_output, "\n")] = 0;
					if (script_line < OsdGetSize() - 2)
					{
						strcpy(script_output[script_line++], script_line_output);
					}
					else
					{
						strcpy(script_output[script_line], script_line_output);
						for (int i = 0; i < script_line; i++) strcpy(script_output[i], script_output[i+1]);
					};
					for (int i = 0; i < OsdGetSize() - 2; i++) OsdWrite(i, script_output[i], 0, 0);
				};
			}
			else {
				pclose(script_pipe);
				cpu_set_t set;
				CPU_ZERO(&set);
				CPU_SET(1, &set);
				sched_setaffinity(0, sizeof(set), &set);
				script_finished=true;
				OsdWrite(OsdGetSize() - 1, "             OK", menusub == 0, 0);
			};
		};

		if (select || menu || script_finished || c == KEY_BACKSPACE)
		{
			if (!script_finished)
			{
				strcpy(script_command, "killall ");
				strcat(script_command, (parentstate == MENU_BTPAIR) ? "-SIGINT btpair btctl" : flist_SelectedItem()->de.d_name);
				system(script_command);
				FILE *p = script_pipe;
				script_pipe = NULL;
				pthread_t tid;
				if (!pthread_create(&tid, NULL, close_pipe_async, p)) pthread_detach(tid);
				else { printf("close_pipe_async: pthread_create failed\n"); pclose(p); }
				cpu_set_t set;
				CPU_ZERO(&set);
				CPU_SET(1, &set);
				sched_setaffinity(0, sizeof(set), &set);
				script_finished = true;
			};

			if (c == KEY_BACKSPACE && (parentstate == MENU_BTPAIR))
			{
				for (int i = 0; i < OsdGetSize() - 1; i++) OsdWrite(i);
				OsdWrite(7, "   Delete all pairings...");
				OsdUpdate();
				system("/bin/bluetoothd renew");
				menustate = MENU_BTPAIR;
			}
			else
			{
				if (parentstate == MENU_BTPAIR)
				{
					bt_pairing = false;
					menustate = MENU_NONE1;
				}
				else
				{
					menustate = MENU_SYSTEM1;
					menusub = 3;
				}
			}
		}
		break;

	case MENU_KBDMAP:
		helptext_idx = 0;
		menumask = 1;
		OsdSetTitle("Keyboard", 0);
		menustate = MENU_KBDMAP1;
		parentstate = MENU_KBDMAP;
		for (int i = 0; i < OsdGetSize() - 1; i++) OsdWrite(i, "", 0, 0);
		m = 8;
		OsdWrite(m++, info_top, 0, 0);
		infowrite(m++, "");
		infowrite(m++, "Enter \x16 Finish");
		infowrite(m++, " ESC \x16 Clear");
		infowrite(m++, "");
		OsdWrite(m++, info_bottom, 0, 0);
		break;

	case MENU_KBDMAP1:
		if(!get_map_button())
		{
			OsdWrite(3, "     Press key to change", 0, 0);
			s[0] = 0;
			if(get_map_vid())
			{
				sprintf(s, "    on keyboard %04x:%04x", get_map_vid(), get_map_pid());
			}
			OsdWrite(5, s, 0, 0);
			OsdWrite(OsdGetSize() - 1, "           finish", menusub == 0, 0);
		}
		else
		{
			sprintf(s, "  Press key to map 0x%02X to", get_map_button() & 0xFF);
			OsdWrite(3, s, 0, 0);
			OsdWrite(5, "      on any keyboard", 0, 0);
			OsdWrite(OsdGetSize() - 1);
		}

		if (select || menu)
		{
			if (!get_map_vid()) menu = 0;

			OsdWrite(3);
			OsdWrite(4, menu ? "          Clearing" : "          Finishing");
			OsdWrite(5);
			finish_map_setting(menu);
			menu_timeout = GetTimer(1000);
			menustate = MENU_KBDMAP2;
		}
		break;

	case MENU_KBDMAP2:
		if (CheckTimer(menu_timeout))
		{
			menustate = MENU_SYSTEM1;
			menusub = 1;
		}
		break;

	case MENU_CORE_FILE_SELECTED1:
		recent_update(SelectedDir, selPath, SelectedLabel, -1);
		menustate = MENU_NONE1;
		memcpy(Selected_tmp, selPath, sizeof(Selected_tmp));
		if (!getStorage(0)) // multiboot is only on SD card.
		{
			selPath[strlen(selPath) - 4] = 0;
			int off = strlen(SelectedDir);
			if (off) off++;
			int fnum = ScanDirectory(SelectedDir, SCANF_INIT, "TXT", 0, selPath + off);
			if (fnum)
			{
				if (fnum == 1)
				{
					//Check if the only choice is <core>.txt
					strcat(selPath, ".txt");
					if (FileLoad(selPath, 0, 0))
					{
						menustate = MENU_CORE_FILE_SELECTED2;
						break;
					}
				}

				strcpy(selPath, Selected_tmp);
				AdjustDirectory(selPath);
				cp_MenuCancel = fs_MenuCancel;
				strcpy(fs_pFileExt, "TXT");
				fs_ExtLen = 3;
				fs_Options = SCANO_CORES;
				fs_MenuSelect = MENU_CORE_FILE_SELECTED2;
				fs_MenuCancel = MENU_CORE_FILE_CANCELED;
				menustate = MENU_FILE_SELECT1;
				break;
			}
		}

		if (isXmlName(Selected_tmp))
		{
			// find the RBF file from the XML
			xml_load(getFullPath(Selected_tmp));
		}
		else
		{
			fpga_load_rbf(Selected_tmp);
		}
		break;

	case MENU_CORE_FILE_SELECTED2:
		fpga_load_rbf(Selected_tmp, selPath);
		menustate = MENU_NONE1;
		break;

	case MENU_CORE_FILE_CANCELED:
		SelectFile("", 0, SCANO_CORES, MENU_CORE_FILE_SELECTED1, cp_MenuCancel);
		break;

	case MENU_ADVANCED_MAP_LIST1:
	{
		OsdSetTitle("Advanced");
		menu_parse_buttons();
		menustate = MENU_ADVANCED_MAP_LIST2;
		parentstate = MENU_ADVANCED_MAP_LIST1;
		while (1)
		{
			if (!menusub) firstmenu = 0;
			adjvisible = 0;

			advancedButtonMap *abms = get_advanced_map_defs(abm_dev_num);
			menumask = 0x1;
			uint32_t menucnt = 1;
			MenuWrite(0, " New                       \x16", menusub == menucnt++, 0);

			int n = 1;
			size_t map_cnt = 0;
			for (size_t i = 0; i < ADVANCED_MAP_MAX; i++)
			{
				advancedButtonMap *abm = abms + i;
				if (!abm->input_codes[0]) break;
				map_cnt++;
				build_advanced_map_summary(abm, s, sizeof(s));
				s[27] = '\x16';
				s[28] = 0;
				menumask |= 1 << (i + 1);
				MenuWrite(n++, s, menusub == i + 1, 0);
			}
			MenuWrite(0, " New                       \x16", menusub == 0, map_cnt >= ADVANCED_MAP_MAX);
			if (map_cnt >= ADVANCED_MAP_MAX)
				menumask &= ~0x1;
			for (; n < OsdGetSize(); n++) MenuWrite(n, "", 0, 0);
			if (!adjvisible) break;
			firstmenu += adjvisible;
		}
		break;
	}

	case MENU_ADVANCED_MAP_LIST2:
	{
		if (select)
		{
			advancedButtonMap *abms = get_advanced_map_defs(abm_dev_num);
			memset(&abm_edit_map, 0, sizeof(abm_edit_map));
			menustate = MENU_ADVANCED_MAP_EDIT1;
			parentstate = MENU_ADVANCED_MAP_LIST1;
			if (menusub == 0) {
				abm_edit_ptr = &abm_edit_map;
			}
			else {
				abm_edit_ptr = &abms[menusub - 1];
			}
			menusub = 0;
		}

		if (left || back || menu)
		{
			menustate = MENU_COMMON1;
			input_advanced_save(abm_dev_num);
			parentstate = 0;
			menusub = 3;
		}
		break;
	}

	case MENU_ADVANCED_MAP_EDIT1:
	{
		menustate = MENU_ADVANCED_MAP_EDIT2;
		parentstate = MENU_ADVANCED_MAP_EDIT1;
		menumask = 0;
		firstmenu = 0;
		adjvisible = 0;
		bool dev_kbd = false;
		if (abm_edit_ptr->input_codes[0] && abm_edit_ptr->input_codes[0] <= 256)
			dev_kbd = true;
		menu_parse_buttons();

		char bname[32] = {};
		build_advanced_map_core_btn_str(abm_edit_ptr, bname, sizeof(bname));
		bool keyboard_only = dev_kbd && (user_io_get_kbdemu() == EMU_NONE);

		uint32_t n = 0;
		char code_str[256] = {};

		build_advanced_map_code_str(abm_edit_ptr->input_codes, sizeof(abm_edit_ptr->input_codes), code_str, sizeof(code_str));
		snprintf(s, sizeof(s), " Input Hotkey(s) %-20s\x16", code_str);
		MenuWrite(n, s, menusub == n, 0);
		menumask |= 1 << n++;

		code_str[0] = 0;
		snprintf(s, sizeof(s), " Core Button(s): %-17s\x10 \x11", bname);
		MenuWrite(n, s, keyboard_only ? 0 : menusub == n, keyboard_only);
		if (!keyboard_only) menumask |= 1 << n;
		n++;

		build_advanced_map_code_str(abm_edit_ptr->output_codes, sizeof(abm_edit_ptr->output_codes), code_str, sizeof(code_str));
		snprintf(s, sizeof(s), " Output(s): %-20s\x16", code_str);
		MenuWrite(n, s, menusub == n, 0);
		menumask |= 1 << n++;

		const char *af_label = get_autofire_rate_hz(abm_edit_ptr->autofire_idx);
		snprintf(s, sizeof(s), " Autofire : %-20s\x16", af_label);
		MenuWrite(n, s, menusub == n, 0);
		menumask |= 1 << n++;


		MenuWrite(n, " Delete", menusub == n, 0);
		menumask |= 1 << n++;
		MenuWrite(n, " Done", menusub == n, 0);
		menumask |= 1 << n++;
		for (int i = n; i < OsdGetSize(); i++) MenuWrite(i, "", 0, 0);
		break;
	}

	case MENU_ADVANCED_MAP_EDIT2:
	{
		if (select || left || right || plus || minus)
		{
			menustate = MENU_ADVANCED_MAP_EDIT1;
			char bname[32] = { 0 };
			switch (menusub)
			{
			case 1:
			{
				int mapped_button_cnt = 0;
				int first_map_idx = -1;
				for (uint bn = 0; bn < sizeof(abm_edit_ptr->button_mask) * 8; bn++)
				{
					if (abm_edit_ptr->button_mask & 1 << bn)
					{
						mapped_button_cnt++;
						if (first_map_idx == -1) first_map_idx = bn;
					}
				}
				if (select)
				{
					menustate = MENU_ADVANCED_MAP_EDIT3;
					menusub = 0;
				}
				else if (mapped_button_cnt <= 1 && (left || right)) {
					menu_button_name(first_map_idx, bname, sizeof(bname));
					do {
						if (right) first_map_idx++;
						if (left) first_map_idx--;
						if (first_map_idx < 0) first_map_idx = joy_bcount + 3;
						if (first_map_idx - 4 >= joy_bcount) first_map_idx = 0;
						menu_button_name(first_map_idx, bname, sizeof(bname));
					} while (!strncmp("-", bname, sizeof(bname)));
					abm_edit_ptr->button_mask = 1 << first_map_idx;
				}
				break;
			}
			case 0:
			case 2:
				if (select) {
					menustate = MENU_ADVANCED_MAP_CAPTURE1;
					start_map_setting(1, menusub ? 2 : 1, abm_edit_ptr);
				}
				break;

			case 3:
				if (select || plus)
				{
					abm_edit_ptr->autofire_idx++;
				}
				else if (minus) {
					abm_edit_ptr->autofire_idx--;
				}
				if (abm_edit_ptr->autofire_idx >= get_autofire_rate_count())
					abm_edit_ptr->autofire_idx = 0;
				if (abm_edit_ptr->autofire_idx < 0)
					abm_edit_ptr->autofire_idx = get_autofire_rate_count() - 1;
				break;
			case 4:
				if (select)
				{
					menustate = MENU_ADVANCED_MAP_LIST1;
					menusub = 0;
					input_advanced_delete(abm_edit_ptr, abm_dev_num);
				}
				break;
			}
		}

		if (back || menu || (menusub == 5 && select))
		{
			input_advanced_save_entry(abm_edit_ptr, abm_dev_num);
			menustate = MENU_ADVANCED_MAP_LIST1;
			menusub = 0;
		}
		break;
	}

	case MENU_ADVANCED_MAP_EDIT3:
	{
		menustate = MENU_ADVANCED_MAP_EDIT4;
		parentstate = MENU_ADVANCED_MAP_EDIT3;
		while (1) {
			menumask = 0;
			uint32_t n = 0;
			if (!menusub) firstmenu = 0;
			adjvisible = 0;
			for (int i = 0; i < joy_bcount + 4; i++)
			{
				char bname[32];
				menu_button_name(i, bname, sizeof(bname));
				if (!strcmp("-", bname)) continue;
				bool b_used = abm_edit_ptr->button_mask & 1 << i;
				sprintfz(s, "%s %s", b_used ? "*" : " ", bname);
				MenuWrite(n, s, menusub == n, 0);
				menumask |= 1 << n;
				n++;
			}
			if (!adjvisible) break;
			firstmenu += adjvisible;
		}
		break;
	}

	case MENU_ADVANCED_MAP_EDIT4:
	{
		if (back || menu)
		{
			menustate = MENU_ADVANCED_MAP_EDIT1;
			menusub = 1;
		}
		else if (select) {
			menustate = MENU_ADVANCED_MAP_EDIT3;
			uint32_t btn_cnt = 0;
			for (int i = 0; i < joy_bcount + 4; i++)
			{
				char bname[32];
				menu_button_name(i, bname, sizeof(bname));
				if (!strcmp("-", bname)) continue;
				if (menusub == btn_cnt)
				{
					abm_edit_ptr->button_mask ^= 1 << i;
					break;
				}
				btn_cnt++;
			}
		}
		break;
	}

	case MENU_ADVANCED_MAP_CAPTURE1:
	{
		OsdSetTitle("Set Hotkey", 0);
		for (int i = 0; i < 4; i++) OsdWrite(i, "", 0, 0);
		OsdWrite(4, info_top, 0, 0);
		if (get_map_set() == 1 || abm_edit_ptr->input_codes[0] > 256)
		{
			infowrite(5, "Press button(s) on joypad");
			infowrite(6, "or key(s) on keyboard");
		}
		else {
			infowrite(5, "");
			infowrite(6, "Press Keyboard key(s)");
		}
		infowrite(7, "");
		infowrite(8, "Esc \x16 Clear");
		infowrite(9, "Menu-hold \x16 Clear");
		OsdWrite(10, info_bottom, 0, 0);
		char code_str[256] = {};
		build_advanced_map_code_str((get_map_set() == 2) ? abm_edit_ptr->output_codes : abm_edit_ptr->input_codes, sizeof(abm_edit_ptr->input_codes), code_str, sizeof(code_str), 14);
		OsdWrite(11, get_map_cancel() ? "          Clearing" : code_str, 0, 0);

		if (get_map_finish() || get_map_cancel())
		{
			menustate = MENU_ADVANCED_MAP_EDIT1;
			finish_map_setting(get_map_cancel());
		}
		break;
	}

	case MENU_ATARI8BIT_CART1:
		helptext_idx = 0;
		menumask = 0;
		OsdSetSize(16);
		OsdSetTitle("Cartridge Type");
		menustate = MENU_ATARI8BIT_CART2;
		parentstate = MENU_ATARI8BIT_CART1;

		{
			int entry = 0;
			uint32_t selentry = 0;
			int a8bit_match_count = is_atari5200() ? atari5200_get_match_cart_count() : atari800_get_match_cart_count();
			for (int i = 0; i < a8bit_match_count; i++)
			{
				s[0] = ' ';
				strcpy(s + 1, is_atari5200() ? atari5200_get_cart_match_name(i) : atari800_get_cart_match_name(i));
				OsdWrite(entry, s, menusub == selentry);
				menumask = (menumask << 1) | 1;
				entry++;
				selentry++;
			}

			while (entry < OsdGetSize() - 1) OsdWrite(entry++);

			OsdWrite(entry, "           Cancel", menusub == selentry);
			menusub_last = selentry;
			menumask = (menumask << 1) | 1;
		}
		break;

	case MENU_ATARI8BIT_CART2:
		if (menu || left)
		{
			menustate = MENU_GENERIC_MAIN1;
			menusub = ioctl_index - 2;
		}
		else if (select)
		{
			menustate = MENU_NONE1;
			if (menusub != menusub_last)
			{
				int match_index = menusub;
				HandleUI(); // What MenuHide() would do...
				if(is_atari5200())
				{
					atari5200_open_cartridge_file(selPath, match_index);
				}
				else
				{
					atari800_open_cartridge_file(selPath, match_index);
				}
			}
		}
		break;

		/******************************************************************/
		/* we should never come here                                      */
		/******************************************************************/
	default:
		break;
	}

	if (is_menu())
	{
		static unsigned long rtc_timer = 0;
		static int init_wait = 0;

		if (!rtc_timer || CheckTimer(rtc_timer))
		{
			rtc_timer = GetTimer(cfg.bootcore[0] != '\0' ? 100 : 1000);
			char str[64] = { 0 };
			char straux[64];

			if (cfg.bootcore[0] != '\0')
			{
				if (btimeout > 0)
				{
					OsdWrite(12, "\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81\x81");
					snprintf(str, sizeof(str), " Bootcore -> %s", bootcoretype);
					OsdWrite(13, str, 0, 0);
					strcpy(straux, cfg.bootcore);
					sprintf(str, " %s", get_rbf_name_bootcore(straux));

					char s[40];
					memset(s, ' ', 32); // clear line buffer
					s[32] = 0; // set temporary string length to OSD line length

					int len = strlen(str);
					if (len > 28)
					{
						len = 27; // trim display length if longer than 30 characters
						s[28] = 22;
					}

					strncpy(s + 1, str, len); // display only name
					OsdWrite(14, s, 1, 0, 0, (32 * btimeout) / cfg.bootcore_timeout);

					sprintf(str, "   Press any key to cancel");
					OsdWrite(15, str, 0, 0);
					btimeout--;
					if (!btimeout)
					{
						OsdWrite(13, "", 0, 0);
						OsdWrite(14, s, 1, 0, 0, 0);
						sprintf(str, "           Loading...");
						OsdWrite(15, str, 1, 0);
						isXmlName(cfg.bootcore) ? xml_load(getFullPath(cfg.bootcore)) : fpga_load_rbf(cfg.bootcore);
					}
				}
			}

			if (init_wait < 1)
			{
				sprintf(str, "       www.MiSTerFPGA.org       ");
				init_wait++;
			}
			else
			{
				sprintf(str, " MiSTer      ");

				time_t t = time(NULL);
				struct tm tm = *localtime(&t);
				if (tm.tm_year >= 117)
				{
					strftime(str + strlen(str), sizeof(str) - 1 - strlen(str), "%b %d %a%H:%M:%S", &tm);
				}

				int n = 8;
				if (getNet(2)) str[n++] = 0x1d;
				if (getNet(1)) str[n++] = 0x1c;
				if (bt_present) str[n++] = 4;
				if (user_io_get_sdram_cfg() & 0x8000)
				{
					switch (user_io_get_sdram_cfg() & 7)
					{
					case 7:
						str[n] = 0x95;
						break;
					case 3:
						str[n] = 0x94;
						break;
					case 1:
						str[n] = 0x93;
						break;
					default:
						str[n] = 0x92;
						break;
					}
				}

				str[22] = ' ';
			}

			OsdWrite(16, "", 1, 0);
			OsdWrite(17, str, 1, 0);
			OsdWrite(18, "", 1, 0);
		}
	}
}

void open_joystick_setup()
{
	OsdSetSize(16);
	menusub = 0;
	OsdClear();
	OsdEnable(DISABLE_KEYBOARD);
	start_map_setting(joy_bcount ? joy_bcount + 4 : 8);
	menustate = MENU_JOYDIGMAP;
	joymap_first = 1;
}

void ScrollLongName(void)
{
	// this function is called periodically when file selection window is displayed
	// it checks if predefined period of time has elapsed and scrolls the name if necessary

	int off = 0;
	int max_len;

	int len = strlen(flist_SelectedItem()->altname); // get name length

	max_len = 30; // number of file name characters to display (one more required for scrolling)
	if (flist_SelectedItem()->de.d_type == DT_DIR)
	{
		max_len = 23; // number of directory name characters to display
		if ((fs_Options & SCANO_CORES) && (flist_SelectedItem()->altname[0] == '_'))
		{
			off = 1;
			len--;
		}
	}

	if (flist_SelectedItem()->de.d_type != DT_DIR) // if a file
	{
		if (!cfg.rbf_hide_datecode && flist_SelectedItem()->datecode[0])
		{
			max_len = 20; // __.__.__ remove that from the end
		}
		else if (cfg.browse_expand && len < 55)
		{
			return;
		}
	}

	ScrollText(flist_iSelectedEntry() - flist_iFirstEntry(), flist_SelectedItem()->altname + off, 0, len, max_len, 1);
}

// print directory contents
void PrintDirectory(int expand)
{
	char s[40];
	ScrollReset();

	if (!cfg.browse_expand) expand = 0;

	if (expand)
	{
		int k = flist_iFirstEntry() + OsdGetSize() - 1;
		if (flist_nDirEntries() && k == flist_iSelectedEntry() && k < flist_nDirEntries()
			&& strlen(flist_DirItem(k)->altname) > 28 && !(!cfg.rbf_hide_datecode && flist_DirItem(k)->datecode[0])
			&& flist_DirItem(k)->de.d_type != DT_DIR && k < flist_nDirEntries() - 1)
		{
			//make room for last expanded line
			flist_iFirstEntryInc();
		}
	}

	int i = 0;
	int k = flist_iFirstEntry();
	while(i < OsdGetSize())
	{
		char leftchar = 0;
		memset(s, ' ', 32); // clear line buffer
		s[32] = 0;
		int len2 = 0;
		leftchar = 0;
		int len = 0;

		if (k < flist_nDirEntries())
		{
			len = strlen(flist_DirItem(k)->altname); // get name length
			if (len > 28)
			{
				len2 = len - 27;
				if (len2 > 27) len2 = 27;
				if (!expand) len2 = 0;

				len = 27; // trim display length if longer than 30 characters
				s[28] = 22;
			}

			if((flist_DirItem(k)->de.d_type == DT_DIR) && (fs_Options & SCANO_CORES) && (flist_DirItem(k)->altname[0] == '_'))
			{
				strncpy(s + 1, flist_DirItem(k)->altname+1, len-1);
			}
			else if (flist_DirItem(k)->flags & DT_EXT_ZIP)
			{
				strncpy(s + 1, flist_DirItem(k)->altname, len-4); // strip .zip extension, see below
			}
			else
			{
				strncpy(s + 1, flist_DirItem(k)->altname, len); // display only name
			}

			char *datecode = flist_DirItem(k)->datecode;
			if (flist_DirItem(k)->de.d_type == DT_DIR) // mark directory with suffix
			{
				if (!strcmp(flist_DirItem(k)->altname, ".."))
				{
					strcpy(&s[19], " <UP-DIR>");
				}
				else
				{
					if (flist_DirItem(k)->flags & DT_EXT_ZIP) // mark ZIP archive with different suffix
						strcpy(&s[22], " <ZIP>");
					else
						strcpy(&s[22], " <DIR>");
				}
				len2 = 0;
			}
			else if (!cfg.rbf_hide_datecode && datecode[0])
			{
				int n = 19;
				s[n++] = ' ';
				s[n++] = datecode[0];
				s[n++] = datecode[1];
				s[n++] = '.';
				s[n++] = datecode[2];
				s[n++] = datecode[3];
				s[n++] = '.';
				s[n++] = datecode[4];
				s[n++] = datecode[5];

				if (len >= 19)
				{
					s[19] = 22;
					s[28] = ' ';
				}
				len2 = 0;
			}

			if (!i && k) leftchar = 17;
			if (i && k < flist_nDirEntries() - 1) leftchar = 16;
		}
		else if(!flist_nDirEntries()) // selected directory is empty
		{
			if (!i) strcpy(s, "          No files!");
			if (home_dir && !filter[0])
			{
				if (i == 6) strcpy(s, "      Missing directory:");
				if (i == 8)
				{
					len = strlen(home_dir);
					if (len > 27) len = 27;
					strncpy(s + 1 + ((27 - len) / 2), home_dir, len);
				}
			}
		}

		int sel = (i == (flist_iSelectedEntry() - flist_iFirstEntry()));
		OsdWriteOffset(i, s, sel, 0, 0, leftchar);
		i++;

		if (sel && len2 && i < OsdGetSize())
		{
			len = strlen(flist_DirItem(k)->altname);
			strcpy(s+1, flist_DirItem(k)->altname + len - len2);
			OsdWriteOffset(i, s, sel, 0, 0, leftchar);
			i++;
		}

		k++;
	}
}

static void set_text(const char *message, unsigned char code)
{
	char s[40];
	int i = 0, l = 1;

	OsdWrite(0, "", 0, 0);

	do
	{
		s[i++] = *message;

		// line full or line break
		if ((i == 29) || (*message == '\n') || !*message)
		{
			s[--i] = 0;
			OsdWrite(l++, s, 0, 0);
			i = 0;  // start next line
		}
	} while (*message++);

	if (code && (l <= 7))
	{
		sprintf(s, " Code: #%d", code);
		OsdWrite(l++, s, 0, 0);
	}

	while (l <= 7) OsdWrite(l++, "", 0, 0);
}

void InfoMessage(const char *message, int timeout, const char *title)
{
	if (menustate <= MENU_INFO)
	{
		if (menustate != MENU_INFO)
		{
			OsdSetTitle(title, 0);
			OsdEnable(OSD_MSG); // do not disable keyboard
		}

		set_text(message, 0);

		menu_timer = GetTimer(timeout);
		menustate = MENU_INFO;
		HandleUI();
		OsdUpdate();
	}
}

void MenuHide()
{
	menustate = MENU_NONE1;
	HandleUI();
}

int menu_present()
{
	return (menustate != MENU_NONE1) && (menustate != MENU_NONE2);
}

void Info(const char *message, int timeout, int width, int height, int frame, int priority)
{
	// A priority message owns the slot until it expires. Without this the video
	// mode popup overwrites save state results -- reported from hardware, where
	// the "Save state: saved" toast was replaced by a resolution readout before
	// it could be read, and the restore's own result likewise.
	static uint32_t info_priority_until = 0;

	int held_back = 0;

	if (priority)
	{
		info_priority_until = GetTimer(timeout);
	}
	else if (info_priority_until && !CheckTimer(info_priority_until))
	{
		held_back = 1;
	}

	// Diagnostic for the save state toast, which has never once appeared on
	// hardware even though the log shows the core raising the request, the host
	// reading it back and resolving it to a non-empty string, and this function
	// being called with it. The gate below is the next candidate: MENU_INFO is
	// 2, so anything from MENU_SYSTEM1 upward -- every real menu page, the
	// AmigaCD settings page included -- drops the call without a word.
	//
	// Logging menustate here separates "the gate dropped it" from "it was drawn
	// and not visible", which no amount of reading either file can settle.
	//
	// It logs EVERY call, including one the priority window above holds back.
	// That return used to happen before this line, so a held-back message left
	// no trace at all, and on 2026-09-26 that cost an hour: a save state was
	// refused by ss_header_parse(), said so through this function while the
	// core's own "Save state: captured" still owned the slot, and the only
	// thing in the log was the twenty-second deadline that follows. A refusal
	// and a save the host never noticed read identically from here, and they
	// have entirely different causes.
	// A no-op off Minimig; see support/minimig/minimig_ssdiag.cpp.
	minimig_ss_diag_log("host Info(\"%s\") menustate=%u gate=%s",
	                    message, menustate,
	                    held_back                ? "HELD (priority)" :
	                    (menustate <= MENU_INFO) ? "open" : "DROPPED");

	if (held_back) return;

	if (menustate <= MENU_INFO)
	{
		OSD_PrintInfo(message, &width, &height, frame);
		InfoEnable(20, (cfg.direct_video && get_vga_fb()) ? 30 : 10, width, height);
		OsdSetSize(16);

		menu_timer = GetTimer(timeout);
		menustate = MENU_INFO;
		OsdUpdate();
	}
}

int menu_lightgun_cb(int idx, uint16_t type, uint16_t code, int value)
{
	if (type == EV_ABS)
	{
		if (code == 0 && value) gun_x = value;
		if (code == 1 && value != 1023) gun_y = value;
	}

	if (type == EV_KEY)
	{
		// 0x133 is BTN_NORTH, which is where this kernel's guncon2 driver puts a
		// GunCon 2's trigger -- confirmed on hardware, forty logged presses. It
		// was missing here, so the one button a user naturally reaches for did
		// nothing on the calibration screen: coordinates moved, no edge was ever
		// confirmed, and the flow never reached input_lightgun_save(). That
		// leaves the gun on built-in defaults that do not match the display,
		// which is its own hard-to-read bug.
		if ((code == 0x130 || code == 0x131 || code == 0x133 || code == 0x120 || code == 0x110) && menustate == MENU_LGCAL1)
		{
			gun_idx = idx;
			if (value == 1) gun_ok = 1;
			if (value == 0) gun_ok = 2;
			return 1;
		}
	}
	return 0;
}

int menu_allow_cfg_switch()
{
	if (user_io_osd_is_visible())
	{
		switch (menustate)
		{
		case MENU_ST_MAIN2:
		case MENU_ARCHIE_MAIN2:
		case MENU_MINIMIG_MAIN2:
		case MENU_COMMON2:
		case MENU_SYSTEM2:
			return 1;

		case MENU_FILE_SELECT2:
			if (is_menu() && (fs_Options & SCANO_CORES)) return 1;
			break;

		case MENU_GENERIC_MAIN3:
			if (!page) return 1;
			break;
		}
	}

	return 0;
}

void menu_process_save()
{
	menu_save_timer = GetTimer(500);
}

static char pchar[] = { 0x8C, 0x8E, 0x8F, 0x90, 0x91, 0x7F };

#define PROGRESS_CNT    28
#define PROGRESS_CHARS  (int)(sizeof(pchar)/sizeof(pchar[0]))
#define PROGRESS_MAX    ((PROGRESS_CHARS*PROGRESS_CNT)-1)

void ProgressMessage(const char* title, const char* text, int current, int max)
{
	static int progress;
	if (!current && !max)
	{
		progress = -1;
		MenuHide();
		return;
	}

	int new_progress = (((uint64_t)current)*PROGRESS_MAX) / max;
	if (progress != new_progress)
	{
		progress = new_progress;
		static char progress_buf[128];
		memset(progress_buf, 0, sizeof(progress_buf));

		if (new_progress > PROGRESS_MAX) new_progress = PROGRESS_MAX;
		char c = pchar[new_progress % PROGRESS_CHARS];
		new_progress /= PROGRESS_CHARS;

		char *buf = progress_buf;
		sprintf(buf, "\n\n %.27s\n ", text);
		buf += strlen(buf);

		for (int i = 0; i <= new_progress; i++) buf[i] = (i < new_progress) ? 0x7F : c;
		buf[PROGRESS_CNT] = 0;

		InfoMessage(progress_buf, 2000, title);
	}
}
