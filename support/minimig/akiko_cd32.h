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

#endif
