// Where a CD32 disc's saved games live.
//
// This is the code that decides whether a player's saves are FOUND. Get the
// name wrong and nothing errors -- the machine comes up with a blank NVRAM and
// the saves are still on the card, under a name nothing looks for. So the
// checks here are mostly about identity: two things that should map to one
// file, and two that must not map to the same one.
//
// The hash is pinned to literal values on purpose. It is part of filenames
// that already exist on people's cards, so it is fixed by compatibility rather
// than by taste, and a "better" hash would silently orphan every existing
// save. A check that recomputed it from the implementation would not notice.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../support/minimig/cd_savename.h"

static int failures = 0;

static void check(const char *name, int got, int want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %d, want %d\n", name, got, want); failures++; }
}

static void check_str(const char *name, const char *got, const char *want)
{
	if (!strcmp(got, want)) printf("PASS %s\n", name);
	else
	{
		printf("FAIL %s:\n  got:  %s\n  want: %s\n", name, got, want);
		failures++;
	}
}

static char tmpdir[256];

static void write_file(const char *path, const char *body)
{
	FILE *f = fopen(path, "wb");
	if (!f) { printf("FAIL cannot write %s\n", path); failures++; return; }
	fputs(body, f);
	fclose(f);
}

static int exists(const char *path)
{
	struct stat st;
	return !stat(path, &st);
}

int main(void)
{
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/cd_savename_test_%d", (int)getpid());
	if (mkdir(tmpdir, 0700)) { printf("FAIL cannot mkdir %s\n", tmpdir); return 1; }

	char names[512];
	snprintf(names, sizeof(names), "%s/discnames.cfg", tmpdir);

	char out[512];

	// --- the hash, pinned -----------------------------------------------
	// FNV-1a 64 over the empty string is the offset basis itself.
	check("hash of empty string is the FNV offset basis",
	      cd_savename_hash("") == 0xcbf29ce484222325ULL, 1);
	check("hash of \"a\" is the FNV-1a reference value",
	      cd_savename_hash("a") == 0xaf63dc4c8601ec8cULL, 1);
	check("hash is case sensitive",
	      cd_savename_hash("Game.chd") != cd_savename_hash("game.chd"), 1);

	// --- image paths ----------------------------------------------------
	// The basename is hashed, not the path, so moving a CHD between folders
	// keeps its saves. This is the property players actually rely on.
	char a[512], b[512];
	cd_savename_path(tmpdir, names, "/media/fat/games/Amiga/Banshee.chd", a, sizeof(a));
	cd_savename_path(tmpdir, names, "/media/usb0/elsewhere/Banshee.chd", b, sizeof(b));
	check_str("same image in a different folder is the same save", a, b);

	cd_savename_path(tmpdir, names, "/x/Banshee.chd", a, sizeof(a));
	cd_savename_path(tmpdir, names, "/x/Microcosm.chd", b, sizeof(b));
	check("two different images are two different saves", strcmp(a, b) != 0, 1);

	cd_savename_path(tmpdir, names, "Banshee.chd", a, sizeof(a));
	check("a bare filename with no slash still works",
	      strstr(a, "/cd32-") != NULL, 1);

	// --- physical discs -------------------------------------------------
	// Named after the disc, not hashed, so the saves directory is readable and
	// a re-burn of the same source keeps its save.
	cd_savename_path(tmpdir, names, "physdisc/ISSUE7", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/ISSUE7.nvr", tmpdir);
	check_str("a physical disc is named after itself", out, a);

	// An image whose FILENAME starts with the prefix must not be mistaken for
	// one: the prefix is a marker cdrom_parse puts on, not a path component
	// anyone types.
	cd_savename_path(tmpdir, names, "/games/physdisc/ISSUE7.chd", out, sizeof(out));
	check("an image in a folder called physdisc is still hashed",
	      strstr(out, "/cd32-") != NULL, 1);

	// --- discnames.cfg --------------------------------------------------
	write_file(names,
	           "# a comment\n"
	           "; another\n"
	           "\n"
	           "CD32_TEST = Simon the Sorcerer\n"
	           "  SPACED  =  Trailing And Leading  \n"
	           "EMPTYVAL =\n"
	           "NASTY = ../../etc/passwd\n"
	           "DOTS = ...\n"
	           "EQVAL = Pinball = Fantasies\n"
	           "no equals sign here\n");

	cd_savename_path(tmpdir, names, "physdisc/CD32_TEST", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/Simon the Sorcerer.nvr", tmpdir);
	check_str("discnames.cfg overrides an unhelpful identity", out, a);

	cd_savename_path(tmpdir, names, "physdisc/cd32_test", out, sizeof(out));
	check_str("the key is matched case-insensitively", out, a);

	// The key ends at the FIRST '=', so a value may contain one. Without this
	// case, splitting on the last '=' instead passes every other check here:
	// the key becomes "EQVAL = Pinball", matches nothing, and the identity
	// fallback quietly looks like correct behaviour.
	cd_savename_path(tmpdir, names, "physdisc/EQVAL", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/Pinball = Fantasies.nvr", tmpdir);
	check_str("the key ends at the first '=', so a value may contain one", out, a);

	cd_savename_path(tmpdir, names, "physdisc/SPACED", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/Trailing And Leading.nvr", tmpdir);
	check_str("whitespace around the value is trimmed", out, a);

	// A value that sanitises to nothing is not an override -- falling back to
	// the identity is the difference between a save in the wrong place and a
	// save in no place.
	cd_savename_path(tmpdir, names, "physdisc/EMPTYVAL", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/EMPTYVAL.nvr", tmpdir);
	check_str("an empty value falls back to the identity", out, a);

	cd_savename_path(tmpdir, names, "physdisc/DOTS", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/DOTS.nvr", tmpdir);
	check_str("a value of only dots falls back to the identity", out, a);

	// The one that matters for safety: a name from a user-edited file must not
	// be able to walk out of the saves directory.
	//
	// The property is containment, not the absence of dots. "../../etc/passwd"
	// sanitises to "....etcpasswd" -- every separator is gone, so what is left
	// is one harmless filename that happens to begin with dots. An earlier
	// version of this check looked for the substring ".." and failed on that,
	// which was the check being wrong rather than the code. So: the result must
	// start with the directory and the rest must be a single component.
	cd_savename_path(tmpdir, names, "physdisc/NASTY", out, sizeof(out));
	{
		char prefix[512];
		snprintf(prefix, sizeof(prefix), "%s/", tmpdir);
		int in_dir = !strncmp(out, prefix, strlen(prefix));
		const char *leaf = in_dir ? out + strlen(prefix) : out;
		check("a value with slashes stays inside the saves directory",
		      in_dir && strchr(leaf, '/') == NULL, 1);
		check("and it is not a traversal component",
		      strcmp(leaf, "..") && strcmp(leaf, ".") && strcmp(leaf, "../"), 1);
	}

	// Leading dots survive, which makes such a save a hidden file on Linux.
	// Pinned rather than fixed: these names are already on people's cards, and
	// trimming the front would rename an existing save to something nothing
	// looks for -- the exact failure this whole file is about.
	check("leading dots are kept, so the name is stable",
	      cd_savename_sanitize(".Simon", out, sizeof(out)) == 6 && !strcmp(out, ".Simon"), 1);

	cd_savename_path(tmpdir, names, "physdisc/ABSENT", out, sizeof(out));
	snprintf(a, sizeof(a), "%s/ABSENT.nvr", tmpdir);
	check_str("an identity with no entry falls back to itself", out, a);

	// A missing file is not an error, it is "no overrides".
	check("a missing discnames.cfg is not a hit",
	      cd_savename_lookup("/nonexistent/discnames.cfg", "CD32_TEST", out, sizeof(out)), 0);

	// --- sanitising -----------------------------------------------------
	check("sanitize collapses runs of whitespace",
	      cd_savename_sanitize("A   B", out, sizeof(out)) > 0 && !strcmp(out, "A B"), 1);
	check("sanitize drops path separators",
	      cd_savename_sanitize("a/b\\c", out, sizeof(out)) > 0 && !strcmp(out, "abc"), 1);
	check("sanitize refuses to produce \"..\"",
	      cd_savename_sanitize("..", out, sizeof(out)), 0);
	check("sanitize drops control characters",
	      cd_savename_sanitize("A\x01\x02" "B", out, sizeof(out)) > 0 && !strcmp(out, "AB"), 1);

	// The two-bytes-per-pass write is the only place this could overflow, so
	// it is checked against a buffer sized to land exactly on the boundary.
	{
		char small[6];
		memset(small, 0x7f, sizeof(small));
		int n = cd_savename_sanitize("A B C D E F", small, 5);
		check("sanitize respects a tight buffer", n < 5, 1);
		check("sanitize terminates within the buffer", small[4] == 0 || n < 4, 1);
		check("sanitize does not write past the size it was given",
		      (unsigned char)small[5] == 0x7f, 1);
	}

	// --- the one-shot migration -----------------------------------------
	// Renaming a save is the one thing here that touches a file a player cares
	// about, and getting it wrong strands the save rather than losing it
	// loudly.
	char from[512], target[512], old_hashed[512], old_named[512];

	snprintf(target, sizeof(target), "%s/Simon the Sorcerer.nvr", tmpdir);
	snprintf(old_named, sizeof(old_named), "%s/CD32_TEST.nvr", tmpdir);
	snprintf(old_hashed, sizeof(old_hashed), "%s/cd32-%016llx.nvr", tmpdir,
	         (unsigned long long)cd_savename_hash("CD32_TEST"));

	// Nothing on disk: nothing to do.
	check("migrate with no old save does nothing",
	      cd_savename_migrate(tmpdir, "physdisc/CD32_TEST", target, from, sizeof(from)), 0);

	// The older of the two hops.
	write_file(old_hashed, "hashed");
	check("migrate moves a hashed save onto the new name",
	      cd_savename_migrate(tmpdir, "physdisc/CD32_TEST", target, from, sizeof(from)), 1);
	check_str("migrate reports which file it moved", from, old_hashed);
	check("the hashed file is gone, not copied", exists(old_hashed), 0);
	check("the save is on the new name", exists(target), 1);

	// The nearer hop wins when both exist.
	unlink(target);
	write_file(old_named, "named");
	write_file(old_hashed, "hashed");
	check("migrate prefers the identity-named file",
	      cd_savename_migrate(tmpdir, "physdisc/CD32_TEST", target, from, sizeof(from)), 1);
	check_str("migrate reports the nearer hop", from, old_named);
	check("the hashed file is left alone when the nearer hop won",
	      exists(old_hashed), 1);

	// An existing save on the new name is never overwritten.
	check("migrate does not touch a save already on the new name",
	      cd_savename_migrate(tmpdir, "physdisc/CD32_TEST", target, from, sizeof(from)), 0);
	check_str("migrate reports nothing when it did nothing", from, "");
	check("the hashed file survives that no-op", exists(old_hashed), 1);

	// Images never migrate: only physical discs were ever renamed.
	unlink(target);
	check("migrate ignores an image path",
	      cd_savename_migrate(tmpdir, "/games/Banshee.chd", target, from, sizeof(from)), 0);

	// Clean up.
	unlink(old_hashed); unlink(old_named); unlink(target); unlink(names);
	rmdir(tmpdir);

	if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
	printf("\nall cd_savename checks passed\n");
	return 0;
}
