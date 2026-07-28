# Auto-boot a physical CD inserted after core start

Status: design approved 2026-07-28. Not implemented, not compiled.

Today the physical drive only mounts when you select **Real CD** in the AmigaCD
OSD. If the tray is empty at that moment the mount fails and nothing retries —
inserting a disc afterwards does nothing until you go back into the OSD and
select the row again. This closes that gap: once the physical drive is armed,
inserting a disc mounts it and the bridges take it from there.

## Scope

In scope:

- Detect a disc appearing in the optical drive while the AmigaCD core is running.
- Mount it through the existing sentinel path.
- Report progress and failure in the OSD.

Out of scope, and deliberately so:

- Watching the drive when the physical drive has not been armed. Arming stays an
  explicit user action.
- Detecting a disc being *removed* while mounted, or hot-swapping one disc for
  another. That is item 12 of `docs/integration-plan.md` and needs the Akiko
  mediachange path rewired; the bridges will keep serving the stale TOC until
  you eject from the OSD.
- Any change to `support/physical_disc/physical_disc.{cpp,h}` or
  `physical_disc_acoustic.{cpp,h}`. Those are a verbatim import of
  `Anime0t4ku/Main_MiSTer_Physical_Disc @ adf39fb` and keeping them
  byte-identical is what makes re-basing onto a newer upstream cheap.

## Prior art, and why none of it is reused

Upstream does **not** do this. `physical_disc_launch_startup()` retries the
mount for an ~8 s window at core load and then gives up; `physical_disc_launch_poll()`
goes idle and never looks again. The `physical_disc_watch_start()` /
`physical_disc_poll_event()` machinery inside `physical_disc.cpp` that *would*
do continuous watching has zero callers anywhere, including in upstream's own
tree. This is new work, not a port.

Watch mode was evaluated and rejected on three counts: it holds the device open
for the whole wait, `drv.watching` disables the prefetch lane in the ring worker
(`physical_disc.cpp:421-484`), and `physical_disc_watch_stop()` calls
`physical_disc_close()` — so it tears the drive down and re-spins it immediately
before the mount it just triggered. It also runs a full `physical_disc_identify()`
ISO9660 PVD read per insert that we do not need, since `load_physical_disc`
reads the TOC anyway.

Retrying `ide_open(0, PHYSICAL_DISC_SENTINEL)` on a bare timer was also
rejected: each failed attempt runs `physical_disc_open` →
`malloc(RING_SECTORS × ENTRY_SIZE)` ≈ 10 MB plus a `pthread_create`, then
`physical_disc_close` → join and free. Every two seconds, for as long as the
tray is empty.

## Architecture

One new unit, `support/physical_disc/physical_disc_autoboot.{cpp,h}`, picked up
automatically by the Makefile's `$(wildcard ./support/*/*.cpp)`. It mirrors
upstream's own layout, where the launch policy lives in `physical_disc_launch.cpp`
beside the driver rather than inside it.

Public surface is one function:

```c
void physical_disc_autoboot_poll(void);
```

Everything else is file-static: the poll timer, the consecutive-failure counter,
the give-up latch, and the last observed tray state. Dependencies are
`<linux/cdrom.h>`, `ide.h` (for `ide_inst` / `ide_open`), `minimig_config.h`
(for `hardfile[0].filename`) and `physical_disc.h` (for the sentinel only).

### State comes from what already exists

No new config key and no new menu row.

| Concept | Source of truth |
|---|---|
| Armed | `minimig_config.hardfile[0].filename == PHYSICAL_DISC_SENTINEL` |
| Mounted | `ide_inst[0].drive[0].phys` |

Arming already persists through **Save settings** and restores on the next core
load via `ApplyConfiguration → hdd_open(0) → ide_open`. BACKSPACE on the CD row
clears the filename, which disarms. Selecting **Real CD** re-arms.

### Call site

From `user_io.cpp:3132`, alongside `akiko_cd32_poll()` and `cdtv_cd_poll()`, so
it runs only on the Minimig branch of `user_io_poll()`.

The probe must never block the poll loop. `user_io_poll()` already stalls behind
synchronous `SG_IO` reads on a cache miss (see "Blocking I/O in the main loop"
in `docs/integration-plan.md`); this must not add a second source of that.
`open(O_NONBLOCK)` plus `CDROM_DRIVE_STATUS` on a closed device is a sub-millisecond
kernel-state query, not a media access.

## Flow

1. Return immediately unless `is_amigacd()`, armed, and not mounted. In the
   common case this costs one `strcmp` per poll.
2. Every 2 s, probe: `open("/dev/sr0".."/dev/sr7", O_RDONLY|O_NONBLOCK|O_CLOEXEC)`,
   `ioctl(CDROM_DRIVE_STATUS, CDSL_CURRENT)`, `close`. No allocation, no thread,
   drive not held open.
3. On the first `CDS_DISC_OK`, wait one further 2 s tick before mounting, so a
   freshly inserted disc has spun up enough for `CDROMREADTOCHDR` to succeed.
   (Upstream burns an 8 s busy-loop for the same reason. One extra tick is
   cheaper and we can retry, which upstream cannot.)
4. Mount by calling `ide_open(0, PHYSICAL_DISC_SENTINEL)` — the identical call
   the OSD row makes, so there is one mount path, not two. On success,
   `Info("Physical CD mounted", 1500)`; `phys` is now 1, so step 1 stops the
   polling on the next call.
5. On failure, increment the consecutive-failure counter and retry on the next
   tick. After 5 consecutive failures, latch off and `Info("Disc could not be read", 3000)`.
6. The latch clears when the probe reports no disc — that is, you took the bad
   disc out. Re-inserting starts a fresh attempt with the counter reset.

## Booting the Amiga

The new unit mounts and stops. Making the Amiga boot the disc is left to the
existing bridges.

**CD32.** `cd_is_mounted()` flips, `akiko_cd32_poll()`'s mediachange edge fires
(`akiko_cd32.cpp:2569`), and with `cd_initialized >= 2` and no prior eject in the
session the Phase 33-E path sets `g_akiko_pending_minimig_reset`, which issues
`minimig_reset()` on the following poll. That reset re-runs `ApplyConfiguration`
with the drive already mounted, so the post-reset boot sees `mounted=1` from
t=0. This is exactly the "boot with no CD, then mount" case Phase 33-E was
written for, so auto-boot should work with no new Amiga-side code.

**CDTV.** `cdrom_parse` calls `cdtv_cd_set_cd_path(PHYSICAL_DISC_SENTINEL)`,
which sets `cd_media = 1` and re-arms the STCH inject (`cdtv_cd.cpp:1109`,
`:1152`). Whether that alone gets the CDTV BIOS to boot a mid-session insert is
**unverified** — the CDTV bridge has no equivalent of Phase 33-E. If it does not,
the fallback is a manual Reset from the OSD, and forcing `minimig_reset()` from
the mount site becomes a follow-up change. **This is the first thing to check on
hardware.**

## OSD

The Real CD row currently renders `mount drive` when unmounted and `re-read TOC`
when mounted. It gains a third state so the console says what it is doing:

| Condition | Row text |
|---|---|
| Not armed | `mount drive` |
| Armed, waiting for a disc | `waiting for disc...` |
| Armed, give-up latch set | `disc unreadable` |
| Mounted | `re-read TOC` |

Selecting the row while waiting forces an immediate attempt and clears the
give-up latch, rather than waiting for the next tick.

## Failure modes

- **No optical drive attached.** Indistinguishable from an empty tray at the
  probe level, so the core waits quietly forever. That is the right behaviour
  for a MiSTer with nothing plugged in.
- **Disc present but unreadable.** Five attempts over ~10 s, then the latch and
  a toast. No further drive access until the tray state changes.
- **Disc removed while mounted.** Not detected — out of scope, see above.
- **Drive disappears mid-session** (USB unplug). The probe simply stops finding
  it and the core falls back to waiting.

## Testing

There is no build toolchain on this machine — `wsl -l -v` reports no
distributions installed — so this cannot be compiled or run here. Verification
is on hardware, via `scripts/wsl-build.sh deploy` from a Linux box.

Hardware checks, in order:

1. **CD32, armed, empty tray at core load.** Insert a CD32 disc. Expect: mount
   toast, then a reset, then the game boots.
2. **CDTV, same.** This is the uncertain one. If it mounts but does not boot,
   record whether a manual OSD Reset boots it — that distinguishes "needs a
   reset" from "the mount itself is wrong".
3. **Unreadable disc.** Expect five attempts, then the toast, then silence.
   Remove and re-insert a good disc; expect a fresh attempt.
4. **Armed, disc already in the tray at core load.** The existing startup mount
   should succeed and the poller should never run. Confirm no regression.
5. **Not armed.** Insert a disc. Expect nothing at all — no drive access, no
   spin-up.

`/tmp/physical_disc_stats.log` gives hit rate and worst-case I/O latency for the
mounted case.

## Relationship to the integration plan

This is a narrower version of item 9's "auto-launch" ambition in
`docs/integration-plan.md`, restricted to the insert-while-armed case and
without `physical_disc_launch.cpp`. It does not touch items 10 (disc
identification), 11 (per-game NVRAM) or 12 (hot swap), and does not block them.
