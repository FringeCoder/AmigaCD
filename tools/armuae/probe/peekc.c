/* Custom chip state through the flat window, with plain volatile loads. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint8_t *lw = mmap(NULL, 0x200000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xFF200000);
	volatile uint8_t *hf = mmap(NULL, 0x1000000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xC0000000);
	volatile uint32_t *ctl = (volatile uint32_t *)(lw + 0x80000);
#define R16(a) __builtin_bswap16(*(volatile uint16_t *)(hf + (a)))
#define R32(a) __builtin_bswap32(*(volatile uint32_t *)(hf + (a)))
	int n = argc > 1 ? atoi(argv[1]) : 5;
	for (int i = 0; i < n; i++) {
		printf("STATUS %08x  INTENAR %04x INTREQR %04x DMACONR %04x VHPOSR %04x  $4 %08x\n",
		       ctl[3], R16(0xDFF01C), R16(0xDFF01E), R16(0xDFF002), R16(0xDFF006), R32(4));
		usleep(300000);
	}
	return 0;
}
