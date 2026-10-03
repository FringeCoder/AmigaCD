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
