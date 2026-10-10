# Analogue Pocket: what of this fork can go there

Written 2026-10-10 in answer to "can the Mr Floppy and CD-ROM features, and the
fixes along the way, be added to the Pocket's Minimig core?" The setup in
question is a Pocket, its Dock, and an Analogizer.

The short answer: **a real CD-ROM drive, no. Mr Floppy, not as the hardware
stands. CD32 from disc images, possibly, but as a port of its own and not a
copy.** The reasons are in the hardware. Everything below cites the
source it was read from, so it can be rechecked when any of it changes.

Sources read:

| What | Where | Commit |
|---|---|---|
| Pocket Amiga core | `Mazamars312/Analogue-Amiga` | `8459f13` (2026-02-23) |
| Analogizer interface RTL and spec | `RndMnkIII/Analogizer` | `4b35f83` |
| Our core | `FringeCoder/AmigaCD_MiSTer` | `master` as of this date |

## How the Pocket core differs from MiSTer

On MiSTer, about half of what we did is **userspace**, in this repository,
running on the DE10-Nano's ARM under Linux: the physical disc reader, the
Akiko CD driver, NVRAM keying, the OSD pages, and save state file handling.

The Pocket has no ARM and no Linux. The Pocket core replaces `Main_MiSTer`
with a **VexRiscv soft CPU inside the FPGA** (the "MPU", 74.2 MHz, 128 KB of
RAM). Its firmware, `src/MPUBIOS/`, is a much smaller C++ program that reads
files through the APF bridge (`drivers/apf/`) and speaks the same HPS bus to the
Minimig RTL. So none of this repository carries over as it is. Any userspace
feature has to be rewritten for that MPU, within 128 KB.

## A real CD-ROM drive: no

On MiSTer the drive hangs off the ARM's USB, and Linux gives us `/dev/sr0`.
On the Pocket there is no path from a core to a drive:

- **The Pocket has no USB host the core can see.** The Dock's USB ports are
  handled by the Dock's firmware, and reach a core only as controller,
  keyboard and mouse input. There is no mass storage passthrough in openFPGA.
- **The Analogizer's SNAC port has seven signal lines** (`SNAC_OUT1`, `OUT2`,
  `IO3`, `IN4`, `IO5`, `IO6`, `IN7` in `openFPGA_Pocket_Analogizer_SNAC.sv`).
  A parallel ATAPI drive needs about 28.

The only route would be new hardware: a microcontroller hosting the USB drive
and streaming sectors to the core over a few SNAC lines. That is a separate
hardware and firmware project, not a port, and nobody has built it.

## Mr Floppy through the Analogizer: no, as the hardware stands

Both use a USB3-A "SNAC7" connector, so on the face of it Mr Floppy should
plug in. It does not work, because the Analogizer fixes each line's direction
(its spec says so: "it does not have freedom to configure SNAC signals as input
or output on an individual level"), and Mr Floppy needs directions it does not
offer.

The mapping between MiSTer `USER_IO` numbers and the Analogizer's names was
confirmed against the PSX pinout, which both sides document
(`docs/psx-snac-hardware.md` here; the Conf. B column of the table in
`openFPGA_Pocket_Analogizer_SNAC.sv`). Mr Floppy's needs come from the header
of `MiSTerFloppyRAWIO.v`:

| USER_IO | USB3 | Mr Floppy needs | Analogizer, switch A | Analogizer, switch B |
|---|---|---|---|---|
| 0 | D+ | I2C SDA, **in and out** | `OUT2`, out only | `OUT2`, out only |
| 1 | D- | write data, out | `OUT1`, out ✓ | `OUT1`, out ✓ |
| 2 | TX- | index *or* head select (cable dependent) | `IO6`, in | `IO6`, out |
| 3 | GND_d | I2C SCL, out | `IO5`, out ✓ | `IO5`, in ✗ |
| 4 | RX+ | read data *or* write gate | `IN4`, in | `IN4`, in |
| 5 | RX- | head select *or* index | `IO3`, in | `IO3`, out |
| 6 | TX+ | write gate *or* read data | `IN7`, in | `IN7`, in |

**SDA is the blocker.** It lands on an output-only pin in both switch
positions, so the core can never read the board's port expander. That expander
is where track 0, write protect and disk change come from (`o_nTrk00`,
`o_nPin34` in `MiSTerFloppyRAWIO.v`), and the drive type detection reads it
too. A drive whose track 0 cannot be read cannot seek.

Counting what is left: Mr Floppy needs five lines that can drive (one of them
bidirectional) and two inputs. The Analogizer has at most four lines that can
drive in any one wiring (`OUT1`, `OUT2`, and the two individually switched
cartridge pins 30 and 31 behind `IO3`/`IO5`/`IO6`). So even a custom rewiring
harness falls one output short for read and write. A harness that drops
writing, fixes the cable orientation, and moves SDA onto pin 31 might work
read-only. Whether the Analogizer's own buffering lets pin 31 go both ways has
not been checked against its schematic (`hardware/SCH` in its repo), and
nobody has tried it.

A Mr Floppy board revision, or an Analogizer revision with a free bidirectional
line, would change this answer.

## CD32 from disc images: possible, but a port in its own right

This is the CD feature that is *not* tied to MiSTer's hardware: CD32 games from
CUE/BIN images on the SD card. The Pocket core does not have it today.

- Its `akiko.v` is 61 lines and does chunky-to-planar only, with no CD. Ours is
  1,634 lines, plus `akiko_hps_bridge.v`, `akiko_nvram.v` and `cdda.v`.
- `src/MPUBIOS/drivers/cue/` holds a CUE parser and seek-time model, but
  nothing calls it: its `#include` is commented out in `app/core.h:32`. It
  looks lifted from another CD core and left for later, and is a starting
  point at best.

What it would take:

1. **Fit.** Our core uses 70% of the DE10-Nano's ALMs
   (`docs/core-accuracy-todo.md` in the core repo), about 29,000 of 41,910,
   though some of that is MiSTer's framework. The Pocket's FPGA is a
   `5CEBA4F23C8`, which has 18,480. The stock Pocket core already fits AGA, a
   68020 and the MPU into that, and its fit report is not committed, so its
   headroom is unknown. **The first job is a fit of the Pocket core with our
   Akiko added, before any other work.** If it does not fit, nothing else here
   matters, and adding the Analogizer module takes area too.
2. **RTL.** Port `akiko.v`, `akiko_hps_bridge.v`, `akiko_nvram.v` and
   `cdda.v`, and the CD32 memory map changes in `gary.v` and `minimig.v`, onto
   the Pocket's Minimig. Both report Minimig 1.2.2 in `minimig_version.vh`,
   but they have been patched differently since: the Pocket's `gary.v` has no
   Akiko decode at all, and ours does.
3. **MPU firmware.** Rewrite the CD driver (`support/minimig/akiko_cd32.cpp`
   here) for the VexRiscv: TOC, sector delivery, CDDA feed, NVRAM. Within
   128 KB of RAM this means CUE/BIN only. CHD decompression is out of reach.
4. **APF plumbing.** Data slots for the CUE and its BIN tracks in `data.json`,
   and the MPU reading them through the APF bridge.

## Which of our fixes apply

Two kinds, and they go different ways:

- **Userspace fixes** (most of what is in this repository: the Akiko driver,
  NVRAM keying, OSD row guard, MGL handling, save state files) are fixes to
  code the Pocket does not run. They carry over only as part of rewriting
  that feature for the MPU.
- **RTL fixes in the core repo** that touch Minimig modules the Pocket also has
  (`agnus_*`, `paula_*`, `denise_*`, `gary.v`, `minimig.v`) could be ported
  one by one. Each needs checking against the Pocket's copy first, since the
  two trees diverged from upstream at different points.

## If this is taken up

The work belongs in a fork of `Mazamars312/Analogue-Amiga` (GPL-3.0, as ours
is), not in either of our repositories. Order: fit first, then the PSX pad
reader through the Analogizer (it needs only the pins the Analogizer already
supports for PSX, and is the cheapest proof that the Analogizer is wired up),
then Akiko and CD images.
