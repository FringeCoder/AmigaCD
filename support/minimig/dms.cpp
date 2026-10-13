// DMS (Disk Masher System) archive -> ADF image.
//
// A C++ port of xdms-rs 0.2.0 (https://github.com/mlund/xdms-rs, MIT OR
// Apache-2.0, Copyright (c) Mikael Lund), which is in turn a port of xDMS, the
// public-domain DMS unpacker by Andre Rodrigues de la Rocha, maintained by
// Heikki Orsila. The decode loops follow the Rust closely so the two can be
// read side by side; the module layout does not.
//
// MIT licence text, as the port requires:
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// Where this differs from the reference, and why:
//
// - Tracks are placed by their track number, not appended in archive order.
//   xDMS writes an ADF for a tool to keep; here the image is a floppy drive's
//   contents, so a cylinder must sit where the drive will seek for it. For a
//   whole-disk archive (cylinders 0..79 in order, nearly all of them) the
//   output is byte-identical.
// - Encrypted archives are refused: there is no way to type a password into
//   a file browser.
// - Fewer than 20 bytes after the last track end the archive, rather than
//   failing it as truncated. Short trailing junk is not worth refusing a disk
//   over; a short *track* (header present, data cut off) is still an error.
// - The HEAVY tree walks and bit consumption are bounded. The reference trusts
//   the stream there, and a corrupt one could walk forever or underflow the
//   bit count. Valid streams never reach either bound.

#include "dms.h"

#include <string.h>

namespace {

const size_t HEADER_LEN       = 56;
const size_t TRACK_HEADER_LEN = 20;
const size_t MAX_TRACK_LEN    = 32000;
const uint16_t TRACK_FILE_ID  = 80;   // FILEID.DIZ; data tracks are below it
const uint16_t MIN_DATA_LEN   = 2048; // smaller "track 0"s are fake bootblocks

// ---------------------------------------------------------------------------
// Integrity: CRC-16/ARC (reflected 0xA001, init 0) and an additive checksum.

uint16_t crc16(const uint8_t *p, size_t n)
{
	static uint16_t table[256];
	static bool built = false;
	if (!built)
	{
		for (int i = 0; i < 256; i++)
		{
			uint16_t c = (uint16_t)i;
			for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
			table[i] = c;
		}
		built = true;
	}
	uint16_t crc = 0;
	while (n--) crc = (uint16_t)(table[(crc ^ *p++) & 0xff] ^ (crc >> 8));
	return crc;
}

uint16_t checksum16(const uint8_t *p, size_t n)
{
	uint16_t sum = 0;
	while (n--) sum = (uint16_t)(sum + *p++);
	return sum;
}

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// ---------------------------------------------------------------------------
// MSB-first bit reader. Holds at least 16 valid bits after every consume, and
// reads zeroes past the end of the data -- the C decoders run into the zeroed
// tail of their buffer and depend on it.

struct BitReader
{
	const uint8_t *data;
	size_t len, pos;
	uint32_t buf, count;
	bool overrun;

	BitReader(const uint8_t *d, size_t n) : data(d), len(n), pos(0), buf(0), count(0), overrun(false) { refill(); }

	void refill()
	{
		while (count < 16)
		{
			uint32_t byte = (pos < len) ? data[pos] : 0;
			pos++;
			buf = (buf << 8) | byte;
			count += 8;
		}
	}

	uint16_t peek(uint32_t n) const { return (uint16_t)(buf >> (count - n)); }

	void consume(uint32_t n)
	{
		// Only a corrupt stream asks for more bits than are buffered (a code
		// length shorter than the table it overflowed). Flag it, don't wrap.
		if (n > count) { overrun = true; n = count; }
		count -= n;
		buf &= (1u << count) - 1;
		refill();
	}

	uint16_t read(uint32_t n) { uint16_t v = peek(n); consume(n); return v; }
};

// ---------------------------------------------------------------------------
// Static distance tables shared by MEDIUM and DEEP (tables.c).

const uint8_t D_CODE[256] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
	0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
	0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
	0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
	0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F,
	0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
	0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x17,
	0x18, 0x18, 0x19, 0x19, 0x1A, 0x1A, 0x1B, 0x1B, 0x1C, 0x1C, 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F,
	0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
	0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
};

const uint8_t D_LEN[256] = {
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
	0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
	0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
	0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
	0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
	0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
	0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
	0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
	0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
	0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
	0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
	0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
	0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
	0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
};

// ---------------------------------------------------------------------------
// RLE: the second stage of most modes, and the whole of SIMPLE.

bool unpack_rle(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
	size_t ip = 0, op = 0;
#define RLE_NEXT(v) do { if (ip >= in_len) return false; (v) = in[ip++]; } while (0)
	while (op < out_len)
	{
		uint8_t byte, run, value;
		RLE_NEXT(byte);
		if (byte != 0x90) { out[op++] = byte; continue; }
		RLE_NEXT(run);
		if (run == 0) { out[op++] = 0x90; continue; }   // 0x90 0x00: a literal 0x90
		RLE_NEXT(value);
		size_t count = run;
		if (run == 0xff)
		{
			uint8_t hi, lo;
			RLE_NEXT(hi);
			RLE_NEXT(lo);
			count = ((size_t)hi << 8) | lo;
		}
		if (count > out_len - op) return false;
		memset(out + op, value, count);
		op += count;
	}
#undef RLE_NEXT
	return true;
}

// ---------------------------------------------------------------------------
// The LZ-family decoders and the window state they share across tracks.

const size_t WINDOW_SIZE  = 0x4000;
const size_t WINDOW_CLEAR = 0x3fc8;   // the C leaves the tail alone on reset

// Each LZ window starts its lookahead (max match length F) short of its end,
// and advances by F at the end of every track, as the encoder does.
const uint16_t QUICK_INIT_POS  = 251;    // 256 - 5
const uint16_t MEDIUM_INIT_POS = 0x3fbe; // 0x4000 - 66
const uint16_t DEEP_INIT_POS   = 0x3fc4; // 0x4000 - 60

const int HEAVY_NC  = 510;
const int HEAVY_NPT = 20;
const uint16_t HEAVY_OFFSET = 253;       // code 256 = a match of length 3

const int DEEP_N_CHAR = 256 - 2 + 60;
const int DEEP_T      = DEEP_N_CHAR * 2 - 1;
const int DEEP_R      = DEEP_T - 1;

struct Decoder
{
	uint8_t window[WINDOW_SIZE];
	uint16_t quick_pos, medium_pos, deep_pos, heavy_pos, heavy_last;

	uint8_t  c_len[HEAVY_NC];
	uint8_t  pt_len[HEAVY_NPT];
	uint16_t c_table[4096];
	uint16_t pt_table[256];
	uint16_t left[2 * HEAVY_NC - 1];
	uint16_t right[2 * HEAVY_NC - 1 + 9];

	uint16_t deep_freq[DEEP_T + 1];
	uint16_t deep_prnt[DEEP_T + DEEP_N_CHAR];
	uint16_t deep_son[DEEP_T];
	bool deep_init;

	Decoder() { memset(this, 0, sizeof(*this)); reset(); }

	// The C Init_Decrunchers. Called between tracks unless one keeps state.
	void reset()
	{
		quick_pos = QUICK_INIT_POS;
		medium_pos = MEDIUM_INIT_POS;
		deep_pos = DEEP_INIT_POS;
		heavy_pos = 0;
		heavy_last = 0;
		deep_init = true;
		memset(window, 0, WINDOW_CLEAR);
	}

	void push(uint16_t &pos, uint16_t mask, uint8_t byte)
	{
		window[pos & mask] = byte;
		pos++;
	}

	bool copy_match(uint16_t &pos, uint16_t mask, uint16_t distance, uint16_t length,
	                uint8_t *out, size_t out_len, size_t &op)
	{
		uint16_t src = (uint16_t)(pos - distance - 1);
		for (uint16_t i = 0; i < length; i++)
		{
			uint8_t byte = window[src & mask];
			push(pos, mask, byte);
			src++;
			if (op >= out_len) return false;
			out[op++] = byte;
		}
		return true;
	}

	// QUICK: a flag bit, then a literal or a short match in a 256-byte window.
	bool unpack_quick(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
	{
		const uint16_t mask = 0xff;
		BitReader bits(in, in_len);
		size_t op = 0;
		while (op < out_len)
		{
			if (bits.read(1))
			{
				uint8_t byte = (uint8_t)bits.read(8);
				push(quick_pos, mask, byte);
				out[op++] = byte;
			}
			else
			{
				uint16_t length = (uint16_t)(bits.read(2) + 2);
				uint16_t distance = bits.read(8);
				if (!copy_match(quick_pos, mask, distance, length, out, out_len, op)) return false;
			}
		}
		quick_pos = (uint16_t)((quick_pos + 5) & mask);
		return true;
	}

	// MEDIUM: LZ with match distances through the static D_CODE/D_LEN tables.
	bool unpack_medium(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
	{
		const uint16_t mask = 0x3fff;
		BitReader bits(in, in_len);
		size_t op = 0;
		while (op < out_len)
		{
			if (bits.read(1))
			{
				uint8_t byte = (uint8_t)bits.read(8);
				push(medium_pos, mask, byte);
				out[op++] = byte;
			}
			else
			{
				uint32_t prefix = bits.read(8) & 0xff;
				uint16_t length = (uint16_t)(D_CODE[prefix] + 3);
				uint32_t extra = D_LEN[prefix];
				uint32_t mid = ((prefix << extra) | bits.read(extra)) & 0xff;
				extra = D_LEN[mid];
				uint32_t low = ((mid << extra) | bits.read(extra)) & 0xff;
				uint16_t distance = (uint16_t)((D_CODE[mid] << 8) | low);
				if (!copy_match(medium_pos, mask, distance, length, out, out_len, op)) return false;
			}
		}
		medium_pos = (uint16_t)((medium_pos + 66) & mask);
		return true;
	}

	// DEEP: LZ with an adaptive Huffman tree (Okumura's LZHUF).
	void deep_init_tree()
	{
		for (int i = 0; i < DEEP_N_CHAR; i++)
		{
			deep_freq[i] = 1;
			deep_son[i] = (uint16_t)(i + DEEP_T);
			deep_prnt[i + DEEP_T] = (uint16_t)i;
		}
		for (int i = 0, j = DEEP_N_CHAR; j <= DEEP_R; i += 2, j++)
		{
			deep_freq[j] = (uint16_t)(deep_freq[i] + deep_freq[i + 1]);
			deep_son[j] = (uint16_t)i;
			deep_prnt[i] = (uint16_t)j;
			deep_prnt[i + 1] = (uint16_t)j;
		}
		deep_freq[DEEP_T] = 0xffff;
		deep_prnt[DEEP_R] = 0;
		deep_init = false;
	}

	void deep_reconstruct()
	{
		int j = 0;
		for (int i = 0; i < DEEP_T; i++)
		{
			if (deep_son[i] >= DEEP_T)
			{
				deep_freq[j] = (uint16_t)((deep_freq[i] + 1) / 2);
				deep_son[j] = deep_son[i];
				j++;
			}
		}
		for (int i = 0, jj = DEEP_N_CHAR; jj < DEEP_T; i += 2, jj++)
		{
			uint16_t f = (uint16_t)(deep_freq[i] + deep_freq[i + 1]);
			deep_freq[jj] = f;
			int k = jj - 1;
			while (f < deep_freq[k]) k--;
			k++;
			memmove(&deep_freq[k + 1], &deep_freq[k], (jj - k) * sizeof(deep_freq[0]));
			deep_freq[k] = f;
			memmove(&deep_son[k + 1], &deep_son[k], (jj - k) * sizeof(deep_son[0]));
			deep_son[k] = (uint16_t)i;
		}
		for (int i = 0; i < DEEP_T; i++)
		{
			int k = deep_son[i];
			deep_prnt[k] = (uint16_t)i;
			if (k < DEEP_T) deep_prnt[k + 1] = (uint16_t)i;
		}
	}

	void deep_update(uint16_t symbol)
	{
		if (deep_freq[DEEP_R] == 0x8000) deep_reconstruct();
		uint16_t c = deep_prnt[symbol + DEEP_T];
		do
		{
			uint16_t k = ++deep_freq[c];
			uint16_t l = (uint16_t)(c + 1);
			if (k > deep_freq[l])
			{
				while (k > deep_freq[++l]) {}
				l--;
				deep_freq[c] = deep_freq[l];
				deep_freq[l] = k;

				uint16_t i = deep_son[c];
				deep_prnt[i] = l;
				if (i < DEEP_T) deep_prnt[i + 1] = l;

				uint16_t j = deep_son[l];
				deep_son[l] = i;
				deep_prnt[j] = c;
				if (j < DEEP_T) deep_prnt[j + 1] = c;
				deep_son[c] = j;

				c = l;
			}
			c = deep_prnt[c];
		} while (c != 0);
	}

	uint16_t deep_decode_char(BitReader &bits)
	{
		uint16_t node = deep_son[DEEP_R];
		while (node < DEEP_T) node = deep_son[node + bits.read(1)];
		uint16_t symbol = (uint16_t)(node - DEEP_T);
		deep_update(symbol);
		return symbol;
	}

	bool unpack_deep(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
	{
		const uint16_t mask = 0x3fff;
		BitReader bits(in, in_len);
		if (deep_init) deep_init_tree();
		size_t op = 0;
		while (op < out_len)
		{
			uint16_t code = deep_decode_char(bits);
			if (code < 256)
			{
				push(deep_pos, mask, (uint8_t)code);
				out[op++] = (uint8_t)code;
			}
			else
			{
				uint16_t length = (uint16_t)(code - 253);
				uint32_t prefix = bits.read(8) & 0xff;
				uint16_t high = (uint16_t)(D_CODE[prefix] << 8);
				uint32_t extra = D_LEN[prefix];
				uint16_t low = (uint16_t)(((prefix << extra) | bits.read(extra)) & 0xff);
				if (!copy_match(deep_pos, mask, (uint16_t)(high | low), length, out, out_len, op)) return false;
			}
		}
		deep_pos = (uint16_t)((deep_pos + 60) & mask);
		return true;
	}

	// HEAVY1/HEAVY2: LZH with static Huffman trees (LHA's maketbl).
	struct TableBuilder
	{
		const uint8_t *blen;
		uint16_t *table, *left, *right;
		uint16_t n, avail, table_size, bit, max_depth, depth, len;
		int c;
		uint16_t codeword;
		bool bad;

		uint16_t build()
		{
			uint16_t node = 0;
			if (bad) return 0;
			if (len == depth)
			{
				while (++c < n)
				{
					if (blen[c] == len)
					{
						uint16_t start = codeword;
						codeword = (uint16_t)(codeword + bit);
						if (codeword > table_size) { bad = true; return 0; }
						for (uint16_t i = start; i < codeword; i++) table[i] = (uint16_t)c;
						return (uint16_t)c;
					}
				}
				c = -1;
				len++;
				bit >>= 1;
			}
			depth++;
			if (depth < max_depth)
			{
				build();
				build();
			}
			else if (depth > 32)
			{
				bad = true;
				return 0;
			}
			else
			{
				node = avail++;
				if (node >= 2 * n - 1) { bad = true; return 0; }
				left[node] = build();
				right[node] = build();
				if (bad) return 0;
				if (codeword >= table_size) { bad = true; return 0; }
				if (depth == max_depth) table[codeword++] = node;
			}
			depth--;
			return node;
		}
	};

	bool make_table(uint16_t nchar, const uint8_t *bitlen, uint16_t tablebits, uint16_t *table)
	{
		TableBuilder b;
		b.blen = bitlen;
		b.table = table;
		b.left = left;
		b.right = right;
		b.n = nchar;
		b.avail = nchar;
		b.table_size = (uint16_t)(1u << tablebits);
		b.bit = (uint16_t)(b.table_size >> 1);
		b.max_depth = (uint16_t)(tablebits + 1);
		b.depth = 1;
		b.len = 1;
		b.c = -1;
		b.codeword = 0;
		b.bad = false;
		b.build();   // left subtree
		b.build();   // right subtree
		return !b.bad && b.codeword == b.table_size;
	}

	bool read_tree_c(BitReader &bits)
	{
		uint16_t count = bits.read(9);
		if (count > 0)
		{
			if (count > HEAVY_NC) return false;
			for (int i = 0; i < count; i++) c_len[i] = (uint8_t)bits.read(5);
			memset(c_len + count, 0, HEAVY_NC - count);
			return make_table(HEAVY_NC, c_len, 12, c_table);
		}
		// No tree: every code is the one symbol that follows.
		uint16_t symbol = bits.read(9);
		memset(c_len, 0, sizeof(c_len));
		for (int i = 0; i < 4096; i++) c_table[i] = symbol;
		return true;
	}

	bool read_tree_p(BitReader &bits, uint16_t np)
	{
		uint16_t count = bits.read(5);
		if (count > 0)
		{
			if (count > HEAVY_NPT) return false;
			for (int i = 0; i < count; i++) pt_len[i] = (uint8_t)bits.read(4);
			memset(pt_len + count, 0, HEAVY_NPT - count);
			return make_table(np, pt_len, 8, pt_table);
		}
		uint16_t symbol = bits.read(5);
		memset(pt_len, 0, sizeof(pt_len));
		for (int i = 0; i < 256; i++) pt_table[i] = symbol;
		return true;
	}

	// Walks left/right from an internal node for a code longer than the table.
	// Bounded at 16 steps: a valid code is resolved within the 16 bits peeked.
	bool walk(uint16_t &node, uint16_t limit, uint16_t path)
	{
		for (uint16_t probe = 0x8000; probe; probe >>= 1)
		{
			if (node >= sizeof(left) / sizeof(left[0])) return false;
			node = (path & probe) ? right[node] : left[node];
			if (node < limit) return true;
		}
		return false;
	}

	bool decode_c(BitReader &bits, uint16_t &code)
	{
		uint16_t node = c_table[bits.peek(12) & 0xfff];
		if (node < HEAVY_NC)
		{
			bits.consume(c_len[node]);
		}
		else
		{
			bits.consume(12);
			if (!walk(node, HEAVY_NC, bits.peek(16))) return false;
			if (c_len[node] < 12) return false;
			bits.consume(c_len[node] - 12u);
		}
		code = node;
		return !bits.overrun;
	}

	bool decode_p(BitReader &bits, uint16_t np, uint16_t &distance)
	{
		uint16_t node = pt_table[bits.peek(8) & 0xff];
		if (node < np)
		{
			bits.consume(pt_len[node]);
		}
		else
		{
			bits.consume(8);
			if (!walk(node, np, bits.peek(16))) return false;
			if (pt_len[node] < 8) return false;
			bits.consume(pt_len[node] - 8u);
		}
		// The last code repeats the previous distance; the others below it
		// are an exponent, extended with that many minus one raw bits.
		if (node != np - 1)
		{
			if (node > 0)
			{
				uint16_t extra = (uint16_t)(node - 1);
				node = (uint16_t)(bits.peek(extra) | (1u << extra));
				bits.consume(extra);
			}
			heavy_last = node;
		}
		distance = heavy_last;
		return !bits.overrun;
	}

	bool unpack_heavy(bool big_dict, bool rebuild, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
	{
		const uint16_t np = big_dict ? 15 : 14;
		const uint16_t mask = big_dict ? 0x1fff : 0x0fff;
		BitReader bits(in, in_len);

		if (rebuild && (!read_tree_c(bits) || !read_tree_p(bits, np))) return false;

		size_t op = 0;
		while (op < out_len)
		{
			uint16_t code;
			if (!decode_c(bits, code)) return false;
			if (code < 256)
			{
				push(heavy_pos, mask, (uint8_t)code);
				out[op++] = (uint8_t)code;
			}
			else
			{
				uint16_t distance;
				if (!decode_p(bits, np, distance)) return false;
				if (!copy_match(heavy_pos, mask, distance, (uint16_t)(code - HEAVY_OFFSET), out, out_len, op)) return false;
			}
		}
		return true;
	}

	// One track. `scratch` holds the first stage of the two-stage modes.
	bool unpack_track(uint8_t mode, uint8_t flags, const uint8_t *in, size_t in_len,
	                  size_t mid_len, uint8_t *out, size_t out_len, std::vector<uint8_t> &scratch)
	{
		if (mode == 0)
		{
			if (in_len < out_len) return false;
			memcpy(out, in, out_len);
			return true;
		}
		if (mode == 1) return unpack_rle(in, in_len, out, out_len);

		scratch.assign(mid_len, 0);
		uint8_t *mid = scratch.data();
		bool ok;
		switch (mode)
		{
		case 2: ok = unpack_quick(in, in_len, mid, mid_len); break;
		case 3: ok = unpack_medium(in, in_len, mid, mid_len); break;
		case 4: ok = unpack_deep(in, in_len, mid, mid_len); break;
		case 5:
		case 6:
			ok = unpack_heavy(mode == 6, (flags & 0x02) != 0, in, in_len, mid, mid_len);
			if (ok && !(flags & 0x04))
			{
				// HEAVY without its RLE pass: the first stage is the output.
				if (mid_len < out_len) return false;
				memcpy(out, mid, out_len);
				return true;
			}
			break;
		default:
			return false;
		}
		return ok && unpack_rle(mid, mid_len, out, out_len);
	}
};

} // namespace

dms_result dms_unpack(const uint8_t *dms, size_t len, std::vector<uint8_t> &adf, int *bad_track)
{
	int dummy;
	if (!bad_track) bad_track = &dummy;
	*bad_track = -1;
	adf.clear();

	if (len < 4 || memcmp(dms, "DMS!", 4)) return DMS_ERR_NOT_DMS;
	if (len < HEADER_LEN) return DMS_ERR_TRUNCATED;
	// The header CRC covers everything between the magic and the CRC itself.
	if (crc16(dms + 4, HEADER_LEN - 6) != be16(dms + HEADER_LEN - 2)) return DMS_ERR_HEADER_CRC;
	uint16_t geninfo = be16(dms + 10);
	uint16_t disktype = be16(dms + 50);
	if (disktype == 7) return DMS_ERR_FMS;
	if (geninfo & 0x02) return DMS_ERR_ENCRYPTED;

	Decoder *dec = new Decoder();
	std::vector<uint8_t> scratch;
	adf.assign(DMS_ADF_SIZE, 0);
	size_t pos = HEADER_LEN;
	int tracks = 0;
	dms_result res = DMS_OK;

	while (len - pos >= TRACK_HEADER_LEN)
	{
		const uint8_t *th = dms + pos;
		// Anything that is not a track header after the last track is junk
		// some tools append. It ends the archive; it does not fail it.
		if (th[0] != 'T' || th[1] != 'R') break;
		if (crc16(th, TRACK_HEADER_LEN - 2) != be16(th + TRACK_HEADER_LEN - 2)) { res = DMS_ERR_HEADER_CRC; break; }

		uint16_t number   = be16(th + 2);
		uint16_t pk_len   = be16(th + 6);
		uint16_t mid_len  = be16(th + 8);
		uint16_t unpk_len = be16(th + 10);
		uint8_t  flags    = th[12];
		uint8_t  mode     = th[13];
		uint16_t csum     = be16(th + 14);
		uint16_t dcrc     = be16(th + 16);
		pos += TRACK_HEADER_LEN;

		if (pk_len > MAX_TRACK_LEN || mid_len > MAX_TRACK_LEN || unpk_len > MAX_TRACK_LEN) { res = DMS_ERR_TOO_LARGE; *bad_track = number; break; }
		if (len - pos < pk_len) { res = DMS_ERR_TRUNCATED; *bad_track = number; break; }
		const uint8_t *packed = dms + pos;
		pos += pk_len;

		if (crc16(packed, pk_len) != dcrc) { res = DMS_ERR_DATA_CRC; *bad_track = number; break; }

		// Banner (0xffff), FILEID.DIZ (80) and fake bootblocks are not disk
		// data. They are skipped without touching the decoder state.
		if (number >= TRACK_FILE_ID || unpk_len <= MIN_DATA_LEN) continue;

		if (mode > 6) { res = DMS_ERR_UNKNOWN_MODE; *bad_track = number; break; }
		if (unpk_len != DMS_CYL_SIZE) { res = DMS_ERR_NOT_DD; *bad_track = number; break; }

		uint8_t *out = adf.data() + (size_t)number * DMS_CYL_SIZE;
		memset(out, 0, DMS_CYL_SIZE);   // a duplicate track replaces, never mixes
		if (!dec->unpack_track(mode, flags, packed, pk_len, mid_len, out, unpk_len, scratch)) { res = DMS_ERR_BAD_DATA; *bad_track = number; break; }
		// State carries to the next track only when this one asks for it.
		if (!(flags & 0x01)) dec->reset();

		if (checksum16(out, unpk_len) != csum) { res = DMS_ERR_CHECKSUM; *bad_track = number; break; }
		tracks++;
	}

	delete dec;
	if (res == DMS_OK && !tracks) res = DMS_ERR_NO_TRACKS;
	if (res != DMS_OK) adf.clear();
	return res;
}

const char *dms_strerror(dms_result r)
{
	switch (r)
	{
	case DMS_OK:               return "ok";
	case DMS_ERR_NOT_DMS:      return "not a DMS archive";
	case DMS_ERR_TRUNCATED:    return "archive is truncated";
	case DMS_ERR_HEADER_CRC:   return "header CRC error";
	case DMS_ERR_DATA_CRC:     return "track data CRC error";
	case DMS_ERR_CHECKSUM:     return "track checksum error";
	case DMS_ERR_BAD_DATA:     return "corrupt track data";
	case DMS_ERR_UNKNOWN_MODE: return "unknown compression mode";
	case DMS_ERR_TOO_LARGE:    return "track too large";
	case DMS_ERR_ENCRYPTED:    return "encrypted (password) DMS not supported";
	case DMS_ERR_FMS:          return "FMS file, not a disk image";
	case DMS_ERR_NOT_DD:       return "not a DD disk (HD DMS not supported)";
	case DMS_ERR_NO_TRACKS:    return "no disk tracks in archive";
	}
	return "unknown error";
}
