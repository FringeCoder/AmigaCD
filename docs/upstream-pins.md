# Upstream pins

Commits the analysis and the line numbers in `docs/integration-plan.md` were read
against. Re-verify line numbers after moving off these.

| Repo | Branch | Commit | Date |
|---|---|---|---|
| `kblood/Main_MiSTer-cd32-cdtv` | `amiga-console` | `52625a4` "AmigaCD: add Save settings to the console OSD" | 2026-06-14 |
| `kblood/Minimig-AGA-cd32-cdtv-native-akiko` | `amiga-console` | `c5eb180` | 2026-06-14 |
| `Anime0t4ku/Main_MiSTer_Physical_Disc` | `master` | `adf39fb` "speed improvements" | 2026-07-24 |
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
