# The first boot after a core flash fails, and what it is not

Reproducible on demand: flash the core, mount a disc, boot once. That round fails.
Reset and mount again and it boots in about five seconds, every time.

Measured 0 of 9 on the 2026-10-01 netlist and again on current mainline
(`6b4486e`, which carries the `minimig_idle` cone split, the `tx_busy` arbiter
gate and the registered DDR3 command boundary):

```
round 1  booted=False  arms=6   lines=161
round 2  booted=True   in 5s    arms=13
round 3  booted=True   in 5s    arms=13
```

## What it looks like

The failing round is **a black screen**, not a stalled splash. The working round
at the same point shows Chuck Rock's intro text. So the machine is not waiting for
something; it has stopped.

It stops *after* reading the disc. Both cases issue the same six `PLAY DATA arm`
bases in the same order — 14, 19, 17, 25621, 26505, 25622 — and the failing one
simply does not continue to the seventh, which in a working boot is lba 12, the
loader. So the loader runs and dies.

## What has been excluded, by measurement

| candidate | how it was excluded |
|---|---|
| the Akiko command traffic | identical counts of INFO (1), TOC rebuilds (4), media-status pushes (3), `CHECKSUM FAIL` (1) and `BAD cmd` (1) in failing and working rounds |
| the corrupted command frame | occurs in the rounds that **succeed** |
| the drive object | every round reports `serving: CD32 slot drive, tracks=2 track0_len=26914 chd=yes f=yes` |
| per-game NVRAM not reloading | an uncleaned trace of a failing round shows `set_cd_path`, the deferred load armed, `init: scheduling reload` and `NVR loaded (1024 bytes)` |
| stale host state across the reflash | `fpga_load_rbf()` ends in `app_restart()`; the PID changes (2569 → 6716, measured) |
| the "60-sector walk" | a `static` trace counter capped per **process**, and `load_core` restarts the process |
| the 8 s cold-boot mount delay | with the delay saved as Off — verified from the core's own `boot settle: holding first media announce 0 ms` — the failure persists |
| a reset between flash and mount | the reset itself boots the game, so that arm measures the reset rather than the mount; the rounds that follow behave as normal rounds |
| the three merged RTL fixes | the failure is unchanged on `6b4486e` |

## Three measurement artifacts, recorded so they are not re-derived

Each of these looked like a finding and two reached a document before being caught:

1. **A cleared log.** The harness clears the Akiko log at the start of a round, and
   the clear landed *after* `set_cd_path` and the NVR load had run. Their absence
   read as "the NVRAM never reloads".
2. **An over-wide window.** `CHECKSUM FAIL` counts of 5 and 8 came from an
   uncleaned 75-second capture spanning the flash, several OSD interactions and
   more than one boot attempt. Scored per round it is **1**, in failing and
   working rounds alike.
3. **A process-lifetime budget.** `DBG sec_req#` was capped at 60 for the life of
   the binary; `load_core` re-execs it, so the first boot after a flash printed 60
   lines and every later boot printed none.

The common lesson, and the one worth keeping: before believing a difference an
instrument reports, check the instrument's **window** and its **budget**.

## Where to look next

The loader runs and the machine goes black, which points at the guest rather than
at the CD channel — the Akiko side is now identical between the two cases on every
counter available. The project's existing note on this title says the loader
destroys itself after a mis-parsed header and points at PBX slot placement; that
is the thread to pull, and it wants guest-side state (the save-state peek path)
rather than more log reading.

Worth knowing while hunting: a `sec_req but no data read armed` runaway has been
seen once, 17,049 times in one round — the core holding `sec_req` asserted while
the host declines because `cd_data_lba_base` is -1. That round **booted**, so it
is a separate defect. It is now throttled and counted rather than logged
unconditionally on the sector path.
