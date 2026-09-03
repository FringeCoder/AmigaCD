# The CD32 is ~72 KB short of contiguous chip RAM

**Date:** 2026-09-03
**Status:** partly fixed. 21.6 KB recovered, ~50 KB still missing.
**Updated:** 2026-09-03, after the no-IDE change landed.
**Occasion:** Flink refuses to start on a stock CD32 profile (2 MB chip, no fast
RAM). WinUAE runs the same CHD on the same ROMs in the same configuration.

This is not a Flink problem. Any CD32 title that wants most of chip RAM in one
allocation will hit it.

## The short version

Flink allocates **one contiguous block of 1774.8 KB**. Our machine can offer
**1703.2 KB**. We are short by **71.6 KB**, and the shortfall is contiguity, not
consumption: total chip RAM in use matches WinUAE to within 6 KB.

## How this was measured

Two instruments, both new, both reusable:

- `scripts/amiga-peek.py` reads the running Amiga's memory on the MiSTer through
  `ss_ctrl`'s peek window. `mem` walks AbsExecBase -> exec MemList -> the
  free-chunk list.
- WinUAE is driven over GDB RSP using the `winuae-gdb.exe` fork in
  `C:\Users\Admin\Documents\mcp-winuae-emu`, launched with
  `-portable -G -s debugging_features=gdbserver -s debugging_trigger=`.
  **The fork halts the CPU at launch and resumes only on `vCont;c`; a bare `c`
  does nothing.** Three measurements were thrown away before that was noticed --
  they read the reset-time ROM overlay at `$0` and reported nonsense. Always
  check that `$0` stops reading `11144EF9` before believing anything.

The same walk runs against both machines, so the numbers are directly
comparable.

## What Flink needs

WinUAE, stock CD32, 2 MB chip, no fast RAM, game running:

```
USED $0002D788 - $001E92E0   1774.8 KB    <- Flink, one block
free $001E92E0 - $001EFD70     26.6 KB
free total 30.2 KB, used 2016.7 KB
```

It leaves **30 KB free out of 2047**. There is no slack anywhere in this title.

## Where our 72 KB goes

|                | WinUAE     | ours       | loss     |
|----------------|------------|------------|----------|
| block starts   | `$2D788`   | `$35000`   | 30.6 KB  |
| block ends     | `$1E92E0`  | `$1DECC0`  | 41.5 KB  |
|                |            |            | **72.1 KB** |

Extra allocations at **both** ends, which is why no single culprit has turned up
yet.

### The bottom, 30 KB

At the CD32 boot animation with no disc -- same ROM, the only fair like-for-like
comparison -- total usage matches (666.4 KB ours, 660.5 KB WinUAE) but the
largest block does not (1014.6 KB ours, 1058.9 KB WinUAE). Address order shows
why:

```
ours:   USED $420-$130A0 75.1K | USED $130D8-$16570 13.1K | free 38.6K | USED $20000-$30000 64K | free 1014.6K
WinUAE: USED $420-~$F800 60.5K | USED $F680 2.1K          | USED $10000-~$24F80 83.8K           | free 1058.9K
```

Our bottom allocations are ~25 KB fatter: 75.1 KB at `$420` against 60.5 KB, plus
a 13.1 KB block at `$130D8` that WinUAE does not have at all. That pushes the
next 64 KB request past `$20000`, which **strands the 38.6 KB below it** and moves
the big free block from `~$25000` to `$30000`.

A 25 KB excess costs 44 KB of contiguity. The knock-on is larger than the cause.

Both extra blocks are zeroed buffers with no strings in them, so they could not
be identified by inspection.

### The top, 41 KB

Unexamined. Our allocations begin at `$1DECD8`; WinUAE's at `$1EFD70`, 68 KB
higher. At the boot animation the top end is IDENTICAL on both machines, so
whatever differs appears during CD loading.

## The IDE hypothesis, and why it is still unproven

A stock CD32 has no IDE. Ours must enable it, because the CD32's disc travels the
userspace IDE path -- `hardfile[0]` holds the image and `ide_open` runs against
it on every config apply. Clearing `ide_cfg` takes the CD away entirely
(2026-09-03, and it shipped as a regression). Device units allocate buffers, and
the two unexplained bottom blocks are the right shape for that.

**Attempts to confirm it in WinUAE failed.** `ide=a600/a1200` produced a
byte-identical map -- a controller with nothing attached allocates nothing.
`cdimage0=<path>,image:ide:0` still booted the game normally, so WinUAE almost
certainly ignored the connection mode and stayed on `cd.device`/Akiko, as a CD32
does. Proving it needs a machine that really reaches its CD over IDE, which a
CD32 config resists being.

Do not start the "serve the CD without the IDE path" rework on this hypothesis
until it is confirmed. It is plausible and unproven, and today already produced
one shipped regression from a half-verified reading.

## What any candidate fix has to achieve

**One contiguous block of 1774.8 KB.** Measurable before it is built, with
`scripts/amiga-peek.py mem` against the Flink failure and
`scripts/winuae-ref.py` against the reference.

---

# What landed, and what is left  [2026-09-03]

|                            | largest block |
|----------------------------|---------------|
| IDE on (original)          | 1703.2 KB     |
| **IDE off, disc on Akiko** | **1724.8 KB** |
| needed                     | 1774.8 KB     |

**A stock CD32 now has no IDE and Akiko serves the disc.** Both halves were
needed and neither works alone: clearing `ide_cfg` by itself took the CD away
and shipped as a regression, because the disc's only mount WAS the IDE unit.

Two hypotheses died on the way, and neither is worth re-testing:

- **The floppy drive.** Real, measured at 26.4 KB, and NOT the cause. Removing
  it was correct anyway: a CD32 has no floppy drive.
- **The IDE unit itself.** Dropping its mount left the memory map
  BYTE-IDENTICAL -- same free, same used, same largest, every block at the same
  address. It cost nothing. What it did was keep the disc dependent on IDE,
  which is what blocked turning Gayle off, and that is why it had to go first.

One false positive to know about, because it nearly shipped as a success:
selecting a disc BY HAND after a reset produced a perfect 1774.8 KB layout and
the game ran. That was allocation ORDER, not the fix -- the same build from a
cold boot gives 1724.8 KB and fails. Reproduce from a cold boot before believing
any result here.

## The remaining ~50 KB is top-region fragmentation

|            | allocated | span                        | holes  |
|------------|-----------|-----------------------------|--------|
| WinUAE top | 64.7 KB   | `$1EFD70-$200000` = 64.7 KB | none   |
| ours top   | 60.9 KB   | `$1DEB90-$200000` = 132 KB  | ~70 KB |

**We allocate slightly LESS than WinUAE up there and spread it over twice the
space.** Ours begins 69.5 KB lower, and that is the whole remaining shortfall:
packed the way WinUAE packs it, the free region would run `$2F840-$1EFD70` =
1793 KB, comfortably past 1774.8.

The blocks, as far as inspection can take them:

    $1DEB90  28.0 KB  filled with 0A repeated, structured data at +4000.
                      THIS is the one that caps the game's region.
    $1ED168  12.6 KB  a pointer and a Kickstart ROM address ($00FA8C60)
    $1F1400   5.6 KB  counters
    $1F2C68   6.6 KB  counters
    $1FDBB8   1.1 KB  mostly zero  -- WinUAE has ONE 11.4 KB block spanning
    $1FE400   7.0 KB  dense data      exactly this range, with no hole

Even at the very top, where WinUAE makes a single allocation, we make two with a
gap between. The fragmentation is systematic, not one bad block.

**Moving just the 28.0 KB block would be enough**: the free region would then
reach `$1ED168`, which is 1782.8 KB.

## Why this needs a different instrument

None of those blocks carry names, and the peek window can only READ memory -- it
cannot trap a call. Attributing them needs a breakpoint on AllocMem with a call
stack, which the WinUAE GDB harness can do for the REFERENCE machine and which
has no equivalent on the MiSTer.

So the next step is not more of the same measurement. It is either:

- **find which blocks are OURS.** The 1.1 KB is plausibly the CD32 NVRAM and the
  7.0 KB sits where Akiko's buffers would. Both are sized by our code, so we
  control them.
- **or chase the media churn.** The akiko log shows media-present/absent
  transitions during boot. Each one makes the guest tear down and rebuild its CD
  structures, and that churn is a plausible source of the holes.
