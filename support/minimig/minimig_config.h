
#ifndef __MINIMIG_CONFIG_H__
#define __MINIMIG_CONFIG_H__

#include "../../file_io.h"

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
void amigacd_default_rom_names(int cdtv, const char **main_name, const char **ext_name);
void amigacd_set_default_roms(int cdtv);
void amigacd_apply_system(int cdtv);
int  amigacd_is_cdtv(void);

#endif
