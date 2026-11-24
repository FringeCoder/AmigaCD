#ifndef AKIKO_PREFETCH_H
#define AKIKO_PREFETCH_H

#include <stdint.h>

// PBX prefetch cache for the Akiko (CD32) bridge, image-backed discs only.
//
// Mirrors WinUAE akiko.cpp:1589-1658. Without it, every sec_req is a fresh CHD
// seek and hunk decode (~600 us+ per sector). With it, sectors come out of one
// of a few 128-sector windows, aligned to 128-sector boundaries so that short
// backward seeks (the BIOS re-reading the PVD or a dirent) stay in-window.
//
// Multi-window LRU. One window thrashes on titles that interleave reads from
// 3+ regions (Sim City alternates LBAs 868/1066/1265 and ran at 39 % hit ratio
// with one window). 4 windows is the sweet spot: Sim City 99 %, Microcosm 95 %,
// no regression on sequential readers. 8 was tested 2026-05-10: +0.7 % on Sim
// City, -1-2 % on Microcosm/CF, and no help for streamers like Litil Divil.
//
// Read-ahead
// ----------
// Every sector read here happens inside user_io_poll, and nothing else runs
// meanwhile: no sector push and no FMV event drain. A synchronous fill of a
// whole window is 128 sector reads, measured at 15-45 ms on James Pond 3 and
// 50-140 ms on FMV streams. So a sequential reader never waits on a window:
//
//  - From the second sequential sector, at any index, the next window is read
//    ahead, AKIKO_PREFETCH_AHEAD_STEP sectors per sector served. The reader
//    consumes one per sector, so the read-ahead is a full window ahead after
//    128 / (STEP - 1) sectors and the reader cannot catch it. The old rule (16
//    in a row and 32 into the window, 2 per sector) left a reader that started
//    late in a window, or crossed one early, to a synchronous fill.
//  - A window being filled is readable at once, slot by slot. A request for a
//    slot not read yet, or a miss inside a sequential run, reads
//    AKIKO_PREFETCH_LEAD sectors from the requested one before serving it, and
//    leaves the rest to the read-ahead. Slots before that one stay unread until
//    something asks for them, so the read-ahead reaches the next window first.
//  - A miss that is not sequential (a seek) still fills the whole window: a
//    random reader such as Sim City lives on whole windows, and a seek has its
//    own mechanical wait in front of it.
//
// Worker mode
// -----------
// Even with the read-ahead, every read-ahead step ran inside user_io_poll, and
// a step that crosses into a new CHD hunk (8 sectors) decodes it: 4.0 ms at the
// median, 5.7 ms at worst, measured on James Pond 3. At 2x a sector is due
// every 6.67 ms, so one decode plus the ~1.8 ms push sometimes made a sector
// late. With `async` set, akiko_prefetch_get never reads ahead itself; it only
// says where the read-ahead should go (`ahead`, `cursor`), and a worker thread
// does the reading through akiko_prefetch_claim / akiko_prefetch_complete:
//
//  - claim marks the next pending slot READING and hands out its LBA and the
//    window's generation. The worker reads the sector into its own buffer
//    with no lock held.
//  - complete copies it into the slot, unless the window was refilled or
//    rebased meanwhile (its generation moved), in which case it is dropped.
//  - A request for a READING slot waits for that read through `wait`, which
//    must let the worker complete it (a condition wait on the caller's lock).
//  - A catch-up or a sequential miss reads only the requested sector on the
//    caller's thread and leaves the rest of the window to the worker.
//
// None of this takes a lock. The caller holds one lock around every call into
// this file, and `wait` drops it while it sleeps.
//
// Kept free of MiSTer headers so it can be unit-tested with the host compiler;
// the sector source is a callback.

#define AKIKO_PREFETCH_SECTOR_BYTES 2352
#define AKIKO_PREFETCH_SECTORS      128
#define AKIKO_PREFETCH_WINDOWS      4

#define AKIKO_PREFETCH_AHEAD_STEP   3   // sectors read ahead per sector served
#define AKIKO_PREFETCH_LEAD         8   // sectors read before serving a slot not read yet

// Slot states in prefetch_window_t.valid.
#define AKIKO_SLOT_BAD     0   // read failed; re-read on access
#define AKIKO_SLOT_OK      1
#define AKIKO_SLOT_PENDING 2   // not read yet
#define AKIKO_SLOT_READING 3   // claimed by the worker, read in flight

// Reads one raw 2352-byte sector. Returns 0 on success.
typedef int (*akiko_prefetch_read_fn)(void *ctx, uint32_t lba, uint8_t *dst);

typedef struct {
	int32_t  base_lba;     // first LBA covered, -1 = empty
	uint32_t lru_tick;     // higher = more recently used
	uint32_t gen;          // bumped whenever the window is refilled or rebased
	uint8_t  valid[AKIKO_PREFETCH_SECTORS];
	uint8_t  buf[AKIKO_PREFETCH_SECTORS * AKIKO_PREFETCH_SECTOR_BYTES];
} prefetch_window_t;

// What the last akiko_prefetch_get did, for the caller's log.
#define AKIKO_PF_HIT      0   // served from a cache slot already read
#define AKIKO_PF_FULL     1   // missed, not sequential: whole window filled
#define AKIKO_PF_PARTIAL  2   // missed inside a sequential run: LEAD sectors filled
#define AKIKO_PF_CATCHUP  3   // slot not read yet: LEAD sectors filled

typedef struct {
	prefetch_window_t win[AKIKO_PREFETCH_WINDOWS];
	uint32_t lru_counter;
	int      ahead;          // window the read-ahead fills, -1 = none
	int      cursor;         // next slot of `ahead` to look at
	uint32_t seq_last;       // last LBA served
	int      seq_valid;      // seq_last means something

	uint32_t hits, misses, failed_sectors;

	// Worker mode, see above. async = 0 reads ahead inline, as before.
	int      async;
	void   (*wait)(void *ctx);   // sleep until a claimed read completes
	void    *wait_ctx;

	// Per call, for the log and the tests.
	int      last_event;     // AKIKO_PF_*
	int      last_win;
	uint32_t last_base;
	int      last_sync;      // sectors read before serving
} akiko_prefetch_t;

void akiko_prefetch_reset(akiko_prefetch_t *pf);  // invalidate and clear the counters
void akiko_prefetch_invalidate(akiko_prefetch_t *pf);

// Returns 0 with out filled, -1 when a whole fill failed, or -2 when this one
// sector failed (the caller holds sec_req so the BIOS retries).
int akiko_prefetch_get(akiko_prefetch_t *pf, uint32_t lba, uint8_t *out,
                       akiko_prefetch_read_fn read, void *ctx);

// The window holding lba, or -1. A window being filled counts.
int akiko_prefetch_find(const akiko_prefetch_t *pf, uint32_t lba);

// Worker mode. claim returns 1 and marks one slot READING when the read-ahead
// has a pending slot left, else 0 (and the read-ahead is finished). complete
// files the result; ok = 0 marks the slot bad, to be re-read on access.
int  akiko_prefetch_claim(akiko_prefetch_t *pf, int *wi, int *idx, uint32_t *lba, uint32_t *gen);
void akiko_prefetch_complete(akiko_prefetch_t *pf, int wi, int idx, uint32_t gen,
                             const uint8_t *data, int ok);

#endif
