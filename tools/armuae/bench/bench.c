/* A Dhrystone-shaped workload for comparing 68k cores, freestanding: record
 * copies, string compare and copy, procedure calls, array and integer work.
 * Each pass adds one to the long at $000400 in chip RAM (COUNT); a harness reads it. */
typedef unsigned long u32;
#define COUNT (*(volatile u32 *)0x000400)

typedef struct rec { struct rec *next; int discr, enum_comp, int_comp; char str[31]; } rec;

static rec glob_a, glob_b;
static int arr1[50], arr2[50][50];
static char str1[31], str2[31];
static int int_glob;
static char ch1, ch2;

static void scopy(char *d, const char *s) { while ((*d++ = *s++)) ; }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a - *b; }

static __attribute__((noinline)) int func1(char a, char b) { return a == b ? 0 : 1; }
static __attribute__((noinline)) int func2(const char *a, const char *b)
{
	int i = 2; char c = 0;
	while (i <= 2) if (func1(a[i], b[i + 1]) == 0) { c = 'A'; i++; } else i++;
	if (c >= 'W' && c < 'Z') i = 7;
	if (c == 'R') return 1;
	if (scmp(a, b) > 0) { i += 7; int_glob = i; return 1; }
	return 0;
}
static __attribute__((noinline)) void proc7(int a, int b, int *c) { *c = b + a + 2; }
static __attribute__((noinline)) void proc8(int *a1, int (*a2)[50], int x, int y)
{
	int loc = x + 5;
	a1[loc] = y; a1[loc + 1] = a1[loc]; a1[loc + 30] = loc;
	for (int i = loc; i <= loc + 1; i++) a2[loc][i] = loc;
	a2[loc][loc - 1] += 1; a2[loc + 20][loc] = a1[loc];
	int_glob = 5;
}
static __attribute__((noinline)) void proc1(rec *p)
{
	rec *n = p->next;
	*n = *p;
	p->int_comp = 5; n->int_comp = p->int_comp; n->next = p->next;
	proc7(n->int_comp, 10, &n->int_comp);
	if (n->discr == 0) { n->int_comp = 6; n->enum_comp = 2; }
	else *p = *n;
}

int main(void)
{
	glob_b.next = &glob_a; glob_b.discr = 0; glob_b.enum_comp = 2; glob_b.int_comp = 40;
	scopy(glob_b.str, "DHRYSTONE PROGRAM, SOME STRING");
	scopy(str1, "DHRYSTONE PROGRAM, 1'ST STRING");
	arr2[8][7] = 10;
	for (;;) {
		int i1 = 2, i2 = 3, i3;
		scopy(str2, "DHRYSTONE PROGRAM, 2'ND STRING");
		ch1 = 'A'; ch2 = 'B';
		int b = !func2(str1, str2);
		while (i1 < i2) { i3 = 5 * i1 - i2; proc7(i1, i2, &i3); i1++; }
		proc8(arr1, arr2, i1, i3);
		proc1(&glob_b);
		for (char c = 'A'; c <= ch2; c++) if (func1(c, 'C') == 0) { ch1 = c; }
		i2 = i2 * i1; i1 = i2 / i3; i2 = 7 * (i2 - i3) - i1;
		int_glob += b + i2;
		COUNT++;
		if (COUNT == 20000) {
			/* a checksum of everything the passes touch, at a fixed point:
			 * cores that disagree on any of it disagree here */
			u32 h = 2166136261u;
			const unsigned char *p[] = { (const unsigned char *)arr1, (const unsigned char *)arr2,
			                             (const unsigned char *)&glob_a, (const unsigned char *)&glob_b,
			                             (const unsigned char *)str1, (const unsigned char *)str2 };
			const u32 n[] = { sizeof(arr1), sizeof(arr2), sizeof(glob_a), sizeof(glob_b), sizeof(str1), sizeof(str2) };
			for (int k = 0; k < 6; k++)
				for (u32 j = 0; j < n[k]; j++) {
					if (k >= 2 && k <= 3 && j < 4) continue;   /* the next pointers */
					h = (h ^ p[k][j]) * 16777619u;
				}
			h ^= (u32)int_glob * 2654435761u ^ (u32)ch1 << 8 ^ (u32)ch2;
			*(volatile u32 *)0x000408 = h;
		}
	}
}
