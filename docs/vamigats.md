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

**A Kickstart is required** and is not ours to ship. The tests' own reference
scripts say so (`regression setup A500_OCS_1MB /tmp/kick13.rom`), and a probe
booted without one stops after printing its title — verified here against
Copperline's bundled AROS, not assumed. The core needs a Kickstart to run at
all, so this is not an extra hurdle, but the ROM must already be on the card.

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

### The practical obstacles, stated plainly

- **A stock CD32 has no floppy drive.** The vAmigaTS tests are ADFs. The routes
  are: burn tests to CD, use an SX-1/SX-32 expansion (which adds a floppy,
  keyboard, IDE and a proper serial port), or upload over serial with
  `probesrv`, which is what it exists for.

- **The AUX port is serial at 0–5 V logic, not RS-232.** The CD32's 6-pin
  mini-DIN carries keyboard data/clock plus serial TX (pin 2) and RX (pin 6),
  and the hardware references warn the pins may not be adequately buffered —
  connecting ±12 V RS-232 directly risks damaging the machine. A USB-TTL
  adapter with a level-appropriate buffer is the route; a straight serial cable
  is not.

- **It is a recapped machine you presumably care about.** None of the above is
  worth a risk to it. The measurements are valuable; the CD32 is less
  replaceable than the data.

### Where it would pay off first

`akiko-sector-slot-order` — the order and timing with which Akiko delivers
sectors. We implement it, Copperline implements it, and neither of us has ever
seen what the chip actually does. One capture would settle a question two
projects are currently guessing at, and it is the kind of thing that shows up as
a game that boots on hardware and not in emulation.
