// Floppies remembered beside a config, and floppy paths written into a .mgl
// (support/minimig/floppy_persist.cpp).
//
// The .mgl half is the one that was broken: the file browser hands
// InsertFloppy a root-relative path, a .mgl item path is read back as
// HomeDir()/<path>, and the two were joined without conversion. The checks
// below resolve every written path the way mgl_selpath() in menu.cpp does,
// and require the original image back -- the property that matters, rather
// than any particular spelling of it.
//
// FALSIFIED against these mutations -- each was applied to floppy_persist.cpp
// and the named check failed:
//   - floppy_mgl_path returning `name` unchanged  -> "a browser path under home
//                                                    resolves back"
//   - the home prefix matched without the '/'     -> "a sibling directory is not
//     check (games/Amiga2 taken as under games/Amiga)  taken for home"
//   - the absolute fallback dropped               -> "a path outside home ..."
//   - CR not trimmed in parse                     -> "CRLF ..."
//   - overflow not detected in format             -> "a list too long ..."

#include <stdio.h>
#include <string.h>

#include "../support/minimig/floppy_persist.h"

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

// What getFullPath() does for a root-relative name.
static void full_path(const char *name, const char *root, char *out, size_t sz)
{
	if (name[0] == '/') snprintf(out, sz, "%s", name);
	else snprintf(out, sz, "%s/%s", root, name);
}

// Writes `name` into a .mgl and reads it back; true when the same image
// comes back, compared as full paths.
static bool round_trips(const char *name, const char *home)
{
	const char *root = "/media/fat";
	char full[1024], mgl[1024], back[2100], back_full[4096];
	full_path(name, root, full, sizeof(full));
	floppy_mgl_path(name, home, full, mgl, sizeof(mgl));
	mgl_resolve(mgl, home, root, back, sizeof(back));
	full_path(back, root, back_full, sizeof(back_full));
	if (strcmp(full, back_full)) printf("     %s -> \"%s\" -> %s\n", name, mgl, back_full);
	return !strcmp(full, back_full);
}

static void test_mgl_paths()
{
	check("a browser path under home resolves back", round_trips("games/Amiga/Lemmings.dms", "games/Amiga"));
	check("a path deeper under home resolves back", round_trips("games/Amiga/WHDLoad/A/Agony.adf", "games/Amiga"));
	check("a path outside home resolves back", round_trips("games/Other/Disk.adf", "games/Amiga"));
	check("an absolute path resolves back", round_trips("/media/usb0/games/Amiga/Disk.dms", "games/Amiga"));
	check("a .dms inside a .zip resolves back", round_trips("games/Amiga/Pack.zip/Game.dms", "games/Amiga"));
	check("a sibling directory is not taken for home", round_trips("games/Amiga2/Disk.adf", "games/Amiga"));
	check("an absolute home (games on USB) resolves back",
	      round_trips("/media/usb0/games/Amiga/Disk.dms", "/media/usb0/games/Amiga"));

	// And the form under home is the short one, which is what a person
	// editing the file expects and what older hand-written files use.
	char out[256];
	floppy_mgl_path("games/Amiga/Lemmings.dms", "games/Amiga", "/media/fat/games/Amiga/Lemmings.dms", out, sizeof(out));
	check("under home the path is written relative to it", !strcmp(out, "Lemmings.dms"));
}

static void test_sidecar()
{
	const char *paths[FLOPPY_UNITS] = { "games/Amiga/A.dms", 0, "/media/usb0/C.adf", "" };
	char text[4096];
	int len = floppy_list_format(text, sizeof(text), paths);
	check("format writes one line per mounted unit",
	      len > 0 && !strcmp(text, "df0=games/Amiga/A.dms\ndf2=/media/usb0/C.adf\n"));

	char back[FLOPPY_UNITS][FLOPPY_PATH_MAX];
	int named = floppy_list_parse(text, back);
	check("parse names the units format wrote", named == 2);
	check("parse gives each path back",
	      !strcmp(back[0], paths[0]) && !back[1][0] && !strcmp(back[2], paths[2]) && !back[3][0]);

	const char *none[FLOPPY_UNITS] = {};
	check("nothing mounted formats to nothing", floppy_list_format(text, sizeof(text), none) == 0 && !text[0]);

	char small[16];
	check("a list too long for the buffer is refused, not cut",
	      floppy_list_format(small, sizeof(small), paths) == -1 && !small[0]);

	named = floppy_list_parse("df1=games/Amiga/B.dms\r\n", back);
	check("CRLF line endings are tolerated", named == 1 && !strcmp(back[1], "games/Amiga/B.dms"));

	named = floppy_list_parse("# a comment\ndf9=x\nhello\ndf0=\ndf1=Disk With Spaces.adf", back);
	check("junk, bad units and empty paths are skipped; no final newline is fine",
	      named == 1 && !back[0][0] && !strcmp(back[1], "Disk With Spaces.adf"));

	named = floppy_list_parse("df0=first.adf\ndf0=second.adf\n", back);
	check("a unit named twice keeps the last and counts once", named == 1 && !strcmp(back[0], "second.adf"));
}

int main(void)
{
	test_mgl_paths();
	test_sidecar();

	if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
	printf("\nall floppy_persist checks passed\n");
	return 0;
}
