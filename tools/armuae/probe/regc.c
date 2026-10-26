/* INTENA set/clear with plain volatile stores through the banked window. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
static volatile uint32_t *ctl;
static volatile uint8_t *win;
static void wr16(uint32_t a, uint16_t v) { ctl[2] = a >> 20; *(volatile uint16_t *)(win + (a & 0xFFFFF)) = __builtin_bswap16(v); }
static uint16_t rd16(uint32_t a) { ctl[2] = a >> 20; return __builtin_bswap16(*(volatile uint16_t *)(win + (a & 0xFFFFF))); }
int main(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint8_t *lw = mmap(NULL, 0x200000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xFF200000);
	ctl = (volatile uint32_t *)(lw + 0x80000);
	win = lw + 0x100000;
	printf("MAGIC %08x\n", ctl[0]);
	ctl[1] = 1 | 6;
	uint16_t seq[] = { 0x7FFF, 0x8001, 0x0001, 0x8002, 0x0002, 0xC020 };
	for (unsigned i = 0; i < sizeof(seq) / 2; i++) {
		wr16(0xDFF09A, seq[i]);
		printf("INTENA <- %04x   INTENAR %04x\n", seq[i], rd16(0xDFF01C));
	}
	wr16(0xDFF09A, 0x7FFF);
	ctl[1] = 0;
	return 0;
}
