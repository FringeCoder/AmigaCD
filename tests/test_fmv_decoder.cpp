// The CD32 FMV module's video decoder (support/minimig/fmv_decoder.cpp):
// libmpeg2 on its own thread, writing the CL450's output window into the
// four frame buffers (docs/fmv-contract.md v5).
//
// data/fmv_colours.m1v is generated, not taken from anything: three red,
// three green and three blue 352x288 pictures, coded I B B I B B I B P so
// that display order differs from decode order. Made with
//   ffmpeg -f lavfi -i "color=c=red:s=352x288:r=25:d=0.12,format=yuv420p[a];
//     color=c=lime:...[b];color=c=blue:...[c];[a][b][c]concat=n=3:v=1:a=0"
//     -c:v mpeg1video -b:v 200k -bf 2 -g 9 -f mpeg1video fmv_colours.m1v

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../support/minimig/fmv_decoder.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static void eq(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld, want %ld\n", what, got, want);
		failures++;
	}
}

static std::vector<uint8_t> load(const char *path)
{
	std::vector<uint8_t> v;
	FILE *f = fopen(path, "rb");
	if (!f)
		return v;
	uint8_t b[4096];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0)
		v.insert(v.end(), b, b + n);
	fclose(f);
	return v;
}

static std::vector<uint8_t> fb((size_t)FMV_FB_BUFS * FMV_FB_BUF_BYTES);

// 'R', 'G', 'B' or '?' for pixel (x, y) of buffer b.
static char colour(int b, int x, int y)
{
	const uint8_t *p = fb.data() + (size_t)b * FMV_FB_BUF_BYTES + (size_t)y * FMV_FB_STRIDE + 2 * x;
	uint16_t v = (uint16_t)(p[0] | p[1] << 8);
	int r = v >> 11, g = (v >> 5) & 63, bl = v & 31;
	if (r > 24 && g < 8 && bl < 4) return 'R';
	if (g > 48 && r < 4 && bl < 4) return 'G';
	if (bl > 24 && r < 4 && g < 8) return 'B';
	return '?';
}

static fmv_window full_window()
{
	fmv_window w = { 0, 0, 352, 288, 60, 18 };
	return w;
}

static void feed(fmv_decoder *d, const std::vector<uint8_t> &es, size_t from, size_t to)
{
	// two bytes at a time, as the model hands them over, polled now and then
	for (size_t i = from; i < to; i += 2) {
		fmv_decoder_bytes(d, es.data() + i, i + 1 < to ? 2 : 1);
		if ((i & 1023) == 0)
			fmv_decoder_poll(d);
	}
	fmv_decoder_poll(d);
}

// Present until nothing new comes; the colours of what was named, in order.
static std::string present_all(fmv_decoder *d, int x = 176, int y = 144)
{
	std::string seen;
	for (int i = 0; i < 40; i++) {
		fmv_decoder_drain(d);
		int b = fmv_decoder_present(d);
		if (b < 0) {
			fmv_decoder_drain(d);
			b = fmv_decoder_present(d);
			if (b < 0)
				break;
		}
		seen += colour(b, x, y);
	}
	return seen;
}

static void test_stream(const std::vector<uint8_t> &es)
{
	memset(fb.data(), 0, fb.size());
	fmv_decoder *d = fmv_decoder_new(fb.data(), -1);
	check(d != 0, "the decoder thread starts");
	if (!d)
		return;
	fmv_decoder_set_window(d, full_window());
	feed(d, es, 0, es.size());
	fmv_decoder_drain(d);

	fmv_decoder_stats s;
	fmv_decoder_get_stats(d, &s);
	eq("every byte handed over", s.bytes_in, (long)es.size());
	eq("one sequence header", s.sequences, 1);
	eq("sequence width", s.width, 352);
	eq("sequence height", s.height, 288);
	eq("with nothing presented, the decoder stops at four buffers", s.pictures, 4);

	std::string seen = present_all(d);
	check(seen == "RRRGGGBBB", "the pictures, in display order, colours right");
	if (seen != "RRRGGGBBB")
		printf("      got \"%s\"\n", seen.c_str());
	fmv_decoder_get_stats(d, &s);
	eq("all nine decoded", s.pictures, 9);
	eq("all nine presented", s.presented, 9);
	check(s.late > 0, "a present with nothing new is counted");
	eq("nothing dropped", s.dropped_bytes, 0);

	// The window: a crop and a position change the buffer layout only by
	// the crop. Top-left pixel of the window = picture (src_x, src_y).
	fmv_window w = { 8, 4, 64, 16, 0, 0 };
	fmv_decoder_flush(d);
	memset(fb.data(), 0xAA, fb.size());
	fmv_decoder_set_window(d, w);
	feed(d, es, 0, es.size());
	std::string seen2 = present_all(d, 63, 15);
	check(seen2 == "RRRGGGBBB", "a cropped window: the same pictures, to its last pixel");
	// and nothing outside it is written: the buffer holds the window only
	bool clean = true;
	for (int b = 0; b < FMV_FB_BUFS; b++) {
		const uint8_t *q = fb.data() + (size_t)b * FMV_FB_BUF_BYTES;
		clean &= q[2 * 64] == 0xAA && q[2 * 64 + 1] == 0xAA;          // right of row 0
		clean &= q[16 * FMV_FB_STRIDE] == 0xAA;                       // below the last row
	}
	check(clean, "a cropped window: nothing written outside it");
	fmv_decoder_free(d);
}

static void test_flush(const std::vector<uint8_t> &es)
{
	memset(fb.data(), 0, fb.size());
	fmv_decoder *d = fmv_decoder_new(fb.data(), -1);
	fmv_decoder_set_window(d, full_window());
	// The whole stream: four pictures decoded, the decoder waits for a
	// buffer. Then the stream again, which waits in the ring behind it.
	// A flush drops both: what is decoded and what is queued.
	feed(d, es, 0, es.size());
	fmv_decoder_drain(d);
	feed(d, es, 0, es.size());
	fmv_decoder_flush(d);
	fmv_decoder_drain(d);
	eq("after a flush nothing old is presented", fmv_decoder_present(d), -1);
	// The whole stream again, from its sequence header
	feed(d, es, 0, es.size());
	std::string seen = present_all(d);
	check(seen == "RRRGGGBBB", "after a flush the stream decodes from the start again");
	if (seen != "RRRGGGBBB")
		printf("      got \"%s\"\n", seen.c_str());
	fmv_decoder_stats s;
	fmv_decoder_get_stats(d, &s);
	eq("one flush counted", s.flushes, 1);
	eq("after a flush: the queued copy is gone, only the new one shows", s.presented, 9);
	fmv_decoder_free(d);
}

static void test_shown_not_overwritten(const std::vector<uint8_t> &es)
{
	// The buffer the core shows, and the one it may still show until the
	// next VSYNC, are never written (contract v5). The stream goes in in
	// thirds, one present between, so the decoder runs out of free buffers
	// while a picture is shown.
	memset(fb.data(), 0, fb.size());
	fmv_decoder *d = fmv_decoder_new(fb.data(), -1);
	fmv_decoder_set_window(d, full_window());
	size_t third = (es.size() / 3) & ~(size_t)1;
	bool ok = true;
	int prev = -1, cur = -1;
	char pc = 0, cc = 0;
	for (int k = 0; k < 3; k++) {
		feed(d, es, k * third, k == 2 ? es.size() : (k + 1) * third);
		fmv_decoder_drain(d);
		if (cur >= 0) ok &= colour(cur, 176, 144) == cc;
		if (prev >= 0) ok &= colour(prev, 176, 144) == pc;
		int b = fmv_decoder_present(d);
		if (b >= 0) {
			prev = cur; pc = cc;
			cur = b; cc = colour(b, 176, 144);
		}
	}
	for (int k = 0; k < 8; k++) {
		fmv_decoder_drain(d);
		if (cur >= 0) ok &= colour(cur, 176, 144) == cc;
		if (prev >= 0) ok &= colour(prev, 176, 144) == pc;
		int b = fmv_decoder_present(d);
		if (b >= 0) { prev = cur; pc = cc; cur = b; cc = colour(b, 176, 144); }
	}
	check(ok, "the shown and the retiring buffer keep their pixels while the decoder runs");
	fmv_decoder_free(d);
}

int main()
{
	std::vector<uint8_t> es = load("data/fmv_colours.m1v");
	check(es.size() > 1000, "data/fmv_colours.m1v loads");
	// ffmpeg writes no sequence_end_code. A disc's stream has one, and
	// without it libmpeg2, like the CL450, cannot finish the last pictures:
	// a picture is complete only once a later start code arrives.
	const uint8_t end[] = { 0, 0, 1, 0xB7 };
	es.insert(es.end(), end, end + sizeof end);
	if (es.size() & 1)
		es.push_back(0);
	if (es.size() > 1000) {
		test_stream(es);
		test_flush(es);
		test_shown_not_overwritten(es);
	}
	if (failures) {
		printf("test_fmv_decoder: %d FAILED\n", failures);
		return 1;
	}
	printf("test_fmv_decoder: OK\n");
	return 0;
}
