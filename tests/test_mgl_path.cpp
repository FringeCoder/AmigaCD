// File paths written into a .mgl <file path="..."> (support/minimig/mgl_path.cpp).
//
// The bug this exists for: the file browser hands the mount code a
// root-relative path, a .mgl item path is read back as HomeDir()/<path>, and
// the two were joined without conversion -- games/Amiga/x.adf came back as
// games/Amiga/games/Amiga/x.adf. Every check resolves the written path the way
// mgl_selpath() in menu.cpp does and requires the original file back, which is
// the property that matters, rather than any one spelling of it.
//
// FALSIFIED against these mutations -- each was applied to mgl_path.cpp and
// the named check failed:
//   - returning `name` unchanged             -> "a browser path under home
//                                               resolves back", and others
//   - the home prefix matched without '/'    -> "a sibling directory is not
//                                               taken for home"
//   - the absolute fallback dropped          -> "a path outside home ..."

#include <stdio.h>
#include <string.h>

#include "../support/minimig/mgl_path.h"

static int failures = 0;

static void check(const char *name, bool ok)
{
	if (ok) printf("PASS %s\n", name);
	else { printf("FAIL %s\n", name); failures++; }
}

// mgl_selpath() in menu.cpp, with HomeDir() and getRootDir() as arguments.
static void mgl_resolve(const char *p, const char *home, const char *root, char *out, size_t sz)
{
	if (p[0] == '/') snprintf(out, sz, "%s", p);
	else if (p[0] == '.' && p[1] == '.') snprintf(out, sz, "%s/%s", root, p);
	else snprintf(out, sz, "%s/%s", home, p);
}

// What getFullPath() does with a name.
static void full_path(const char *name, const char *root, char *out, size_t sz)
{
	if (name[0] == '/') snprintf(out, sz, "%s", name);
	else snprintf(out, sz, "%s/%s", root, name);
}

static bool round_trips(const char *name, const char *home)
{
	const char *root = "/media/fat";
	char full[1024], mgl[1024], back[2100], back_full[4096];
	full_path(name, root, full, sizeof(full));
	mgl_item_path(name, home, full, mgl, sizeof(mgl));
	mgl_resolve(mgl, home, root, back, sizeof(back));
	full_path(back, root, back_full, sizeof(back_full));
	if (strcmp(full, back_full)) printf("     %s -> \"%s\" -> %s\n", name, mgl, back_full);
	return !strcmp(full, back_full);
}

int main(void)
{
	check("a browser path under home resolves back", round_trips("games/Amiga/Lemmings.dms", "games/Amiga"));
	check("a path deeper under home resolves back", round_trips("games/Amiga/WHDLoad/A/Agony.adf", "games/Amiga"));
	check("a path outside home resolves back", round_trips("games/Other/Disk.adf", "games/Amiga"));
	check("an absolute path resolves back", round_trips("/media/usb0/games/Amiga/Disk.dms", "games/Amiga"));
	check("an image inside a .zip resolves back", round_trips("games/Amiga/Pack.zip/Game.dms", "games/Amiga"));
	check("a sibling directory is not taken for home", round_trips("games/Amiga2/Disk.adf", "games/Amiga"));
	check("an absolute home (games on USB) resolves back",
	      round_trips("/media/usb0/games/Amiga/Disk.dms", "/media/usb0/games/Amiga"));

	char out[256];
	mgl_item_path("games/Amiga/Lemmings.dms", "games/Amiga", "/media/fat/games/Amiga/Lemmings.dms", out, sizeof(out));
	check("under home the path is written relative to it", !strcmp(out, "Lemmings.dms"));

	if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
	printf("\nall mgl_path checks passed\n");
	return 0;
}
