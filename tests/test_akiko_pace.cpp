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

#include "../support/minimig/akiko_pace.h"

static int failures = 0;

static void check(const char *name, int64_t got, int64_t want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %lld, want %lld\n", name, (long long)got, (long long)want); failures++; }
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

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
