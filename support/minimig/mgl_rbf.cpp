#include <string.h>

#include "mgl_rbf.h"

int mgl_rewrite_rbf(char *xml, size_t xmlsz, const char *rbf)
{
	if (!xml || !rbf || !xmlsz) return 0;

	// Case-sensitive: every .mgl in the wild writes <rbf> lower case, and the
	// parser that reads these files back (mra_loader.cpp) is what decides what
	// counts -- guessing more liberally here than it does would let this edit a
	// tag the loader then ignores.
	char *open_tag = strstr(xml, "<rbf>");
	if (!open_tag) return 0;

	char *value = open_tag + 5;
	char *close_tag = strstr(value, "</rbf>");
	if (!close_tag) return 0;

	const size_t old_len = (size_t)(close_tag - value);
	const size_t new_len = strlen(rbf);

	// Already right. Reported as "no change" rather than as a failure: pressing
	// the row twice in a row is a reasonable thing to do and a no-op is the
	// honest answer.
	if (old_len == new_len && !memcmp(value, rbf, new_len)) return 0;

	const size_t tail_len = strlen(close_tag);            // includes the NUL's worth
	const size_t total = (size_t)(value - xml) + new_len + tail_len + 1;
	if (total > xmlsz) return 0;

	// memmove, not memcpy: the tail overlaps itself whenever the two lengths
	// differ, which is the usual case -- a date changes width far less often
	// than a directory does.
	memmove(value + new_len, close_tag, tail_len + 1);
	memcpy(value, rbf, new_len);
	return 1;
}
