# Phase 1C: fast RAM in the save state

**Do not start this until 1B is complete, working on hardware and merged.** It
changes the transport, which every part of 1B depends on; doing both at once
means a hardware failure has two candidate causes instead of one. New branch off
the merged 1B.

## Goal

Save and restore the machine's fast RAM, so a game that keeps state there
resumes correctly. Fast RAM is in use on this machine (4 MB), and today a save
silently omits it: the restore looks successful and quietly drops several
megabytes of the machine. That is the worst failure shape available.

## How much is actually achievable

The FastRAM setting offers `none / 2M / 4M / 8M`, then jumps to `256M / 384M`
(`support/minimig/minimig_config.cpp:30`). There is nothing between, and there
cannot be a Zorro II option above 8 MB — **8 MB is the Z2 architectural
ceiling**. Everything above it is a Z3 board.

So:

- **Target: full support to 8 MB.** That covers every Z2 setting, which is the
  whole of the useful range for this core's machines.
- **12 MB is not available to offer.** It is not a Z2 size, and adding it as a
  menu entry would mean a Z3 board, which brings the problems below.
- **Z3 (indices >= 4) stays refused.** Two independent reasons: 256-384 MB
  cannot be carried in any plausible window, and Z3 already collides with the
  save window's DDR3 region, which is why `minimig_ss_blocked()` refuses it
  today.

## Transport

Fast RAM lives in **DDR3**, not SDRAM: `Minimig.sv`'s
`zram_sel = |ram_addr[28:26]` routes Zorro fast RAM to `ddram_ctrl` (ram2). So
capture is a DDR3-to-DDR3 copy on the master `ss_ctrl` already owns — the chip
RAM path, which borrows the SDRAM CPU port, does not extend to it. This is a
second streaming path, not a bigger loop.

Current window: `SS3E000000:400000` — 4 MB per slot, four slots,
`0x3E000000`-`0x3F000000`.

Needed: 2 MB chip + 8 MB fast + the state vector and shadow, so **at least
10 MB** per slot.

`user_io.cpp` constrains the window to `ss_base >= 0x20000000` and
`ss_base + ss_size < 0x40000000`, which leaves 512 MB of range to place it in.

Proposal: **16 MB slots, four of them, base `0x3C000000`** — 64 MB, ending
exactly at DDR3's top. Sixteen is a round number that leaves headroom over the
10 MB minimum without another resize later.

Before committing to that base, verify nothing else in the core uses
`0x3C000000`-`0x40000000`. With Z3 refused, Z2 fast RAM occupies only low
addresses, but the A2065 network buffers and the CD audio path are also DDR3
users and must be checked, not assumed.

Consequences:

- config string token becomes `SS3C000000:1000000`
- `SS_SLOT_BASE` and `SS_SLOT_STRIDE` change; note the existing comment there
  about a truncated 29-bit literal, which once put the window inside fast RAM
- the host picks the new size up from the config string automatically
  (`user_io.cpp` parses base and size), so no host change is needed for sizing
- each `.ss` file grows to ~10 MB, and `process_ss` reads all four slots on
  every restore press — 40 MB per press. Worth measuring before accepting.

## Format

The fast RAM section is **variable length**: its size follows the configured
fast RAM, so `CORE_WORDS` stops being a localparam and becomes a runtime value.
That is the invasive part of this phase.

- the configured size has to reach `ss_ctrl` as an input, decoded the same way
  `minimig_ss_blocked()` decodes it, from the same bits
- `CORE_WORDS_24` / `CORE_WORDS_32` and `MIN_LENGTH` follow that input rather
  than a constant
- payload order: fingerprint, state vector, register shadow, chip RAM, fast RAM.
  Fast RAM last, so a reader that stops early still has a coherent machine
- a state saved with one fast RAM size restored into a machine with another must
  be refused. The length gate catches it for free, since the length changes with
  the size — worth asserting explicitly rather than relying on it

## Refusal

`minimig_ss_blocked()` currently refuses Z3 only, for the collision reason. It
should refuse anything the payload cannot carry, by size, with the reason
distinguishable in the toast:

- fast RAM index >= 4 (Z3): refused, cannot be carried and collides
- any configured size that exceeds the slot: refused

The user-visible rule becomes "save states work up to 8 MB fast RAM", which is
the whole Z2 range and needs no new menu entry.

## Ordering on restore

Fast RAM is written back like chip RAM: inside the freeze, before the register
shadow replay and before the cache flush. The 68020 caches fast RAM as well as
chip RAM, so the existing `cache_flush` covers it — but confirm that, because it
was chip RAM that motivated the flush.

## Testing

- `ss_ctrl_tb` gains a fast RAM section with a parameterised size, the same way
  `SHADOW_ENTRIES` was parameterised so the mock DDR3 could hold it
- a case with fast RAM size zero, which must produce a payload identical in
  shape to 1B's — the common configuration must not regress
- a case where the saved size and the configured size differ, which must refuse
- mutation: dropping the fast RAM section from the CRC while still streaming it,
  which round-trips against itself and must fail the reference check
