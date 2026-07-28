# Integration plan

Target: this repo, which is `kblood/Main_MiSTer-cd32-cdtv` @ `amiga-console`
with `support/physical_disc/` from `Anime0t4ku/Main_MiSTer_Physical_Disc`
grafted on.

Line numbers are against the pins in `docs/upstream-pins.md`. `ide_cdrom.cpp`
is byte-identical between `cd32-native-mvp` and `amiga-console`; the
`support/minimig/` files are **not** — they shifted by ~130 lines.

Phases 0-2 are the minimum for "a CD32 game boots from a real disc".
Phase 3 decides whether it is *usable*.

---

## Phase 0 — bring the module across ✅ done (`efde1c5`)

Copied `support/physical_disc/{physical_disc,physical_disc_acoustic}.{cpp,h}`.
The Makefile globs `$(wildcard ./support/*/*.cpp)`, so no build change was
needed, and `-lpthread` is already in `LFLAGS`. Both files are self-contained:
libc, `<linux/cdrom.h>`, `<scsi/sg.h>`, and their own headers only. No `cfg`
references, so the `physical_disc_launch` / `physical_disc_mount_delay` /
`physical_disc_acoustic` settings from the source fork were **not** needed.

`physical_disc_launch.cpp/.h` were deliberately **excluded**. They include
`../megacd/megacd.h`, `../psx/psx.h`, `../saturn/saturn.h`,
`../neogeo/neogeocd.h`, `../3do/3do.h`, `../pcecd/pcecd.h`, `../cdi/cdi.h`,
`../../cfg.h` and `../../achievements.h` — none of those cores have
physical-disc support in this tree, and `achievements.h` does not exist here at
all. Auto-launch is phase 4; bringing it in early would have meant porting six
unrelated cores first.

---

## Phase 1 — the adapter ✅ done (`efde1c5`)

Applied by `tools/patches/phase1_adapter.py` (asserted anchors, idempotent).

| File | Change |
|---|---|
| `cd.h` | `int phys` in `toc_t` |
| `ide.h` | `uint8_t phys` in `drive_t` |
| `ide_cdrom.cpp` | `load_physical_disc()` after `load_chd_file` (277) |
| `ide_cdrom.cpp` | phys branch at the head of `cdrom_read_raw_sector` (1162) |
| `ide_cdrom.cpp` | sentinel dispatch in `cdrom_parse` (1837) |
| `ide_cdrom.cpp` | `cdrom_close_chd` (1820) releases the drive |

`load_physical_disc` is `load_chd_file`'s `toc_t` → `drive_t/track_t` loop with
the CHD specifics removed: `sectorSize` 2352, `attr = 0x40` for `type != TT_CDDA`,
`chd_offset = 0`, plus the lead-out sentinel track and the first-data-track
`data_num` picker.

The sentinel branch in `cdrom_parse` sits *ahead* of the `last_path` /
`same_path` bookkeeping, because there is no file to `getFullPath` and a
re-mount must not re-spin the drive. `physical_disc_open` returns early when
the same device is already open, so calling `load_physical_disc` again just
re-reads the TOC — which gets disc-swap-on-remount for free.

---

## Phase 2 — make the bridge see the disc ✅ done (`73fd46b`)

Applied by `tools/patches/phase2_bridge.py` and `phase2b_mount.py`.

Turned out to be two layers, not one. The bridge fixes below are necessary but
not sufficient — see 2b, without which all of phase 1 and 2a is dead code.

### 1. Media detection — three call sites

All test `drv->cd && (drv->chd_f || drv->f)`. A physical drive has neither
handle.

| File | Function | Line |
|---|---|---|
| `support/minimig/akiko_cd32.cpp` | `cd_is_mounted` (447) | 452 |
| `support/minimig/akiko_cd32.cpp` | `cd_find_drive` (461) | 466 |
| `support/minimig/cdtv_cd.cpp` | `cdtv_find_drive` (189) | 194 |

Change all three to `(drv->chd_f || drv->f || drv->phys)`.

### 2. A fourth `chd_f` guard, on the CDTV **data** path

`cdtv_cd.cpp:881`, inside the READ DATA case of `cdtv_dispatch`:

```c
if (!drv || !drv->chd_f) { ... cd_error = 1; ... }
```

This one is easy to miss because it is not an audio guard — it sits directly
above the `cdrom_read_raw_sector` call at 896. Without it, CDTV data reads fail
even after the find-drive fix. Same treatment: `|| drv->phys`.

Grep `chd_f ||` and `!drv->chd_f` across `support/minimig/` before calling this
step done.

### 3. CDDA read path

`cd_read_audio_sector` (`akiko_cd32.cpp:1624`) and `cdtv_read_audio_sector`
(`cdtv_cd.cpp:379`) both open with `if (!drv || !drv->chd_f || !buf2352) return false;`
(1626 and 381) and call `mister_chd_read_sector` directly (1655 and 397). Add a
phys branch after the existing track lookup and `attr & 0x40` audio check.

### 4. CDDA endianness — will produce white noise if missed

`akiko_push_audio_sector` (1691) applies `bswap_16` per sample at 1700-1701,
because CHD stores red-book audio big-endian. `cdtv_push_audio_sector` (424)
does the same at 433-434. `READ CD` with flags `0x10` returns **disc order**,
i.e. little-endian.

Swap inside the phys read branch so the buffer arrives at the push path in CHD
order, leaving the push code untouched. Test early with an audio-track title
(Microcosm, Banshee) — the failure mode is loud and unmistakable.

### 5. Subcode (amiga-console only)

`build_subcode_deint` (1712) synthesises the Q channel from the track table and
`akiko_push_subcode` (1762) ships it. This is *synthesised*, so it keeps working
unchanged on a physical disc. Real Q-channel data from the drive
(`physical_disc_read_sector_sub`) is a later accuracy improvement, not a
requirement.

### 6. Mount path plumbing (phase 2b)

Not in the original plan; found while testing the phase 2a reasoning.

`ide_open` (`ide.cpp:1114`) runs the entire CD mount behind:

```c
if (filename[0] && FileOpenEx(&hdd_file[unit], filename, ...))
```

The sentinel is not a path and never opens, so `cdrom_parse` was unreachable
and every phase 1 and 2a change was dead code. The physical branch now runs
*ahead* of that gate. On failure it deliberately does not return: `FileOpenEx`
rejects the sentinel, the mount block is skipped, and control falls to the
normal not-present cleanup, which closes any stale CHD and clears the
akiko/cdtv save paths.

`ide_img_set` (`ide.cpp:416`) then had the same shape of assumption:
`drive->present = f ? 1 : 0`. A physical drive has media but no file, so it
landed with `present=0` / `cd=0` and `cd_find_drive` refused it regardless of
`drive->phys`. Presence now comes from `(f || drive->phys)`, and the no-file
branch no longer zeroes the `total_sectors` that `load_physical_disc` derived
from the drive's own TOC.

Geometry is passed as zeros: `ide_img_set` only applies cylinders/heads/spt
on the `!drive->cd` path.

---

## Phase 3 — make it fast enough ⬜ next

### 7. Seek hints

Call `physical_disc_seek_hint(s_lba)` where a data read is armed —
`akiko_cd32.cpp:1027`, in the `cmd_multi` PLAY DATA branch right after
`cd_data_lba_base = (int32_t)s_lba;` — and at the CDTV equivalent
(`cmd_play`, `cdtv_cd.cpp:804`).

Without this the background prefetch lane never runs ahead, and every Akiko
cache-window fill becomes 16 synchronous `SG_IO` round trips inside
`user_io_poll()`, each with a 3-second timeout. That is a hard freeze of input
and OSD, not merely slow loading.

### 8. Collapse the double cache

Akiko's 4-window × 128-sector LRU (`akiko_prefetch_fill`, 1856; `akiko_prefetch_get`,
1882) now sits on top of physical_disc's own two-lane ring. A cold window fill
costs 16 blocking SCSI transactions (`SYNC_IO_BURST = 8` per miss).

1. Preferred: bypass `akiko_prefetch_get` when `drv->phys` and call
   `cdrom_read_raw_sector` directly, letting physical_disc do the caching.
2. Or: drop `AKIKO_PREFETCH_SECTORS` to 16 when `drv->phys`.

Measure first. `/tmp/physical_disc_stats.log` gives hit rate and worst-case I/O
latency; the `sec_req GAP` diagnostic in `akiko_handle_sec_req` (2006) gives the
other half.

Target is ~150 sectors/s (2x red book). kblood's history shows the SPI push path
was already the bottleneck against CHD, so this may end up transport-limited
rather than drive-limited.

---

## Phase 4 — plumbing and polish ⬜

### 9. OSD trigger ✅ done (`7f9e9d1`)

A "Real CD" row in the AmigaCD console OSD, under the CD image row.
Selecting it parks `PHYSICAL_DISC_SENTINEL` in `hardfile[0].filename`, sets
`cfg=2` and calls `ide_open` — the same three steps as an image mount. The CD
row shows `* physical drive *` while mounted; BACKSPACE on it still ejects.

Re-selecting while mounted re-reads the TOC, which is the manual disc-swap
path until item 12 lands. "Save settings" persists it, so a saved config
re-mounts the drive on the next core load.

The stock Minimig Drives menu is deliberately untouched.

Bring `physical_disc_launch.cpp` in (or write a minimal Minimig-only
replacement) and add:

```c
else if (is_minimig()) {
    ok = (cdrom_parse(cd_slot, PHYSICAL_DISC_SENTINEL) != NULL);
    if (ok) minimig_reset();
}
```

`physical_disc_launch_startup()` gates on core names beginning with `"CD-"`;
`CONF_STR` here is `MinimigCD`, so relax that gate or match `minimig*`.

The OSD entry goes in `menu.cpp` next to the existing CD image selector — on
`amiga-console` that is the reworked console OSD, not the stock Minimig menu.

### 10. Disc identification

`physical_disc_identify()` has no CD32/CDTV type. Add `PHYSICAL_DISC_DISC_CD32`
/ `_CDTV`, detected from the ISO9660 PVD system identifier at LBA 16
(`CDTV`, `AMIGA`) or the CD32 boot block. This wires the disc into the existing
auto-launch machinery.

### 11. Per-game NVRAM

`akiko_cd32_set_cd_path()` (2280-ish) hashes the image path basename into
`cd32-<hash>.nvr` via `compute_save_path` (417). With a physical disc that path
is a constant sentinel, so every game shares one save. Key off
`physical_disc_disc_label()` / `physical_disc_disc_serial()` instead — both
already exist. There is a `TODO(phase 4)` marking the spot in `cdrom_parse`.

### 12. Hot swap

Wire `physical_disc_swap_consume()` / `physical_disc_media_changed()` into the
Akiko mediachange path (the `mounted != cd_last_mounted` block at 2554) so a
swap triggers `akiko_build_toc()` rather than serving stale geometry.

---

## Known risks

**Throughput.** A cache miss against a USB optical drive is milliseconds where
CHD is microseconds. Phase 3 is not an optimisation pass; it decides whether
anything boots.

**Blocking I/O in the main loop.** `fetch_sector()` blocks on a miss with a 3s
timeout, and everything in `user_io_poll()` stalls behind it. If seek hints are
not enough, the read path has to become non-blocking, with the Akiko engine
holding `sec_req` across polls until data is ready. That is a redesign, not a
patch — it is the single most likely thing to derail this.

**Pregaps.** `CDROMREADTOCENTRY` reports INDEX 1 starts only. The Akiko TOC
builder consumes only track starts, so data is unaffected, but CDDA tracks with
real pregaps may start slightly early or late.

**Nothing compiled yet.** `scripts/wsl-build.sh` replaces the inherited
`.wsl_build.sh` (which pointed at another machine) and has a `check` mode
running `-fsyntax-only` over just the seven changed files.

`core.autocrlf=true` had produced a CRLF checkout, which would have broken
both GNU make (`CC` picking up a trailing CR) and every `.sh`
(`bad interpreter: /bin/bash^M`) under WSL. `.gitattributes` now forces LF in
the working tree.
