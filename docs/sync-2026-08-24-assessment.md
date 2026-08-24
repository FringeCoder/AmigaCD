# Sync assessment, 2026-08-24

Every repository listed in `docs/upstream-pins.md`, fetched and measured against
its pin. Written before touching any code beyond the one sync that turned out to
be mechanical.

## Summary

| Repo | Pin | Tip now | Delta | Verdict |
|---|---|---|---|---|
| `MiSTer-devel/Main_MiSTer` @ `master` | `d8c88df` | `0a8fb44` (2026-08-23) | **44 commits** | real merge, 61 conflict hunks, RTL-coupled |
| `MiSTer-devel/Minimig-AGA_MiSTer` @ `MiSTer` | `809b955` | `b265a3b` (2026-08-23) | **4 commits** | real merge, 36 conflicted files, deep-core |
| `Anime0t4ku/Main_MiSTer_Physical_Disc` @ `master` | `748eb1d` | `de40622` (2026-08-23) | 12 commits, 5 touching our files | **done** — re-extracted |
| `kblood/Main_MiSTer-cd32-cdtv` @ `amiga-console` | `52625a4` | `52625a4` | none | nothing to take |
| `kblood/Minimig-AGA-...-akiko` @ `amigacd-core` | `a27f4e8` | `a27f4e8` | none | nothing to take |
| `MiSTer-devel/Menu_MiSTer` @ `master` | `3c3634c` | `3c3634c` (2026-06-03) | none | nothing to take |
| `RobSmithDev/Main_MiSTer` @ `master` | `353089b` | `353089b` | none | nothing to take |
| `RobSmithDev/Minimig-AGA_MiSTer` @ `MiSTer` | `3b2c3dd` | `3b2c3dd` | none | nothing to take |
| `theshaneobrien/mister-disc-drive-support` @ `physcd` | `1a42060` | `1a42060` (2026-07-29) | none | watched only, quiet |

**The headline is that kblood's whole second CD32/CDTV round has landed
upstream** — in `Main_MiSTer` as 29 minimig commits, and in `Minimig-AGA_MiSTer`
as the `cd32_cdtv` branch (14 commits, PRs #217–#227) folded into `MiSTer` by
`2a600e4` "Implement CD32 and CDTV". So both root upstreams are now the place to
sync from, and both of kblood's forks are at their pinned tips with nothing new
on the branches we track. The new work on his other branches
(`cdtv-memory-card`, `cdtv-nvram-persistence`, `pr-akiko-fill-cadence`, and a
dozen more) is the staging ground for those upstream PRs, not a separate line to
mine.

**These two merges are one job, not two.** Taking the userspace side alone
breaks CD32: `#223` "akiko: ingest the host sector stream as 16-bit words"
changes the wire format between `akiko_cd32.cpp` and `rtl/akiko.v`, and `#218` /
`#224` move the CD32 and CDTV host transports onto the ext bus and route CDTV
register access through the chip bus. Our core still has `rtl/cdtv_hps_bridge.v`,
which upstream deleted. This is the same trap `docs/upstream-pins.md` records for
MiSTerFloppy — userspace sending commands nothing decodes, presenting as a
regression rather than as a wrong branch.

## `MiSTer-devel/Main_MiSTer`: `d8c88df` -> `0a8fb44`

44 commits. Grouped by what they are, since the mix matters more than the list:

**kblood's CD32/CDTV round — 29 commits.** The substance of the merge:

- `ac8f375` (#1264) dedicated CD32/CDTV drive slots, with back-compat for
  existing configs; `e7b417d` adds the core-side flag for it
- `dff5567` (#1267) drive the Akiko NVRAM and sector channels over the ext bus
- `af770c5` (#1269) CDTV NVRAM persistence, OSD-open save policy, full-width
  status read
- `2c410aa` (#1273) CDTV memory card persistence + play-start STCH
- `343c75c` (#1277) pace the Akiko ext-bus block write
- `f589c8b`, `fea677d`, `79dc937`, `87c7091`, `cba7300` — Akiko media change
  without resetting the machine, and the two bugs that fall out of it: a swap
  must show the guest a removal rather than a second arrival, and a zero-filled
  sector must not be reported as a successful read
- `fd9d609` (#1281), `a3ae3b0` (#1288), `e239fa5` (#1289) — the removal-edge and
  eject-STCH follow-ups on both the Akiko and CDTV sides
- `688d470`, `f3f0a0c` — raw cue+bin discs in the CDDA path, not just CHD
- `0a8fb44` (#1292) quick-start ROMs resolved through `HomeDir()`
- `b7dd336` (#1283) A2065 CMD doorbell fix, from Nigel Shearman
- `2e1a75b` (#1263) fit the minimig System page inside the 16-line OSD

**Sorgelig's framework and menu work — 9 commits.** `021f866` reorganize the
minimig menu, `43cce3c` CD32/CDTV quick start, `98643c0` presets in the OSD,
`5106991` OSD reordering, `9fda9aa` recents-index fix, `7b5c8de` auto-save on
Alt-F12, `eecde37` an ini sanity-check option, `79aaf02` HDMI off/on fix,
`ebd9628` A2065 config folded into `minimig_config`, `98f2cd1` an unused A2065
function dropped. Plus `4c046de` "Normalize line endings to LF via
.gitattributes" and `ee22d34` project files.

**Unrelated cores — 6 commits.** Atari800/5200 (`7d70868`, `2980794`), x86 Z486
(`e4132d6`), PC110 detection (`a517379`), and two file-I/O error-reporting fixes
from littlegamer87 (`0c2130a`, `5c22c2a`).

### Measured conflict scale

`git merge --no-commit --no-ff mister-upstream/master` onto `develop`: 40 files
merge clean, 12 conflict, 61 conflict hunks.

| File | Hunks |
|---|---|
| `support/minimig/akiko_cd32.cpp` | 22 |
| `menu.cpp` | 15 |
| `support/minimig/cdtv_cd.cpp` | 11 |
| `ide.cpp` | 3 |
| `ide_cdrom.cpp`, `support/minimig/minimig_config.h`, `user_io.cpp` | 2 each |
| `.gitattributes`, `MiSTer.ini`, `cfg.cpp`, `cfg.h`, `support/minimig/minimig_config.cpp` | 1 each |

Most of the 40 clean files are line-ending normalization only — `lib/lzma/*`,
`yc.txt`, `names.txt`, the LPF/coeff tables, `mat4x4.h`. The real clean content
is `file_io.cpp`/`.h`, `video.cpp`, `support/atari8bit/*`,
`support/minimig/minimig_a2065*` and `ide_cdrom.h`.

`.gitattributes` conflicts add/add: we wrote one ourselves (forcing LF because a
CRLF checkout breaks the Makefile and every `.sh`), and upstream has now added
its own. Upstream's line 54 is not a valid attribute name and git warns about it
on every command once merged — worth keeping our version and taking only the
patterns upstream adds that we lack.

### Where the judgment lies

The 22 hunks in `akiko_cd32.cpp` and 11 in `cdtv_cd.cpp` are where our tree and
kblood's have genuinely diverged: ours is a superset carrying the physical-disc
path, per-game NVRAM, disc-name overrides and autoboot, and it already has its
own answers to some of what upstream now ships — our NVRAM work, our STCH
retry, our sector-push path with the `AKIKO_SLOW_PUSH` escape hatch and
`UIO_SECTOR_RD` on `AKIKO_SEC_SLOT` where upstream now uses `UIO_DMA_WRITE`.
Deciding per hunk which implementation survives is the work, and it is not
mechanical.

## `MiSTer-devel/Minimig-AGA_MiSTer`: `809b955` -> `b265a3b`

Four commits, but `2a600e4` "Implement CD32 and CDTV" carries the entire
`cd32_cdtv` branch:

| PR | |
|---|---|
| #217 | native CD32 (Akiko) and CDTV CR-511/DMAC/TPI RTL support |
| #218 | move the CD32/CDTV host transports onto the ext bus |
| #219 | CDTV signal routing cleanup, `cdtv_base` decode fix, NVRAM transport |
| #220 | CDTV memory card at `$E00000` |
| #221 | pulse SCOR at the CD frame rate, stop latching STEN on TPI line 4 |
| #222 | apply the CDDA volume DAC to the audio mixer |
| #223 | ingest the host sector stream as 16-bit words |
| #224 | route CDTV register accesses through the chip bus on TG68K |
| #225 | instantiate BRAM via `bram.vhd` instead of ad-hoc `altsyncram` |
| #226 | present read data captured BEFORE the read side effects fire |
| #227 | carry the full 32-bit DMA address (Z3 fast RAM aliases onto the vector table) |

plus Sorgelig's `2939638` "Reorganize source files" and `a875f57` cleanup inside
that line, and `d39b26f` "Update sys" (`hps_io.sv`, `ascal.vhd`, `yc_out.sv`)
and `bc4b31c` / `b265a3b` outside it.

`#227` is worth noting on its own: a DMA address truncated to less than 32 bits
aliasing Z3 fast RAM onto the vector table is the same shape as the restore bug
recorded in memory, so it is a fix we want rather than churn.

### Measured conflict scale

`git merge --no-commit --no-ff minimig-upstream/MiSTer` onto
`feature/misterfloppy`: 13 modify/modify conflicts, 23 add/add, 6 clean.

The add/add set is `2939638`'s source reorganization colliding with our tree —
mostly `rtl/sim/**` testbenches. The modify/modify set is the problem:
`Minimig.sv`, `Minimig.sdc`, `files.qip`, `hps_ext.v`, `rtl/akiko*.v`,
`rtl/cdtv*.v`, and then the deep core — `rtl/sdram_ctrl.v`, `rtl/chipdma_arb.v`,
`rtl/cpu_cache_new.v`, `rtl/cpu_wrapper.v`, `rtl/ddram_ctrl.v`,
`rtl/memory_router.v`, `rtl/gary.v`, `rtl/fastchip.v`,
`rtl/tg68k/TG68KdotC_Kernel.vhd`. Our branch is 180 commits ahead of upstream
and carries the save-state work and MiSTerFloppy, so these are all files we have
modified for reasons upstream knows nothing about.

Our core also still has `rtl/cdtv_hps_bridge.v`, which upstream removed when
CDTV moved onto the chip bus — that file's fate is a decision, not a conflict
git can express.

## `Anime0t4ku/Main_MiSTer_Physical_Disc`: `748eb1d` -> `de40622` — done

12 commits upstream, 5 of which touch the four files we track verbatim. Taken as
a re-extract on branch `sync/physdisc-de40622` (`e73f05e`), per the procedure in
`docs/upstream-pins.md`.

The changed functions are `ring_worker_main`, `refill_ring`, `fetch_sector`,
`physical_disc_seek_hint` and `physical_disc_load_toc` — "performance
improvements", "many improvements", "TOC Fix", "CD-DA swap fix", "PSX dual mode
disc fixes" and "mister hi-fi support". 1389 -> 2064 lines.

Checked before taking it:

- `physical_disc_disc_t` is unchanged, so `PHYSICAL_DISC_DISC_UNKNOWN` is still
  11, `PHYSICAL_DISC_DISC_NONE` still 0, and nothing hashed into
  `derive_toc_uuid()` moved. This is the check the pins doc asks for on every
  sync, and last time it had drifted.
- The two-lane ring design is intact: `lane_cursor`, `RING_SECTORS`,
  `SYNC_IO_BURST`, `subch_ok`, `leadout_lba` and `warmup` all still present.
- Public headers unchanged apart from one blank line. No signature changed,
  nothing removed.
- The only function dropped from the `.cpp`, `physical_disc_q_index00`, was
  file-static with no caller outside the module.
- No new includes, still no `cfg.*` reference, so the module stays
  self-contained.

`physical_disc_launch.cpp` stays excluded, which is where the remaining upstream
commit ("Only re-mount the physical disc on an actual core reset") lives.

**Needs a hardware pass on real discs.** The changed functions are the sector
delivery path and nothing here exercises it.

## Repos with nothing to take

- **kblood, both forks.** `amiga-console` is still `52625a4` and `amigacd-core`
  still `a27f4e8`, exactly the pins. All his new work sits on other branches and
  has reached us via the two upstreams instead.
- **`Menu_MiSTer`.** Upstream is still `3c3634c` (2026-06-03). Our fork's
  `master` is 5 ahead of it with the SNAC PSX work merged; the local checkout in
  `upstream/core-menu` is parked on `feature/snac-psx`, one commit below its own
  merge commit `e1db5f4`. Nothing upstream to merge.
- **RobSmithDev, both repos.** Byte-identical to the pins recorded in
  `docs/superpowers/plans/2026-08-02-misterfloppy-integration.md`. The
  MiSTerFloppy extraction is still current.
- **`theshaneobrien/mister-disc-drive-support`.** Still 144 commits ahead of
  `0f3a6ad` and last touched 2026-07-29, all of it release-workflow and overlay
  proof-of-concept work. Nothing that changes the "do not switch engines"
  recommendation.

## Recommended order

1. **Core first.** Merge `minimig-upstream/MiSTer` into a branch off
   `feature/misterfloppy`. Resolve the deep-core files against our save-state
   and MiSTerFloppy changes, decide `rtl/cdtv_hps_bridge.v`, then a Quartus fit
   — both setup and hold slack positive, per the seed lesson.
2. **Userspace second**, on `sync/mister-upstream-0a8fb44`, resolving the 61
   hunks with the core's new wire format as the reference for which side wins in
   `akiko_cd32.cpp` and `cdtv_cd.cpp`.
3. **Hardware together.** Neither half is testable alone: the sector wire format
   and the CDTV register path change on both sides at once.
4. **Then re-pin** `docs/upstream-pins.md` and re-verify the line numbers
   `docs/integration-plan.md` cites, as the header of that file asks.
