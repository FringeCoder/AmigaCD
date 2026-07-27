// CDTV native-mode bridge host driver — M2 phase-1c.
// Mirrors the CR-511 command interpreter in WinUAE cdtv.cpp:524-695 just
// far enough to boot a CDTV title past the Welcome splash. Only fires
// when the active Minimig CFG has CONFIG_CDTV set (other modes are no-ops).

#ifndef CDTV_CD_H
#define CDTV_CD_H

void cdtv_cd_init(void);
void cdtv_cd_poll(void);

// Called from ide_cdrom.cpp::cdrom_parse on every CD mount/unmount. Pass the
// CHD path on mount, empty string on unmount. The driver uses it to decide
// when to start handing out CAPACITY/TOC/IDENTIFY replies with cd_media=1.
void cdtv_cd_set_cd_path(const char *path);

#endif
