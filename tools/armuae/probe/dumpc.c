/* dumpc addr len file: copy 68k memory through the flat window, word reads */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
int main(int argc, char **argv)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint8_t *hf = mmap(NULL, 0x1000000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xC0000000);
	uint32_t a = strtoul(argv[1], 0, 0), n = strtoul(argv[2], 0, 0);
	FILE *f = fopen(argv[3], "wb");
	for (uint32_t i = 0; i < n; i += 2) { uint16_t w = *(volatile uint16_t *)(hf + a + i); fwrite(&w, 2, 1, f); }
	fclose(f);
	return 0;
}
