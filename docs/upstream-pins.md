# Upstream pins

Commits the analysis and the line numbers in `docs/integration-plan.md` were read
against. Re-verify line numbers after moving off these.

| Repo | Branch | Commit | Date |
|---|---|---|---|
| `kblood/Main_MiSTer-cd32-cdtv` | `amiga-console` | `52625a4` "AmigaCD: add Save settings to the console OSD" -- all branches re-checked 2026-10-09, nothing new to take | 2026-06-14 |
| `kblood/Minimig-AGA-cd32-cdtv-native-akiko` | `amigacd-core` | `a27f4e8` "AmigaCD: identify as a separate core" | 2026-07-29 |
| ^ **but we build `feature/misterfloppy`** — see "Which core branch to build" | | | 2026-08-03 |
| ^ previous pin, superseded 2026-08-01 | `amiga-console` | `c5eb180` | 2026-06-13 |
| `Anime0t4ku/Main_MiSTer_Physical_Disc` | `master` | `e6e62b2` "Merge PR #23, megacd-cdg-psx-fixes" -- module files unchanged, see below | 2026-10-16 |
| ^ previous pin, superseded 2026-10-16 | | `554dbbc` "Merge PR #20, megacd-cdg" | 2026-10-11 |
| ^ previous pin, superseded 2026-10-11 | | `dc0c4c9` "Merge PR #18, boot-launch" | 2026-10-09 |
| ^ previous pin, superseded 2026-10-09 | | `e6c7abd1` "Merge PR #17, stock LibCrypt" | 2026-10-01 |
| ^ previous pin, superseded 2026-10-01 | | `bcdb16f` "Fix for NGCD identification" | 2026-09-18 |
| ^ previous pin, superseded 2026-09-18 | | `f2963eb` "dvd fpga core support" | 2026-09-12 |
| ^ previous pin, superseded 2026-09-14 | | `de40622` "Merge PR #9, remount on savestate" | 2026-08-23 |
| ^ previous pin, superseded 2026-08-24 | | `748eb1d` "cd-da fixes" | 2026-08-03 |
| ^ `ab2f602` was recorded here on 2026-08-07 but never extracted — see below | | | |
| ^ its fork base (upstream `Main_MiSTer`) | | `7317947` | 2026-07-13 |
| `MiSTer-devel/Main_MiSTer` | `master` | `b4aac02` "input: force feedback requested by the core (#1393)" -- **both taken, the Action Replay adapted to status[63]; see "Sync 2026-10-29"** | 2026-10-29 |
| ^ previous pin, superseded 2026-10-29 | | `bfb5cd0` "Minimig: CD read speed for the CD32 and CDTV drive" -- **seven of thirteen taken (two adapted); the CD read speed setting NOT taken, its status bits are our save state's; see "Sync 2026-10-28"** | 2026-10-28 |
| ^ previous merge base, superseded 2026-10-28 | | `8328d9f` "Minimig: CD32 with up to 99 tracks, WAV tracks in cue (#1386)" -- taken, and the Akiko TOC bound follows it; SGI Indy not taken; see "Sync 2026-10-26" | 2026-10-26 |
| ^ previous merge base, superseded 2026-10-26 | | `e698ccd` "Minimig: let the Akiko TOC carry all 99 tracks (#1384)" -- two of four taken (the 68EC030 adapted), two already here; see "Sync 2026-10-24" | 2026-10-24 |
| ^ previous merge base, superseded 2026-10-24 | | `0c98d24` "Minimig: draw FDD Turbo at its own line in System (#1379)" -- upstream rewrote its history; content-merged from `d3b8537`, see "Sync 2026-10-22" | 2026-10-22 |
| ^ previous merge base, superseded 2026-10-22 (**no longer on upstream's master**) | | `d3b8537` "Adds PCFX system support (#1365)" -- three of four taken (one adapted), see "Sync 2026-10-17" | 2026-10-17 |
| ^ previous merge base, superseded 2026-10-17 | | `e836aea` "Minimig: 68030 with its clocks..." -- six of seven taken (two adapted), see "Sync 2026-10-16" | 2026-10-16 |
| ^ previous merge base, superseded 2026-10-16 | | `738cb7a` "Release 20261010." | 2026-10-11 |
| ^ previous merge base, superseded 2026-10-11 | | `42e6400` "input: Add X-Gunner lightgun support (#1348)" | 2026-10-09 |
| ^ previous merge base, superseded 2026-10-09 | | `57276f0c` "Change user_io_status_set value in video.cpp (#1335)" | 2026-10-01 |
| ^ previous merge base, superseded 2026-10-01 | | `5b3ae64` "N64: Improved CIC detection (#1318)" | 2026-09-18 |
| ^ previous merge base, superseded 2026-09-18 | | `4145f1d` "Rotate analog OSD to match core direction (#1316)" | 2026-09-17 |
| ^ previous merge base, superseded 2026-09-17 | | `0039110` "Apple-II: WOZ support, save states, screenshots (#1312)" | 2026-09-12 |
| ^ previous merge base | | `0a8fb44` "minimig: resolve the quick-start ROMs through HomeDir() (#1292)" | 2026-08-24 |
| `MiSTer-devel/Minimig-AGA_MiSTer` | `MiSTer` | `bf5d061` "The Action Replay cartridge (AR5, DeMoN v2 layout) as an option to HRTmon (#291)" -- **taken, adapted to status[63]; its Verilator system sim not; see "Sync 2026-10-29"** -- FringeCoder/AmigaCD_MiSTer `claude/resync-upstream-70pn9b` | 2026-10-29 |
| ^ previous pin, superseded 2026-10-29 | | `06db70b` "CD read speed; fix CPU (Pierre le Chef) and Denise (Alien 3)" -- **all taken, three adapted (`ce1f386` as the core half of our own CD rate), see "Sync 2026-10-28"** -- FringeCoder/AmigaCD_MiSTer `claude/resync-upstream-70pn9b` | 2026-10-28 |
| ^ previous pin, superseded 2026-10-28 | | `ba5adba` "Improve the timings (SDRAM pins, CPU data path) - #289" -- **all but `cf24493` taken, see "Sync 2026-10-26"** -- FringeCoder/AmigaCD_MiSTer `claude/resync-upstream-1026` | 2026-10-26 |
| ^ previous pin, superseded 2026-10-26 | | `b988bd09` "Fix Denise (black stripes on Flink AGA), fix 68EC020 freeze" -- all but `4ab0d5f` taken, see "Sync 2026-10-24" -- FringeCoder/AmigaCD_MiSTer#62, merged into `cd32-native-mvp` as `b8079fce`; `430209e` and the real-floppy line still out | 2026-10-24 |
| ^ previous pin, superseded 2026-10-24 (**reverted upstream by #284**) | | `5b93485b` "Fix RTG with the 68030 MMU enabled (#281)" -- upstream rewrote its history; content-merged from `0bf2ab1`, see "Sync 2026-10-22" | 2026-10-22 |
| ^ previous pin, superseded 2026-10-22 (**no longer on upstream's MiSTer**) | | `0bf2ab1` "68020/68030 fixes and the 68030 MMU bus error - #268" -- all ten commits since `26eb46a` taken, see "Sync 2026-10-17" -- FringeCoder/AmigaCD_MiSTer#58 | 2026-10-17 |
| ^ previous pin, superseded 2026-10-17 | | `26eb46a` -- taken through `26eb46a` (68030 hand-ported in its own PR) -- FringeCoder/AmigaCD_MiSTer#56/#57 | 2026-10-16 |
| ^ previous pin, superseded 2026-10-16 | | `c5ffbf9` "Fix Absolute Inebriation" -- partial take, floppy line still declined, see "Sync 2026-10-11" | 2026-10-11 |
| ^ previous pin, superseded 2026-10-11 | | `da7632e` "Add OS3-style Mountlist (#244)" | 2026-09-10 |
| ^ **upstream has moved to `c1134ac` "Add real floppy drive support (#245)"** -- NOT taken, see below | | | 2026-09-17 |
| ^ previous pin, superseded 2026-09-14 | | `809b955` "A2065: emulate the Commodore A2065 ZorroII Ethernet card" | 2026-07-26 |
| ^ our core's fork point | | `3ab91cd` "Release 20260220." | 2026-02-20 |

`52625a4` is merged into this repo's `master` as the working tree.

**A newer userspace line exists and we are not on it.** `origin/amigacd-userspace`
is the rebased successor to `amiga-console`, the same move we already made on the
core side. It is a content superset, and it contains `7317947` — the
physical-disc fork's own base, which our line predates by four months. See
[userspace-branch-move.md](userspace-branch-move.md) for the gained/lost
analysis and why moving first makes the physical-disc merge much smaller.

## Main_MiSTer `5b3ae64` is a PARTIAL take, and the reason is an on-disk format

`3d7a844` "Minimig: Add external floppy drive, SCP and IPF support (#1315)"
landed upstream on 2026-09-18, from RobSmithDev -- the userspace half of the
same feature the core repo's T26 covers. **It is not merged, and it must not be,
because upstream's config struct and ours put the external drives in different
places:**

| | |
|---|---|
| ours | `extDrives[4]` inside `mm_floppyTYPE`, i.e. inside `floppy` |
| upstream | a separate `mm_floppyexTYPE externalfloppy { exDrives[4] }` member |

That changes `sizeof(mm_configTYPE)` and the offset of **every field after
`floppy`** -- which on this fork means `cd_speed`, the SNAC port modes and the
hardfiles. `minimig_cfg_load()` dispatches on exact file size and carries two
migration rungs (pre-ext-floppy and pre-SNAC); taking upstream's layout
invalidates all of it and silently misreads every config already on a user's
card.

So this is a permanent divergence in an on-disk format, not a merge that can be
deferred until someone has time. Whoever revisits it is choosing between
rewriting our layout plus a third migration rung, or staying where we are.
**Staying is the recommendation** until there is a reason: ours works, it is
migrated, and the feature is the same feature.

### What WAS taken from 5b3ae64

- `support/minimig/scpfile.{cpp,h}`, `ipffile.{cpp,h}`, `capsapi/` -- upstream's
  copies wholesale. They contain no reference to `minimig_config` at all, so the
  struct divergence does not reach them. Two real gains:
  - **two extra `dlopen` fallbacks for `libcapsimg.so`** (`/media/fat/` and
    `/media/fat/linux/CAPSIMG/`). `caps_init()` is what decides whether the
    browser offers IPF at all, so a user with the library in either of those
    places previously got **no IPF support, silently**;
  - the `// RobSmithDev` attribution and the CAPSAPI licence note, which
    upstream added after we vendored these files and which should travel with
    third-party code.
- `support/n64/n64.cpp` (#1318 CIC detection) and `support/a2/*` (#1317) --
  cores we do not build, taken wholesale to stay close to upstream at no cost.

`minimig_fdd.{cpp,h}` and `capsapi/CapsAPI.h` were already **byte-identical** to
upstream's, so #1315 brought nothing there.

### What was NOT taken

Upstream's `menu.cpp` floppy rows, `minimig_config.{cpp,h}` struct and
`minimig_ConfigUserPort()`. We have `minimig_ConfigSnac()` doing that job plus
the SNAC third tenant, and our OSD rows already exist.

**Our one standing patch to a taken file:** `scpfile.cpp` uses
`std::max<int64_t>(1, ...)` where upstream has `std::max(1LL, ...)`. `int64_t` is
`long` on an LP64 host and `long long` on 32-bit ARM, so upstream's form resolves
only on the target and makes the file unparseable by `syntax_check.sh`. It needs
re-applying at every sync that touches this file.

## Sync 2026-10-29: force feedback, and the Action Replay on a free status bit

Main_MiSTer `bfb5cd0..b4aac02`, Minimig-AGA `06db70b..bf5d061`, and
Template_MiSTer `sys/` `5dda373..76dd8f5` (into both the core and
FringeCoder/Menu_MiSTer).

| Upstream | Change | Here |
|---|---|---|
| `b4aac02` (#1393) | input: force feedback a core asks for through hps_io (`UIO_GET_FFB`, 0x45) | Taken, clean. Main probes each core with 0x45 at start-up and stops asking one that does not answer 0xFB. Nothing in the AmigaCD core's `hps_ext.v` or `hps_io.sv` used 0x45. |
| Template `76dd8f5` (#122) | hps_io: the force feedback request, `FFB` parameter | Taken into `sys/hps_io.sv` of the core and of Menu_MiSTer, SNAC lines kept. `FFB` defaults to 0, so 0x45 answers 0 on both. |
| `b57370d` (#1397) | Minimig: "Cartridge: Off / HRTmon / Action Replay" (AR5.ROM, DeMoN v2), ext_cfg bit 23 | Taken, **adapted**. Upstream's core reads it at `status[55]`, this core's screen test pattern, and our `minimig_status_compose()` drops ext_cfg bits above 18 anyway. The bit stays where upstream keeps it (config, MGL, menu); compose sends it to **`status[63]`**, the last status bit free on both sides. Menu row at this fork's menusub 10. |
| `bf5d061` (#291) | The Action Replay cartridge in `cart.v`, its fetch detect in `cpu_wrapper`, no Kickstart mirror at $A80000 with it fitted | Taken, **adapted**: wired to `status[63]` in `AmigaCD.sv`; our comments and the CDTV ports kept in the three conflicted files; `chipdma_arb`'s own `memory_router` ties `ar_on` off. **Not taken:** `rtl/sim/system`, a Verilator system sim built around upstream's `Minimig.sv`, not our top. So the Action Replay is **not exercised by any bench here** and is unverified until hardware. |

**Status bits are now all spoken for** on the AmigaCD core: [50:32] ext_cfg,
[51] save, [53:52] slot, [54] restore, [55] test pattern, [58:56] A2065
(Main-side only), [60:59] drive sound volume, [62:61] CD sector rate, [63]
cartridge type. The next upstream feature that wants a status bit needs a
decision, not a free slot.

**Save states and the cartridge.** Neither HRTmon's nor the Action Replay's
registers or RAM ($A80000-$AFFFFF) are in the save state. Not new -- HRTmon has
always been outside it -- but a state saved with the Action Replay frozen will
not restore into a frozen Action Replay.

The physical-disc fork (`e6e62b2`), `kblood/Main_MiSTer-cd32-cdtv` (`52625a4`)
and Menu_MiSTer (`fa83dc1`) have not moved.

## Sync 2026-10-28: the CD fixes, the core with them, and a status-bit collision

Main_MiSTer `8328d9f..bfb5cd0`, cherry-picked (`-x`) onto
`claude/resync-upstream-70pn9b`.

| Upstream | Change | Here |
|---|---|---|
| `89f4313` (#1387) | CD track boundaries: a track's end LBA is exclusive, and a read outside every track returns zeros instead of dereferencing NULL | Taken, clean. `get_track_from_lba()` is static to `ide_cdrom.cpp`. |
| `86b40e9` (#1391) | HORI FFB Racing Wheel as a wheel | Taken, clean. |
| `02fbd26` (#1392) | MRA files mount images the way MGL does (`<file>` only) | Taken; one conflict in `mgl_parse()`, resolved by keeping both lines -- our `xml_path` capture (the `<minimig>` MGL config reads it) and upstream's `mgl_from_mra`. |
| `52e81c9` (#1394) | Lingering ROM load screen | Taken, clean. |
| `2bc93a9` (#1395) | Early SPD infoframe | Taken, clean. |
| `edfe8d7` | CD32: the sector counter read in 16 bits, so a read past 256 sectors in one ENABLE period no longer serves its start again | Taken, **adapted** by hand (`akiko_cd32.cpp` is ours almost throughout): the read and both callers that hold the value. Safe against our core as it is: `fastchip.v:143` pads the bridge's 8-bit output with a zero high byte, so it reads as before until the core takes Minimig-AGA `3192739`. |
| `1f55555` | CDTV: a data READ stops the CD audio (one pickup, as WinUAE) | Taken, **adapted**: upstream's sits on `a97efe2`'s paced read pump; here the same stop goes where the READ is armed (our own pump since, see the next rows). |
| `4f2da7a`, `a97efe2`, `0437012` | "CD read speed" 1x/2x/4x for CD32 and CDTV; CD32 seek delay; CDTV read paced from the poll; CDTV seek takes 500 ms | **Taken as a mechanism, not as written.** As written the setting is extcfg bits 21-22 = `status[54:53]`, which on our core are the save state's restore request and slot (`AmigaCD.sv:1947-1953`): changing the speed would have fired a restore. Instead: our own `cd_speed` (Fast / CD32 2x / Drive) stays the setting and the on-disk format; the Main sends the rate the core should pace CD32 sectors at on `status[62:61]` (`akiko_send_rate()`, codes in `akiko_pace.h`: 0 = 8x so an older Main is not slowed down, 1/2/3 = 1x/2x/4x) and no longer sleeps in the main loop; Drive sends what the guest commanded. The CDTV READ is a pump in the poll (from `a97efe2`), at 1x in Drive (the CR-511 is 1x), 2x in CD32 2x, and the old 1 ms gap in Fast; the CDTV SEEK takes 500 ms and raises a status change in Drive mode only (`0437012`). **Decided 2026-10-28 (owner): CDTV stays authentic in Drive mode** -- 1x, about 3x slower to load than the old unpaced loop; Fast is the setting for the old speed. Our CD32 seek model (distance based) stays; upstream's WinUAE tick formula (`4f2da7a`) is not taken. |
| `a9da97c` (#1389) | Bandai Pippin | **Not taken**: sits on the Mac fast-SCSI work (`6ddadac`, MacPPC7300) this fork never took, like `79787d3`. |
| `1270309` (#1378) | PC-8801: one disk of a multi-disk D88 | **Not taken**: `support/pc88` is not here. Its `user_io_file_mount()` signature change goes with it. |
| `bfb5cd0` | Merge of the five Minimig commits above | Nothing of its own. |

**The core sync**, Minimig-AGA `ba5adba..06db70b`, applied three-way
(FringeCoder/AmigaCD_MiSTer `claude/resync-upstream-70pn9b`):

| Upstream | Change | Core |
|---|---|---|
| `1abd1fa` | Akiko writes the PBX status long last, byte 2 last of all: the Core Design loader (Chuck Rock, Premiere) polls a slot and takes it when byte 2's $8000 mark goes, not on the interrupt | Taken, **adapted** by hand into this fork's PBX engine. And extended: a stale ship (one the next READ overtook, `pbx_ship_invalid`) now writes no status at all, so a polling loader cannot take the previous read's sector from it either -- the same reasoning as the existing rule that it raises no interrupt. `tb_akiko_pbx_dma` gained a write-order check on every slot and a stale-ship status check; both falsified. |
| `3192739` | The sector counter to the Main in 16 bits | Taken; `akiko.v` by hand. **The save state keeps 8 bits** (`SS_O_SECCNT`): widening it would move every field after it. A state saved past sector 255 of one ENABLE period restores the counter mod 256, as before. Pairs with `edfe8d7` above. Bench check added (the counter past 255 reaches `hps_sec_status`), falsified. |
| `f1af4fe` | cpu_wrapper: TG68K's skipped CLR/Scc read stays off the chip bus (CLR.W INTREQ set every request; Pierre le Chef) | Taken; conflicts around this fork's save state park and the CDTV bridge gate. `skip_rd` also excludes `cdtv_selack`, as upstream excludes `fastchip_selack`. **Not covered by a bench**: the cpuwrap bench's TG68K stub ties `skipfetch` low. |
| `c1f14c3` | Denise keeps BPLAM to the last pixel of the window (Alien 3) | Taken, clean. Not covered by a bench. |
| `ce1f386` | Akiko paces sectors at the CD read speed from `status[54:53]` | **Taken, adapted** (separate commit after the sync): at most one PBX ship starts per period, the period running from each start (upstream's runs free); the rate comes from `status[62:61]`, sent by the Main, not snooped from the READ command into a register the save state does not carry; 382500 clocks at 1x for our 28.6875 MHz clk_sys (upstream's 378334 is for 28.375 MHz). `tb_akiko_pbx_dma` Test J checks the spacing at all four rates; falsified four ways. |
| `06db70b` | Merge | Nothing of its own. |

The physical-disc fork (`e6e62b2`) and `kblood/Main_MiSTer-cd32-cdtv`
(`52625a4`) have not moved. Menu_MiSTer is already at upstream `fa83dc1` and
Template_MiSTer `sys/` at `5dda373`: nothing to take there.

## Sync 2026-10-26: the 99-track table, and the core's timing work

Main_MiSTer `e698ccd..8328d9f`, a fast-forward.

| Upstream | Change | Here |
|---|---|---|
| `8328d9f` (#1386) | CD32: `drive_t.track[]` 50 -> 100, so a cue or CHD holds up to 99 tracks; WAV tracks in a cue read from their audio data, not their RIFF header | Taken, clean `cherry-pick -x`. **And the Akiko TOC bound moves with it:** `AKIKO_TOC_MAX_POINTS` was 52 because the table held 49 real tracks (see "Sync 2026-10-24" on `e698ccd`); with 99 it would have truncated a disc of 50+ tracks and wedged the BIOS at the spinning-CD splash, the bug this bound has had twice before. It is 102 now, upstream's figure, with a `static_assert` that ties it to `sizeof(drive_t::track)`, so the next resize fails the build instead. Every other track limit in the tree is already derived from the struct. |
| `eddc655` (#1385) | SGI Indy: Ethernet and SCSI write buffer | **Not taken**: a core this fork does not build (`support/sgi` is not here), like HP 9000 and PCFX before it. |

Minimig-AGA `b988bd09..ba5adba`, applied three-way (FringeCoder/AmigaCD_MiSTer
`claude/resync-upstream-1026`, `6450b9a`):

| Upstream | Change | Core |
|---|---|---|
| `8813609` (#287) | Blitter: a CPU write to a blitter register waits while a blit runs with BLTPRI set (Deep Core's CD32 background); ddram_ctrl's write queue and read-ahead restructured for timing | Taken. `agnus` keeps this fork's `blit_busy` output (save state quiesce) beside upstream's new `blit_nasty`. |
| `5306e18` (#289) | sdram_ctrl plans the SDRAM pin commands a clock ahead | Taken, **adapted**: this fork's `cacheAddr` (the read cache's address a clock late, which keeps the save state / CPU address mux out of the clk_114 into `sd_addr`) stays, and upstream's new row mux reads it where upstream reads `cpuAddr`. `slot_is_dma` stays in the chip slot. `make_sim_sdram.py` expects one procedural `sd_data` driver now, not three. |
| `a189426` (#289) | TG68K: the memory word is taken last in the long chains | Taken; the MMU's `ss_flush` port stays. |
| `cf24493` (#289) | cpu_wrapper picks the SDRAM word, the DDR3 word or the swapped DDR3 word with one choice | **Not taken.** Here the read return already goes through `cpu_rdgate`, a registered stage between the SDRAM/DDR3 mux and the CPU, so the path upstream shortens is already cut; taking it would route around the gate. **Standing divergence.** |

The physical-disc fork (`e6e62b2`) and `kblood/Main_MiSTer-cd32-cdtv`
(`52625a4`) have not moved.

## Sync 2026-10-24: two of four taken, and the core follows with the 68EC030

Main_MiSTer `0c98d24..e698ccd`, a fast-forward this time (`0c98d24` is still on
upstream's `master`).

| Upstream | Change | Here |
|---|---|---|
| `23b4a18` (#1383) | Apple-II: write sync bytes when converting 5.25" images to WOZ | Taken, clean `cherry-pick -x`: `support/a2/iigs_fmt.cpp` was unchanged here since the pin. |
| `8565774` (#1382) | Keep a saved 68020 (0x03) as a 68020 instead of loading it as the 68030 Fast | **Already here.** This fork never folded 0x03 -- it is CPU Turbo On, the standing divergence of the 2026-10-16, -17 and -22 syncs. Upstream has now come round to the same behaviour, so that divergence is gone. |
| `e698ccd` (#1384) | Akiko TOC buffer 32 -> 102 points, for discs over 29 tracks (Fightin' Spirit, 34) | **Already fixed here**, at 52 (`support/minimig/akiko_cd32.cpp:541`), since Super Street Fighter II Turbo hit the same wedge on 2026-09-03. 52 is the structural ceiling: `drive_t.track[]` is `track_t[50]` in `ide.h`, here and upstream, so no loadable disc has more than 49 real tracks. 102 would only size for tracks the CD pipeline can never deliver. Not taken. |
| `065d0d2` (#1381) | 68EC030: CPU byte bit 5 on the 68030 (0x22, and 0x62/0xa2 with a clock), and a saved 0x22 loads as 68EC030 | **Taken, adapted** (decided 2026-10-24, to keep current). The System page's CPU row steps 68000 / 68010 / 68020 / **68EC030** / 68030; the clock stays on the CPU Clock row, which is live for both 68030s and keeps bit 5. The AmigaCD Settings CPU row takes upstream's last seven steps as they are: 68020, 68EC030 40MHz / 50MHz / Fast, 68030 40MHz / 50MHz / Fast. `minimig_fold_removed()` no longer folds 0x22 to the 68030 Fast and keeps the clock on 0x62/0xa2, as upstream's load now does, so a config saved with the old "68030 no MMU" opens as the 68EC030. **The core runs it** since FringeCoder/AmigaCD_MiSTer `763e6db` (Minimig-AGA #283, below), which also holds TC.E off on a save state restore into a 68EC030 -- the restore path is this fork's, so upstream's PMOVE mask does not reach it. Merged into `cd32-native-mvp` as `b8079fce` (#62); a core released before that runs a 68EC030 picked here as the full 68030 with its MMU. |

Minimig-AGA `MiSTer` moved past the `5b93485b` pin, to `b988bd09`, and the core
followed the same day (FringeCoder/AmigaCD_MiSTer `claude/resync-upstream-xdi173`,
`763e6db` and `3f8e4bf`). Applied three-way: upstream moved linearly this time.

| Upstream | Change | Core |
|---|---|---|
| `9194cef` (#283) | The 68EC030: `ec030` in `cpu_wrapper.v`, TC.E held off and the MMU table walk skipped in the kernel | Taken, plus the save state restore's TC load masked the same way, with a TG68K bench phase (PHASE 9) and a third CI kernel run; falsified both masks. |
| `9efd296` | CPU cache: a read stops when the CPU leaves the cache (NetBSD MMU fault) | Taken. |
| `de795a7` | `sdram_ctrl` raises `write_req` only once the cache takes the write (68EC020 freeze without Fast RAM) | Taken. `tb_sdram_fwd` had relied on the old behaviour and posted nothing; it now posts as the CPU does (`3f8e4bf`). |
| `4767064` (#284) | Revert of `5b93485b` (#281), the RTG driver change for the MMU | Taken. |
| `4ab0d5f` | Flink AGA sprite stripes: delay the sprite's shift along with its load | **Not taken.** The core's `f9399d6` fixes the same stripes by delaying the serial output, A/B tested on hardware 2026-10-18; both together would move the sprite twice. **Standing divergence.** |

All 65 steps of the core's `rtl-sim.yml` pass locally on the result. Not yet fitted.

The physical-disc fork (`e6e62b2`) and `kblood/Main_MiSTer-cd32-cdtv`
(`52625a4`) have not moved.

## Sync 2026-10-22: upstream rewrote both histories

Between 2026-10-19 and 2026-10-22 both upstreams replaced the commits this fork
had pinned. Minimig-AGA's `MiSTer` no longer contains `26eb46a..0bf2ab1`: the
same work was re-landed as squashed PRs #275-#281 on top of `cbf17d2`, with
real changes. Main_MiSTer's `master` no longer contains `e836aea`, `60a868f` or
`d3b8537`: re-landed as #1363-#1379. Nothing can be cherry-picked across that,
so each side was merged by CONTENT: a three-way merge per file with the old pin
as base, upstream's new tip as theirs and this fork as ours. The old pins stay
in the table above, marked as gone, because the 2026-10-16 and 2026-10-17
notes below cite them.

### What upstream changed, and what this fork did with it

| Upstream | Change | Here |
|---|---|---|
| ec1353dc (#277) / d49cd64 (#1376) | The 68030 always has its MMU (the "68030 no MMU" of 60a868f is gone) and runs at 40MHz, 50MHz or Fast; CPU byte bits 7:6, cpucfg[4:3] | Taken. This fork has no line to spare for upstream's extra CPU steps, so the System page's MMU row becomes **CPU Clock: 40MHz / 50MHz / Fast** (greyed but for the 68030, like CPU Turbo for the 68020), and the AmigaCD Settings CPU row steps 68020 / 68030 40MHz / 68030 50MHz / 68030 Fast. A config or .mgl holding the old 0x22 opens as the 68030 Fast; stray clock bits are cleared (`minimig_fold_removed()`). |
| ec1353dc (#277) | The SDRAM-backed Zorro III Fast RAM of 26eb46a is gone: the old SDRAM controller is back, `sdram_big`/`sdram_dual`, the `z3sdr` windows and ddram_ctrl's SDRAM proxy and prefetch with it | Taken in the core. Host side: nothing to do -- the SDRAM size is still sent to every core by the generic hps_io path; this core just stops reading it. |
| c55ecd91 (#276) / 8f79cce (#1375) | The 68EC020 is cycle exact at A1200 speed | Taken. The **unthrottled 68020 (CPU Turbo On) stays**: upstream's load sends it to the 68030 Fast, this fork keeps it as its CD32/A1200 default, as at the 2026-10-16 and 2026-10-17 syncs. **Standing divergence.** |
| 6c66eb1d (#275) / 781a5f4 (#1374) | Drive sounds | Already here since 26eb46a/e836aea, at status[60:59]; nothing new to take. |
| 0c98d24 (#1379) | FDD Turbo on its own line in System | Already so here (row 11). |
| 2ebd9f5, b988ec9, 1288361 (#1370-#1372) / c9502bae (#274) | A2065: RINT re-raise, stale acknowledge, transmit ring polling; the core skips no-op CSR0 writes | Taken, both halves. |
| e8e915a, 8bc47c0 (#1369) | MGL `option` items; Atari ST MGL items | Taken (mra_loader.h keeps this fork's `xml_path` beside the new option table). |
| bb73090 (#1367), 63186a3 (#1377) | PCAT-286 on the x86 support; VRR infoframes on the menu core | Taken -- generic, and on support this fork carries. |
| 144cee3 (#1368) | HP 9000 family | **Not taken**: a core this fork does not build, like PCFX, FM Towns Marty, Sun, Falcon and PC-88 before it. Every hunk that names `hp9k` was dropped from the merge. |

## Sync 2026-10-17: the 68030 simplification, and upstream's 68020/68030 fixes

Upstream's reviewers asked for the 68030 to be cut back (Minimig-AGA #265,
Main_MiSTer #1360), and both halves landed: the 68030 runs at its maximum clock
only, a **68030 no MMU** variant is added (CPU byte bit 5, cpucfg[2] on the
68030), and the 64M/128M Zorro III boards are gone. This fork follows it, as
decided on 2026-10-17. The core side is FringeCoder/AmigaCD_MiSTer#58, on top
of the timing work there.

### Main_MiSTer `e836aea..d3b8537`: three of four taken

- `601b90e` screenshot_info setting, `7b0073b` KEY_RO mapping -- clean
  `cherry-pick -x`.
- **`60a868f` "Minimig: 68030 at its maximum clock only, 68030 no MMU, no
  64M/128M Fast RAM"** -- adapted. The System page has no free line, so the
  CPU Clock row becomes **CPU MMU: On/Off** (live on the 68030 only, like CPU
  Turbo on the 68020) instead of a fifth CPU step, and every row below keeps
  its index; the AmigaCD Settings CPU row steps 68020 / 68030 / 68030 no MMU.
  FastRAM follows upstream's list. The 64M/128M -> 256M load migration is
  taken; the migration of the unthrottled 68020 (now to "68030 no MMU") stays
  declined, as `e836aea`'s did -- it is this fork's CPU Turbo On default.
- Not taken: `d3b8537` "Adds PCFX system support" -- a core we do not build,
  and it sits on `ce432dc` FM Towns Marty, which was declined for the same
  reason (see the 2026-10-01 sync).

### Minimig-AGA `26eb46a..0bf2ab1`: all ten taken

`a5f1637` (the core half of the above), and the TG68K and cpu_wrapper fixes:
MOVES, the M bit of the status register, CHK/CHK2/CMP2/DIVU/DIVS flags, T0
trace on change of flow, MOVEC CACR on the 68020, the chip long write when the
low word does not follow, address error on an odd PC and the RTE format
error, MOVES in user mode, and the 68030 MMU bus error with RTE continuation.
Adaptations: `AmigaCD.sv` takes the 3-bit cpucfg; the save state path writes
the new `a7_live` with A7 and restores the M bit's shadow (`preMmode`) with
SR -- MSP/ISP are not in the vector, so a state saved with M set is wrong,
which AmigaOS never does; save states stay refused on the 68030, with or
without its MMU.

## Sync 2026-10-16: six of seven taken, two of them adapted, one not taken

Seven commits landed on `MiSTer-devel/Main_MiSTer` between `738cb7a` and
`e836aea`. Four are taken as `cherry-pick -x`, two are adapted; `syntax_check.sh`
and `tests/` pass, and the ARM build is CI's. The core side of the same sync is
FringeCoder/AmigaCD_MiSTer#56.

### Taken

- **`00bdff0` "minimig: break the A2065/IDE deadlock (#1357)"** -- the host
  half of a pair. `ide_wait_req()` services the A2065 mailbox while an IDE
  transfer waits, and `INT_STATE` publishes the CMD ring read index. The core
  half, `d241338` "A2065: apply CSR0 acknowledges in the mailbox; poll
  INT_STATE before CSR_SHADOW (#256)", goes into FringeCoder/AmigaCD_MiSTer in
  the same sync: one without the other is a protocol half-change.
- `2ec7c0b` "Remove the legacy Sharp MZ support (#1353)" -- one conflict in
  `user_io.h`, upstream context only (`is_falcon()`, which this fork does not
  have). Resolved by dropping `is_sharpmz()` as the commit intends.
- `2dd3662` "menu: show mounted file for S option" and `146a6df` "Saturn:
  detect RAM cartridge via boot metadata (#1355)" -- generic, clean.

### Adapted: `c3dc8ed` "Minimig: keep the extended config when a core option is set (#1352)"

Taken as-is it would break save states. It makes `user_io_status_set()` send
`minimig_get_extcfg()` in all of status words 2-3 instead of `cur_status`.
Upstream's core reads all of status[63:32] as the extended config; this core
does not: status[51] is the save request, [53:52] the slot, [54] the restore
request and [55] the test pattern (`AmigaCD.sv`), raised by the menu through
`user_io_status_set()`. So here only the extended config's own bits,
`MINIMIG_EXTCFG_STATUS_MASK` (status[50:32]), come from it, and status[63:51]
from `cur_status`. **A standing divergence**: re-apply at every sync that
touches `user_io_status_set()`.

### Found while taking it: the A2065 option sat on the save-state bits

`A2065_STATUS_OPT` was upstream's `[54:52]` (#1247, 2026-07-25), and this
fork's save states took status[51..55] two weeks later. The core never reads
the A2065 mode from status -- `a2065_ena` is `cpu_wrapper.v`'s -- so it is host
scratch, but in [54:52] selecting tap0 (mode 4) held the restore request high
and modes 1-3 moved the save slot, at every config load. Moved to `[58:56]`,
unused by the core. The mode is persisted in `mm_configTYPE`, so nothing on a
card moves. **A standing patch**, in `minimig_a2065.h`.

### Paired with the core: the Akiko unknown-opcode length

Not a Main_MiSTer commit, but the host half of Minimig-AGA `cbf17d2` (#263),
taken in the core sync: `akiko_drain_command()` drains one byte for an opcode
the drive does not define (0xB-0xF), where it used to drain the full 32-byte
buffer to match the core's old "frame on buffer full". **Both halves must ship
together** -- the comment there says what each mismatch does.

### Adapted (in its own PR, 2026-10-16): `e836aea` "Minimig: 68030 with its clocks, 68EC020, Fast RAM up to 384MB, drive sounds (#1360)"

The host half of Minimig-AGA `26eb46a`, which goes into FringeCoder/AmigaCD_MiSTer
in the paired PR; the two move together. Hand-ported (the conflicts were too
wide for `cherry-pick -x` to record it honestly; the commit names `e836aea`)
and adapted in four places, each **a standing divergence**:

- **The unthrottled 68020 stays.** Upstream drops it from the CPU row and, on
  load, moves `cpu & 0x23 == 0x03` to the 68030 at 56.7MHz. Here that value is
  CPU Turbo On, the CD32 and A1200 default since `4b4f604`, and the core still
  runs it (cpucfg 011). The migration is not taken: it would have made every
  CD32 on a card a 68030 on a load nobody asked to change anything. The System
  page's CPU row cycles 68000/68010/68020/68030 and leaves bit 5 to the Turbo
  row on the 68020; the 68030's clock gets the line D-Cache had (the core no
  longer reads that bit). The AmigaCD Settings page, which has no CPU row,
  gets one in D-Cache's place too: 68020 or 68030 at 25-56.7MHz, for the
  68020 machines.
- **The drive sound volume moves to status[60:59].** Upstream's core reads it
  at status[52:51] -- ext bits 20:19 -- which here are the save state request
  and slot. `minimig_status_compose()` (`minimig_status.h`) puts it at bit 27
  of status[63:32], and `tests/test_minimig_status.cpp` pins every bit.
- **`minimig_set_extcfg()` no longer writes status[63:32] itself.** Upstream
  sends 0 and then the raw extended config; with the volume in bit 19 picking
  "Quiet" would have requested a save state. It goes through
  `user_io_status_resend()`, the same path as every other status write.
- **`cd32_active()` and the save state guard know the 68030.** A CD32 with a
  68030 is still a CD32 (`cpu & 2`, was `== 3`), and save states are refused
  on it as the core refuses them -- the state vector has no room for the MMU.
  [Since 2026-10-19 the 68030 is allowed, with or without its MMU: the core
  carries the rest of its CACR and, from format 1.5, the MMU registers. Older
  files are upgraded on load (ss_upgrade_14_to_15).]

`drivesounds.bin` is looked for in `games/Amiga` (HomeDir() for the AmigaCD
core too); the core's release package carries it there.

### Not taken
- `13f1de9` "PC-8801: let a floppy image grow (#1358)" -- a core we do not
  build, and it sits on the PC-8801, Mac, Sun, Falcon and A3 SD service hooks,
  none of which were taken. Same reasoning as `79787d3` below.

### `Anime0t4ku/Main_MiSTer_Physical_Disc` -> `e6e62b2`, no re-extract

Two commits (PR #23): Mega CD subcode alignment and a PSX region-change
reset, in `support/megacd/` and `support/psx/`. None of the extracted
`support/physical_disc/` files changed, so the pin moves and nothing on disk
does.

`kblood/Main_MiSTer-cd32-cdtv` `amiga-console`: nothing new since `52625a4`.

## Sync 2026-10-11: one of three taken, and that one only in part

Three commits landed on `MiSTer-devel/Main_MiSTer` between `42e6400` and
`738cb7a`.

- **`c265709` "menu: adjust terminal OSD message"** -- taken in part, as
  `fce5435`. The `vga_nag()` hunk (two fewer blank rows, the "or" lines
  re-indented) applies cleanly. The rest of the commit is whitespace in
  upstream's MiSTer Floppy menu, which reads `minimig_config.externalfloppy`;
  this fork keeps external drives inside `floppy` (see "Main_MiSTer `5b3ae64`
  is a PARTIAL take"), so that code does not exist here and the hunks have
  nothing to land on.
- `79787d3` "mac: add MacPPC7300 support (#1350)" -- not taken. A core we do
  not build, and it sits on `6ddadac` mac fast SCSI, also not taken.
- `738cb7a` "Release 20261010." -- a prebuilt binary in `releases/`; not taken.

### `Anime0t4ku/Main_MiSTer_Physical_Disc` -> `554dbbc`, no re-extract

Ten commits (PR #20): Mega CD CD+G subcode passthrough and a subcode log, in
`support/megacd/`, `cfg.{cpp,h}` and `README.md`. None of the four extracted
`support/physical_disc/` files changed, so the pin moves and nothing on disk
does.

### `MiSTer-devel/Minimig-AGA_MiSTer` -> `c5ffbf9`, and a sweep of every core source

Eight commits since `da7632e`, plus a merge. Taken in core PR #41, by
`cherry-pick -x`:

- **`7f93ed1`, `cfcc0dc`, `3c8a0ed`** (vanfanel), merged upstream as `c5ffbf9`
  "Fix Absolute Inebriation". Denise applies OCS/ECS `BPLCON1` writes to the
  live scroll state, Agnus exports which bus cycles are genuine bitplane DMA
  and their fetch phase, and Denise gains a pending-copy path for OCS/ECS
  LORES FMODE=0 off-grid fetches. AGA keeps the old load path. One conflict,
  in `minimig.v`'s Agnus instance: our save-state replay renamed
  `reg_address` to `reg_address_agnus`; ours kept, upstream's four new ports
  added. The fork's delta against upstream in the four touched files is the
  same before and after, apart from the floppy line below.
  `denise_bitplanes.v` was byte-identical before and is again after.
- **`8325735`** "readme: document A2065 Ethernet setup (#250)" -- into the core
  README. The OSD path it names (System -> Ethernet) holds here: the row is on
  `MENU_MINIMIG_CHIPSET1`, titled "System".

Already here or declined:

- `5fea457` chipdma_arb `minimig_idle` -- **settled**: taken as a design merge,
  `faf9588`, core PR #36. The "owed to the core repo" note above is closed.
- `c1134ac`, `e6d9262`, `a527fbb` -- the floppy line, still declined for the
  reasons below.

**Upstream's `cd32_cdtv` branch** (`0e1eed3`, 2026-08-20), not looked at
before. Fourteen commits: kblood's PRs #217-#227 plus Sorgelig's cleanup and
source reorganisation. None match a mainline commit by hash, patch-id or
subject, so they were checked by content -- the share of each commit's added
lines present in `cd32-native-mvp`. All of it arrived through `2ced823`
"Merge Minimig-AGA_MiSTer b265a3b: the upstream CD32/CDTV line", which
reworks some of it: the `hps_ext` status word is wider here, CDTV register
routing goes through `cdtv_selack`, and the memory card, 32-bit CDTV DMA
address, word-wide sector ingest and `bram.vhd` switch are all in. Nothing to
take.

**`RobSmithDev/Minimig-AGA_MiSTer`** `b5312c5`: tree-identical to upstream
`c1134ac`, the squash of it. Nothing beyond the declined floppy line.

**`kblood/Minimig-AGA-cd32-cdtv-native-akiko`**, the three branches no doc
named:

- `universe-chip-dma-slot-guard` -- `114ab43`, already in.
- `cdtv-z3-dma-addr` -- the branch behind upstream #224-#227, already in.
- `a1200-speed-5x` -- the stock-speed throttle is in, as upstream's corrected
  `148931d` (T22 in the core's `core-accuracy-todo.md` benches it). Its last
  commit, `38f3f55`, is a diagnostic build forcing the throttle on at divisor
  5; not for a shipping core.

## Sync 2026-10-09: seven of eleven taken, and why the other four are not

Eleven commits landed on `MiSTer-devel/Main_MiSTer` between `57276f0c` and
`42e6400`. Seven are taken, all as `cherry-pick -x`, all applied without
conflict. `syntax_check.sh` and `tests/` pass; the ARM build is CI's.

### Taken

- **`629c7ba` "Minimig: switch Direct Video to HDMI for RTG (#1346)"** -- ours by
  subject. With `direct_video` on, a Minimig core that enables its RTG
  framebuffer now leaves Direct Video for a normal HDMI mode and comes back when
  RTG is off. Checked against this fork rather than assumed:
  - it gates on `is_minimig()`, which here prefix-matches `minimig*` and
    `amigacd*` and returns 1 or 2 -- both truthy -- so AmigaCD and MinimigCD get
    it, not only stock Minimig;
  - `get_video_info()` now reads `UIO_GET_FB_PAR` on every call for Minimig
    instead of only on a CRC change. Its one periodic caller is the
    `res_timer` block in `user_io_poll()`, every 500 ms: seven SPI words twice a
    second, nowhere near the sector path;
  - every `cfg.direct_video` / `cfg.vsync_adjust` reader left in `video.cpp` is
    line-for-line upstream's, so the fork has no private reader the patch
    missed. The eleven in `menu.cpp` match upstream's eleven, which also stay
    on the raw setting.
  - It also drops upstream's "Disabling vsync_adjust because of enabled direct
    video" overwrite of `cfg.vsync_adjust`; `video_vsync_adjust()` masks it
    instead, so the setting survives an RTG round trip.
- **`c97c052` "video: fix crash on bad EDID extension count (#1341)"** -- a crash
  on a malformed display, any core.
- **`4360db7` "video: pad vertical blanking when vsync_adjust=2 lands on an
  integer PLL multiplier (#1343)"** -- any core; applied before `629c7ba`, which
  then rewrites its guard to `video_vsync_adjust()`.
- **`756178e` "Terminal sized for VGA without scaler (fb_terminal=2)"** and
  **`dc04c10` "Fix Win+F12 opening system settings for cores using F12
  modifier"** -- small, menu-level, no Minimig interaction. Taken to keep
  `video.cpp` / `menu.cpp` close to upstream so the next sync stays textual.
- **`e55f03c` default main stick axes to stick 1** and **`42e6400` X-Gunner
  lightgun** -- `input.cpp` only, additive.

### Not taken, and why

- `6ddadac` mac fast SCSI, `93033fe` Atari Falcon SCSI, `ebcdb4e` Sun family,
  `d5f3844` Sun ss20 CD audio -- cores we do not build, same reasoning as the
  NeXT series on 2026-10-01. They add `support/{falcon,sun}/` and a few
  `user_io.cpp` dispatch lines; none touch Minimig paths, so a later take stays
  textual.

### `kblood/Main_MiSTer-cd32-cdtv`, all 29 branches: nothing to take

`amiga-console` is still `52625a4` and is an ancestor of our `master`. The
other branches carry 96 commits that are neither in our tree nor in
`MiSTer-devel/Main_MiSTer`. 16 of those match one of our commits by patch-id or
subject, or are already cited in these docs. The rest are what
`sync-2026-08-24-assessment.md` found: staging for the upstream PRs that reached
us through `Main_MiSTer`, plus diagnostics. Only two branches moved after that
assessment:

- **`dotc-cdtv-stch-playstart-mvp` `5e0f2ea` (2026-09-01)**: a forward-port of
  `4964847` / upstream `2c410aa` (play-start STCH, the DotC map-of-Britain park)
  onto his `cd32-native-mvp` line. `2c410aa` is an ancestor of our `master`, and
  `stch_playstart_pending` with its drain is in `cdtv_cd.cpp`. Already have it.
- **`hybris-chipset-trace` (to 2026-08-26)**: instruments for the Hybris
  cycle-diff work. `chipset_trace.cpp` drains `rtl/chipset_bus_trace.v` over UIO
  class `0xF600`, plus a `cpu_trace` marker arm, a `MINIMIG_DEBUG` INI byte,
  `/tmp/minimig_dbg` runtime knobs and an OSD Debug page. **None of it is
  portable here**, for two reasons:
  - our core has neither the trace ring nor `dbg_config` in `userio.v`;
  - `015bc67` defines `UIO_MM2_DBG 0xFA`, and **`0xFA` is already
    `UIO_MM2_FLPEX01`** in this tree, MiSTer Floppy's first command. Taken as is,
    every debug-byte push would be decoded as a floppy command.

  `668218a` (any `minimig*` name is Minimig) is the one general fix, and our
  `is_minimig()` already prefix-matches, `amigacd*` included.

If the Hybris instruments are ever wanted, they need a different UIO opcode and
the RTL half in the core repo first.

### `Anime0t4ku/Main_MiSTer_Physical_Disc` -> `dc0c4c9`, no re-extract

Two commits (`cc22b97` + its merge): `PHYSICAL_DISC_BOOT=1` launches the disc
left in the drive at power-on. It lives entirely in
`physical_disc_launch.cpp` (excluded, see "Syncing the physical_disc module")
plus a `cfg` key with no reader without it. The four extracted files are
**byte-identical** at `dc0c4c9`, checked with the diff below, so the pin moves
and nothing on disk does. This tree already boots a disc at power-on its own
way: the `.mgl` carries the disc as a setting (`ee8bb6b`).

### `theshaneobrien/mister-disc-drive-support`: four new commits, nothing to lift

Last looked at `1a42060` (2026-08-24). New on `physcd`, ignoring the upstream
merge `fc44c31` (Release 20260907, already in this tree by our own syncs):

- `470794e` "physcd: the shared ide-family seam" -- the interesting one, and it
  is **convergence, not a fix for us**. It routes the physical drive into
  `cdrom_read_raw_sector()` / `cdrom_read_track_raw()` in `ide_cdrom.cpp` so
  Akiko, CDTV and ATAPI share one TOC adapter. That is the design
  `load_physical_disc()` already has here. Its two "easy to get backwards"
  conventions are both handled on our side: the 49-real-tracks-plus-lead-out
  cap, and reusing `chd_last_partial_lba` as the running LBA across a partial
  read. Its own commit says CD32/CDTV mounting is "not yet wired" there; we are
  ahead on exactly the cores it names.
- `254e49d` / `a629ac3` -- the "support log" (`physcd_log.txt`, written because
  `cfg.cpp` sends stdout to `/dev/null` without `DEBUG=`). Built on their engine
  and their state, so not portable as code. The **idea** stands for us too: a
  tester on a normal box sees none of our `printf`s. Noted, not taken.
- `9a8c513` "translate: an overlay toggle is not a video change" -- fixes their
  translation overlay; this tree has no `overlay_present()`, so nothing to fix.

`physcd-ra` (RetroAchievements line) is 107 commits beyond `physcd`, all overlay
/ translation / RA work. Not reviewed commit by commit; nothing in it touches
Amiga.

## Sync 2026-10-01: three of thirteen taken, and why the other ten are not

Thirteen commits landed on `MiSTer-devel/Main_MiSTer` between `5b3ae64` and
`57276f0c`. Three are taken.

### Taken

- **`57276f0c` "Change user_io_status_set value in video.cpp (#1335)"** -- a real
  one-line bug: `user_io_status_set("[8:5]", 0x160)` shifts left by the field's
  start bit before masking, so `(0x160 << 5) & 0x1E0` is 0 and bits 5-8 were
  always cleared. The right-aligned value is `0xB`. Guarded by `is_menu()`, so it
  changes nothing for this core; taken to keep the file identical to upstream.
- **`aa271e41` "minimig: Update CD32 and CDTV MGL file handling logic (#1323)"** --
  ours by subject. It lets an `.mgl` launch mount a CD32 or CDTV image, which this
  tree could not do: only the ADF arms of the MGL dispatch existed, so items 4 and
  5 fell through. Applied without conflict, which given a `menu.cpp` that diverges
  by ~3200 lines means only that git found matching context. What makes it correct
  here is checked and written in the commit message: this fork kept upstream's row
  numbering and mask bits (`menusub` 4 / `menumask` 0x10 is "Start CD32 Game", 5 /
  0x20 is CDTV), so `index == 4` and `index == 5` select the rows upstream means.
  **The CD mount delay is not bypassed** -- it lives in the Akiko disc-announce
  path, downstream of `minimig_cd_drive_open()`, not in the menu, so an MGL launch
  gets the same delayed announce a hand mount does.
- **`a34a5e8b` "FileSave: write to a temp file and rename it over the target
  (#1331)"** -- `file_io.cpp` verbatim, which is where the value is: every caller
  (the minimig config, NVRAM, the Akiko settings, the recent lists) becomes
  crash-safe for free. **Its `user_io.cpp` half is deliberately not taken**: it
  replaces the save-state write with a single `FileSave()` call, and ours writes in
  chunks with `ProgressMessage()` between them because a state is up to ~11.5 MB on
  an O_SYNC fd and one write freezes the OSD for seconds. The atomic rename was
  added to our chunked writer instead, so both paths end up crash-safe and nothing
  regresses. Upstream also deletes the `fileTYPE f` our writer still opens; that
  line is restored, with a comment, because losing it is a compile error and not an
  obvious one to attribute later.

### Not taken, and why

- `dee9fabd` "minimig: some refactoring in floppy control" and `3994f052`
  "Minimig: expose zero floppy drive support, add floppy amount to presets" -- the
  userspace half of the floppy line this document already declines (see the next
  section). `3994f052` also touches the `minimig_config` struct, which is the
  permanent divergence described under `5b3ae64`.
- `ce432dca` FM Towns Marty, `6e14ab22` Apple-III, `f6a3caa6` Mac ethernet/SCSI --
  cores we do not build. No conflict with anything here; left out only because a
  take has to be verified and these have no reader.
- `7875437b`, `7f344862`, `3116e7f4`, `5fb9bd10`, `5a3a0866` -- the NeXT series,
  same reasoning.

So the pin moves to `57276f0c` and stays a **partial take** for the same reason as
before: the floppy userspace and the `minimig_config` struct.

### `Anime0t4ku/Main_MiSTer_Physical_Disc` -> `e6c7abd1`

Twenty commits, sixteen of them binary "Add files via upload" and workflow churn.
Re-extracted verbatim per the procedure below; additive in both files, and the
disc-type enum -- the check this document insists on at every sync -- is unmoved.

It brings `physical_disc_tray_status()`: `CDROM_DRIVE_STATUS` on a separate
`O_NONBLOCK` handle, no spin-up and no data read, and it answers while no disc
session is open. Upstream uses it to notice a disc inserted after launch. **This is
the piece a CD32 tray-close would need**, which the core cannot do at all today --
once the mount at startup fails nothing looks at the drive again. Not wired up,
noted here so the next person does not go looking for a way to poll the tray.

### `MiSTer-devel/Minimig-AGA_MiSTer`: pin unchanged, one commit owed to the core repo

Five commits since `da7632e`. Three (`c1134ac`, `e6d9262`, `a527fbb`) are the
floppy line, declined below. The fourth is a merge.

The fifth matters and is **not** a userspace change:
`5fea457` "chipdma_arb: take minimig_idle off the address and drive-select cones"
restructures the module this fork's RTL owns, splitting `arm_now` into
`start_candidate` (without `minimig_idle`) and the qualified arm so the late
`minimig_idle` stays out of the `sd_addr` cone. Our `chipdma_arb.v` carries extra
terms upstream does not have (`~cpu_chip_slot_req`, `~dma_hold`, `chip_slot_window`)
and RTL #32 adds `sdram_ready_sync`, so taking it is a design merge -- our terms fold
into `start_candidate` -- and not a textual one. Tracked in the core repo.

## Minimig-AGA upstream has merged real floppy support, and we already have some

`c1134ac` "Add real floppy drive support (#245)" landed upstream after the
`da7632e` pin. **It is not taken, and it must not be taken blind**, because the
core already carries MiSTer Floppy from a different source: `rtl/MiSTerFloppy*.v`
are in the tree today and the core builds `feature/misterfloppy` (see "Which core
branch to build").

So this is not a missing feature, it is two implementations of one feature
arriving from two directions. What the sync needs is a file-by-file comparison of
upstream's `rtl/MiSTerFloppy*.v`, `rtl/paula_floppy.v` and `rtl/userio.v` against
ours, and a decision per file -- exactly the shape of the `ss_*` ownership
question T25 settled in the core repo.

Taking it as a merge would either duplicate the modules or silently replace a
version the fork has been testing against, and the symptom is a floppy that does
not read on hardware nobody has in front of them.

**Compared 2026-09-17, and it is a downgrade.** The comparison is written up as
T26 in the core repo's `docs/core-accuracy-todo.md`. In short: we lack nothing
`c1134ac` adds, we are ahead in six ways -- including `MiSTerFloppyIBM.v`, a
three-tenant user port, two retimings in `paula_floppy.v` and a bench for the
`_READY` rule -- and upstream's `misterfloppy.qip` lists a Verilog file that
commit does not ship.

**One of those six has expired.** On 2026-09-17 upstream `Main_MiSTer` had no
userspace side for the feature at all. `3d7a844` landed it on 2026-09-18, so
that point no longer stands -- see the section above, where the userspace half
turns out to diverge from ours in the config struct. The other five are
unaffected.

So the Minimig-AGA pin stays at `da7632e` deliberately, not for want of effort.
Revisit when upstream adds the missing file, or when the MiSTerFloppy project
moves past the snapshot both trees share.

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

**`AmigaCD.qsf` (`Minimig.qsf` before the 2026-09-15 rename) is byte-identical
to `amigacd-core`.** The raised
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

**Merged through `0039110` on 2026-09-14** (see the table above), so the gap
below is history rather than a standing debt. It was, as of 2026-07-31, **58
commits** ahead of `0f3a6ad`, the base our tree shares with it. Unlike the physical_disc module this cannot be a re-extract: it
touches `menu.cpp`, `ide.cpp`, `ide_cdrom.cpp` and `user_io.cpp`, all of which
we have modified. Treat it as a real merge with conflicts, on its own branch,
with a hardware test afterwards.

