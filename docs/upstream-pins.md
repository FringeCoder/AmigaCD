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
