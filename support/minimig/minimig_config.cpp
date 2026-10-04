// config.c

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#include "../../hardware.h"
#include "../../file_io.h"
#include "../../menu.h"
#include "../../user_io.h"
#include "../../input.h"
#include "../../cfg.h"
#include "../../ide.h"
#include "minimig_boot.h"
#include "minimig_fdd.h"
#include "minimig_profile.h"
#include "minimig_config.h"
#include "rom_crypt.h"
#include "vadjust_info.h"
#include "minimig_share.h"
#include "../physical_disc/physical_disc.h"   // PHYSICAL_DISC_SENTINEL
#include "akiko_cd32.h"
#include "cdtv_cd.h"
#include "minimig_a2065.h"
#include "../arcade/mra_loader.h"
#include <string>
#include <unistd.h>

const char *config_memory_chip_msg[] = { "512K", "1M",   "1.5M", "2M" };
const char *config_memory_slow_msg[] = { "none", "512K", "1M",   "1.5M" };
const char *config_memory_fast_msg[][8] = { { "none", "2M", "4M", "8M", "8M",    "8M",    "8M",   "8M" } ,
											{ "none", "2M", "4M", "8M", "256M", "384M", "256M", "256M" } };
const char *config_cpu_msg[] = { "68000", "68010", "-----","68020" };
const char *config_chipset_msg[] = { "OCS-A500", "OCS-A1000", "ECS", "---", "---", "---", "AGA", "---" };

typedef struct
{
	char            id[8];
	unsigned long   version;
	char            kickstart[1024];
	mm_filterTYPE   filter;
	unsigned char   memory;
	unsigned char   chipset;
	// Deliberately mm_floppyTYPEOld: this describes a legacy ON-DISK layout, so
	// it must not follow mm_floppyTYPE when that grows fields (extDrives[4] was
	// added for MiSTer Floppy). Using the live type here would silently
	// mis-parse every old config.
	mm_floppyTYPEOld floppy;
	unsigned char   disable_ar3;
	unsigned char   enable_ide;
	unsigned char   scanlines;
	unsigned char   audio;
	mm_hardfileTYPE hardfile[2];
	unsigned char   cpu;
	unsigned char   autofire;
} configTYPE_old;

// The config layout as it was before MiSTer Floppy, i.e. what every AmigaCD.cfg
// on disk today looks like. extDrives[4] went INSIDE mm_floppyTYPE and userport
// onto the end of mm_configTYPE, so a pre-change file loaded straight into the
// new struct would misalign every field after `floppy` -- the same trap the
// physical-disc enum insertion set. minimig_cfg_load migrates via this instead.
typedef struct
{
	char             id[8];
	unsigned short   version;
	unsigned short   ext_cfg2;
	char             kickstart[992];
	char             label[32];
	unsigned short   ext_cfg;
	unsigned char    memory;
	unsigned char    chipset;
	mm_floppyTYPEOld floppy;
	unsigned char    disable_ar3;
	unsigned char    ide_cfg;
	unsigned char    scanlines;
	unsigned char    audio;
	mm_hardfileTYPE  hardfile[4];
	unsigned char    cpu;
	unsigned char    autofire;
	char             info[64];
} mm_configTYPE_NoExtFloppy;

// The config layout as it was before PSX SNAC, i.e. every AmigaCD.cfg written
// between the MiSTer Floppy change and this one. snac[2] went onto the end, so
// a file of this size loaded into the new struct would leave snac[] as garbage
// rather than misaligning -- but the size check is what tells the two apart,
// and without this struct there is nothing to compare against.
typedef struct
{
	char             id[8];
	unsigned short   version;
	unsigned short   ext_cfg2;
	char             kickstart[992];
	char             label[32];
	unsigned short   ext_cfg;
	unsigned char    memory;
	unsigned char    chipset;
	mm_floppyTYPE    floppy;
	unsigned char    disable_ar3;
	unsigned char    ide_cfg;
	unsigned char    scanlines;
	unsigned char    audio;
	mm_hardfileTYPE  hardfile[4];
	unsigned char    cpu;
	unsigned char    autofire;
	char             info[64];
	mm_userportMode  userport;
} mm_configTYPE_NoSnac;

// The layout as it was before the CD32/CDTV drive slots were appended -- the
// 5228-byte file every config on the card is today. Everything through
// ss_slot, and nothing after it.
typedef struct
{
	char             id[8];
	unsigned short   version;
	unsigned short   ext_cfg2;
	char             kickstart[992];
	char             label[32];
	unsigned short   ext_cfg;
	unsigned char    memory;
	unsigned char    chipset;
	mm_floppyTYPE    floppy;
	unsigned char    disable_ar3;
	unsigned char    ide_cfg;
	unsigned char    scanlines;
	unsigned char    audio;
	mm_hardfileTYPE  hardfile[4];
	unsigned char    cpu;
	unsigned char    autofire;
	char             info[64];
	mm_userportMode  userport;
	unsigned char    snac[2];
	unsigned char    ss_slot;
} mm_configTYPE_NoCdSlots;

// The layout as it was before the CD mount delay was appended: everything
// through a2065_mode. Every config written before 2026-09-03 is this shape.
typedef struct
{
	char             id[8];
	unsigned short   version;
	unsigned short   ext_cfg2;
	char             kickstart[992];
	char             label[32];
	unsigned short   ext_cfg;
	unsigned char    memory;
	unsigned char    chipset;
	mm_floppyTYPE    floppy;
	unsigned char    disable_ar3;
	unsigned char    ide_cfg;
	unsigned char    scanlines;
	unsigned char    audio;
	mm_hardfileTYPE  hardfile[4];
	unsigned char    cpu;
	unsigned char    autofire;
	char             info[64];
	mm_userportMode  userport;
	unsigned char    snac[2];
	unsigned char    ss_slot;
	mm_hardfileTYPE  cd32_drive;
	mm_hardfileTYPE  cdtv_drive;
	uint8_t          a2065_mode;
} mm_configTYPE_NoCdDelay;

// Every layout above predates the dedicated CD drive slots, so none of them
// says anything about them -- and minimig_ConfigChipset() now derives
// CONFIG_CDTV from cdtv_drive.cfg and CLEARS the bit when that slot is empty.
// Migrating a CDTV config without this would load one that still has the
// chipset bit set and then send the core a non-CDTV chipset word: the machine
// comes up as a plain Amiga and nothing about the saved config looks wrong.
// The memset also stops a stale filename surviving from a previous load.
static void derive_cd_slots_from_legacy(void)
{
	memset(&minimig_config.cd32_drive, 0, sizeof(minimig_config.cd32_drive));
	memset(&minimig_config.cdtv_drive, 0, sizeof(minimig_config.cdtv_drive));
	const bool cdtv = (minimig_config.chipset & CONFIG_CDTV) != 0;
	minimig_config.cdtv_drive.cfg = cdtv ? 1 : 0;
	minimig_config.cd32_drive.cfg = (!cdtv && minimig_config.hardfile[0].cfg == 2) ? 1 : 0;
}

mm_configTYPE minimig_config = { };
static unsigned char romkey[3072];

// Why the configured Kickstart would not load, for the boot screen. Only the
// FIRST attempt writes it: ApplyConfiguration retries with KICK.ROM and then a
// bare KICK.ROM, and "no such file" for a fallback the user never asked for is
// not the answer they need. Cleared before the first attempt.
static char rom_fail_reason[160] = {};

static void rom_fail(const char *why)
{
	BootPrint(why);
	if (!rom_fail_reason[0]) snprintf(rom_fail_reason, sizeof(rom_fail_reason), "%s", why);
}

// Load ROM.KEY if there is one. Returns its length, 0 when there is no key or
// it cannot be used.
//
// Extracted from UploadKickstart, where it was a local, because the ext-ROM
// composite path runs FIRST and had no way to reach a key at all -- see
// LoadRomSlot.
//
// Returns 0 rather than the file's size when the key does not fit romkey[].
// It used to set keysize from the size regardless and leave romkey untouched,
// so an oversized key said "too large" and then decrypted with three kilobytes
// of zeroes, which is a working ROM turned into noise by a message the user
// was told to ignore.
static int LoadRomKey(void)
{
	fileTYPE file = {};
	int keysize = 0;

	BootPrint("Checking for Amiga Forever key file:");
	if (FileOpen(&file, user_io_make_filepath(HomeDir(), "ROM.KEY")) || FileOpen(&file, "ROM.KEY"))
	{
		if (file.size > 0 && (uint64_t)file.size < sizeof(romkey))
		{
			keysize = (int)file.size;
			FileReadAdv(&file, romkey, keysize);
			BootPrint("Loaded Amiga Forever key file");
		}
		else
		{
			BootPrint("ROM.KEY is empty or too large - ignoring it");
		}
		FileClose(&file);
	}
	return keysize;
}

static void SendFileV2(fileTYPE* file, unsigned char* key, int keysize, int address, int size)
{
	static uint8_t buf[512];
	unsigned int keyidx = 0;
	printf("File size: %dkB\n", size >> 1);
	printf("[");
	if (keysize)
	{
		// read header
		FileReadAdv(file, buf, 0xb);
	}

	for (int i = 0; i<size; i++)
	{
		if (!(i & 31)) printf("*");
		FileReadAdv(file, buf, 512);

		if (keysize)
		{
			// decrypt ROM
			for (int j = 0; j<512; j++)
			{
				buf[j] ^= key[keyidx++];
				if ((int)keyidx >= keysize) keyidx -= keysize;
			}
		}
		EnableIO();
		unsigned int adr = address + i * 512;
		spi8(UIO_MM2_WR);
		spi8(adr & 0xff); adr = adr >> 8;
		spi8(adr & 0xff); adr = adr >> 8;
		spi8(adr & 0xff); adr = adr >> 8;
		spi8(adr & 0xff); adr = adr >> 8;
		for (int j = 0; j<512; j = j + 4)
		{
			spi8(buf[j + 0]);
			spi8(buf[j + 1]);
			spi8(buf[j + 2]);
			spi8(buf[j + 3]);
		}
		DisableIO();
	}

	printf("]\n");
}


// Ext-ROM path is stored piggybacked in minimig_config.kickstart[]
// past the kickstart string's null terminator. Kept in the same field
// to preserve the on-disk CFG binary layout (kickstart is at a fixed
// offset; adding a sibling field would shift every later field and
// break every existing per-game .cfg file).
const char* minimig_get_extrom()
{
	const size_t cap = sizeof(minimig_config.kickstart);
	size_t kicklen = strnlen(minimig_config.kickstart, cap);
	if (kicklen + 1 >= cap) return "";
	return &minimig_config.kickstart[kicklen + 1];
}

static void SendBufferV2(const uint8_t *buf, int address, int size_bytes)
{
	int sectors = size_bytes / 512;
	printf("Upload %dkB -> 0x%08x [", size_bytes >> 10, address);
	for (int i = 0; i < sectors; i++)
	{
		if (!(i & 31)) printf("*");
		EnableIO();
		unsigned int adr = address + i * 512;
		spi8(UIO_MM2_WR);
		spi8(adr & 0xff); adr >>= 8;
		spi8(adr & 0xff); adr >>= 8;
		spi8(adr & 0xff); adr >>= 8;
		spi8(adr & 0xff); adr >>= 8;
		const uint8_t *p = buf + i * 512;
		for (int j = 0; j < 512; j += 4)
		{
			spi8(p[j + 0]);
			spi8(p[j + 1]);
			spi8(p[j + 2]);
			spi8(p[j + 3]);
		}
		DisableIO();
	}
	printf("]\n");
}

// Load a ROM image into a 512K slot. Supports 256K (mirrored to 512K)
// and 512K (direct). Returns true on success.
static bool LoadRomSlot(const char *path, uint8_t *dst512k, int keysize)
{
	fileTYPE file = {};
	if (!FileOpen(&file, path)) {
		printf("Ext-ROM open failed: %s\n", path);
		return false;
	}
	int sz = (int)file.size;

	// Amiga Forever ROMs, which this path used to refuse outright.
	//
	// That refusal was invisible AND consequential. Invisible because it went
	// to printf, which is the log, not BootPrintEx, which is the boot screen.
	// Consequential because of what happens next: ApplyConfiguration falls
	// back to UploadKickstart, which has always handled the main ROM's
	// encrypted form and knows nothing about the extended one. So a CD32 with
	// encrypted ROMs came up with its main Kickstart and NO extended ROM,
	// which does not boot a disc, and nothing on screen said why.
	const int plain = rom_enc_plain_size(sz);
	if (plain && !keysize)
	{
		rom_fail("Encrypted ROM but no ROM.KEY found");
		FileClose(&file);
		return false;
	}

	const int want = plain ? plain : sz;
	if (want != 0x80000 && want != 0x40000)
	{
		printf("Unsupported ROM size %d for slot upload\n", sz);
		FileClose(&file);
		return false;
	}

	if (plain)
	{
		uint8_t hdr[ROM_ENC_HEADER_BYTES];
		FileReadAdv(&file, hdr, ROM_ENC_HEADER_BYTES);
	}
	FileReadAdv(&file, dst512k, want);
	if (plain) rom_decrypt(dst512k, want, romkey, keysize);

	// The 256 KB mirror happens after decryption, or it would duplicate
	// ciphertext into the upper half and decrypt only the lower one.
	if (want == 0x40000) memcpy(dst512k + 0x40000, dst512k, 0x40000);

	FileClose(&file);
	return true;
}

// Composite upload: ext-ROM in lower 512K, main Kickstart in upper 512K.
// Mirrors the 1MB-image path used by the CD32 BIOS, but assembles the
// two halves from separate files instead of a pre-baked concat.
static char UploadKickstartWithExtRom(const char *kick_path, const char *extrom_path)
{
	BootPrint("Loading Kickstart + Ext.ROM:");
	BootPrint(kick_path);
	BootPrint(extrom_path);

	static uint8_t img[0x100000];
	memset(img, 0, sizeof(img));

	// The key has to be loaded here as well: this path runs before
	// UploadKickstart, which is where it used to be found.
	const int keysize = LoadRomKey();

	if (!LoadRomSlot(extrom_path, img, keysize)) return 0;
	if (!LoadRomSlot(kick_path,   img + 0x80000, keysize)) return 0;

	// Discard residents (matches UploadKickstart pre-amble).
	EnableIO();
	spi8(UIO_MM2_WR);
	for (int i = 0; i < 8; i++) spi8(0);
	for (int i = 0; i < 4; i++) spi8(1);
	DisableIO();

	SendBufferV2(img,           0xe00000, 0x80000);
	SendBufferV2(img + 0x80000, 0xf80000, 0x80000);
	return 1;
}

static char UploadKickstart(char *name)
{
	fileTYPE file = {};
	int keysize = LoadRomKey();

	BootPrint("Loading file: ");
	BootPrint(name);

	if (FileOpen(&file, name))
	{
		// discard from possible residents
		EnableIO();
		spi8(UIO_MM2_WR);
		for (int i = 0; i < 8; i++) spi8(0);
		for (int i = 0; i < 4; i++) spi8(1);
		DisableIO();

		if (file.size == 0x100000) {
			// 1MB Kickstart ROM
			BootPrint("Uploading 1MB Kickstart ...");
			SendFileV2(&file, NULL, 0, 0xe00000, file.size >> 10);
			SendFileV2(&file, NULL, 0, 0xf80000, file.size >> 10);
			FileClose(&file);
			return(1);
		}
		else if ((file.size == 8203) && keysize) {
			// Cloanto encrypted A1000 boot ROM
			BootPrint("Uploading encrypted A1000 boot ROM");
			SendFileV2(&file, romkey, keysize, 0xf80000, file.size >> 9);
			FileClose(&file);
			//clear tag (write 0 to $fc0000) to force bootrom to load Kickstart from disk
			//and not use one which was already there.
			spi_uio_cmd32_cont(UIO_MM2_WR, 0xfc0000);
			spi8(0x00);spi8(0x00);
			DisableIO();
			return(1);
		  }
		else if (file.size == 0x2000) {
			// 8KB A1000 boot ROM
			BootPrint("Uploading A1000 boot ROM");
			SendFileV2(&file, NULL, 0, 0xf80000, file.size >> 9);
			FileClose(&file);
			spi_uio_cmd32_cont(UIO_MM2_WR, 0xfc0000);
			spi8(0x00);spi8(0x00);
			DisableIO();
			return(1);
		  }
		else if (file.size == 0x80000) {
			// 512KB Kickstart ROM
			BootPrint("Uploading 512KB Kickstart ...");
			SendFileV2(&file, NULL, 0, 0xf80000, file.size >> 9);
			FileClose(&file);
			FileOpen(&file, name);
			SendFileV2(&file, NULL, 0, 0xe00000, file.size >> 9);
			FileClose(&file);
			return(1);
		}
		else if ((file.size == 0x8000b) && keysize) {
			// 512KB Kickstart ROM
			BootPrint("Uploading 512 KB Kickstart (Probably Amiga Forever encrypted...)");
			SendFileV2(&file, romkey, keysize, 0xf80000, file.size >> 9);
			FileClose(&file);
			FileOpen(&file, name);
			SendFileV2(&file, romkey, keysize, 0xe00000, file.size >> 9);
			FileClose(&file);
			return(1);
		}
		else if (file.size == 0x40000) {
			// 256KB Kickstart ROM
			BootPrint("Uploading 256 KB Kickstart...");
			SendFileV2(&file, NULL, 0, 0xf80000, file.size >> 9);
			FileClose(&file);
			FileOpen(&file, name); // TODO will this work
			SendFileV2(&file, NULL, 0, 0xfc0000, file.size >> 9);
			FileClose(&file);
			return(1);
		}
		else if ((file.size == 0x10000b) && keysize) {
			// 1 MB Amiga Forever. Until now this size fell straight through to
			// "Unsupported ROM file size!", which docs/vamigats.md records as
			// a gap worth knowing about before it wastes an evening.
			//
			// It cannot be done the way the other encrypted sizes are, with two
			// SendFileV2 calls: that function reads the 11-byte header at the
			// start of EVERY call and restarts the key at index 0, and this
			// image is one continuous stream whose second half carries neither.
			// So it is buffered, decrypted once end to end, and pushed in two
			// halves -- the same split the plain 1 MB path makes, ext ROM low
			// and Kickstart high.
			BootPrint("Uploading 1MB Kickstart (Amiga Forever encrypted)...");
			static uint8_t img[0x100000];
			uint8_t hdr[ROM_ENC_HEADER_BYTES];
			FileReadAdv(&file, hdr, ROM_ENC_HEADER_BYTES);
			FileReadAdv(&file, img, sizeof(img));
			rom_decrypt(img, sizeof(img), romkey, keysize);
			SendBufferV2(img,           0xe00000, 0x80000);
			SendBufferV2(img + 0x80000, 0xf80000, 0x80000);
			FileClose(&file);
			return(1);
		}
		else if ((file.size == 0x4000b) && keysize) {
			// 256KB Kickstart ROM
			BootPrint("Uploading 256 KB Kickstart (Probably Amiga Forever encrypted...");
			SendFileV2(&file, romkey, keysize, 0xf80000, file.size >> 9);
			FileClose(&file);
			FileOpen(&file, name); // TODO will this work
			SendFileV2(&file, romkey, keysize, 0xfc0000, file.size >> 9);
			FileClose(&file);
			return(1);
		}
		else {
			// Three different failures used to arrive as one line about size,
			// and the likeliest of them is not a size problem at all: an
			// Amiga Forever ROM with no ROM.KEY beside it has exactly the
			// right size and fails the `&& keysize` guard on every branch
			// above. Telling that user their ROM is the wrong size sends them
			// looking for a different ROM.
			const int plain = rom_enc_plain_size((int)file.size);
			if (plain && !keysize)
			{
				rom_fail("Encrypted ROM but no ROM.KEY found");
			}
			else
			{
				char msg[96];
				snprintf(msg, sizeof(msg), "Unsupported ROM size: %lld bytes",
				         (long long)file.size);
				rom_fail(msg);
			}
		}
		FileClose(&file);
	}
	else {
		char msg[96];
		snprintf(msg, sizeof(msg), "ROM file not found: %s", name);
		rom_fail(msg);
	}
	return(0);
}

static char UploadActionReplay()
{
	fileTYPE file = {};
	if(FileOpen(&file, user_io_make_filepath(HomeDir(), "HRTMON.ROM")) || FileOpen(&file, "HRTMON.ROM"))
	{
		int adr, data;
		puts("Uploading HRTmon ROM... ");
		SendFileV2(&file, NULL, 0, 0xa10000, (file.size + 511) >> 9);
		// HRTmon config
		adr = 0xa10000 + 20;
		spi_uio_cmd32_cont(UIO_MM2_WR, adr);
		data = 0x00800000; // mon_size, 4 bytes
		spi8((data >> 24) & 0xff); spi8((data >> 16) & 0xff);
		spi8((data >> 8) & 0xff); spi8((data >> 0) & 0xff);
		data = 0x00; // col0h, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0x5a; // col0l, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0x0f; // col1h, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // col1l, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // right, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0x00; // keyboard, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // key, 1 byte
		spi8((data >> 0) & 0xff);
		data = (minimig_config.ide_cfg & 1) ? 0xff : 0; // ide, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // a1200, 1 byte
		spi8((data >> 0) & 0xff);
		data = minimig_config.chipset&CONFIG_AGA ? 0xff : 0; // aga, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // insert, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0x0f; // delay, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // lview, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0x00; // cd32, 1 byte
		spi8((data >> 0) & 0xff);
		data = minimig_config.chipset&CONFIG_NTSC ? 1 : 0; // screenmode, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0xff; // novbr, 1 byte
		spi8((data >> 0) & 0xff);
		data = 0; // entered, 1 byte
		spi8((data >> 0) & 0xff);
		data = 1; // hexmode, 1 byte
		spi8((data >> 0) & 0xff);
		DisableIO();
		adr = 0xa10000 + 68;
		spi_uio_cmd32_cont(UIO_MM2_WR, adr);
		data = ((minimig_config.memory & 0x3) + 1) * 512 * 1024; // maxchip, 4 bytes TODO is this correct?
		spi8((data >> 24) & 0xff); spi8((data >> 16) & 0xff);
		spi8((data >> 8) & 0xff); spi8((data >> 0) & 0xff);
		DisableIO();

		FileClose(&file);
		return(1);
	}
	else {
		puts("\nhrtmon.rom not found!\n");
		return(0);
	}
	return(0);
}

// `sys_suffix` gives a machine its own config file, so switching the System row
// does not overwrite the settings of the machine being left. Null keeps the
// historical names: <core>.cfg for slot 0 and <core>N.cfg for the numbered
// presets, which are the user's own and are shared across machines on purpose.
//
// CD32 deliberately passes null too. It was the only machine this core had for
// most of its life, so its settings are already in <core>.cfg -- giving it a
// suffixed file of its own would strand every existing config.
static char* GetConfigurationName(int num, int chk, const char *sys_suffix)
{
	static char name[128];
	if (sys_suffix && *sys_suffix)
		sprintf(name, CONFIG_DIR "/%s_%s.cfg", user_io_get_core_name(), sys_suffix);
	else if (num) sprintf(name, CONFIG_DIR "/%s%d.cfg", user_io_get_core_name(), num);
	else sprintf(name, CONFIG_DIR "/%s.cfg", user_io_get_core_name());

	if (chk && !S_ISREG(getFileType(name))) return 0;
	return name+strlen(CONFIG_DIR)+1;
}

// The suffix for a machine, or null for the one that keeps the plain name.
int minimig_cfg_save(int num, const char *sys_suffix)
{
	minimig_config.a2065_mode = a2065_cfg_get();
	return FileSaveConfig(GetConfigurationName(num, 0, sys_suffix), &minimig_config, sizeof(minimig_config));
}

// ---- MGL surgical save ----
// Updates the launching MGL's <file type="f"> entries to reflect df[] state.
// - Mounted slot, no MGL line  -> append new <file delay=... type="f" index="N" path="..."/>
// - Mounted slot, MGL line with different path -> rewrite the path attribute in place
// - Ejected slot, MGL line     -> rewrite path="" (preserves any delay etc.)
// - All other lines (rbf, setname, reset, comments, savestate items, etc.) untouched.

static bool mgl_find_attr(const std::string &xml, size_t tag_start, size_t tag_end,
                          const char *name, std::string &out_val,
                          size_t *out_vstart = nullptr, size_t *out_vend = nullptr)
{
	size_t nlen = strlen(name);
	for (size_t p = tag_start; p + nlen + 2 < tag_end; p++)
	{
		bool ws_before = (p == tag_start) || isspace((unsigned char)xml[p - 1]);
		if (!ws_before) continue;
		if (strncasecmp(xml.data() + p, name, nlen) != 0) continue;
		if (xml[p + nlen] != '=' || xml[p + nlen + 1] != '"') continue;

		size_t vs = p + nlen + 2;
		size_t ve = xml.find('"', vs);
		if (ve == std::string::npos || ve > tag_end) return false;
		out_val.assign(xml, vs, ve - vs);
		if (out_vstart) *out_vstart = vs;
		if (out_vend)   *out_vend   = ve;
		return true;
	}
	return false;
}

static void mgl_replace_path(std::string &xml, size_t tag_start, size_t &tag_end,
                             const std::string &newval)
{
	std::string oldval;
	size_t vs = 0, ve = 0;
	if (mgl_find_attr(xml, tag_start, tag_end, "path", oldval, &vs, &ve))
	{
		xml.replace(vs, ve - vs, newval);
		tag_end += (ssize_t)newval.size() - (ssize_t)(ve - vs);
		return;
	}
	size_t ip = tag_end;
	if (ip > 0 && xml[ip - 1] == '/') ip--;
	while (ip > 0 && isspace((unsigned char)xml[ip - 1])) ip--;
	std::string ins = " path=\"";
	ins += newval;
	ins += "\"";
	xml.insert(ip, ins);
	tag_end += ins.size();
}

int minimig_mgl_save()
{
	mgl_struct *m = mgl_get();
	if (!m || !m->xml_path[0]) return 0;

	const char *path = m->xml_path;
	FILE *f = fopen(path, "rb");
	if (!f) { printf("MGL save: cannot read %s\n", path); return 0; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > (1 << 20)) { fclose(f); return 0; }
	std::string xml((size_t)sz, '\0');
	if (fread(&xml[0], 1, sz, f) != (size_t)sz) { fclose(f); return 0; }
	fclose(f);

	bool dirty = false;

	for (int slot = 0; slot < 4; slot++)
	{
		bool mounted = (df[slot].status & DSK_INSERTED) != 0;
		const char *cur_c = df[slot].name;
		std::string target = mounted ? (cur_c ? cur_c : "") : "";

		size_t found_start = std::string::npos;
		size_t found_end   = std::string::npos;
		std::string existing_path;
		size_t scan = 0;
		while (true)
		{
			size_t tag = xml.find("<file", scan);
			if (tag == std::string::npos) break;
			size_t te = xml.find('>', tag);
			if (te == std::string::npos) break;

			std::string vtype, vindex, vpath;
			bool ht = mgl_find_attr(xml, tag, te, "type",  vtype);
			bool hi = mgl_find_attr(xml, tag, te, "index", vindex);
			mgl_find_attr(xml, tag, te, "path", vpath);

			if (ht && hi && (vtype == "f" || vtype == "F") && atoi(vindex.c_str()) == slot)
			{
				found_start = tag;
				found_end   = te;
				existing_path = vpath;
				break;
			}
			scan = te + 1;
		}

		if (found_start != std::string::npos)
		{
			if (existing_path != target)
			{
				mgl_replace_path(xml, found_start, found_end, target);
				dirty = true;
			}
		}
		else if (mounted)
		{
			size_t close = xml.find("</mistergamedescription>");
			if (close == std::string::npos) continue;
			size_t ln = xml.rfind('\n', close);
			std::string indent = "    ";
			if (ln != std::string::npos)
			{
				size_t a = ln + 1, b = a;
				while (b < close && (xml[b] == ' ' || xml[b] == '\t')) b++;
				if (b > a) indent.assign(xml, a, b - a);
			}
			char buf[1200];
			int delay = (slot == 0) ? 2 : 0;
			snprintf(buf, sizeof(buf),
				"%s<file delay=\"%d\" type=\"f\" index=\"%d\" path=\"%s\"/>\n",
				indent.c_str(), delay, slot, cur_c ? cur_c : "");
			xml.insert(close, buf);
			dirty = true;
		}
	}

	if (!dirty)
	{
		printf("MGL save: no floppy deltas (path=%s)\n", path);
		return 0;
	}

	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *out = fopen(tmp, "wb");
	if (!out) { printf("MGL save: cannot create %s\n", tmp); return 0; }
	size_t wrote = fwrite(xml.data(), 1, xml.size(), out);
	fclose(out);
	if (wrote != xml.size() || rename(tmp, path) != 0)
	{
		printf("MGL save: write/rename failed for %s\n", path);
		unlink(tmp);
		return 0;
	}
	printf("MGL save: updated %s (%zu bytes)\n", path, xml.size());
	return 1;
}

const char* minimig_get_cfg_info(int num, int label)
{
	char *filename = GetConfigurationName(num, 1, 0);
	if (!filename) return NULL;

	static mm_configTYPE tmpconf;
	memset(&tmpconf, 0, sizeof(tmpconf));

	if (FileLoadConfig(filename, &tmpconf, sizeof(tmpconf)))
	{
		return (label && !tmpconf.kickstart[sizeof(tmpconf.kickstart)-1] && tmpconf.label[0]) ? tmpconf.label : tmpconf.info;
	}

	return "";
}

inline int hdd_open(int unit)
{
	return ide_open(unit, minimig_config.hardfile[unit].filename);
}

static int force_reload_kickstart = 0;
static void ApplyConfiguration(char reloadkickstart)
{
	if (force_reload_kickstart) reloadkickstart = 1;
	force_reload_kickstart = 0;

	minimig_ConfigCPU(minimig_config.cpu);

	if (!reloadkickstart)
	{
		minimig_ConfigChipset(&minimig_config);
		minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
	}

	printf("CPU clock     : %s\n", minimig_config.chipset & 0x01 ? "turbo" : "normal");
	uint8_t memcfg = minimig_config.memory;

	printf("Chip RAM size : %s\n", config_memory_chip_msg[memcfg & 0x03]);
	printf("Slow RAM size : %s\n", config_memory_slow_msg[memcfg >> 2 & 0x03]);
	printf("Fast RAM size : %s\n", config_memory_fast_msg[(minimig_config.cpu >> 1) & 1][((memcfg >> 4) & 0x03) | ((memcfg & 0x80) >> 5)]);

	printf("Floppy drives : %u\n", minimig_config.floppy.drives + 1);
	printf("Floppy speed  : %s\n", minimig_config.floppy.speed ? "fast" : "normal");

	printf("\n");

	printf("\nIDE state: %s.\n", (minimig_config.ide_cfg & 1) ? "enabled" : "disabled");
	if (minimig_config.ide_cfg & 1)
	{
		printf("Primary Master HDD is %s.\n", (minimig_config.hardfile[0].cfg == 2) ? "CD" : minimig_config.hardfile[0].cfg ? "HDD" : "disabled");
		printf("Primary Slave HDD is %s.\n", (minimig_config.hardfile[1].cfg == 2) ? "CD" : minimig_config.hardfile[1].cfg ? "HDD" : "disabled");
		printf("Secondary Master HDD is %s.\n", (minimig_config.hardfile[2].cfg == 2) ? "CD" : minimig_config.hardfile[2].cfg ? "HDD" : "disabled");
		printf("Secondary Slave HDD is %s.\n", (minimig_config.hardfile[3].cfg == 2) ? "CD" : minimig_config.hardfile[3].cfg ? "HDD" : "disabled");
	}

	uint8_t hotswap[4] = {
		minimig_config.hardfile[0].cfg == 2,
		minimig_config.hardfile[1].cfg == 2,
		minimig_config.hardfile[2].cfg == 2,
		minimig_config.hardfile[3].cfg == 2
	};
	ide_reset(hotswap);

	rstval = SPI_CPU_HLT;
	spi_uio_cmd8(UIO_MM2_RST, rstval);
	// Unit 0 is the CD slot and is always offered. Units 1..3 are hard disks,
	// and a stock CD32 has none. This is what "no hard disk support" means now
	// that ide_cfg has to stay set for the CD to work: greying the OSD rows
	// alone would leave a hardfile from an earlier profile mounted and still
	// visible to the guest.
	const int hdd_ok = !is_amigacd() || amigacd_profile_has_hdd(amigacd_system());

	// Unit 0 is the CD. On an AmigaCD machine Akiko serves the disc, so
	// mounting it as an IDE unit as well is redundant -- proven on hardware:
	// with the IDE mount gone the CD still loads and the game still runs, and
	// the memory map came back byte-identical, so the unit itself costs
	// nothing. What it does do is keep the disc dependent on IDE, which is what
	// blocked turning Gayle off.
	//
	// The short-circuit matters: when this is false, hdd_open(0) -- and so
	// ide_open() -- is never called for the image.
	//
	// A physical disc is the exception and stays on IDE; see
	// amigacd_cd_needs_ide().
	const int cd_on_ide = !is_amigacd() || amigacd_cd_needs_ide();

	// CD32+ pays for Gayle in contiguous chip RAM: measured at ~52 KB, which is
	// the difference between Flink running and not on a 2 MB machine. It is a
	// fair price for hard disks and a pointless one without them, so the
	// controller only comes up when a drive is actually configured.
	//
	// Computed here rather than stored, so it cannot drift from the hardfile
	// state. Attaching a drive already sets ide_cfg and hardfile[n].cfg
	// together (MENU_AMIGACD_HDFFILE_SELECTED), so the bit comes back on the
	// next reset -- which a hard disk needs anyway, on this machine as on a
	// real one.
	//
	// Deliberately narrow: only CD32+. A stock CD32 already has no IDE, and
	// A1200 and CDTV keep exactly the behaviour they have today. The physical
	// disc exception applies here too, since that reaches its drive over IDE.
	unsigned char ide_bits = minimig_config.ide_cfg;

	// A STOCK CD32 HAS NO IDE AT ALL, whatever the config file says.
	//
	// amigacd_apply_system() sets ide_cfg correctly when a machine is chosen,
	// and nothing corrects it on LOAD -- so a card whose AmigaCD_CD32.cfg was
	// written while the bit was set keeps it for ever, and the user never
	// re-selects the profile because the machine is already the one they want.
	// That is the third time this asymmetry has bitten: chip RAM and the floppy
	// count are corrected a few lines up for the same reason.
	//
	// It is not cosmetic. Gayle costs about 190 KB of chip RAM and, worse,
	// fragments the low region: docs/chip-ram-shortfall.md measured the largest
	// contiguous block at 1703 KB with IDE on against 1774.8 KB with it off, and
	// Flink wants 1774.8 KB in one piece. Found again on 2026-10-04 with Banshee,
	// which stops at "not enough memory" on a stock CD32 and runs as soon as fast
	// RAM is added -- the classic shape of this fault.
	//
	// Computed here rather than written back, so it cannot drift from what the
	// machine actually is, and so a user who later picks CD32+ still gets IDE.
	if (is_amigacd()
	    && amigacd_system() == AMIGACD_SYS_CD32
	    && !amigacd_cd_needs_ide())
	{
		ide_bits &= (unsigned char)~0x01;
	}

	// CD32+ pays for Gayle only when a hard disk is actually attached.
	if (is_amigacd()
	    && amigacd_system() == AMIGACD_SYS_CD32PLUS
	    && !amigacd_cd_needs_ide()
	    && !minimig_config.hardfile[1].cfg
	    && !minimig_config.hardfile[2].cfg
	    && !minimig_config.hardfile[3].cfg)
	{
		ide_bits &= (unsigned char)~0x01;
	}

	spi_uio_cmd8(UIO_MM2_HDD, (ide_bits & 0x21) |
		((cd_on_ide && hdd_open(0)) ? 2 : 0) |
		((hdd_ok && hdd_open(1)) ? 4 : 0) |
		((hdd_ok && hdd_open(2)) ? 8 : 0) |
		((hdd_ok && hdd_open(3)) ? 16 : 0));

	// cd32_drive.filename is written when the user PICKS a disc, so a config
	// saved before that -- or by any older build -- carries the path in
	// hardfile[0].filename instead. That used to be mounted by hdd_open(0) on
	// the IDE side, and cd_is_mounted() was satisfied through ide_inst. With
	// the disc off IDE nothing mounted it at boot, and the machine came up at
	// the CD32 animation until the disc was selected by hand. Reported from
	// hardware; this is the fallback that fixes it.
	const char *cd0 = minimig_config.cd32_drive.cfg
	                ? minimig_config.cd32_drive.filename : "";
	if (!cd0[0] && minimig_config.hardfile[0].cfg == 2
	    && strcmp(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL))
	{
		cd0 = minimig_config.hardfile[0].filename;
	}
	minimig_cd_drive_open(0, cd0);
	minimig_cd_drive_open(1, minimig_config.cdtv_drive.cfg ? minimig_config.cdtv_drive.filename : "");

	minimig_ConfigMemory(memcfg);
	minimig_ConfigCPU(minimig_config.cpu);

	minimig_ConfigChipset(&minimig_config);
	minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);

	if (minimig_config.memory & 0x40) UploadActionReplay();

	if (reloadkickstart)
	{
		printf("Reloading kickstart ...\n");
		rstval |= (SPI_RST_CPU | SPI_CPU_HLT);
		spi_uio_cmd8(UIO_MM2_RST, rstval);
		const char *extrom = minimig_get_extrom();
		bool uploaded = false;
		if (extrom[0])
		{
			uploaded = UploadKickstartWithExtRom(minimig_config.kickstart, extrom);
		}
		// Cleared here so the reason recorded below belongs to THIS boot and to
		// the first attempt -- the ROM the user actually configured.
		rom_fail_reason[0] = 0;

		if (!uploaded && !UploadKickstart(minimig_config.kickstart))
		{
			snprintf(minimig_config.kickstart, sizeof(minimig_config.kickstart) - 1, "%s/%s", HomeDir(), "KICK.ROM");
			if (!UploadKickstart(minimig_config.kickstart))
			{
				strcpy(minimig_config.kickstart, "KICK.ROM");
				if (!UploadKickstart(minimig_config.kickstart))
				{
					BootPrintEx("No Kickstart loaded. Press F12 for settings.");
					// The cause, on the screen the user is looking at. Every
					// message about why a ROM would not load went to BootPrint,
					// which is printf -- the log, not the boot screen. So the
					// machine halted with no reason given anywhere the user
					// would see it, and the one they most often need is
					// "your ROM is encrypted and ROM.KEY is not next to it".
					if (rom_fail_reason[0]) BootPrintEx(rom_fail_reason);
					BootPrintEx("** Halted! **");
					return;
				}
			}
		}
		rstval |= (SPI_RST_USR | SPI_RST_CPU);
		spi_uio_cmd8(UIO_MM2_RST, rstval);
	}
	else
	{
		printf("Resetting ...\n");
		rstval |= (SPI_RST_USR | SPI_RST_CPU);
		spi_uio_cmd8(UIO_MM2_RST, rstval);
	}

	rstval = 0;
	spi_uio_cmd8(UIO_MM2_RST, rstval);

	minimig_ConfigVideo(minimig_config.scanlines);
	minimig_ConfigAudio(minimig_config.audio);

	// Joystick mode lives in autofire bits [2:1] (0=Digital, 1=Analog, 2=CD32).
	// userio.v:466 decodes it as cd32pad = ~IO_DIN[1] & IO_DIN[2], and only
	// feeds the CD32 pad shift register when that is set (userio.v:332). In
	// Digital mode the red button is still readable as plain pin-6 fire, so a
	// game that polls fire directly works while one that clocks the serial pad
	// protocol reads nothing -- Lotus Trilogy's title selector responds, its
	// in-game menu does not, and directions keep working throughout because
	// they come from the JOYxDAT quadrature counters, not the shift register.
	//
	// The full Minimig OSD lets the player pick the mode, but AmigaCD jumps
	// straight to MENU_AMIGACD_MAIN1 (menu.cpp:5828) and never shows that row,
	// so on this variant it can only ever be whatever was stored -- 0 for a
	// fresh config. A real CD32 always has a CD32 pad and a CDTV never does,
	// so derive it from the system instead of trusting the saved value.
	if (is_amigacd())
	{
		minimig_config.autofire = (minimig_config.autofire & ~6) |
		                          (amigacd_is_cdtv() ? 0 : 4);
	}
	minimig_ConfigAutofire(minimig_config.autofire, 0xC);
	minimig_ConfigFloppyExt(minimig_config.floppy.extDrives[0], minimig_config.floppy.extDrives[1],
	                        minimig_config.floppy.extDrives[2], minimig_config.floppy.extDrives[3]);
	minimig_ConfigSnac();
	minimig_set_extcfg(minimig_get_extcfg() & ~1);
}

// Save settings, from the OSD row of that name.
//
// Two files, not one. <core>.cfg is the live config and is what a core load
// reads back, so it has to hold whatever machine the user is on right now.
// The per-machine file is that machine's memory, and keeping it current here
// means "Save settings" and the System row agree about what an A1200 is rather
// than the row handing back something older.
//
// For CD32 the two are the same file, so it is written once.
int amigacd_cfg_save_current(void)
{
	int sys = amigacd_system();
	int ok = minimig_cfg_save(0);
	const char *suffix = amigacd_cfg_suffix(sys);
	if (suffix) ok = minimig_cfg_save(0, suffix) && ok;
	return ok;
}

// Remember the machine we are leaving, then become the one we are going to.
//
// The System row used to apply a profile and nothing else, so anything set on
// top of it -- the mounted disc, the hard disks, FastRAM, the ROM -- was lost
// the moment the row was pressed, and pressing it back did not bring it back.
// Each machine now keeps its own config file.
//
// Returns 1 if the new machine had a saved config, 0 if this is the first time
// it has been selected and the caller should build it from the profile.
int amigacd_switch_system(int from_sys, int to_sys)
{
	minimig_cfg_save(0, amigacd_cfg_suffix(from_sys));

	// Checked before loading rather than letting the load fall through: the
	// no-config path inside minimig_cfg_load resets every hardfile and applies
	// the CD32 profile, which is right at boot and wrong here.
	if (!GetConfigurationName(0, 1, amigacd_cfg_suffix(to_sys))) return 0;

	minimig_cfg_load(0, amigacd_cfg_suffix(to_sys));

	// Verify the arrival. A stored config that does not describe the machine it
	// is filed under leaves the System row apparently frozen -- the user presses
	// it and lands back where they were -- and there is nothing on screen to say
	// why. Treating it as "never configured" hands the caller the profile path
	// instead, which loses whatever that file held but always moves.
	if (amigacd_system() != to_sys)
	{
		printf("AmigaCD: %s config describes a %s, ignoring it\n",
		       amigacd_system_name(to_sys), amigacd_system_name(amigacd_system()));
		return 0;
	}
	return 1;
}

int minimig_cfg_load(int num, const char *sys_suffix)
{
	static const char config_id[] = "MNMGCFG0";
	int result = 0;

	const char *filename = GetConfigurationName(num, 1, sys_suffix);

	// load configuration data
	int size;
	if(filename && (size = FileLoadConfig(filename, 0, 0))>0)
	{
		BootPrint("Opened configuration file\n");
		printf("Configuration file size: %s, %d\n", filename, size);
		if (size == sizeof(mm_configTYPE_NoExtFloppy) || size == 5152)
		{
			// Written before MiSTer Floppy. Copy field by field -- a straight
			// memcpy would shift everything after `floppy`. External drives
			// default to none and the user port to MT32-pi, which is what a
			// config that predates the feature meant.
			printf("Pre ext-floppy configuration file, migrating.\n");
			static mm_configTYPE_NoExtFloppy oldconf = {};
			if (FileLoadConfig(filename, &oldconf, sizeof(oldconf))
			    && !strncmp(oldconf.id, config_id, sizeof(minimig_config.id))
			    && oldconf.floppy.drives <= 4)
			{
				memcpy(minimig_config.id, oldconf.id, sizeof(oldconf.id));
				minimig_config.version  = oldconf.version;
				minimig_config.ext_cfg2 = oldconf.ext_cfg2;
				memcpy(minimig_config.kickstart, oldconf.kickstart, sizeof(oldconf.kickstart));
				memcpy(minimig_config.label,     oldconf.label,     sizeof(oldconf.label));
				minimig_config.ext_cfg     = oldconf.ext_cfg;
				minimig_config.memory      = oldconf.memory;
				minimig_config.chipset     = oldconf.chipset;
				minimig_config.floppy.speed  = oldconf.floppy.speed;
				minimig_config.floppy.drives = oldconf.floppy.drives;
				memset(minimig_config.floppy.extDrives, 0, sizeof(minimig_config.floppy.extDrives));
				minimig_config.disable_ar3 = oldconf.disable_ar3;
				minimig_config.ide_cfg     = oldconf.ide_cfg;
				minimig_config.scanlines   = oldconf.scanlines;
				minimig_config.audio       = oldconf.audio;
				memcpy(minimig_config.hardfile, oldconf.hardfile, sizeof(oldconf.hardfile));
				minimig_config.cpu      = oldconf.cpu;
				minimig_config.autofire = oldconf.autofire;
				memcpy(minimig_config.info, oldconf.info, sizeof(oldconf.info));
				minimig_config.userport = mmup_mp32pi;
				minimig_config.snac[0] = mmsp_off;
				minimig_config.snac[1] = mmsp_off;
				derive_cd_slots_from_legacy();
				result = 1;
			}
			else BootPrint("Cannot migrate configuration file\n");
		}
		else if (size == sizeof(mm_configTYPE_NoSnac))
		{
			// Written before PSX SNAC. Copy field by field -- a straight memcpy
			// would leave snac[] holding whatever garbage followed the file on
			// disk instead of a defined "both ports off".
			printf("Pre PSX SNAC configuration file, migrating.\n");
			static mm_configTYPE_NoSnac oldconf = {};
			if (FileLoadConfig(filename, &oldconf, sizeof(oldconf))
			    && !strncmp(oldconf.id, config_id, sizeof(minimig_config.id))
			    && oldconf.floppy.drives <= 4)
			{
				memcpy(minimig_config.id, oldconf.id, sizeof(oldconf.id));
				minimig_config.version  = oldconf.version;
				minimig_config.ext_cfg2 = oldconf.ext_cfg2;
				memcpy(minimig_config.kickstart, oldconf.kickstart, sizeof(oldconf.kickstart));
				memcpy(minimig_config.label,     oldconf.label,     sizeof(oldconf.label));
				minimig_config.ext_cfg     = oldconf.ext_cfg;
				minimig_config.memory      = oldconf.memory;
				minimig_config.chipset     = oldconf.chipset;
				minimig_config.floppy      = oldconf.floppy;
				minimig_config.disable_ar3 = oldconf.disable_ar3;
				minimig_config.ide_cfg     = oldconf.ide_cfg;
				minimig_config.scanlines   = oldconf.scanlines;
				minimig_config.audio       = oldconf.audio;
				memcpy(minimig_config.hardfile, oldconf.hardfile, sizeof(oldconf.hardfile));
				minimig_config.cpu      = oldconf.cpu;
				minimig_config.autofire = oldconf.autofire;
				memcpy(minimig_config.info, oldconf.info, sizeof(oldconf.info));
				minimig_config.userport = oldconf.userport;
				minimig_config.snac[0] = mmsp_off;
				minimig_config.snac[1] = mmsp_off;
				derive_cd_slots_from_legacy();
				result = 1;
			}
			else BootPrint("Cannot migrate configuration file\n");
		}
		else if (size == sizeof(mm_configTYPE_NoCdSlots))
		{
			// Written before the CD32/CDTV drive slots. Same field-by-field
			// shape as the rungs above: a straight memcpy of sizeof(oldconf)
			// would drop the old layout's last padding byte onto
			// cd32_drive.cfg, and the drive filenames would keep whatever the
			// previous load left in them.
			printf("Pre CD-slot configuration file, migrating.\n");
			static mm_configTYPE_NoCdSlots oldconf = {};
			if (FileLoadConfig(filename, &oldconf, sizeof(oldconf))
			    && !strncmp(oldconf.id, config_id, sizeof(minimig_config.id))
			    && oldconf.floppy.drives <= 4)
			{
				memcpy(minimig_config.id, oldconf.id, sizeof(oldconf.id));
				minimig_config.version  = oldconf.version;
				minimig_config.ext_cfg2 = oldconf.ext_cfg2;
				memcpy(minimig_config.kickstart, oldconf.kickstart, sizeof(oldconf.kickstart));
				memcpy(minimig_config.label,     oldconf.label,     sizeof(oldconf.label));
				minimig_config.ext_cfg     = oldconf.ext_cfg;
				minimig_config.memory      = oldconf.memory;
				minimig_config.chipset     = oldconf.chipset;
				minimig_config.floppy      = oldconf.floppy;
				minimig_config.disable_ar3 = oldconf.disable_ar3;
				minimig_config.ide_cfg     = oldconf.ide_cfg;
				minimig_config.scanlines   = oldconf.scanlines;
				minimig_config.audio       = oldconf.audio;
				memcpy(minimig_config.hardfile, oldconf.hardfile, sizeof(oldconf.hardfile));
				minimig_config.cpu      = oldconf.cpu;
				minimig_config.autofire = oldconf.autofire;
				memcpy(minimig_config.info, oldconf.info, sizeof(oldconf.info));
				minimig_config.userport = oldconf.userport;
				minimig_config.snac[0]  = oldconf.snac[0];
				minimig_config.snac[1]  = oldconf.snac[1];
				minimig_config.ss_slot  = oldconf.ss_slot & 3;
				derive_cd_slots_from_legacy();
				result = 1;
			}
			else BootPrint("Cannot migrate configuration file\n");
		}
		else if (size == sizeof(mm_configTYPE_NoCdDelay))
		{
			// Written before the CD mount delay existed. A straight memcpy
			// would leave cd_mount_delay reading whatever followed in memory,
			// and a zero there means "no delay" -- silently restoring the
			// behaviour this setting exists to fix. So it is set explicitly.
			printf("Pre CD-delay configuration file, migrating.\n");
			static mm_configTYPE_NoCdDelay oldconf = {};
			if (FileLoadConfig(filename, &oldconf, sizeof(oldconf))
			    && !strncmp(oldconf.id, config_id, sizeof(minimig_config.id))
			    && oldconf.floppy.drives <= 4)
			{
				memcpy((void*)&minimig_config, (void*)&oldconf, sizeof(oldconf));
				minimig_config.cd_mount_delay = AMIGACD_CD_DELAY_DEFAULT;
				// Also appended after this layout; the memcpy above stops
				// short of it, so it would keep whatever the previous load
				// left there. The shipped rate is the value that changes
				// nothing.
				// A file that predates the field carries no preference, so it
				// gets the current default rather than the old constant.
				minimig_config.cd_speed = AMIGACD_CD_SPEED_DEFAULT;
				result = 1;
			}
			else BootPrint("Cannot migrate configuration file\n");
		}
		else if (size == (int)sizeof(minimig_config))
		{
			static mm_configTYPE tmpconf = {};
			if (FileLoadConfig(filename, &tmpconf, sizeof(tmpconf)))
			{
				// check file id and version
				if (strncmp(tmpconf.id, config_id, sizeof(minimig_config.id)) == 0) {
					// A few more sanity checks...
					if (tmpconf.floppy.drives <= 4) {
						memcpy((void*)&minimig_config, (void*)&tmpconf, sizeof(minimig_config));
						result = 1; // We successfully loaded the config.
					}
					else BootPrint("Config file sanity check failed!\n");
				}
				else BootPrint("Wrong configuration file format!\n");
			}
			else printf("Cannot load configuration file\n");
		}
		else if (size == sizeof(configTYPE_old))
		{
			static configTYPE_old tmpconf;
			printf("Old Configuration file.\n");
			if (FileLoadConfig(filename, &tmpconf, sizeof(tmpconf)))
			{
				// check file id and version
				if (strncmp(tmpconf.id, config_id, sizeof(minimig_config.id)) == 0) {
					// A few more sanity checks...
					if (tmpconf.floppy.drives <= 4) {
						memset((void*)&minimig_config, 0, sizeof(minimig_config));
						memcpy((void*)&minimig_config, (void*)&tmpconf, sizeof(tmpconf));
						minimig_config.cpu = tmpconf.cpu;
						minimig_config.autofire = tmpconf.autofire;
						memset(&minimig_config.hardfile[2], 0, sizeof(minimig_config.hardfile[2]));
						memset(&minimig_config.hardfile[3], 0, sizeof(minimig_config.hardfile[3]));
						result = 1; // We successfully loaded the config.
					}
					else BootPrint("Config file sanity check failed!\n");
				}
				else BootPrint("Wrong configuration file format!\n");
			}
			else printf("Cannot load configuration file\n");
		}
		else if ((size_t)size <= sizeof(minimig_config))
		{
			static mm_configTYPE tmpconf = {};
			memset((void*)&tmpconf, 0, sizeof(tmpconf));
			if (FileLoadConfig(filename, &tmpconf, sizeof(tmpconf)))
			{
				// check file id and version
				if (strncmp(tmpconf.id, config_id, sizeof(minimig_config.id)) == 0) {
					// A few more sanity checks...
					if (tmpconf.floppy.drives <= 4) {
						memcpy((void*)&minimig_config, (void*)&tmpconf, sizeof(minimig_config));
						result = 1; // We successfully loaded the config.
					}
					else BootPrint("Config file sanity check failed!\n");
				}
				else BootPrint("Wrong configuration file format!\n");
			}
			else printf("Cannot load configuration file\n");
		}
		else printf("Wrong configuration file size: %d (expected: %u)\n", size, sizeof(minimig_config));
	}

	if (!result) {
		BootPrint("Can not open configuration file!\n");
		BootPrint("Setting config defaults\n");
		// set default configuration
		memset((void*)&minimig_config, 0, sizeof(minimig_config));
		memcpy(minimig_config.id, config_id, sizeof(minimig_config.id));
		snprintf(minimig_config.kickstart, sizeof(minimig_config.kickstart) - 1, "%s/%s", HomeDir(), "KICK.ROM");
		minimig_config.memory = 0x11;
		minimig_config.cpu = 0;
		minimig_config.chipset = 0;
		minimig_config.floppy.speed = CONFIG_FLOPPY2X;
		minimig_config.floppy.drives = 1;
		minimig_config.ide_cfg = 0;
		minimig_config.hardfile[0].cfg = 1;
		minimig_config.hardfile[0].filename[0] = 0;
		minimig_config.hardfile[1].cfg = 1;
		minimig_config.hardfile[1].filename[0] = 0;
		minimig_config.hardfile[2].cfg = 0;
		minimig_config.hardfile[2].filename[0] = 0;
		minimig_config.hardfile[3].cfg = 0;
		minimig_config.hardfile[3].filename[0] = 0;
		minimig_config.cd32_drive.cfg = 0;
		minimig_config.cd32_drive.filename[0] = 0;
		minimig_config.cdtv_drive.cfg = 0;
		minimig_config.cdtv_drive.filename[0] = 0;
		minimig_config.cd_mount_delay = AMIGACD_CD_DELAY_DEFAULT;
		minimig_config.cd_speed       = AMIGACD_CD_SPEED_DEFAULT;
		BootPrintEx(">>> No config found. Using defaults. <<<");

		// AmigaCD console variant: no saved CFG -> start as a CD32 with its
		// authentic profile + PSX-style default ROMs auto-found in /media/fat/.
		if (is_amigacd())
		{
			amigacd_apply_system(AMIGACD_SYS_CD32);
			BootPrintEx(">>> AmigaCD: defaulting to CD32. <<<");
		}
	}
	else if (is_amigacd())
	{
		// A loaded config carries whatever chip RAM was last persisted, and
		// until now nothing ever corrected it: amigacd_apply_system() runs only
		// in the no-config branch above, and mounting a CD image
		// (MENU_AMIGACD_CDFILE_SELECTED) only sets the filename and calls
		// ide_open. So a config holding the generic Minimig default (0x11) or a
		// CDTV profile (0x01) -- both 1 MB chip -- left CD32 discs running on
		// half the chip RAM a CD32 has. Many titles need more than 1 MB and
		// failed with an out-of-memory message; the less demanding ones ran,
		// which is what made it look intermittent.
		//
		// Chip RAM is a property of the machine, not a preference: the AmigaCD
		// OSD offers no row for it. Re-assert it from the system the config
		// itself names, leaving FastRAM and HRTmon untouched. SlowRAM is
		// part of the profile for an A500 and the user's elsewhere.
		// The announce hold, for the same reason in the same shape. Changing
		// AMIGACD_CD_DELAY_DEFAULT only reaches a card with NO config: every
		// machine that has booted this core carries a stored hold, and leaving
		// it there ships the fix to nobody. That is the third time this
		// asymmetry has bitten -- chip RAM and the floppy count below are the
		// other two.
		//
		// Both values this core has handed out are cleared: 8, which lost the
		// boot read, and 14, which replaced it. Neither was ever chosen by
		// anyone, and a CD32 needs no hold at all -- see minimig_config.h for
		// the measurements and the reference run.
		//
		// This DOES overwrite a deliberate 8 or 14. Nothing in the file tells a
		// chosen value from a handed-out one, and shipping a known-bad default
		// to everyone is worse than overriding the few who picked it: they can
		// set 0..15 again, and /tmp/akiko_boot_delay overrides without a
		// rebuild. Any OTHER stored value is the user's and is left alone.
		if (minimig_config.cd_mount_delay == AMIGACD_CD_DELAY_LEGACY
		    || minimig_config.cd_mount_delay == AMIGACD_CD_DELAY_LEGACY2)
		{
			printf("AmigaCD: clearing the CD announce hold %us -> %us "
			       "(a CD32 needs none; see minimig_config.h)\n",
			       minimig_config.cd_mount_delay, AMIGACD_CD_DELAY_DEFAULT);
			minimig_config.cd_mount_delay = AMIGACD_CD_DELAY_DEFAULT;
		}

		int sys = amigacd_system();
		uint8_t fixed = amigacd_profile_memory(minimig_config.memory, sys);
		if (fixed != minimig_config.memory)
		{
			printf("AmigaCD: correcting chip RAM for %s: memory 0x%02X -> 0x%02X\n",
			       amigacd_system_name(sys), minimig_config.memory, fixed);
			minimig_config.memory = fixed;
		}

		// The floppy drive count needs exactly the same treatment, and for
		// exactly the same reason. Making it a machine property in
		// amigacd_apply_system() only reached machines the user re-selected:
		// every config already on a card kept its stored count, which for a
		// CD32 written before that change is TWO drives (the field is a count
		// minus one). Kickstart probes both and trackdisk.device takes an MFM
		// track buffer per unit out of chip RAM, which is what the change was
		// meant to stop.
		//
		// Only CD32 and CDTV are forced here. For an A500 or A1200 the count is
		// a preference with an OSD row, and amigacd_profile_floppy_drives_onload
		// returns it untouched -- forcing it every load would undo the user's
		// choice the same way forcing FastRAM above would.
		uint8_t fixed_drives =
			amigacd_profile_floppy_drives_onload(minimig_config.floppy.drives, sys);
		if (fixed_drives != minimig_config.floppy.drives)
		{
			printf("AmigaCD: correcting floppy drives for %s: %d -> %d\n",
			       amigacd_system_name(sys),
			       minimig_config.floppy.drives + 1, fixed_drives + 1);
			minimig_config.floppy.drives = fixed_drives;
		}
	}

	if ((minimig_config.cpu & 0x03) == 0x02) minimig_config.cpu |= 0x01;

	a2065_cfg_set(minimig_config.a2065_mode);

	for (int i = 0; i < 4; i++)
	{
		df[i].status = 0;
		FileClose(&df[i].file);
	}

	// print config to boot screen
	char cfg_str[256];
	sprintf(cfg_str, "CPU: %s, Chipset: %s, ChipRAM: %s, FastRAM: %s, SlowRAM: %s",
		config_cpu_msg[minimig_config.cpu & 0x03], config_chipset_msg[(minimig_config.chipset >> 2) & 7],
		config_memory_chip_msg[(minimig_config.memory >> 0) & 0x03], config_memory_fast_msg[(minimig_config.cpu >> 1) & 1][((minimig_config.memory >> 4) & 0x03) | ((minimig_config.memory & 0x80) >> 5)], config_memory_slow_msg[(minimig_config.memory >> 2) & 0x03]
	);
	BootPrintEx(cfg_str);

	input_poll(0);
	if (is_key_pressed(59))
	{
		BootPrintEx("Forcing NTSC video ...");
		//force NTSC mode if F1 pressed
		minimig_config.chipset |= CONFIG_NTSC;
	}
	else if (is_key_pressed(60))
	{
		BootPrintEx("Forcing PAL video ...");
		// force PAL mode if F2 pressed
		minimig_config.chipset &= ~CONFIG_NTSC;
	}

	ApplyConfiguration(1);
	return(result);
}

void minimig_reset()
{
	ApplyConfiguration(0);
	user_io_rtc_reset();
	minimig_share_reset();
	akiko_cd32_init();
	cdtv_cd_init();
	a2065_start();
}

void minimig_set_kickstart(const char *name)
{
	uint len = strlen(name);
	if (len > (sizeof(minimig_config.kickstart) - 1)) len = sizeof(minimig_config.kickstart) - 1;
	memcpy(minimig_config.kickstart, name, len);
	// Zero the tail. This also clears any previously-set ext-ROM string
	// (stored past the first null) — pairing a stale ext-ROM with a new
	// main ROM is almost never what the user wants, and they can re-pick.
	memset(minimig_config.kickstart + len, 0, sizeof(minimig_config.kickstart) - len);
	force_reload_kickstart = 1;
}

void minimig_set_extrom(const char *name)
{
	int cap = sizeof(minimig_config.kickstart);
	int kicklen = strnlen(minimig_config.kickstart, cap) + 1;
	if (kicklen + 1 >= cap) return; // at least one additional byte for extrom is required (for NULL termination)
	int room = cap - kicklen;
	int nlen = strlen(name);
	if (nlen > room) nlen = room;
	memcpy(minimig_config.kickstart + kicklen, name, nlen);
	room = cap - kicklen - nlen;
	if(room > 0) memset(minimig_config.kickstart + kicklen + nlen, 0, room);
	minimig_config.kickstart[cap - 1] = 0; // make sure NULL is at the end
	force_reload_kickstart = 1;
}

// ---- AmigaCD console variant: per-system profiles + default ROM auto-load ----
// PSX-BIOS style: a small set of default Kickstart filenames live in the core
// boot folder (/media/fat/ = getRootDir()); the System toggle picks which pair
// to auto-load. Names follow the FS-UAE / Amiga Forever convention. See
// research/docs/amiga-console-design-2026-06-13.md and the shipped AmigaCD-ROMs
// guide.

void amigacd_default_rom_names(int sys, const char **main_name, const char **ext_name)
{
	if (sys == AMIGACD_SYS_CDTV)
	{
		if (main_name) *main_name = "kick34005.CDTV";       // Kickstart 1.3 rev 34.5 (256K)
		if (ext_name)  *ext_name  = "kick34005.CDTV.ext";   // CDTV extended ROM
	}
	else if (sys == AMIGACD_SYS_A500)
	{
		// An A500 has no extended ROM. The name is still returned so callers
		// need no special case: amigacd_set_default_roms only pairs an ext ROM
		// when the file exists, and this one is not expected to.
		if (main_name) *main_name = "kick34005.A500";       // Kickstart 1.3 rev 34.5 (256K)
		if (ext_name)  *ext_name  = "kick34005.A500.ext";
	}
	else if (sys == AMIGACD_SYS_A1200)
	{
		// Kickstart 3.1 rev 40.68, the A1200 ROM. Not the CD32's 40.60: that
		// one boots the CD and never reaches the hard disk, which is the whole
		// reason this profile exists. No extended ROM on an A1200; the name is
		// returned anyway because amigacd_set_default_roms only pairs one when
		// the file is actually there.
		if (main_name) *main_name = "kick40068.A1200";
		if (ext_name)  *ext_name  = "kick40068.A1200.ext";
	}
	else
	{
		if (main_name) *main_name = "kick40060.CD32";       // Kickstart 3.1 rev 40.60 (512K)
		if (ext_name)  *ext_name  = "kick40060.CD32.ext";   // CD32 extended ROM
	}
}

// Point the main + ext ROM at this system's defaults under the boot folder.
// Only sets the ext pairing if the ext file actually exists, so a missing ext
// ROM doesn't force the composite path / a load failure.
int amigacd_set_default_roms(int sys)
{
	const char *mname = 0, *ename = 0;
	amigacd_default_rom_names(sys, &mname, &ename);

	char mpath[1024], epath[1024];
	snprintf(mpath, sizeof(mpath), "%s/%s", getRootDir(), mname);
	snprintf(epath, sizeof(epath), "%s/%s", getRootDir(), ename);

	// The main ROM is only replaced when the default is actually there. It
	// used to be set unconditionally, which meant switching machine on a card
	// whose ROMs are named anything else -- CD32.rom rather than
	// kick40060.CD32, say, which is what MiSTer's own Amiga sets ship as --
	// pointed the config at a file that does not exist and took the working
	// ROM with it. The ext ROM has always been guarded this way; the main one
	// is the half that matters more.
	if (!FileExists(mpath, 0))
	{
		printf("AmigaCD: default ROM for %s not found (%s), keeping the current one\n",
		       amigacd_system_name(sys), mpath);
		return 0;
	}

	minimig_set_kickstart(mpath);              // also clears any stale ext pairing
	if (FileExists(epath, 0)) minimig_set_extrom(epath);
	return 1;
}

// Apply a complete authentic CD32 or CDTV machine profile (CPU / chipset /
// memory / IDE) + default ROMs, and push the live config to the FPGA. ChipRAM
// is baked into the profile; FastRAM and D-Cache are user-adjustable in the
// AmigaCD Settings menu. Defaults match an authentic CD32: D-Cache OFF (the
// 68EC020 has no data cache) and no FastRAM. CDTV is a 68000 (cpu 0x00,
// chipset 0x20, 1M chip).
// Does this machine's CD have to be reached over IDE?
//
// Only for a PHYSICAL disc. physical_disc_autoboot.cpp mounts it with
// ide_open(), so the controller has to be there. An image is served by Akiko and
// needs no IDE at all, which is what lets a stock CD32 drop Gayle and recover
// the contiguous chip RAM that CD32 titles want.
//
// Not a machine property, so it is not in minimig_profile.h: it depends on what
// is in the drive right now, not on which machine was selected.
int amigacd_cd_needs_ide(void)
{
	return !strcmp(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL);
}

int amigacd_apply_system(int sys)
{
	if (sys == AMIGACD_SYS_CDTV)
	{
		minimig_config.cpu     = 0x00; // 68000
		minimig_config.chipset = 0x20; // OCS + CONFIG_CDTV
		minimig_config.memory  = 0x01; // 1M chip, no fast
		minimig_config.ide_cfg = 0x00; // CDTV bridge (no IDE)
	}
	else if (sys == AMIGACD_SYS_A500)
	{
		// A plain A500: 68000, OCS, 512K chip + the 512K trapdoor card, and no
		// CD hardware at all -- neither the CDTV bridge nor Akiko. Games come
		// from df0 and hardfiles, which the Floppy and CD rows already cover.
		//
		// The 68000 here is fx68k, the same core the CDTV profile selects
		// (cpucfg 0). Save states are refused in that mode -- AmigaCD.sv gates
		// them on `ss_supported = |cpucfg` -- so an A500 cannot save state
		// until fx68k gains an export and restore path.
		minimig_config.cpu     = 0x00; // 68000
		minimig_config.chipset = 0x00; // OCS, no CDTV bridge
		minimig_config.memory  = 0x04; // 512K chip + 512K slow, no fast
		minimig_config.ide_cfg = 0x00; // no CD bridge
	}
	else if (sys == AMIGACD_SYS_A1200)
	{
		// An A1200 with a hard disk: AGA, 68EC020 with the D-Cache on, 2 MB
		// chip and 8 MB of Zorro II fast, Gayle IDE, and NO CD drive.
		//
		// The missing CD drive is the point of this profile, not an omission.
		// A CD32 boots its own ROM, which owns the machine and goes looking
		// for a disc; ejecting one does not hand the boot over to the hard
		// disk. Hard-disk software -- AmigaVision, a Workbench install --
		// needs a machine that has no CD boot path at all and an A1200
		// Kickstart that enumerates IDE, which is this.
		//
		// hardfile[0].cfg = 0 below is what removes the drive, and it doubles
		// as how amigacd_system() tells an A1200 from a CD32: the two agree on
		// every other bit.
		minimig_config.cpu     = 0x13; // 68EC020, D-Cache ON
		minimig_config.chipset = 0x18; // AGA + ECS (PAL)
		// 8 MB of Zorro II fast is a seed, not a clamp: it is set here, when
		// the user picks the machine, and left alone afterwards. Hard-disk
		// software wants fast RAM and a machine that boots into AmigaVision
		// with none is not a configuration anyone chose on purpose -- but the
		// FastRAM row still owns it from this point on.
		minimig_config.memory  = 0x33; // 2M chip + 8M Zorro II fast
		minimig_config.ide_cfg = 0x01; // Gayle IDE
	}
	else if (sys == AMIGACD_SYS_CD32PLUS)
	{
		// A CD32 with the expansions people actually fitted -- an SX-1 or an
		// SX-32 brought a floppy drive and IDE. Identical to the stock machine
		// in CPU, chipset and chip RAM, because that is what those cartridges
		// left alone.
		//
		// Both CD32s run with IDE enabled, so the marker is a spare persisted
		// byte instead -- see AMIGACD_HF0_CD32PLUS. It is set below, after
		// hardfile[0] is configured, so that assignment cannot clear it.
		minimig_config.cpu     = 0x03; // 68EC020, D-Cache OFF, as the CD32
		minimig_config.chipset = 0x18; // AGA + ECS (PAL)
		minimig_config.memory  = 0x03; // 2M chip, no fast
		minimig_config.ide_cfg = 0x01; // as the stock machine; the CD needs it
	}
	else
	{
		// A stock CD32: no floppy drive, and no hard disks offered.
		//
		// IDE STAYS ENABLED, and the comment that used to sit here was wrong.
		// It argued that clearing ide_cfg was safe because Akiko is decoded
		// separately -- sel_akiko in fastchip.v really is independent of
		// ide_ena, so that half was correct and irrelevant. The CD32's disc
		// also travels the USERSPACE IDE path: hardfile[0] holds the same
		// image and ide_open runs against it on every config apply. Clearing
		// the bit left the drive present at mount and gone seconds later, when
		// the machine reset to boot it. Measured on hardware -- the CHD
		// mounted, its 19-track TOC built, then "media level -> absent".
		//
		// So a stock CD32 is not authentic in this one respect, and cannot be
		// until the CD is served without the IDE path. What it does not get is
		// any hard disk: amigacd_profile_has_hdd() keeps units 1..3 unopened
		// and their OSD rows greyed, which is the part that was asked for.
		minimig_config.cpu     = 0x03; // 68EC020, D-Cache OFF (authentic CD32 default)
		minimig_config.chipset = 0x18; // AGA + ECS (PAL)
		minimig_config.memory  = 0x03; // 2M chip, no fast (authentic CD32 default)
		// No IDE. A real CD32 has none, and on this core it is not free:
		// Gayle's guest-side allocations cost about 190 KB and, worse, they
		// fragment low memory. Measured on hardware -- with IDE enabled the
		// largest contiguous block is 1703 KB, and Flink wants a single
		// 1774.8 KB allocation, so it fails. With IDE off the block reaches
		// exactly 1774.8 KB and the game runs. See docs/chip-ram-shortfall.md.
		//
		// This is safe now only because the disc no longer travels the IDE
		// path: Akiko serves it, which was proven separately before this was
		// switched off. An earlier attempt to clear ide_cfg on its own took the
		// CD away entirely and shipped as a regression -- the disc's only mount
		// was the IDE unit at the time.
		//
		// A PHYSICAL disc still needs IDE: physical_disc_autoboot.cpp opens it
		// with ide_open(). So the bit stays set for that one case, and such a
		// machine pays the memory. An image gets the authentic, lean machine.
		minimig_config.ide_cfg = amigacd_cd_needs_ide() ? 0x01 : 0x00;
	}
	// One removable CD slot on every machine that has a CD drive. The A1200
	// has none -- see above.
	minimig_config.hardfile[0].cfg = (sys == AMIGACD_SYS_A1200) ? 0 : 2;

	// After the line above, which writes the same struct. This is what tells a
	// CD32+ from a stock CD32 on the next config load.
	if (sys == AMIGACD_SYS_CD32PLUS)
		minimig_config.hardfile[0].reserved |= AMIGACD_HF0_CD32PLUS;
	else
		minimig_config.hardfile[0].reserved &= (unsigned char)~AMIGACD_HF0_CD32PLUS;

	// Upstream's dedicated drive slots have to agree with the profile:
	// minimig_ConfigChipset() derives CONFIG_CDTV from cdtv_drive.cfg and
	// clears the bit when that slot is empty, so setting minimig_config.chipset
	// above is not on its own enough to bring the core up as a CDTV.
	minimig_config.cdtv_drive.cfg = (sys == AMIGACD_SYS_CDTV) ? 1 : 0;
	minimig_config.cd32_drive.cfg =
		(sys == AMIGACD_SYS_CD32 || sys == AMIGACD_SYS_CD32PLUS) ? 1 : 0;

	// Floppy drive count is a machine property, so it comes from the profile.
	// This used to read
	//
	//     if (minimig_config.floppy.drives < 1) minimig_config.floppy.drives = 1;
	//
	// commented "single df0" -- but the field is a count minus one, so that is
	// TWO drives, and it ran for every machine including CD32 and CDTV, which
	// have none. Kickstart then probed two drives and spent chip RAM on a
	// trackdisk buffer for each. See amigacd_profile_floppy_drives().
	minimig_config.floppy.drives = amigacd_profile_floppy_drives(sys);

	int rom_ok = amigacd_set_default_roms(sys);

	minimig_ConfigCPU(minimig_config.cpu);
	minimig_ConfigChipset(&minimig_config);
	minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
	// memory is applied on reset via the UploadKickstart / ApplyConfiguration path
	return rom_ok;
}

int amigacd_is_cdtv(void)
{
	return (minimig_config.chipset & CONFIG_CDTV) ? 1 : 0;
}

// Which machine the live config describes. Derived rather than stored, so a
// config written before A500 existed still reads correctly: CONFIG_CDTV names
// the CDTV, and of what is left a 68000 with no CD bridge is the A500 -- a
// CD32 is always a 68EC020.
int amigacd_system(void)
{
	if (minimig_config.chipset & CONFIG_CDTV) return AMIGACD_SYS_CDTV;
	if ((minimig_config.cpu & 0x03) == 0)     return AMIGACD_SYS_A500;
	// A1200 and CD32 agree on CPU and chipset; the CD drive is what separates
	// them, and its absence is a property of the machine rather than of what
	// the user happens to have mounted. hardfile[0].cfg is the slot, not the
	// disc: 2 means "there is a removable CD device", 0 means there is not.
	if (minimig_config.hardfile[0].cfg == 0)  return AMIGACD_SYS_A1200;

	// Stock CD32 and CD32+ agree on CPU, chipset, chip RAM and now IDE as well,
	// so the marker is an explicit bit in a spare persisted byte. IDE was tried
	// first and had to be abandoned: clearing it for the stock machine took
	// that machine's own CD away, on hardware.
	//
	// A config written before this bit existed reads 0 and is therefore a stock
	// CD32, so anyone with hard disks configured picks CD32+ once to get them
	// back. That is the safe direction now: a visibly missing drive beats a
	// machine that quietly is not the one the System row names.
	return (minimig_config.hardfile[0].reserved & AMIGACD_HF0_CD32PLUS)
	         ? AMIGACD_SYS_CD32PLUS : AMIGACD_SYS_CD32;
}

static char minimig_adjust = 0;

typedef struct
{
	uint32_t mode;
	uint32_t hpos;
	uint32_t vpos;
	uint32_t reserved;
} vmode_adjust_t;

vmode_adjust_t vmodes_adj[64] = {};

// What the last video mode was, and whether a stored screen position was found
// for it. Recorded here rather than re-read by the caller: minimig_adjust_vsize()
// below already asks the core for the mode, and it only asks when the mode
// changes. A menu row that issued its own UIO_GET_VMODE would put an SPI
// transaction in every OSD redraw for a value that changes maybe twice a
// session.
static uint16_t vadj_hsize = 0;
static uint16_t vadj_vsize = 0;
static char     vadj_known = 0;   // the core has reported a mode at all
static char     vadj_found = 0;   // that mode has a stored position

static const char* get_shared_vadjust_path()
{
	static char path[1024] = {};
	if (!strlen(path))
	{
		if (strlen(cfg.shared_folder))
		{
			if (cfg.shared_folder[0] == '/')
			{
				snprintf(path, sizeof(path), "%s/%s_vadjust.dat", cfg.shared_folder, user_io_get_core_name());
			}
			else
			{
				snprintf(path, sizeof(path), "%s/%s/%s_vadjust.dat", HomeDir(), cfg.shared_folder, user_io_get_core_name());
			}
		}
		else
		{
			snprintf(path, sizeof(path), "%s/shared/%s_vadjust.dat", HomeDir(), user_io_get_core_name());
		}
	}
	return path;
}

void minimig_adjust_vsize(char force)
{
	static uint16_t nres = 0;
	spi_uio_cmd_cont(UIO_GET_VMODE);
	uint16_t res = spi_w(0);
	if ((res & 0x8000) && (nres != res || force))
	{
		nres = res;
		uint16_t scr_hsize = spi_w(0);
		uint16_t scr_vsize = spi_w(0);
		DisableIO();

		printf("\033[1;37mVMODE: resolution: %u x %u, mode: %u\033[0m\n", scr_hsize, scr_vsize, res & 255);

		vadj_hsize = scr_hsize;
		vadj_vsize = scr_vsize;
		vadj_known = 1;

		static int loaded = 0;
		if (!loaded && FileExists(get_shared_vadjust_path(), 0))
		{
			FileLoad(get_shared_vadjust_path(), vmodes_adj, sizeof(vmodes_adj));
		}
		else if (!loaded)
		{
			static char name[256];
			snprintf(name, sizeof(name), "%s_vadjust.dat", user_io_get_core_name());
			FileLoadConfig(name, vmodes_adj, sizeof(vmodes_adj));
			loaded = 1;
		}

		uint32_t mode = vadjust_mode_key(scr_hsize, scr_vsize, res);
		if (mode)
		{
			for (uint i = 0; i < sizeof(vmodes_adj) / sizeof(vmodes_adj[0]); i++)
			{
				if (vmodes_adj[i].mode == mode)
				{
					vadj_found = 1;
					spi_uio_cmd_cont(UIO_SET_VPOS);
					spi_w(vmodes_adj[i].hpos >> 16);
					spi_w(vmodes_adj[i].hpos);
					spi_w(vmodes_adj[i].vpos >> 16);
					spi_w(vmodes_adj[i].vpos);
					printf("\033[1;37mVMODE: set positions: [%u-%u, %u-%u]\033[0m\n", vmodes_adj[i].hpos >> 16, (uint16_t)vmodes_adj[i].hpos, vmodes_adj[i].vpos >> 16, (uint16_t)vmodes_adj[i].vpos);
					DisableIO();
					return;
				}
			}
			printf("\033[1;37mVMODE: preset not found.\033[0m\n");
			vadj_found = 0;
			spi_uio_cmd_cont(UIO_SET_VPOS); spi_w(0); spi_w(0); spi_w(0); spi_w(0);
			DisableIO();
		}
	}
	else
	{
		DisableIO();
	}
}

static void store_vsize()
{
	minimig_adjust = 0;

	spi_uio_cmd_cont(UIO_GET_VMODE);
	uint16_t res = spi_w(0);
	uint16_t scr_hsize = spi_w(0);
	uint16_t scr_vsize = spi_w(0);
	uint16_t scr_hbl_l = spi_w(0);
	uint16_t scr_hbl_r = spi_w(0);
	uint16_t scr_vbl_t = spi_w(0);
	uint16_t scr_vbl_b = spi_w(0);
	DisableIO();

	printf("\033[1;37mVMODE: store position: [%u-%u, %u-%u]\033[0m\n", scr_hbl_l, scr_hbl_r, scr_vbl_t, scr_vbl_b);

	// The four numbers, on screen, once -- the only chance to see them.
	//
	// Adjusting happens with the OSD CLOSED: selecting the row sets menustate to
	// MENU_NONE1 before entering the mode, which is exactly why the arrows reach
	// the core instead of the menu. So there is no menu surface to put a live
	// readout on, and this toast is the one moment a value can be shown.
	//
	// Worth showing because the picture is the only other feedback there is.
	// Without it a setting that looked right cannot be written down, compared
	// against another mode, or re-entered after something resets it -- and the
	// same four numbers appear in the VMODE log line above, so one read off the
	// screen and one read out of the log are the same value.
	// Two lines, not one. OSD_PrintInfo() drops any character past INFO_MAXW,
	// which is 32, SILENTLY -- and "Stored [65535-65535, 65535-65535]" is 33.
	// Those values are not reachable in practice, but a message that truncates
	// on an input nobody has bounded is the same shape of bug as a row drawn on
	// a line the OSD never sends. Split, the widest line is 26.
	char msg[64];
	snprintf(msg, sizeof(msg), "Stored\n[%u-%u, %u-%u]",
	         scr_hbl_l, scr_hbl_r, scr_vbl_t, scr_vbl_b);
	Info(msg);

	uint32_t mode = vadjust_mode_key(scr_hsize, scr_vsize, res);
	if (mode)
	{
		int applied = 0;
		int empty = -1;
		for (int i = 0; (uint)i < sizeof(vmodes_adj) / sizeof(vmodes_adj[0]); i++)
		{
			if (vmodes_adj[i].mode == mode)
			{
				vmodes_adj[i].hpos = (scr_hbl_l << 16) | scr_hbl_r;
				vmodes_adj[i].vpos = (scr_vbl_t << 16) | scr_vbl_b;
				applied = 1;
			}
			if (empty < 0 && !vmodes_adj[i].mode) empty = i;
		}

		if (!applied && empty >= 0)
		{
			vmodes_adj[empty].mode = mode;
			vmodes_adj[empty].hpos = (scr_hbl_l << 16) | scr_hbl_r;
			vmodes_adj[empty].vpos = (scr_vbl_t << 16) | scr_vbl_b;
			applied = 1;
		}

		if (applied)
		{
			// The preset exists from now on. Set here as well as in
			// minimig_adjust_vsize(), which does not re-run on a store: without
			// this the row that reports it would keep saying "not adjusted"
			// until the next mode change, right after the user adjusted it.
			vadj_found = 1;

			static char name[256];
			snprintf(name, sizeof(name), "%s_vadjust.dat", user_io_get_core_name());
			FileSaveConfig(name, vmodes_adj, sizeof(vmodes_adj));
		}
	}
}

// 0 - disable
// 1 - enable
// 2 - cancel
void minimig_set_adjust(char n)
{
	if (minimig_adjust && !n) store_vsize();
	minimig_adjust = (n == 1) ? 1 : 0;
	if (n == 2) minimig_adjust_vsize(1);

	// The calibration pattern is on for exactly as long as the adjustment is.
	//
	// Tied to the mode rather than given a row of its own for two reasons. The
	// Video & Audio page is exactly full, and more to the point this is the only
	// time the pattern is any use: its whole job is to put a visible border on
	// the first and last active pixel so there is something to align to, and
	// the rest of the time it would just be hiding the Amiga.
	//
	// status[55] is free -- AmigaCD.sv used up to 54. rtl/video_testpattern.v
	// in the core repo is the other half.
	user_io_status_set("[55]", minimig_adjust ? 1 : 0);
}

char minimig_get_adjust()
{
	return minimig_adjust;
}

// The current video mode and whether it has a stored screen position, for the
// row that offers to adjust it.
//
// Worth saying on screen because the storage is PER MODE -- the key is
// hsize | vsize<<12 | mode<<24 -- and nothing said so. Adjust the picture on a
// CD32 boot screen and a game that runs at another resolution gets "preset not
// found" and a zeroed position, which reads as the adjustment not having been
// saved rather than as it having been saved for a different mode.
//
// Returns 1 when a position is stored for the mode now showing.
// The wording itself lives in vadjust_info.cpp, which no MiSTer header reaches
// and tests/test_vadjust_info.cpp therefore links. This wrapper is the part
// that cannot be tested: reading the statics the SPI side fills.
int minimig_vadjust_info(char *out, int outsz)
{
	if (!out || outsz <= 0) return 0;
	return vadjust_info_text(vadj_known, vadj_found, vadj_hsize, vadj_vsize,
	                         out, (size_t)outsz);
}

void minimig_ConfigVideo(unsigned char scanlines)
{
	spi_uio_cmd16(UIO_MM2_VID, (((scanlines >> 6) & 0x03) << 10) | (((scanlines >> 4) & 0x03) << 8) | (scanlines & 0x07));
}

void minimig_ConfigAudio(unsigned char audio)
{
	spi_uio_cmd8(UIO_MM2_AUD, audio);
}

void minimig_ConfigMemory(unsigned char memory)
{
	spi_uio_cmd8(UIO_MM2_MEM, memory);
}

void minimig_ConfigCPU(unsigned char cpu)
{
	spi_uio_cmd8(UIO_MM2_CPU, cpu & 0x3f);
}

void minimig_ConfigChipset(mm_configTYPE *config)
{
	unsigned char chipset = config->cdtv_drive.cfg ? (config->chipset | CONFIG_CDTV) : (config->chipset & ~CONFIG_CDTV);
	spi_uio_cmd8(UIO_MM2_CHIP, chipset & 0x3f);
}

void minimig_ConfigFloppy(unsigned char drives, unsigned char speed)
{
	// Bit 4 says the machine has NO floppy drive at all, which the drive count
	// cannot say: it is a count minus one on both sides of the link, so 0
	// already means one drive.
	//
	// Derived here rather than passed in, so every existing call site gets it
	// and none of them can forget. It is a fact about the machine, so there is
	// nothing to store, migrate or correct on load -- unlike the drive count,
	// where a stored value outlived the fix that was supposed to change it.
	unsigned char none = (is_amigacd() && amigacd_profile_floppy_none(amigacd_system()))
	                   ? 0x10 : 0x00;
	spi_uio_cmd8(UIO_MM2_FLP, none | ((drives & 0x03) << 2) | (speed & 0x03));
}

// External-drive assignment, 3 bits per unit, split across two commands because
// six bits is all the RTL decode takes per word (userio.v: floppyex01_cfg_sel
// takes IO_DIN[5:0] into floppy_ext_drive[5:0], floppyex23_cfg_sel into [11:6]).
void minimig_ConfigFloppyExt(unsigned char drive0, unsigned char drive1, unsigned char drive2, unsigned char drive3)
{
	spi_uio_cmd8(UIO_MM2_FLPEX01, ((drive1 & 0x07) << 3) | (drive0 & 0x07));
	spi_uio_cmd8(UIO_MM2_FLPEX23, ((drive3 & 0x07) << 3) | (drive2 & 0x07));
}

// One command carries both the tenant and the per-port modes, so they can never
// be set inconsistently. Bits 1:0 keep their old meaning, which is why widening
// the payload needs no config migration for the tenant itself.
void minimig_ConfigSnac(void)
{
	unsigned char p1 = minimig_config.snac[0] & 0x07;
	unsigned char p2 = minimig_config.snac[1] & 0x07;
	uint16_t param = (uint16_t)((p2 << 11) | (p1 << 8) | (minimig_config.userport & 0x03));
	spi_uio_cmd16(UIO_MM2_USRPORT, param);
}

void minimig_ConfigAutofire(unsigned char autofire, unsigned char mask)
{
	uint16_t param = mask;
	param = (param << 8) | autofire;
	spi_uio_cmd16(UIO_MM2_JOY, param);
}

void minimig_set_extcfg(unsigned int ext_cfg)
{
	minimig_config.ext_cfg = (unsigned short)ext_cfg;
	minimig_config.ext_cfg2 = (unsigned short)(ext_cfg >> 16);

	spi_uio_cmd_cont(UIO_SET_STATUS2);
	spi32_w(0);
	spi32_w(ext_cfg);
	DisableIO();
}

unsigned int minimig_get_extcfg()
{
	return (minimig_config.ext_cfg2 << 16) | minimig_config.ext_cfg;
}

#define CD32_MAIN_ROM  "CD32.rom"
#define CD32_EXT_ROM   "CD32_ext.rom"
#define CDTV_MAIN_ROM  "CDTV.rom"
#define CDTV_EXT_ROM   "CDTV_ext.rom"
#define A500_MAIN_ROM  "a500.rom"
#define A600_MAIN_ROM  "a600.rom"
#define A1200_MAIN_ROM "a1200.rom"

static const char *preset_rom_path(const char *name)
{
	static char path[1024];
	snprintf(path, sizeof(path), "%s/%s", HomeDir(), name);
	return path;
}

void minimig_cfg_set(int preset)
{
	switch (preset)
	{
	case CONFIG_PRESET_CD32:
		minimig_config.cpu = 3; // 68020, d-cache off, Turbo on (bit 5 clear)
		minimig_config.chipset = (6 << 2); // AGA
		minimig_config.memory = 3; // ChipRAM 2MB, FastRAM 0MB
		minimig_set_kickstart(preset_rom_path(CD32_MAIN_ROM));
		if(getFileSize(minimig_config.kickstart) < 1024 * 1024) minimig_set_extrom(preset_rom_path(CD32_EXT_ROM));
		minimig_config.autofire = 2 << 1; // CD32 joystick
		minimig_config.cd32_drive.cfg = 1;
		minimig_config.cdtv_drive.cfg = 0;
		minimig_config.ide_cfg = 0;
		break;

	case CONFIG_PRESET_CDTV:
		minimig_config.cpu = 0; // 68000
		minimig_config.chipset = (2 << 2); // ECS
		minimig_config.memory = 1; // ChipRAM 1MB, FastRAM 0MB
		minimig_set_kickstart(preset_rom_path(CDTV_MAIN_ROM));
		if (getFileSize(minimig_config.kickstart) < 1024 * 1024) minimig_set_extrom(preset_rom_path(CDTV_EXT_ROM));
		minimig_config.autofire = 0; // Digital joystick
		minimig_config.cd32_drive.cfg = 0;
		minimig_config.cdtv_drive.cfg = 1;
		minimig_config.ide_cfg = 0;
		break;

	case CONFIG_PRESET_A500:
		minimig_config.cpu = 0; // 68000
		minimig_config.chipset = (0 << 2); // OCS
		minimig_config.memory = 0; // ChipRAM 512KB, FastRAM 0MB
		minimig_set_kickstart(preset_rom_path(A500_MAIN_ROM));
		minimig_config.autofire = 0; // Digital joystick
		minimig_config.cd32_drive.cfg = 0;
		minimig_config.cdtv_drive.cfg = 0;
		minimig_config.ide_cfg = 0;
		break;

	case CONFIG_PRESET_A600:
		minimig_config.cpu = 0; // 68000
		minimig_config.chipset = (2 << 2); // ECS
		minimig_config.memory = 1; // ChipRAM 1MB, FastRAM 0MB
		minimig_set_kickstart(preset_rom_path(A600_MAIN_ROM));
		minimig_config.autofire = 0; // Digital joystick
		minimig_config.cd32_drive.cfg = 0;
		minimig_config.cdtv_drive.cfg = 0;
		minimig_config.ide_cfg = 0;
		break;

	case CONFIG_PRESET_A1200:
		minimig_config.cpu = 3; // 68020, d-cache off, Turbo on (bit 5 clear)
		minimig_config.chipset = (6 << 2); // AGA
		minimig_config.memory = 3; // ChipRAM 2MB, FastRAM 0MB
		minimig_set_kickstart(preset_rom_path(A1200_MAIN_ROM));
		minimig_config.autofire = 0; // Digital joystick
		minimig_config.cd32_drive.cfg = 0;
		minimig_config.cdtv_drive.cfg = 0;
		minimig_config.ide_cfg = 0;
		break;
	}
}

bool minimig_cfg_available(int preset)
{
	switch (preset)
	{
	case CONFIG_PRESET_CD32:
		if (is_minimig() == 2)
		{
			uint64_t sz = getFileSize(preset_rom_path(CD32_MAIN_ROM));
			return (sz >= 1024 * 1024) || ((sz >= 512 * 1024) && getFileSize(preset_rom_path(CD32_EXT_ROM)) >= 512 * 1024);
		}
		break;

	case CONFIG_PRESET_CDTV:
		if (is_minimig() == 2)
		{
			uint64_t sz = getFileSize(preset_rom_path(CDTV_MAIN_ROM));
			return (sz >= 1024 * 1024) || ((sz >= 256 * 1024) && getFileSize(preset_rom_path(CDTV_EXT_ROM)) >= 256 * 1024);
		}
		break;

	case CONFIG_PRESET_A500:
		return getFileSize(preset_rom_path(A500_MAIN_ROM)) >= 256 * 1024;

	case CONFIG_PRESET_A600:
		return getFileSize(preset_rom_path(A600_MAIN_ROM)) >= 512 * 1024;

	case CONFIG_PRESET_A1200:
		return getFileSize(preset_rom_path(A1200_MAIN_ROM)) >= 512 * 1024;
	}

	return 0;
}

static drive_t cd32_drive = {};
static drive_t cdtv_drive = {};

int minimig_cd_drive_open(int slot, const char *filename)
{
	static fileTYPE cd_drive_file[2] = {};

	drive_t *drv = slot ? &cdtv_drive : &cd32_drive;
	drv->cd = 1;

	const char *res = cd_drive_parse(drv, slot, filename);

	int present = res ? ide_img_mount(&cd_drive_file[slot], res, 0) : 0;
	drv->f = present ? &cd_drive_file[slot] : NULL;

	const char *full = present ? res : "";
	if (slot) cdtv_cd_set_cd_path(full);
	else akiko_cd32_set_cd_path(full);

	return present;
}

drive_t* minimig_cd_drive_get(int slot)
{
	drive_t *drive = slot ? &cdtv_drive : &cd32_drive;
	if (drive->cd && (drive->chd_f || drive->f)) return drive;
	return NULL;
}
