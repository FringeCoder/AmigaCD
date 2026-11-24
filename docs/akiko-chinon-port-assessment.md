# The upstream Chinon Akiko rework: what to port

Written 2026-11-22, after the upstream sync of that day (userspace PR #158, core
PR FringeCoder/AmigaCD_MiSTer#73) declined the series as a whole. See
`docs/upstream-pins.md`, "Sync 2026-11-22", for why it could not be merged as
text.

The series:

- **Main_MiSTer #1401**, merged upstream as `232286e`: `fad6661` (seek times),
  `6b310ec` (drive protocol), `90e2670` (a data read clears CD audio),
  `c7715f3` (spin-up), `aa8b988` (prefetch read-ahead), `de9415b` (minimum
  seek at the faster speeds), `3b417e7` (a short first fill after a seek). All
  of it is in `support/minimig/akiko_cd32.cpp`, plus EDC/ECC in
  `ide_cdrom.cpp`.
- **Minimig-AGA** `48a35dd` (the drive's command flow and sector timing) and
  `9709669` (a queue of four sectors from the HPS, the fix for the James
  Pond 3 CDXL audio), in `rtl/akiko.v`, `hps_ext.v` and the top level.

Ours has diverged from that `akiko_cd32.cpp` by about 3000 lines, and the
core's sector path is not upstream's either. Our core paces the ships at the
rate the Main sends on `status[62:61]`. The announce hold (PR #154, PR #157)
and the command-channel fixes are what boot Chuck Rock, Theme Park and Super
Stardust. Those boots are memory-layout races measured to within a few KB of
chip RAM and a few hundred milliseconds of boot timing (see
`docs/chip-ram-shortfall.md` and `docs/banshee-needs-the-disc-at-power-on.md`).
So nothing here is ported wholesale. Each behaviour is judged on its own.

## Summary

| # | Upstream behaviour | Ours today | Matters? | Risk to the boots | Verdict |
|---|---|---|---|---|---|
| 1 | Four-sector queue in the core (`9709669`) | One sector | Barely: measured below | Medium (PBX/`sector_ready` logic, save state, fit) | **Not now.** Do 1a and 9 first |
| 1a | Ships on a fixed 75/150 Hz grid, one owed tick (`48a35dd`) | The period restarts at each ship | Yes: the only systematic error measured, 0.26 % slow | Low | **Port next**, core only |
| 2 | TOC streamed at 600 packets/s; next command held until the dump ends | 50 packets/s (20 ms), commands not held | Possibly | High: moves every boot by ~1.5 s | Branch plus a library sweep only |
| 3 | Media status `0x83`, only at power-on and on a disc change | `0x01`, at the held announce and again after INFO | Unknown | High: the announce hold is built on these pushes | Leave; one byte test at most |
| 4 | SUBQ: audio position only, Red Book track time, pregap index 0, Q CRC | Data and audio position, WinUAE's +150 track time, CRC 0 | Possibly, for titles that sync to CD audio | Low | Port, with a Copperline trace first |
| 5 | EDC/ECC on cooked (2048-byte) tracks | Zero EDC/ECC | Nothing known checks it | Low | Port when convenient |
| 6 | Real seek times at Original (253 ms to 1360 ms) | 120 ms plus a linear stroke to 500 ms | Yes, for fidelity | High: boot timing | Branch plus a library sweep only |
| 7 | Spin-up: lead-in unreadable 8.8 s after power-on, 3.5 s after a disc change | Announce hold 4 s and INFO + 1 s | Already measured | High | **Do not port.** Our hold supersedes it |
| 8 | Minimum seek of P/16 at 2x/4x/Max | No seek at 2x or Fast | For Alien Breed 3D | Low to medium | Port after 6 |
| 9 | Prefetch read-ahead from any sequential read, 3 per poll; an 8-sector first fill | Read-ahead after 16 sequential sectors and 32 into a window, 2 per sector; a 128-sector first fill | **Yes: the only stalls measured** | Low (Main only, same data) | **Port next**, Main only |
| 10 | Guest reset reported to the Main, which clears the drive state | Not reported; the Main keeps its state | Probably, for a reset during CD audio | Low to medium (core status bits) | Port as a pair, later |
| 11 | READ ends at its end address with a boundary frame; nothing outside the data track | Audio-track hold, silence past the lead-out, pre-gap streaming for DotC | Unknown | High: these guards each fixed a boot | Leave |
| 12 | PLAY restarts; an empty range is accepted; ends at the lead-out | A re-PLAY of the same range keeps its position | Unknown | Medium | Leave until a title needs it |
| 13 | A data read or negative seek clears the CD audio state (`90e2670`) | The pump stops already (sync 2026-10-30) | n/a | n/a | Already done |
| 14 | Data sectors carry their subcode | Subcode only with CD audio | Nothing known reads it | Medium (subcode slots in the queue) | Leave |
| 15 | Command flow: TX DMA keeps feeding, one command at a time, reply ~1 ms after the last byte (`48a35dd`) | Our own command-channel fixes (phantom drains, `cmd_done`) | Chuck Rock hangs on this | High | Leave; ours is measured on hardware |

## The four-sector queue: measured, and not worth it yet

### What it fixes upstream

Upstream's core held one sector, and from `48a35dd` on it shipped on a fixed
grid with one owed tick. A sector pushed later than its tick plus one period
lost a whole tick, which is 6.7 ms at 2x. Upstream's Main also did a
synchronous 128-sector window fill at every window boundary, about 65 ms
(`aa8b988`). James Pond 3's CDXL player played a wrong audio chunk when its
sectors arrived late. The queue lets the Main stay up to four sectors ahead,
so a hiccup shorter than four periods (27 ms at 2x) never reaches the guest.

### How ours differs

Ours restarts the period at each ship (`akiko.v`, PBX_IDLE: "The period runs
from this start"). A late push therefore delays that sector by exactly its
lateness, not by a whole tick, but the delay is never made back. Each late
sector moves every later sector by the same amount. Lateness accumulates as a
stream that runs slow.

### The measurement

A Main built from master `895dc968` plus a cadence counter (this PR,
`akiko_cadence_t` in `support/minimig/akiko_pace.h`). For each sector of a
sequential read it models the core's ship time as
`ship[N] = max(push[N], ship[N-1] + P)` and records how late each push was
against `ship[N-1] + P`. That is an upper bound on what the Main costs the
guest, because a guest with no free PBX slot also delays the request. An
earlier version recorded the time between push completions. That is no
measure of lateness: it varies with the poll loop's jitter while every ship
stays on time, and it reported 12 % of sectors "late".

The setup was core `_Test/fmv/amigacd.rbf` (md5 75d1fd76, the live one) on
the MiSTer, with James Pond 3 from a config-carrying `.mgl` at cd_speed Drive
and a 4 s hold. The CDXL intro is one 2x read of 5839 sectors (LBA 39752 to
45591), 39 s. Copperline runs the same disc with the same command sequence:
the intro is a single `MULTI` with `cmd[8] = 0x40`, so 2x, from MSF 08:52:04.

Per 600 sectors (4 s) of the intro, steady state:

| late by | 0 | ≤0.5 ms | ≤1 ms | ≤2 ms | ≤5 ms | more | total | worst |
|---|---|---|---|---|---|---|---|---|
| typical bucket | 560-576 | 17-37 | 4-16 | 0-2 | 0-1 | 0 | 7.5-17 ms | 0.9-3.4 ms |
| first bucket (read start) | 565 | 27 | 4 | 2 | 0 | 2 | 31 ms | 14.9 ms |
| the 1x read that follows | 599 | 0 | 0 | 0 | 0 | 1 | 45 ms | 44.9 ms |

Cross-checked from the log's own timestamps: 600 sectors take 4.008 s to
4.019 s against 4.000 s at the rate, so the stream runs 0.2-0.5 % slow, about
100 ms over the 39 s intro. The Main's own costs, from the `sec_req timing`
lines: a push is 1.81-1.84 ms and a cached read 0.5 ms, against a 6.67 ms
period. The longest gap between two polls of the bridge was 7-10 ms in steady
state.

Both events over 5 ms are prefetch window refills, from the log's `prefetch
refill` lines:

- The 14.9 ms one is at the start of the intro. The read starts at index 72
  of window 39680. Read-ahead only starts after 16 sequential sectors, at 2
  sectors per sector served. That leaves 80 of the next window's 128 sectors
  filled when the reader reaches it, and the rest is filled synchronously.
- The 44.9 ms one is a read at 1x that crosses into window 13696 13 sectors
  after it started. Read-ahead had not started, so the whole 128-sector
  window was filled synchronously.

### Decision

Do not build the queue now. What it would change, on this measurement:

- The 0.2-0.5 % drift. A queue removes it, but so does the fixed grid with an
  owed tick from `48a35dd` (row 1a). That is about fifteen lines in
  `akiko.v`, with no BRAM, no change to what `hps_sec_status` means, and no
  Main change. Almost all of our lateness is under 2 ms, well inside one
  period, so a single owed tick absorbs it.
- The rare 15-45 ms stalls. A queue covers up to 27 ms at 2x and 53 ms at 1x,
  but these come from the Main's window fills (row 9). Fixing the read-ahead
  removes them at the source, and is Main-only.
- Anything bigger: the hiccups seen elsewhere (an uncached save-state window,
  a CHD on a network share) are hundreds of milliseconds to seconds, which a
  queue of four does not cover.

The queue costs more than it saves. It changes `sector_ready`, the PBX start
and the ENABLE/STOP/MULTI flush paths, which carry the Chuck Rock and
phantom-drain fixes. `hps_sec_status` would then count queued sectors too, and
our Main reads it as the shipped count: for the LBA of a push, for SUBQ, and
in the STALL detector, which compares it with the sectors pushed. It also needs 4x the sector BRAM, and the fit is still a lottery
in places.

Revisit it if, after rows 1a and 9, the cadence line shows lateness over one
period in normal play, or a title is found that loses audio with both in.
James Pond 3's intro audio was not checked by ear on this build. That is the
one test that can overturn this, and it needs someone at the MiSTer.

## The other behaviours

**2. TOC rate.** Copperline sends the same packets (tracks first, then A0,
A1, A2, three times each). James Pond 3 is a one-track disc: 12 packets. Our
20 ms period comes from WinUAE's frame-synchronous push. At 600 packets/s a
30-track disc's dump drops from about 1.8 s to 0.15 s. That moves everything
after the TOC, which is exactly the window the Super Stardust and Theme Park
fixes were tuned in. It needs a branch and a full library sweep at the 4 s
hold (`sweep.sh`) before it can be judged. Holding commands until the dump
ends is part of the same change.

**3. Media status.** Copperline sends one `0a 83` at power-on and nothing
after INFO. Dropping our post-INFO push (`/tmp/akiko_no_postinfo`, branch
`exp/toc-spinup`) changed nothing for Theme Park on 2026-11-08. The value
`0x83` has not been tried here. Our announce hold is built around when these
pushes go out, so only the byte is a candidate, and only behind a test.

**4. SUBQ.** Ours reports `(lba - start) + 150` as the track time, which is
WinUAE's, and also answers during a data read. Upstream reports Red Book time
(no +150, counting down in the pregap with index 0), audio only, with the Q
CRC. A title that syncs to CD audio through SUBQ would see ours 2 s off. Ours
does one thing upstream does not: it reports the audible position, behind the
FIFO (`cdda_buffered_sectors`). Keep that. Before porting, take a Copperline
`RUST_LOG=copperline::akiko=trace` run of a title that polls SUBQ during CD
audio and compare the replies byte for byte.

**5. EDC/ECC.** `cdrom_read_raw_sector` builds a Mode 1 frame for 2048-byte
tracks with zero EDC/ECC. No CD32 title is known to check it. Upstream's
function is self-contained and ports cleanly. It adds CPU time per sector to
every window fill, so measure the fill time with it.

**6 and 8. Seek times.** Upstream's figures are from a real PAL CD32 timed
with Copperline's cd32-probe, so they are the best numbers there are. Ours is
two to three times faster for near seeks. Like row 2, this moves boot timing,
so it needs a branch and a sweep. Row 8 only touches 2x and Fast, which the
boot races were not measured at, and can follow.

**7. Spin-up.** Tried on 2026-11-08 as `/tmp/akiko_spinup_ms` (branch
`exp/toc-spinup`). At 8.8 s, Theme Park and Banshee each passed with the
re-announce hook in one setting and failed in the other. The hold fixes
(PR #154, PR #157) are the measured answer for our boot path.

**9. Read-ahead.** The only stalls measured above came from here. Port the
idea, not the text: start the read-ahead at the second sequential sector
rather than the sixteenth, at any index, at 3 sectors per sector served, and
on a miss inside a sequential run fill 8 sectors before serving and leave the
rest to the read-ahead. The cadence line shows the result directly.

**10. Guest reset.** Upstream's core counts resets in two status bits that
the Main watches. Ours clears the drive state only on a core load
(`akiko_cd32_init`). A reset during CD audio or an armed read leaves the Main
pumping into a core that has forgotten the read. It has not been seen to
cause a fault, but it is cheap and isolated. It needs core status bits, so
it is a userspace and core pair.

**11, 12, 14, 15.** Each of our versions was put in to fix a specific boot,
and each is documented at its site in `akiko_cd32.cpp` and `akiko.v`.
Upstream's versions were checked against a real drive, but not against those
titles. Leave them unless a title fails on ours and works upstream.

## Next steps, in order

1. Core: the fixed grid with one owed tick (row 1a), with a bench in
   `rtl/sim/akiko/tb_akiko_pbx_dma.sv` for a late push followed by an on-time
   one. Expected effect: the drift in the cadence line goes to zero.
2. Main: the read-ahead (row 9). Expected effect: no lateness over 5 ms in the
   cadence line on James Pond 3.
3. Listen to James Pond 3's intro on hardware with both.
4. Then rows 4, 5 and 10, each with its own test. Rows 2 and 6 only with a
   library sweep.
