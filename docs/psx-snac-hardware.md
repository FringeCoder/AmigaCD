# PSX SNAC — hardware notes

Bring-up notes for a PSX pad on the SNAC user port, for the Menu core fork built
in [Plan 1](superpowers/plans/2026-08-04-psx-snac-reader-and-menu.md). The
**It works.** Verified on hardware 2026-08-06: a PSX pad on SNAC port 1
navigates the Menu core and launches a core with no USB device attached, and
`snac_psx=0` / `=1` both behave correctly. That is the capability no MiSTer core
had before — [SNAC input is not implemented in the menu
core](https://misterfpga.org/viewtopic.php?t=7486).

The remaining checks below have **not** been run. Each is marked. This document
is written for the person about to run them, not as a record that they passed.
The design background is in
[the design doc](superpowers/specs/2026-08-04-psx-snac-design.md).

## `SW[1]` on the DE10-Nano must be off

Check this physical slide switch **before touching anything else** — exactly
the same trap as [MiSTer Floppy](misterfloppy-hardware.md#sw1-on-the-de10-nano-must-be-off),
and it costs the same hours if skipped.

`sys_top.v` hands USER_IO pins **2, 4 and 5** to HDMI I2S audio when `SW[1]` is
on, and forces those inputs high. On the PSX pinout below those are `CMD`,
`DAT` and `CLK` — the entire bus. With the switch on, the pad simply never
answers: no crash, no error, no OSD message. It looks exactly like "nothing is
plugged in."

This is the physical switch on pin W24, not an `.ini` setting. `hdmi_audio_96k`
is unrelated (96 kHz sample rate selection); this is the slide switch itself.

## Pin map

From `MiSTer-devel/PSX_MiSTer`'s `PSX.sv`, the de facto standard every PSX SNAC
adapter is wired to (`docs/superpowers/specs/2026-08-04-psx-snac-design.md`):

| USER_IO | Dir | Signal |
|---|---|---|
| 0 | out | `~ATT` port 2 |
| 1 | out | `~ATT` port 1 |
| 2 | out | `CMD` |
| 3 | in | `ACK` |
| 4 | in | `DAT` |
| 5 | out | `CLK` |
| 6 | out | csync to the GunCon (`USER_OUT[6] <= ~csync` in `PSX.sv`) — **wired in the AmigaCD core as of Plan 2**, from `hs & vs` (both active low); driven only while a port is in light-gun mode, otherwise the reader's own idle level goes out |

Both ports share `CMD`/`CLK`/`DAT`/`ACK` and are selected by their own `~ATT`
line, so two pads cost one bus master, polled in sequence. This is total
overlap with the MiSTer Floppy pin map (`docs/misterfloppy-hardware.md`), which
uses the same six pins for I2C, write data, index and read data. PSX pads and a
real floppy drive can never be wanted at the same time — that is physics, not
a policy this project chose.

## Install

`./scripts/stage-output.sh --binary` collects everything into `output/`, always
overwriting. It writes a manifest alongside the files; this section is the why.

| File | Goes to |
|---|---|
| `AmigaCD_<date>.rbf` | `/media/fat/_Computer/` (or wherever you keep cores) |
| `menu.rbf` | `/media/fat/menu.rbf` |
| `MiSTer_AmigaCD` | `/media/fat/MiSTer_AmigaCD` |
| `MiSTer` | `/media/fat/MiSTer` — replaces the stock main binary |

**The two binaries are the same build, byte-identical, and both are needed.**
`main=` only redirects the AmigaCD core (`docs/deployment.md`), so the boot menu
keeps running whatever is at `/media/fat/MiSTer` — and the Menu core's SNAC pad
navigation lives in *userspace*. Install only `MiSTer_AmigaCD` and the pad works
in AmigaCD and does nothing in the menu, with no error to explain why.

The AmigaCD core builds as `AmigaCD.rbf` and is staged as
`AmigaCD_YYYYMMDD.rbf` — the date is what the MiSTer menu shows beside the core
name, and it comes from the filename or not at all (`docs/deployment.md`). It
built as `Minimig.rbf` until the Quartus project was renamed on 2026-09-15, so a
tree that has not been rebuilt since still has the old one; `stage-output.sh`
accepts either. The filename is otherwise cosmetic: the core's identity comes
from `CONF_STR`, which is why `docs/deployment.md` spends a section on that too.

`menu.rbf` is built from the fork
[`FringeCoder/Menu_MiSTer`](https://github.com/FringeCoder/Menu_MiSTer), branch
`feature/snac-psx` — that is where the SNAC pad reader and the OSD-navigation
glue for the Menu core actually live; this repo carries the userspace poll, the
shared reader module and the docs.

`/media/fat/MiSTer.ini` needs **both** of these, and half-doing it is the common
mistake:

```ini
[AmigaCD]
main=MiSTer_AmigaCD

snac_psx=1
```

The first selects our binary for the AmigaCD core. The second enables the pad in
the boot menu and is read by whichever binary is running at boot. AmigaCD's own
SNAC settings are **not** here — they live in `AmigaCD.cfg` via the OSD, so they
can differ per saved configuration.

**`update_all` overwrites `menu.rbf`.** Any run of `update_all` (manually or on
a schedule) replaces the forked `menu.rbf` with the stock one, silently
dropping SNAC pad support from the boot menu until it is copied back in. This
is a documentation problem, not an engineering one — the same bargain the
[`antoniovillena/MiSTer_DB9`](https://github.com/antoniovillena/MiSTer_DB9)
fork makes for the same reason. Re-apply `menu.rbf` after every `update_all`
that touches the Menu core.

## Fitter results

Both cores as they ship. **Any further RTL change invalidates these and needs a
re-run** — the spec makes worst-case setup slack the binary pass/fail gate, not
an FYI.

| Core | Commit | Time | Worst slack | ALMs |
|---|---|---|---|---|
| Menu (`FringeCoder/Menu_MiSTer`) | `797d1c3` | 7m 43s | **+0.504 ns** | 7,541 / 41,910 (18%) |
| AmigaCD (`feature/misterfloppy`) | `624dc22` | 22m 40s | **+0.084 ns** | 23,779 / 41,910 (57%) |

Both: TNS 0.000 on every clock, 0 errors, no failing paths. Measured 2026-08-07.

The Menu figure supersedes the 2026-08-06 run (6m 52s, +0.478 ns); that build
predates the GunCon gate widening in `rtl/snac_psx.v`, which the Menu core
carries only because the module is byte-identical across both cores.

**The AmigaCD core needs `SEED 2`, pinned in `AmigaCD.qsf`.** This is not
cosmetic: seed 5 closes timing for the pad-only build at +0.400 ns and *fails*
at −0.180 ns once the light pen and read path are added. Seeds do not transfer
across netlists. The core also required retiming the SDRAM state decode before
it would close at all — `docs/sdram-timing-headroom.md` is the whole story, and
names the fix to reach for when this returns.

**Never pass `--seed` or `--set` to `quartus_fit` in this repo.** They are not
per-invocation: they rewrite `AmigaCD.qsf` permanently and expand its `source`
includes inline, leaving the design declared twice.

Plan 1 brought the reader up in the Menu core rather than in AmigaCD precisely
because of the time column above — a 4.5x faster loop for iterating on bus
timing, ACK handling and reply IDs, so the expensive runs are spent on wiring
rather than exploration.

## Running the checks — Plan 1, the Menu core

In order — each one gates the next, and `SW[1]` above must be confirmed off
first or everything below fails silently:

1. **Digital pad enumerates on port 1.** Plug one in, confirm the menu
   responds to the D-pad. — **PASSED 2026-08-06.**
2. **DualShock enumerates**, in both digital and analog mode (press its ANALOG
   button). Buttons must work in both. — *not yet run.* Note the analog half is
   only about the axes reaching the HPS; nothing consumes them until Plan 2.
3. **Port 2 works**, with port 1 empty. This is the case that breaks if
   absent-pad handling regressed — a port with nothing plugged in must report
   ID 0 and contribute no buttons. — *not yet run.*
4. **Nothing plugged in: the menu does not move on its own.** Phantom keys are
   the failure this guards against. Leave it sitting in the core browser for a
   few minutes: that is the observable for both the floating-bus flicker and a
   glitch slipping past the ACK filter. — *not yet run.*
5. **Start+Select opens the OSD**; directions repeat at a comfortable rate; ✕
   enters, ○ goes back; a core can be launched with no USB device attached at
   all. Include diagonal D-pad rolls (e.g. Right to Down) in this check,
   watching for a dropped move — that exercises the same-poll key-ordering fix
   in `support/snac/snac_psx.cpp` and its one known-unfixable residual case
   (a press and release of the same button landing in adjacent ~1.6 ms RTL
   samples, both inside the framework's 20 ms debounce). Repeat rate itself is
   whatever the framework uses on every other screen — 500 ms initial delay,
   50 ms interval (`REPEATDELAY`/`REPEATRATE` in `osd.h`), not the 500/100 the
   design spec once assumed — worth a feel check since the spec's number was
   never the one actually shipping.
6. **MT32-pi regression, in reverse.** Set `snac_psx=0` and confirm MT32-pi
   MIDI and audio still work in the Menu core. This plan adds a runtime mux
   over `USER_OUT`, `UART_TXD`, `AUDIO_L` and `AUDIO_R` shared with MT32-pi,
   and it is the highest-consequence regression surface in this change:
   disabling the feature must be a clean no-op. — **PARTLY PASSED 2026-08-06:**
   toggling `snac_psx` between 0 and 1 behaves correctly and the menu is
   unaffected either way, so the mux itself switches cleanly. **MT32-pi's own
   MIDI and audio have NOT been confirmed against real MT32-pi hardware** —
   that half of the check is still open, and is the one an MT32-pi user cares
   about.

## Running the checks — Plan 2, the AmigaCD core

None of these has been run. **Order matters here more than in Plan 1**, because
the first check guards a regression that would make every later result
meaningless. `SW[1]` off first, as always.

1. **Boot an ordinary game and confirm nothing changed.** Plan 2 puts a mux in
   front of `VPOSR`/`VHPOSR` in `agnus_beamcounter.v`, and most Amiga software
   reads the beam position every frame. If that mux is not a pure pass-through
   with the light pen disabled, this is where it shows — and nothing below would
   mean anything. — *not yet run.*
2. **Chip RAM integrity under blitter load.** A demo or game that hammers the
   blitter, run long enough for a rare fault to surface. This is the check for
   the `sdram_ctrl.v` retiming, which is a **memory controller change with no
   simulation behind it**. — *not yet run.*
3. **CD32 and CDTV both boot.** Chip/CPU slot arbitration differs between them,
   and Akiko DMA shares the arbiter the retiming sits next to. — *not yet run.*
4. **MT32-pi still works**, then **MiSTer Floppy still detects.** Both share the
   user port with SNAC and both were touched — `enable_mister_floppy` changed
   from a bare signal to an explicit comparison when the tenant field widened to
   two bits. `floppy_ext_drive` also sits on one of the paths that was failing
   before the retiming. — *not yet run.*
5. **An old `AmigaCD.cfg` migrates**, with both SNAC ports reading Off rather
   than garbage. Third on-disk layout, third chance at that trap. — *not yet run.*
6. **CD32 pad, all seven buttons**, in a game that uses them: Red, Blue, Yellow,
   Green, Forward, Reverse, Play. **This is the check that validates the button
   mapping, and simulation structurally cannot.** The table in `rtl/snac_cd32.v`
   was derived from the shift-register load order in `userio.v`, not from
   documentation; the testbench asserts the same table the module implements, so
   19 green checks prove only self-consistency. A reviewer independently
   confirmed the *positional* order from the RTL, but which physical button name
   sits at each position — and which PSX button should map to it — has no
   support anywhere in the tree. — *not yet run.*
7. **A USB pad still works alongside a SNAC pad.** They are OR-ed, not muxed. —
   *not yet run.*
8. **Analog stick moves the mouse pointer.** Press the pad's ANALOG button
   first — the axes only carry data for device id `0x73`. Check both axes move
   the right way, and that a centred stick does not drift. — *not yet run.*
9. **Light pen, last.** CRT on direct analog output, scaler off, a
   Phazer-compatible game. Needs `USER_IO[6]` carrying csync, which is only
   driven while a port is in light-gun mode. — *not yet run.*

Also needed, in the Menu core rather than AmigaCD: **OSD navigation by pad still
works**, because `rtl/snac_psx.v` changed underneath a core that was already
verified on hardware.

## Known behaviour

Both pads are OR-ed together so that "whichever pad is to hand" works. Two
consequences follow directly from that and are expected, not bugs:

- **Start+Select can be split across two pads.** One pad holding Start and
  the other holding Select opens (or closes) the OSD exactly like a single
  pad holding both.
- **A button held on one pad suppresses that button's press edge on the
  other.** Edges are detected on the OR of both pads, so if pad A is already
  holding, say, ✕, pad B pressing ✕ produces no new edge until pad A's ✕ is
  released. This is a shared-state artifact of OR-ing two pads into one
  logical input, not a per-pad bug.

## Things simulation cannot settle

Flagged in review, not yet checked against a real controller:

- **Hot replug at the electrical level.** What the reader does if a pad is
  unplugged and replugged while the core is running — whether it drops back
  to "absent" cleanly or needs a full poll cycle to recover.
- **Floating-bus flicker.** Whether a floating bus can momentarily read as a
  plausible-looking byte and produce a one-frame flicker — a false-positive
  button or ID read that clears itself on the next poll rather than a
  persistent phantom key.
- **GunCon: the protocol is now confirmed, the hardware still untested.** The
  reply layout was settled from `MiSTer-devel/PSX_MiSTer`'s `rtl/joypad_pad.vhd`
  — device id `0x63`, six payload bytes (buttons LSB/MSB, X LSB/MSB, Y LSB/MSB),
  X in 9-bit 8 MHz dot-clock ticks, Y in 9-bit scanlines, off-screen sentinel
  `X=0x0001, Y=0x000A`, trigger on buttons-MSB bit 5 active low. The simulation
  suite covers a GunCon frame (42 assertions, both clock rates), and Plan 2
  wires csync and the light-pen latch. **No real gun has been connected**, and
  the conversion from gun ticks to Amiga `hpos`/`vpos` carries calibration
  offsets that default to zero and have never been fitted to a real display.
- **The gun's coordinates were nearly lost to a one-line gate.** `axes` was
  populated only for device id `0x73`, so a GunCon read the centred sentinel and
  its position never reached software. Found during Plan 2, fixed by widening
  the gate to accept `0x63`. Worth remembering that the reader silently
  substitutes a plausible value for anything it does not recognise — a device
  that enumerates but reports centred axes is the signature.
- **A pad whose ACK latency exceeds the 100 us timeout reads as ABSENT, with
  no diagnostic.** That is indistinguishable at the OSD from `SW[1]` being on,
  a bad cable, or an empty port — all four look identical: ID 0, no buttons.
  If a real pad enumerates as absent, raise `ACK_TIMEOUT_US` in
  `rtl/snac_psx.v` before assuming the reader itself is broken, and scope
  `USER_IO[3]` (ACK) against `USER_IO[5]` (CLK) to see how late the real pad
  is acking relative to the 100 us window. This is the highest-probability
  bring-up surprise — simulation's pad model always acks in 12 us and has
  never exercised this path.
- **ACK is now a control input, not just data.** A 2-flip-flop synchroniser
  plus a ~200 ns glitch filter (`ACK_FILTER_CYCLES` in `rtl/snac_psx.v`) reject
  noise on it, but that threshold has never met a real cable. If it is wrong,
  the symptom is a pad that enumerates fine but occasionally produces wrong
  buttons — not a hang or an absent port, which makes it easy to mistake for a
  core or USB-side bug instead.
- **The 20 us ATT setup and the ACK-driven inter-byte gap have never met a
  real controller.** Both are estimates of the PSX protocol taken from
  documentation and the DualShock's published tolerances, not measurements off
  actual hardware. If a real pad is pickier than the model, symptoms would
  show up as intermittent enumeration or occasional wrong bytes rather than a
  clean failure.
