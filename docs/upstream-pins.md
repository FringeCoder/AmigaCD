# Upstream pins

Commits the analysis and the line numbers in `docs/integration-plan.md` were read
against. Re-verify line numbers after moving off these.

| Repo | Branch | Commit | Date |
|---|---|---|---|
| `kblood/Main_MiSTer-cd32-cdtv` | `amiga-console` | `52625a4` "AmigaCD: add Save settings to the console OSD" | 2026-06-14 |
| `kblood/Minimig-AGA-cd32-cdtv-native-akiko` | `amigacd-core` | `a27f4e8` "AmigaCD: identify as a separate core" | 2026-07-29 |
| ^ **but we build `feature/misterfloppy`** — see "Which core branch to build" | | | 2026-08-03 |
| ^ previous pin, superseded 2026-08-01 | `amiga-console` | `c5eb180` | 2026-06-13 |
| `Anime0t4ku/Main_MiSTer_Physical_Disc` | `master` | `de40622` "Merge PR #9, remount on savestate" | 2026-08-23 |
| ^ **head is `f2963eb` (2026-08-30), NOT taken** — DVD player core support and autolaunch, a feature for a different core; see below | | | |
| ^ previous pin, superseded 2026-08-24 | | `748eb1d` "cd-da fixes" | 2026-08-03 |
| ^ `ab2f602` was recorded here on 2026-08-07 but never extracted — see below | | | |
| ^ its fork base (upstream `Main_MiSTer`) | | `7317947` | 2026-07-13 |
| `MiSTer-devel/Main_MiSTer` | `master` | `915ca33` "minimig: repair the reserved CPU value when loading a config (#1297)" | 2026-08-29 |
| ^ previous pin, superseded 2026-08-31 | | `0a8fb44` | 2026-08-23 |
| `MiSTer-devel/Minimig-AGA_MiSTer` | `MiSTer` | `74d6ce0` "gary: decode the custom registers at $DFxxxx, not across all of $C0-$DF (#235)" | 2026-08-28 |
| ^ previous pin, superseded 2026-08-31 | | `c26772a` "Akiko: PIO command register, and DRIVEXMIT for zero-result commands (#229)" | 2026-08-27 |
| ^ previous pin, superseded 2026-08-27 | | `b265a3b` | 2026-08-23 |
| ^ superseded 2026-08-24 | | `809b955` "A2065 Ethernet" | 2026-07-26 |
| ^ our core's fork point | | `3ab91cd` "Release 20260220." | 2026-02-20 |

`52625a4` is merged into this repo's `master` as the working tree.

**A newer userspace line exists and we are not on it.** `origin/amigacd-userspace`
is the rebased successor to `amiga-console`, the same move we already made on the
core side. It is a content superset, and it contains `7317947` — the
physical-disc fork's own base, which our line predates by four months. See
[userspace-branch-move.md](userspace-branch-move.md) for the gained/lost
analysis and why moving first makes the physical-disc merge much smaller.

**Check what your local clone is actually on.** As of 2026-08-07 the
`upstream/main-cd32-cdtv` working tree was checked out on `master` — the branch
this document warns contains none of the CD32 work — even though
`scripts/fetch-upstream.sh` checks out `amiga-console`. Anything read out of it
in that state was read against the wrong branch.

## Branch trap

`master` on **both** of kblood's repos tracks plain upstream Main_MiSTer /
Minimig-AGA and contains none of the CD32 work. Cloning the default branch of
`Main_MiSTer-cd32-cdtv` gets you `cd32-native-mvp`; cloning `master` gets you
nothing useful. The branches that matter:

| Branch | Contains |
|---|---|
| `cd32-native-mvp` | Akiko + CDTV native bridges (through 2026-05-26) |
| `amiga-console` | superset: adds CDDA subcode Q-stream, play-end STCH retry, and the AmigaCD console OSD (11 commits ahead, 0 behind) |

We were based on `amiga-console` for the same reasons — the CDDA subcode and
STCH work overlaps the physical-disc audio path, and its OSD rework is where the
"Physical CD" mount entry lands. **Moved to `amigacd-core` on 2026-08-01.**

## Core branch move: `amiga-console` -> `amigacd-core`

`amigacd-core` is newer (2026-07-29 vs 2026-06-13) and is **not a superset**.
The two lines fork at `59df90f` (2026-05-30): 8 commits on `amiga-console`,
17 on `amigacd-core`. Verified by content, not commit identity.

**Gained**

- `rtl/A2065/*` — the Ethernet FPGA half, via the upstream merge `d7c36cc`.
  This is what makes the host-side A2065 and the Advanced OSD row functional.
- `b3f46cb` — owner-freeze cross-engine DMA ack fix plus a Deep Core residual
  fix, a later and differently-shaped attack on the ground `fce5cc5` /
  `32c0a3c` covered on the old line.
- Upstream `Minimig-AGA` through `809b955`, de-narrated RTL comments, and
  `8178ddd` removing the debug trace infrastructure.
- `a27f4e8` keeps `CONF_STR = "AmigaCD"`, so `is_amigacd()` still resolves and
  the console OSD, per-game NVRAM and physical-disc paths are unaffected.

**Lost — resolved on hardware 2026-08-01, do not re-litigate**

`c3d412b` (Fix B, "private `chipRD_dma` return reg for DMA-stolen slots") is
**absent by content**: `chipRD_dma` appears nowhere in `amigacd-core`'s
`sdram_ctrl.v` or `chipdma_arb.v`. `3a1005f`'s in-flight bleed gate also differs
(`slot_cnt` 12 hits vs 7).

This was the one thing that could have sent us back to `amiga-console`, since
Fix B and the Deep Core commits are exactly about CD sector delivery. **A core
built from `amigacd-core` was flashed and tested on 2026-08-01 and works**, so
`b3f46cb`'s owner-freeze does cover the ground Fix B covered and cherry-picking
`c3d412b` on top is unnecessary. Do not add it back speculatively.

Worth keeping in mind if a *future* sector-delivery bug appears: reverting Fix B
on the old line moved timing by only ~160 ps, so it was never shown to be doing
observable work there either. It is not a proven-good patch being withheld.

**Consequence for userspace.** `8178ddd` removes the RTL trace rings, so
`akiko_drain_trace()`, `chipset_trace_drain()` and `z2_trace_drain()` now poll
addresses nothing decodes. All three are bounded (128 / `RING_DEPTH`), so there
is no hang, but if an undecoded read returns `0xFF` the akiko drain runs its
full 128 iterations at 4 SPI reads each — 512 wasted transfers per poll. Taking
kblood's `63d848d` (removes the drains and the trace files outright) is the
matching userspace change.

## Which core branch to build

**Build `feature/misterfloppy`, not `amigacd-core`.**

`amigacd-core` is deliberately kept as a clean mirror of kblood's tip so it stays
pullable and `git fetch` never conflicts. Our RTL work sits on
`feature/misterfloppy`, seven commits above it, and that is what produces the
RBF in use:

```bash
git -C upstream/core-cd32-cdtv checkout feature/misterfloppy
```

Building `amigacd-core` gives a core with **no MiSTer Floppy support** — the
userspace will send UIO `0xFA`/`0xFB`/`0xFC` that nothing decodes, the floppy
rows will offer external drives that never respond, and it will look like a
regression rather than a wrong branch.

That branch carries the MiSTerFloppy RTL and the `paula_floppy` flux-select
retiming, plus four Quartus settings commits that are all dead ends kept for
the record (`SEED 2`, `SEED 3`, raised fitter effort, and the revert of it).

**`Minimig.qsf` is byte-identical to `amigacd-core`.** The raised
`PLACEMENT_EFFORT_MULTIPLIER` / `ROUTER_EFFORT_MULTIPLIER` and
`ROUTER_TIMING_OPTIMIZATION_LEVEL MAXIMUM` were added while chasing a timing
failure diagnosed as routing, which turned out to be logic depth — the
flux-select retiming is what actually fixed it. Tested on defaults 2026-08-03:

| Fitter settings | Worst slack | ALMs |
|---|---|---|
| raised effort (3.0, MAXIMUM) | +0.143 ns | 23,609 |
| **stock (kblood's)** | **+0.177 ns** | **23,555** |

So the extra effort was not merely unnecessary, it was marginally worse. Stock
settings stand, and the only deviation from upstream is RTL. Do not re-add the
effort multipliers if timing gets tight later — that is not where the margin
came from.

## Other branches (not used)

`cd32-cdtv-floppy-ipf`, `dual-mouse` in both repos; `vrakiko-cd32`,
`dual-mouse-pr` in the core repo only.

## The pin that was written down but not taken

This table said `ab2f602` from 2026-08-07 until 2026-08-24, and the module on
disk was `748eb1d` the whole time — checked byte for byte. The doc moved and the
re-extract did not follow, so for two and a half weeks the pin described code
that was not there.

Worth a habit rather than an apology: move the pin in the same commit as the
files, and if they ever have to be separate, verify with

```bash
git show <pin>:support/physical_disc/physical_disc.cpp | diff - support/physical_disc/physical_disc.cpp
```

which is the whole check, because the module is a verbatim copy by design.

## Syncing the physical_disc module

`support/physical_disc/{physical_disc,physical_disc_acoustic}.{cpp,h}` are a
**verbatim** copy of the pinned commit. Keep it that way — it is what makes a
sync a re-extract rather than a merge:

```bash
git fetch physdisc
for f in physical_disc.cpp physical_disc.h \
         physical_disc_acoustic.cpp physical_disc_acoustic.h; do
    git show physdisc/master:support/physical_disc/$f > support/physical_disc/$f
done
```

`physical_disc_launch.cpp` stays excluded. It includes `../megacd/megacd.h`,
`../psx/psx.h`, `../saturn/saturn.h`, `../neogeo/neogeocd.h`, `../3do/3do.h`,
`../pcecd/pcecd.h`, `../cdi/cdi.h`, `../arcade/mra_loader.h` and
`../../achievements.h` — none of which exist in this tree.

**Check the disc-type enum on every sync.** `d24d2b0` inserted
`PHYSICAL_DISC_DISC_CDI` / `_MDPLUS` / `_SNES` *in the middle* of
`physical_disc_disc_t`, moving `PHYSICAL_DISC_DISC_UNKNOWN` from 8 to 11. That
value is hashed into `derive_toc_uuid()`, so anything keying a save slot off it
would have been silently relocated. `cdrom_parse` now pins
`PHYSICAL_DISC_DISC_NONE`, fixed at 0 by the enum, which cannot drift. Any
future code passing a type into `physical_disc_save_name()` needs the same care.

## Watched, not merged

Two repos we track for improvements rather than build from.

### `theshaneobrien/mister-disc-drive-support` @ `physcd`

*"A custom MiSTer fork that can autoboot retail CD's into their respective
cores."* Active, 26 stars. Branches from `MiSTer-devel/Main_MiSTer` at
`0f3a6ad` (2026-04-30), the same base our tree descends from, and is 144
commits ahead of it. `main` is a separate distribution tree with no shared
history — the code lives on `physcd`.

**It is a sibling of Anime0t4ku's work, not an independent invention.** The
public API matches function for function modulo prefix (`physcd_open` /
`physical_disc_open`, `physcd_seek_hint` / `physical_disc_seek_hint`, and so on
through swap, watch, poll_event, menu_status). Too exact for convergence.

**But the engine underneath is a full rewrite.** `lane_cursor`, `RING_SECTORS`,
`SYNC_IO_BURST`, `subch_ok`, `leadout_lba` and `warmup` — the whole two-lane
ring and warmup design our integration is built around — appear **zero** times
in `support/physcd/mister_physcd.cpp`. It is 2151 lines against our 1389.

| | Anime0t4ku (ours) | theshaneobrien |
|---|---|---|
| Module | `support/physical_disc/` | `support/physcd/` |
| Engine | 1389 lines, two-lane ring + warmup | 2151 lines, different design |
| Acoustic | 373 lines | 448 lines |
| Autoboot | in `physical_disc_launch.cpp` | `physcd_autoboot.cpp`, 745 lines |
| `save_name()` | yes — label, else TOC UUID | **no** — only label and serial |

**Recommendation: do not switch engines.** Ours is hardware-proven at ~351
sectors/s with real headroom, so the engine is not a bottleneck, and swapping
would replace working code with 2151 unexercised lines while losing
`physical_disc_save_name()` — which per-game NVRAM depends on. Mine it for
specific fixes instead, particularly if a drive turns out to have quirks ours
mishandles, since a full rewrite of the same interface is exactly where such
fixes would live.

**Its autoboot is not our autoboot.** `physcd_autoboot.h` exports
`mount_current_core()`, `swap_current_core()`, `menu_row()`, `is_menu_row()` —
the same menu-level shape as Anime0t4ku's launch file: identify a disc from the
MiSTer menu and load the matching core. Ours is in-core mount-and-boot on
insert. Useful as reference for the poller and for `swap_current_core()`, not
as something to lift.

### `MiSTer-devel/Minimig-AGA_MiSTer` @ `MiSTer`

The RTL root upstream, the core-side counterpart to `Main_MiSTer`. Our core
(`kblood/Minimig-AGA-cd32-cdtv-native-akiko` @ `amiga-console`) descends from it,
forking at `3ab91cd` (2026-02-20). Only **5 commits** ahead of that point, so
the RTL upstream moves far more slowly than the userspace one.

Added as remote `minimig-upstream` in the core clone, not in this repo — it is
Verilog, and nothing in it builds here.

**The A2065 host/FPGA split — verified 2026-08-01.** The five commits include
`809b955`, which adds `rtl/A2065/*` (the LANCE FPGA half). Our `amiga-console`
pin predates it:

| Tree | Has `rtl/A2065/` |
|---|---|
| `amiga-console` @ `c5eb180` (what we build) | **no** |
| `minimig-upstream/MiSTer` @ `809b955` | yes |
| kblood's `amigacd-core` @ `a27f4e8` | yes |

So this repo carries the A2065 **host** half — pulled in by the `Main_MiSTer`
merge (`#1247`) and surfaced by the AmigaCD Advanced OSD page — against a core
with no A2065 in it. The OSD row selects an interface and the poller runs, but
no FPGA logic is listening, so nothing reaches the Amiga. Making Ethernet
actually work needs a core containing `rtl/A2065/`.

Harmless meanwhile, and checked rather than assumed: `a2065_ddr3_clear()` writes
24 bytes at `0x1FF08000`/`+0x10`/`+0x18` inside the window at
`DDR3_FLAT_BASE 0x1FF00000`. The only other mapper in the tree is
`minimig_share` at `0x27FF4000`, so there is no collision — on a core without
the FPGA half those bytes are simply unread.

### `MiSTer-devel/Main_MiSTer` @ `master`

The root upstream. Our tree reaches it through kblood's fork, which reaches it
through its own base — and **nobody in that chain is rebasing**, so staying
current is ours to do.

As of 2026-07-31 it is **58 commits** ahead of `0f3a6ad`, the base our tree
shares with it. Unlike the physical_disc module this cannot be a re-extract: it
touches `menu.cpp`, `ide.cpp`, `ide_cdrom.cpp` and `user_io.cpp`, all of which
we have modified. Treat it as a real merge with conflicts, on its own branch,
with a hardware test afterwards.

