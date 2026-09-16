#ifndef AKIKO_PACE_H
#define AKIKO_PACE_H

#include <stdint.h>

// Sector delivery pacing for the Akiko (CD32) bridge.
//
// Why a rate limit exists at all
// ------------------------------
// Enabling Z2 fast RAM with a CD mounted used to boot the CD32 core to a black
// screen (#1277 here, core #218). The controlling variable is elapsed time per
// sector: below roughly 0.4 ms it fails every time, at 1.6 ms it passes every
// time. We shipped that time as 64-word chunks with a usleep(10) between them,
// which spends it as a side effect of the transport shape.
//
// That was a mitigation for a core-side defect, now identified: ddram_ctrl's
// state 1 dropped a DDR3 read return that arrived while waitrequest was high,
// which wedges master 0 permanently -- a 68k stalled on a fast-RAM fetch. Z2
// fast RAM lives in DDR3, and Akiko's sector DMA is what makes waitrequest
// coincide with a return often enough to matter. See T24 in the core repo's
// docs/core-accuracy-todo.md.
//
// Why this module exists
// ----------------------
// The chunk gaps were never a rate limit that anyone could read. usleep(10)
// does not cost 10 us: it costs whatever a scheduler wakeup costs, measured at
// ~92 us on the MiSTer and ~70 us in a desktop container. So the old form spent
// ~1.7 ms per sector while its constants said 180 us, and the protection would
// have vanished -- silently, at a boot far away from any related change -- the
// moment someone widened the chunks, shrank the gap, or ran on a platform where
// short sleeps are cheap.
//
// So the floor is stated as a floor, in one place, in microseconds of wall
// clock, and it no longer depends on how coarse usleep happens to be.
//
// Kept free of MiSTer headers so it can be unit-tested with the host compiler.
// The caller owns the clock and the sleep; this is only the arithmetic, which
// is the part with the signed-wrap edge cases worth pinning down.

// Minimum interval between the starts of two sector pushes, in microseconds.
//
// 1700 is deliberately about what the chunked form already costs, so this
// commit changes the guarantee rather than the behaviour: the failing side of
// the measured knee is 0.37 ms (0/3 boots) and the passing side is 1.64 ms
// (3/3) and 1.66 ms (8/8), and we sit just above the latter.
//
// It is NOT the authentic rate. A CD32 is a 2x unit -- 150 sectors/s, 6667 us
// per sector -- and nothing on real hardware feeds the pipeline faster. Moving
// there would be more faithful and would give ~4x the margin over the knee, at
// the cost of roughly 4x the load time and 4x this stall in the main poll loop.
// That is a policy change, not a cleanup, and it wants its own hardware pass.
//
// If the ddram_ctrl fix holds on hardware, the right end state may be no floor
// at all. Removing it is then a hardware test, not an assumption.
#define AKIKO_SECTOR_PERIOD_US 1700

// What a CD32 actually is: a 2x unit, 150 sectors/s, one sector every 6667 us.
// Selectable from the OSD (mm_cdSpeed in minimig_config.h) rather than
// compiled in, because which of the two you want is a judgement about the
// machine you are after -- faithful loading times, or short ones -- and not
// something this file can settle. Nothing above changes: the floor stays the
// default, so a machine nobody has touched behaves exactly as it always has.
#define AKIKO_SECTOR_PERIOD_2X_US 6667

// How long to wait, in microseconds, before starting a push, so that it begins
// at least period_us after the previous one began.
//
//   prev_us   when the previous push began. Negative means "no previous push",
//             which is the first sector after a mount or a seek.
//   now_us    now, from the same monotonic clock as prev_us.
//
// Returns 0 when the interval has already elapsed, so a caller that is slower
// than the floor -- a CHD miss, a physical drive seek -- is never delayed
// further by it.
int64_t akiko_pace_delay_us(int64_t prev_us, int64_t now_us, int64_t period_us);

#endif
