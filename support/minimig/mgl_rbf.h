#ifndef MGL_RBF_H
#define MGL_RBF_H

#include <stddef.h>

// Rewriting the <rbf> line of an existing .mgl.
//
// Why this exists: cores here are staged as AmigaCD_<date>.rbf, so the name an
// .mgl points at changes every deploy and every file naming the old one stops
// working. The OSD can rewrite it -- it knows which core is running -- but the
// obvious way to do that, writing the file out again, throws away whatever the
// user added to it. A <reset/>, a second <file> for a save volume, a comment:
// all gone, silently, for pressing a row that was supposed to fix a path.
//
// So the existing file is edited rather than replaced, and only between the
// <rbf> tags. Everything else in the file is returned byte for byte.
//
// Kept free of MiSTer headers so it builds with the host compiler.

// Replace the text between the first <rbf> and its </rbf> with `rbf`.
//
//   xml     the file's contents, edited in place, NUL terminated
//   xmlsz   the capacity of that buffer, including room for the NUL
//   rbf     the replacement, without the <rbf> tags
//
// Returns 1 when the file was changed, 0 when it was left alone -- no <rbf>
// element, a malformed one, a replacement that would not fit, or a file that
// already names exactly this core. The last of those is not a failure: pressing
// the row twice is a no-op and should report as one.
int mgl_rewrite_rbf(char *xml, size_t xmlsz, const char *rbf);

#endif
