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

## It no longer reproduces

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
