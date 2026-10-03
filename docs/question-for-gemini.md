# Round 5: your READ TOC identification was right, and it falsifies your own step 4

Thank you — the `cmd[7]=0x03` identification is the most useful thing we have had
from this exchange. It let us test the mechanism directly, and the test says the
TOC is not where this fails.

## Your identification, confirmed in our own code

`op=0x04` with bit 7 of `cmd[7]` clear and MSF 00:00:00 (seekpos < 0) is the scan
TOC trigger. We already implement exactly that, citing the same WinUAE lines you
pointed at:

```c
} else if (seek_negative) {
    // PLAY with seekpos < 0 = "scan TOC" trigger (akiko.cpp:1095-1097).
    cd_data_lba_base = -1;
    cd_playing = 0;
    toc_push_idx = (toc_point_count > 0) ? 0 : -1;   // start the drip
    r[1] = 0x00;                                     // no play-started flag
```

So the two frames are the ROM's two `READ TOC`s, one before our announce (which
finds no disc) and one after it. That part of your account matches ours exactly,
and it is the first time anyone has named those frames correctly.

## Step 4 is falsified: a failing round DOES get a complete TOC

Your mechanism was that a failing round never receives the full stream of TOC
entries, so `cd.device` never learns track 1 geometry and therefore never asks
for LBA 14. We counted TOC frames in every round of a 16-round run:

| | TOC frames pushed | `TOC drip complete` | asks for LBA 14 |
|---|---|---|---|
| failing rounds | exactly **12** | **yes**, every time | never |
| booting rounds | 20–27 | no (a second drip was still running) | yes |

Twelve frames is the whole TOC: 4 points (`0xa0`, `0xa1`, `0xa2`, `0x01`) at 3
repeats each, which is the drip our code emits and the count WinUAE emits. The
failing rounds complete it, log the completion, and the ROM then does nothing at
all. The booting rounds show *more* frames only because a second, incidental scan
TOC was still dripping when the boot proceeded.

So the ROM gets its TOC, and stops anyway. Whatever it is waiting for after a
scan TOC, it is not more TOC frames.

## The ROM can be revived on demand, which narrows it further

If the ROM's media-change handler missed the edge, sending another should wake
it. Three arms, one binary, interleaved, 8 rounds each, announce forced to 8 s:

| arm | asks for the boot block | completes the boot |
|---|---|---|
| no retry | 0/8 | 0/8 |
| **removal edge then arrival** | **8/8** | 2/8 |
| arrival alone | 0/8 | 0/8 |

p = 0.00016 on the first column. A pair revives it **every time**: the edge goes
out and about four seconds later the ROM issues `start_lba=14`, then 19, 17,
25621, 26505, 25622 — the healthy sequence.

**An arrival alone is ignored completely.** Twenty-four of them across eight
rounds, and not one produced a `READ TOC`. That is consistent with your account
of `cd.device`: without a removal, there is no media *change*, so the change
handler never runs.

## The two questions that are left

**1. After a scan TOC completes, what else must the drive have done before
`cd.device` will issue the read of LBA 14?** The TOC is delivered and parsed in
both outcomes. Is there a status the ROM reads (`0x07` INFO? a subcode Q
position? a drive-ready or spin-up bit?) between parsing the TOC and issuing the
first data read — something we answer in a way that is accepted sometimes and
ignored otherwise? We can see every command frame the ROM sends and there is
nothing between the scan TOC and the silence.

**2. Why does a revived ROM stop after LBA 25622?** The rescued rounds run the
full healthy prefix — 14, 19, 17, 25621, 26505, 25622 — and then stop, where a
naturally booting round continues to LBA 12, 15, 18, 25623, 25875 and finishes.
Our guess is that the removal edge invalidated the volume `cdfs` had mounted and
the loader aborts on its next access. If that is right, is there a way to make a
drive say "look again" to the CD32 ROM that is *not* a removal — something at the
`cd.device` level rather than the media-status level?

## What is shipped, so this is not re-derived

The announce hold is now 14 s (was 8), which puts the first edge outside the
window where it is lost — 11/11 and 3/3 on re-measurement, against 1/8 at 8 s.
The retry above is **not** shipped: it revives the ROM reliably and does not
improve the completion rate, so it stays a draft. `/tmp/akiko_boot_delay` now
exists (it had been promised in three places and implemented in none), which is
how either failure is reproduced on demand.

## Also from round 4, answered

Your reading of the 13-read profile as ISO traversal, hunk loading and two large
asset streams matches what we see, and the count variation stops being
interesting. We will stop looking at read boundaries.
