// Where a CD32 disc's saved games live, and what the file is called.
//
// Extracted from akiko_cd32.cpp so it can be unit-tested. It was static in a
// 3000-line file that pulls in the whole MiSTer stack, so nothing could link
// it -- and it decides whether a player's saves are found, which is a poor
// thing to leave unchecked. rom_crypt.cpp and mgl_rbf.cpp were split out for
// the same reason and for the same kind of logic.
//
// Deliberately free of MiSTer headers: <stdint.h>, <stddef.h> and the C
// library only, so tests/ can compile it with the host compiler. The
// directory and the discnames path are parameters rather than the compiled-in
// constants for the same reason -- a test points them at a temp directory.

#ifndef CD_SAVENAME_H
#define CD_SAVENAME_H

#include <stdint.h>
#include <stddef.h>

// Physical discs arrive from cdrom_parse as "physdisc/<identity>"; everything
// else is an image path.
#define CD_PHYSDISC_PREFIX     "physdisc/"
#define CD_PHYSDISC_PREFIX_LEN (sizeof(CD_PHYSDISC_PREFIX) - 1)

// FNV-1a over the whole string. The hash is part of a filename that already
// exists on people's cards, so it is fixed by compatibility, not by taste.
uint64_t cd_savename_hash(const char *s);

// Reduce a user-supplied name to something safe to put in a filename. Spaces
// survive -- they are legal and legibility is the point -- but path
// separators, control characters and the usual shell-hostile punctuation go.
// Runs of whitespace collapse to one; leading and trailing spaces and dots are
// trimmed, so the result can never be "", "." or "..".
//
// Returns the length written. 0 means nothing survived, which callers read as
// "no usable name".
int cd_savename_sanitize(const char *src, char *out, size_t outsz);

// Look `identity` up in a "identity = friendly name" file: one per line, '#'
// or ';' comments, whitespace either side of the '=' ignored, key matched
// case-insensitively.
//
// Returns 1 and fills out[] on a hit. An absent file, an absent entry, and a
// value that sanitises to nothing all mean "no override" -- the caller falls
// back to the identity itself.
int cd_savename_lookup(const char *discnames_path, const char *identity,
                       char *out, size_t outsz);

// The save file for `cd_path`, under `dir`.
//
// A physical disc is named after itself, because its identity is already
// filesystem-safe and a re-burn of the same source then keeps its save. An
// image is named after a hash of its BASENAME, so the same CHD in a different
// folder maps to the same slot.
void cd_savename_path(const char *dir, const char *discnames_path,
                      const char *cd_path, char *out, size_t outsz);

// One-shot rename of a physical disc's save onto its current name.
//
// Two hops are possible and a disc can need either: the identity-named file
// exists when discnames.cfg gained an entry after the disc had been played,
// and the hashed file exists from before saves were named at all.
//
// Renames rather than copies -- leaving the old file behind strands a real
// save somewhere the code no longer looks.
//
// Returns 1 on a rename, 0 when there was nothing to do, and -1 when a rename
// was attempted and failed (errno is left set). `from` receives the path it
// moved, or tried to move, so the caller can log it; it is set to "" when
// there was nothing to do.
int cd_savename_migrate(const char *dir, const char *cd_path, const char *new_path,
                        char *from, size_t fromsz);

#endif
