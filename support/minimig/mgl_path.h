#ifndef __MINIMIG_MGL_PATH_H__
#define __MINIMIG_MGL_PATH_H__

// How a file's path is written into a .mgl <file path="...">. Pure string
// work, so tests/ can build it.

#include <stddef.h>

// A .mgl item path is read as HomeDir()/<path> unless it starts with '/' or
// "..", so a root-relative name -- which is what the file browser hands the
// mount code -- cannot go in as it is:
//   - under `home`: written relative to it
//   - already absolute: as it is
//   - anything else: `absolute`, the caller's getFullPath(name)
void mgl_item_path(const char *name, const char *home, const char *absolute, char *out, size_t sz);

#endif
