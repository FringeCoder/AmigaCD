#include "mgl_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char BLOCK_OPEN[]  = "<amigacd_config";
static const char BLOCK_CLOSE[] = "</amigacd_config>";

// --- writing -----------------------------------------------------------------

// Append s to out at *len, keeping out NUL-terminated. Sets *ok to 0 when it
// does not fit and writes nothing further after that.
static void put(char *out, size_t outsz, size_t *len, int *ok, const char *s)
{
	if (!*ok) return;
	size_t sl = strlen(s);
	if (*len + sl + 1 > outsz) { *ok = 0; return; }
	memcpy(out + *len, s, sl + 1);
	*len += sl;
}

// XML-escape src into dst. Paths are the only free text here and they can
// hold an ampersand or a quote ("Rock & Roll", "Kid's"); anything below 0x20
// is written as a character reference so the file stays one line per setting.
static void escape(char *dst, size_t dstsz, const char *src, size_t srclen)
{
	size_t d = 0;
	for (size_t i = 0; i < srclen && src[i]; i++)
	{
		char rep[8];
		const unsigned char c = (unsigned char)src[i];
		switch (c)
		{
		case '&':  strcpy(rep, "&amp;");  break;
		case '<':  strcpy(rep, "&lt;");   break;
		case '>':  strcpy(rep, "&gt;");   break;
		case '"':  strcpy(rep, "&quot;"); break;
		case '\'': strcpy(rep, "&apos;"); break;
		default:
			if (c < 0x20) snprintf(rep, sizeof(rep), "&#%u;", c);
			else { rep[0] = (char)c; rep[1] = 0; }
		}
		size_t rl = strlen(rep);
		if (d + rl + 1 > dstsz) break;
		memcpy(dst + d, rep, rl);
		d += rl;
	}
	dst[d] = 0;
}

static uint32_t read_uint(const uint8_t *p, int type)
{
	switch (type)
	{
	case MGLCFG_U8:  return p[0];
	case MGLCFG_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
	case MGLCFG_U32: { uint32_t v; memcpy(&v, p, 4); return v; }
	}
	return 0;
}

int mglcfg_format(char *out, size_t outsz, const void *cfg,
                  const mglcfg_field *f, int n,
                  const mglcfg_extra *extra, int nextra, const char *indent)
{
	if (!out || !outsz) return -1;
	out[0] = 0;
	size_t len = 0;
	int ok = 1;
	const uint8_t *base = (const uint8_t *)cfg;
	static char esc[6 * 1100];
	char line[sizeof(esc) + 256];

	snprintf(line, sizeof(line), "%s%s version=\"%d\">\n", indent, BLOCK_OPEN, MGLCFG_VERSION);
	put(out, outsz, &len, &ok, line);

	for (int i = 0; i < n; i++)
	{
		const uint8_t *p = base + f[i].off;
		if (f[i].type == MGLCFG_STR)
		{
			escape(esc, sizeof(esc), (const char *)p, strnlen((const char *)p, f[i].size));
			snprintf(line, sizeof(line), "%s\t<setting name=\"%s\" value=\"%s\"/>\n", indent, f[i].name, esc);
		}
		else
		{
			snprintf(line, sizeof(line), "%s\t<setting name=\"%s\" value=\"%u\"/>\n", indent, f[i].name,
			         (unsigned)read_uint(p, f[i].type));
		}
		put(out, outsz, &len, &ok, line);
	}
	for (int i = 0; i < nextra; i++)
	{
		escape(esc, sizeof(esc), extra[i].value, strlen(extra[i].value));
		snprintf(line, sizeof(line), "%s\t<setting name=\"%s\" value=\"%s\"/>\n", indent, extra[i].name, esc);
		put(out, outsz, &len, &ok, line);
	}

	snprintf(line, sizeof(line), "%s%s\n", indent, BLOCK_CLOSE);
	put(out, outsz, &len, &ok, line);

	if (!ok) { out[0] = 0; return -1; }
	return (int)len;
}

// --- reading -----------------------------------------------------------------

// Decode XML character entities from [s, e) into dst. Unknown entities are
// copied through as written rather than dropped.
static int unescape(char *dst, size_t dstsz, const char *s, const char *e)
{
	size_t d = 0;
	while (s < e)
	{
		char c = *s;
		size_t adv = 1;
		if (c == '&')
		{
			const char *semi = (const char *)memchr(s, ';', e - s);
			if (semi)
			{
				size_t el = semi - s + 1;
				if      (el == 5 && !strncmp(s, "&amp;", 5))  c = '&';
				else if (el == 4 && !strncmp(s, "&lt;", 4))   c = '<';
				else if (el == 4 && !strncmp(s, "&gt;", 4))   c = '>';
				else if (el == 6 && !strncmp(s, "&quot;", 6)) c = '"';
				else if (el == 6 && !strncmp(s, "&apos;", 6)) c = '\'';
				else if (el > 3 && s[1] == '#')
				{
					char num[16];
					size_t nl = el - 3;
					if (nl < sizeof(num))
					{
						memcpy(num, s + 2, nl);
						num[nl] = 0;
						char *endp;
						unsigned long v = (num[0] == 'x' || num[0] == 'X')
						                ? strtoul(num + 1, &endp, 16) : strtoul(num, &endp, 10);
						if (*endp == 0 && v > 0 && v < 256) c = (char)v;
						else el = 1;
					}
					else el = 1;
				}
				else el = 1;
				adv = el;
			}
		}
		if (d + 1 >= dstsz) return -1;
		dst[d++] = c;
		s += adv;
	}
	dst[d] = 0;
	return (int)d;
}

// End of the tag starting at t ('<'), honouring quoted attribute values.
static const char *tag_end(const char *t, const char *limit)
{
	char q = 0;
	for (const char *p = t; p < limit; p++)
	{
		if (q) { if (*p == q) q = 0; }
		else if (*p == '"' || *p == '\'') q = *p;
		else if (*p == '>') return p;
	}
	return NULL;
}

// The raw (still escaped) value of attribute `name` in tag [t, te).
static int attr(const char *t, const char *te, const char *name, const char **vs, const char **ve)
{
	size_t nl = strlen(name);
	char q = 0;
	for (const char *p = t; p < te; p++)
	{
		if (q) { if (*p == q) q = 0; continue; }
		if (*p == '"' || *p == '\'') { q = *p; continue; }
		if ((p == t || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r')
		    && p + nl < te && !strncmp(p, name, nl))
		{
			const char *a = p + nl;
			while (a < te && (*a == ' ' || *a == '\t')) a++;
			if (a >= te || *a != '=') continue;
			a++;
			while (a < te && (*a == ' ' || *a == '\t')) a++;
			if (a >= te || (*a != '"' && *a != '\'')) continue;
			const char qq = *a++;
			const char *z = (const char *)memchr(a, qq, te - a);
			if (!z) return 0;
			*vs = a;
			*ve = z;
			return 1;
		}
	}
	return 0;
}

static int find_block(const char *xml, const char **bs, const char **be)
{
	const char *s = strstr(xml, BLOCK_OPEN);
	if (!s) return 0;
	const char *e = strstr(s, BLOCK_CLOSE);
	if (!e) return 0;
	*bs = s;
	*be = e;
	return 1;
}

// Calls fn for each <setting> in the block, with the decoded name and value.
// Stops early when fn returns nonzero, and returns that.
typedef int (*setting_fn)(const char *name, const char *value, void *ud);
static int each_setting(const char *xml, setting_fn fn, void *ud)
{
	const char *bs, *be;
	if (!find_block(xml, &bs, &be)) return -1;
	static char name[128];
	static char value[2048];
	for (const char *p = bs; (p = strstr(p, "<setting")) && p < be; )
	{
		const char *te = tag_end(p, be);
		if (!te) break;
		const char *ns, *ne, *vs, *ve;
		if (attr(p + 8, te, "name", &ns, &ne) && attr(p + 8, te, "value", &vs, &ve)
		    && unescape(name, sizeof(name), ns, ne) >= 0)
		{
			int r;
			if (unescape(value, sizeof(value), vs, ve) < 0) r = fn(name, NULL, ud);
			else r = fn(name, value, ud);
			if (r) return r;
		}
		p = te + 1;
	}
	return 0;
}

struct apply_ctx
{
	uint8_t            *base;
	const mglcfg_field *f;
	int                 n;
	int                 applied, unknown, bad;
};

static int apply_one(const char *name, const char *value, void *ud)
{
	apply_ctx *c = (apply_ctx *)ud;
	for (int i = 0; i < c->n; i++)
	{
		if (strcmp(name, c->f[i].name)) continue;
		uint8_t *p = c->base + c->f[i].off;
		if (!value) { c->bad++; return 0; }
		if (c->f[i].type == MGLCFG_STR)
		{
			size_t vl = strlen(value);
			if (vl >= c->f[i].size) { c->bad++; return 0; }
			memset(p, 0, c->f[i].size);
			memcpy(p, value, vl);
		}
		else
		{
			char *endp;
			errno = 0;
			unsigned long v = strtoul(value, &endp, 0);
			const unsigned long max = (c->f[i].type == MGLCFG_U8) ? 0xFFUL :
			                          (c->f[i].type == MGLCFG_U16) ? 0xFFFFUL : 0xFFFFFFFFUL;
			if (!value[0] || *endp || errno || v > max || value[0] == '-') { c->bad++; return 0; }
			if (c->f[i].type == MGLCFG_U8)       { p[0] = (uint8_t)v; }
			else if (c->f[i].type == MGLCFG_U16) { uint16_t w = (uint16_t)v; memcpy(p, &w, 2); }
			else                                  { uint32_t w = (uint32_t)v; memcpy(p, &w, 4); }
		}
		c->applied++;
		return 0;
	}
	c->unknown++;
	return 0;
}

int mglcfg_apply(const char *xml, void *cfg, const mglcfg_field *f, int n,
                 int *unknown, int *bad)
{
	apply_ctx c = { (uint8_t *)cfg, f, n, 0, 0, 0 };
	if (each_setting(xml, apply_one, &c) < 0) return -1;
	if (unknown) *unknown = c.unknown;
	if (bad) *bad = c.bad;
	return c.applied;
}

struct get_ctx
{
	const char *name;
	char       *out;
	size_t      outsz;
};

static int get_one(const char *name, const char *value, void *ud)
{
	get_ctx *c = (get_ctx *)ud;
	if (strcmp(name, c->name) || !value) return 0;
	if (strlen(value) >= c->outsz) return 0;
	strcpy(c->out, value);
	return 1;
}

int mglcfg_get(const char *xml, const char *name, char *out, size_t outsz)
{
	get_ctx c = { name, out, outsz };
	return each_setting(xml, get_one, &c) == 1;
}

// --- editing an existing file -----------------------------------------------

static char *line_start(char *xml, char *p)
{
	while (p > xml && p[-1] != '\n') p--;
	return p;
}

static char *line_after(char *p)
{
	char *nl = strchr(p, '\n');
	return nl ? nl + 1 : p + strlen(p);
}

static int splice(char *xml, size_t xmlsz, char *from, char *to, const char *with)
{
	const size_t wl = strlen(with);
	const size_t tail = strlen(to);
	const size_t newlen = (size_t)(from - xml) + wl + tail;
	if (newlen + 1 > xmlsz) return -1;
	memmove(from + wl, to, tail + 1);
	memcpy(from, with, wl);
	return 1;
}

int mglcfg_put_block(char *xml, size_t xmlsz, const char *block)
{
	char *bs = strstr(xml, BLOCK_OPEN);
	char *be = bs ? strstr(bs, BLOCK_CLOSE) : NULL;
	if (bs && be)
	{
		char *from = line_start(xml, bs);
		char *to = line_after(be);
		const size_t cur = (size_t)(to - from);
		if (cur == strlen(block) && !memcmp(from, block, cur)) return 0;
		return splice(xml, xmlsz, from, to, block);
	}
	char *close = strstr(xml, "</mistergamedescription>");
	if (!close) return -1;
	char *at = line_start(xml, close);
	return splice(xml, xmlsz, at, at, block);
}

int mgl_remove_file_index(char *xml, int index)
{
	int removed = 0;
	char *p = xml;
	while ((p = strstr(p, "<file")))
	{
		char *te = (char *)tag_end(p, p + strlen(p));
		if (!te) break;
		const char *vs, *ve;
		char num[16];
		if (attr(p + 5, te, "index", &vs, &ve) && (size_t)(ve - vs) < sizeof(num))
		{
			memcpy(num, vs, ve - vs);
			num[ve - vs] = 0;
			char *endp;
			long v = strtol(num, &endp, 0);
			if (!*endp && v == index)
			{
				// Take the whole line when the element is alone on it, so the
				// file does not keep an empty indented line behind.
				char *ls = line_start(xml, p);
				char *le = line_after(te);
				int alone = 1;
				for (char *q = ls; q < p; q++) if (*q != ' ' && *q != '\t') alone = 0;
				for (char *q = te + 1; q < le; q++) if (*q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') alone = 0;
				char *from = alone ? ls : p;
				char *to = alone ? le : te + 1;
				memmove(from, to, strlen(to) + 1);
				removed++;
				p = from;
				continue;
			}
		}
		p = te + 1;
	}
	return removed;
}
