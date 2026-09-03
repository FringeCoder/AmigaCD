# The CD32 is ~72 KB short of contiguous chip RAM

**Date:** 2026-09-03
**Status:** measured, cause not yet isolated. No fix attempted.
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

**One contiguous block of 1774.8 KB**, i.e. 72 KB more than we manage now.
Measurable before it is built, with `scripts/amiga-peek.py mem` against the
Flink failure and `winuae_ref` against the reference.
