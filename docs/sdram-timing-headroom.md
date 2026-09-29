# The AmigaCD core has no timing headroom, and why

**Date:** 2026-08-06
**Status:** analysis, with one experiment run. No fix merged.

**Updated 2026-08-31.** The designated fix below was attempted, it landed, the problem
returned anyway, and a seed then absorbed it. See "What actually happened" at the end of
that section before using anything here to plan work — the *analysis* holds up, the
prediction attached to it did not.
**Occasion:** PSX SNAC Plan 2 added 62 ALMs and broke timing. This is what we found
underneath.

## The short version

The AmigaCD core meets timing by **+0.177 ns** — about 2% of one 113.5 MHz clock period.
Adding 62 ALMs anywhere in the design pushes it to **−0.627 ns**. The failing path is not
the added logic; it is the core's own SDRAM address register, which is already at its limit
and is simply displaced by any change in placement.

This is not a PSX SNAC problem. Any feature added to this core will hit it.

## The evidence

Measured on this machine, Quartus Prime Lite 17.0.2, stock fitter settings:

| Build | Worst setup slack | TNS | ALMs |
|---|---|---|---|
| `feature/misterfloppy` (parent) | **+0.177 ns** | 0.000 | 23,555 |
| + PSX SNAC RTL (`3003c6f`) | **−0.627 ns** | −1.221 | 23,617 |

The parent build reproduces the figure recorded in `docs/upstream-pins.md:105-108` exactly,
so the fitter is deterministic here and the difference is real rather than placement noise.

### Seed sweep

`quartus_fit --seed=N`, same netlist:

| Seed | Slack |
|---|---|
| 1 (default) | −0.627 |
| 2 | −0.555 |
| 3 | −0.358 |
| 4 | −0.401 |

Seed variance spans 0.27 ns. Recovering the 0.36 ns still missing from the best seed is
outside what re-rolling can deliver.

### The failing paths

All three worst paths end at the same register — `emu|ram1|sd_addr[7]`, the SDRAM
controller's address output:

| # | From | To | Cycles allowed | Slack |
|---|---|---|---|---|
| 1 | `ram1\|sdram_state[2]~DUPLICATE` | `ram1\|sd_addr[7]` | **1** (8.808 ns) | −0.401 |
| 2 | `USERIO1\|floppy_ext_drive[5]` | `ram1\|sd_addr[7]` | 2 (17.616 ns) | −0.255 |
| 3 | `ram1\|sdram_state[0]~DUPLICATE` | `ram1\|sd_addr[7]` | **1** | −0.197 |

Path 2's source is a **MiSTer Floppy** signal, not a SNAC one. The pressure on this register
predates our change.

The full path for the default-seed failure ran:

```
AGNUS1|bl1 (blitter) -> address_generator_1 -> AGNUS1|address_out[20]
  -> GARY1|sel_chip[2] -> BMAP1|Mux0 -> chipdma_arb|arb_drive_chip
  -> chipdma_arb|chip_out_addr[17] -> ram1|sd_addr[7]
```

with **4.728 ns of pure interconnect** on the final hop into `sd_addr[7]|d`. That is routing
congestion around a heavily-loaded register, not logic depth — which is exactly why moving
62 ALMs of unrelated logic can cost 0.8 ns.

## Why the existing constraints do not help

`Minimig.sdc` already relaxes traffic into the SDRAM controller:

```tcl
set_multicycle_path -from {emu|minimig|*}     -to {emu|ram1|*} -setup 2
set_multicycle_path -from {emu|chipdma_arb|*} -to {emu|ram1|*} -setup 2
```

Those are why path 2 gets 17.616 ns instead of 8.808. But **paths 1 and 3 start inside
`ram1` itself**, so no `-from {emu|minimig|*}` rule matches them.

Nor can they be relaxed. `sdram_state` advances every `sysclk` cycle
(`rtl/sdram_ctrl.v:143`), so the state decode really does have one cycle to reach
`sd_addr`. A multicycle here would be a lie that shows up as rare memory corruption.

### Experiment run, and its result

Hypothesis: the Agnus→SDRAM address path could take a wider multicycle, since
`{sd_ba,sd_addr,casaddr} <= chipAddr` executes only in `sdram_state == 0`
(`rtl/sdram_ctrl.v:334`) — one slot in a 16-state machine, i.e. once per 7.09 MHz Amiga
cycle. The address is stable far longer than two 113.5 MHz cycles.

Applied against the existing placement:

```tcl
set_multicycle_path -from {emu|minimig|AGNUS1|*} -to {emu|ram1|*} -setup 3
set_multicycle_path -from {emu|minimig|AGNUS1|*} -to {emu|ram1|*} -hold  2
```

**Result: no change.** Worst slack stayed at −0.401. The hypothesis was aimed at the wrong
path — the binding constraint is the single-cycle path *internal* to `sdram_ctrl`, which no
`-from` rule scoped to Agnus can reach.

Recording this because the idea is an obvious one to have twice.

## THE DESIGNATED NEXT FIX: reduce fan-in pressure on `sd_addr`

**Status: attempted 2026-08-09, landed, and did not hold. See "What actually happened"
below.** The reasoning in this section is still the best account we have of why `sd_addr`
keeps losing; treat it as diagnosis rather than as a remedy in reserve.

`emu|ram1|sd_addr` has now been the destination of the binding path on **three separate
occasions**, with a different source register each time. That is the signature of an
over-loaded register, not of any one bad path:

| Occasion | Source of the worst path | Destination | Slack |
|---|---|---|---|
| PSX SNAC added, before retiming | `ram1\|sdram_state[2]` | `sd_addr[7]` | −0.627 |
| ” (2nd and 3rd worst) | `USERIO1\|floppy_ext_drive[5]`, `sdram_state[0]` | `sd_addr[7]` | −0.255, −0.197 |
| After the state-decode retiming | `minimig\|ciab:CIAB1\|regportb[6]` | `sd_addr[8]` | −0.040 |
| Light pen + read path added | `USERIO1\|floppy_ext_drive[4]` (all three worst) | `sd_addr[7]` | −0.180 |

Two things worth drawing out. First, **MiSTer Floppy is implicated in two of the three** —
this pressure is not caused by PSX SNAC and predates it. Second, each fix so far has only
moved *which* source wins the race; the destination never changes.

### Why it keeps happening

`sd_addr` is 13 bits wide, drives a device pin, and is loaded in `rtl/sdram_ctrl.v` from
three different sources plus a state decode:

- `chipAddr` at the CHIP slot (line ~334)
- `writeAddr` at CPU_WRITECACHE (~352)
- `cpuAddr` at CPU_READCACHE (~363)
- `{1'b1, casaddr}` at the CAS state (~377), with `cas_dqm` overriding `[12:11]`

Every one of those sources arrives from a different region of the die — Agnus and the chip
DMA arbiter on one side, the CPU wrapper and write buffer on another. The register cannot
be placed near all of them, so whichever source the fitter leaves furthest away becomes the
critical path. The measured cost is routing, not logic: the original failure carried
**4.728 ns of pure interconnect** on the final hop into `sd_addr[7]|d`.

That is why seeds and retiming only ever buy a temporary reprieve, and why adding *any*
logic anywhere re-rolls it.

### The fix, in outline

Split the load so the chip path and the CPU path do not share the same physical register:

1. Give the chip slot its own address register, and the CPU slots theirs.
2. Select between them as late as possible — ideally at the output, where the choice is
   already known a cycle ahead because `slot_type` is registered.
3. Let the fitter place each near its own source region.

**What must be checked before writing any of it:**

- `sd_addr` drives a pin, so an output-side mux adds delay on the pin path. Confirm the pin
  timing has room, or the fix trades one violation for another.
- The CAS state (`sd_addr <= {1'b1, casaddr}`, plus the `cas_dqm` override on `[12:11]`)
  writes the *same* register on a later cycle. Any split must keep that behaviour bit-exact,
  including the partial `[12:11]` write.
- `sd_ba` and `casaddr` are loaded in the same concatenation and may need the same treatment.

### Do NOT reach for these instead

They have been measured and are dead ends — see the experiments above:

- Seeds: 0.27 ns of spread, and it is luck, not engineering.
- Wider multicycle from Agnus: no effect; the binding path is internal to `sdram_ctrl`.
- Quartus register retiming: already on in the committed `.qsf`.
- Raised fitter effort: `PHYSICAL_SYNTHESIS_EFFORT EXTRA` is already on.

### When to do it

When a change fails timing on `sd_addr` and a seed sweep does not recover it — or sooner, if
someone is planning a feature larger than the ~62 ALMs that started all this.

### What actually happened

Recorded 2026-08-31, because the prediction above was confident and wrong in a way worth
keeping.

The fix was implemented as described — `5cb32ea perf(sdram): split sd_addr into chip and
CPU halves, select at the output`. It landed. **The problem returned regardless.** At the
tip of `sync/minimig-upstream-74d6ce0` the design read setup −0.347 / hold −0.401, TNS
−3.303 / −1.104, and the four builds before that had each needed a seed sweep to close.

So this was not the fix that ends the problem. What ends each occurrence, in practice, is
a seed: the 74d6ce0 merge closed on seed 10 (`1a4c33e quartus: seed 10 for the 74d6ce0
netlist`) at **+0.117 setup / +0.234 hold**, TNS 0.000 on every domain, with no logic
change at all. That is the honest state of this core's timing — it closes, thinly, on a
seed found by sweeping, and it has done so through every change since.

Two consequences for anyone reading this doc to plan work:

1. **Do not gate accuracy work on "fixing timing" as a prerequisite.** The core accuracy
   backlog did exactly that (T0 in the core repo's `docs/core-accuracy-todo.md`) and had
   to be corrected: it concluded from the pre-sweep numbers that the merge's chipset
   commits might have to be dropped, when in fact all four fit. Fit the change and look at
   both slacks; do not reason about affordability in advance.
2. **Rank the seed sweep as a first-class step, not a workaround.** It has resolved more
   of these than any logic change in this document has. `seed_sweep_both.sh` in the core
   repo requires both slacks positive, which is the part that matters — seed 1 in the
   2026-08-31 sweep looked healthy at +0.134 setup while carrying −0.495 hold.

### The binding path has moved off `sd_addr` entirely

Measured 2026-08-31 with `quartus_sta -t` and `report_timing -setup -npaths 10` against the
seed 10 netlist — ten seconds, no re-fit. **Neither `sd_addr` nor anything else in
`sdram_ctrl` appears in the worst ten paths.** Seven of the ten end inside `ss_ctrl`, the
save state controller:

```
    0.117  f2sdram~FF_3780                     -> ss_ctrl|state.S_L_KICK_CHK
    0.122  f2sdram~FF_3780                     -> ss_ctrl|state.S_L_KICK_CHK
    0.146  f2sdram~FF_3777                     -> ss_ctrl|state.S_L_KICK_CHK
    0.151  f2sdram~FF_3777                     -> ss_ctrl|state.S_L_KICK_CHK
    0.219  ss_ctrl|hdr_length[12]              -> ss_ctrl|rd_idx[1]
    0.219  ss_ctrl|hdr_length[12]              -> ss_ctrl|rd_idx[2]
    0.230  ss_ctrl|hdr_length[12]              -> ss_ctrl|rd_idx[23]
    0.263  ddram_ctrl|a2065_ddram_arbiter|busy -> f2sdram~FF_1381
    0.286  f2sdram~FF_3780                     -> ss_ctrl|state.S_L_MAGIC
    0.307  ss_ctrl|rd_data[52]                 -> ss_ctrl|rd_idx[1]
```

Hold is comfortable and unrelated: worst +0.234 on an `IIR_filter` tap.

So the `sd_addr` framing that this entire document is built around describes a problem the
design has moved past. The three occasions tabulated above were real, and the reasoning
about fan-in pressure is still sound as history — but the register under pressure today is
not that one, and none of the remedies below are aimed at what is actually binding.

What is binding is the save state controller's restore-side load path: a DDR3 read arriving
at the `S_L_KICK_CHK` and `S_L_MAGIC` states (four of the worst ten, including both of the
top two), and the header/index arithmetic feeding `rd_idx` (four more). That is tracked as
T17 in the core repo's `docs/core-accuracy-todo.md`, which was already proposing to trim
`ss_ctrl` — 2,530 lines, much of it diagnostics for a bug fixed in 2026-08 — and has now
been promoted on the strength of this measurement.

Re-measure before acting on anything below this line.

## Other things that would buy headroom

In rough order of payoff against risk. **None of these has been attempted**, and all of them
are changes to a memory controller in a core that is in daily use — the failure mode is
intermittent chip-RAM corruption, which will be blamed on anything but the memory
controller.

### 1. Retime the `sd_addr` load (most promising)

At `rtl/sdram_ctrl.v:320-340`, state 0 does the slot-type decision and the address mux in
one cycle:

```verilog
if(~chipDMA | ~chipRW) begin
    slot_type <= CHIP;
    {sd_ba,sd_addr,casaddr[8:0]} <= chipAddr;
```

This splits into two halves with very different verdicts.

**The decode half is sound, and is what was built.** Only states 0 and 2 drive `sd_addr`,
and the next-state expression is right there in the counter. Registering the two decodes
from that same expression gives signals that are true on exactly the cycles
`sdram_state == 0` and `== 2`, with no assumption about clock ratios:

```verilog
next_state  = (~old_7m & c_7m) ? 4'd0 : (sdram_state + 1'd1);
sdram_state <= next_state;
state_is_0  <= (next_state == 4'd0);
state_is_2  <= (next_state == 4'd2);
```

The data path is untouched, so the existing multicycles still govern it unchanged.

**The mux half is NOT sound — do not build it.** The original suggestion here was to compute
the slot-type decision and the address selection during state 15. That rests on the claim
that `chipDMA`, `chipRW` and `chipAddr` are "stable long before state 0 arrives". They are
not. `c_7m` is the Amiga `c1` phase (`AmigaCD.sv:657`), and `chipdma_arb` is clocked from
that same `c1` (`AmigaCD.sv:701`) — so the arbiter's outputs change on the very edge that
resets `sdram_state` to 0. Sampling them during state 15 captures the **previous** Amiga
cycle's request: a stale address handed to the SDRAM, which is the exact corruption mode
this work is trying to avoid.

Recorded because the idea reads as obviously correct until you trace where `c_7m` comes
from.

### 2. Reduce fan-in pressure on `sd_addr`

**Promoted — see "THE DESIGNATED NEXT FIX" above.**

### 3. LogicLock the new logic away from the SDRAM region

Treats the symptom rather than the cause, and Quartus Lite's support is limited, but it
directly targets the mechanism (placement displacement) rather than the path.

### Experiment 2: Quartus register retiming

Rather than hand-edit the memory controller, ask the fitter to do the retiming:

```
quartus_fit Minimig --seed=3 \
  --set=PHYSICAL_SYNTHESIS_REGISTER_RETIMING=ON \
  --set=PHYSICAL_SYNTHESIS_REGISTER_DUPLICATION=ON
```

**Result: −0.358 ns — and the experiment was worthless.** `AmigaCD.qsf:35-38` already carries
`PHYSICAL_SYNTHESIS_REGISTER_RETIMING ON` and `PHYSICAL_SYNTHESIS_REGISTER_DUPLICATION ON`,
along with `PHYSICAL_SYNTHESIS_EFFORT EXTRA`. The run set options that were already set,
which is precisely why it reproduced seed 3's slack exactly. It says nothing about whether
retiming can help.

**The real lesson is about the tool.** `quartus_fit --set=X=Y` and `--seed=N` do **not** apply
per-invocation — they are written permanently into the `.qsf`. Worse, the rewrite expanded
every `source sys/sys.tcl` / `source files.qip` include into ~260 lines of literal inline
assignments while leaving the `source` lines in place, so the whole design ended up declared
twice. The next compile crashed with `Fatal Error: Access Violation` inside the STA engine's
wildcard keeper matching (`add_keeper_to_vector_if_wildcard_matches`).

Never pass `--set` or `--seed` to `quartus_fit` in this repository. To try fitter settings,
copy the project to scratch, or edit the `.qsf` deliberately and revert it deliberately.

### Known dead ends

- **Raised fitter effort**: already tried on this core, came out *worse* than stock
  (+0.143 vs +0.177) — `docs/upstream-pins.md:105-108`.
- **Seed sweep**: 0.27 ns of spread against a 0.36 ns deficit, measured above.
- **Wider multicycle from Agnus**: no effect, measured above.

## Verification any fix would need

Timing closure is necessary and nowhere near sufficient. A change to `sdram_ctrl` needs, at
minimum:

- Chip RAM integrity under sustained blitter load — a demo or game that hammers the blitter,
  run long enough for a rare fault to show.
- CD32 and CDTV boot, both chipsets, since the chip/CPU slot arbitration differs.
- MiSTer Floppy still working, because `floppy_ext_drive` is on one of the failing paths.
- Akiko / CD-ROM DMA, which shares the arbiter.

That is a real test campaign, not a smoke test. It is the reason this document ends with
analysis rather than a patch.

---

## 2026-09-02: the `sd_addr` framing above is out of date

Everything before this section is still accurate as history, and wrong as a description of
the current design. Two things have happened since.

**The designated fix was attempted, and it landed.** The section above says reducing fan-in
pressure on `sd_addr` is "not attempted. This is the one to reach for when the problem
returns". It was reached for -- `5cb32ea perf(sdram): split sd_addr into chip and CPU
halves, select at the output` -- and the problem returned anyway, and a seed then absorbed
it. That whole sequence is worth more than the original prediction: a fix aimed at one
register moved which source wins, and the pressure reappeared elsewhere.

**`sd_addr` is no longer the destination of anything that binds.** `report_timing -setup
-npaths 10` against the seed 10 netlist (2026-08-31) found neither `sd_addr` nor
`sdram_ctrl` anywhere in the worst ten. Seven of the ten were inside `ss_ctrl`, the save
state controller, in two shapes: the DDR3 read return arriving at the restore state
machine, and the header/index arithmetic feeding `rd_idx`.

### The DDR3 read return, fixed 2026-09-02

The first shape was the f2sdram bridge reaching `ss_ctrl`'s state register directly across
the die:

    0.117  f2sdram~FF_3780 -> ss_ctrl|state.S_L_KICK_CHK      <- worst in the design
    0.286  f2sdram~FF_3780 -> ss_ctrl|state.S_L_MAGIC

Both of those states are only ever entered from `S_L_RD_DATA`'s
`if (ddr_readdatavalid) state <= rd_ret`, so the source was the valid strobe itself.
Registering the read return inside `ss_ctrl` splits that hop in two. `ddr_waitrequest` is
deliberately not registered: Avalon requires it sampled in the same cycle as the command.

**Result: the entire `f2sdram -> ss_ctrl|state` family left the worst twelve.** The cycle it
costs is free -- both consumers sit waiting on the beat, a 64-bit read already costs tens of
cycles of DDR3 latency, and a restore is a bulk transfer with the machine frozen.

### What binds now

    0.095  ss_ctrl|rom_scan -> sdram_ctrl|cpu_cache|fill_tag[10]
    0.124  ss_ctrl|rom_scan -> sdram_ctrl|cpu_cache|cpu_sm_tag_dat_w[*]    (ten of these)

A different shape again, and a promising one. `rom_scan` is a mode flag: raised when the
module needs the SDRAM CPU port, held for the length of a scan, dropped at the end. It
feeds `ss_port_own = ss_freeze | ss_rom_scan | ss_peek_scan` at `Minimig.sv:741`. A signal
that is static for thousands of cycles is being held to a single-cycle budget, which is
exactly the case `Minimig.sdc` already documents for `ram1|init_done`. `ss_ctrl.v:366`
records that widening this signal's fan-in once cost 0.136 ns, so the pressure on it is
known.

The obvious next step is a `-setup 2` from `rom_scan` into the cache, written conservatively
rather than as a false path for the same reason `init_done` was: the 0->1 edge does change
downstream behaviour, and on a memory path "it only moves once per scan" is not an argument
worth relying on unaltered. Establish first that the port mux cannot act on the edge within
one cycle.

### The lottery is structural, not a settings problem

Five seed changes in two days, each invalidated by a smaller RTL edit than the last -- the
final one differed from its predecessor by two parameters defaulting to zero. Quartus Prime
Lite cannot preserve placement across edits: incremental compilation and Rapid Recompile are
Standard and Pro features. There is no setting that fixes this, and the fitter settings in
`Minimig.qsf` are already at maximum (`HIGH PERFORMANCE EFFORT`, `STANDARD FIT`, physical
synthesis combo/duplication/retiming all on, aggressive routability always). The only cure
is to fix the marginal paths themselves so no seed is needed, which is what this section is
a log of.

Utilisation is not the constraint and has never been: 70% ALM, 50% RAM blocks, 62% DSP,
46% pins. The cost is routing distance between blocks that must talk.

## 2026-09-27: the address bus is not in the I/O registers, and nothing times it

Two facts, both checked rather than reasoned:

1. `AmigaCD.qsf:271` asks for `FAST_OUTPUT_REGISTER ON -to SDRAM_*`, but
   `rtl/sdram_ctrl.v:324` drives the address bus from a combinational mux:

   ```verilog
   assign sd_addr = sd_addr_sel_cpu ? sd_addr_cpu : sd_addr_chip;
   ```

   A pad fed by a mux has no register to pack, so the request is silently a
   no-op for `SDRAM_A[*]`. `sd_addr_chip` and `sd_addr_cpu` stay in the core
   and their outputs reach the pins through ordinary interconnect.

2. Neither `AmigaCD.sdc` nor `sys/sys_top.sdc` contains a single
   `set_input_delay` or `set_output_delay`. The external SDRAM interface is
   unconstrained, so that interconnect has no timing requirement on it at all.

Taken together those say the address bus reaches the SDRAM on whatever routing
the fitter felt like, with STA neither measuring it nor caring. Skew across the
thirteen lines is then a property of the seed.

### Why this matters now

On 2026-09-26 seed 16 of the CLUT netlist fit with all five worst-case slacks
positive -- setup +0.152, hold +0.174, recovery +3.452, removal +1.063, minimum
pulse +1.101, zero critical warnings -- and produced a machine that never
reached Kickstart. Seed 18 of the same netlist booted. Seed 1 of the next one
booted. The failure mode fits: the 68020 fetches its reset vectors through this
bus, so an address line that arrives after the SDRAM has latched gives a dead
machine and nothing for STA to have caught.

That is the same shape as the three occasions tabulated above, where "a seed
absorbed it". A seed absorbing a problem and a seed causing one are the same
mechanism seen from either end.

### The cost of the fix that moved the binding path

The split `sd_addr` was introduced to get the internal path off a heavily
loaded register, and by the 2026-08-31 measurement it worked: `sdram_ctrl` is
nowhere in the worst ten paths. But the thing it traded away was IOE packing on
the address bus -- an internal path that STA measures, exchanged for an
external one that it does not. That is worth knowing before anyone reaches for
the same move again.

### What would settle it

- Re-register `sd_addr` immediately before the pad so `FAST_OUTPUT_REGISTER`
  can take it, and re-measure both the internal path and the boot rate across
  several seeds.
- Constrain the interface: `create_generated_clock` on `SDRAM_CLK`, then
  `set_output_delay` against the SDRAM's t_IS/t_IH and `set_input_delay`
  against t_AC/t_OH, so a seed that skews the bus fails the fit instead of the
  machine. Expect this to fail loudly at first, which is the point.

Neither has been attempted. Stock MiSTer cores are reportedly unconstrained
here too, so this may be a framework-wide property rather than ours alone --
that claim has not been verified in this repository and should be before it is
relied on.

## 2026-09-27: both of those have now been attempted. One worked, one did not.

The two items above are no longer untried. Taking them in the order they were
written:

**Constraining the interface worked, and it was needed.** `AmigaCD.sdc` now
declares a generated clock for `SDRAM_CLK`, sourced from the `q` pin of the
register that actually produces it (`sd_clk <= sdram_state[0]`) rather than from
the PLL output, so the model carries the real clock-to-out; it forwards that
clock to the port and applies `set_output_delay` / `set_input_delay` against
worst-case part figures (tIS 1.5 ns, tIH 0.8 ns, tAC 6.0 ns, tOH 2.0 ns) plus
0.2-0.4 ns of board trace, with setup 2 / hold 2 multicycles both directions.
It did not fail loudly: seed 1 closes at **setup +0.337, hold +0.243**. So the
interface has been meeting a constraint it was never given, and now a seed that
skews the bus will fail the fit instead of the machine.

One correction to the note above: `SDRAM_CLK` is **sysclk/2 = 56.75 MHz**, not
113.5. `sd_clk <= sdram_state[0]` divides it. Parts of this document were
written as though the interface ran at the full sysclk rate.

**Re-registering `sd_addr` for `FAST_OUTPUT_REGISTER` does not work here, and
the reason is worth keeping.** The two halves `sd_addr_chip` / `sd_addr_cpu`
with a mux in front of the pad (`assign sd_addr = sd_addr_sel_cpu ? ... : ...`)
were merged back into a single `output reg [12:0] sd_addr` across all seven
assignment sites, so nothing stood between the register and the pin.

The packing happened. The fit report lists `sd_addr[0]` through `sd_addr[12]`
as `Packed Register / Fast Output Register assignment` into `SDRAM_A[*]~output`.
That was the entire objective and it was met.

And the build fails timing by a mile: **setup -1.625 ns**, TNS -8.647, against
+0.337 for the same seed with the split halves. The three worst paths all end
at `sd_addr` bits and all carry one enormous segment:

```
    26.971      4.640 FF    IC  emu|ram1|sd_addr[8]|d
```

4.640 ns of interconnect in a single hop, from the last logic cell in the fabric
out to the register now sitting in the I/O ring. Packing did not remove the
4.175 ns of unmanaged interconnect measured earlier on this path -- it moved it
from *after* the register, where it is clock-to-out and the new constraints
model it, to *before* the register, where it is setup and the core has to pay it
inside one 8.8 ns clk_114 cycle. It cannot.

The launching register is `minimig|ciab:CIAB1|ddrportb[6]`, reaching `sd_addr`
via `chipdma_arb|chip_out_addr[18]`. The chip address arrives at the SDRAM
controller through a long combinational chain whatever happens at the pad end,
which is why there is no slack to spend on an extra 4.6 ns.

So the mux in front of the pad is load-bearing: it keeps `sd_addr` in the fabric
beside the logic that feeds it. Approach abandoned. If the clock-to-out at the
pin ever becomes the binding constraint, the way to buy it is to shorten the
chip-address chain first, not to move the register.

## 2026-09-29: once constrained, the interface is not where this design is tight

Everything above is about the SDRAM interface, on the premise that it is the
risky part of this design. With the constraints in place that premise can
finally be tested, and it does not hold. Per-clock worst slacks from a seed 7
fit of the scanline-inhibit netlist:

```
setup                                                   hold
  -0.069  emu|pll ... counter[0]|divclk   (113.5 MHz)     -0.468  same domain
  +0.277  pll_hdmi                                        +0.204  pll_hdmi
  +0.570  emu|pll ... counter[1]|divclk                    +0.261  counter[1]
  +3.508  FPGA_CLK2_50                                     +0.271  FPGA_CLK2_50
  +5.698  SDRAM_CLK_out                                   (not in the worst 8,
  +7.934  FPGA_CLK1_50                                     so above +0.403)
```

**The external SDRAM interface has 5.7 ns of setup margin and over 0.4 ns of
hold margin.** The binding path in both directions is the internal 113.5 MHz
core clock. So the four part figures in `AmigaCD.sdc` could be loosened by
nanoseconds without changing a single fit result, and tightening them -- by
reading the actual part number off the chip -- buys accuracy rather than
headroom.

Two corrections to earlier notes in this file and in the PR threads follow from
that measurement.

**The constraints did not make fits harder.** After #30 merged, a run of pairs
on a rebased branch came back tighter than the same seeds had on the old branch
point, and I attributed that to five newly constrained path groups. A domain
with 5.7 ns spare is not constraining anything. The tighter numbers came from
the other thing the rebase brought in -- the baseline arm became mainline
*including* #27's command phantom gate, so it is a larger design than the old
baseline was -- plus ordinary seed scatter. #30 added visibility, not
difficulty.

**`SDRAM_CLK` being 56.75 MHz does not mean the bus has spare cycle time to
trade.** It has spare cycle time and the constraints already account for it via
the 17.616 ns period. What it does not do is shrink `tAC`: 6.0 ns is a
propagation delay from the clock edge to valid data, identical whether the part
is clocked at 56 MHz or at the 167 MHz its speed grade is rated for. A board
measured stable at 167 MHz confirms the part is healthy and consistent with the
-6 grade the SDC already assumes; it is not a licence to reduce tAC. Note also
that the 167 MHz rating on these parts is at CL3, while this controller programs
CL=2 (`sdram_ctrl.v:378`), where tAC is the larger figure.

**Where the work actually is**, if core timing ever needs buying back: the
113.5 MHz domain, and specifically the chip-address chain. The worst paths seen
across this week's fits launch from `minimig|ciab:CIAB1|ddrportb[6]` and from
`ss_ctrl:savestate|ddr_read`, reaching `sd_addr` through
`chipdma_arb|chip_out_addr[18]` and the DDR3 arbiter respectively. Neither is an
interface path.
