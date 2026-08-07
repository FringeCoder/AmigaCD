# The AmigaCD core has no timing headroom, and why

**Date:** 2026-08-06
**Status:** analysis, with one experiment run. No fix merged.
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

## What would actually buy headroom

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
not. `c_7m` is the Amiga `c1` phase (`Minimig.sv:657`), and `chipdma_arb` is clocked from
that same `c1` (`Minimig.sv:701`) — so the arbiter's outputs change on the very edge that
resets `sdram_state` to 0. Sampling them during state 15 captures the **previous** Amiga
cycle's request: a stale address handed to the SDRAM, which is the exact corruption mode
this work is trying to avoid.

Recorded because the idea reads as obviously correct until you trace where `c_7m` comes
from.

### 2. Reduce fan-in pressure on `sd_addr`

Three sources feed it (`chipAddr`, `writeAddr`, `cpuAddr`, lines 334/352/363) plus the state
decode. Splitting the mux so the chip path — the one that fails — does not share logic with
the CPU path may let the fitter place them apart.

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

**Result: −0.358 ns — and the experiment was worthless.** `Minimig.qsf:35-38` already carries
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
