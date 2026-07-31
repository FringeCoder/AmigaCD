# Upstream pins

Commits the analysis and the line numbers in `docs/integration-plan.md` were read
against. Re-verify line numbers after moving off these.

| Repo | Branch | Commit | Date |
|---|---|---|---|
| `kblood/Main_MiSTer-cd32-cdtv` | `amiga-console` | `52625a4` "AmigaCD: add Save settings to the console OSD" | 2026-06-14 |
| `kblood/Minimig-AGA-cd32-cdtv-native-akiko` | `amiga-console` | `c5eb180` | 2026-06-14 |
| `Anime0t4ku/Main_MiSTer_Physical_Disc` | `master` | `d24d2b0` "autoboot added" | 2026-07-31 |
| ^ its fork base (upstream `Main_MiSTer`) | | `7317947` | 2026-07-13 |

`52625a4` is merged into this repo's `master` as the working tree.

## Branch trap

`master` on **both** of kblood's repos tracks plain upstream Main_MiSTer /
Minimig-AGA and contains none of the CD32 work. Cloning the default branch of
`Main_MiSTer-cd32-cdtv` gets you `cd32-native-mvp`; cloning `master` gets you
nothing useful. The branches that matter:

| Branch | Contains |
|---|---|
| `cd32-native-mvp` | Akiko + CDTV native bridges (through 2026-05-26) |
| `amiga-console` | superset: adds CDDA subcode Q-stream, play-end STCH retry, and the AmigaCD console OSD (11 commits ahead, 0 behind) |

We are based on `amiga-console`. It was chosen over `cd32-native-mvp` because
the CDDA subcode and STCH work overlaps the physical-disc audio path, and
because its OSD rework is where the "Physical CD" mount entry has to land.

## Other branches (not used)

`cd32-cdtv-floppy-ipf`, `dual-mouse` in both repos; `vrakiko-cd32`,
`dual-mouse-pr` in the core repo only.

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

### `MiSTer-devel/Main_MiSTer` @ `master`

The root upstream. Our tree reaches it through kblood's fork, which reaches it
through its own base — and **nobody in that chain is rebasing**, so staying
current is ours to do.

As of 2026-07-31 it is **58 commits** ahead of `0f3a6ad`, the base our tree
shares with it. Unlike the physical_disc module this cannot be a re-extract: it
touches `menu.cpp`, `ide.cpp`, `ide_cdrom.cpp` and `user_io.cpp`, all of which
we have modified. Treat it as a real merge with conflicts, on its own branch,
with a hardware test afterwards.
