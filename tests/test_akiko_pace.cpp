// Native unit tests for the Akiko sector delivery decisions the Main still
// makes: which rate the core should pace at, whether to serve a request at
// all, and the drive mechanics (seek, stuck pause).
//
// The sleep arithmetic that used to lead this file went with the sleep, on
// 2026-10-28, when the core took the pacing over (akiko_pace.h).

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include "../support/minimig/akiko_pace.h"

static int failures = 0;

static void check(const char *name, int64_t got, int64_t want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %lld, want %lld\n", name, (long long)got, (long long)want); failures++; }
}

static void check_bool(const char *name, bool got, bool want)
{
	if (got != want) {
		printf("FAIL %s: got %s want %s\n", name,
		       got ? "true" : "false", want ? "true" : "false");
		failures++;
	}
}

static void check_i64(const char *name, int64_t got, int64_t want)
{
	if (got != want) {
		printf("FAIL %s: got %lld want %lld\n", name,
		       (long long)got, (long long)want);
		failures++;
	}
}

int main(void)
{
	// ------------------------------------------------- the rate to send
	// akiko_cd_rate_bits() picks what akiko.v paces at. The codes are the
	// core's (0 = 8x, 1 = 1x, 2 = 2x, 3 = 4x), so these pin the numbers, not
	// the macros: a macro that drifted would move both sides of a check on it.
	{
		// Drive, the default, runs at what the guest commanded.
		check("Drive, guest asks 1x -> 1x",  akiko_cd_rate_bits(2, 1), 1);
		check("Drive, guest asks 2x -> 2x",  akiko_cd_rate_bits(2, 2), 2);

		// CD32 (2x) is 2x whatever the guest asked.
		check("CD32 2x, guest asks 1x -> 2x", akiko_cd_rate_bits(1, 1), 2);
		check("CD32 2x, guest asks 2x -> 2x", akiko_cd_rate_bits(1, 2), 2);

		// Fast is 8x, the old 1.7 ms floor's neighbour, code 0.
		check("Fast, guest asks 1x -> 8x",   akiko_cd_rate_bits(0, 1), 0);
		check("Fast, guest asks 2x -> 8x",   akiko_cd_rate_bits(0, 2), 0);

		// A stored mode this build does not know is Fast, as the OSD shows it.
		check("unknown mode -> 8x",          akiko_cd_rate_bits(7, 2), 0);

		// Code 0 must stay 8x. An older Main leaves status[62:61] clear and
		// paces by sleeping; a newer core reading 0 as anything slower would
		// slow it down. And the fastest code may not undercut the Z2 fast RAM
		// + CD boot knee: 8x is 1e6/600 = 1667 us, above the 1.64 ms measured
		// passing side; the failing side was ~0.4 ms.
		check("code 0 is 8x", AKIKO_RATE_8X, 0);
		if (1000000 / 600 >= 1640) printf("PASS 8x is above the evidenced knee\n");
		else { printf("FAIL 8x undercuts the evidenced knee\n"); failures++; }
	}

	// ------------------------------------------------ the CDTV's spacing
	// The CR-511 is a 1x drive, so Drive is 75 sectors/s; CD32 2x gives it
	// 150/s; Fast is the 1 ms gap the synchronous loop slept.
	{
		check("CDTV Drive is 1x (1e6/75)",    cdtv_sector_period_us(2), 13333);
		check("CDTV CD32 2x is 2x (1e6/150)", cdtv_sector_period_us(1), 6667);
		check("CDTV Fast keeps the 1 ms gap", cdtv_sector_period_us(0), 1000);
		check("CDTV unknown mode is Fast",    cdtv_sector_period_us(9), 1000);
		check("CDTV seek is half a second",   CDTV_SEEK_US, 500000);
	}

	// ------------------------------------------------ the sector-serve gate
	{
		// Serve only when a read is armed and the drive is not paused.
		check_bool("armed, running -> serve",
		           akiko_sec_should_serve(0, 0), true);
		check_bool("armed at a high lba, running -> serve",
		           akiko_sec_should_serve(25622, 0), true);

		// A paused drive delivers nothing. This is the change: the surplus
		// sector that used to land in a PBX slot after the guest PAUSEd, and
		// that the next read then parsed as its own first sector.
		check_bool("armed, paused -> decline",
		           akiko_sec_should_serve(0, 1), false);
		check_bool("armed at a high lba, paused -> decline",
		           akiko_sec_should_serve(25622, 1), false);

		// No armed read: the engine is asking for data nobody requested.
		check_bool("no armed read -> decline",
		           akiko_sec_should_serve(-1, 0), false);
		check_bool("no armed read and paused -> decline",
		           akiko_sec_should_serve(-1, 1), false);

		// A negative base that is not the -1 sentinel is still an armed read:
		// DotC arms PLAY DATA at start_lba=-34 and streams across the pre-gap,
		// and refusing those requests stranded its BIOS waiting for a first
		// sector that never came. Only -1 means "nothing armed".
		check_bool("a pre-gap base is armed, not idle",
		           akiko_sec_should_serve(-34, 0), true);
	}

	// ------------------------------------------- the stuck-pause detector
	{
		// since_ms == 0 means "not declining", whatever the clock says.
		check_bool("not declining is never stuck",
		           akiko_pause_stuck(0, 999999, 2000), false);

		// Inside the limit is a pause; past it is a hang.
		check_bool("1 s of pause is just a pause",
		           akiko_pause_stuck(1000, 2000, 2000), false);
		check_bool("exactly the limit is not yet stuck",
		           akiko_pause_stuck(1000, 3000, 2000), false);
		check_bool("past the limit is stuck",
		           akiko_pause_stuck(1000, 3001, 2000), true);

		// The clock wraps. Unsigned subtraction has to carry the caller through
		// it: computed signed, or widened, a wrap reads as a pause of about
		// forty-nine days -- which would either warn instantly for ever or
		// never warn again, depending on which way the comparison fell.
		check_bool("a pause spanning a clock wrap still measures",
		           akiko_pause_stuck(0xFFFFF000u, 0x00000100u, 2000), true);
		check_bool("a short pause spanning a wrap is still short",
		           akiko_pause_stuck(0xFFFFF000u, 0xFFFFF500u, 2000), false);   // 1280 ms
	}

	// --------------------------------------------- the head has to get there
	{
		const uint32_t span = 26914;            // Chuck Rock, lead-out

		// Streaming costs nothing. This is the case the drive is built for and
		// the one every loader depends on, so it has to be exactly zero rather
		// than small.
		check_i64("the next sector is free", akiko_seek_us(100, 101, span), 0);
		check_i64("the same sector is free", akiko_seek_us(100, 100, span), 0);

		// Anything else pays the settle, at least.
		check_bool("a short hop costs the base",
		           akiko_seek_us(100, 140, span) >= AKIKO_SEEK_BASE_US, true);
		check_bool("a short hop costs little more than the base",
		           akiko_seek_us(100, 140, span) < AKIKO_SEEK_BASE_US + 2000, true);

		// A full stroke is the cap, and nothing exceeds it -- including a seek
		// past the lead-out, which BIOS does issue.
		check_i64("lead-in to lead-out is the full stroke",
		          akiko_seek_us(0, (int32_t)span, span), AKIKO_SEEK_FULL_US);
		check_i64("past the lead-out is still the full stroke",
		          akiko_seek_us(0, 286166, span), AKIKO_SEEK_FULL_US);

		// Direction does not matter to a head.
		check_i64("seeking back costs what seeking out costs",
		          akiko_seek_us(25622, 14, span), akiko_seek_us(14, 25622, span));

		// An unknown head position is a drive that just spun up: the head is at
		// the lead-in, not wherever the last disc left it.
		check_i64("unknown is the lead-in",
		          akiko_seek_us(-1, 25622, span), akiko_seek_us(0, 25622, span));

		// A pre-gap target is a real place on the disc, and DotC reads from one.
		check_i64("a pre-gap target is the lead-in",
		          akiko_seek_us(5000, -34, span), akiko_seek_us(5000, 0, span));

		// A disc whose span we do not know yet must not come out free.
		check_i64("no span means assume the worst",
		          akiko_seek_us(0, 25622, 0), AKIKO_SEEK_FULL_US);
	}

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
