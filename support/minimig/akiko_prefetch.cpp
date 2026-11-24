// PBX prefetch cache for the Akiko bridge. See akiko_prefetch.h for the policy
// and why this file carries no MiSTer includes.

#include <stdint.h>
#include <string.h>

#include "akiko_prefetch.h"

void akiko_prefetch_invalidate(akiko_prefetch_t *pf)
{
	for (int i = 0; i < AKIKO_PREFETCH_WINDOWS; i++) {
		pf->win[i].base_lba = -1;
		pf->win[i].gen++;
	}
	pf->ahead = -1;
	pf->cursor = 0;
	pf->seq_valid = 0;
}

void akiko_prefetch_reset(akiko_prefetch_t *pf)
{
	akiko_prefetch_invalidate(pf);
	pf->lru_counter = 0;
	pf->hits = pf->misses = pf->failed_sectors = 0;
	pf->last_event = AKIKO_PF_HIT;
	pf->last_win = -1;
	pf->last_base = 0;
	pf->last_sync = 0;
}

int akiko_prefetch_find(const akiko_prefetch_t *pf, uint32_t lba)
{
	for (int i = 0; i < AKIKO_PREFETCH_WINDOWS; i++) {
		const prefetch_window_t *w = &pf->win[i];
		if (w->base_lba >= 0
		    && lba >= (uint32_t)w->base_lba
		    && lba <  (uint32_t)w->base_lba + AKIKO_PREFETCH_SECTORS) {
			return i;
		}
	}
	return -1;
}

// An empty window if any, else the oldest by lru_tick that is not `keep`.
// Wraparound at 4 billion accesses flips one comparison; no correctness impact.
static int pick_evict(const akiko_prefetch_t *pf, int keep)
{
	int pick = -1;
	for (int i = 0; i < AKIKO_PREFETCH_WINDOWS; i++) {
		if (i == keep) continue;
		if (pf->win[i].base_lba < 0) return i;
		if (pick < 0 || pf->win[i].lru_tick < pf->win[pick].lru_tick) pick = i;
	}
	return pick;
}

static int read_slot(akiko_prefetch_t *pf, prefetch_window_t *w, int i,
                     akiko_prefetch_read_fn read, void *ctx)
{
	uint8_t *slot = &w->buf[i * AKIKO_PREFETCH_SECTOR_BYTES];
	if (read(ctx, (uint32_t)w->base_lba + i, slot) == 0) {
		w->valid[i] = AKIKO_SLOT_OK;
		return 1;
	}
	w->valid[i] = AKIKO_SLOT_BAD;
	pf->failed_sectors++;
	return 0;
}

// Point the read-ahead at window wi, now covering base, nothing read yet.
static void begin_ahead(akiko_prefetch_t *pf, int wi, uint32_t base, int cursor)
{
	prefetch_window_t *w = &pf->win[wi];
	w->base_lba = (int32_t)base;
	w->lru_tick = ++pf->lru_counter;
	w->gen++;
	memset(w->valid, AKIKO_SLOT_PENDING, sizeof(w->valid));
	pf->ahead = wi;
	pf->cursor = cursor;
}

// Read up to n pending slots of the read-ahead window, in order from the
// cursor to the end of the window. Returns how many were read. Slots behind a
// mid-window start are left for an on-demand read, so the read-ahead moves on
// to the next window as soon as the reader could need it.
static int ahead_step(akiko_prefetch_t *pf, int n, akiko_prefetch_read_fn read, void *ctx)
{
	if (pf->ahead < 0) return 0;
	prefetch_window_t *w = &pf->win[pf->ahead];
	int done = 0;
	for (; done < n && pf->cursor < AKIKO_PREFETCH_SECTORS; pf->cursor++) {
		if (w->valid[pf->cursor] != AKIKO_SLOT_PENDING) continue;
		read_slot(pf, w, pf->cursor, read, ctx);
		done++;
	}
	while (pf->cursor < AKIKO_PREFETCH_SECTORS && w->valid[pf->cursor] != AKIKO_SLOT_PENDING) pf->cursor++;
	if (pf->cursor >= AKIKO_PREFETCH_SECTORS) pf->ahead = -1;
	return done;
}

int akiko_prefetch_get(akiko_prefetch_t *pf, uint32_t lba, uint8_t *out,
                       akiko_prefetch_read_fn read, void *ctx)
{
	const int seq = pf->seq_valid && lba == pf->seq_last + 1;
	pf->last_event = AKIKO_PF_HIT;
	pf->last_sync = 0;

	int wi = akiko_prefetch_find(pf, lba);
	if (wi < 0) {
		pf->misses++;
		// A miss is somewhere the read-ahead is not: drop it. Its window keeps
		// its unread slots, which are read on demand like any other.
		pf->ahead = -1;
		wi = pick_evict(pf, -1);
		uint32_t base = (lba / AKIKO_PREFETCH_SECTORS) * AKIKO_PREFETCH_SECTORS;
		pf->last_win = wi;
		pf->last_base = base;
		if (seq) {
			// The slot is read below, as a catch-up from the requested sector.
			begin_ahead(pf, wi, base, (int)(lba - base));
			pf->last_event = AKIKO_PF_PARTIAL;
		} else {
			prefetch_window_t *w = &pf->win[wi];
			w->base_lba = (int32_t)base;
			w->gen++;   // drops any worker read still in flight for this window
			int filled = 0;
			for (int i = 0; i < AKIKO_PREFETCH_SECTORS; i++) {
				w->valid[i] = AKIKO_SLOT_BAD;
				filled += read_slot(pf, w, i, read, ctx);
			}
			pf->last_event = AKIKO_PF_FULL;
			pf->last_sync = AKIKO_PREFETCH_SECTORS;
			if (filled == 0) return -1;
		}
	} else {
		pf->hits++;
	}

	prefetch_window_t *w = &pf->win[wi];
	w->lru_tick = ++pf->lru_counter;
	const int idx = (int)(lba - (uint32_t)w->base_lba);

	// The worker is reading this very slot: waiting for it is never slower
	// than reading it again, and it may be most of the way through the hunk.
	// Only this thread rebases a window, so wi and idx still hold afterwards.
	while (w->valid[idx] == AKIKO_SLOT_READING && pf->wait) {
		pf->wait(pf->wait_ctx);
	}

	if (w->valid[idx] == AKIKO_SLOT_PENDING) {
		// The reader is ahead of the fill, or the window was just started:
		// read a few from here and leave the rest to the read-ahead.
		if (pf->last_event == AKIKO_PF_HIT) {
			pf->last_event = AKIKO_PF_CATCHUP;
			pf->last_win = wi;
			pf->last_base = (uint32_t)w->base_lba;
		}
		if (pf->async) {
			// Just this sector here; the worker carries on from the next one.
			read_slot(pf, w, idx, read, ctx);
			pf->last_sync++;
			pf->ahead = wi;
			pf->cursor = idx + 1;
			if (pf->cursor >= AKIKO_PREFETCH_SECTORS) pf->ahead = -1;
		} else {
			pf->ahead = wi;
			pf->cursor = idx;
			pf->last_sync += ahead_step(pf, AKIKO_PREFETCH_LEAD, read, ctx);
		}
		if (pf->last_event == AKIKO_PF_PARTIAL && w->valid[idx] != AKIKO_SLOT_OK) {
			// Nothing in the first lead read is a disc problem, not a cache
			// one: report it the way a failed whole fill was reported.
			int any = 0;
			for (int i = 0; i < AKIKO_PREFETCH_SECTORS; i++) any |= (w->valid[i] == AKIKO_SLOT_OK);
			if (!any) return -1;
		}
	}
	if (w->valid[idx] == AKIKO_SLOT_BAD) {
		// In-window but flagged bad. A transient CHD error must not poison
		// the slot for good, so try this one LBA again.
		pf->last_sync++;
		if (!read_slot(pf, w, idx, read, ctx)) return -2;
	}
	memcpy(out, &w->buf[idx * AKIKO_PREFETCH_SECTOR_BYTES], AKIKO_PREFETCH_SECTOR_BYTES);

	pf->seq_last = lba;
	pf->seq_valid = 1;

	if (pf->ahead >= 0) {
		if (!pf->async) ahead_step(pf, AKIKO_PREFETCH_AHEAD_STEP, read, ctx);
	} else if (seq) {
		uint32_t next = (uint32_t)w->base_lba + AKIKO_PREFETCH_SECTORS;
		if (akiko_prefetch_find(pf, next) < 0) {
			begin_ahead(pf, pick_evict(pf, wi), next, 0);
			if (!pf->async) ahead_step(pf, AKIKO_PREFETCH_AHEAD_STEP, read, ctx);
		}
	}
	return 0;
}

int akiko_prefetch_claim(akiko_prefetch_t *pf, int *wi, int *idx, uint32_t *lba, uint32_t *gen)
{
	if (pf->ahead < 0) return 0;
	prefetch_window_t *w = &pf->win[pf->ahead];
	while (pf->cursor < AKIKO_PREFETCH_SECTORS && w->valid[pf->cursor] != AKIKO_SLOT_PENDING) pf->cursor++;
	if (pf->cursor >= AKIKO_PREFETCH_SECTORS) {
		pf->ahead = -1;
		return 0;
	}
	*wi  = pf->ahead;
	*idx = pf->cursor;
	*lba = (uint32_t)w->base_lba + (uint32_t)pf->cursor;
	*gen = w->gen;
	w->valid[pf->cursor++] = AKIKO_SLOT_READING;
	return 1;
}

void akiko_prefetch_complete(akiko_prefetch_t *pf, int wi, int idx, uint32_t gen,
                             const uint8_t *data, int ok)
{
	if (wi < 0 || wi >= AKIKO_PREFETCH_WINDOWS || idx < 0 || idx >= AKIKO_PREFETCH_SECTORS) return;
	prefetch_window_t *w = &pf->win[wi];
	if (w->gen != gen || w->valid[idx] != AKIKO_SLOT_READING) return;
	if (ok) {
		memcpy(&w->buf[idx * AKIKO_PREFETCH_SECTOR_BYTES], data, AKIKO_PREFETCH_SECTOR_BYTES);
		w->valid[idx] = AKIKO_SLOT_OK;
	} else {
		w->valid[idx] = AKIKO_SLOT_BAD;
		pf->failed_sectors++;
	}
}
