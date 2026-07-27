# Integration plan

Target: `kblood/Main_MiSTer-cd32-cdtv`, with `support/physical_disc/` dropped in from
`Anime0t4ku/Main_MiSTer_Physical_Disc`.

Ordering below is dependency order, not priority order. Steps 1-6 are the minimum for
"a CD32 game boots from a real disc". Steps 7-8 decide whether it is *usable*.

---

## Phase 0 — bring the module across

Copy `support/physical_disc/` (6 files) as a unit. Hand-apply the small set of real
changes to `cd.h`, `cfg.cpp`, `cfg.h`, `main.cpp`, `menu.cpp`, `scheduler.cpp`,
`support.h`, `file_io.*`, `user_io.cpp`. Do **not** merge the comment-stripped versions
of those files — see `docs/analysis.md`.

Required from `cd.h`: `int phys;` in `toc_t`.
Required from `cfg.h`: `physical_disc_launch`, `physical_disc_mount_delay`,
`physical_disc_acoustic`.

Verify: builds clean, existing CHD-backed CD32 boot still works, physical disc path is
dead code so far.

---

## Phase 1 — the adapter

### 1. Add the flag

`ide.h`, `struct drive_t`:

```c
uint8_t phys;   // media is a physical CD-ROM drive, not a file
```

### 2. `load_physical_disc(drive_t *drv)`

New function in `ide_cdrom.cpp`, alongside `load_chd_file`. Body is the translation
loop from `load_chd_file` lines 323-352 with the CHD specifics dropped:

```c
static const char* load_physical_disc(drive_t *drv)
{
    toc_t tmp = {};
    if (physical_disc_open(NULL)) return 0;
    if (physical_disc_load_toc(&tmp)) { physical_disc_close(); return 0; }

    memset(drv->track, 0, sizeof(drv->track));
    drv->track_cnt = 0;

    for (int i = 0; i < tmp.last; i++) {
        cd_track_t *src = &tmp.tracks[i];
        track_t    *trk = &drv->track[i];
        trk->number     = i + 1;
        trk->sectorSize = src->sector_size;          // always 2352
        trk->attr       = src->type ? 0x40 : 0x00;   // TT_MODE1 -> data
        trk->mode2      = 0;                          // raw path ignores this
        trk->chd_offset = 0;
        trk->start      = src->start;
        trk->length     = src->end - src->start;
        drv->track_cnt++;
    }

    // lead-out sentinel, same as load_chd_file
    track_t *lead_out = &drv->track[drv->track_cnt];
    lead_out->number  = drv->track_cnt + 1;
    lead_out->attr    = 0;
    lead_out->start   = tmp.tracks[tmp.last - 1].end;
    lead_out->length  = 0;

    drv->phys    = 1;
    drv->cd      = 1;
    drv->present = 1;
    drv->chd_f   = NULL;
    drv->f       = NULL;
    return PHYSICAL_DISC_SENTINEL;
}
```

Also set whatever `load_chd_file` sets after its loop (`total_sectors`, `data_num`,
`load_state`) — check the tail of that function when implementing.

### 3. Hook `cdrom_parse`

`ide_cdrom.cpp:1837`. Before the `getFullPath` / `load_chd_file` chain:

```c
if (filename && !strcmp(filename, PHYSICAL_DISC_SENTINEL)) {
    res = load_physical_disc(&ide_inst[num].drive[drv]);
    ...
}
```

Also patch the idempotent-remount short-circuit at line ~1860: it currently requires
`ide_inst[num].drive[drv].chd_f != NULL`, so a physical disc would re-read the TOC and
re-spin the drive on every in-core OSD reset. Add `|| ide_inst[num].drive[drv].phys`.

### 4. Read seam

Top of `cdrom_read_raw_sector()` (`ide_cdrom.cpp:1162`), before the `chd_f` branch:

```c
if (drive->phys)
    return physical_disc_read_sector((int)lba, buf, NULL) ? -1 : 0;
```

`READ CD` with flags `0xF8` returns exactly the 2352-byte layout the PBX engine
expects. No mode-2 offsetting, no sync-header synthesis.

---

## Phase 2 — make the bridge see the disc

### 5. Media detection (do not skip)

`akiko_cd32.cpp:416` (`cd_is_mounted`) and `:430` (`cd_find_drive`), plus
`cdtv_cd.cpp:180`, all test:

```c
if (drv->cd && (drv->chd_f || drv->f))
```

A physical disc has neither handle. Left unpatched, the bridge concludes there is no
media, `cmd_multi` falls into the no-disk branch, and the BIOS sits on the no-CD splash
forever. Change all three to:

```c
if (drv->cd && (drv->chd_f || drv->f || drv->phys))
```

Grep for `chd_f ||` across `support/minimig/` before declaring this done.

### 6. CDDA

`cd_read_audio_sector()` — `akiko_cd32.cpp:1485` and `cdtv_cd.cpp:353` — both open with
`if (!drv || !drv->chd_f || !buf2352) return false;`. Add a phys branch that calls
`physical_disc_read_sector(lba, buf2352, NULL)` after the existing track lookup and
`attr & 0x40` audio check.

**Endianness:** `akiko_push_audio_sector()` (`akiko_cd32.cpp:1561`) applies `bswap_16`
per sample because CHD stores red-book audio big-endian. `READ CD` with flags `0x10`
returns disc order, i.e. little-endian. Swap inside the phys read branch so the buffer
reaches the push path in CHD order and the push code stays untouched. Getting this
wrong produces white noise, not silence — it is an obvious failure, so test with an
audio-track title early (Microcosm, Banshee).

---

## Phase 3 — make it fast enough

### 7. Seek hints

Call `physical_disc_seek_hint(s_lba)` where a data read is armed:

- `akiko_cd32.cpp:923` — `cmd_multi` PLAY DATA branch, right after
  `cd_data_lba_base = (int32_t)s_lba;`
- the equivalent arm point in `cdtv_cd.cpp`

Without this the background prefetch lane never runs ahead, and every Akiko cache-window
fill becomes 16 synchronous `SG_IO` round trips inside `user_io_poll()`, each with a
3-second timeout. That is a hard freeze of input and OSD, not merely slow loading.

### 8. Collapse the double cache

Akiko's 4-window x 128-sector LRU now sits on top of physical_disc's own two-lane ring.
A cold window fill costs 16 blocking SCSI transactions (`SYNC_IO_BURST = 8` per miss).

Options, in order of preference:

1. Bypass `akiko_prefetch_get()` entirely when `drv->phys` — call
   `cdrom_read_raw_sector()` directly and let physical_disc do the caching.
2. Drop `AKIKO_PREFETCH_SECTORS` to 16 when `drv->phys`, so a miss is one burst.

Measure before choosing. `/tmp/physical_disc_stats.log` gives hit rate and worst-case
I/O latency; the Akiko `sec_req GAP` diagnostic gives the other half of the picture.

Target is ~150 sectors/s (2x red book). kblood's commit history shows the SPI push path
was already the bottleneck against CHD, so this may end up limited by transport rather
than by the drive.

---

## Phase 4 — plumbing and polish

### 9. Mount path

`physical_disc_launch.cpp::physical_disc_mount_current_core()` has no Minimig branch.
Add:

```c
else if (is_minimig()) {
    ok = (cdrom_parse(cd_slot, PHYSICAL_DISC_SENTINEL) != NULL);
    if (ok) minimig_reset();
}
```

`physical_disc_launch_startup()` gates on core names beginning with `"CD-"`; the
CD32 core's `CONF_STR` is `MinimigCD`, so relax that gate or match on `minimig*` as the
core repo's README expects.

Minimig's CD slot is mounted from `menu.cpp`, not from `CONF_STR` — add a
"Physical CD" entry there next to the existing CD image selector.

### 10. Disc identification

`physical_disc_identify()` has no CD32/CDTV type. Add `PHYSICAL_DISC_DISC_CD32` /
`_CDTV`, detected from the ISO9660 PVD system identifier at LBA 16 (`CDTV`, `AMIGA`)
or the CD32 boot block. This wires the disc into the existing auto-launch machinery for
free.

### 11. Per-game NVRAM

`akiko_cd32_set_cd_path()` hashes the path basename into `cd32-<hash>.nvr`. With a
physical disc that path is a constant sentinel, so every game would share one save.
Key off `physical_disc_disc_label()` / `physical_disc_disc_serial()` instead — both
already exist and are used for save naming in the other cores.

### 12. Hot swap

Wire `physical_disc_swap_consume()` / `physical_disc_media_changed()` into the Akiko
mediachange path (`akiko_cd32.cpp:2258`) so a disc swap triggers `akiko_build_toc()`
rather than serving stale geometry.

---

## Known risks

**Throughput.** A cache miss against a USB optical drive is milliseconds where CHD is
microseconds. Steps 7 and 8 are not optimisations; they decide whether anything boots.

**Pregaps.** `CDROMREADTOCENTRY` reports INDEX 1 starts only — no INDEX 0 pregap. The
Akiko TOC builder only consumes track starts, so data is unaffected, but CDDA tracks
with real pregaps may start slightly early or late. If that bites, `READ CD` subchannel
(`physical_disc_read_sector_sub`, already implemented) provides real Q-channel data to
replace the synthesised SUBQ replies at `akiko_cd32.cpp:1103`.

**Blocking I/O in the main loop.** `fetch_sector()` blocks on a miss with a 3s timeout.
Everything in `user_io_poll()` — input, OSD, floppy — stalls behind it. If step 7 is not
enough, the read path may need to become non-blocking with the Akiko engine holding
`sec_req` across polls until data is ready.

**Fork drift.** The physical-disc fork's base is ~2 months ahead of kblood's tip.
Rebase kblood onto current upstream first if that turns out to be cheap; otherwise pin
and move on.
