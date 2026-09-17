#ifndef AKIKO_CD32_H
#define AKIKO_CD32_H

void akiko_cd32_init(void);
void akiko_cd32_poll(void);

// Phase 32.5.1 — per-game NVRAM. Called from ide_cdrom.cpp::cdrom_parse
// whenever a CD image is mounted or unmounted on a Minimig CD slot. `path`
// is the full filesystem path of the CHD/CUE/ISO; pass an empty string on
// unmount. The basename is hashed (FNV-1a 64-bit) into a per-game save
// filename `<saves>/cd32-XXXXXXXXXXXXXXXX.nvr`. On a CD swap any pending
// dirty data is flushed to the OLD save file before the new one is loaded.
// Safe to call from non-Minimig cores: behaves as a no-op until the akiko
// poll runs (Minimig-only — see user_io.cpp).
void akiko_cd32_set_cd_path(const char *path);

// ---------------------------------------------------------------------------
// Per-game NVRAM, for the OSD.
//
// akiko_cd32_nvram_info returns 1 when a save file is in use and 0 when there
// is none -- no disc, or a machine with no CD drive. On 1:
//
//   path    full path of the save file this disc maps to
//   exists  whether that file is on disk yet. A disc that has never been saved
//           has a path and no file, which is not an error and is worth saying
//           rather than showing a filename that is not there.
//   failed  the last load of this file failed its verify pass, so writes to it
//           are being refused (see cd_save_load_failed). Nothing else surfaces
//           that, and a player whose saves have silently stopped persisting has
//           no way to find out.
//
// akiko_cd32_nvram_clear deletes the save and zeroes the mirror in FPGA BRAM.
// Returns 1 on success, 0 when there was nothing to clear or the unlink failed.
// Zeroing matters as much as the delete: without it the running machine still
// holds the old contents and the next dirty write puts them straight back.
int akiko_cd32_nvram_info(char *path, int pathsz, int *exists, int *failed);
int akiko_cd32_nvram_clear(void);

// ---------------------------------------------------------------------------
// What is in the drive, for the OSD.
//
// Returns 0 when no drive has media, 1 otherwise. On success:
//
//   label   the ISO9660 volume identifier, trimmed, or "" when there is none
//           to read. Empty is a normal answer, not a failure -- a CD32
//           pressing is not obliged to carry one, and several well known ones
//           do not (Simon the Sorcerer ships as "CD32_TEST").
//   tracks  tracks on the disc, lead-out excluded
//   audio   how many of those are CDDA
//
// The counts are the point. Several CD32 titles circulate both as a data-only
// rip and as the full disc with its music, they mount identically, and the
// difference is only audible once you are in the game. Any of the three
// pointers may be NULL.
int akiko_cd32_disc_info(char *label, int labelsz, int *tracks, int *audio);

// ---------------------------------------------------------------------------
// Save state, host side.
//
// Akiko's registers live in the FPGA and travel in the core payload. The DRIVE
// does not: where the head is, what was playing, whether the BIOS has finished
// its auto-init, all of that is host state in akiko_cd32.cpp. Without it a
// restore puts the CD32's registers back and leaves userspace serving sectors
// from wherever the drive has since wandered to, which for a title streaming
// from CD means the data it asked for and the data it gets stop matching.
//
// The block is versioned and self-describing. akiko_cd32_state_load refuses a
// version it does not know rather than reinterpreting the bytes, because the
// failure mode of guessing here is a drive seeking to a plausible wrong place.
int  akiko_cd32_state_size(void);
void akiko_cd32_state_save(void *buf);
bool akiko_cd32_state_load(const void *buf, int len);

#endif
