/* CIA-A's SDR (the last keyboard byte) through the flat window, taking the bus
 * for one read. Releasing it restarts the 68000 from its reset vector. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint8_t *lw = mmap(NULL, 0x200000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xFF200000);
	volatile uint8_t *hf = mmap(NULL, 0x1000000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xC0000000);
	volatile uint32_t *ctl = (volatile uint32_t *)(lw + 0x80000);
	if (ctl[0] != 0x41435055) { printf("no bridge: %08x\n", ctl[0]); return 1; }
	printf("STATUS %08x\n", ctl[3]);
	if (!(ctl[3] & 0x100)) { printf("not 68000\n"); return 1; }
	ctl[1] = 1;
	usleep(1000);
	uint8_t sdr = hf[0xBFEC01], tod = hf[0xBFE801], pra = hf[0xBFE001], todb = hf[0xBFD800];
	ctl[1] = 0;
	printf("SDR %02x  CIA-A TODLO %02x PRA %02x  CIA-B TODLO %02x\n", sdr, tod, pra, todb);
	return 0;
}
