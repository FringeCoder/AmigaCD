#include "cd_savename.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <sys/stat.h>

uint64_t cd_savename_hash(const char *s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	while (*s) {
		h ^= (uint64_t)(uint8_t)*s++;
		h *= 0x100000001b3ULL;
	}
	return h;
}

int cd_savename_sanitize(const char *src, char *out, size_t outsz)
{
	if (!src || !out || !outsz) return 0;

	size_t n = 0;
	int pending_space = 0;

	// The loop writes at most two bytes per pass -- a held-over space and the
	// character itself -- so the space is guarded one tighter than the loop.
	// That is what keeps the terminator below inside the buffer.
	for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < outsz; p++)
	{
		unsigned char c = *p;
		if (c == ' ' || c == '\t') { if (n) pending_space = 1; continue; }
		if (c < 0x20 || c > 0x7E) continue;
		if (strchr("/\\:*?\"<>|", (char)c)) continue;
		if (pending_space && n + 2 < outsz) out[n++] = ' ';
		pending_space = 0;
		out[n++] = (char)c;
	}

	while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
	out[n] = 0;
	return (int)n;
}

int cd_savename_lookup(const char *discnames_path, const char *identity,
                       char *out, size_t outsz)
{
	if (!discnames_path || !identity || !*identity || !out || outsz < 2) return 0;
	out[0] = 0;

	FILE *f = fopen(discnames_path, "r");
	if (!f) return 0;

	char line[512];
	int found = 0;

	while (!found && fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#' || *p == ';' || *p == '\r' || *p == '\n') continue;

		char *eq = strchr(p, '=');
		if (!eq) continue;

		char *key_end = eq;
		while (key_end > p && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
		*key_end = 0;
		if (strcasecmp(p, identity)) continue;

		char *val = eq + 1;
		while (*val == ' ' || *val == '\t') val++;
		char *nl = strpbrk(val, "\r\n");
		if (nl) *nl = 0;

		// A value that sanitises to nothing is not a hit, and the scan carries
		// on: a later line may carry a usable name for the same identity.
		found = cd_savename_sanitize(val, out, outsz) > 0;
	}

	fclose(f);
	return found;
}

void cd_savename_path(const char *dir, const char *discnames_path,
                      const char *cd_path, char *out, size_t outsz)
{
	if (!out || !outsz) return;
	out[0] = 0;
	if (!dir || !cd_path) return;

	if (!strncmp(cd_path, CD_PHYSDISC_PREFIX, CD_PHYSDISC_PREFIX_LEN))
	{
		const char *identity = cd_path + CD_PHYSDISC_PREFIX_LEN;

		// discnames.cfg wins when it has an entry, because the identity is
		// often not a name anyone would recognise. Simon the Sorcerer's
		// official pressing carries volume ID "CD32_TEST" and nothing else on
		// the disc is a title, so the only honest source is the person holding
		// it.
		char friendly[128];
		if (cd_savename_lookup(discnames_path, identity, friendly, sizeof(friendly)))
		{
			snprintf(out, outsz, "%s/%s.nvr", dir, friendly);
			return;
		}

		snprintf(out, outsz, "%s/%s.nvr", dir, identity);
		return;
	}

	const char *base = strrchr(cd_path, '/');
	base = base ? base + 1 : cd_path;
	snprintf(out, outsz, "%s/cd32-%016" PRIx64 ".nvr", dir, cd_savename_hash(base));
}

int cd_savename_migrate(const char *dir, const char *cd_path, const char *new_path,
                        char *from, size_t fromsz)
{
	if (from && fromsz) from[0] = 0;

	if (!dir || !cd_path || !new_path) return 0;
	if (strncmp(cd_path, CD_PHYSDISC_PREFIX, CD_PHYSDISC_PREFIX_LEN)) return 0;
	if (!new_path[0]) return 0;

	struct stat st;
	if (!stat(new_path, &st)) return 0;        // already on the new name

	const char *identity = cd_path + CD_PHYSDISC_PREFIX_LEN;
	char cand[256];

	// Try the nearer hop first.
	snprintf(cand, sizeof(cand), "%s/%s.nvr", dir, identity);
	if (strcmp(cand, new_path) && !stat(cand, &st))
	{
		if (from && fromsz) snprintf(from, fromsz, "%s", cand);
		return rename(cand, new_path) ? -1 : 1;
	}

	snprintf(cand, sizeof(cand), "%s/cd32-%016" PRIx64 ".nvr",
	         dir, cd_savename_hash(identity));
	if (stat(cand, &st)) return 0;             // no old save for this disc

	if (from && fromsz) snprintf(from, fromsz, "%s", cand);
	return rename(cand, new_path) ? -1 : 1;
}
