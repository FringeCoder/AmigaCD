
#ifndef __MINIMIG_CONFIG_H__
#define __MINIMIG_CONFIG_H__

#include "../../file_io.h"
#include "minimig_profile.h"   // AMIGACD_SYS_* for the system helpers below
#include "../../ide.h"

#define CONFIG_TURBO     1
#define CONFIG_NTSC      2
#define CONFIG_A1000     4
#define CONFIG_ECS       8
#define CONFIG_AGA       16
#define CONFIG_CDTV      32

#define CONFIG_FLOPPY1X  0
#define CONFIG_FLOPPY2X  1

#define CONFIG_PRESET_CD32  1
#define CONFIG_PRESET_CDTV  2
#define CONFIG_PRESET_A500  3
#define CONFIG_PRESET_A600  4
#define CONFIG_PRESET_A1200 5

extern const char *config_memory_chip_msg[];
extern const char *config_memory_slow_msg[];
extern const char *config_memory_fast_msg[][8];
extern const char *config_cpu_msg[];
extern const char *config_chipset_msg[];

typedef struct
{
	unsigned char lores;
	unsigned char hires;
} mm_filterTYPE;

typedef struct
{
	unsigned char speed;
	unsigned char drives;      // COUNT MINUS ONE: 0 = df0 only, 1 = df0+df1
	// Per-unit external (real) drive assignment for MiSTer Floppy.
	// 0 = use the mounted image, 1..4 = external drive 0/A .. 3.
	// Sized 4 for the Amiga maximum; the AmigaCD OSD exposes two today.
	unsigned char extDrives[4];
} mm_floppyTYPE;

// The pre-extDrives layout, kept so minimig_cfg_load can still read configs
// written before MiSTer Floppy support. Our old mm_configTYPE was field-for-field
// identical to upstream's, so those files migrate without special-casing.
typedef struct
{
	unsigned char speed;
	unsigned char drives;
} mm_floppyTYPEOld;

// Who owns the SNAC user port. Mutually exclusive -- AmigaCD.sv muxes USER_OUT
// on this and resets mt32pi when it changes.
enum mm_userportMode {
	mmup_mp32pi       = 0,
	mmup_misterfloppy = 1,
	mmup_snacpsx      = 2
};

// What a SNAC port drives on the Amiga side. Pad and mouse are independent
// signals in userio.v, so "both" is not a compromise -- it is the classic
// Amiga arrangement of mouse in port 1, joystick in port 2.
enum mm_snacPortMode {
	mmsp_off       = 0,
	mmsp_pad       = 1,
	mmsp_mouse     = 2,
	mmsp_pad_mouse = 3,
	mmsp_lightgun  = 4
};

typedef struct
{
	unsigned char cfg;
	// hardfile[0].reserved carries AMIGACD_HF0_CD32PLUS: this machine is a
	// CD32+ rather than a stock CD32. The two agree on every other bit, and
	// the thing that used to tell them apart -- whether IDE was enabled --
	// turned out to be unavailable, because the CD32's own CD needs it.
	//
	// This byte is the marker because it is the only genuinely spare persisted
	// storage in the config: declared, never read, never written, reachable
	// from no menu path. Using it changes no struct size, so minimig_cfg_load's
	// exact-size dispatch is untouched and every existing config keeps loading
	// down the same branch, with the bit reading 0 and meaning "stock CD32".
	//
	// The alternatives were worse. ext_cfg/ext_cfg2 are a live 32-bit word sent
	// to the core. hardfile[1].cfg is cleared when the user ejects a drive
	// (menu.cpp), so a CD32+ would silently demote itself. Appending a field
	// changes sizeof and needs a new migration branch in the riskiest function
	// in minimig_config.cpp.
	unsigned char reserved;
	char filename[1024];
} mm_hardfileTYPE;

// Bit 0 of hardfile[0].reserved. See above.
#define AMIGACD_HF0_CD32PLUS 0x01

typedef struct
{
	char            id[8];
	unsigned short  version;
	unsigned short  ext_cfg2;
	char            kickstart[992];
	char            label[32];
	unsigned short  ext_cfg;
	unsigned char   memory;
	unsigned char   chipset;
	mm_floppyTYPE   floppy;
	unsigned char   disable_ar3;
	unsigned char   ide_cfg;
	unsigned char   scanlines;
	unsigned char   audio;
	mm_hardfileTYPE hardfile[4];
	unsigned char   cpu;
	unsigned char   autofire;
	char            info[64];
	mm_userportMode userport;
	unsigned char   snac[2];   // mm_snacPortMode per SNAC port
	// Save state slot, 0..3 (shown as 1..4). Appended at the end so the
	// on-disk layout stays loadable. It lands at offset 5226, inside the two
	// bytes of tail padding snac[2] already left before the struct's 4-byte
	// alignment, so sizeof(mm_configTYPE) was UNCHANGED at 5228 and
	// minimig_cfg_load's exact-size dispatch still routed existing files to
	// the current branch. Those files carry whatever was in that padding --
	// zero in practice, since minimig_config is a zero-initialised global --
	// so every reader masks with & 3 rather than trusting it.
	unsigned char   ss_slot;
	// Upstream's dedicated CD drive slots, appended after ss_slot so a 5228
	// byte file stays a prefix of this layout. cd32_drive.cfg lands on the
	// last padding byte of the old layout, which is zero in practice and
	// means "disabled" anyway; minimig_cfg_load derives both slots from the
	// chipset bit and hardfile[0] on migration rather than trusting it.
	mm_hardfileTYPE cd32_drive;
	mm_hardfileTYPE cdtv_drive;
	uint8_t         a2065_mode;
	// Seconds to hold the first CD media announcement at cold boot. 0 disables
	// the hold entirely; 8 is the default and the value measured to work.
	//
	// Presenting a disc that is already there at power-on is not what the
	// hardware does, and a CD32 title that wants most of chip RAM in one
	// allocation fails because of it -- the ROM allocates its CD structures in
	// a different order and fragments the region. See
	// docs/chip-ram-shortfall.md; the OSD row is on the Advanced page.
	//
	// Appended, so every older layout migrates through the branches in
	// minimig_cfg_load and gets the working default rather than a zero.
	unsigned char   cd_mount_delay;

	// How fast sectors are handed to the CD32. See mm_cdSpeed below.
	//
	// Appended after cd_mount_delay, and the order is deliberate. Both fields
	// were added as the single byte at offset 7280 on different branches, so
	// a 7281-byte config on a card is ambiguous: it carries one or the other.
	// The machines this core has actually run on saved cd_mount_delay there,
	// and a delay read back into cd_speed would be a nonsense rate while a
	// rate read back into cd_mount_delay would silently bring the Flink
	// out-of-memory failure back. With cd_mount_delay first, those files load
	// exactly as written and cd_speed arrives as 0 -- mmcd_fast, the shipped
	// rate -- which is the one value that changes nothing.
	uint8_t         cd_speed;

	// CD32 audio buffer depth. See mm_cdAudioBuffer below.
	//
	// Lands at offset 7282, in the tail padding cd_speed left before the
	// struct's 4-byte alignment, so sizeof(mm_configTYPE) is unchanged and a
	// file written before this field existed loads through the current-layout
	// branch with a zero here -- mmab_normal, the behaviour it always had.
	// Read through amigacd_cd_audio_buffer(), never directly: it is a byte
	// from a file.
	uint8_t         cd_audio_buffer;

	// The floppy images in df0 and df1 when the config was saved, beside the
	// hard disks and the CD, and mounted again when it loads. The paths are
	// as InsertFloppy was given them -- root-relative from the file browser
	// -- so any image it takes comes back: ADF, DMS, SCP, IPF. Empty means
	// no disk. Filled from df[] at save time (minimig_floppy_snapshot), so
	// every insert and eject path is covered without each having to say so.
	//
	// Appended, so the size changed (7284 -> 9332) and minimig_cfg_load has a
	// branch for the old one. The first byte lands on the old layout's last
	// padding byte; that branch clears both paths rather than trust it.
	// Two, not four: this OSD has df0 and df1. A stock Minimig with df2/df3
	// does not get those back.
	char            floppy_path[2][1024];
} mm_configTYPE;

// The layout before floppy_path was appended. Every config written before
// 2026-10-13 is this size.
#define MM_CONFIG_SIZE_NO_FLOPPY_PATH 7284

// Seconds. Kept here because the menu, the config loader and akiko all need it.
//
// 14, raised from 8 on 2026-10-03. 8 was measured against a DIFFERENT failure
// -- Flink's 1774.8 KB contiguous allocation, where announcing at t=0
// fragments chip RAM (docs/chip-ram-shortfall.md). 6000 ms failed and 8000
// worked, so 8 was the bottom of that window with about two seconds of margin,
// and that file already said to raise it if trouble appeared.
//
// Trouble appeared. Chuck Rock does not boot reliably when the announce lands
// at 8 s, across two independent mount routes:
//
//   announce at  0 s   0/2      disc in the config, delay 0
//   announce at  8 s   1/8      disc in the config, and mount_cd at 3 s and 8 s
//   announce at 10 s   1/2
//   announce at 12 s+  11/11    12, 14, 16, 26 s, and the config route at 14
//
// The ROM issues INFO and two non-data PLAY frames (cmd[7]=0x03) either way;
// in a failing round the data read (cmd[7]&0x80) simply never follows, and the
// ROM stops asking. 1 of 8 against 11 of 11 is p < 0.001 by Fisher.
//
// The two failures pull the same way -- too early fragments chip RAM, too early
// also loses the boot read -- so this moves away from both edges rather than
// trading one for the other. Overridable without a rebuild: milliseconds into
// /tmp/akiko_boot_delay, then reload the core.
// 0 since 2026-10-08: a CD32 needs no hold at all.
//
// The hold was added because Flink failed when the disc was announced at t=0 --
// the ROM allocated its CD structures in an order that fragmented chip RAM and
// Flink could not get its 1774.8 KB in one piece. That was real, and it was a
// symptom: the frame whose checksum byte had not been written yet (PR #110) was
// making the ROM take a different path. With that fixed, Flink boots to
// GAMEPLAY at a 0 ms hold, which is the case it was failing in.
//
// Checked against a reference rather than argued: Copperline, configured as the
// same stock CD32 with the same ROMs and the same CHDs, boots both Flink and
// Banshee with the disc in the tray at power-on and no hold of any kind.
//
// BANSHEE IS NOT THIS SETTING, and an earlier version of this comment said it
// was. Banshee needs 1731 KB contiguous and a 2 MB machine has about 1728 KB
// free, so it only runs when the disc is in the tray AT POWER-ON: the ROM boots
// it straight from the boot block and never builds a DOS environment. Give the
// same machine the same disc LATE -- which is the only thing a MiSTer can do,
// because the OSD mounts after the core has started -- and the ROM mounts it as
// a data volume and runs bans.exe from an AmigaDOS shell instead, where the
// console and its structures cost more than the margin:
//
//     bans.exe: not enough memory available
//     bans.exe failed returncode 10
//
// Measured on this core at a 0 ms hold on 2026-10-08, and measured on
// Copperline the same afternoon with `--insert-cd-after 20`, chip 2M and no
// fast RAM: the same two lines, at the same prompt. A real CD32 fails this disc
// the same way we do, so there is nothing to fix at this setting -- the fix is
// a disc that is present from reset (or fast RAM, which is what the user did).
//
// What DOES boot at 0 is the late-mount route for titles that fit: Chuck Rock
// reaches its loader in 13 s (lba 14 then 12), and Flink reads its game data
// and starts CD audio. Neither needs a hold.
//
// The row stays, because a hold is still the only lever if some title ever needs
// one, and /tmp/akiko_boot_delay still overrides without a rebuild.
#define AMIGACD_CD_DELAY_DEFAULT 0
#define AMIGACD_CD_DELAY_MAX     15
// The old default, which existing configs on every card still carry. See the
// migration in minimig_cfg_load.
// The two values this core has handed out by default: 8 until 2026-10-03 and 14
// after it. Both are corrected on load; anything else is the user's own choice.
#define AMIGACD_CD_DELAY_LEGACY  8
#define AMIGACD_CD_DELAY_LEGACY2 14
// CD sector delivery rate.
//
// mmcd_fast is the floor the core has always shipped with (see
// AKIKO_SECTOR_PERIOD_US in akiko_pace.h): roughly 588 sectors/s, about 8x,
// and not a rate any real machine produces. It exists because sectors
// delivered faster than ~0.4 ms apart would not boot with Z2 fast RAM on.
//
// mmcd_2x is what a CD32 actually is -- a 2x unit, 150 sectors/s -- which is
// more faithful, gives about four times the margin over that knee, and costs
// about four times the load time. Which is right is a judgement about the
// machine you want, so it is a choice and not a constant.
// mmcd_drive is what the hardware does: the guest COMMANDS a rate, in bit 6 of
// cmd[8] of every PLAY/READ, and the drive runs at it. We parsed that byte and
// threw it away -- "recorded for future seek-time simulation" -- so the CD32 has
// been asking for 2x and getting about 8x on the shipped default, for every
// title, since this core existed. It is the default now; the other two remain as
// deliberate overrides.
enum mm_cdSpeed
{
	mmcd_fast  = 0,
	mmcd_2x    = 1,
	mmcd_drive = 2,
	mmcd_count
};

// New configs get the authentic behaviour. An existing config keeps whatever it
// stores: unlike the announce hold, neither of the other two values is broken,
// and which one you want is a preference about loading times.
#define AMIGACD_CD_SPEED_DEFAULT mmcd_drive

// How much CD audio the core buffers ahead of the speaker.
//
// A real CD32 buffers nothing: the drive clocks audio straight off the disc.
// Here a software feeder fills a FIFO in the FPGA (rtl/cdda.v), and any stall
// in the MiSTer main loop longer than what that FIFO holds is a gap in the
// music.
//
// mmab_normal is the depth the core has always had, ~46 ms, and is the closest
// to the machine: what the guest is told is playing is at most that far ahead
// of what it hears.
//
// mmab_large is ~186 ms. It survives four times the stall, at the cost of
// audio that keeps sounding for up to that long after a PAUSE, and of the
// per-frame subcode stream running that far ahead of the speaker. The SUBQ
// position and the end-of-play notification are corrected for the depth, so
// games that time themselves off those still see the audible position.
enum mm_cdAudioBuffer
{
	mmab_normal = 0,
	mmab_large  = 1,
	mmab_count
};
#define AMIGACD_CD_AUDIO_BUFFER_DEFAULT mmab_normal


extern mm_configTYPE minimig_config;

static inline int amigacd_cd_audio_buffer(void)
{
	return (minimig_config.cd_audio_buffer < mmab_count)
	     ? (int)minimig_config.cd_audio_buffer : (int)mmab_normal;
}

// sys_suffix gives a machine its own config file so the System row can leave
// one machine and arrive at another without losing either one's settings.
// Null keeps the historical names, which is what every existing caller wants
// and what CD32 keeps using -- see the comment on GetConfigurationName.
// The default argument also satisfies upstream's plain minimig_cfg_load(num).
int minimig_cfg_load(int num, const char *sys_suffix = 0);
int minimig_cfg_save(int num, const char *sys_suffix = 0);

// Upstream's quick-start presets (CONFIG_PRESET_*), which pick a machine and
// its ROMs in one go. Separate from the AmigaCD system helpers below.
void minimig_cfg_set(int preset);
bool minimig_cfg_available(int preset);

// Saves from_sys, then loads to_sys. Returns 0 when to_sys has never been
// configured, in which case the caller should apply its profile instead.
int amigacd_switch_system(int from_sys, int to_sys);

// Writes both the live config and the current machine's own file. What the
// "Save settings" row calls.
int amigacd_cfg_save_current(void);
int minimig_mgl_save();

// A floppy's path in the form a .mgl <file> item resolves back to the same
// image; see mgl_item_path in mgl_path.h.
void minimig_mgl_floppy_path(const char *name, char *out, size_t outsz);

// The <amigacd_config> block for a .mgl: every setting of minimig_config as it
// stands now. Returns its length, or -1 if it does not fit. See mgl_cfg.h.
int minimig_mgl_config_block(char *out, size_t outsz, const char *indent);
const char* minimig_get_cfg_info(int num, int label);

void minimig_reset();
void minimig_set_kickstart(const char *name);
void minimig_set_extrom(const char *name);
const char* minimig_get_extrom();

void minimig_set_adjust(char n);
char minimig_get_adjust();
int  minimig_vadjust_info(char *out, int outsz);
void minimig_adjust_vsize(char force);

void minimig_ConfigVideo(unsigned char scanlines);
void minimig_ConfigAudio(unsigned char audio);
void minimig_ConfigMemory(unsigned char memory);
void minimig_ConfigCPU(unsigned char cpu);
void minimig_ConfigChipset(mm_configTYPE *config);
void minimig_ConfigFloppy(unsigned char drives, unsigned char speed);
void minimig_ConfigFloppyExt(unsigned char drive0, unsigned char drive1, unsigned char drive2, unsigned char drive3);
void minimig_ConfigSnac(void);
void minimig_ConfigAutofire(unsigned char autofire, unsigned char mask);

void minimig_set_extcfg(unsigned int ext_cfg);
unsigned int minimig_get_extcfg();

// The bits of status[63:32] that the extended config owns: status[50:32],
// the highest being the 0x40000 toggle in the Amiga video/audio menu.
// status[63:51] belongs to cur_status -- the AmigaCD core's save-state and
// test-pattern requests, and the A2065 mode. See user_io_status_set().
#define MINIMIG_EXTCFG_STATUS_MASK 0x0007FFFFu

// AmigaCD console variant helpers (see minimig_config.cpp).
void amigacd_default_rom_names(int sys, const char **main_name, const char **ext_name);
// Both return 1 if the machine's default main ROM was found and selected, and
// 0 if it was missing -- in which case whatever ROM was already loaded is left
// alone. Callers should tell the user when it returns 0: the machine will boot,
// but as the wrong Amiga, and the only other clue is the Main ROM row.
int amigacd_set_default_roms(int sys);
int amigacd_apply_system(int sys);

// True when the CD must be reached over IDE -- a physical disc only. An image is
// served by Akiko, which is what allows a stock CD32 to run with no IDE at all.
int amigacd_cd_needs_ide(void);
int  amigacd_is_cdtv(void);
// The AMIGACD_SYS_* values these two speak in live in minimig_profile.h, which
// is included above so callers need only this header.
int  amigacd_system(void);
// slot: 0 = cd32_drive, 1 = cdtv_drive
int minimig_cd_drive_open(int slot, const char *filename);
drive_t* minimig_cd_drive_get(int slot);

#endif
