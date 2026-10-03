# The first boot after a core flash: what it tracks, and that it stopped reproducing

Earlier revisions of this file called this a deterministic core failure and
offered three explanations in turn. All three were wrong, and the failure itself
no longer reproduces. This is what the evidence actually supports.

## The one thing that tracks it perfectly

Across fourteen captured rounds, with no exceptions:

| | mounts in the round | `set_cd_path` runs | `NVR loaded` | outcome |
|---|---|---|---|---|
| failed | **1** | **0** | **0** | 5 of 5 |
| booted | **2** | 1 | 2 | 9 of 9 |

A round in which the disc is announced once, `akiko_cd32_set_cd_path()` takes its
same-slot early return, and the per-game NVRAM is therefore never loaded into the
BRAM the flash just wiped — fails. A round with two mounts, a real path change
and an NVRAM load — boots.

That is a correlation over fourteen rounds, not a mechanism. It is written down
because it is the only invariant that survived, and because the obvious reading
is testable: a CD32 title booting against an EEPROM full of whatever the FPGA
powered up with.

## IT REPRODUCES AGAIN, on demand, 6 times in 8

Found 2026-10-03, by accident, while testing something else. With the announce
hold forced to 8 s the ROM asks for nothing at all; a removal-then-arrival media
pair revives it every time, and **the revived ROM runs six arms -- 14, 19, 17,
25621, 26505, 25622 -- and stops, with no loader read at lba 12**. That is the
signature in `akiko-push-collision.md` for the first boot after a flash, exactly.

```bash
echo 8000 > /tmp/akiko_boot_delay        # put the announce back in the lost window
echo pair > /tmp/akiko_reannounce_mode   # revive the ROM from it
# load the core, mount the disc, watch for the stop after 25622
```

About a hundred seconds a round, six failures in eight, with
`scripts/setting-ab.py` for the round loop. Both files are read once per core
load and both are logged, so a round can always be checked against the state it
claims. The re-announce ships OFF; it is a hook, not behaviour.

**This is what the whole investigation was missing.** Every fix proposed for this
failure -- including the ones in this session -- was untestable because the
failure would not come back. It now comes back on demand, so the next person can
test instead of argue.

The section below is kept as written, because the thing it describes was true:
the failure stopped appearing by itself, and nothing in the table re-enabled it.
What re-enables it is a media edge delivered into the window where the ROM loses
it, which is not something any of those controls varied.

## It no longer reproduces (as of 2026-10-02, superseded by the section above)

Fifteen consecutive first boots have succeeded, across:

- three binaries — master, a build with a stall probe, a build with the swap fix;
- two mount sequences — the old "take the highlighted disc" and a directory-aware
  one that selects by name;
- insertion 0 s and 40 s after the core came up;
- the netlist that failed 9 of 9 the day before.

So there is nothing to test a fix against, and none is proposed here. Whatever
re-enables it, the table above is the thing to check first.

## 2026-10-03: two more arms, and the sector-count lead withdrawn

Thirty-two further consecutive boots, in two interleaved eight-round A/Bs on one
binary and one bitstream:

| arm | | arm | |
|---|---|---|---|
| `mmcd_fast`, 588 sectors/s | **8/8** | `mmcd_2x`, 150 sectors/s | **8/8** |
| rewrite the `.rbf`, then load | **8/8** | load the same file | **8/8** |

The rate was worth testing because the machine's CD32 profile had been left at
2x, with the config written the day after the failure stopped reproducing -- a
four-times change on the sector path that was in nobody's log. It is now logged
once per core load, because a round whose settings are unknown cannot be used as
evidence. It is not the explanation.

The flash arm was worth testing because the failure is always described as the
first boot after a *flash*, and `load_core` on a file already on the card is not
that. Writing the same bytes back and syncing first changes nothing; both arms
were verified by md5 to load identical bitstreams.

**The 20-versus-24 sector lead is withdrawn.** It read: a failing round takes
about twenty sectors before the BIOS pauses, a booting one at least twenty-four.
The "at least" was the instrument -- a trace capped at 24 lines per read -- so
the comparison was a cap against a real number. With an exact count taken at the
pause, sixteen booting rounds give:

```
first read pauses at: 21 22 22 22 22 22 22 22 23 25 25 25 25 26
then:                 18, 21-22, 8, 6-7, 20-21, 1, 1-2, 4, 4, 4, 258, 189
```

A boot is **thirteen** reads, not the six we had been looking at; a pause after
about twenty sectors is the sixth read of every successful boot; and the first
read's count varies from 21 to 26 with no effect on anything. The phantom drain
and the single `CHECKSUM FAIL` also appear in every one of the 32 boots, so they
are not discriminators either.

## One failure that does reproduce: the announce lands too early

First reported here as "the disc already in the bay never boots, 0 of 3". That
was true of the rounds taken and **overstated as a rate** -- a later three rounds
on the same path booted once. The real variable is not the mount route but WHEN
the first media-present announce lands, and at the shipped hold of 8 s the boot
is marginal rather than impossible:

| announce at | booted |
|---|---|
| 0 s | 0/2 |
| 8 s | 1/8 (the shipped default) |
| 10 s | 1/2 |
| 12, 14, 16, 26 s | 11/11 |

Measured across two independent mount routes -- the disc in the config, so the
core mounts it on load, and the `mount_cd` hook at a controlled delay -- which is
why the rows pool. 1 of 8 against 11 of 11 is p < 0.001 by Fisher.

The ROM looks identical up to the failure: `INFO`, then two non-data `PLAY`
frames (`cmd[7]=0x03`), one before the announce and one after, in rounds that
boot and rounds that do not. In a failing round the data read (`cmd[7]` bit 7)
never follows and the ROM stops asking. Nothing is refused host-side; it is
never asked for.

**Fixed by raising the hold to 14 s**, which also moves further from the chip-RAM
edge the hold exists for (`chip-ram-shortfall.md` measured 6 s failing, 8 s
working, and said to raise it if trouble appeared). Re-measured with the fix
deployed: the two arrival times that were 0/2 each are 3/3 each, and the core
reports a 14000 ms hold from a config that still stores 8, so the load-path
migration works.

This is **not** the first-boot-after-a-flash failure the rest of this file is
about: that one still does not reproduce, and 32 boots today were clean at both
sector rates and with and without a real flash. It is a different failure that
was found while building an instrument for the first one.

## An early announce fails in TWO different ways

Both were found with the hold forced short through `/tmp/akiko_boot_delay`, which
is the only way to reproduce either on demand now that the default is 14 s.

**At an 8 s announce the ROM asks for nothing at all.** No `PLAY DATA` with the
data bit set, ever; no timeout, no retry, no error frame. It sits on the CD32
animation, which a screenshot confirms is the idle animation and not a crash.
Nothing is refused host-side, because nothing is asked for.

**At a 0 s announce the ROM reads the disc but never boots it.** Thirteen reads,
the same count as a healthy boot, and they go to the wrong place: the first is
**LBA 16**, the ISO primary volume descriptor, where a healthy boot starts at
**LBA 14**, the CD32 boot block. LBA 14 is read once, later, and the game loader
at LBA 12 is never reached. Eight rounds out of eight, identical. That reads like
the ROM treating the disc as a data volume to mount rather than a disc to boot.

So "announce too early" is not one failure with a threshold; it is two failures
with different signatures, and the 12 s boundary in the table above is where the
first of them stops happening.

## Telling the guest again revives it, and is not enough

If the edge is lost rather than mistimed, sending another should fix it, and it
half does. Eight rounds an arm, one binary, interleaved, hold 8000 ms, the arms
separated only by a flag file:

| | ROM asks for the boot block | completes the boot |
|---|---|---|
| re-announce on | **8/8** (it fired in 6) | 2/8 |
| re-announce off | 3/8 | 3/8 |

p = 0.026 on the first column. In every round where the ROM was dead the retry
revived it: removal edge, arrival edge, and about four seconds later `start_lba=14`
followed by 19, 17, 25621, 26505, 25622 -- the healthy sequence. **And then it
stops**, without going on to LBA 12 and the rest of the load. A guess worth
testing: the removal half invalidates the mounted volume and the loader aborts.

Left unmerged as a draft. It converts a silent hang into a boot attempt that
stalls, which is a better diagnosis and not yet a fix.

**A measurement warning that cost three builds.** At a short hold the boot is
marginal enough that the INSTRUMENT moves it: eight rounds failed on one build
and the next round booted on a build differing only by log lines, because
`akiko_diag` is a synchronous write on the path being timed. Two builds cannot be
compared here. One binary with a runtime flag can.

## The TOC is not where it fails, and the ROM is revivable

The two `cmd[7]=0x03` frames either side of the announce are the ROM's **`READ
TOC`** commands -- `op=0x04`, bit 7 clear, MSF 00:00:00, so seekpos < 0, which is
the scan-TOC trigger we already implement (`akiko_cd32.cpp`, citing WinUAE
akiko.cpp:1095-1097). Identification from Gemini; it is the first correct naming
of those frames in this investigation.

The obvious mechanism that follows -- a failing round never receives its TOC, so
`cd.device` never learns track 1 geometry and never reads LBA 14 -- is **false**.
Counted across a 16-round run:

| | TOC frames pushed | `TOC drip complete` | asks for LBA 14 |
|---|---|---|---|
| failing | exactly **12** | **yes**, every time | never |
| booting | 20-27 | no (a second drip still running) | yes |

Twelve is the whole TOC: four points at three repeats. A failing round completes
it, logs the completion, and the ROM then does nothing. Booting rounds show more
frames only because an incidental second scan-TOC was still dripping.

**The ROM can be revived on demand.** Three arms, one binary, interleaved, eight
rounds each, announce forced to 8 s:

| arm | asks for the boot block | completes the boot |
|---|---|---|
| no retry | 0/8 | 0/8 |
| removal edge then arrival | **8/8** | 2/8 |
| arrival alone | 0/8 | 0/8 |

p = 0.00016 on the first column. A pair revives it every time, about four seconds
later, into the normal read sequence. An arrival alone is ignored completely --
twenty-four of them across eight rounds, not one producing a `READ TOC`, because
without a removal there is no media *change* for the handler to see.

Completion is unchanged: pooled over both runs, 3/14 with the retry and 3/14
without. The rescued rounds stop after LBA 25622 where a natural boot continues
to 12, 15, 18, 25623, 25875. So the retry stays a draft (#106): it converts a
silent hang into a stalled boot attempt, which is a better diagnosis and no
better outcome for the user.

What is left to explain: after a completed scan-TOC, what else must the drive
have done before `cd.device` will read LBA 14? Every command frame the ROM sends
is visible, and there is nothing between the scan-TOC and the silence.

## Five explanations, each falsified by its own control

Recorded so none of them is re-derived:

- **A missing or extra `CDINT_PBX`**, the project's leading theory for months. A
  probe reading the FPGA's own completion counter at the stall shows
  `pushed=21 fpga_ctr=20` — one *behind*, which is the normal staged sector, not
  ahead. The completion count is right.
- **The swap removal edge.** The control build had it and booted 3 of 3.
- **Insertion timing.** 0 s and 40 s both boot 2 of 2.
- **Closing the OSD on top of the mount.** Both arms boot 2 of 2.
- **The RTL.** Master, with none of this session's host changes, boots 3 of 3 on
  the same netlist.

Earlier revisions also blamed, and this file withdrew in turn, a stale PBX ship,
cold page cache (the CHD is 1.7 MB and entirely covered by the 16 MB `fadvise`),
the 8 s mount delay (tested with the setting *saved*, verified from the core's
own boot-settle line), and a "60-sector walk" that was a per-process debug
counter.

## The measurement lessons, which cost more than the bug

Four separate artifacts read as findings during this hunt:

1. **A log cleared after the lines of interest had been written** — which made
   `set_cd_path` and the NVRAM load look absent, and sent the first diagnosis
   down the wrong path.
2. **An over-wide capture window** — `CHECKSUM FAIL` counts of 5 and 8 that came
   from a trace spanning a flash, several OSD interactions and more than one boot
   attempt. Per round it is 1, in failing and working rounds alike.
3. **A process-lifetime diagnostic budget** — `DBG sec_req#` capped at 60 for the
   life of a binary that `load_core` restarts.
4. **Falsifying a true hypothesis with the wrong window.** The NVRAM theory was
   dropped on the strength of an uncleaned trace whose `set_cd_path` line belonged
   to a different mount. Per-round logs put it back as the only surviving
   correlation.

The rule all four share: **check an instrument's window and budget before
believing a difference it reports** — and before disbelieving one.
