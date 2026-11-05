// The CD32 FMV module's sound (support/minimig/fmv_audio.cpp, and the
// L64111 model's phase 4 hooks), against docs/fmv-plan.md phase 4.
//
// data/fmv_tone_44k.mp2 and data/fmv_tone_48k.mp2 are generated, not taken
// from anything: MPEG-1 Layer II, 192 kbit/s, made with
//   ffmpeg -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=0.25"
//     -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=0.25"
//     -filter_complex "[0][1]amerge=inputs=2" -c:a mp2 -b:a 192k -f mp2 fmv_tone_44k.mp2
//   ffmpeg -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=0.1"
//     -ac 2 -c:a mp2 -b:a 192k -f mp2 fmv_tone_48k.mp2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../support/minimig/fmv_audio.h"
#include "../support/minimig/fmv_model.h"
#include "fmv_fpga_ref.h"
extern "C" {
#include "kjmp2.h"
}

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

// The stream cut into frames, by kjmp2's own size count.
static std::vector<std::vector<uint8_t>> frames_of(const std::vector<uint8_t> &es)
{
	std::vector<std::vector<uint8_t>> out;
	static kjmp2_context_t k;
	kjmp2_init(&k);
	size_t i = 0;
	while (i + 4 < es.size()) {
		std::vector<uint8_t> pad(es.begin() + i, es.end());
		pad.resize(pad.size() + KJMP2_MAX_FRAME_SIZE, 0);
		unsigned long n = kjmp2_decode_frame(&k, pad.data(), NULL);
		if (!n || i + n > es.size())
			break;
		out.emplace_back(es.begin() + i, es.begin() + i + n);
		i += n;
	}
	return out;
}

// What a continuous decode of the same frames gives: the reference.
static std::vector<int16_t> reference(const std::vector<std::vector<uint8_t>> &fr)
{
	kjmp2_context_t k;
	kjmp2_init(&k);
	std::vector<int16_t> out, pcm(2 * KJMP2_SAMPLES_PER_FRAME);
	for (auto &f : fr) {
		std::vector<uint8_t> pad = f;
		pad.resize(pad.size() + KJMP2_MAX_FRAME_SIZE, 0);
		kjmp2_decode_frame(&k, pad.data(), pcm.data());
		out.insert(out.end(), pcm.begin(), pcm.end());
	}
	return out;
}

static std::vector<int16_t> take_all(fmv_audio *a)
{
	std::vector<int16_t> out;
	int16_t sec[2 * FMV_AUDIO_SECTOR_FRAMES];
	while (fmv_audio_sector(a, sec))
		out.insert(out.end(), sec, sec + 2 * FMV_AUDIO_SECTOR_FRAMES);
	return out;
}

static bool all_zero(const int16_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i]) return false;
	return true;
}

static void test_decode(const std::vector<std::vector<uint8_t>> &fr)
{
	fmv_audio *a = fmv_audio_new(-1);
	check(a != 0, "the audio thread starts");
	if (!a) return;
	for (auto &f : fr)
		fmv_audio_frame(a, f.data(), f.size(), 44100);
	fmv_audio_drain(a);
	for (size_t i = 0; i < fr.size(); i++)
		fmv_audio_tick(a, false);
	eq("each tick adds one frame of output", (long)fmv_audio_pending(a), (long)(fr.size() * 1152));
	std::vector<int16_t> got = take_all(a);
	std::vector<int16_t> want = reference(fr);
	want.resize(got.size());
	check(got.size() == (fr.size() * 1152 / 588) * 588 * 2, "taken in whole 588-frame sectors");
	check(got == want, "the output is the frames' decode, in order, L R interleaved");
	check(!all_zero(got.data(), got.size()), "...and not silence");
	// left 1000 Hz and right 440 Hz: the channels differ
	bool differ = false;
	for (size_t i = 0; i + 1 < got.size(); i += 2) differ |= got[i] != got[i + 1];
	check(differ, "left and right are kept apart");
	fmv_audio_stats s;
	fmv_audio_get_stats(a, &s);
	eq("all decoded", s.decoded, (long)fr.size());
	eq("none late", s.late, 0);
	eq("sectors counted", s.sectors, (long)(fr.size() * 1152 / 588));
	fmv_audio_free(a);
}

static void test_schedule(const std::vector<std::vector<uint8_t>> &fr)
{
	fmv_audio *a = fmv_audio_new(-1);
	// A tick with nothing handed over keeps the time with silence
	fmv_audio_tick(a, false);
	eq("a tick with no frame: 1152 samples of silence", (long)fmv_audio_pending(a), 1152);
	// Muted: the frame is consumed, its time is silence
	fmv_audio_frame(a, fr[0].data(), fr[0].size(), 44100);
	fmv_audio_frame(a, fr[1].data(), fr[1].size(), 44100);
	fmv_audio_drain(a);
	fmv_audio_tick(a, true);
	fmv_audio_tick(a, false);
	std::vector<int16_t> got;
	{
		int16_t sec[2 * FMV_AUDIO_SECTOR_FRAMES];
		while (fmv_audio_pending(a) >= 588 && fmv_audio_sector(a, sec))
			got.insert(got.end(), sec, sec + 2 * 588);
	}
	// samples 0-1151 silence (no frame), 1152-2303 silence (muted), then frame 1
	check(got.size() >= 2 * 2304 && all_zero(got.data(), 2 * 2304),
	      "the muted tick plays its frame as silence");
	check(got.size() > 2 * 2304 && !all_zero(got.data() + 2 * 2304, got.size() - 2 * 2304),
	      "the next frame plays: a mute does not shift the frames after it");
	fmv_audio_stats s;
	fmv_audio_get_stats(a, &s);
	eq("muted ticks counted", s.muted, 1);
	eq("the tick with no frame counted late", s.late, 1);

	// A flush drops frames handed over and not yet played
	size_t left = fmv_audio_pending(a);
	fmv_audio_drop_output(a);
	eq("drop_output empties the output", (long)fmv_audio_pending(a), 0);
	fmv_audio_frame(a, fr[2].data(), fr[2].size(), 44100);
	fmv_audio_drain(a);            // decoded, so only the flush keeps it silent
	fmv_audio_flush(a);
	fmv_audio_tick(a, false);
	std::vector<int16_t> g2 = take_all(a);
	check(fmv_audio_pending(a) + g2.size() / 2 == 1152 && all_zero(g2.data(), g2.size()),
	      "after a flush the tick has no frame: silence, not the dropped one");
	fmv_audio_get_stats(a, &s);
	eq("drop_output counts what it drops", s.dropped, (long)left);
	eq("one flush", s.flushes, 1);
	fmv_audio_free(a);
}

static void test_rate(const std::vector<std::vector<uint8_t>> &fr48)
{
	fmv_audio *a = fmv_audio_new(-1);
	for (auto &f : fr48)
		fmv_audio_frame(a, f.data(), f.size(), 48000);
	fmv_audio_drain(a);
	for (size_t i = 0; i < fr48.size(); i++)
		fmv_audio_tick(a, false);
	std::vector<int16_t> got = take_all(a);
	check(!got.empty() && all_zero(got.data(), got.size()),
	      "48 kHz frames play as silence: the CDDA FIFO is 44.1 kHz");
	fmv_audio_stats s;
	fmv_audio_get_stats(a, &s);
	eq("counted as not 44.1 kHz", s.wrong_rate, (long)fr48.size());
	fmv_audio_free(a);
}

// ---------------------------------------------------- the model's hooks

struct afpga : fmv_fpga_ref {
	fmv_model *model = 0;
	uint32_t ev_count = 0, host_rdidx = 0;
	std::vector<std::vector<uint8_t>> frames;
	std::vector<uint32_t> rates;
	int ticks = 0, flushes = 0;
	void post(uint32_t off, bool read, uint16_t data, uint8_t haddr) override
	{
		fmv_event e;
		memset(&e, 0, sizeof e);
		e.index = ev_count++;
		e.offset = off;
		e.read = read;
		e.uds = e.lds = true;
		e.data = data;
		e.haddr = haddr;
		fmv_model_event(model, e);
		host_rdidx = ev_count;
	}
};

static void h_update(void *ctx, const fmv_update &u) { afpga *f = (afpga *)ctx; f->apply_update(u, f->host_rdidx); }
static void h_dram(void *, uint32_t, uint16_t) {}
static void h_log(void *, const char *) {}
static void a_frame(void *ctx, const uint8_t *p, size_t n, uint32_t rate)
{
	afpga *f = (afpga *)ctx;
	f->frames.emplace_back(p, p + n);
	f->rates.push_back(rate);
}
static void a_tick(void *ctx) { ((afpga *)ctx)->ticks++; }
static void a_flush(void *ctx) { ((afpga *)ctx)->flushes++; }

enum { L64 = FMV_OFF_L64111 };

static std::vector<uint8_t> audio_packet(const std::vector<uint8_t> &payload, uint64_t pts)
{
	std::vector<uint8_t> h = { 0xFF, 0xFF, 0x40, 0x20,
	                           (uint8_t)(0x21 | ((pts >> 29) & 0x0E)), (uint8_t)(pts >> 22),
	                           (uint8_t)((pts >> 14) | 1), (uint8_t)(pts >> 7), (uint8_t)((pts << 1) | 1) };
	size_t len = h.size() + payload.size();
	std::vector<uint8_t> p = { 0, 0, 1, 0xC0, (uint8_t)(len >> 8), (uint8_t)len };
	p.insert(p.end(), h.begin(), h.end());
	p.insert(p.end(), payload.begin(), payload.end());
	return p;
}

static void test_model(const std::vector<std::vector<uint8_t>> &fr)
{
	afpga f;
	fmv_hooks h = { &f, h_update, h_dram, h_log, &f, 0, 0, 0, 0, 0, 0 };
	f.model = fmv_model_new(h);
	fmv_model_set_audio_hooks(f.model, a_frame, a_tick, a_flush);
	fmv_model_reset(f.model);
	int fl0 = f.flushes;
	check(fl0 >= 1, "a board reset flushes the audio");

	// Six frames in system-stream packets, a packet boundary inside a frame
	std::vector<uint8_t> es;
	for (int i = 0; i < 6; i++) es.insert(es.end(), fr[i].begin(), fr[i].end());
	const uint8_t pack[] = { 0, 0, 1, 0xBA, 0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x1B, 0x91 };
	size_t half = es.size() / 2 + 7;
	std::vector<uint8_t> a(es.begin(), es.begin() + half), b(es.begin() + half, es.end());
	for (auto *part : { &a, &b }) {
		for (uint8_t x : pack) f.write(L64, x);
		for (uint8_t x : audio_packet(*part, 90000)) f.write(L64, x);
	}
	eq("audio_frame once per frame", (long)f.frames.size(), 6);
	bool same = f.frames.size() == 6;
	for (size_t i = 0; same && i < 6; i++) same = f.frames[i] == fr[i];
	check(same, "audio_frame: each frame whole, header included, across a packet boundary");
	check(!f.rates.empty() && f.rates[0] == 44100, "audio_frame carries the sample rate");

	// The play schedule: STARTI, then one tick per 1152 samples
	f.write(L64 + 2 * 1, 0x01);
	fmv_model_advance(f.model, 3 * 26122 + 100);
	eq("audio_tick once per frame put out (26.1 ms at 44.1 kHz)", f.ticks, 3);
	fmv_model_state s;
	fmv_model_get_state(f.model, &s);
	eq("ticks match the model's frames out", f.ticks, (long)s.l64_frames_out);

	// Mute: SOFT_MUTE, and the IO word's 0x0200 by byte lane
	check(!fmv_model_audio_muted(f.model), "not muted after reset");
	f.write(L64 + 2 * 2, 0x20);
	check(fmv_model_audio_muted(f.model), "SOFT_MUTE mutes [L64 4-7]");
	f.write(L64 + 2 * 2, 0x00);
	check(!fmv_model_audio_muted(f.model), "SOFT_MUTE cleared");
	f.write(FMV_OFF_IO, 0x0200);
	check(fmv_model_audio_muted(f.model), "IO word 0x0200 mutes");
	fmv_event e;
	memset(&e, 0, sizeof e);
	e.offset = FMV_OFF_IO; e.lds = true; e.data = 0x0000;
	fmv_model_event(f.model, e);
	check(fmv_model_audio_muted(f.model), "a low-byte write does not reach bit 9");
	e.lds = false; e.uds = true;
	fmv_model_event(f.model, e);
	check(!fmv_model_audio_muted(f.model), "a high-byte write clears it");

	// BUF_RST and SOFT_RST empty the channel buffer: flush
	int fl = f.flushes;
	f.write(L64 + 2 * 1, 0x05);
	eq("BUF_RST flushes the audio", f.flushes, fl + 1);
	f.write(L64 + 2 * 1, 0x02);
	check(f.flushes >= fl + 2, "SOFT_RST flushes the audio");
	fmv_model_free(f.model);
}

int main()
{
	std::vector<uint8_t> es44 = load("data/fmv_tone_44k.mp2"), es48 = load("data/fmv_tone_48k.mp2");
	std::vector<std::vector<uint8_t>> fr44 = frames_of(es44), fr48 = frames_of(es48);
	check(fr44.size() >= 8, "data/fmv_tone_44k.mp2: at least 8 frames");
	check(fr48.size() >= 2, "data/fmv_tone_48k.mp2: at least 2 frames");
	if (fr44.size() >= 8 && fr48.size() >= 2) {
		test_decode(fr44);
		test_schedule(fr44);
		test_rate(fr48);
		test_model(fr44);
	}
	if (failures) {
		printf("test_fmv_audio: %d FAILED\n", failures);
		return 1;
	}
	printf("test_fmv_audio: OK\n");
	return 0;
}
