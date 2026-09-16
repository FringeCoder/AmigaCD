# vAmigaTS: comparing the core against real hardware

[vAmigaTS](https://github.com/dirkwhoffmann/vAmigaTS) is a suite of 2,117 Amiga
test programs whose reference images were **photographed on real machines** —
2,967 on an Amiga 500, 18 on an A1200. That is silicon, and
`docs/core-accuracy-todo.md` in the core repository opens by saying we do not
have any.

It is also the suite T7 was written against. `Agnus/Registers/VPOS/` holds
`probe1`–`probe13`, `vprobe1`–`vprobe4`, `ersy1`, `ersy2`, `vhpos1`–`5`,
`lof1`, `lof2`, `cycle01v` and `cycleD9v`, and T7 cites "`ersy1` is correct on
only 14 of 16" and "304 probe rows" by those names. 13×16 + 4×16 + 2×16 = 304.
The arithmetic closes: those are the rows.

## Setup

The suite is **3.6 GB and is not vendored**. Clone it wherever you keep such
things:

```sh
git clone https://github.com/dirkwhoffmann/vAmigaTS
```

**A Kickstart is required** and is not ours to ship. It is Cloanto's, still sold
as Amiga Forever, and the core's own README says where a legitimate copy comes
from: dump the ROM from your own machine, or buy that. A probe booted without one
stops after printing its title — verified against Copperline's bundled AROS, not
assumed.

This is not an extra hurdle. **The core cannot run without a Kickstart**, so if
the core runs at all, the ROM is already on the card and the harness uses
whatever is configured.

**Which version, though, is a variable worth controlling.** vAmigaTS's
references were taken with **Kickstart 1.3** — every `.retrosh` in the suite says
`regression setup A500_OCS_1MB /tmp/kick13.rom`. The VPOS probes take the machine
over completely (interrupts and DMA off, no OS calls), so they *should* be
ROM-independent, and Copperline's `timing-test` README reports its display probes
verified byte-identical between AROS and a Kickstart 1.3 boot. But "should" is
not "were". If a probe disagrees with the photograph, the ROM version is the
first thing to rule out, not the last:

```sh
./scripts/vamigats.sh kickstart
```

reports which Kickstart the core is configured with, whether a `rom.key` is
present, and sizes every ROM on the card against what the core accepts.

### Amiga Forever ROMs need no decryption

Copy them across as they are, along with `rom.key`. `UploadKickstart()` in
`support/minimig/minimig_config.cpp` loads the key from the core's home
directory or the card root and decrypts on upload.

The key goes in **`/media/fat/games/Amiga/`** (or the card root). That is the
core's home directory: `user_io_get_core_path` maps both `minimig` and `amigacd`
to `Amiga` (`user_io.cpp:206`), so the two cores share ROMs and disks.

**The core identifies a ROM by its size** — that is the whole detection
mechanism, not a sanity check, and an unrecognised size fails with nothing but
`Unsupported ROM file size!`:

| bytes | what |
|---|---|
| 262,144 | 256 KB plain — **Kickstart 1.2/1.3**, the one vAmigaTS wants |
| 262,155 | 256 KB Amiga Forever encrypted |
| 524,288 | 512 KB plain — Kickstart 2.x/3.x |
| 524,299 | 512 KB Amiga Forever encrypted |
| 1,048,576 | 1 MB plain |
| 8,192 / 8,203 | A1000 boot ROM, plain / encrypted |

The encrypted forms are the plain size plus 11 bytes.

**One gap worth knowing before it wastes an evening:** there are branches for
encrypted 256 KB and 512 KB ROMs but **not** for an encrypted 1 MB one. A 1 MB
Amiga Forever ROM (1,048,587 bytes) falls straight through to `Unsupported ROM
file size!`. Use a decrypted 1 MB image, or a 512 KB ROM, in that case.

## Running a test

```sh
./scripts/vamigats.sh inventory /path/to/vAmigaTS
./scripts/vamigats.sh run /path/to/vAmigaTS Agnus/Registers/VPOS/probe6
```

`run` pushes the ADF, writes an MGL that loads the core with it mounted in df0,
loads it, waits for the display to settle, takes a screenshot and pulls the PNG
back. The whole loop goes over ssh through `/dev/MiSTer_cmd`, which accepts
`load_core` and `screenshot`; the MGL's `<file type="f" index="0">` is what
`support/minimig/minimig_config.cpp` parses as df0.

The core is found by asking the card for the newest `AmigaCD*.rbf` rather than
by a fixed path, because the dated filenames mean there is no fixed path any
more. `VAMIGATS_CORE` overrides it.

## The comparison is not automatic, and cannot be

The reference is a photograph of a CRT: different geometry, different colour,
JPEG artefacts, a visible curve. Pixel-diffing it against our framebuffer would
be measuring the camera.

What *is* comparable is the numbers the probes print — which is why they print
them in large text inside the CRT-safe area. Read them off both, and record what
the hardware said in
[`docs/reference/vamigats-groundtruth.tsv`](reference/vamigats-groundtruth.tsv).

**That file is the durable artifact here.** Once a probe's true values are
written down, a bench can hold the RTL to them forever without the suite, the
photograph, the disk or the machine. Two of nineteen probes are transcribed so
far; the rest is a reading exercise, not a research one.

The transcription is by eye, and the file says so. For `probe6` the arithmetic
happens to check it — all sixteen values step by exactly `$0A` and the wrap
closes to the colour clock — which rules out a single-digit slip, though not a
consistently misread column.

## What a real CD32 adds that nothing else can

vAmigaTS's hardware references are an **A500**. A CD32 is an AGA machine, so it
cannot reproduce those columns — running an OCS test on AGA gives different
answers by design, not by fault. What a CD32 *can* do is more valuable:

**1. It is the only ground truth that exists for the CD32 and Akiko.** There is
no test suite for Akiko's chunky-to-planar, the CD command surface, subcode
delivery or the CDTV bridges — I looked, and Copperline's own
`HARDWARE-RIG-PLAN.md` lists `akiko-sector-slot-order-no-ground-truth` as an
open item. Our core's *speciality* is the least verifiable part of it, and a
real CD32 is the only instrument that changes that. Anything measured there
would be the first such measurement, for us and for Copperline both.

**2. It supplies the AGA column the suite is thin on.** vAmigaTS has 18 A1200
references against 2,967 A500 ones. Copperline's `timing-test/README.md` notes
where its AGA probes have only one reference and says the tie "needs hardware".

**3. Copperline's harness is already built for it and has never had a machine.**
`tools/hwrig/hwrig.py` "drives the emulator over TCP and a real machine over a
UART through one code path"; `timing-test/probesrv.asm` is a serial-driven
loader that uploads a probe binary, runs it, and returns the results, so
iterating costs seconds instead of a floppy image per attempt.
`HARDWARE-RIG-PLAN.md` says phase 0 is built and validated with **no hardware
attached yet**.

### The route, with no serial cable: a burned CD

Copperline already built this and it needs no cable. `tools/cd32-probe/` there
is a small AmigaDOS program — the binary `PROBE` is committed — that renders
**30 measurement rows on screen as hex, to be photographed**, the same approach
the timing-test disk takes. `make-images.py` splices it into a copy of a
commercial CD32 disc image in place of `SYS:FSCD`.

That splice is the clever part, and it solves a problem we would otherwise have
had. A bootable CD32 disc needs a `.TM` trademark block from the Commodore
developer kit, located by a pointer in the ISO9660 primary volume descriptor —
no more ours to ship than a Kickstart. Reusing a real disc's boot structure
means never needing one. (Reportedly `CDTV.TM` works on both machines where
`CD32.TM` does not, if you ever do build one from scratch.)

**The same ISO goes to all three targets**, which is what makes the comparison
worth anything:

| target | how |
|---|---|
| real CD32 | burn it — slow, 4×–8×; CD32 drives dislike fast burns |
| our core | `scripts/cd32-probe.sh run <iso>` (mounts via `mount_cd`) |
| Copperline | `--model CD32 --insert-cd-after 0 <iso>` |

`docs/reference/cd32-probe-copperline.tsv` holds Copperline's column for all 30
rows with what each measures. **Rows 14–23 are the CD seek and read costs** —
Akiko's behaviour, the thing nothing else can measure. Rows 24–29 are bus-cycle
costs against the CIA E-clock, which bear on the memory router and DDR3 paths.

You supply the game image; Copperline's README says which variant answers which
rows, and the data-only padded variant is the one for the seek rows.

### Still true, and worth keeping in mind

- **A stock CD32 has no floppy drive**, so the vAmigaTS ADFs do not run on it
  without an SX-1/SX-32. That is fine — as above, they are the wrong machine
  class anyway. The CD route reaches what actually matters.
- **Do not use the AUX port as RS-232.** If you ever do get a cable: the 6-pin
  mini-DIN carries serial at 0–5 V logic, and the hardware references warn the
  pins may not be adequately buffered. ±12 V RS-232 straight in risks the
  machine. A USB-TTL adapter with a level-appropriate buffer, or nothing.
- **It is a recapped machine you presumably care about.** Burning a CD-R risks
  nothing; that is the other reason this is the right route.

### Where it pays off first

`akiko-sector-slot-order` — the order and timing with which Akiko delivers
sectors. We implement it, Copperline implements it, and neither of us has seen
what the chip actually does. Rows 14–23 are that question. One photograph
settles what two projects are currently guessing at, and it is the class of
thing that shows up as a game booting on hardware and not in emulation.

A row where our core and the real machine disagree is a finding about this core.
A row where **Copperline** and the real machine disagree is worth sending them:
their `HARDWARE-RIG-PLAN.md` says the rig is built and validated with no
hardware ever attached.
