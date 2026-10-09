// Settings carried inside a .mgl (support/minimig/mgl_cfg.cpp).
//
// The contract that matters: a config written out and read back is the same
// config, byte for byte, including paths with characters XML treats as markup;
// a file that names only some settings changes only those; a bad value or an
// unknown name costs that one setting and nothing else; and editing an existing
// file touches only the block and the CD <file> line.

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "../support/minimig/mgl_cfg.h"

static int failures = 0;

static void check(const char *name, long got, long want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %ld, want %ld\n", name, got, want); failures++; }
}

static void check_str(const char *name, const char *got, const char *want)
{
	if (!strcmp(got, want)) printf("PASS %s\n", name);
	else { printf("FAIL %s:\n  got:  %s\n  want: %s\n", name, got, want); failures++; }
}

// A stand-in with every field type, and the awkward layouts the real struct has:
// a string buffer holding two strings, odd alignment, a 32-bit enum.
struct cfg_t
{
	char     kick[64];
	uint16_t ext;
	uint8_t  cpu;
	uint8_t  chipset;
	uint32_t userport;
	char     disc[48];
	uint8_t  delay;
};

static const mglcfg_field fields[] =
{
	{ "kickstart", offsetof(cfg_t, kick),     sizeof(((cfg_t *)0)->kick),     MGLCFG_STR },
	{ "ext_cfg",   offsetof(cfg_t, ext),      2,                              MGLCFG_U16 },
	{ "cpu",       offsetof(cfg_t, cpu),      1,                              MGLCFG_U8  },
	{ "chipset",   offsetof(cfg_t, chipset),  1,                              MGLCFG_U8  },
	{ "userport",  offsetof(cfg_t, userport), 4,                              MGLCFG_U32 },
	{ "disc",      offsetof(cfg_t, disc),     sizeof(((cfg_t *)0)->disc),     MGLCFG_STR },
	{ "delay",     offsetof(cfg_t, delay),    1,                              MGLCFG_U8  },
};
static const int nfields = sizeof(fields) / sizeof(fields[0]);

static void fill(cfg_t *c)
{
	memset(c, 0, sizeof(*c));
	strcpy(c->kick, "games/AmigaCD32/CD32.rom");
	c->ext = 0xBEEF;
	c->cpu = 3;
	c->chipset = 0x1A;
	c->userport = 2;
	strcpy(c->disc, "games/Rock & Roll \"Kid's\" <Disc>.chd");
	c->delay = 255;
}

int main(void)
{
	static char block[8192];
	static char xml[16384];

	// --- round trip ---------------------------------------------------------
	cfg_t a, b;
	fill(&a);
	const mglcfg_extra extra[] = { { "extrom", "games/AmigaCD32/CD32-ext & co.rom" } };
	int len = mglcfg_format(block, sizeof(block), &a, fields, nfields, extra, 1, "\t");
	check("format succeeds", len > 0, 1);
	check("format length is strlen", len, (long)strlen(block));
	check("markup in a path is escaped", strstr(block, "Rock &amp; Roll &quot;Kid&apos;s&quot; &lt;Disc&gt;.chd") != NULL, 1);
	check("no raw ampersand survives", strstr(block, "Rock & Roll") == NULL, 1);
	check("numbers are decimal", strstr(block, "name=\"ext_cfg\" value=\"48879\"") != NULL, 1);

	snprintf(xml, sizeof(xml), "<mistergamedescription>\n\t<rbf>_Test/amigacd</rbf>\n%s</mistergamedescription>\n", block);
	memset(&b, 0x5A, sizeof(b));
	int unknown = -1, bad = -1;
	int n = mglcfg_apply(xml, &b, fields, nfields, &unknown, &bad);
	check("every table field applied", n, nfields);
	check("the extra is unknown to the table", unknown, 1);
	check("nothing bad", bad, 0);
	check("kickstart round-trips", strcmp(a.kick, b.kick), 0);
	check("string field is zero-filled past its NUL", b.kick[sizeof(b.kick) - 1], 0);
	check("disc with markup round-trips", strcmp(a.disc, b.disc), 0);
	check("u16 round-trips", b.ext, a.ext);
	check("u8 round-trips", b.cpu, a.cpu);
	check("u8 max round-trips", b.delay, 255);
	check("u32 round-trips", (long)b.userport, (long)a.userport);

	char ext[256];
	check("extra readable by name", mglcfg_get(xml, "extrom", ext, sizeof(ext)), 1);
	check_str("extra decoded", ext, "games/AmigaCD32/CD32-ext & co.rom");
	check("absent name is not found", mglcfg_get(xml, "nope", ext, sizeof(ext)), 0);

	// --- a partial, hand-edited block -----------------------------------------
	fill(&b);
	const char *partial =
		"<mistergamedescription>\n"
		"  <amigacd_config version='1'>\n"
		"    <setting value='0x11' name='chipset'/>\n"          // reversed order, single quotes, hex
		"    <setting name=\"cpu\"   value=\"999\"/>\n"           // does not fit a byte
		"    <setting name=\"delay\" value=\"-1\"/>\n"            // negative
		"    <setting name=\"disc\"  value=\"x&#38;y&#x41;\"/>\n" // numeric references
		"    <setting name=\"future_thing\" value=\"7\"/>\n"      // a later build's setting
		"    <setting name=\"ext_cfg\" value=\"\"/>\n"            // empty
		"    <setting name=\"kickstart\" value=\"" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" "\"/>\n" // 64 chars, no room for NUL
		"  </amigacd_config>\n"
		"</mistergamedescription>\n";
	n = mglcfg_apply(partial, &b, fields, nfields, &unknown, &bad);
	check("only the good ones applied", n, 2);
	check("unknown counted", unknown, 1);
	check("bad counted (cpu, delay, ext_cfg, kickstart)", bad, 4);
	check("hex accepted", b.chipset, 0x11);
	check("too-big value leaves the field", b.cpu, 3);
	check("negative leaves the field", b.delay, 255);
	check("empty leaves the field", b.ext, 0xBEEF);
	check_str("overlong string leaves the field", b.kick, "games/AmigaCD32/CD32.rom");
	check_str("numeric references decoded", b.disc, "x&yA");
	check("untouched u32 stays", (long)b.userport, 2);

	check("no block is -1", mglcfg_apply("<mistergamedescription/>", &b, fields, nfields, NULL, NULL), -1);

	// --- editing an existing file ---------------------------------------------
	snprintf(xml, sizeof(xml),
		"<mistergamedescription>\n"
		"\t<rbf>_Test/amigacd</rbf>\n"
		"\t<file delay=\"2\" type=\"f\" index=\"4\" path=\"games/x.chd\"/>\n"
		"\t<!-- mine -->\n"
		"\t<file delay=\"1\" type=\"f\" index=\"5\" path=\"Saves.hdf\"/>\n"
		"\t<reset delay=\"1\"/>\n"
		"</mistergamedescription>\n");
	check("CD file line removed", mgl_remove_file_index(xml, 4), 1);
	check("hard disk line kept", strstr(xml, "index=\"5\"") != NULL, 1);
	check("no empty line left", strstr(xml, "\n\n") == NULL, 1);
	check("insert block", mglcfg_put_block(xml, sizeof(xml), block), 1);
	check("block sits before the close tag", strstr(xml, "</amigacd_config>\n</mistergamedescription>") != NULL, 1);
	check("comment kept", strstr(xml, "<!-- mine -->") != NULL, 1);
	check("reset kept", strstr(xml, "<reset delay=\"1\"/>") != NULL, 1);
	check("same block again is no change", mglcfg_put_block(xml, sizeof(xml), block), 0);

	fill(&a);
	a.cpu = 1;
	static char block2[8192];
	mglcfg_format(block2, sizeof(block2), &a, fields, nfields, extra, 1, "\t");
	check("changed block replaces", mglcfg_put_block(xml, sizeof(xml), block2), 1);
	const char *first = strstr(xml, "<amigacd_config");
	check("exactly one block", first && !strstr(first + 1, "<amigacd_config"), 1);
	memset(&b, 0, sizeof(b));
	mglcfg_apply(xml, &b, fields, nfields, NULL, NULL);
	check("replaced block is the one read", b.cpu, 1);
	check("no close tag, no insert", mglcfg_put_block(block2, sizeof(block2), block), 1); // block2 has its own block: replace works
	char nofile[64] = "<foo></foo>";
	check("nowhere to insert is -1", mglcfg_put_block(nofile, sizeof(nofile), block), -1);
	char tiny[100];
	snprintf(tiny, sizeof(tiny), "<mistergamedescription>\n</mistergamedescription>\n");
	check("no room is -1 and leaves the text", mglcfg_put_block(tiny, sizeof(tiny), block), -1);
	check_str("text intact after refusal", tiny, "<mistergamedescription>\n</mistergamedescription>\n");

	char small[16];
	check("format that does not fit is -1", mglcfg_format(small, sizeof(small), &a, fields, nfields, NULL, 0, ""), -1);
	check("and leaves an empty string", small[0], 0);

	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL PASS\n");
	return 0;
}
