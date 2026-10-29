/* Chip RAM from the CPU, for timing armuae's paths to it (ARM-CPU plan,
 * step 5): built three times, MODE 0 reads 4 KB of chip RAM, MODE 1 writes
 * it, MODE 2 copies 4 KB from fast RAM into it, the way a renderer puts a
 * finished frame where the display can see it. All long accesses. Each pass
 * adds one to the long at $000400 in chip RAM (COUNT); a harness reads it. */
typedef unsigned long u32;
#define COUNT (*(volatile u32 *)0x000400)
#define CHIP  ((volatile u32 *)0x010000)
#define LONGS 1024
#ifndef KITER
#define KITER 16
#endif

static u32 fastbuf[LONGS];

int main(void)
{
	u32 sum = 0;
	for (int i = 0; i < LONGS; i++) fastbuf[i] = i * 0x01010101u;
	for (;;) {
#if MODE == 0
		for (int i = 0; i < LONGS; i++) sum += CHIP[i];
#elif MODE == 1
		for (int i = 0; i < LONGS; i++) CHIP[i] = sum + i;
		sum++;
#elif MODE == 2
		for (int i = 0; i < LONGS; i++) CHIP[i] = fastbuf[i];
#else
		/* MODE 3: a renderer's shape -- some arithmetic, then one long to chip
		 * RAM; the arithmetic is what a posted write lets run on */
		for (int i = 0; i < LONGS; i++) {
			u32 x = fastbuf[i];
			for (int k = 0; k < KITER; k++) x = x * 0x9E3779B1u + (x >> 7);
			CHIP[i] = x;
		}
#endif
		COUNT = COUNT + 1 + (sum & 0);
	}
	return 0;
}
