#include "vadjust_info.h"

#include <stdio.h>

int vadjust_info_text(int known, int found, unsigned hsize, unsigned vsize,
                      char *out, size_t outsz)
{
	if (!out || outsz == 0) return 0;

	if (!known)
	{
		snprintf(out, outsz, "(no video mode reported yet)");
		return 0;
	}

	snprintf(out, outsz, "%ux%u: %s", hsize, vsize,
	         found ? "position saved" : "not adjusted yet");
	return found ? 1 : 0;
}

uint32_t vadjust_mode_key(unsigned hsize, unsigned vsize, unsigned vmode)
{
	return (uint32_t)hsize | ((uint32_t)vsize << 12) | ((vmode & 0xFF) << 24);
}
