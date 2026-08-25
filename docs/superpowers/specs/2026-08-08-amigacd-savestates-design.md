# AmigaCD save states — design

Status: design approved 2026-08-08. Nothing implemented. Phase 0 (the SDRAM
timing fix already designated in `docs/sdram-timing-headroom.md`) is a hard
prerequisite and is not part of this spec.

The goal is player convenience: quit mid-game, come back tomorrow, resume
exactly. Not rewind, not a debugging instrument, not a session-resume
approximation. Saves are occasional, so a few hundred milliseconds of freeze is
acceptable and the state may be megabytes.

Coverage is the whole core — CD32, CDTV and floppy Amiga. A save state that
worked only on CD32 would feel half-finished. That coverage is reached in
phases, but the plan commits to all of it.

## The shape of the problem

Two repos. This one (`Main_MiSTer` userspace) already contains MiSTer's generic
savestate machinery. The core RTL lives in `upstream/core-cd32-cdtv`, a separate
checkout of `kblood/Minimig-AGA-cd32-cdtv-native-akiko`, and that is where the
new hardware goes.

The scope boundary is not the one it appears to be. `rtl/cpu_wrapper.v:158`
selects **fx68k** when `cpucfg == 0` and **TG68K** otherwise. So CD32 (68020)
and CDTV/A500 in 68010/020 modes all run TG68K; only the "68000" OSD setting
reaches fx68k, the microcoded cycle-exact core with no state export. Full
machine coverage and deferring fx68k are therefore compatible goals — the split
is by *CPU setting*, not by machine.

## Approach

Three were considered.

| | Approach | Verdict |
|---|---|---|
| A | MiSTer's standard savestate framework (FPGAzumSpass pattern): wrap every stateful register on a shared bus | Held in reserve |
| B | Quiesce the machine at a safe point, then capture architectural state only | **Chosen** |
| C | Freezer-cartridge snapshot in 68k code via the existing HRTmon `cart.v` | Research spike at most |

**B is chosen, using A's transport.** A is correct by construction and needs no
reasoning about what is safe to omit, but it touches ~60 RTL files and drops a
design-wide fan-out bus into a core that closes timing by 0.177 ns. B captures
perhaps an order of magnitude fewer registers, which is what makes it survivable
here.

B also has an oracle. WinUAE's save state format is twenty-five years of
refinement on the question "what is the minimum state of an Amiga", and its
source is already checked out on the development machine at
`C:\Users\Admin\Documents\mcp-winuae-emu\src`. The inventory is transcribed, not
guessed.

A is kept in reserve: if B's fidelity proves insufficient in a specific
subsystem, that subsystem can be promoted to flop-exact capture without
rearchitecting the transport.

## Architecture

### The host half already exists

The core declares `SS<base>:<size>` (hex) in config-string index 1;
`user_io.cpp:717` onward parses it, validating `size <= 128 MB` and
`0x20000000 <= base`, `base + size < 0x40000000`.

`process_ss` (`user_io.cpp:1969`) mmaps **four consecutive windows** of
`ss_size` — four save slots — pre-fills them from
`savestates/<core>/<name>.ss1..4` on load, and stamps `0xFFFFFFFF` into word 0
of each.

The **core is the active party**:

- To save: write the payload into a window, put its length in 32-bit words at
  word 1, then bump the counter at word 0. The host polls at 1 Hz, sees the
  counter move, and writes `(size + 2) * 4` bytes to disk (`user_io.cpp:2051`).
- To load: read a window the host has already populated.

The basic path therefore needs **no new userspace code beyond the config-string
token**. Host-side work is confined to the media block described below.

### User interaction

Four slots, matching the four windows the host maps. Slot selection, save and
restore are OSD entries in the AmigaCD menu, because the core is the active
party in the protocol and therefore owns the trigger.

Hotkey bindings for save and restore are deferred past phase 1. They are
convenience over an already-working OSD path, and binding keys before the
underlying mechanism is proven would only make the mechanism harder to debug.

### New RTL

One `ss_ctrl` module with three responsibilities — sequence the quiesce, collect
state, master the DDR3 window. It attaches to `ddram_ctrl`, which already
carries multiple masters (`Minimig.sv:579`), so this adds a port to an existing
arbiter rather than inventing a path.

### Freezing is cheap here

The entire Amiga side runs on clock *enables* off `clk_114` — `clk7_en` and
`clk7n_en` through the chipset, `clkena_in` on TG68K, `enPhi1`/`enPhi2` on
fx68k. Freezing means holding enables low, not gating a clock: no PLL work, no
clock-domain-crossing hazards, no fitter surprises. The machine stops between
ticks while `ss_ctrl` runs at full `clk_114`.

This is load-bearing for approach B, and it enables the governing implementation
rule below.

### The reuse rule

**Savestate logic reuses idle paths rather than adding parallel ones.**

Chip RAM lives in SDRAM and must be read out to the DDR3 window. Rather than
adding a port to `sdram_ctrl` and widening its arbiter — `sd_addr` is already
*the* failing timing path — `ss_dma` borrows the CPU port, which is guaranteed
idle because the machine is frozen. No new arbiter input, no extra fan-in on the
register that is already at its limit.

This rule applies to the whole feature and is only safe because of the freeze.

### DDR3 placement

`rtl/ddram_ctrl.v:307` maps fast RAM as `{3'b001, addr[28:3]}` — byte range
`0x20000000–0x3FFFFFFF`, exactly the region the host requires `ss_base` to
occupy. `rtl/cpu_wrapper.v:479` shows Z3 fast RAM can claim 128 MB or 256 MB of
it.

Resolution: place the window at the top, following GBA's convention of
`0x3E000000` (`user_io.cpp:1108`), and accept that **save states are mutually
exclusive with the largest Z3 configuration**. CD32 has no Z3 RAM, so phase 1 is
unaffected; this is a documented limitation for large-fast-RAM Workbench setups,
which are not the quit-mid-game use case.

Sizing: 2 MB of chip RAM dominates; the register inventory is low kilobytes.
Four slots at ~2–4 MB each sit comfortably in the top of the region.

## Components

### `ss_quiesce` — the freeze sequencer

Three classes of state, handled three ways.

**Run to completion.** The blitter is the significant one and is already
observable: `rtl/agnus.v:236` feeds `blit_busy` into DMACONR bit 14. Its
internals are never captured — it is allowed to finish. Likewise the in-flight
disk DMA word and the current audio DMA fetch. The cost is microseconds of extra
emulated time, invisible to a player, and it removes the nastiest part of the
inventory.

**Stop at a boundary.** The CPU. TG68K reports `busstate == 1` for "no memory
access", which combined with holding `clkena_in` gives a clean architectural
boundary.

**Capture anyway.** Whatever cannot be quiesced without changing behaviour —
audio period and volume counters, the beam counter, CIA timers and TOD. All
plain registers, small counts.

The sequencer has a bounded wait; see Error handling.

### `ss_regfile` — the inventory

1. **CPU architectural state.** D0–D7, A0–A7, PC, SR, USP, VBR, CACR.

   This requires the one VHDL change in the design. `regin_out`
   (`rtl/tg68k/TG68KdotC_Kernel.vhd:441`) is the register-file *write-data* bus
   and is no use for reading state out. The actual change is a read port on the
   `regfile` array at line 570 plus exposure of the named registers — a
   contained edit to a 16×32 array, but it is on the critical path for every
   phase and crosses the Verilog/VHDL boundary.

2. **Custom chip registers.** Agnus (DMACON, beam counter, copper pointers,
   bitplane pointers and modulos), Denise (BPLCON, colour table — 256×24-bit
   BRAM under AGA — sprites), Paula (four audio channels, INTENA/INTREQ, ADKCON,
   disk registers).

3. **CIA A and B.** Timers, TOD, ports, ICR.

4. **Machine-specific.** Akiko's C2P and CD command state for CD32; the bridge
   for CDTV.

5. **Memory-map state.** `ovl` (`rtl/minimig.v:952`, the Kickstart overlay over
   chip RAM), `rom_readonly`, and the `sel_kick1mb` / `sel_kick256kmirror`
   mapping configuration. Without these the machine resumes with the wrong thing
   at address zero.

6. **The memory image.** Chip RAM, plus slow/fast RAM within the configured
   bounds.

### `ss_dma` — the memory copier

SDRAM to DDR3 window, borrowing the idle CPU port per the reuse rule.

## The RTC is deliberately excluded

`rtl/minimig.v:962` holds `rtc_reg` as 64-bit BCD, seeded from the HPS wall
clock when `rtc[64]` toggles, then self-incremented by a 28,375,159-cycle
counter — one second at 28.36 MHz.

Restoring it would be wrong. Save at 3pm, resume tomorrow evening, and the Amiga
should believe it is tomorrow evening; a real machine switched off and on gets
the current time, and Workbench file timestamps depend on that.

Conveniently, that block has **no clock-enable gating** — it runs off raw `clk`,
outside the enable-gated domain. The freeze does not stop it and it keeps
ticking across a restore with no special handling. It is simply never captured.

## Kickstart is mutable

`rtl/gary.v:110` declares `rom_readonly = 0 //when zero allows to write to
$fc-$ff`. Kickstart lives in an SDRAM bank (`rtl/minimig.v:36`) and is only
conditionally protected: A1000 WCS mode writes it, and HRTmon patches it.
Treating it as constant would produce states that restore into subtly wrong
machines.

**Fingerprint by default, store when writable:**

- Always capture a CRC32 over the ROM region, plus its size and the
  `rom_readonly` flag. The machine is frozen and memory is already being DMA'd,
  so a pass over 512 KB–1 MB costs a few milliseconds.
- On restore, compare. A mismatch means the loaded ROM is not the one the state
  was made under — a KS 3.1 state in a KS 1.3 machine — and the restore is
  refused rather than producing a guru.
- If `rom_readonly` is clear the ROM is live state, so store the whole image.
  Roughly 1 MB per slot, but only in the configuration that needs it.

## Media and host-side state

State exists in three layers and only the first is inside the core.

**In-core** CD state — Akiko's C2P registers, command registers, DMA pointers,
interrupt state — is captured by `ss_regfile` like anything else.

**Userspace session** state is live outside the FPGA, in
`support/minimig/akiko_cd32.cpp`: `cd_cdda_lba_next` / `cd_cdda_lba_end` /
`cd_cdda_q_pos` (lines 304–307), `cd_play_start_lba` / `cd_play_end_lba`
(195–196), `cd_data_lba_base` (210), and pending-push flags including
`toc_push_idx` and `cd_post_info_media_push_pending`. Without these, a restore
mid-CDDA-track resumes into silence.

**The disc itself** cannot be saved. It can only be verified.

### Getting host state into the same file

The host writes the window verbatim and controls the length: the core sets word
1, and `process_ss` writes `(size + 2) * 4` bytes. Userspace therefore **appends
its own block to the tail and bumps the size** between noticing the counter
change and calling `FileWriteAdv` — same process, same poll, no race. On load it
parses the tail back off.

One file, nothing to orphan, and a `.ss1` stays a single unit the user can copy.
Preferred over a sidecar file for that reason.

### Disc verification reuses per-game NVRAM

Discs are already identified as `physdisc/<identity>` — ISO9660 volume label, or
TOC-geometry UUID for discs without one (`akiko_cd32.cpp:481`) — and
`lookup_disc_name()` (line 430) maps that to a friendly name via
`discnames.cfg`. Both paths are proven on hardware.

Store the identity in the host block. On mismatch, prompt by name — "Insert
*Microcosm*" — rather than failing obscurely.

### What is not saved

The prefetch windows (`akiko_cd32.cpp:228–239`) are pure cache. Call
`akiko_prefetch_invalidate()` on restore and let them refill. The hit/miss
counters are diagnostics.

Re-seek then falls out: restore the logical LBA cursors, invalidate the cache,
let the next read seek naturally, restart CDDA at `cd_cdda_lba_next`. A short
audible gap when resuming mid-track is unavoidable and acceptable.

### Writable media

Floppy and HDD are not more of the same, because writes have already left the
machine. Save a state, play on, let the game write to disk, then restore: the
Amiga's in-memory filesystem state rewinds — cached blocks, allocation bitmap,
pending buffers — but the image file does not. For a floppy that means a
corrupted disk; for a writable HDD image it can mean a corrupted volume, which
is user data loss rather than a glitch.

`mm_configTYPE` (`support/minimig/minimig_config.h:73`) already carries the whole
machine identity in one struct: `kickstart`, `memory`, `chipset`, `cpu`,
`floppy`, `hardfile[4]`, `ide_cfg`. Fingerprinting it turns "load your drives
before restoring" from a convention into a check.

**Three outcomes:**

| Condition | Behaviour |
|---|---|
| Config matches, images unchanged | Restore silently |
| Config matches, a writable image changed since the save | Warn, allow override |
| Config differs | Refuse, no override |

The third is a hard stop not because it is dangerous but because it is not the
same machine and there is nothing sensible to restore into.

This makes the AmigaVision flow work as the *common* case: boot AmigaVision,
restore the game that was in progress, carry on with no prompt. It works because
AmigaVision games are typically WHDLoad'd into RAM, so the restored state is
largely self-contained and the drives are along for the ride. The small separate
saves drive is the one that actually changes between sessions, and it is the one
the middle row catches.

Two implementation constraints:

- **There is no read-only mount flag to lean on.** `mm_hardfileTYPE.cfg` is only
  `0 = disabled / 1 = Fixed HDD / 2 = Removable CD`
  (`minimig_config.cpp:652`). The AmigaVision drive cannot be auto-classified as
  safe by mount mode; change detection must be size plus mtime.
- **Size plus mtime, never a hash.** Hashing a multi-gigabyte AmigaVision HDF on
  every save would take far longer than the save itself. This is a heuristic —
  an in-place same-length write could slip through — but ordinary writes to a
  FAT32 SD card do move mtime, and the override exists because heuristics fail
  in both directions.

## Error handling

**Validate before freezing.** A rejected restore is a no-op on a still-running
machine, never a half-restored one. A partially restored Amiga is a hung Amiga
with no diagnosis, so there is no best-effort path anywhere in this design.

Refusal gates, cheapest first: config fingerprint, state format version, payload
CRC, Kickstart CRC, disc identity (prompt to insert), writable image changed
(warn with override). Nothing freezes until all pass.

**Do not trust the window.** The host reads the file into DDR3 and memsets what
it did not fill (`user_io.cpp:1998`), so a truncated file arrives as a
valid-looking buffer with a zero tail. The payload carries a magic number, a
format version, a length and a CRC, all checked before the core acts on a byte.

**Version skew is a certainty**, not a risk — the format changes as phases land,
and these files live in `savestates/` where users copy them around. An unknown
version is refused outright.

**Quiesce timeout.** A crashed or wedged machine may never reach a quiet point.
The sequencer waits **three video frames — roughly 60 ms at PAL — then aborts
the save and reports it**. Three frames is far longer than any legitimate
blitter operation or DMA word needs, and the figure is a tunable parameter, not
a protocol constant. Capturing a machine that never quiesced would produce a state that
looks fine and restores into garbage — worse than no state. The user retries.

**The Z3 overlap is a startup check.** If the memory configuration collides with
the savestate window, save states are disabled and the OSD entry greys out at
config time. Discovering the collision by corrupting fast RAM mid-session is not
an acceptable failure mode.

**Two non-errors.** Saving during a guru correctly yields a state that restores
into a guru; do not be clever about it. And saving is not instantaneous from the
user's perspective — `process_ss` polls at 1 Hz, so the existing
`Info("Saving the state", 500)` toast (`user_io.cpp:2056`) is what tells them it
reached disk.

## Testing

**CI already has the right shape.** `.github/workflows/build.yml` runs an
`rtl-sim` job — `iverilog -g2005 -Wall` plus `vvp` over self-checking benches in
`rtl/tb/` — alongside the ARM cross-build. New RTL follows that pattern: benches
for the quiesce sequencer (synthetic busy signals, asserting it both reaches
quiesce *and* times out correctly), payload framing, CRC32, and the DDR3 window
writer.

**The round-trip bench matters most.** Save a synthetic register set, scramble
it, restore, compare. This catches *"register X is in the save path but not the
restore path"* — the most likely bug class in this feature and the most
miserable to diagnose on hardware, because it presents as a game that resumes
and then misbehaves ten seconds later. Write it before the collector, and make
it the gate on every register added thereafter.

**WinUAE as an inventory oracle.** Not equivalence testing. A one-off audit —
does our state cover what WinUAE's `.uss` chunks cover, subsystem by subsystem —
recorded as a checklist rather than an automated test.

**One honest cost:** there is no userspace test harness. No `test` target in the
`Makefile`, nothing under `scripts/` or `tools/`. The host-side block
serialisation is plain C++ and eminently unit-testable, but a native-host build
path must be created from scratch. Budget it in phase 1, since everything later
depends on that framing being right.

**Hardware acceptance, per phase.** Phase 1: save mid-game, power cycle,
restore, verify play continues; save mid-CDDA, restore, verify audio resumes;
restore with the wrong disc; restore with no disc; and a soak of ~20 cycles
looking for drift.

**Hardware test sequencing.** Happy paths are exercised first — save, restore,
resume, on CD32 and then per phase. The refusal and warning paths (wrong disc,
no disc, changed image, config mismatch, corrupt file, version skew) are checked
after the phase's happy path is working, not interleaved with it. They are
acceptance criteria for the phase, not optional; they are simply sequenced last.

**A timing gate on every phase.** Worst-case slack from the fit report, recorded
as a mandatory acceptance criterion, not a note afterwards. Given +0.177 ns is
the starting position and 62 ALMs blew through it, a phase that does not report
slack has not been verified.

## Phasing

| Phase | Content | Gate |
|---|---|---|
| 0 | `sd_addr` fan-in reduction per `docs/sdram-timing-headroom.md` | Real, measured slack. **Blocking** — until this lands, any savestate RTL builds on a design that fails to fit |
| 1 | Framework proven on CD32: `ss_ctrl`, `ss_quiesce`, `ss_dma`, TG68K read port, payload framing, host media block, round-trip bench, userspace test harness | Save/restore a CD32 game across a power cycle; slack reported |
| 2 | CDTV bridge state; floppy state and writable-media policy | Same, plus floppy save/restore; slack reported |
| 3 | fx68k, if cycle-exact 68000 mode is still wanted | Same; slack reported |

Each phase ships something that works. Nothing is left half-finished — the
coverage is staged, not reduced.

This spec describes the whole programme. The implementation plan that follows it
targets **phase 1 only**; phases 2 and 3 get their own plans once phase 1 is
proven on hardware.

## Out of scope

- **Rewind.** Frequent cheap snapshots are a different design with a different
  budget. Not ruled out later, but nothing here is built for it.
- **Cross-version state compatibility.** States are refused across format
  versions, not migrated.
- **Snapshotting HDD images.** Too large. Detection and refusal only.
- **Snapshotting floppy images.** Possible later at 880 KB per disk if
  detect-and-refuse proves annoying; not in phase 2.
- **State portability to or from WinUAE.** WinUAE is an inventory reference, not
  an interchange format.
