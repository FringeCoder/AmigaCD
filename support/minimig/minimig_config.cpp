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
#include "minimig_config.h"
#include "minimig_share.h"
#include "akiko_cd32.h"
#include "cdtv_cd.h"
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
	mm_floppyTYPE   floppy;
	unsigned char   disable_ar3;
	unsigned char   enable_ide;
	unsigned char   scanlines;
	unsigned char   audio;
	mm_hardfileTYPE hardfile[2];
	unsigned char   cpu;
	unsigned char   autofire;
} configTYPE_old;

mm_configTYPE minimig_config = { };
static unsigned char romkey[3072];

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
static bool LoadRomSlot(const char *path, uint8_t *dst512k)
{
	fileTYPE file = {};
	if (!FileOpen(&file, path)) {
		printf("Ext-ROM open failed: %s\n", path);
		return false;
	}
	int sz = file.size;
	if (sz == 0x80000) {
		FileReadAdv(&file, dst512k, 0x80000);
	} else if (sz == 0x40000) {
		FileReadAdv(&file, dst512k, 0x40000);
		memcpy(dst512k + 0x40000, dst512k, 0x40000);
	} else {
		printf("Unsupported ROM size %d for slot upload\n", sz);
		FileClose(&file);
		return false;
	}
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

	if (!LoadRomSlot(extrom_path, img)) return 0;
	if (!LoadRomSlot(kick_path,   img + 0x80000)) return 0;

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
	int keysize = 0;

	BootPrint("Checking for Amiga Forever key file:");
	if (FileOpen(&file, user_io_make_filepath(HomeDir(), "ROM.KEY")) || FileOpen(&file, "ROM.KEY")) {
		keysize = file.size;
		if (file.size<sizeof(romkey))
		{
			FileReadAdv(&file, romkey, keysize);
			BootPrint("Loaded Amiga Forever key file");
		}
		else
		{
			BootPrint("Amiga Forever keyfile is too large!");
		}
		FileClose(&file);
	}
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
			BootPrint("Unsupported ROM file size!");
		}
		FileClose(&file);
	}
	else {
		printf("No \"%s\" file!\n", name);
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

static char* GetConfigurationName(int num, int chk)
{
	static char name[128];
	if (num) sprintf(name, CONFIG_DIR "/%s%d.cfg", user_io_get_core_name(), num);
	else sprintf(name, CONFIG_DIR "/%s.cfg", user_io_get_core_name());

	if (chk && !S_ISREG(getFileType(name))) return 0;
	return name+strlen(CONFIG_DIR)+1;
}

int minimig_cfg_save(int num)
{
	return FileSaveConfig(GetConfigurationName(num, 0), &minimig_config, sizeof(minimig_config));
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
	char *filename = GetConfigurationName(num, 1);
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
		minimig_ConfigChipset(minimig_config.chipset);
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
	spi_uio_cmd8(UIO_MM2_HDD, (minimig_config.ide_cfg & 0x21) |
		(hdd_open(0) ? 2 : 0) |
		(hdd_open(1) ? 4 : 0) |
		(hdd_open(2) ? 8 : 0) |
		(hdd_open(3) ? 16 : 0));

	minimig_ConfigMemory(memcfg);
	minimig_ConfigCPU(minimig_config.cpu);

	minimig_ConfigChipset(minimig_config.chipset);
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
		if (!uploaded && !UploadKickstart(minimig_config.kickstart))
		{
			snprintf(minimig_config.kickstart, sizeof(minimig_config.kickstart) - 1, "%s/%s", HomeDir(), "KICK.ROM");
			if (!UploadKickstart(minimig_config.kickstart))
			{
				strcpy(minimig_config.kickstart, "KICK.ROM");
				if (!UploadKickstart(minimig_config.kickstart))
				{
					BootPrintEx("No Kickstart loaded. Press F12 for settings.");
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
	minimig_ConfigAutofire(minimig_config.autofire, 0xC);
	minimig_set_extcfg(minimig_get_extcfg() & ~1);
}

int minimig_cfg_load(int num)
{
	static const char config_id[] = "MNMGCFG0";
	int result = 0;

	const char *filename = GetConfigurationName(num, 1);

	// load configuration data
	int size;
	if(filename && (size = FileLoadConfig(filename, 0, 0))>0)
	{
		BootPrint("Opened configuration file\n");
		printf("Configuration file size: %s, %d\n", filename, size);
		if (size == sizeof(minimig_config) || size == 5152)
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
		else printf("Wrong configuration file size: %d (expected: %u)\n", size, sizeof(minimig_config));
	}
	if (!result) {
		BootPrint("Can not open configuration file!\n");
		BootPrint("Setting config defaults\n");
		// set default configuration
		memset((void*)&minimig_config, 0, sizeof(minimig_config));  // Finally found default config bug - params were reversed!
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
		BootPrintEx(">>> No config found. Using defaults. <<<");

		// AmigaCD console variant: no saved CFG -> start as a CD32 with its
		// authentic profile + PSX-style default ROMs auto-found in /media/fat/.
		if (is_amigacd())
		{
			amigacd_apply_system(0); // 0 = CD32
			BootPrintEx(">>> AmigaCD: defaulting to CD32. <<<");
		}
	}

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
}

void minimig_set_kickstart(char *name)
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

void minimig_set_extrom(char *name)
{
	const size_t cap = sizeof(minimig_config.kickstart);
	size_t kicklen = strnlen(minimig_config.kickstart, cap);
	if (kicklen + 1 >= cap) return;
	size_t off = kicklen + 1;
	size_t room = cap - off - 1;
	size_t nlen = strlen(name);
	if (nlen > room) nlen = room;
	memcpy(minimig_config.kickstart + off, name, nlen);
	memset(minimig_config.kickstart + off + nlen, 0, cap - off - nlen);
	force_reload_kickstart = 1;
}

// ---- AmigaCD console variant: per-system profiles + default ROM auto-load ----
// PSX-BIOS style: a small set of default Kickstart filenames live in the core
// boot folder (/media/fat/ = getRootDir()); the System toggle picks which pair
// to auto-load. Names follow the FS-UAE / Amiga Forever convention. See
// research/docs/amiga-console-design-2026-06-13.md and the shipped AmigaCD-ROMs
// guide.

void amigacd_default_rom_names(int cdtv, const char **main_name, const char **ext_name)
{
	if (cdtv)
	{
		if (main_name) *main_name = "kick34005.CDTV";       // Kickstart 1.3 rev 34.5 (256K)
		if (ext_name)  *ext_name  = "kick34005.CDTV.ext";   // CDTV extended ROM
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
void amigacd_set_default_roms(int cdtv)
{
	const char *mname = 0, *ename = 0;
	amigacd_default_rom_names(cdtv, &mname, &ename);

	char mpath[1024], epath[1024];
	snprintf(mpath, sizeof(mpath), "%s/%s", getRootDir(), mname);
	snprintf(epath, sizeof(epath), "%s/%s", getRootDir(), ename);

	minimig_set_kickstart(mpath);              // also clears any stale ext pairing
	if (FileExists(epath, 0)) minimig_set_extrom(epath);
}

// Apply a complete authentic CD32 or CDTV machine profile (CPU / chipset /
// memory / IDE) + default ROMs, and push the live config to the FPGA. ChipRAM
// is baked into the profile; FastRAM and D-Cache are user-adjustable in the
// AmigaCD Settings menu. Defaults match an authentic CD32: D-Cache OFF (the
// 68EC020 has no data cache) and no FastRAM. CDTV is a 68000 (cpu 0x00,
// chipset 0x20, 1M chip).
void amigacd_apply_system(int cdtv)
{
	if (cdtv)
	{
		minimig_config.cpu     = 0x00; // 68000
		minimig_config.chipset = 0x20; // OCS + CONFIG_CDTV
		minimig_config.memory  = 0x01; // 1M chip, no fast
		minimig_config.ide_cfg = 0x00; // CDTV bridge (no IDE)
	}
	else
	{
		minimig_config.cpu     = 0x03; // 68EC020, D-Cache OFF (authentic CD32 default)
		minimig_config.chipset = 0x18; // AGA + ECS (PAL)
		minimig_config.memory  = 0x03; // 2M chip, no fast (authentic CD32 default)
		minimig_config.ide_cfg = 0x01; // akiko / IDE
	}
	minimig_config.hardfile[0].cfg = 2; // single removable CD slot
	if (minimig_config.floppy.drives < 1) minimig_config.floppy.drives = 1; // single df0

	amigacd_set_default_roms(cdtv);

	minimig_ConfigCPU(minimig_config.cpu);
	minimig_ConfigChipset(minimig_config.chipset);
	minimig_ConfigFloppy(minimig_config.floppy.drives, minimig_config.floppy.speed);
	// memory is applied on reset via the UploadKickstart / ApplyConfiguration path
}

int amigacd_is_cdtv(void)
{
	return (minimig_config.chipset & CONFIG_CDTV) ? 1 : 0;
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

		uint32_t mode = scr_hsize | (scr_vsize << 12) | ((res & 0xFF) << 24);
		if (mode)
		{
			for (uint i = 0; i < sizeof(vmodes_adj) / sizeof(vmodes_adj[0]); i++)
			{
				if (vmodes_adj[i].mode == mode)
				{
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
	Info("Stored");
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

	uint32_t mode = scr_hsize | (scr_vsize << 12) | ((res & 0xFF) << 24);
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
}

char minimig_get_adjust()
{
	return minimig_adjust;
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

void minimig_ConfigChipset(unsigned char chipset)
{
	spi_uio_cmd8(UIO_MM2_CHIP, chipset & 0x3f);
}

void minimig_ConfigFloppy(unsigned char drives, unsigned char speed)
{
	spi_uio_cmd8(UIO_MM2_FLP, ((drives & 0x03) << 2) | (speed & 0x03));
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
