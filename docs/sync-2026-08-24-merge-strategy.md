# Merge strategy: ours, upstream's, or a split

Follow-up to `docs/sync-2026-08-24-assessment.md`. The question is what wins
where our implementation and upstream's now solve the same problem. Answered
per subsystem by reading both sides, not by policy.

**Conclusion: a split, and not an even one.** Neither "keep ours" nor "take
theirs" survives contact with the code. Ours wins on identity, naming and the
AmigaCD product surface — three places where upstream would actively break us.
Upstream wins on the CD32/CDTV mechanism — the sector transport, the CDTV
persistence we do not have at all, and a real address-width bug in our core.
The split is roughly 70/30 in upstream's favour by volume and the other way by
risk.

## Why not "keep ours"

Three findings kill it outright.

**Our core has a 24-bit DMA address where upstream has 32.** `rtl/cdtv_bridge.v`
declares `output [23:0] cdtv_dma_baddr` and `reg [23:0] drain_baddr`; upstream's
is `[31:0]` on both, from `0e1eed3` (#227) "carry the full 32-bit DMA address
(Z3 fast RAM aliases onto the vector table)". A DMA write aimed at Z3 fast RAM
truncates and lands on the vector table instead. That is not churn to weigh
against our stability — it is a live bug in what we ship, in the same family as
the save-state DMA corruption already on record.

**Upstream's Akiko sector ingest is faster than our fast path and costs less.**
Measured, not asserted:

| Path | Elements pushed | Mechanism | Cost |
|---|---|---|---|
| ours, default | 2352 bytes | `UIO_SECTOR_RD` slot 1 + `fpga_spi_fast_block_write_8` | **0.95 ms/sector** (hardware A/B, 2026-05-07) |
| ours, `AKIKO_SLOW_PUSH=1` | 2352 bytes | per-byte `spi_w` on 0xF500, SSPI_ACK each | 2.3 ms/sector |
| upstream | **1176 words** | `fpga_spi_fast_block_write` on 0xF500, 64-word chunks, 10 µs gaps | ≈0.65 ms/sector |

Both fast paths are the same primitive — two `fpga_gpo_writeN` per element, no
ACK round-trip. Upstream simply pushes half as many elements because
`rtl/akiko.v` now takes `input [15:0] hps_sec_word` into a 1176-word buffer
(`sec_wr_ptr != 11'd1176`) instead of our byte-wide `sector_buffer [2352]`. The
18 pacing gaps from `343c75c` (#1277) cost ~0.18 ms and buy the reliability our
chunking never needed to address. Our path also burns hps_io disk slot 1
(`VDNUM=2`) and requires the `sd_ack`/`sd_buff` mux inside `akiko.v` — RTL only
we maintain. Upstream's `akiko.v` has no `sd_buff` sector port at all, so after
the core merge our fast path is dead silicon unless we keep our own ingest.

**We have no CDTV NVRAM or memory card at all.** `grep -i nvram
support/minimig/cdtv_cd.cpp` returns nothing. Upstream's 290-line block is pure
addition: per-disc `.nvr` and `.card` files, dirty-observed save policy, and a
verify-after-write pass. Rejecting it means writing it ourselves later.

## Why not "take theirs"

Two findings kill that one just as fast.

**`is_minimig()` would stop recognising our core.** Upstream now returns a
tri-state and gates the CD polls on it:

```c
is_minimig_type = (!strcasecmp(orig_name, "minimig") || !strcasecmp(orig_name, "minimigcd")) ? 1 : 2;
...
if (is_minimig() == 2) { akiko_cd32_poll(); cdtv_cd_poll(); }
```

Exact match on two names. Our `CONF_STR` is `AmigaCD`, and ours prefix-matches
precisely so renamed variants still resolve. Take upstream's verbatim and the
Akiko and CDTV polls never run on our core — silent, total.

**Our NVRAM naming is a strict superset of theirs, and theirs cannot name a
physical disc.** Both derive `cd32-<fnv1a_64(basename)>.nvr` — identical scheme,
common ancestry. Ours adds two layers on top: `discnames.cfg` friendly-name
overrides, and `physical_disc_save_name()` identity for a real disc in a real
drive. Upstream's `cdtv_compute_save_path()` hashes `strrchr(cd_path, '/')` —
with no file there is no path, so per-game saves for physical discs simply do
not exist on their side. This is the feature the pins doc already flags as the
reason not to switch disc engines; the same logic applies here.

Also worth keeping: our `menu.cpp` conflicts are 11 hunks of pure menusub
renumbering (upstream shuffled the Chipset/System rows, we added CPU Speed and
Joystick rows). Taking theirs wholesale would delete the AmigaCD console OSD,
which is the product.

## The split, subsystem by subsystem

| Subsystem | Take | Why |
|---|---|---|
| Akiko sector ingest (RTL + host) | **theirs** | 0.65 vs 0.95 ms/sector, frees hps_io slot 1, deletes the `sd_buff` mux we alone maintain |
| Akiko command/status framing | theirs, unchanged | addresses are already identical on both sides — `0xF400/0xF500/0xF440/0xF200/0xF410` |
| Akiko NVRAM path derivation | **ours** | `discnames.cfg` + `physical_disc_save_name()`; theirs is the subset we grew from |
| Akiko NVRAM transport | theirs | `akiko_ext_block_write()` word-wide upload, matching the new ingest |
| CDTV NVRAM + memory card | **theirs, then extend** | we have none; graft our physical-disc naming onto `cdtv_compute_save_path()` / `_card_path()` the way we did for Akiko |
| Media change without reset | **theirs** | see below — this is the close call |
| CDDA source selection | **both** | keep our `drv->phys` branch and its byte-swap, add their cue+bin branch beside the CHD one |
| CDTV RTL fixes #221/#222/#224/#226/#227 | **theirs** | SCOR frame rate, CDDA volume DAC, chip-bus register path, read-before-side-effects latch, and the 32-bit DMA address |
| `minimig_config` struct | **combined, carefully** | see below |
| `is_minimig()` / core identity | **ours**, plus their `UIO_GET_VMODE` probe | exact-match would unrecognise `AmigaCD` |
| `menu.cpp` rows | **ours**, renumbered onto their layout | our OSD is the product; their renumbering is the tiebreak for row order |
| `ide.cpp`, `ide_cdrom.cpp`, `cfg.*` | **both** | every conflict there is textual adjacency, not disagreement |
| video/file_io/OSD recents/other cores | theirs | no overlap with anything of ours |

### The close call: media change

This is the one place where two working designs collide.

Ours is `akiko_cd32_poll()`'s Phase 33-D/33-E block — 85 lines, WinUAE-shaped,
preserving `cd_initialized` across a mediachange, with `had_prior_unmount`
distinguishing "booted with no CD, then inserted" (BIOS sits on the spinning-CD
splash and ignores the `0x0a/0x01` push — verified against a 107-line Akiko log)
from a hot swap. Its recovery for the stuck case is a deferred `minimig_reset()`.

Upstream's is 23 lines: a `cd_media_level` edge detector that synthesises an
absent announcement before a present one when a swap happens with
`cd_initialized >= 2` (`cd_media_swap_absent`). And upstream explicitly deleted
the reset approach — `f589c8b` "report media state to the CD32 BIOS instead of
resetting the machine", `fea677d` "do not reboot the machine when a disc is
ejected from the start menu".

**Take theirs.** Not because ours is wrong — ours is the one proven on this
hardware — but because upstream is attacking the same BIOS behaviour without
rebooting the machine, and a synthesised removal edge is the better answer than
a reset if it holds. The honest risk is that it does not hold on the no-CD-boot
path, which is exactly what our `had_prior_unmount` case exists for. Keep our
deferred-reset block commented out rather than deleted for one hardware round,
and delete it once the splash case is confirmed fixed. Do not run both — two
edge detectors on the same transition is how a swap becomes a double
announcement.

Note that kblood has independently fixed our Defender of the Crown bug:
`2c410aa` (#1273) is "CDTV memory card persistence + play-start STCH (fixes
Defender of the Crown park)", against our `c59266d` / `fd1aabe` retry-until-acked
play-end STCH. Same bug, different mechanism, and his rides on RTL we would be
taking anyway. Ours goes with our `cdtv_cs_stch` channel; his does not need one.

### The landmine: `minimig_config`

`mm_configTYPE` diverges at the same struct offset:

```
ours:      mm_userportMode userport;
upstream:  mm_hardfileTYPE cd32_drive;  mm_hardfileTYPE cdtv_drive;  uint8_t a2065_mode;
```

Both sides then added their own migration path for configs written before their
field existed — ours keys off `sizeof(mm_configTYPE_NoExtFloppy)` and the magic
size 5152, upstream's off `#1264`'s back-compat. The merged struct needs all
four fields and a migration ladder that recognises **three** historical layouts,
not two. Every saved `.cfg` on the SD card goes through it.

This is also where the drive-slot change bites beyond the struct.
`cd32_active()` currently tests `minimig_config.hardfile[0].cfg == 2`, and
`ide.cpp` decides a CDTV CD slot the same way. Upstream's `cd32_drive` /
`cdtv_drive` replace that overloading, so the guard, the `ide.cpp` mount path
and the OSD rows that toggle it all move together. Do this one first and on its
own, with a config written by the current binary as the regression case.

## Order of work

1. **Core merge**, resolving deep-core files toward ours (save states,
   MiSTerFloppy) and the CD32/CDTV files toward upstream. Drop
   `rtl/cdtv_hps_bridge.v` — upstream removed it when CDTV moved onto the chip
   bus. Drop our `sd_buff` sector mux and `VDNUM=2` slot 1 with it. Fit, both
   slacks positive.
2. **`minimig_config` alone**, struct plus the three-way migration, verified
   against a config the current binary wrote.
3. **`akiko_cd32.cpp` / `cdtv_cd.cpp`** against the new RTL: their transport and
   media-change, our naming, our physical-disc branches.
4. **`menu.cpp`**, our rows onto their numbering.
5. **Adjacency conflicts** — `ide.cpp`, `ide_cdrom.cpp`, `cfg.*`, `user_io.cpp`
   identity — which are keep-both throughout.
6. **Hardware**, both halves together: a CHD boot, a cue+bin CDDA track, a
   physical disc, a hot swap, a no-CD boot followed by an insert, and a per-game
   NVRAM round trip on both a file and a physical disc.
