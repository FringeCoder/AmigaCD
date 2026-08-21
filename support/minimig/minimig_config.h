
#ifndef __MINIMIG_CONFIG_H__
#define __MINIMIG_CONFIG_H__

#include "../../file_io.h"
#include "minimig_profile.h"   // AMIGACD_SYS_* for the system helpers below

#define CONFIG_TURBO     1
#define CONFIG_NTSC      2
#define CONFIG_A1000     4
#define CONFIG_ECS       8
#define CONFIG_AGA       16
#define CONFIG_CDTV      32

#define CONFIG_FLOPPY1X  0
#define CONFIG_FLOPPY2X  1

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

// Who owns the SNAC user port. Mutually exclusive -- Minimig.sv muxes USER_OUT
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
	unsigned char reserved;
	char filename[1024];
} mm_hardfileTYPE;

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
	// alignment, so sizeof(mm_configTYPE) is UNCHANGED at 5228 and
	// minimig_cfg_load's exact-size dispatch still routes existing files to
	// the current branch. Those files carry whatever was in that padding --
	// zero in practice, since minimig_config is a zero-initialised global --
	// so every reader masks with & 3 rather than trusting it.
	unsigned char   ss_slot;
} mm_configTYPE;

extern mm_configTYPE minimig_config;

int minimig_cfg_load(int num);
int minimig_cfg_save(int num);
int minimig_mgl_save();
const char* minimig_get_cfg_info(int num, int label);

void minimig_reset();
void minimig_set_kickstart(char *name);
void minimig_set_extrom(char *name);
const char* minimig_get_extrom();

void minimig_set_adjust(char n);
char minimig_get_adjust();
void minimig_adjust_vsize(char force);

void minimig_ConfigVideo(unsigned char scanlines);
void minimig_ConfigAudio(unsigned char audio);
void minimig_ConfigMemory(unsigned char memory);
void minimig_ConfigCPU(unsigned char cpu);
void minimig_ConfigChipset(unsigned char chipset);
void minimig_ConfigFloppy(unsigned char drives, unsigned char speed);
void minimig_ConfigFloppyExt(unsigned char drive0, unsigned char drive1, unsigned char drive2, unsigned char drive3);
void minimig_ConfigSnac(void);
void minimig_ConfigAutofire(unsigned char autofire, unsigned char mask);

void minimig_set_extcfg(unsigned int ext_cfg);
unsigned int minimig_get_extcfg();

// AmigaCD console variant helpers (see minimig_config.cpp).
void amigacd_default_rom_names(int sys, const char **main_name, const char **ext_name);
// Both return 1 if the machine's default main ROM was found and selected, and
// 0 if it was missing -- in which case whatever ROM was already loaded is left
// alone. Callers should tell the user when it returns 0: the machine will boot,
// but as the wrong Amiga, and the only other clue is the Main ROM row.
int amigacd_set_default_roms(int sys);
int amigacd_apply_system(int sys);
int  amigacd_is_cdtv(void);
// The AMIGACD_SYS_* values these two speak in live in minimig_profile.h, which
// is included above so callers need only this header.
int  amigacd_system(void);
const char *amigacd_system_name(int sys);

#endif
