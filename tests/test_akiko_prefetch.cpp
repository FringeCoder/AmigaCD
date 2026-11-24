// Native unit tests for the Akiko PBX prefetch cache and its read-ahead
// (support/minimig/akiko_prefetch.h).
//
// The point of the read-ahead is that a sequential reader never waits on a
// window fill inside user_io_poll. So the checks below are mostly about
// last_sync, the number of sectors read before a request was served: zero for
// a sequential reader in steady state, at most AKIKO_PREFETCH_LEAD when it
// does run ahead, and a whole window only for a seek.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../support/minimig/akiko_prefetch.h"

static int failures = 0;

static void check(const char *name, int64_t got, int64_t want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %lld, want %lld\n", name, (long long)got, (long long)want); failures++; }
}

// A disc whose sector n holds n in its first four bytes. Sectors in
// [bad_lo, bad_hi) fail while bad_reads_left > 0 (each failure uses one).
struct fake_disc {
	uint32_t reads;
	uint32_t bad_lo, bad_hi;
	int      bad_reads_left;
	uint32_t times_read[1 << 16];
};

static int fake_read(void *ctx, uint32_t lba, uint8_t *dst)
{
	fake_disc *d = (fake_disc *)ctx;
	d->reads++;
	if (lba < (1u << 16)) d->times_read[lba]++;
	if (lba >= d->bad_lo && lba < d->bad_hi && d->bad_reads_left > 0) {
		d->bad_reads_left--;
		return -1;
	}
	memset(dst, 0, AKIKO_PREFETCH_SECTOR_BYTES);
	memcpy(dst, &lba, sizeof(lba));
	dst[AKIKO_PREFETCH_SECTOR_BYTES - 1] = (uint8_t)(lba * 7);
	return 0;
}

static akiko_prefetch_t pf;
static fake_disc disc;
static uint8_t out[AKIKO_PREFETCH_SECTOR_BYTES];

static void fresh(void)
{
	akiko_prefetch_reset(&pf);
	memset(&disc, 0, sizeof(disc));
}

static bool holds(uint32_t lba)
{
	uint32_t got;
	memcpy(&got, out, sizeof(got));
	return got == lba && out[AKIKO_PREFETCH_SECTOR_BYTES - 1] == (uint8_t)(lba * 7);
}

// Reads [from, from+n) one at a time and reports the worst wait after the
// first request, which is a seek.
struct run_result { int errors, wrong, worst_sync, fulls, partials, catchups; };

static run_result read_run(uint32_t from, int n)
{
	run_result r = {0, 0, 0, 0, 0, 0};
	for (int i = 0; i < n; i++) {
		uint32_t lba = from + i;
		if (akiko_prefetch_get(&pf, lba, out, fake_read, &disc) != 0) r.errors++;
		else if (!holds(lba)) r.wrong++;
		if (i == 0) continue;
		if (pf.last_sync > r.worst_sync) r.worst_sync = pf.last_sync;
		if (pf.last_event == AKIKO_PF_FULL) r.fulls++;
		if (pf.last_event == AKIKO_PF_PARTIAL) r.partials++;
		if (pf.last_event == AKIKO_PF_CATCHUP) r.catchups++;
	}
	return r;
}

static void test_sequential_from_window_start(void)
{
	fresh();
	run_result r = read_run(0, 2000);
	check("seq from 0: errors", r.errors, 0);
	check("seq from 0: wrong data", r.wrong, 0);
	check("seq from 0: first read is a whole window", disc.times_read[0], 1);
	check("seq from 0: no wait after the first", r.worst_sync, 0);
	check("seq from 0: no fills", r.fulls + r.partials + r.catchups, 0);
	// Every sector is read once; the read-ahead overshoots by at most a window.
	int twice = 0;
	for (int i = 0; i < 2000 + AKIKO_PREFETCH_SECTORS * 2; i++) twice += disc.times_read[i] > 1;
	check("seq from 0: no sector read twice", twice, 0);
	check("seq from 0: overshoot within one window",
	      disc.reads <= 2000 + 2 * AKIKO_PREFETCH_SECTORS, 1);
}

// The old rule (16 in a row and 32 into a window) never read ahead for a
// reader that started this late, and met a whole-window fill at the boundary.
static void test_sequential_from_late_in_window(void)
{
	for (int start = 96; start < AKIKO_PREFETCH_SECTORS; start += 7) {
		fresh();
		run_result r = read_run(1024 + start, 600);
		char name[64];
		snprintf(name, sizeof(name), "seq from idx %d: no wait", start);
		check(name, r.worst_sync, 0);
		snprintf(name, sizeof(name), "seq from idx %d: data", start);
		check(name, r.errors + r.wrong, 0);
	}
}

// James Pond 3 on hardware, 2026-11-22: a seek to 13683, then 13 sequential
// sectors into window 13696, which cost a 45 ms whole-window fill.
static void test_jp3_window_cross(void)
{
	fresh();
	akiko_prefetch_get(&pf, 13683, out, fake_read, &disc);
	check("jp3: the seek fills its window", pf.last_event, AKIKO_PF_FULL);
	int worst = 0;
	for (uint32_t lba = 13684; lba <= 13811; lba++) {
		akiko_prefetch_get(&pf, lba, out, fake_read, &disc);
		if (pf.last_sync > worst) worst = pf.last_sync;
	}
	check("jp3: crossing into 13696 does not wait", worst, 0);
	check("jp3: data", holds(13811), 1);
}

// A sequential request that misses everything (here: the first request was the
// last sector of a window, so nothing was read ahead yet) reads LEAD sectors,
// not 128, and the read-ahead takes the rest.
static void test_sequential_miss_reads_lead(void)
{
	fresh();
	akiko_prefetch_get(&pf, 127, out, fake_read, &disc);
	check("seq miss: setup seek", pf.last_event, AKIKO_PF_FULL);
	check("seq miss: rc", akiko_prefetch_get(&pf, 128, out, fake_read, &disc), 0);
	check("seq miss: partial", pf.last_event, AKIKO_PF_PARTIAL);
	check("seq miss: lead only", pf.last_sync, AKIKO_PREFETCH_LEAD);
	check("seq miss: data", holds(128), 1);
	run_result r = read_run(129, 400);
	check("seq miss: then no wait", r.worst_sync, 0);
	check("seq miss: then data", r.errors + r.wrong, 0);
}

// A sequential miss always lands on slot 0 (the sector before it is cached), so
// slots behind the cursor arise when a reader jumps into a window the
// read-ahead is still filling. Those are read from the requested slot on: the
// lead, and not the whole window.
static void test_unread_slot_catch_up(void)
{
	fresh();
	akiko_prefetch_get(&pf, 3071, out, fake_read, &disc);       // seek: whole 2944
	akiko_prefetch_get(&pf, 3072, out, fake_read, &disc);       // sequential miss
	check("catch-up: setup partial", pf.last_event, AKIKO_PF_PARTIAL);
	akiko_prefetch_get(&pf, 9000, out, fake_read, &disc);       // seek, drops the read-ahead
	check("catch-up: setup seek", pf.last_event, AKIKO_PF_FULL);

	check("catch-up: rc", akiko_prefetch_get(&pf, 3072 + 120, out, fake_read, &disc), 0);
	check("catch-up: event", pf.last_event, AKIKO_PF_CATCHUP);
	check("catch-up: reads to the window end", pf.last_sync, 8);
	check("catch-up: data", holds(3072 + 120), 1);
	check("catch-up: slot 50 still unread", disc.times_read[3072 + 50], 0);

	check("catch-up: back slot rc", akiko_prefetch_get(&pf, 3072 + 50, out, fake_read, &disc), 0);
	check("catch-up: back slot event", pf.last_event, AKIKO_PF_CATCHUP);
	check("catch-up: back slot lead", pf.last_sync, AKIKO_PREFETCH_LEAD);
	check("catch-up: back slot data", holds(3072 + 50), 1);
	run_result r = read_run(3072 + 51, 300);
	check("catch-up: then sequential without waits", r.worst_sync, 0);
	check("catch-up: then data", r.errors + r.wrong, 0);
}

// A seek still fills a whole window, so a random reader over a few regions
// (Sim City: 868/1066/1265) lives on hits.
static void test_random_reader_stays_on_whole_windows(void)
{
	fresh();
	const uint32_t regions[3] = {868, 1066, 1265};
	int bad = 0;
	for (int round = 0; round < 50; round++) {
		for (int k = 0; k < 3; k++) {
			uint32_t lba = regions[k] + (uint32_t)(round % 10);
			if (akiko_prefetch_get(&pf, lba, out, fake_read, &disc) != 0 || !holds(lba)) bad++;
		}
	}
	check("random: data", bad, 0);
	check("random: three misses", pf.misses, 3);
}

static void test_bad_sector_retries(void)
{
	fresh();
	disc.bad_lo = 5; disc.bad_hi = 6; disc.bad_reads_left = 2;  // the fill and the retry
	check("bad sector: first ask", akiko_prefetch_get(&pf, 5, out, fake_read, &disc), -2);
	check("bad sector: second ask", akiko_prefetch_get(&pf, 5, out, fake_read, &disc), 0);
	check("bad sector: data", holds(5), 1);

	fresh();
	disc.bad_lo = 0; disc.bad_hi = 1u << 30; disc.bad_reads_left = 1 << 30;
	check("dead disc: seek", akiko_prefetch_get(&pf, 300, out, fake_read, &disc), -1);
	check("dead disc: sequential", akiko_prefetch_get(&pf, 301, out, fake_read, &disc), -2);

	fresh();
	akiko_prefetch_get(&pf, 127, out, fake_read, &disc);
	disc.bad_lo = 0; disc.bad_hi = 1u << 30; disc.bad_reads_left = 1 << 30;
	check("dead disc: sequential miss", akiko_prefetch_get(&pf, 128, out, fake_read, &disc), -1);
}

static void test_invalidate_forgets(void)
{
	fresh();
	read_run(0, 50);
	akiko_prefetch_invalidate(&pf);
	check("invalidate: nothing cached", akiko_prefetch_find(&pf, 10), -1);
	check("invalidate: read-ahead stopped", pf.ahead, -1);
	akiko_prefetch_get(&pf, 50, out, fake_read, &disc);
	check("invalidate: next read is a seek", pf.last_event, AKIKO_PF_FULL);
}


// ---- Worker mode -----------------------------------------------------------
//
// A simulated worker: claims are queued, and run either a few at a time
// between requests (a worker keeping pace) or when the reader waits on one.

struct claim_t { int wi, idx; uint32_t lba, gen; };
static claim_t inflight[AKIKO_PREFETCH_SECTORS * AKIKO_PREFETCH_WINDOWS];
static int n_inflight;
static int waits;

static int worker_claim(void)
{
	claim_t c;
	if (!akiko_prefetch_claim(&pf, &c.wi, &c.idx, &c.lba, &c.gen)) return 0;
	inflight[n_inflight++] = c;
	return 1;
}

static void worker_finish_all(void)
{
	uint8_t sec[AKIKO_PREFETCH_SECTOR_BYTES];
	for (int i = 0; i < n_inflight; i++) {
		int ok = fake_read(&disc, inflight[i].lba, sec) == 0;
		akiko_prefetch_complete(&pf, inflight[i].wi, inflight[i].idx, inflight[i].gen, sec, ok);
	}
	n_inflight = 0;
}

static int worker_run(int n)
{
	int done = 0;
	while (done < n && worker_claim()) done++;
	worker_finish_all();
	return done;
}

static void sim_wait(void *)
{
	waits++;
	worker_finish_all();
}

static void fresh_async(void)
{
	fresh();
	pf.async = 1;
	pf.wait = sim_wait;
	pf.wait_ctx = 0;
	n_inflight = 0;
	waits = 0;
}

static void test_async_get_never_reads_ahead(void)
{
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);      // a seek: whole window
	check("async: seek fills a window", disc.reads, AKIKO_PREFETCH_SECTORS);
	akiko_prefetch_get(&pf, 1, out, fake_read, &disc);
	check("async: a hit reads nothing", disc.reads, AKIKO_PREFETCH_SECTORS);
	check("async: read-ahead aimed at the next window", pf.win[pf.ahead].base_lba, AKIKO_PREFETCH_SECTORS);
	check("async: the worker has the next window to read", worker_run(1000), AKIKO_PREFETCH_SECTORS);
	check("async: and it is all valid", pf.win[akiko_prefetch_find(&pf, 200)].valid[200 - 128], AKIKO_SLOT_OK);
}

static void test_async_sequential_never_waits(void)
{
	fresh_async();
	int errors = 0, wrong = 0, worst = 0;
	for (uint32_t lba = 300; lba < 300 + 2000; lba++) {
		if (akiko_prefetch_get(&pf, lba, out, fake_read, &disc) != 0) errors++;
		else if (!holds(lba)) wrong++;
		if (lba > 301 && pf.last_sync > worst) worst = pf.last_sync;
		worker_run(AKIKO_PREFETCH_AHEAD_STEP);
	}
	check("async sequential: no errors", errors, 0);
	check("async sequential: data", wrong, 0);
	check("async sequential: never reads on the caller", worst, 0);
	check("async sequential: never waited", waits, 0);
}

static void test_async_waits_for_inflight_slot(void)
{
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);
	akiko_prefetch_get(&pf, 1, out, fake_read, &disc);
	worker_claim();                                      // slot 0 of window 128, in flight
	uint32_t before = disc.reads;
	check("async inflight: hit", akiko_prefetch_get(&pf, 128, out, fake_read, &disc), 0);
	check("async inflight: waited once", waits, 1);
	check("async inflight: data", holds(128), 1);
	check("async inflight: read once, by the worker", disc.reads - before, 1);
}

static void test_async_stale_completion_dropped(void)
{
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);
	akiko_prefetch_get(&pf, 1, out, fake_read, &disc);
	worker_claim();
	akiko_prefetch_invalidate(&pf);
	worker_finish_all();
	check("async stale: invalidated window stays empty", akiko_prefetch_find(&pf, 128), -1);

	// Seeks that evict and refill the very window the worker is reading.
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);
	akiko_prefetch_get(&pf, 1, out, fake_read, &disc);
	worker_claim();                                      // LBA 128, old generation
	claim_t c = inflight[0];
	n_inflight = 0;
	const uint32_t seeks[4] = {600, 1000, 1400, 2000};   // the last one evicts window 128
	for (int i = 0; i < 4; i++) akiko_prefetch_get(&pf, seeks[i], out, fake_read, &disc);
	check("async stale: window 128 evicted", akiko_prefetch_find(&pf, 128), -1);
	check("async stale: its slot now holds 1920", pf.win[c.wi].base_lba, 1920);
	uint8_t junk[AKIKO_PREFETCH_SECTOR_BYTES];
	memset(junk, 0xEE, sizeof(junk));
	akiko_prefetch_complete(&pf, c.wi, c.idx, c.gen, junk, 1);
	check("async stale: served", akiko_prefetch_get(&pf, 1920, out, fake_read, &disc), 0);
	check("async stale: a late completion cannot overwrite it", holds(1920), 1);
}

static void test_async_reader_passes_worker(void)
{
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);
	for (uint32_t lba = 1; lba < 128; lba++) akiko_prefetch_get(&pf, lba, out, fake_read, &disc);
	// The worker never ran: the reader is into a window nothing has read yet.
	check("async passes: hit", akiko_prefetch_get(&pf, 128, out, fake_read, &disc), 0);
	check("async passes: data", holds(128), 1);
	check("async passes: catch-up", pf.last_event, AKIKO_PF_CATCHUP);
	check("async passes: one sector on the caller", pf.last_sync, 1);
	check("async passes: worker carries on after it", pf.cursor, 1);
	check("async passes: then fills the rest", worker_run(1000), AKIKO_PREFETCH_SECTORS - 1);
}

static void test_async_seq_miss_reads_one(void)
{
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);
	for (uint32_t lba = 1; lba < 128; lba++) akiko_prefetch_get(&pf, lba, out, fake_read, &disc);
	akiko_prefetch_invalidate(&pf);
	pf.seq_valid = 1; pf.seq_last = 127;   // still sequential, nothing cached
	check("async seq miss: served", akiko_prefetch_get(&pf, 128, out, fake_read, &disc), 0);
	check("async seq miss: partial", pf.last_event, AKIKO_PF_PARTIAL);
	check("async seq miss: one sector on the caller", pf.last_sync, 1);
	check("async seq miss: data", holds(128), 1);
}

static void test_async_bad_sector(void)
{
	fresh_async();
	akiko_prefetch_get(&pf, 0, out, fake_read, &disc);
	akiko_prefetch_get(&pf, 1, out, fake_read, &disc);
	disc.bad_lo = 130; disc.bad_hi = 131; disc.bad_reads_left = 1;
	worker_run(1000);
	check("async bad: counted", pf.failed_sectors, 1);
	check("async bad: re-read on access", akiko_prefetch_get(&pf, 130, out, fake_read, &disc), 0);
	check("async bad: data", holds(130), 1);
}

int main(void)
{
	test_sequential_from_window_start();
	test_sequential_from_late_in_window();
	test_jp3_window_cross();
	test_sequential_miss_reads_lead();
	test_unread_slot_catch_up();
	test_random_reader_stays_on_whole_windows();
	test_bad_sector_retries();
	test_invalidate_forgets();
	test_async_get_never_reads_ahead();
	test_async_sequential_never_waits();
	test_async_waits_for_inflight_slot();
	test_async_stale_completion_dropped();
	test_async_reader_passes_worker();
	test_async_seq_miss_reads_one();
	test_async_bad_sector();
	if (failures) { printf("%d FAILED\n", failures); return 1; }
	printf("all akiko_prefetch tests passed\n");
	return 0;
}
