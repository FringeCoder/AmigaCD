# Upstream teardown

Line numbers refer to the commits recorded in `docs/upstream-pins.md`.

## Anime0t4ku/Main_MiSTer_Physical_Disc

Fork of upstream `Main_MiSTer` at `7317947` (2026-07-13).

Real footprint outside `support/physical_disc/` is ~230 semantically changed lines —
diffing with comments and whitespace stripped shows the bulk of the visible diff in
`input.cpp`, `menu.cpp`, `user_io.cpp` and `file_io.cpp` is a comment-removal pass:

| File | Raw diff lines | Real changed lines |
|---|---|---|
| `input.cpp` | 3413 | 81 |
| `menu.cpp` | 564 | 101 |
| `user_io.cpp` | 350 | 20 |
| `file_io.cpp` | 221 | 32 |

**Do not merge these files wholesale** — it would strip kblood's inline documentation,
which is where most of the Akiko reverse-engineering notes live.

### `support/physical_disc/physical_disc.cpp` (1389 lines)

- Device discovery: probes `/dev/sr0`..`/dev/sr7` with `CDROM_DRIVE_STATUS`, honours a
  preferred device set by `physical_disc_set_device()`. Installs a udev rule at startup.
- Sector reads: `SG_IO` with a 12-byte `READ CD` (opcode `0xBE`) CDB.
  - `cdb[9] = 0xF8` for data tracks (sync + header + user data + EDC/ECC = 2352 bytes)
  - `cdb[9] = 0x10` for audio tracks (2352 bytes, disc byte order)
  - `cdb[10] = 0x01` adds 96 bytes of subchannel; auto-disabled if the drive rejects
    multi-sector subchannel reads.
  - Falls back to per-sector `CDROMREADRAW` ioctl, then to zero-fill with a bad-sector
    counter.
- TOC: `CDROMREADTOCHDR` + `CDROMREADTOCENTRY`, emitted as a `toc_t` with the new
  `int phys` field set. Track type is `TT_MODE1` for data, `TT_CDDA` for audio;
  `sector_size` is always 2352 and `offset` is always 0.
- Caching: background pthread, ring split into two lanes (data / audio),
  `IO_BURST = 16`, `LOOKAHEAD_SPAN = 96`, `WARMUP_SECTORS = 768`.
  `physical_disc_seek_hint(lba)` moves a lane cursor so the prefetcher runs ahead.
- Speed: `CDROM_SELECT_SPEED` capped at 4x unless native speed is requested and the
  disc has no audio tracks.
- Media watcher thread with disc-in/disc-out events, swap arming, disc label and serial
  extraction, and disc-type identification (MegaCD, Saturn, PSX, PCE, NeoGeo, 3DO,
  audio) used to auto-launch the matching core.

### Integration pattern

The sentinel string `"*PHYSICAL_DISC*"` is passed wherever a filename would go. Each
core's CD backend recognises it, calls `physical_disc_open()` + `physical_disc_load_toc()`,
and thereafter branches on `toc.phys` in its seek/read paths.

Cores wired: MegaCD, PSX, Saturn, PCE-CD, NeoGeo CD, 3DO, CD-i.
`ide_cdrom.cpp` is untouched. There is **no Minimig/Amiga branch anywhere**.

## kblood/Main_MiSTer-cd32-cdtv

Tip `feaabb4`, 2026-05-26 — roughly two months behind the physical-disc fork's base.

### `support/minimig/akiko_cd32.cpp` (2595 lines)

Userspace reimplementation of WinUAE's `akiko.cpp` command interpreter. Polled from
`user_io.cpp:3119` (main loop, not frame-rate limited), one bridge action per poll.

UIO channels:

| Address | Purpose |
|---|---|
| `0xF400` | Akiko command / result framer |
| `0xF500` | Sector push (PBX DMA), 2352 bytes |
| `0xF440` | NVRAM dump / clear-dirty, 1024 bytes |
| `0xF200` | CDDA audio FIFO |
| `0xF800` | CDTV CR-511 bridge |

Status word (cmd `0x63`): bit 11 `akiko_req`, bit 10 `akiko_sec_req`, bit 9 `rx_busy`,
bit 8 `cdda_req`, bit 7 `nvr_dirty`.

Data path:

- `cd_find_drive()` / `cd_is_mounted()` scan `ide_inst[p].drive[d]` for
  `drv->cd && (drv->chd_f || drv->f)`.
- Data sectors: `cdrom_read_raw_sector(drv, lba, buf)` behind a 4-window x 128-sector
  LRU prefetch cache (`akiko_prefetch_get`).
- Audio sectors: `cd_read_audio_sector()` calls `mister_chd_read_sector` **directly**
  and requires `drv->chd_f`. `akiko_push_audio_sector()` applies `bswap_16` per sample
  because CHD stores red-book audio big-endian.
- Per-game NVRAM keyed on FNV-1a of the CD image path basename, via
  `akiko_cd32_set_cd_path()` called from `cdrom_parse`.

### `support/minimig/cdtv_cd.cpp` (1182 lines)

Same shape for the CR-511, gated on `minimig_config.chipset & CONFIG_CDTV`. Same
`chd_f` assumption in its audio reader (line ~353).

### `ide_cdrom.cpp` seams

- `cdrom_parse(num, filename)` at line 1837 — mount entry point. Dispatches to
  `load_chd_file` / `load_cue_file` / `load_iso_file`, then notifies the Akiko and CDTV
  bridges of the new path. Has an idempotent-remount short-circuit that tests
  `chd_f != NULL`.
- `load_chd_file()` at line 277 — **already translates `toc_t` -> `drive_t` + `track_t`**
  (loop at 323-352). This is the template for the physical-disc adapter.
- `cdrom_read_raw_sector()` at line 1162 — returns one 2352-byte raw frame, handling
  CHD (2352 / 2336 / 2048 per-track sector sizes) and flat BIN/ISO.

## Minimig-AGA-cd32-cdtv-native-akiko (`cd32-native-mvp`)

RTL: `akiko.v`, `akiko_hps_bridge.v`, `akiko_nvram.v`, `akiko_bus_trace.v`, `cdda.v`,
`cdtv_bridge.v`, `cdtv_hps_bridge.v`, `cdtv_nvram.v`, `cdtv_trace.v`.
`hps_ext.v` decodes the UIO address classes listed above. `CONF_STR` is `MinimigCD`.

Recent work is a memory router extraction and CDC handshaking for bridge DMA writes
into DDR3 for Z2/Z3 address ranges.

**The RTL never sources sector data itself.** Physical-disc support therefore requires
no core rebuild.
