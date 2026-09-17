// Rewriting the <rbf> line of an existing .mgl.
//
// The whole reason this is surgery rather than a rewrite is that the file may
// carry things the OSD did not put there -- a <reset/>, a second <file> for a
// save volume, a comment. Losing those would be silent, and would happen on a
// keypress whose stated purpose is to fix a path. So the check that matters is
// not "does it change the rbf" but "does it change ONLY the rbf".
//
// ONE MUTATION THIS DOES NOT CATCH, recorded so the coverage is not mistaken
// for complete: changing the implementation's memmove to memcpy leaves every
// check below passing. The two regions overlap whenever the old and new names
// differ in length, which makes memcpy undefined behaviour -- but on this host
// glibc happens to produce the right answer anyway, and a test cannot depend on
// undefined behaviour misbehaving. The memmove is correct by the standard and
// unverified by this file; the ARM build is where it would differ, if anywhere.

#include <stdio.h>
#include <string.h>

#include "../support/minimig/mgl_rbf.h"

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

int main(void)
{
	char buf[1024];

	// --- the point of the whole exercise --------------------------------
	// Everything the user added survives, and the rbf is the only thing that
	// moves. The tail here is deliberately awkward: a reset element, a second
	// file, and a comment, all after the rbf.
	const char *rich =
		"<mistergamedescription>\n"
		"\t<rbf>_Test/AmigaCD_20260916</rbf>\n"
		"\t<file delay=\"2\" type=\"f\" index=\"4\" path=\"Games/X.chd\"/>\n"
		"\t<file delay=\"1\" type=\"f\" index=\"5\" path=\"Saves.hdf\"/>\n"
		"\t<reset delay=\"1\" hold=\"1\"/>\n"
		"\t<!-- hand edited, do not lose me -->\n"
		"</mistergamedescription>\n";

	snprintf(buf, sizeof(buf), "%s", rich);
	check("a changed core reports a change",
	      mgl_rewrite_rbf(buf, sizeof(buf), "_Test/AmigaCD_20261001"), 1);
	check_str("only the rbf moved; reset, second file and comment survive", buf,
		"<mistergamedescription>\n"
		"\t<rbf>_Test/AmigaCD_20261001</rbf>\n"
		"\t<file delay=\"2\" type=\"f\" index=\"4\" path=\"Games/X.chd\"/>\n"
		"\t<file delay=\"1\" type=\"f\" index=\"5\" path=\"Saves.hdf\"/>\n"
		"\t<reset delay=\"1\" hold=\"1\"/>\n"
		"\t<!-- hand edited, do not lose me -->\n"
		"</mistergamedescription>\n");

	// --- lengths that differ in both directions -------------------------
	// The tail overlaps itself whenever the lengths differ, which is why the
	// implementation uses memmove. A shorter replacement and a much longer one
	// exercise both directions of the slide.
	snprintf(buf, sizeof(buf), "%s", "<rbf>_Test/AmigaCD_20260916</rbf>TAIL");
	check("a shorter core still reports a change",
	      mgl_rewrite_rbf(buf, sizeof(buf), "a"), 1);
	check_str("the tail slides left intact", buf, "<rbf>a</rbf>TAIL");

	snprintf(buf, sizeof(buf), "%s", "<rbf>a</rbf>TAIL");
	check("a longer core reports a change",
	      mgl_rewrite_rbf(buf, sizeof(buf), "_Computer/A_Very_Long_Core_Name_20261001"), 1);
	check_str("the tail slides right intact", buf,
	          "<rbf>_Computer/A_Very_Long_Core_Name_20261001</rbf>TAIL");

	// An empty element, which is what a hand-written stub looks like.
	snprintf(buf, sizeof(buf), "%s", "<rbf></rbf>TAIL");
	check("an empty rbf is filled", mgl_rewrite_rbf(buf, sizeof(buf), "_Test/X"), 1);
	check_str("and the tail is still there", buf, "<rbf>_Test/X</rbf>TAIL");

	// --- no change --------------------------------------------------------
	// Pressing the row twice. Reported as 0, and the file must be untouched.
	snprintf(buf, sizeof(buf), "%s", "<rbf>_Test/Same</rbf>TAIL");
	check("the same core reports no change",
	      mgl_rewrite_rbf(buf, sizeof(buf), "_Test/Same"), 0);
	check_str("and the file is byte for byte what it was", buf, "<rbf>_Test/Same</rbf>TAIL");

	// A prefix is not a match: "_Test/Sam" must not be mistaken for "_Test/Same".
	snprintf(buf, sizeof(buf), "%s", "<rbf>_Test/Same</rbf>TAIL");
	check("a prefix of the current core is a change",
	      mgl_rewrite_rbf(buf, sizeof(buf), "_Test/Sam"), 1);
	check_str("and is written", buf, "<rbf>_Test/Sam</rbf>TAIL");

	// --- refusals ---------------------------------------------------------
	// A file with no rbf element at all, which is not an .mgl this core wrote.
	// Left alone: editing a file we do not understand is worse than declining.
	snprintf(buf, sizeof(buf), "%s", "<mistergamedescription>\n</mistergamedescription>\n");
	check("no rbf element is a refusal", mgl_rewrite_rbf(buf, sizeof(buf), "_Test/X"), 0);
	check_str("and the file is untouched", buf,
	          "<mistergamedescription>\n</mistergamedescription>\n");

	// An opening tag with no closing one. Truncated, or being written by
	// something else right now.
	snprintf(buf, sizeof(buf), "%s", "<rbf>_Test/Unclosed");
	check("an unclosed rbf is a refusal", mgl_rewrite_rbf(buf, sizeof(buf), "_Test/X"), 0);
	check_str("and that file is untouched too", buf, "<rbf>_Test/Unclosed");

	// Case matters, because it matters to the loader that reads these back.
	snprintf(buf, sizeof(buf), "%s", "<RBF>_Test/Upper</RBF>TAIL");
	check("an upper case tag is not ours", mgl_rewrite_rbf(buf, sizeof(buf), "_Test/X"), 0);

	// Would not fit. The buffer is exactly the string plus its NUL, so any
	// longer replacement has to be refused rather than truncated -- a truncated
	// core name is a path that silently does not exist.
	{
		char tight[] = "<rbf>ab</rbf>";
		check("a replacement that would not fit is refused",
		      mgl_rewrite_rbf(tight, sizeof(tight), "abcdef"), 0);
		check_str("and leaves the buffer as it was", tight, "<rbf>ab</rbf>");

		// One that fits exactly must still work: the boundary is <=, not <.
		char exact[] = "<rbf>ab</rbf>";
		check("a replacement that fits exactly is written",
		      mgl_rewrite_rbf(exact, sizeof(exact), "xy"), 1);
		check_str("with the right contents", exact, "<rbf>xy</rbf>");
	}

	// Null and empty guards.
	check("a null buffer is refused", mgl_rewrite_rbf(NULL, 16, "x"), 0);
	snprintf(buf, sizeof(buf), "%s", "<rbf>a</rbf>");
	check("a null replacement is refused", mgl_rewrite_rbf(buf, sizeof(buf), NULL), 0);
	check("a zero capacity is refused", mgl_rewrite_rbf(buf, 0, "x"), 0);

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
