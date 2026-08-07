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

The decision inputs (`chipDMA`, `chipRW`) and the address are all stable long before state 0
arrives — they move on the 7.09 MHz Amiga cadence. So the *selection* could be computed and
registered during state 15, leaving state 0 to load an already-muxed, already-selected
value. That removes the state decode from the critical path without changing when anything
is actually presented to the SDRAM.

Cost: the pipeline register, and a careful argument that every input really is stable a
cycle earlier — including on the first access after reset and around refresh slots.

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

**Result: −0.358 ns, 23,602 ALMs.** Identical slack to seed 3 without retiming. The ALM count
moved slightly, so the options did take effect — they simply cannot help this path. That is
consistent with the diagnosis: the cost is interconnect around a congested register, and
moving a register boundary does not relieve routing congestion.

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
