# Phase 1B-2: the chipset state vector

Phase 1A captures the CPU's architectural registers and 2 MB of chip RAM, and
nothing else. On hardware that produces a restore which completes every stage
cleanly — validation, Kickstart fingerprint, freeze, state vector, write-back,
resume, release, `load_ok` — and then resets the Amiga, because the machine it
hands that state to is not the machine the state came from. This phase closes
that gap.

## What the hardware evidence actually says

Worth recording, because three plausible explanations were wrong before this one:

- A restore **8 seconds** after its save still reset. So this is not about long
  elapsed time or stale contents drifting; a chipset that is seconds old is
  already wrong enough.
- A restore **26 minutes** after its save completed and reset the machine, and
  every restore afterwards refused with `FAIL_KICK` because the live Kickstart
  no longer fingerprinted the same. Something had written to ROM. The write-back
  is bounded and asserted (`restore writes only inside chip RAM`), so the likely
  writer is the crashed machine itself: `rom_readonly` is restored from the
  vector, and gary permits ROM writes when it is clear.
- Saving a running game failed roughly three attempts in four until the quiesce
  limit was raised, which is unrelated to the chipset but was masking how often
  the restore path was even reachable.

Two causes were found and fixed along the way, and both were capable of killing
a restored machine on their own, independently of the chipset:

- the restore ran ~270 ms of validation with the CPU parked and the chipset
  running, then installed state from before all of it (fixed: freeze first);
- the restore rewrote chip RAM through the borrowed SDRAM CPU port while the
  cache's snoop is tied to `chipWE`, so the 68020 resumed against a cacheful of
  pre-restore lines (fixed: invalidate inside the freeze).

Neither is chipset state. Until the current build is tested it is not known how
much of the reset each explains.

## Approach: shadow the register bus (B), not per-module export (A)

`minimig.v` routes exactly two wires to agnus, paula, denise and both CIAs:
`reg_address` (agnus's `reg_address_out`, minimig.v:582) and `custom_data_in`
(gary's, minimig.v:929). Every custom chipset register write is a value on the
second while the first holds that register's address, taken on `clk7_en`.

There is no write strobe and none is needed: **the address is the strobe**,
which is why the real modules decode writes with a bare address compare
(`paula_intcontroller.v:52`). Reads are unambiguous for free, because the Amiga
puts read and write registers at different addresses (`INTENAR` $01C reads what
`INTENA` $09A writes).

So `ss_regshadow` sees every write using the same decode the hardware uses, with
no change to any chipset module. Replay drives the same two buses, which is two
muxes in `minimig.v` since both are module outputs.

A — exporting the real registers — was rejected because most Amiga custom
registers are write-only in hardware and this core is faithful about that, so
there is nothing to export. It remains the fallback if replay proves unworkable.

### Done

- `ss_regshadow.v` + bench: snoop, 256×16 storage, readback, `rd_writable`.
- The hardware's set/clear rule applied to `DMACON`, `INTENA`, `ADKCON`, so the
  shadow entry *is* the register's contents. This also avoids reassembling
  `DMACON` from the two partial copies paula (bits [4:0] + `dmaen`) and agnus
  each hold.
- `INTREQ` exported by value, `paula_intcontroller` → `paula` → `minimig` →
  `Minimig.sv`. It is the one set/clear register hardware also writes — Paula
  raises a bit for every interrupt it wants — so an accumulator built from bus
  writes drifts within a frame.
- Exclusions, each mutation-tested: strobes (`COPJMP1/2`, `STREQU`, `STRVBL`,
  `STRHOR`, `STRLONG`) because writing one is an event; the read window below
  $020 because nothing writes it and replaying into it would drive a read
  address onto the bus.

### Not captured, and why that is survivable

State the chipset advances on its own: a bitplane pointer moved by DMA, an audio
pointer walked by Paula. The shadow holds what was last *written*. In practically
all software the copper rewrites those every frame, so they re-converge within a
frame of the restore.

## The CIAs need approach A, not B

The CIAs are on the **CPU** bus (`aen`, `rs`, `cpu_data_out` — minimig.v:770,
796), not the RGA bus, so the register shadow does not see them. Their timers
count down, so writes would not give the current value even if it did.

The tempting extension — drive the CIA bus to read all 16 registers of each CIA
while frozen — **must not be done**: reading a CIA's ICR clears the pending
interrupts, and reading TOD latches it. A capture with side effects would damage
the machine on every save, which is worse than not capturing.

So the CIAs get direct exports of the underlying registers: timer A/B counters
and latches, TOD counter/latch/alarm, ICR mask and pending, CRA/CRB, the port
data and direction registers, SDR. Across `ciaa.v`, `ciab.v`, `cia_timera.v`,
`cia_timerb.v`, `cia_timerd.v`, `cia_int.v`.

## Replay ordering

Writing the shadow back is not a loop over 256 addresses in address order.

1. Everything except the ordering-sensitive registers, plain writes.
2. Set/clear registers via the dance: `0x7FFF` to clear every bit, then
   `0x8000 | value`. Writing the value straight back would set the bits that are
   1 and clear nothing. `rd_setclear` marks these.
3. `DMACON` and `INTENA` **last**, so DMA and interrupts do not start against a
   half-restored chipset.
4. Strobes never.
5. All of it inside the freeze, before `S_L_FLUSH`.

## Payload format

The shadow is 4096 bits — a streamed section like chip RAM, not a flat entry in
`SS_STATE_LIST`. Payload becomes: kick CRC, state vector, **register shadow**,
CIA block, chip RAM. That changes `CORE_WORDS` and therefore the on-disk length,
which the existing length gate rejects for old files automatically. This is also
the moment to drop the two dead `ss_sel_kick*` entries the 1A vector carries
(see `ss_state.vh`).

## Open items outside this phase

Recorded so they are not lost:

- Fast RAM is not captured. The `fastram >= 4` guard exists only because Z3 fast
  RAM collides with the DDR3 save window, not for capture, so Z2 fast RAM up to
  8 MB is permitted and silently not saved.
- Media identity is never checked. The fingerprint verifies the ROM; nothing
  verifies the mounted disc, so a state restores against a different CD with
  every gate passing. The ISO9660 volume label is already computed for the
  filename (`amigacd_ss_name()`) and would be a cheap fingerprint.
- CD32/Akiko and floppy state are not captured.
- CPU configuration is not validated; neither the length nor the ROM CRC changes
  between CPU settings.
- The payload loops terminate on `==` rather than `>=` (`ss_ctrl.v`). Correct
  today because each loop increments once per pass, but a future path that skips
  an increment turns the chip writer loose through SDRAM.
- The write path is unverified at the real 2 MB; the bench models eight words.
- The save state toast has never appeared. The core raises it, the host reads it
  back and resolves a non-empty string, and `Info()` drops it unless
  `menustate <= MENU_INFO` (== 2). A diagnostic now logs which.

## Phase 1B-3: fast RAM

Not optional. The machines this core targets have no fast RAM as standard, but
it is switched on in practice (4 MB here), and a game keeping state there
restores wrong today with no warning — chip RAM and the CPU come from the save,
fast RAM is whatever is there now. That is the worst failure shape available: it
looks like it worked.

### Where it lives

DDR3, not SDRAM. `Minimig.sv`'s `zram_sel = |ram_addr[28:26]` routes Zorro fast
RAM to `ddram_ctrl` (ram2), and the save window sits deliberately above it —
the comment at `SS_SLOT_BASE` records that a truncated literal once put the
window "squarely inside fast RAM".

So capture is a DDR3-to-DDR3 copy on the master `ss_ctrl` already owns, not the
borrowed SDRAM CPU port the chip RAM path uses. The chip RAM machinery does not
extend to it; this is a second streaming path.

### Transport

The window cannot carry it as it stands: `SS3E000000:400000` is 4 MB per slot,
and 2 MB chip + up to 8 MB fast is 10 MB.

Decision: **8 MB slots, four of them**, `0x3E000000`–`0x40000000`. That is 32 MB,
which fits exactly to the top of DDR3 (`user_io.cpp` rejects
`ss_base + ss_size >= 0x40000000`), keeps all four slots, and needs no new
region. It carries 2 MB chip plus ~5.9 MB fast.

Consequences:
- the config string token changes to `SS3E000000:800000`
- `SS_SLOT_STRIDE` doubles
- 8 MB of Z2 fast RAM still does not fit and must be refused, alongside the Z3
  refusal that already exists for a different reason (Z3 collides with the
  window rather than overflowing the slot)

### Format

The fast RAM section is variable-length: its size follows the configured fast
RAM, so `CORE_WORDS` becomes a runtime value rather than a localparam, and the
size has to reach ss_ctrl as an input. The header already carries a length the
host reads rather than assumes, so the on-disk shape does not change — but the
length gate's `MIN_LENGTH` comparison and `CORE_WORDS_24/32` need to follow the
configured size instead of a constant.

Payload order: fingerprint, state vector, register shadow, chip RAM, fast RAM.
Fast RAM last so that a reader which stops early still has a coherent machine.

### Interim safety

Until the above lands, `minimig_ss_blocked()` should refuse a save whenever ANY
fast RAM is configured, not only Z3. A clear refusal is better than a restore
that silently drops several megabytes of the machine's state.

## Open: the shadow does not infer a memory

Two fits, both failing timing, with the shadow costing ~4700 ALMs for 4 KB:

| | slack | ALMs |
|---|---|---|
| before the chipset vector | +0.212 | 26,232 (63%) |
| chipset vector, reset over the array | -0.357 | 31,223 (75%) |
| chipset vector, `initial` instead | -0.199 | 31,028 (74%) |

Removing the reset was necessary and not sufficient. The array has FOUR read
ports and Quartus will not infer a block memory for that:

1. `rd_data` — the save readback
2. `cur` — the read-modify-write behind the set/clear rule
3. `replay_data <= shadow[ridx]` — the replay walk
4. `sc_value()` — three fixed-address reads for DMACON/INTENA/ADKCON

Fix, in order of what it removes:

- **Take the set/clear registers out of the array.** DMACON, INTENA and ADKCON
  are three 16-bit registers; hold them as three named regs instead of array
  entries. That deletes read ports 2 and 4 outright, and the set/clear rule
  stops being a read-modify-write on a memory.
- **Share one read port between the save readback and the replay.** They never
  run at once — a save is not a restore — so one address mux serves both.

That leaves one write port and one read port, which infers. Expect the ~4700
ALMs back and timing with it.

Do this before any more fits: two fits have already been spent on the same
cause.

### Read-port fix: worked for area, timing still fails

| | slack | ALMs |
|---|---|---|
| baseline (no chipset vector) | +0.212 | 26,232 (63%) |
| four read ports, array reset | -0.357 | 31,223 (75%) |
| four read ports, `initial` | -0.199 | 31,028 (74%) |
| one read port, set/clear regs out | **-0.512** | **28,095 (67%)** |

The read-port fix did what it was for: ~2900 ALMs came back. But timing went
further negative, and the two failing clocks are the PLL outputs
(`counter[1]`, `counter[0]`), i.e. the fastest domains — not a path named
inside ss_regshadow.

So the remaining problem is not that one module's structure. The chipset vector
costs ~1900 ALMs over baseline and the design is now at 67%, and that is enough
to push the fastest clock negative.

Next, in order:

1. **Seed sweep.** Slack on identical logic has ranged +0.05 to +0.46 across
   this session, so the fitter's variance is roughly 0.4 ns. -0.512 is outside
   that band, so a seed alone probably will not close it -- but it is one fit to
   find out and it needs no design change.
2. **Read the actual failing path**, from the STA report's per-path detail rather
   than the clock summary, and see whether it runs through the new logic at all.
   If it does not, the vector is simply the straw and the fix is elsewhere.
3. **Register the shadow's read port.** A combinational 256-entry read is still a
   wide mux even with one port; registering it would infer a true M10K, at the
   cost of a cycle of latency that ss_ctrl's S_SHADOW_RD already anticipates for
   one cycle and would need to anticipate for two.

Do not deploy any of these builds. The last good one is the quiesce + cache
invalidate fit at +0.212.

### The failing path, named

Seed 7 gave -1.081 against seed 1's -0.512, so seed variance is not going to
close this. `report_timing -detail full_path` names the path:

```
From: ss_ctrl:savestate|shadow_rd_addr[0]
To:   ss_regshadow:ss_regshadow_inst|replay_data[12]
Slack: -1.081
```

ss_ctrl's registered read address, across the module boundary, through the
address mux, through the **256-entry combinational array read**, into
replay_data. A 256-deep asynchronous read is a very wide mux and it sits
between two registers in different modules.

So the chipset vector is not merely the straw. The remaining cost is the
combinational read port, which is also the last thing stopping a true M10K.

**Fix: register the read port.** `mem_q` becomes synchronous, which infers block
RAM and breaks the path at the register.

Consequences for ss_ctrl, which must be handled together:

- `S_SHADOW_RD` already allows one cycle for the readback to settle. With a
  registered read it needs **two**: drive the address, wait, then sample.
- The replay walk takes `replay_data <= mem_q`, where `mem_q` will correspond to
  the address driven the cycle before. The walk must drive `ridx` one step ahead
  of the entry it is about to emit, or hold a cycle between the address and the
  emit.
- `ss_ctrl_tb` and `ss_regshadow_tb` both assert the current one-cycle
  behaviour, so both change with it. The directed accumulate-then-read test is
  unaffected -- the set/clear registers are no longer in the array.

Note on the -through investigation: `get_pins -compatibility_mode {*name*}`
matched 32339 pins for the shadow and 22108 for ss_ctrl, i.e. the wildcards
matched most of the design, so a `-through` filter built that way is worthless
and silently returns the global worst path. Use `-detail full_path -file` and
read the node names instead.

## What the replay actually did before it was simulated (2026-08-17)

Everything above was implemented and every module bench passed, and the replay
still wrote nothing into the machine. Both faults are of the same kind: the
module benches end at `ss_regshadow`'s pins, and nothing had ever elaborated
`minimig.v` to see what happened after them.

`rtl/sim/ssmux/tb_ss_regbus_mux.sv` in the core repo now does. It instantiates
the real `minimig` -- real agnus, paula, denise, gary -- with a real
`ss_regshadow` beside it, wired as `Minimig.sv` wires them, including both
`amiga_clk` instances and the freeze, and then reads the chipset's own
flip-flops. On the wiring as it stood, all three sampled registers stayed at
zero after a replay.

**1. The freeze stops the clock the writes are decoded on.** Point 5 above --
"all of it inside the freeze" -- is what breaks it. The freeze works by dropping
`ce` on the Amiga's `amiga_clk`, which holds `clk7_en` low, and every chipset
register decode in the machine is `always @(posedge clk) if (clk7_en)`. The
replay drove both buses correctly for the whole walk and not one write was ever
sampled.

Fixed with `ss_replay_tick`: `Minimig.sv` derives `clk7_en & ss_replay_we` from
the *master* generator and `minimig.v` ORs it into the clock enable of the
modules that decode the register bus (agnus, paula, denise, userio, cart). Only
the cycles carrying a replay write produce a tick.

That tick has a consequence: `agnus_beamcounter` increments `hpos` under
`clk7_en && cck`, so a machine parked with `cck` high would walk the beam
forward once per replay write, about two raster lines over a full walk. The
freeze sampler now samples `ss_freeze` on `clk7_en && cck`, which parks the
Amiga with `cck` low -- that edge is the one that toggles `cck`, and the freeze
lands on the edge after it. The bench checks the beam position rather than the
polarity.

**2. Agnus never saw the mux at all.** `minimig.v` muxes `reg_address` onto
`ss_replay_addr`, and that is the bus paula, denise and the rest read. Agnus
does not read it -- Agnus *generates* it, and decodes its own registers from an
internal copy chosen by the DMA priority mux. So DMACON, every DMA pointer,
DIWSTRT/STOP, DDFSTRT/STOP, BEAMCON0, VPOSW/VHPOSW and the copper and blitter
registers ignored a replay completely, which is most of the chipset state the
phase exists to restore. Denise and Paula took their values; Agnus's DMACON sat
at 0.

Fixed by passing `ss_replay_we`/`ss_replay_addr` into `agnus.v` and taking them
at the head of that priority mux.

Both fixes are in the core repo. The bench runs in its `rtl-sim` workflow.

## Coverage audit, and what it turned up (2026-08-17)

Prompted by asking whether the benches actually cover the phase. Four things
came out of it; three were defects, not gaps.

**The shadow section was never checked on the way back in.** `ss_ctrl_tb`
asserted the packing -- two entries per payload word, low entry in the low half
-- and nothing asserted the unpacking. A restore that took the halves the other
way round would reload every chipset register with its neighbour's value and
pass every check in the suite: the save-side words are right, the CRC agrees
with itself, and `ss_regshadow_tb` drives `ld_we` itself rather than receiving a
section `ss_ctrl` unpacked. The bench now records the replay writes during the
restore and checks the entries come back at their own addresses. Mutating
`S_L_SH_LOAD` to swap the halves fails six checks and nothing else.

**INTREQ was captured nowhere and restored from the wrong machine.**
`ss_regshadow` excludes it from the array -- correctly, Paula raises its bits in
hardware, so accumulating bus writes drifts -- and says it is "captured by value
elsewhere". There was no elsewhere. It was absent from `SS_STATE_LIST`, so a
save wrote zero for `$09C`, and the replay's INTREQ step wrote back
`intreq_in`, which `Minimig.sv` had wired to Paula's LIVE output: a restore
reinstalled the pending interrupts of the machine it was replacing and then
enabled them with the restored INTENA. `ss_intreq` is now in the vector,
`ss_state_fanout` exposes the restored field, and the shadow reads that.
`SS_STATE_W` goes 632 -> 647, so the payload length changes again.

**`ss_state_fanout` -- the whole register-write half of a restore -- had no
bench**, because it was written at the bottom of `Minimig.sv` where nothing
could compile it without the top level. Now `rtl/ss_state_fanout.sv` with a
bench covering the register indices, the five singles, the map bit order, the
one-shared-bus rule, the ordering and the handshake. Two things fell out:

- The re-seed went out in the same edge that dropped `busy`, so the kernel's
  "seed before the clock enable is released" requirement rested on
  `ss_load_busy` still being high in `Minimig.sv`'s `ss_arm` term -- another
  module's timing holding up a property this one documents as its own. An idle
  step after the re-seed fixes it.
- Quartus 17.0 rejects `$bits` in a `.v` file and accepts it in `.sv`. The
  width guard had always been in `Minimig.sv`, which is `.sv`; moving the module
  to `.v` broke the fit. The extension was carrying the feature and nothing said
  so. Hence `.sv` and `SYSTEMVERILOG_FILE`.

**The host-side parser was tested and unused.**
`support/minimig/minimig_savestate.cpp` had good coverage and no callers: the
shipped path is upstream's generic `process_ss()`, which writes the window out
on a counter change with no validation. It now validates before the write, gated
on `is_minimig()`, so a corrupt window cannot replace a good state file.

Also from the audit, not a defect: the freeze sampler and the replay tick were
written twice, once in `Minimig.sv` and once in the ssmux bench. They are now
one module, `ss_freeze_phase`, that both instantiate.

## The CIAs, done (2026-08-17)

Approach A as planned, with one deviation from the payload sketch above: the two
CIA words go **into the state vector**, not into a separate CIA block. The
vector is already a streamed bit field of arbitrary width with one ordered list
that both directions expand from, and `ss_ctrl` derives `STATE_WORDS` and
`CORE_WORDS` from `SS_STATE_W` on its own. A separate section would have needed
its own fetch/load states, its own packing order and its own place in the CRC,
all to carry two fixed-width words. `SS_STATE_W` goes 647 -> 1041, which is 33
payload words for the vector.

What is carried, per CIA: both timers' counters, latch bytes and control
registers; TOD with its alarm and its read latch; the interrupt mask and pending
flags; the port data and direction registers; SDR. 191 bits for CIA A, 203 for
CIA B -- CIA A drives only four port A bits and has no port B output register.

The read latch is in there deliberately. Reading the TOD high byte freezes it
and reading the low byte releases it, so a machine saved between those two reads
is mid-sequence; restoring the counter without the latch would hand the program
a time that jumps.

Every write port is taken **outside** the `clk7_en` gate, the same way gary's
`ss_rom_readonly` restore is. A restore runs with the Amiga's timebase stopped,
so a load that waited for `clk7_en` would wait forever -- the same fault that
made the chipset replay write nothing at all, found earlier this session.

Not carried, and deliberately: `ser_tx_cnt`/`ser_tx_run` in CIA A (a serial
transmission in flight), the input synchronisers, and the delayed-tick edge
detectors. These are one or two ticks of history rather than programmer-visible
state, and the keyboard protocol re-synchronises.
