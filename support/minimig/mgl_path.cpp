#include "mgl_path.h"

#include <stdio.h>
#include <string.h>

void mgl_item_path(const char *name, const char *home, const char *absolute, char *out, size_t sz)
{
	size_t hl = home ? strlen(home) : 0;
	if (hl && !strncmp(name, home, hl) && name[hl] == '/') snprintf(out, sz, "%s", name + hl + 1);
	else if (name[0] == '/') snprintf(out, sz, "%s", name);
	else snprintf(out, sz, "%s", absolute ? absolute : name);
}
