# AmigaCD

Work towards running the MiSTer **Amiga CD32 / CDTV cores from a real CD-ROM drive**
attached to the MiSTer, instead of from a CHD/CUE image on the SD card.

## The three upstreams

| Role | Repo | Branch |
|---|---|---|
| Physical disc userspace | [`Anime0t4ku/Main_MiSTer_Physical_Disc`](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) | `master` |
| CD32/CDTV userspace driver | [`kblood/Main_MiSTer-cd32-cdtv`](https://github.com/kblood/Main_MiSTer-cd32-cdtv) | `master` |
| CD32/CDTV core RTL | [`kblood/Minimig-AGA-cd32-cdtv-native-akiko`](https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko) | `cd32-native-mvp` |

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

Planning. Nothing built yet.
