# PSX SNAC support — design

Status: design approved 2026-08-04. Not implemented, not compiled, no RTL built.

Today the SNAC user port has two possible tenants — MT32-pi and MiSTer Floppy —
and a PlayStation pad plugged into it does nothing under any core but PSX. This
adds a third tenant: PSX controllers read natively on the user port, driving the
CD32 pad, an Amiga mouse, and the Amiga light pen, plus the MiSTer OSD and the
boot menu.

Three surfaces, because that is what was asked for and each needs different work:

| Surface | What it buys |
|---|---|
| AmigaCD core | CD32 pad at SNAC latency, stick-as-mouse, light pen |
| OSD while AmigaCD runs | Open and navigate the menu without reaching for a USB pad |
| MiSTer Menu core | Pick and launch a core with nothing but a PSX pad attached |

## Scope

In scope:

- A PSX pad reader on the user port, both SNAC ports, digital pad and DualShock.
- CD32 pad / Amiga joystick injection, at SNAC latency.
- Analog stick as an Amiga mouse.
- GunCon as an Amiga **light pen** — including un-stubbing the light-pen latch in
  `agnus_beamcounter.v`.
- OSD navigation from the pad, in AmigaCD and in the Menu core.
- A forked `menu.rbf` carrying the reader.
- Per-port mode in the AmigaCD OSD, persisted in `AmigaCD.cfg`; in `MiSTer.ini`
  for the Menu core.

Out of scope, and deliberately so:

- **PSX memory cards.** They share the bus and the same ATT lines (device `0x81`
  rather than `0x01`), so the reader could reach them, but there is no Amiga-side
  consumer. Per-game NVRAM already works via the ISO9660/TOC identity path.
- **Rumble, NeGcon, Jogcon, multitap.** No Amiga meaning.
- **Making PSX SNAC work under cores other than AmigaCD and Menu.** The reader is
  a module other cores could take; carrying that is not this work.
- **A real light pen on the joystick port's FIRE line.** The latch we add is
  loaded directly rather than by beam coincidence (see "Light pen" below), so a
  physical Phazer plugged into a DE10-Nano joystick adapter is not covered.
- **Touching `support/physical_disc/*`.** Unrelated, and kept byte-identical to
  upstream on purpose.

## Hardware facts that constrain everything

**The SNAC PSX pinout**, taken from `MiSTer-devel/PSX_MiSTer` `PSX.sv`, which is
the de facto standard every PSX SNAC adapter is wired to:

| USER_IO | Dir | Signal |
|---|---|---|
| 0 | out | `~ATT` port 2 |
| 1 | out | `~ATT` port 1 |
| 2 | out | `CMD` |
| 3 | in | `ACK` |
| 4 | in | `DAT` |
| 5 | out | `CLK` |
| 6 | out | csync to the GunCon (`USER_OUT[6] <= ~csync` in `PSX.sv`) |

Both ports share `CMD`/`CLK`/`DAT`/`ACK` and are selected by their own ATT line,
so **two pads cost one bus master**, polled in sequence.

**The overlap with MiSTer Floppy is total.** Its pin map
(`docs/misterfloppy-hardware.md`) uses 0–6 for I2C, write data, index and read
data. Mutual exclusion is therefore physical, not a policy we chose. Confirmed
with the user 2026-08-04: PSX pads and real floppy drives are never wanted at the
same time.

**`SW[1]` on the DE10-Nano must be off.** `sys_top.v` hands USER_IO pins 2, 4 and
5 to HDMI I2S audio when that slide switch is on, and forces those inputs high.
Those are `CMD`, `DAT` and `CLK` — the entire bus. The same switch already breaks
Mr Floppy, for the same reason, and the failure is silent both times.

**The SuperStation has a dedicated GunCon composite port** (user, 2026-08-04), so
the sync path to the gun exists in hardware. What does not exist is the sync
itself: `Minimig.sv` generates no csync today.

**A GunCon needs a CRT, on direct analog output.** The gun measures when the beam
passes it, and we tell it our core's raster timing. Through the scaler or over
HDMI those numbers no longer describe the picture the gun is looking at. This is
arithmetic, not a preference.

## Prior art

| Source | What it proves |
|---|---|
| [`antoniovillena/MiSTer_DB9`](https://github.com/antoniovillena/MiSTer_DB9) | A patched `MiSTer` binary plus a patched `menu.rbf` can drive the OSD and the boot menu from the user port. The pattern works; only the adapter differs |
| [`MiSTer-Enhanced/Minimig-AGA_MiSTer`](https://github.com/MiSTer-Enhanced/Minimig-AGA_MiSTer/blob/MiSTer/README%20DB9%20Support.md) | A Minimig-AGA fork already carries a "UserIO Joystick" OSD row — closest relative to our core, DB9 only |
| `MiSTer-devel/PSX_MiSTer` | The pad protocol and the pinout. VHDL (`joypad.vhd`, `joypad_pad.vhd`, `pJoypad.vhd`), entangled with that core's SIO/IRQ model |
| [`blue212/SNAC-PSX`](https://github.com/blue212/SNAC-PSX) | The adapter PCB — two ports, GunCon composite, rumble jumper. PCB only, no RTL |

**No MiSTer core supports SNAC in the Menu core.** That is a
[documented gap](https://misterfpga.org/viewtopic.php?t=7486), not an oversight
we are working around, and it is the part of this work that exists nowhere else.

## Architecture

**Approach A: split path.** Buttons are read and injected in RTL, at SNAC
latency. Full pad state — sticks and gun coordinates included — is published to
the HPS over a UIO read, which drives OSD navigation, the mouse and the light-pen
latch.

Rejected alternatives:

- **B, HPS-only.** The core reads the pad and reports it; userspace feeds it back
  as an ordinary joystick, inheriting joy mapping, autofire and OSD for free.
  Rejected because a Linux round-trip per poll gives USB-grade latency, which is
  most of the reason not to simply use a USB adapter.
- **C, in-core only.** No reporting, no OSD, no Menu core. Does not meet the
  requirement.

The asymmetry is the point: buttons need microseconds and get them in RTL; a
mouse pointer and a gun coordinate tolerate a frame and are handled where they
are cheap to change.

```
PSX pad ──► snac_psx.v ──┬──► JOY0/JOY1 ──► userio.v ──► CD32 pad regs
  (both ports)           │                              Amiga joystick
                         └──► UIO_MM2_SNACRD ──► snac_psx_poll()
                                                  ├──► menu_key_set()   (OSD nav)
                                                  ├──► user_io_mouse()  (stick)
                                                  └──► light-pen latch  (GunCon)
```

## Components

### `rtl/snac_psx.v` — the pad reader

Written in Verilog rather than lifted. The PSX core's reader is four VHDL files
wired into that core's SIO and interrupt model; Quartus would take mixed
language, but we would be importing those assumptions to obtain one shift
register. A standalone module is also the only part of this work that can be
tested without hardware (see Verification).

Responsibilities: drive `CLK`/`CMD`/ATT, clock the `0x01` → `0x42` controller
poll, decode the reply ID, present results. Reply IDs `0x41` (digital), `0x73`
(DualShock analog), `0x53` (config mode) and `0x63` (GunCon) — the GunCon ID to
be confirmed against `PSX_MiSTer`'s implementation before it is relied on.

Outputs per port: an 11-bit joystick vector shaped exactly like `JOY0`/`JOY1`,
four 8-bit analog axes, and a presence/type flag distinguishing "nothing
plugged in" from each pad type.

`~csync`, derived from the core's H/V sync, is driven onto `USER_OUT[6]` whenever
a port is in light-gun mode.

### Amiga-side wiring

The joystick vectors are OR'd into `JOY0`/`JOY1` where `hps_io` delivers them,
before `Minimig.sv:984` passes them to `minimig` as `_joy1`/`_joy2`. Bits are
active-high there (inverted at that instantiation), so OR is the correct
combiner and a USB pad keeps working alongside a SNAC pad.

Entering at that exact point is the design: `joy_swap`, the CD32 pad shift
registers (`cd32pad1_reg` / `cd32pad2_reg`, `rtl/userio.v:210-230`), `joy_ana_en`
and autofire all sit downstream and behave as they do for a USB pad today. SNAC
port 1 lands wherever player 1 lands now. **The injection path itself needs no
`userio.v` change**; the mode widening below does.

### Light pen

The Amiga's light-gun mechanism is the chipset light pen: `BPLCON0` bit 3 (LPEN)
enables the latch, `BEAMCON0` bit 13 (LPENDIS) disables it, and `VPOSR`/`VHPOSR`
read the captured beam position. Our core stubs both halves —
`rtl/denise.v:131,154` and `rtl/agnus_beamcounter.v:126` are commented out.

The read paths already exist in `agnus_beamcounter.v`:

```verilog
data_out[15:0] = {long_frame,1'b0,ecs,ntsc,2'b00,{2{aga}},long_line,4'b0000,vpos[10:8]};  // VPOSR
data_out[15:0] = {vpos[7:0],hpos[8:1]};                                                    // VHPOSR
```

`hpos[8:1]` is already the 280 ns / one-160th-of-screen-width resolution the
Amiga Hardware Reference Manual specifies for `VHPOSR`.

So the change is: two capture registers, a 2:1 mux on those two read
expressions, and a `BPLCON0` bit-3 decode — added to the same always block that
already decodes `BEAMCON0`, `VPOSW` and `VHPOSW`, so the register write bus needs
no new plumbing. **The counter increment logic is not touched.** `agnus.v` does
not decode `BPLCON0` today; the decode is added in the beam counter, not routed
in from Denise.

**The latch is loaded directly, not by beam coincidence.** Userspace converts the
gun's dot-clock/scanline coordinates into `vpos` / `hpos[8:1]` and writes them
over UIO. The authentic alternative — compare the gun position against live
`hpos`/`vpos` in RTL and latch on coincidence — was rejected as more logic for no
gain: it needs a coincidence comparator, and it inherits any mismatch between our
timing and what the CRT actually displays. Direct load also puts per-CRT
calibration offsets in userspace config, adjustable without a 40-minute Quartus
run.

Because the gun reports absolute position and the light pen wants an absolute
beam position, no absolute-to-relative conversion occurs anywhere, and there is no
pointer drift.

### Userspace: the UIO channel

Two new commands. Our MM2 block currently ends at `0xFC` (`user_io.h:105`):

| Command | Value | Direction |
|---|---|---|
| `UIO_MM2_SNAC` | `0xFD` | write: port modes, light-pen latch |
| `UIO_MM2_SNACRD` | `0xFE` | read: pad state |

`UIO_MM2_USRPORT` (`0xFC`) widens from 1 bit to 2 to carry the third tenant; 0
and 1 keep their present meanings, so no config migration is needed for the mode
value itself. The RTL side is not free, though: `rtl/userio.v` decodes these as
`cmd[3:0]` (`0xFC` → `userport_cfg_sel`, `cmd[3:0] == 12`, line 435) and today
latches a **single** bit — `output reg user_port_mode` at line 59, assigned
`IO_DIN[0]` at line 472. Both widen to `[1:0]`, as does the signal where
`minimig.v` and `Minimig.sv` carry it, and the `USER_OUT` mux at `Minimig.sv:28`
becomes 3-way. The two new commands land as `cmd[3:0] == 13` and `14` in the same
decode block.

The read follows the `UIO_GET_OSDMASK` pattern already in the tree
(`user_io.cpp:3778`): `spi_uio_cmd_cont()`, successive `spi_w(0)`, `DisableIO()`.
Seven words — one status word (presence and device ID per port), then three per
port for the joystick bits and four axes.

`snac_psx_poll()` joins the Minimig branch of `user_io_poll()`
(`user_io.cpp:3216-3234`), beside `akiko_cd32_poll()` and `a2065_poll()`, and
gains a second call site under `is_menu()`. It runs only when the user port is in
PSX SNAC mode. The RTL reader polls the pads on its own clock; the HPS samples
the latest capture, so the two rates are decoupled and the in-core button path
never waits on Linux.

**The read is gated on core identity** — `is_amigacd()` and `is_menu()`, the way
this tree already identifies cores — never on probing. A UIO read to a core with
no decode for it returns whatever the bus floats to, and a false positive means
phantom menu keys.

### OSD navigation

Through `menu_key_set()` (`menu.cpp:567`), the same door `support/c64/c64.cpp:1159`
already uses. Start+Select opens the menu (`KEY_F12`); D-pad maps to
`KEY_UP`/`DOWN`/`LEFT`/`RIGHT`, ✕ to `KEY_ENTER`, ○ to `KEY_ESC`.

Two behaviours we own because we are not an evdev device: **edge detection**, as
the reader gives level state where `menu_key_set` wants events; and
**auto-repeat** for held directions, since the repeat a keyboard gets comes from
evdev's `EV_REP` and never reaches us. ~500 ms delay then ~100 ms interval
matches the OSD under a keyboard.

Routing the pad through `uinput` as a virtual device was considered — it would
inherit joy mapping, autofire and repeat — and rejected: it re-enters the input
stack that is already feeding the core over USB, and the mapping wanted here is
fixed rather than user-configurable.

### Mouse

Stick deflection becomes velocity: subtract centre, apply a deadzone, scale,
accumulate the fraction across polls so slow movement does not quantise to zero,
then `user_io_mouse(buttons, dx, dy, 0)` (`user_io.cpp:4110`). Its early return
when the OSD is visible is the wanted behaviour — the pointer freezes in the menu.

The main binary's existing light-gun machinery (`menu_lightgun_cb()`,
`input[].guncal[]`, `QUIRK_LIGHTGUN_CRT`, `input.cpp:6069`) is deliberately not
reused: it is built around evdev devices with absolute axes and an on-screen
calibration flow, none of which survives contact with a core that has no absolute
input channel.

### Modes, OSD rows and config

`mm_userportMode` gains `mmup_snacpsx = 2`. Each SNAC port gains its own mode:

```c
enum mm_snacPortMode {
    mmsp_off = 0,     // port ignored
    mmsp_pad,         // CD32 pad / joystick
    mmsp_mouse,       // analog stick -> mouse
    mmsp_pad_mouse,   // both at once, the DualShock default
    mmsp_lightgun     // GunCon -> Amiga light pen
};
```

`mmsp_lightgun` on either port is what enables csync onto `USER_OUT[6]`.

The Advanced page's `User Port:` row becomes a 3-way cycle, with two port rows
appearing beneath it only in PSX SNAC mode:

```
 User Port : PSX SNAC
   Port 1  : Pad + Mouse
   Port 2  : Pad
```

Same conditional-row approach as the Mr Floppy detection line at `menu.cpp:7002`,
except these rows are **selectable** — so `menumask` stops being the constant
`0x7` at `menu.cpp:6985`, and the Back row's `menusub` index moves. The
`MENU_AMIGACD_ADVANCED2` handler tests `menusub == 0/1/2` literally today; those
become mode-dependent. Done carelessly, "Back" reaches the Ethernet handler.

Two `unsigned char`s append to `mm_configTYPE`, making a **third** on-disk
layout. The loader dispatches on file size through a frozen
`mm_configTYPE_NoExtFloppy` snapshot (`support/minimig/minimig_config.cpp:59-79`);
we add an `mm_configTYPE_NoSnac` snapshot of today's layout the same way, and
existing configs migrate with both ports defaulting to `mmsp_off`. The comment at
`minimig_config.cpp:41` explains why the snapshot must be frozen rather than
aliased to the live type — this is the third time that trap has been available.

`minimig_ConfigSnac()` joins `minimig_ConfigUserPort()` in the config-apply path
at `minimig_config.cpp:717`.

### Menu core

A subset: `snac_psx.v` drops in unchanged, `Menu.sv` declares `USER_IN`/`USER_OUT`
and answers `UIO_MM2_SNACRD`. No joystick injection, no CD32 pad, no light pen —
its only job is handing pad state to the HPS.

Port modes come from a `snac_psx` key in `MiSTer.ini`, there being no
`minimig_config` in that core. That is also the right long-term home: the setting
describes the machine's wiring, not a core.

`update_all` will overwrite `menu.rbf` whenever it refreshes the menu core, so it
needs re-applying after updates — the same bargain the DB9 fork makes, and a
documentation problem rather than an engineering one. The forked `MiSTer` binary
needs no new distribution channel; this repo already produces the one in use.

## Verification

**CI proves the userspace half only.** `.github/workflows/build.yml` cross-builds
with the pinned ARM GNU-A 10.2 toolchain; the bar is a green build with **zero
new warnings**. There is no unit-test framework in this tree and none is being
added.

**The RTL gate is binary.** Quartus must still report no failing setup paths.
Baseline is **+0.177 ns** worst-case slack at 23,555 ALMs on stock fitter
settings, measured 2026-08-03 (`docs/upstream-pins.md:105-108`). A failing path
means re-run once — placement luck alone moves a margin this thin — and if it
reproduces, stop.

**Add a testbench for `snac_psx.v`.** It is the only component whose correctness
can be established without hardware: a PSX pad protocol model driving `DAT` and
`ACK` against our master, checking the poll sequence, both ATT lines, and each
reply ID. Every bug it catches is a fitter run and a hardware session not spent.

Hardware checks, in the order they must happen:

1. **`SW[1]` off.** First and named, because the failure is silent.
2. **Menu core: pad enumerates.** Both ports, digital and DualShock, presence
   flag correct with nothing plugged in.
3. **Menu core: OSD navigation.** Start+Select opens; directions repeat sanely;
   ✕/○ enter and back.
4. **Reverse-direction regression.** Back to MT32-Pi: MIDI still works. To
   MiSTer Floppy: drives still detect. A widened mux is exactly where old tenants
   break, which is why the Mr Floppy plan makes the same check explicit.
5. **Old config loads.** A pre-change `AmigaCD.cfg` migrates with both ports
   `off`, rather than misparsing.
6. **AmigaCD: CD32 pad.** A game exercising all seven buttons, with a USB pad
   attached simultaneously to confirm the OR locked neither out.
7. **Stick as mouse.** Workbench or a CDTV title.
8. **Light pen.** Last: needs the CRT, analog output, and a Phazer-compatible
   game.

## Risks

| Risk | Signal | Response |
|---|---|---|
| **DualShock stays in digital mode** | Sticks read centred, buttons fine | Pads power up digital; forcing analog needs a config-mode init sequence. The stick-mouse feature depends entirely on this. Verify against `PSX_MiSTer` before assuming |
| Timing regression | Any failing setup path | Re-run once, then stop |
| Phantom menu keys | OSD moves under an unrelated core | Gate the read on core identity, never probe |
| GunCon calibration | Aim consistently offset | Calibration lives in userspace config by design |
| `update_all` overwrites `menu.rbf` | Menu nav stops after an update | Document; re-apply |
| Mode change with a pad held | Stuck buttons after switching tenants | Clear the injected vector on mode change, as `Minimig.sv` already resets mt32-pi on `user_port_mode` change |
| Light-pen mux touches video reads | `VPOSR`/`VHPOSR` wrong when LPEN is off | The mux must be a pure pass-through unless `lpen & ~lpendis`; verify a normal game still runs before testing any gun |

## Sequencing

**Bring the reader up in the Menu core first**, contrary to the obvious ordering.
This project's dominant constraint is that every AmigaCD RTL change costs a ~40
minute Quartus run, and the part of this work that actually needs iteration is
the reader — bus timing, ACK handling, reply IDs, whether both ports enumerate.
Menu_MiSTer is a tiny design whose fitter run should be a small fraction of that
(measure it on the first build rather than assuming). It also front-loads the
capability available nowhere else: picking a core with a PSX pad.

The module then lands in Minimig already proven, and the expensive runs are spent
on wiring that is mechanical rather than exploratory.

1. `snac_psx.v` plus its testbench.
2. Menu core: reader, UIO read, `MiSTer.ini` key.
3. Userspace: poll, OSD navigation, mouse.
4. AmigaCD RTL: joystick injection, user-port mux widening, csync.
5. AmigaCD userspace: modes, OSD rows, config migration.
6. Light pen: `agnus_beamcounter.v` latch and mux, coordinate conversion,
   calibration.

## Corrections recorded

Two claims made during design and withdrawn, kept so they are not re-derived:

- *"The beam counter is the most timing-critical logic in the design."* False.
  `docs/superpowers/plans/2026-08-02-misterfloppy-integration.md:238` places the
  critical path in the SDRAM/CPU paths. The light-pen change adds a capture
  register and a read mux, and does not touch the counters.
- *"The CD32/CDTV library has essentially no light-gun titles, so the light pen
  is not worth implementing."* The wrong denominator: the CD32 is an AGA Amiga
  and runs Phazer-compatible Amiga games from floppy, HDD or WHDLoad. The target
  library is the Amiga's.

Also superseded: the +0.017 ns slack figure quoted from the Mr Floppy plan is the
2026-08-02 `amigacd-core` baseline, not the current one. Use +0.177 ns.

## Open questions

- GunCon reply ID and response layout, to be confirmed against `PSX_MiSTer`
  rather than assumed.
- The DualShock analog-mode init sequence — needed, and its exact form is not
  yet established.
- Menu_MiSTer fitter time, unmeasured; the sequencing argument assumes it is
  materially shorter than 40 minutes and should be checked at step 2.
- Whether the SuperStation's front ports expose `USER_IO[6]` to the GunCon
  composite port with the polarity `PSX.sv` drives.
