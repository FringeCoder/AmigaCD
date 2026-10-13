// DMS archives unpacked to ADF images (support/minimig/dms.cpp).
//
// Two kinds of evidence, and they prove different things:
//
//  1. data/dms-rle-small.dms was made by an INDEPENDENT encoder (adf2dms), and
//     comes from the xdms-rs test suite. Its source image is regenerated here
//     from the recipe that made it, so the check is byte-for-byte against what
//     went in, not against a hash someone pasted. It covers the archive and
//     track headers, both CRCs, the checksum, NOCOMP, SIMPLE, and the banner
//     and FILEID.DIZ tracks that must be skipped.
//
//  2. Every other mode is a ROUND TRIP through encoders written here, from the
//     bitstream each decoder reads. That proves the decoders invert a reading
//     of the format; it cannot prove the reading is the format's. The bit
//     layouts were ported from xdms-rs, which is byte-exact against the
//     reference C xDMS on real HEAVY1/HEAVY2 disks, but no real QUICK, MEDIUM,
//     DEEP or HEAVY archive has been through THIS decoder: none was reachable
//     from where it was written. A real disk is the remaining check.
//
// The encoders model the decoder's sliding window by keeping a Decoder of
// their own and pushing every byte they emit through it, so window positions
// and the tail that survives a reset agree by construction. That much is
// shared with the DUT and cannot be checked by a round trip -- the per-mode
// end-of-track advance (+5/+66/+60) in particular. Everything about the BITS
// is independent: literal/match flags, the MEDIUM and DEEP distance tables
// (inverted here by search), DEEP's adaptive tree (walked leaf to root), and
// HEAVY's canonical codes (assigned here from lengths; make_table has to
// agree).
//
// FALSIFIED against these mutations -- each was applied to dms.cpp and the
// named check failed:
//   - one D_CODE entry changed (0x20 -> 0x21)  -> "D_CODE has LZHUF's shape"
//   - one D_LEN entry changed (0x07 -> 0x08)   -> "D_LEN has LZHUF's shape"
//   - HEAVY "repeat last distance" not stored -> "HEAVY1 round trip", HEAVY2
//   - HEAVY2's window masked to 4 KB           -> "HEAVY2 matches beyond 4 KB"
//   - long-code tree walk, left/right swapped  -> "HEAVY1 round trip", HEAVY2
//   - QUICK end-of-track advance 5 -> 4        -> "QUICK keep-state ..."
//   - MEDIUM end-of-track advance 66 -> 65     -> "MEDIUM round trip, with ..."
//   - DEEP end-of-track advance 60 -> 59       -> "DEEP keep-state ..."
//   - reset even after a keep-state track      -> every keep-state check
//   - tracks appended in order, not placed     -> "out of order tracks ..."
//   - the banner decoded as data, into cyl 0   -> "banner, FILEID.DIZ ..."
//   - the data-CRC check removed               -> "data CRC catches ..."
//
// TWO MUTATIONS THIS CANNOT CATCH, because the encoders' window model is the
// DUT's own Decoder and so moves with it:
//   - DEEP's tree rebuilt at 0x7fff instead of 0x8000. "DEEP chain" does
//     take the rebuild (five noise tracks, ~56K symbols against a 32K
//     threshold), but encoder and decoder rebuild at the same wrong moment.
//   - the window cleared over 0x100 bytes on reset instead of 0x3fc8. Nothing
//     here matches into a freshly cleared window.
// The threshold and the clear length are LZHUF's and xDMS's constants, carried
// over as they are; a real DEEP disk is what would check them, and DEEP is the
// mode xdms-rs found no public sample of either.

// The DUT is included, not linked: the encoders need its Decoder (window
// model, DEEP tree) and its tables, which are file-local by design.
#include "../support/minimig/dms.cpp"

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

static int failures = 0;

static void check(const char *name, bool ok)
{
	if (ok) printf("PASS %s\n", name);
	else { printf("FAIL %s\n", name); failures++; }
}

static void check_res(const char *name, dms_result got, dms_result want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got \"%s\", want \"%s\"\n", name, dms_strerror(got), dms_strerror(want)); failures++; }
}

// ---------------------------------------------------------------------------
// Test data: a cylinder with literals, repeats near and far, runs, and 0x90s,
// so every encoder has matches to find and RLE has escapes to write.

static uint32_t rng_state = 1;
static uint32_t rnd() { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static std::vector<uint8_t> make_cylinder(uint32_t seed, int style)
{
	rng_state = seed;
	std::vector<uint8_t> d;
	while (d.size() < DMS_CYL_SIZE)
	{
		uint32_t pick = style == 1 ? 0 : rnd() % 6;   // style 1: mostly noise
		size_t n = 1 + rnd() % 200;
		if (pick <= 1 || d.size() < 16)
			for (size_t i = 0; i < n; i++) d.push_back((uint8_t)rnd());
		else if (pick == 2)
			d.insert(d.end(), n * 4, (uint8_t)(rnd() & 1 ? 0x90 : rnd()));
		else if (pick == 3)
			for (size_t i = 0; i < n; i++) d.push_back(0x90);
		else
		{
			// a copy of something earlier, near or far
			size_t back = 1 + rnd() % std::min<size_t>(d.size(), pick == 4 ? 64 : 9000);
			size_t from = d.size() - back;
			for (size_t i = 0; i < n; i++) d.push_back(d[from + i]);
		}
	}
	d.resize(DMS_CYL_SIZE);
	return d;
}

// Two cylinders where `b` opens with the last `n` bytes of `a`, as plain noise
// with no 0x90 and no runs, so RLE leaves the junction alone. A keep-state
// encoder then finds matches that reach back across the track boundary --
// the only place a decoder's end-of-track window advance is visible.
static void make_junction(uint32_t seed, size_t n, std::vector<uint8_t> &a, std::vector<uint8_t> &b)
{
	a = make_cylinder(seed, 0);
	b = make_cylinder(seed + 1, 0);
	rng_state = seed + 2;
	std::vector<uint8_t> tail;
	while (tail.size() < n)
	{
		uint8_t v = (uint8_t)rnd();
		if (v == 0x90 || (!tail.empty() && v == tail.back())) continue;
		tail.push_back(v);
	}
	// Separate the tail from whatever precedes it, so no run straddles it.
	a[DMS_CYL_SIZE - n - 1] = (uint8_t)(tail[0] ^ 0x55) == 0x90 ? 0x11 : (uint8_t)(tail[0] ^ 0x55);
	memcpy(a.data() + DMS_CYL_SIZE - n, tail.data(), n);
	memcpy(b.data(), tail.data(), n);
	b[n] = (uint8_t)(tail[n - 1] ^ 0x55) == 0x90 ? 0x11 : (uint8_t)(tail[n - 1] ^ 0x55);
}

// ---------------------------------------------------------------------------
// Bit writer, MSB first, matching BitReader.

struct BitWriter
{
	std::vector<uint8_t> out;
	uint64_t acc = 0;
	int n = 0;
	void put(uint32_t v, int bits)
	{
		for (int i = bits - 1; i >= 0; i--)
		{
			acc = (acc << 1) | ((v >> i) & 1);
			if (++n == 8) { out.push_back((uint8_t)acc); acc = 0; n = 0; }
		}
	}
	std::vector<uint8_t> finish() { if (n) { out.push_back((uint8_t)(acc << (8 - n))); n = 0; } return out; }
};

// ---------------------------------------------------------------------------
// RLE, the stage after every LZ mode: 0x90 escapes, short and long runs.

static std::vector<uint8_t> rle_encode(const std::vector<uint8_t> &in)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; i < in.size();)
	{
		size_t run = 1;
		while (i + run < in.size() && in[i + run] == in[i] && run < 0xffff) run++;
		if (run >= 4)
		{
			out.push_back(0x90);
			if (run < 0xff) out.push_back((uint8_t)run);
			else { out.push_back(0xff); }
			out.push_back(in[i]);
			if (run >= 0xff) { out.push_back((uint8_t)(run >> 8)); out.push_back((uint8_t)run); }
			i += run;
		}
		else
		{
			out.push_back(in[i]);
			if (in[i] == 0x90) out.push_back(0x00);
			i++;
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Greedy match search against the model window. `data` is the stage-1 stream
// being encoded; bytes before `cur` have been pushed into the window already.
// A match may overlap what it is producing, exactly as the decoder copies.

static int match_len(const Decoder &m, uint16_t pos, uint16_t mask, const std::vector<uint8_t> &data,
                     size_t cur, uint16_t dist, int maxlen)
{
	int len = 0;
	while (len < maxlen && cur + len < data.size())
	{
		uint8_t b = (len <= dist) ? m.window[(uint16_t)(pos - dist - 1 + len) & mask]
		                          : data[cur + len - (dist + 1)];
		if (b != data[cur + len]) break;
		len++;
	}
	return len;
}

static void model_emit(Decoder &m, uint16_t &pos, uint16_t mask, const std::vector<uint8_t> &data, size_t cur, int len)
{
	for (int i = 0; i < len; i++) m.push(pos, mask, data[cur + i]);
}

// QUICK: 1+8 literal, 0+2+8 match, lengths 2..5, distances 0..255.
static std::vector<uint8_t> quick_encode(Decoder &m, const std::vector<uint8_t> &data)
{
	BitWriter w;
	for (size_t i = 0; i < data.size();)
	{
		int best = 0, bd = 0;
		for (int d = 0; d < 256; d++)
		{
			int l = match_len(m, m.quick_pos, 0xff, data, i, (uint16_t)d, 5);
			if (l > best) { best = l; bd = d; }
		}
		if (best >= 2)
		{
			w.put(0, 1); w.put(best - 2, 2); w.put(bd, 8);
			model_emit(m, m.quick_pos, 0xff, data, i, best);
			i += best;
		}
		else
		{
			w.put(1, 1); w.put(data[i], 8);
			model_emit(m, m.quick_pos, 0xff, data, i, 1);
			i++;
		}
	}
	m.quick_pos = (uint16_t)((m.quick_pos + 5) & 0xff);
	return w.finish();
}

// MEDIUM: invert the two-level table by search. For every prefix and every
// pair of extra-bit fields the decoder could read, record which (length,
// distance) it yields; the encoder then only emits matches that exist.
struct MediumCode { bool ok; uint8_t p, e1, n1, e2, n2; };
static std::vector<MediumCode> medium_codes(67 * 0x4000);   // [len * 0x4000 + dist]

static void build_medium_codes()
{
	for (int p = 0; p < 256; p++)
	{
		int n1 = D_LEN[p];
		for (int e1 = 0; e1 < (1 << n1); e1++)
		{
			int mid = ((p << n1) | e1) & 0xff;
			int n2 = D_LEN[mid];
			for (int e2 = 0; e2 < (1 << n2); e2++)
			{
				uint32_t dist = (D_CODE[mid] << 8) | (((mid << n2) | e2) & 0xff);
				uint32_t len = D_CODE[p] + 3;
				MediumCode &c = medium_codes[len * 0x4000 + dist];
				if (!c.ok) c = MediumCode{ true, (uint8_t)p, (uint8_t)e1, (uint8_t)n1, (uint8_t)e2, (uint8_t)n2 };
			}
		}
	}
}

static std::vector<uint8_t> medium_encode(Decoder &m, const std::vector<uint8_t> &data)
{
	BitWriter w;
	for (size_t i = 0; i < data.size();)
	{
		int best = 0;
		MediumCode bc = {};
		for (int d = 0; d < 4096; d++)
		{
			int l = match_len(m, m.medium_pos, 0x3fff, data, i, (uint16_t)d, 66);
			// The length is fixed by the prefix, so try each one from longest.
			for (; l > best && l >= 3; l--)
			{
				const MediumCode &c = medium_codes[l * 0x4000 + d];
				if (c.ok) { best = l; bc = c; break; }
			}
		}
		if (best >= 3)
		{
			w.put(0, 1); w.put(bc.p, 8); w.put(bc.e1, bc.n1); w.put(bc.e2, bc.n2);
			model_emit(m, m.medium_pos, 0x3fff, data, i, best);
			i += best;
		}
		else
		{
			w.put(1, 1); w.put(data[i], 8);
			model_emit(m, m.medium_pos, 0x3fff, data, i, 1);
			i++;
		}
	}
	m.medium_pos = (uint16_t)((m.medium_pos + 66) & 0x3fff);
	return w.finish();
}

// DEEP: symbol bits come from walking the model's adaptive tree from the leaf
// up; the model then updates exactly as the decoder will.
struct DeepDist { bool ok; uint8_t p, e; };
static std::vector<DeepDist> deep_dist_codes(0x4000);

static void build_deep_codes()
{
	for (int p = 0; p < 256; p++)
		for (int e = 0; e < (1 << D_LEN[p]); e++)
		{
			uint16_t dist = (uint16_t)((D_CODE[p] << 8) | (((p << D_LEN[p]) | e) & 0xff));
			if (!deep_dist_codes[dist].ok) deep_dist_codes[dist] = DeepDist{ true, (uint8_t)p, (uint8_t)e };
		}
}

static void deep_put_symbol(Decoder &m, BitWriter &w, uint16_t sym)
{
	// Tree positions run 0..T-1; a position whose son[] is >= T is the leaf
	// for symbol son[] - T. The decoder steps son[pos] + bit from the root,
	// so each bit going up is a position minus its parent's son[].
	int bits[64], n = 0;
	uint16_t node = m.deep_prnt[sym + DEEP_T];
	while (node != DEEP_R)
	{
		uint16_t parent = m.deep_prnt[node];
		bits[n++] = node - m.deep_son[parent];
		node = parent;
	}
	while (n--) w.put(bits[n], 1);
	m.deep_update(sym);
}

static std::vector<uint8_t> deep_encode(Decoder &m, const std::vector<uint8_t> &data)
{
	BitWriter w;
	if (m.deep_init) m.deep_init_tree();
	for (size_t i = 0; i < data.size();)
	{
		int best = 0, bd = 0;
		for (int d = 0; d < 4096; d++)
		{
			if (!deep_dist_codes[d].ok) continue;
			int l = match_len(m, m.deep_pos, 0x3fff, data, i, (uint16_t)d, 60);
			if (l > best) { best = l; bd = d; }
		}
		if (best >= 3)
		{
			deep_put_symbol(m, w, (uint16_t)(best + 253));
			const DeepDist &c = deep_dist_codes[bd];
			w.put(c.p, 8); w.put(c.e, D_LEN[c.p]);
			model_emit(m, m.deep_pos, 0x3fff, data, i, best);
			i += best;
		}
		else
		{
			deep_put_symbol(m, w, data[i]);
			model_emit(m, m.deep_pos, 0x3fff, data, i, 1);
			i++;
		}
	}
	m.deep_pos = (uint16_t)((m.deep_pos + 60) & 0x3fff);
	return w.finish();
}

// HEAVY: canonical Huffman. Lengths come from symbol counts (every symbol
// counted at least once, so a later no-rebuild track can use any of them,
// and so the long tail runs well past the 12- and 8-bit tables); codes are
// assigned in (length, symbol) order, which is the order make_table fills
// its table in.
static std::vector<uint8_t> huffman_lengths(std::vector<uint32_t> freq, int maxlen)
{
	size_t n = freq.size();
	std::vector<uint8_t> len(n);
	for (;;)
	{
		// Plain Huffman by repeated merge over (weight, members).
		std::vector<std::pair<uint64_t, std::vector<int> > > nodes;
		for (size_t i = 0; i < n; i++) nodes.push_back(std::make_pair((uint64_t)freq[i], std::vector<int>(1, (int)i)));
		std::fill(len.begin(), len.end(), 0);
		while (nodes.size() > 1)
		{
			std::sort(nodes.begin(), nodes.end(),
			          [](const std::pair<uint64_t, std::vector<int> > &a, const std::pair<uint64_t, std::vector<int> > &b) { return a.first > b.first; });
			std::pair<uint64_t, std::vector<int> > a = nodes.back(); nodes.pop_back();
			std::pair<uint64_t, std::vector<int> > b = nodes.back(); nodes.pop_back();
			for (int s : a.second) len[s]++;
			for (int s : b.second) len[s]++;
			a.second.insert(a.second.end(), b.second.begin(), b.second.end());
			nodes.push_back(std::make_pair(a.first + b.first, a.second));
		}
		if (*std::max_element(len.begin(), len.end()) <= maxlen) return len;
		for (auto &f : freq) f = f / 2 + 1;   // flatten and retry
	}
}

struct Canon { std::vector<uint32_t> code; std::vector<uint8_t> len; };

static Canon canonical(const std::vector<uint8_t> &len)
{
	Canon c;
	c.len = len;
	c.code.assign(len.size(), 0);
	uint32_t code = 0;
	for (int l = 1; l <= 31; l++)
	{
		for (size_t s = 0; s < len.size(); s++)
			if (len[s] == l) c.code[s] = code++;
		code <<= 1;
	}
	return c;
}

struct HeavyEnc
{
	Canon c, p;            // carried across tracks, as the decoder's tables are
	uint16_t np = 14;
};

static int bitlen(uint32_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

// One HEAVY track. `single` forces the no-tree form (count 0, one symbol) on
// data that is a single repeated literal with no matches.
static std::vector<uint8_t> heavy_encode(Decoder &m, HeavyEnc &h, bool big, bool rebuild, bool single,
                                         const std::vector<uint8_t> &data)
{
	const uint16_t np = big ? 15 : 14;
	const uint16_t mask = big ? 0x1fff : 0x0fff;
	const int maxdist = 1 << (np - 2);

	// Parse first against a copy of the window, to count symbols.
	struct Tok { uint16_t sym; uint16_t dist; };
	std::vector<Tok> toks;
	{
		Decoder probe = m;
		for (size_t i = 0; i < data.size();)
		{
			int best = 0, bd = 0;
			if (!single)
				for (int d = 0; d < maxdist; d++)
				{
					int l = match_len(probe, probe.heavy_pos, mask, data, i, (uint16_t)d, 256);
					if (l > best) { best = l; bd = d; }
				}
			if (best >= 3)
			{
				toks.push_back(Tok{ (uint16_t)(best + HEAVY_OFFSET), (uint16_t)bd });
				model_emit(probe, probe.heavy_pos, mask, data, i, best);
				i += best;
			}
			else
			{
				toks.push_back(Tok{ data[i], 0 });
				model_emit(probe, probe.heavy_pos, mask, data, i, 1);
				i++;
			}
		}
	}

	BitWriter w;
	if (rebuild)
	{
		if (single)
		{
			w.put(0, 9); w.put(toks[0].sym, 9);
			w.put(0, 5); w.put(0, 5);
			h.c = Canon(); h.c.len.assign(HEAVY_NC, 0); h.c.code.assign(HEAVY_NC, 0);
			h.p = Canon(); h.p.len.assign(np, 0); h.p.code.assign(np, 0);
		}
		else
		{
			std::vector<uint32_t> cf(HEAVY_NC, 1), pf(np, 1);
			uint16_t last = m.heavy_last;
			for (const Tok &t : toks)
			{
				cf[t.sym] += 50;
				if (t.sym >= 256) { pf[t.dist == last ? np - 1 : bitlen(t.dist)] += 50; last = t.dist; }
			}
			h.c = canonical(huffman_lengths(cf, 16));
			h.p = canonical(huffman_lengths(pf, 15));
			w.put(HEAVY_NC, 9);
			for (int i = 0; i < HEAVY_NC; i++) w.put(h.c.len[i], 5);
			w.put(np, 5);
			for (int i = 0; i < np; i++) w.put(h.p.len[i], 4);
		}
		h.np = np;
	}

	for (const Tok &t : toks)
	{
		w.put(h.c.code[t.sym], h.c.len[t.sym]);
		if (t.sym >= 256)
		{
			if (t.dist == m.heavy_last && h.p.len[np - 1])
				w.put(h.p.code[np - 1], h.p.len[np - 1]);
			else
			{
				int node = bitlen(t.dist);
				w.put(h.p.code[node], h.p.len[node]);
				if (node > 1) w.put(t.dist - (1u << (node - 1)), node - 1);
				m.heavy_last = t.dist;
			}
		}
	}
	// Advance the real model by what was emitted.
	for (size_t i = 0; i < data.size(); i++) m.push(m.heavy_pos, mask, data[i]);
	return w.finish();
}

// ---------------------------------------------------------------------------
// Archive assembly.

static void put16(std::vector<uint8_t> &v, size_t at, uint16_t x) { v[at] = (uint8_t)(x >> 8); v[at + 1] = (uint8_t)x; }

static std::vector<uint8_t> archive_header(uint16_t geninfo = 0, uint16_t disktype = 0)
{
	std::vector<uint8_t> h(HEADER_LEN, 0);
	memcpy(h.data(), "DMS!", 4);
	put16(h, 10, geninfo);
	put16(h, 50, disktype);
	put16(h, 52, 6);
	put16(h, 54, crc16(h.data() + 4, HEADER_LEN - 6));
	return h;
}

static void add_track(std::vector<uint8_t> &a, uint16_t number, uint8_t mode, uint8_t flags,
                      const std::vector<uint8_t> &packed, size_t mid_len, const std::vector<uint8_t> &unpacked)
{
	std::vector<uint8_t> t(TRACK_HEADER_LEN, 0);
	t[0] = 'T'; t[1] = 'R';
	put16(t, 2, number);
	put16(t, 6, (uint16_t)packed.size());
	put16(t, 8, (uint16_t)mid_len);
	put16(t, 10, (uint16_t)unpacked.size());
	t[12] = flags;
	t[13] = mode;
	put16(t, 14, checksum16(unpacked.data(), unpacked.size()));
	put16(t, 16, crc16(packed.data(), packed.size()));
	put16(t, 18, crc16(t.data(), TRACK_HEADER_LEN - 2));
	a.insert(a.end(), t.begin(), t.end());
	a.insert(a.end(), packed.begin(), packed.end());
}

// The encoder side of one archive: a model decoder plus HEAVY's tables.
struct Enc
{
	Decoder m;
	HeavyEnc h;
	std::vector<uint8_t> archive = archive_header();
	std::vector<uint8_t> expect = std::vector<uint8_t>(DMS_ADF_SIZE, 0);

	void track(uint16_t number, uint8_t mode, uint8_t flags, const std::vector<uint8_t> &cyl, bool single = false)
	{
		std::vector<uint8_t> stage1, packed;
		switch (mode)
		{
		case 0: packed = cyl; break;
		case 1: packed = rle_encode(cyl); break;
		case 2: stage1 = rle_encode(cyl); packed = quick_encode(m, stage1); break;
		case 3: stage1 = rle_encode(cyl); packed = medium_encode(m, stage1); break;
		case 4: stage1 = rle_encode(cyl); packed = deep_encode(m, stage1); break;
		default:
			stage1 = (flags & 4) ? rle_encode(cyl) : cyl;
			packed = heavy_encode(m, h, mode == 6, (flags & 2) != 0, single, stage1);
			break;
		}
		if (!(flags & 1)) m.reset();
		add_track(archive, number, mode, flags, packed, stage1.size(), cyl);
		if (number < DMS_CYLINDERS) memcpy(expect.data() + number * DMS_CYL_SIZE, cyl.data(), DMS_CYL_SIZE);
	}
};

static bool unpack_equals(const char *name, const Enc &e)
{
	std::vector<uint8_t> adf;
	int bad = 0;
	dms_result r = dms_unpack(e.archive.data(), e.archive.size(), adf, &bad);
	if (r != DMS_OK)
	{
		printf("FAIL %s: %s on track %d\n", name, dms_strerror(r), bad);
		failures++;
		return false;
	}
	check(name, adf == e.expect);
	return adf == e.expect;
}

// ---------------------------------------------------------------------------

static std::vector<uint8_t> read_file(const char *path)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(path, "rb");
	if (!f) return v;
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
	fclose(f);
	return v;
}

static void test_rle_fixture()
{
	std::vector<uint8_t> dms = read_file("data/dms-rle-small.dms");
	check("rle fixture is present", dms.size() == 2225);
	if (dms.size() != 2225) return;

	// The source image, from the recipe in xdms-rs tests/data/README.md.
	std::vector<uint8_t> src;
	for (int cyl = 0; cyl < 2; cyl++)
	{
		src.insert(src.end(), 4000, 0xAA);
		src.insert(src.end(), 300, 0x90);
		for (int i = 0; i < 1000; i++) src.push_back((uint8_t)((i * 7 + cyl) & 0xff));
		src.insert(src.end(), 11264 - 5300, 0);
	}

	std::vector<uint8_t> adf;
	dms_result r = dms_unpack(dms.data(), dms.size(), adf);
	check_res("rle fixture unpacks", r, DMS_OK);
	if (r != DMS_OK) return;
	check("rle fixture gives a full 80-cylinder image", adf.size() == DMS_ADF_SIZE);
	check("rle fixture matches its source, byte for byte", !memcmp(adf.data(), src.data(), src.size()));
	bool rest_zero = std::all_of(adf.begin() + src.size(), adf.end(), [](uint8_t b) { return b == 0; });
	check("cylinders the archive does not carry are zero", rest_zero);
}

// D_CODE and D_LEN are shared with the encoders above, so a round trip cannot
// see an error in them. They are Okumura's LZHUF position tables, which have
// a fixed shape: six groups, each a run of codes repeated a fixed number of
// times, and every code in a group taking the same extra-bit count. Rebuilt
// here from that shape, not copied.
static void test_tables()
{
	static const struct { int codes, repeat; } groups[] = { { 1, 32 }, { 3, 16 }, { 8, 8 }, { 12, 4 }, { 24, 2 }, { 16, 1 } };
	uint8_t code[256], len[256];
	int at = 0, c = 0;
	for (int g = 0; g < 6; g++)
		for (int i = 0; i < groups[g].codes; i++, c++)
			for (int r = 0; r < groups[g].repeat; r++, at++) code[at] = (uint8_t)c;
	// The extra-bit count steps up every 32, 48, 64, 48, 48, 16 entries.
	static const int span[] = { 32, 48, 64, 48, 48, 16 };
	at = 0;
	for (int g = 0; g < 6; g++)
		for (int i = 0; i < span[g]; i++) len[at++] = (uint8_t)(3 + g);
	check("D_CODE has LZHUF's shape", !memcmp(code, D_CODE, 256));
	check("D_LEN has LZHUF's shape", !memcmp(len, D_LEN, 256));
}

static void test_modes()
{
	{
		Enc e;
		e.track(0, 0, 0, make_cylinder(11, 0));
		e.track(1, 1, 0, make_cylinder(12, 0));
		unpack_equals("NOCOMP and SIMPLE round trip", e);
	}
	{
		Enc e;
		e.track(0, 2, 0, make_cylinder(21, 0));
		e.track(1, 2, 0, make_cylinder(22, 0));
		unpack_equals("QUICK round trip", e);
	}
	{
		// The second track's matches reach into the first track's window.
		Enc e;
		std::vector<uint8_t> a, b;
		make_junction(23, 200, a, b);
		e.track(0, 2, 1, a);
		e.track(1, 2, 0, b);
		unpack_equals("QUICK keep-state carries the window", e);
	}
	{
		Enc e;
		std::vector<uint8_t> a = make_cylinder(31, 0);
		e.track(0, 3, 1, a);
		e.track(1, 3, 0, a);
		e.track(2, 3, 0, make_cylinder(32, 0));
		unpack_equals("MEDIUM round trip, with keep-state", e);
	}
	{
		Enc e;
		e.track(0, 4, 0, make_cylinder(41, 0));
		e.track(1, 4, 0, make_cylinder(42, 0));
		unpack_equals("DEEP round trip", e);
	}
	{
		Enc e;
		std::vector<uint8_t> a, b;
		make_junction(44, 1000, a, b);
		e.track(0, 4, 1, a);
		e.track(1, 4, 0, b);
		unpack_equals("DEEP keep-state carries the window", e);
	}
	{
		// Mostly-noise tracks chained with keep-state: the adaptive tree sees
		// well over 0x8000 symbols, so it is rebuilt mid-track at least once.
		Enc e;
		for (int t = 0; t < 5; t++) e.track((uint16_t)t, 4, t < 4 ? 1 : 0, make_cylinder(43 + t, 1));
		unpack_equals("DEEP chain through a tree reconstruct", e);
	}
	{
		Enc e;
		e.track(0, 5, 2 | 4, make_cylinder(51, 0));     // rebuild, RLE
		e.track(1, 5, 2, make_cylinder(52, 0));         // rebuild, no RLE
		e.track(2, 5, 4, make_cylinder(53, 0));         // reuse tables across a reset
		unpack_equals("HEAVY1 round trip", e);
	}
	{
		Enc e;
		std::vector<uint8_t> a = make_cylinder(61, 0);
		e.track(0, 6, 2 | 4 | 1, a);
		e.track(1, 6, 0 | 4, a);                        // tables reused, window kept
		unpack_equals("HEAVY2 round trip, with keep-state", e);
	}
	{
		// HEAVY2's whole point is its 8 KB window: a repeat 6000 bytes back
		// is reachable there and not in HEAVY1's 4 KB.
		Enc e;
		std::vector<uint8_t> a = make_cylinder(62, 1);
		memcpy(a.data() + 6000, a.data(), 5000);
		e.track(0, 6, 2, a);
		unpack_equals("HEAVY2 matches beyond 4 KB", e);
	}
	{
		// Noise has nothing to match and every literal in play, so the code
		// lengths spread out past 12 bits and decode through the tree walk.
		Enc e;
		e.track(0, 5, 2, make_cylinder(71, 1));
		bool long_codes = false;
		for (int s = 0; s < HEAVY_NC; s++) long_codes |= e.h.c.len[s] > 12;
		check("HEAVY test tree has codes longer than the table", long_codes);
		unpack_equals("HEAVY long codes through the tree walk", e);
	}
	{
		Enc e;
		e.track(0, 5, 2, std::vector<uint8_t>(DMS_CYL_SIZE, 0x41), true);
		unpack_equals("HEAVY single-symbol tree", e);
	}
}

static void test_layout()
{
	{
		// Non-data tracks between data tracks must neither land in the image
		// nor disturb the decoder state the data tracks share.
		Enc e;
		e.track(0, 2, 1, make_cylinder(81, 0));
		// Banner big enough that only its track number marks it as not data.
		std::vector<uint8_t> banner(DMS_CYL_SIZE, 'B');
		add_track(e.archive, 0xffff, 1, 0, rle_encode(banner), 0, banner);
		add_track(e.archive, 80, 0, 0, std::vector<uint8_t>(40, 'D'), 0, std::vector<uint8_t>(40, 'D'));
		add_track(e.archive, 1, 0, 0, std::vector<uint8_t>(1024, 'F'), 0, std::vector<uint8_t>(1024, 'F')); // fake bootblock
		e.track(1, 2, 0, make_cylinder(82, 0));
		unpack_equals("banner, FILEID.DIZ and fake bootblock are skipped", e);
	}
	{
		Enc e;
		e.track(5, 1, 0, make_cylinder(91, 0));
		e.track(2, 1, 0, make_cylinder(92, 0));
		e.track(79, 0, 0, make_cylinder(93, 0));
		unpack_equals("out of order tracks land at their own cylinder", e);
	}
	{
		Enc e;
		e.track(0, 1, 0, make_cylinder(94, 0));
		e.archive.push_back('x');
		e.archive.push_back('y');
		unpack_equals("short trailing junk is ignored", e);
		e.archive.insert(e.archive.end(), 30, 'z');
		unpack_equals("long trailing junk that is not a track is ignored", e);
	}
}

static void test_errors()
{
	std::vector<uint8_t> adf;
	std::vector<uint8_t> cyl = make_cylinder(101, 0);
	Enc good;
	good.track(0, 1, 0, cyl);
	const std::vector<uint8_t> &g = good.archive;

	check_res("an empty file is not DMS", dms_unpack(g.data(), 0, adf), DMS_ERR_NOT_DMS);
	{
		std::vector<uint8_t> a = g; a[0] = 'X';
		check_res("bad magic is not DMS", dms_unpack(a.data(), a.size(), adf), DMS_ERR_NOT_DMS);
	}
	check_res("a cut header is truncated", dms_unpack(g.data(), 30, adf), DMS_ERR_TRUNCATED);
	{
		std::vector<uint8_t> a = g; a[20] ^= 1;
		check_res("archive header CRC", dms_unpack(a.data(), a.size(), adf), DMS_ERR_HEADER_CRC);
	}
	{
		std::vector<uint8_t> a = g; a[HEADER_LEN + 5] ^= 1;
		check_res("track header CRC", dms_unpack(a.data(), a.size(), adf), DMS_ERR_HEADER_CRC);
	}
	{
		std::vector<uint8_t> a = g; a[HEADER_LEN + TRACK_HEADER_LEN + 7] ^= 1;
		check_res("data CRC catches a flipped packed byte", dms_unpack(a.data(), a.size(), adf), DMS_ERR_DATA_CRC);
	}
	check_res("a cut track is truncated", dms_unpack(g.data(), g.size() - 1, adf), DMS_ERR_TRUNCATED);
	{
		std::vector<uint8_t> a = archive_header(0x02);
		a.insert(a.end(), g.begin() + HEADER_LEN, g.end());
		check_res("encrypted archives are refused", dms_unpack(a.data(), a.size(), adf), DMS_ERR_ENCRYPTED);
	}
	{
		std::vector<uint8_t> a = archive_header(0, 7);
		check_res("FMS files are refused", dms_unpack(a.data(), a.size(), adf), DMS_ERR_FMS);
	}
	{
		std::vector<uint8_t> a = archive_header();
		check_res("an archive with no data tracks", dms_unpack(a.data(), a.size(), adf), DMS_ERR_NO_TRACKS);
	}
	{
		std::vector<uint8_t> a = archive_header();
		std::vector<uint8_t> wrong = cyl; wrong[100] ^= 0xff;   // checksum of other data
		std::vector<uint8_t> packed = rle_encode(cyl);
		add_track(a, 0, 1, 0, packed, 0, wrong);
		check_res("checksum catches a wrong decode", dms_unpack(a.data(), a.size(), adf), DMS_ERR_CHECKSUM);
	}
	{
		std::vector<uint8_t> a = archive_header();
		std::vector<uint8_t> hd(2 * DMS_CYL_SIZE, 0);
		add_track(a, 0, 0, 0, hd, 0, hd);
		check_res("HD tracks are refused", dms_unpack(a.data(), a.size(), adf), DMS_ERR_NOT_DD);
	}
	{
		std::vector<uint8_t> a = archive_header();
		add_track(a, 0, 7, 0, cyl, 0, cyl);
		check_res("mode 7 is unknown", dms_unpack(a.data(), a.size(), adf), DMS_ERR_UNKNOWN_MODE);
	}
	{
		// A HEAVY tree whose lengths leave half the code space unassigned.
		// (An OVER-full one is not an error: make_table stops once the table
		// is full and never looks at the symbols left over, in the reference
		// too, so that is accepted and the checksum is what catches it.)
		BitWriter w;
		w.put(2, 9); w.put(2, 5); w.put(2, 5);
		w.put(0, 5); w.put(0, 5);
		std::vector<uint8_t> a = archive_header();
		add_track(a, 0, 5, 2, w.finish(), DMS_CYL_SIZE, cyl);
		check_res("an incomplete HEAVY tree is corrupt", dms_unpack(a.data(), a.size(), adf), DMS_ERR_BAD_DATA);
	}
	{
		// Garbage through every decoder: an error or an image, never a hang
		// or a fault. The checksum turns a wrong decode into an error.
		rng_state = 7;
		bool all_errors = true;
		for (int i = 0; i < 300; i++)
		{
			std::vector<uint8_t> junk(1 + rnd() % 3000);
			for (auto &b : junk) b = (uint8_t)rnd();
			std::vector<uint8_t> a = archive_header();
			add_track(a, 0, (uint8_t)(2 + i % 5), (uint8_t)(rnd() & 7), junk, DMS_CYL_SIZE + rnd() % 2000, cyl);
			if (dms_unpack(a.data(), a.size(), adf) == DMS_OK) all_errors = false;
		}
		check("random packed data is rejected, not crashed on", all_errors);
	}
	{
		int bad = 0;
		std::vector<uint8_t> a = archive_header();
		add_track(a, 3, 7, 0, cyl, 0, cyl);
		dms_unpack(a.data(), a.size(), adf, &bad);
		check("the failing track is reported", bad == 3);
		check("a failed unpack leaves no image", adf.empty());
	}
}

int main(void)
{
	build_medium_codes();
	build_deep_codes();

	test_rle_fixture();
	test_tables();
	test_modes();
	test_layout();
	test_errors();

	if (failures) { printf("\n%d FAILURE(S)\n", failures); return 1; }
	printf("\nall dms checks passed\n");
	return 0;
}
