#include "osd_rowguard.h"

int osd_rowguard_warn(int writes, int selected, unsigned menusub, uint64_t menumask)
{
	if (writes <= 0) return 0;
	if (menusub >= 64) return 0;

	// A zero menumask needs no test of its own: no bit is set, so the cursor is
	// never on one. An explicit `if (!menumask) return 0;` was written here first
	// and removed -- dropping it changed no behaviour any check could see, which
	// makes it a line that reads as load-bearing and is not.
	if (!(menumask & ((uint64_t)1 << menusub))) return 0;

	return selected != 1;
}
