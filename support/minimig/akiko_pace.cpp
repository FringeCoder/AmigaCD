// Akiko sector delivery pacing. See akiko_pace.h for why this file carries no
// MiSTer includes, and for what the floor is protecting against.

#include <stdint.h>

#include "akiko_pace.h"

int64_t akiko_pace_delay_us(int64_t prev_us, int64_t now_us, int64_t period_us)
{
	// A floor of zero or less is "no pacing", which is what a build that has
	// established it no longer needs one would set.
	if (period_us <= 0) return 0;

	// No previous push: nothing to be too soon after.
	if (prev_us < 0) return 0;

	const int64_t elapsed = now_us - prev_us;

	// Time apparently ran backwards. CLOCK_MONOTONIC does not do this, so
	// reaching here means the caller passed stamps from different clocks or a
	// stamp it never set. Wait the whole period rather than none of it: the
	// failure this floor exists to prevent is delivering too FAST, and the cost
	// of being wrong in this direction is one slow sector.
	if (elapsed < 0) return period_us;

	if (elapsed >= period_us) return 0;

	return period_us - elapsed;
}

bool akiko_sec_should_serve(int32_t data_lba_base, int paused)
{
	// -1 is the sentinel for "nothing armed", and ONLY -1. A negative base is
	// still a real read: DotC arms PLAY DATA at start_lba=-34 and streams across
	// the pre-gap into track 1, and refusing those requests stranded its BIOS
	// waiting for a first sector that never came. `< 0` here would have brought
	// that back; the unit test for it is why this reads the way it does.
	if (data_lba_base == -1) return false;
	if (paused)            return false;
	return true;
}

bool akiko_pause_stuck(uint32_t since_ms, uint32_t now_ms, uint32_t limit_ms)
{
	// since_ms == 0 is "not declining"; the caller sets it on the first
	// decline and clears it the moment a request is served.
	if (!since_ms) return false;
	// Unsigned subtraction, so a GetTimer() wrap behaves rather than reporting
	// a four-billion-millisecond pause.
	return (uint32_t)(now_ms - since_ms) > limit_ms;
}

int64_t akiko_seek_us(int32_t from_lba, int32_t to_lba, uint32_t span_lba)
{
	// Unknown head position means the drive has just spun up, and the head is
	if (from_lba < 0) from_lba = 0;              // at the lead-in.
	if (to_lba   < 0) to_lba   = 0;              // pre-gap reads start there too

	const int64_t delta = (int64_t)to_lba - (int64_t)from_lba;
	const int64_t dist  = (delta < 0) ? -delta : delta;

	// Continuing where the last sector stopped costs nothing: the head is
	// already over the next block and the drive streams.
	if (dist <= 1) return 0;

	// Everything else pays a settle, plus the stroke. A disc whose span we do
	// not know is treated as a full stroke rather than as free.
	if (!span_lba) return AKIKO_SEEK_FULL_US;

	int64_t travel = (int64_t)(AKIKO_SEEK_FULL_US - AKIKO_SEEK_BASE_US);
	travel = (travel * dist) / (int64_t)span_lba;
	if (travel < 0) travel = 0;

	int64_t us = AKIKO_SEEK_BASE_US + travel;
	if (us > AKIKO_SEEK_FULL_US) us = AKIKO_SEEK_FULL_US;   // dist beyond the span
	return us;
}
