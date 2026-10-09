#ifndef MGL_CFG_H
#define MGL_CFG_H

// Settings carried inside a .mgl.
//
// A .mgl written by the OSD holds the whole machine configuration, so that
// launching it gives the machine it was written from -- system, RAM, ROMs,
// the disc in the drive at power-on, every OSD setting -- whatever the
// startup config on the card happens to say. The block is ordinary XML that
// MiSTer's own .mgl scanner skips (it only acts on <file> and <reset>):
//
//     <amigacd_config version="1">
//         <setting name="cpu" value="3"/>
//         <setting name="kickstart" value="games/AmigaCD32/CD32.rom"/>
//         ...
//     </amigacd_config>
//
// One element per setting so a file can be read, diffed and edited by hand. A
// setting the file does not name keeps the value from the startup config; a
// name this build does not know is skipped and counted.
//
// This module knows nothing about the Minimig config struct: it formats and
// applies a table of fields over a byte buffer. The table lives beside the
// struct in minimig_config.cpp, where offsetof() and a compile-time coverage
// check keep it complete. Keeping this side free of MiSTer headers is what
// lets tests/test_mgl_cfg.cpp exercise it on a host.

#include <stddef.h>
#include <stdint.h>

#define MGLCFG_VERSION 1

enum mglcfg_type
{
	MGLCFG_U8,
	MGLCFG_U16,
	MGLCFG_U32,
	MGLCFG_STR,     // NUL-terminated within `size` bytes; written up to the NUL
};

struct mglcfg_field
{
	const char *name;
	size_t      off;
	size_t      size;
	int         type;
};

// An extra string setting outside the table (the ext ROM, which shares the
// kickstart buffer). Formatted after the table fields.
struct mglcfg_extra
{
	const char *name;
	const char *value;
};

// Append the whole <amigacd_config> block, each line prefixed by `indent`.
// Returns the length written, or -1 if it did not fit (out is then unchanged
// beyond a terminating NUL at out[0]).
int mglcfg_format(char *out, size_t outsz, const void *cfg,
                  const mglcfg_field *f, int n,
                  const mglcfg_extra *extra, int nextra, const char *indent);

// Apply every <setting> of the first <amigacd_config> block in xml to cfg.
// Returns the number of settings applied, or -1 when there is no block.
// *unknown counts names not in the table (and not claimed by extra_name),
// *bad counts values that did not parse or did not fit their field. Neither
// stops the rest from applying.
int mglcfg_apply(const char *xml, void *cfg, const mglcfg_field *f, int n,
                 int *unknown, int *bad);

// The decoded value of one setting by name, for those outside the table.
// Returns 1 and fills out when present, 0 when not.
int mglcfg_get(const char *xml, const char *name, char *out, size_t outsz);

// Replace the <amigacd_config> block in an existing .mgl with `block`, or
// insert it before </mistergamedescription> when there is none. Returns 1 if
// the text changed, 0 if it was already identical, -1 on no room or no
// </mistergamedescription> to insert before.
int mglcfg_put_block(char *xml, size_t xmlsz, const char *block);

// Remove every <file .../> element whose index attribute is `index`, with its
// line. Returns how many were removed.
int mgl_remove_file_index(char *xml, int index);

#endif
