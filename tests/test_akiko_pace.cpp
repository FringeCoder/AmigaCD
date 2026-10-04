// Native unit tests for the Akiko sector delivery floor.
//
// What these cover: the arithmetic that decides how long to wait before the
// next sector push. The reason it is worth covering at all is that the thing it
// replaced was invisible -- 18 usleep(10) gaps that cost ~1.7 ms while reading
// as 180 us -- so the replacement has to be something a later reader can check
// against its stated intent rather than by measuring a boot.
//
// The edge cases are signed-time ones. akiko_cd32.cpp already carries a comment
// about exactly this class of bug ("Casting to unsigned mid-computation
// produces 2^64-class garbage values"), which is why the clock arithmetic is
// pulled out into a function with no clock in it.

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
	const int64_t P = AKIKO_SECTOR_PERIOD_US;

	// The first sector after a mount or a seek has nothing to be too soon
	// after, so it goes out immediately.
	check("no previous push waits not at all",
	      akiko_pace_delay_us(-1, 5000000, P), 0);

	// Same, with a clock reading smaller than the period. CLOCK_MONOTONIC
	// counts from boot, so this is the only case where the sentinel is load
	// bearing: at a large `now` the subtraction happens to exceed the period
	// anyway and a missing sentinel check cannot be seen.
	check("no previous push, clock still near zero",
	      akiko_pace_delay_us(-1, 100, P), 0);

	// Back to back: the whole period is still owed.
	check("zero elapsed waits the full period",
	      akiko_pace_delay_us(5000000, 5000000, P), P);

	// Part way through: the remainder, not the whole period. Getting this
	// wrong in the safe direction would halve the delivery rate.
	check("half elapsed waits the remainder",
	      akiko_pace_delay_us(5000000, 5000000 + P / 2, P), P - P / 2);

	check("one microsecond short waits one microsecond",
	      akiko_pace_delay_us(5000000, 5000000 + P - 1, P), 1);

	// A caller already slower than the floor is never delayed further: a CHD
	// miss or a physical-drive seek has spent the interval already.
	check("exactly the period waits not at all",
	      akiko_pace_delay_us(5000000, 5000000 + P, P), 0);
	check("longer than the period waits not at all",
	      akiko_pace_delay_us(5000000, 5000000 + 10 * P, P), 0);

	// Time apparently running backwards means stamps that cannot be compared.
	// Wait the whole period: the failure being prevented is delivering too
	// fast, and one slow sector is the cost of being wrong this way.
	check("backwards clock waits the full period",
	      akiko_pace_delay_us(5000000, 4000000, P), P);

	// A build that has established it no longer needs a floor sets it to zero,
	// and then nothing here may introduce a wait.
	check("zero period never waits",
	      akiko_pace_delay_us(5000000, 5000000, 0), 0);
	check("negative period never waits",
	      akiko_pace_delay_us(5000000, 5000000, -1), 0);

	// Stamps are microseconds since boot and the MiSTer stays up for weeks, so
	// the values in play are far outside 32 bits.
	const int64_t big = 4000LL * 24 * 3600 * 1000000LL;   // ~4000 days
	check("large stamps, interval elapsed",
	      akiko_pace_delay_us(big, big + P, P), 0);
	check("large stamps, interval outstanding",
	      akiko_pace_delay_us(big, big + P / 4, P), P - P / 4);

	// The two above pass even on a 32-bit computation, because only the
	// DIFFERENCE reaches the arithmetic and theirs is small. This one does not:
	// an hour of idle between sectors -- a paused game, a disc left in the
	// drive -- is 3.6e9 us, past INT32_MAX, so a truncating implementation
	// reads it as negative and waits instead of going straight out.
	const int64_t hour = 3600LL * 1000000LL;
	check("an hour between sectors still waits not at all",
	      akiko_pace_delay_us(big, big + hour, P), 0);

	// The floor itself: a value inside the band the hardware evidence covers.
	// Below ~0.4 ms the Z2 + CD boot fails outright; 1.64 and 1.66 ms are the
	// measured passing points. If someone lowers this, that is the range the
	// number has to be argued against.
	if (P >= 1600 && P <= 6667) printf("PASS floor is inside the evidenced band\n");
	else { printf("FAIL floor %lld us is outside the evidenced band 1600..6667\n", (long long)P); failures++; }

	// The other rate the OSD offers. A CD32 is a 2x unit -- 150 sectors/s --
	// so a sector is 1e6/150 = 6666.7 us. Checked to the microsecond because
	// it is a fact about the machine and not a tuning knob: a number that has
	// drifted is a setting still labelled "CD32 (2x)" that is not 2x.
	const int64_t P2 = AKIKO_SECTOR_PERIOD_2X_US;
	if (P2 == 6667) printf("PASS 2x rate is one sector at 150/s\n");
	else { printf("FAIL 2x rate is %lld us, want 6667 (1e6/150)\n", (long long)P2); failures++; }

	// And it must not be FASTER than the floor. The floor is what keeps the
	// Z2 fast RAM + CD boot alive below ~0.4 ms; a "more authentic" setting
	// that undercut it would reintroduce that failure wearing a name that
	// sounds like the safer choice.
	if (P2 >= P) printf("PASS 2x rate is not faster than the floor\n");
	else { printf("FAIL 2x rate %lld us is faster than the floor %lld us\n", (long long)P2, (long long)P); failures++; }

	// The arithmetic itself has no opinion about which period it is given, and
	// this is the check that says so: the same three cases at the 2x period.
	check("2x: zero elapsed waits the full period",
	      akiko_pace_delay_us(1000000, 1000000, P2), P2);
	check("2x: one microsecond short waits one microsecond",
	      akiko_pace_delay_us(1000000, 1000000 + P2 - 1, P2), 1);
	check("2x: exactly the period waits not at all",
	      akiko_pace_delay_us(1000000, 1000000 + P2, P2), 0);

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
