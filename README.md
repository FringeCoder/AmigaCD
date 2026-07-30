# AmigaCD

Work towards running the MiSTer **Amiga CD32 / CDTV cores from a real CD-ROM drive**
attached to the MiSTer, instead of from a CHD/CUE image on the SD card.

## The three upstreams

| Role | Repo | Branch |
|---|---|---|
| Physical disc userspace | [`Anime0t4ku/Main_MiSTer_Physical_Disc`](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) | `master` |
| CD32/CDTV userspace driver | [`kblood/Main_MiSTer-cd32-cdtv`](https://github.com/kblood/Main_MiSTer-cd32-cdtv) | `amiga-console` |
| CD32/CDTV core RTL | [`kblood/Minimig-AGA-cd32-cdtv-native-akiko`](https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko) | `amiga-console` |

Fetch them into `upstream/` with `scripts/fetch-upstream.sh` (not tracked).

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
grafted on. Phases 0, 1, 2 and the phase 4a OSD trigger are done: the adapter,
the bridge changes, the mount plumbing and a **Real CD** row in the AmigaCD
console OSD that mounts the drive. Selecting that row while mounted re-reads
the TOC, which is the manual disc-swap path for now.

Phase 3 -- seek hints and collapsing the double cache -- is next, and it is the
phase that decides whether a real disc is fast enough to boot from at all. See
[`docs/integration-plan.md`](docs/integration-plan.md).

Auto-mounting a disc inserted *after* the core has started is designed but not
built: [`docs/superpowers/specs/2026-07-28-physical-cd-autoboot-design.md`](docs/superpowers/specs/2026-07-28-physical-cd-autoboot-design.md).

**Nothing has been compiled.** There is no build toolchain on this machine, so
none of the above has been through a compiler, let alone hardware.
[`scripts/wsl-build.sh`](scripts/wsl-build.sh) is written and waiting.
