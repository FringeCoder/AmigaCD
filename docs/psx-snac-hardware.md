# PSX SNAC — hardware notes

Bring-up notes for a PSX pad on the SNAC user port, for the Menu core fork built
in [Plan 1](superpowers/plans/2026-08-04-psx-snac-reader-and-menu.md). The
checks below have **not yet been run against real hardware** — this document
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
| 6 | out | csync to the GunCon (`USER_OUT[6] <= ~csync` in `PSX.sv`) — unused by this plan, wired by Plan 2 |

Both ports share `CMD`/`CLK`/`DAT`/`ACK` and are selected by their own `~ATT`
line, so two pads cost one bus master, polled in sequence. This is total
overlap with the MiSTer Floppy pin map (`docs/misterfloppy-hardware.md`), which
uses the same six pins for I2C, write data, index and read data. PSX pads and a
real floppy drive can never be wanted at the same time — that is physics, not
a policy this project chose.

## Install

`menu.rbf` is built from the fork
[`FringeCoder/Menu_MiSTer`](https://github.com/FringeCoder/Menu_MiSTer), branch
`feature/snac-psx` — that is where the SNAC pad reader and the OSD-navigation
glue for the Menu core actually live; this repo only carries the userspace
poll and the docs. Copy `bin/MiSTer` to `/media/fat/MiSTer` and that fork's
built `menu.rbf` to `/media/fat/menu.rbf`. Add to `/media/fat/MiSTer.ini`:

```ini
snac_psx=1
```

**`update_all` overwrites `menu.rbf`.** Any run of `update_all` (manually or on
a schedule) replaces the forked `menu.rbf` with the stock one, silently
dropping SNAC pad support from the boot menu until it is copied back in. This
is a documentation problem, not an engineering one — the same bargain the
[`antoniovillena/MiSTer_DB9`](https://github.com/antoniovillena/MiSTer_DB9)
fork makes for the same reason. Re-apply `menu.rbf` after every `update_all`
that touches the Menu core.

## Fitter time

Measured 2026-08-05, Menu_MiSTer fork: **8m 49s**, fitter successful, worst-case
setup slack +0.253 ns, no failing paths, 0 errors, 0 critical warnings,
7,518 / 41,910 ALMs (18%).

This is why Plan 1 brings the reader up in the Menu core rather than in
AmigaCD directly: the AmigaCD core's fitter run is roughly 40 minutes, so
iterating on bus timing, ACK handling and reply IDs here instead is a
4.5x faster loop. The intent is for the module to land in AmigaCD (Plan 2)
once it has been proven against real pads here first, so the expensive runs
are spent on wiring rather than exploration — nothing has been run against
real hardware yet (see the checklist below).

## Running the checks

In order — each one gates the next, and `SW[1]` above must be confirmed off
first or everything below fails silently:

1. **Digital pad enumerates on port 1.** Plug one in, confirm the menu
   responds to the D-pad.
2. **DualShock enumerates**, in both digital and analog mode (press its ANALOG
   button). Buttons must work in both.
3. **Port 2 works**, with port 1 empty. This is the case that breaks if
   absent-pad handling regressed — a port with nothing plugged in must report
   ID 0 and contribute no buttons.
4. **Nothing plugged in: the menu does not move on its own.** Phantom keys are
   the failure this guards against.
5. **Start+Select opens the OSD**; directions repeat at a comfortable rate; ✕
   enters, ○ goes back; a core can be launched with no USB device attached at
   all.
6. **MT32-pi regression, in reverse.** Set `snac_psx=0` and confirm MT32-pi
   MIDI and audio still work in the Menu core. This plan adds a runtime mux
   over `USER_OUT`, `UART_TXD`, `AUDIO_L` and `AUDIO_R` shared with MT32-pi,
   and nobody has run that mux yet — it is the highest-consequence regression
   surface in this change, and disabling the feature must be a clean no-op.

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
- **GunCon (device ID `0x63`) is entirely unmodelled and untested.** It is
  Plan 2 work — light-pen decoding, csync on `USER_OUT[6]`, and the ID itself
  is unconfirmed against `PSX_MiSTer`'s implementation. Nothing in this plan
  exercises it.
