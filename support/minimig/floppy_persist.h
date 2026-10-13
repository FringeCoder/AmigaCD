#ifndef __MINIMIG_FLOPPY_PERSIST_H__
#define __MINIMIG_FLOPPY_PERSIST_H__

// Which floppy images were mounted, kept beside a config, and how a floppy's
// path is written into a .mgl. Pure string work, so tests/ can build it.

#include <stddef.h>

#define FLOPPY_UNITS    4
#define FLOPPY_PATH_MAX 1024

// The sidecar text: one "dfN=<path>" line per unit that has a path. Units
// with a null or empty path are left out. Returns the length written, 0 when
// no unit has a path, or -1 when it does not fit in `sz`.
int floppy_list_format(char *out, size_t sz, const char *const paths[FLOPPY_UNITS]);

// The reverse. Units the text does not name come back empty. Unknown lines,
// out-of-range units and CR line endings are tolerated, since the file is
// plain text a user may edit. Returns how many units it named.
int floppy_list_parse(const char *text, char paths[FLOPPY_UNITS][FLOPPY_PATH_MAX]);

// A floppy path as a .mgl <file path="..."> must hold it to resolve back to
// the same file. A .mgl item path is read as HomeDir()/<path> unless it starts
// with '/' or "..", so a root-relative name -- which is what the file browser
// hands InsertFloppy -- cannot go in as it is:
//   - under `home`: written relative to it
//   - already absolute: as it is
//   - anything else: `absolute`, the caller's getFullPath(name)
void floppy_mgl_path(const char *name, const char *home, const char *absolute, char *out, size_t sz);

#endif
