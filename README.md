# AmigaCD

Work towards running the MiSTer **Amiga CD32 / CDTV cores from a real CD-ROM drive**
attached to the MiSTer, instead of from a CHD/CUE image on the SD card.

**This is the userspace half** — a `Main_MiSTer` fork, which builds
`MiSTer_AmigaCD`. The core half, the RTL that Quartus fits into the `.rbf`,
is [`FringeCoder/AmigaCD_MiSTer`](https://github.com/FringeCoder/AmigaCD_MiSTer),
and its README carries the feature list and its status. Several features here
are split across the two repositories, so it is worth knowing which one you are
looking at. This repository is also the home of `rtl/snac_psx.v`, which both
core repositories vendor a copy of; see [`rtl/README.md`](rtl/README.md).

## Upstreams

Three we are built from, two we track for improvements:

| Role | Repo | Branch |
|---|---|---|
| Physical disc userspace | [`Anime0t4ku/Main_MiSTer_Physical_Disc`](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) | `master` |
| CD32/CDTV userspace driver | [`kblood/Main_MiSTer-cd32-cdtv`](https://github.com/kblood/Main_MiSTer-cd32-cdtv) | `amiga-console` |
| CD32/CDTV core RTL | [`kblood/Minimig-AGA-cd32-cdtv-native-akiko`](https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko) | `amiga-console` |
| *Watched* — parallel disc effort | [`theshaneobrien/mister-disc-drive-support`](https://github.com/theshaneobrien/mister-disc-drive-support) | `physcd` |
| *Watched* — the root upstream | [`MiSTer-devel/Main_MiSTer`](https://github.com/MiSTer-devel/Main_MiSTer) | `master` |

The last two are not merged from automatically. `MiSTer-devel` matters because
nobody is rebasing the forks between us and it, so keeping current is our job.
See [`docs/upstream-pins.md`](docs/upstream-pins.md) for the pins, the sync
procedure, and what is worth taking from each.

Fetch them into `upstream/` with `scripts/fetch-upstream.sh` (not tracked), or
use the git remotes: `physdisc`, `cd32-main`, `discdrive`, `mister-upstream`.

## Key finding

**No RTL changes are required.** The Akiko and CR-511 bridges in the core are a pure
UIO transport — every CD sector is fetched by userspace and pushed over SPI. Physical
disc support is entirely a `Main_MiSTer` change.

The blocker is that the two userspace forks meet MiSTer's CD subsystem at different
abstractions:

```
/dev/sr0 -> physical_disc.cpp -> toc_t   (cd.h)
                                   |
                              [ MISSING ]  load_physical_disc()
                                   |
             akiko_cd32.cpp <- drive_t   (ide.h)  <- cdrom_read_raw_sector()
```

`ide_cdrom.cpp::load_chd_file()` already performs a `toc_t` -> `drive_t` translation,
so the adapter is roughly 30 lines of lifted code.

See [`docs/integration-plan.md`](docs/integration-plan.md) for the step-by-step plan and
[`docs/analysis.md`](docs/analysis.md) for the upstream teardown.

## Branches

- `master` — known-good, buildable states only.
- `develop` — integration branch; feature work branches off here.

## Status

This repo *is* Main_MiSTer @ `amiga-console`, with the physical-disc module
grafted on. Phases 0-3 and the phase 4a OSD trigger are done: the adapter, the
bridge changes, the mount plumbing, a **Real CD** row in the AmigaCD console
OSD, seek hints and the collapsed prefetch cache. Per-game NVRAM (item 11) and
hot swap (item 12) are done too, the latter alongside auto-boot-on-insert.

The tree is current with all three upstreams, including 58 commits of
`MiSTer-devel/Main_MiSTer` — nobody between us and it is rebasing, so that is
ours to keep up.

See [`docs/integration-plan.md`](docs/integration-plan.md) for what each phase
actually proved, and what is left.

**It compiles.** [`.github/workflows/build.yml`](.github/workflows/build.yml)
cross-builds on GitHub Actions with the exact pinned toolchain (ARM GNU-A
10.2-2020.11), producing a 1.06 MB stripped `ELF 32-bit LSB executable, ARM,
EABI5` with **zero** compiler warnings. The binary needs at most `GLIBC_2.28`.

CI carries the build because there is no local toolchain: this is a Windows
machine without WSL, and ARM never shipped a Windows-hosted build of the
pinned version.

**It boots a CD32 game from a real disc.** Verified on hardware 2026-07-31:
mount, TOC read, and a game running from the optical drive. The drive measured
~720 kB/s (~351 sectors/s, ~4.7× red book), well above what the CD32 consumes —
so throughput, the plan's headline risk, is retired.

**Per-game NVRAM works too.** Two discs on hardware resolved to distinct save
slots via both identity paths — one by ISO9660 volume label (`ISSUE7`), one by
TOC-geometry UUID because it has no label — and the old save is flushed before
the slot changes.

**Audio CDs play.** That exercises the CDDA path end to end, including the
byte-order normalisation where a mistake would have been unmistakable — white
noise rather than silence.

**Insert a disc and it mounts; take it out and it unmounts.** One drive watcher
covers both auto-boot-on-insert and hot swap, so changing discs mid-session no
longer needs the OSD.

Not yet exercised on hardware: the Real CD reset paths, and everything CDTV.
See "Still to do" in [`docs/integration-plan.md`](docs/integration-plan.md).

## Using it

| | |
|---|---|
| Building and copying to the card | [`docs/deployment.md`](docs/deployment.md) |
| Launching a game straight from the MiSTer menu | [`docs/mgl.md`](docs/mgl.md) |
| Comparing the core against real hardware | [`docs/vamigats.md`](docs/vamigats.md), [`docs/timing-probe.md`](docs/timing-probe.md) |
