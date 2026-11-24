// Akiko sector delivery pacing. See akiko_pace.h for why this file carries no
// MiSTer includes, and for what the floor is protecting against.

#include <stdint.h>

#include "akiko_pace.h"

int akiko_cd_rate_bits(int mode, int commanded_speed)
{
	switch (mode) {
	case 1:  return AKIKO_RATE_2X;                               // mmcd_2x
	case 2:  return (commanded_speed >= 2) ? AKIKO_RATE_2X       // mmcd_drive
	                                       : AKIKO_RATE_1X;
	default: return AKIKO_RATE_8X;                               // mmcd_fast
	}
}

int64_t cdtv_sector_period_us(int mode)
{
	switch (mode) {
	case 1:  return 6667;    // mmcd_2x
	case 2:  return 13333;   // mmcd_drive: the CR-511 is 1x
	default: return 1000;    // mmcd_fast
	}
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

int64_t akiko_rate_period_us(int rate_code)
{
	switch (rate_code) {
	case AKIKO_RATE_1X: return 13333;
	case AKIKO_RATE_2X: return 6667;
	case AKIKO_RATE_4X: return 3333;
	default:            return 1667;    // AKIKO_RATE_8X
	}
}

void akiko_cadence_reset(akiko_cadence_t *c)
{
	*c = akiko_cadence_t();
	c->last_lba = -1;
}

void akiko_cadence_add(akiko_cadence_t *c, int32_t lba, int64_t now_us,
                       int64_t period_us)
{
	const bool follows = c->last_lba >= 0 && lba == c->last_lba + 1
	                  && period_us > 0
	                  && now_us >= c->ship_us
	                  && now_us - c->ship_us <= AKIKO_CADENCE_IDLE_US;
	c->last_lba = lba;
	if (!follows) {
		c->ship_us = now_us;            // the first of a run goes at once
		return;
	}

	const int64_t due  = c->ship_us + period_us;
	const int64_t late = (now_us > due) ? now_us - due : 0;
	c->ship_us = (now_us > due) ? now_us : due;

	int bin;
	if      (late == 0)     bin = 0;
	else if (late <=  500)  bin = 1;
	else if (late <= 1000)  bin = 2;
	else if (late <= 2000)  bin = 3;
	else if (late <= 5000)  bin = 4;
	else                    bin = 5;
	c->bins[bin]++;
	c->n++;
	c->late_us += late;
	if (late > c->max_us) c->max_us = late;
}
