# Save states in 68000 mode (fx68k)

## Why

Two of the four machines this core offers cannot save state at all. `Minimig.sv`
gates the request on

```verilog
wire ss_supported = |cpucfg;
```

and `menu.cpp` greys the rows out to match:

```c
return (fastram >= 4) || !(minimig_config.cpu & 0x03);
```

`cpucfg == 0` is the 68000, which is fx68k. That is the **CDTV** profile and the
**A500** profile — and the A500 profile was added specifically so people could
run A500 software, which makes "this machine cannot save" a gap in a feature that
was just built rather than a pre-existing limitation.

The lockout is honest: `cpu_wrapper` exports TG68K's architectural state and
fx68k has no equivalent, so without this the request would be swallowed silently
in RTL. The job is to give fx68k the same three things TG68K already has —
export, park, restore — and then delete the gate.

## What already exists

The whole framework is CPU-agnostic apart from the CPU itself:

- `ss_ctrl` sequences the payload and knows nothing about which core is running.
- `ss_state_fanout` walks the state vector back into the machine on `clk_sys`,
  one register per cycle, and drives `cpu_wr_index` / `cpu_wr_data` /
  `cpu_pc_wr` / `cpu_sr_wr` / `cpu_usp_wr` / `cpu_vbr_wr` / `cpu_cacr_wr` /
  `cpu_resume` through `cpu_wrapper`.
- `cpu_wrapper` already muxes everything else on `cpucfg[1:0]`, including the
  bus, `cacr` and `vbr`. The savestate ports are the one thing it does not mux.
- `SS_STATE_LIST` carries D0-D7, A0-A7, PC, USP, VBR, SR, CACR. A 68000 has no
  VBR and no CACR; `cpu_wrapper` already hardwires `cacr = 1` and `vbr = 0` in
  the fx68k branch, so those two fields simply go nowhere on the way back in and
  the vector does not change shape.

So this is not new architecture. It is one more implementation behind an
interface that already has one.

## The hazard, stated first

**fx68k's `PcL`/`PcH` are almost certainly a fetch pointer, not the
architectural PC.**

Evidence from `rtl/fx68k/fx68k.sv`: `PcL` is loaded from `auReg[15:0]` and `PcH`
from `auReg[31:16]` — `auReg` is the address unit, which is what generates
addresses including prefetch. TG68K had exactly this shape: `TG68_PC` is the
fetch pointer and `exe_pc` is the architectural PC, and saving the wrong one
produced a machine that resumed decoding operand words as instructions. That bug
cost days and is written up in `2026-08-10-amigacd-savestates-phase1b1-restore.md`.

A 68000 prefetches two words, so the fetch pointer runs up to 4 bytes ahead of
the instruction being executed, and the offset is not constant — it depends on
where in the instruction the freeze lands.

**Nothing else in this plan matters until this is settled.** It is settled by
measurement, not by reading: the round-trip bench below freezes at every one of
N successive points across a known program and checks the CPU resumes at the
same place. A wrong PC fails at most freeze points and passes at a few, which is
exactly the signature that made the TG68 version look intermittent.

## Work, in order

### 1. A round-trip bench, before any RTL

Mirror `rtl/sim/tg68k/tg68k_ss_tb.vhd`, which already has the shape:

- PHASE 1: does the bench run the CPU at all
- PHASE 1b: where can the hardware park
- PHASE 2 (RED): restore without the re-seed, expect failure
- PHASE 3 (GREEN): the same scan with the re-seed
- PHASE 4: capture the state and put it straight back, at 80 successive freeze
  points, and require all 80

fx68k is SystemVerilog, so this is Icarus rather than GHDL and can go in CI
beside the akiko benches. Build it against a small ROM image the way the TG68
bench does.

The bench is first because it is the only thing that can answer the PC question,
and because a 80/80 round trip is the acceptance criterion for the whole task.

### 2. Export

Add to `fx68k.sv`, muxed at `cpu_wrapper` on `cpucfg`:

- `regs68L[0..15]` / `regs68H[0..15]` — D0-D7 then A0-A6 and A7. Note A7 is
  index 15 for user mode and `REG_SSP` (16) for supervisor; `rxMux` already
  does `pswS ? REG_SSP : 15`, so the export has to follow the same rule or the
  saved A7 belongs to the wrong stack.
- `REG_USP` (15) and `REG_SSP` (16) — the vector carries USP separately, and
  which one is "A7" depends on `pswS`.
- `PcL`/`PcH` — whatever they turn out to mean, per §1.
- `psw` — already assembled as `{pswT, 1'b0, pswS, 2'b00, pswI, ccr}` at line 307.

`REG_DT` (17) is scratch, not architectural. Leave it.

### 3. Park

TG68K parks at `decodeOPC` and `cpu_wrapper` latches it, because `ss_resume`
clears the signal. fx68k's equivalent boundary is `enT1 && Nanod.Ir2Ird` — the
edge where the fetched instruction moves into IRD, i.e. the start of an
instruction.

The park mechanism itself is already in `cpu_wrapper` (`ss_parked`, held until
`ss_arm` drops) and is CPU-independent; only the boundary signal changes. fx68k
is clocked by `enPhi1`/`enPhi2` clock enables rather than a clock, so the park
holds by gating those, which is closer to the freeze mechanism than to TG68K's
`clkena`.

### 4. Restore

A 68000 has no architectural way to load the PC and SR from outside. TG68K got a
purpose-built `ss_resume` in the kernel. For fx68k the least invasive route is
the one the chip itself uses: **the reset microcode**, with the two vector
fetches substituted.

Reset reads the initial SSP from `$0` and the initial PC from `$4`. If the
restore intercepts those two bus reads and returns the saved A7 and the saved
PC, the CPU enters the state machine at a known point with the right PC without
any new microcode. The remaining registers are written directly, and `psw` last.

Microcode entry points worth knowing about, from `fx68k.sv`: `RSTP0_NMA`,
`HALT1_NMA`, `TRAC1_NMA`, `ITLX1_NMA`.

This is the part most likely to need a second approach. The fallback is a
direct write port on `PcL`/`PcH` and `psw` plus a microcode jump, which is what
TG68K ended up with.

### 5. Remove the gates

- `Minimig.sv`: `ss_supported = |cpucfg` — delete.
- `menu.cpp`: `minimig_ss_blocked()` — drop the `!(cpu & 3)` term, keep the Z3
  fast RAM one.

Do this last. While the gate stands, a half-finished fx68k path cannot damage
anyone's machine.

## Acceptance

- 80/80 freeze points round-trip in the bench, matching what TG68K achieves.
- A500 profile: save and restore a floppy game, in session.
- CDTV profile: the same.
- Both survive an Amiga reboot and a cold MiSTer reboot, which is where every
  savestate bug found so far has actually shown up.

## What this does not cover

Nothing about the payload format changes, so existing save states stay valid and
a 68000 save is the same shape as a 68020 one. `cacr`/`vbr` ride along as zero.
