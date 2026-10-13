#include "floppy_persist.h"

#include <stdio.h>
#include <string.h>

int floppy_list_format(char *out, size_t sz, const char *const paths[FLOPPY_UNITS])
{
	size_t len = 0;
	if (sz) out[0] = 0;
	for (int i = 0; i < FLOPPY_UNITS; i++)
	{
		if (!paths[i] || !paths[i][0]) continue;
		int n = snprintf(out + len, sz - len, "df%d=%s\n", i, paths[i]);
		if (n < 0 || (size_t)n >= sz - len) { if (sz) out[0] = 0; return -1; }
		len += n;
	}
	return (int)len;
}

int floppy_list_parse(const char *text, char paths[FLOPPY_UNITS][FLOPPY_PATH_MAX])
{
	for (int i = 0; i < FLOPPY_UNITS; i++) paths[i][0] = 0;
	int named = 0;
	const char *p = text;
	while (p && *p)
	{
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;

		if (len > 4 && p[0] == 'd' && p[1] == 'f' && p[2] >= '0' && p[2] < '0' + FLOPPY_UNITS && p[3] == '=')
		{
			int unit = p[2] - '0';
			size_t plen = len - 4;
			if (plen < FLOPPY_PATH_MAX)
			{
				if (!paths[unit][0]) named++;
				memcpy(paths[unit], p + 4, plen);
				paths[unit][plen] = 0;
			}
		}
		p = eol ? eol + 1 : 0;
	}
	return named;
}

void floppy_mgl_path(const char *name, const char *home, const char *absolute, char *out, size_t sz)
{
	size_t hl = home ? strlen(home) : 0;
	if (hl && !strncmp(name, home, hl) && name[hl] == '/') snprintf(out, sz, "%s", name + hl + 1);
	else if (name[0] == '/') snprintf(out, sz, "%s", name);
	else snprintf(out, sz, "%s", absolute ? absolute : name);
}
