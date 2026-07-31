#ifndef X86_CDROM_H
#define X86_CDROM_H

int cdrom_handle_cmd(ide_config *ide);
void cdrom_handle_pkt(ide_config *ide);
void cdrom_reply(ide_config *ide, uint8_t error, uint8_t asc_code = 0, uint8_t ascq_code = 0, bool unit_attention = true, uint8_t extra_status = 0);
void cdrom_read(ide_config *ide);
void cdrom_mode_select(ide_config *ide);
void ide_cdda_send_sector();

// Read one full 2352-byte raw sector (Mode 1/Mode 2 form 1, sync+header+data+ECC)
// from the CD image into `buf`. Used by the Akiko PBX sector DMA path
// (akiko_cd32.cpp). Returns 0 on success, non-zero on error/no data; on error
// the caller is expected to push zeros so the FPGA engine doesn't stall.
int cdrom_read_raw_sector(struct drive_t *drive, uint32_t lba, uint8_t *buf);

const char* cdrom_parse(uint32_t num, const char *filename);
void cdrom_close_chd(drive_t *drv);

#endif
