# CD32 fidelity audit

**Date:** 2026-10-04
**Question:** where does our Akiko handling still differ from a real CD32?
**References used:** the CD32 extended ROM read off the test machine (see
`cd32-extended-rom-modules.md`), the WinUAE parity comments already carried in
`akiko_cd32.cpp`, and measurements on hardware.

Everything below is either checked against one of those or marked as unchecked.
Nothing here is an opinion about what "looks right".

## Method

The command surface is small and finite, so it can be audited exhaustively. The
ROM tells us which commands matter: scanning the extended ROM for the two forms
that build a command byte (`move.b #imm,311(a6)` and `move.w #imm,311(a6)`)
gives every site `cd.device` issues one from.

| opcode | build sites in the ROM | our handler |
|---|---|---|
| `0x02` PAUSE | 8 | `cmd_pause` |
| `0x03` UNPAUSE | 4 | `cmd_unpause` |
| `0x04` MULTI (play / read / scan TOC) | 6 | `cmd_multi` |
| `0x05` LED | 9 | `cmd_led` |
| `0x06` SUBQ | 1 | `cmd_subq` |
| `0x07` INFO | 1 | `cmd_info` |
| `0x01` STOP | **0** | `cmd_stop` — `cd.device` never issues it |

## Checked and correct

- **Response lengths.** 2 bytes for STOP/PAUSE/UNPAUSE/LED/MULTI, 15 for SUBQ,
  20 for INFO, matching WinUAE and the framer's own `expected_total_len`.
- **The INFO firmware string** is `"CHINON  O-658-2 24"`, WinUAE's exactly. The
  ROM contains no copy of it, so nothing compares it.
- **LED with bit 7 clear sends no response at all**, which is WinUAE's behaviour
  and matters: the RTL's `cmd_zero_result` agrees, and pushing two bytes there
  once put our RX buffer permanently ahead of the ROM's.
- **Opcodes `0x08`, `0x09`, `0x0a` are consumed silently** rather than answered
  with `CH_ERR_BADCOMMAND`.
- **Subcode is real.** `build_subcode_deint` builds deinterleaved Q from the disc
  for the current LBA; it is not synthesised.
- **`cd_door` pinned to 1 is correct.** The byte is a status mask whose bit 0 is
  "disk present" (AROS `chinon.h`), not a door switch. At 0 every response
  advertises no disc and the ROM halts at the splash.
- **The command framer counts the checksum.** `expected_total_len` returns 13 for
  op 4, so `REQ` is not raised a byte early.
- **The TX DMA is index-driven**, fetching only up to the `cdcomtxcmp` the guest
  publishes, and holds off three PAL scanlines after that write — WinUAE's own
  timing, not a cycle count picked by hand.
- **There is one driver.** Only `cd.device` touches the Akiko register page, so
  there is no second consumer whose state we could fall out of step with.

## Found and fixed

1. **An empty drive answered "no error" to `PLAY`/`READ`/`READ TOC`.** STOP,
   PAUSE and UNPAUSE answered `CH_ERR_NODISK | cd_door`; the multi command
   answered `0x01`. `checkdisk`, a ROM resident at priority 49, hits that branch
   on every cold boot. Fixed — but only shippable together with the re-announce,
   because `cd.device` aborts on bit 7 and then waits for a media change
   (`$E58CA4`, `$E58DCE`). Alone it measured 0/6; together 6/6, every round
   opening at the CD32 boot block. (PR #116)
2. **The commanded drive speed was parsed and ignored.** Every `PLAY`/`READ`
   carries a rate in bit 6 of `cmd[8]`; the CD32 asks for 2x 130 times in our
   logs and got about 8x. `mmcd_drive` now serves at the commanded rate and is
   the default for a config that has none. (PR #117)

## Known gaps, since closed

Three of the four were one mechanism -- the head has to get there -- and were
closed together in PR #119. `akiko_seek_us` charges nothing for a read that
continues where the last one stopped, and otherwise a settle plus a stroke
proportional to the distance, capped at a full sweep. Audio arms through the
same function and a 1x/2x change charges a spindle settle. Only `mmcd_drive`
pays any of it.

Measured in authentic mode, all three titles opening at lba 14:

| title | arms | max lba | seeks | modelled travel |
|---|---|---|---|---|
| Chuck Rock | 13 | 26505 | 13 | 2677 ms |
| Flink | 12 | 779 | 14 | 1688 ms |
| Diggers | 16 | 3204 | 16 | 2170 ms |

With `fast` selected: no seeks charged, boots as before.

**The design error is the part worth keeping.** The first version SLEPT the seek
inside the sector push, which blocks the binary's single main loop -- the one
that also services the bridge and the OSD -- for up to half a second, where a
sector interval is 6.7 ms. A drive asked for data before the head arrives does
not stall the bus; it does not answer yet. The seek is now waited out by
declining the request, the same path a paused drive uses. Anything that models
mechanical time in this core has to be written that way.

## Second pass (2026-10-04)

Checked and correct, so the next audit can skip them:

- **CD audio always plays at 1x.** The CDDA pump is paced by the FIFO's
  `cdda_req` backpressure, and that FIFO drains at the audio clock, so the
  sector-rate setting cannot make music play fast. This was the first thing
  worth ruling out.
- **The NVRAM is a real I2C slave.** `akiko_nvram.v` implements the protocol
  against a 1 KB memory, which is what a CD32 carries; `akiko.v` only models the
  SCL/SDA pin pair. The "M1 stub" comment beside the register writes is stale.
- **TOC control bits** come from the track attribute: 0x04 for data, 0x00 for
  audio, with ADR=1, and the A0/A1/A2 points take track 1's.
- **The SUBQ response layout** is a faithful Q-channel copy.

Found and fixed (PR #121):

- **The audio path never spent its seek.** `akiko_arm_seek` charged a trip for
  audio as well as data and only the data path honoured it, so the first audio
  sector went out instantly and the unspent delay then postponed a later data
  read instead. A defect in the change that closed the seek gap, found by
  re-reading it rather than by a failure.
- **SUBQ put the pre-gap in both times.** The Q channel carries running time
  within the track, which starts at 00:00:00 at index 1, and absolute time,
  where lba 0 is 00:02:00. We added 150 frames to both, so every track-relative
  position was two seconds high.

### Known and not fixed

**Pre-gap sectors are reported as the previous track, index 1.** Between the end
of one track and the start of the next -- 150 sectors on Banshee, lba 1773..1922
-- a real drive reports the NEXT track with index 0 and a relative time counting
DOWN to zero. We report the previous track at index 1 counting up. Fixing it
means teaching the track search about pre-gaps and index 0, which is more
invasive than the two above and has nothing measured behind it yet.

**The SUBQ change is unverified on hardware.** Nothing in the boot path issues
SUBQ, and driving a title into audio playback needs a pad on the machine. The
arithmetic is from the Red Book rather than a measurement -- which, after the
NODISK episode above, is exactly the kind of claim this project has learned to
label rather than trust.

### What is still not modelled

Spin-up remains a timer rather than a state: the drive reports empty for the
announce hold and then present, with no not-ready in between. The protocol has
no way to say "ready soon" -- the status byte's bit 0 is a binary disk-present
flag -- so a faithful version would have to be built out of the timing alone,
which is what the hold already is. Left as it stands deliberately.

## Superseded: the gaps as they were first listed

Ranked by how likely they are to matter.

1. **Seek time is not modelled at all.** A real drive takes up to about a second
   to cross the disc; we deliver the first sector of a new read immediately. This
   is the gap the old `cdrom_speed` comment was reserving room for. A title that
   times its loader against a seek, or that relies on the gap to finish other
   work, would see a machine faster than any that ever existed.
2. **Spin-up is a timer, not a state.** The announce hold reports the drive empty
   for 14 s and then present, with nothing in between. A real drive reports not
   ready while it focuses and reads the lead-in. We know the ROM's behaviour
   changes across this window — an early announce makes it mount the disc as a
   data volume (opening at LBA 16) instead of booting it (LBA 14) — so the shape
   of the transition is doing real work and is worth modelling properly.
3. **Speed changes are instant.** A drive asked to switch between 1x and 2x takes
   time to settle; we change the period on the next sector.
4. **Audio start latency.** CDDA begins as soon as the pump is armed.

## What would make the next one findable

Every gap above was found by comparing against something, never by inspection:
the ROM's own code, WinUAE's line-cited behaviour, or a measurement. The two that
were fixed were both invisible in normal operation and both had a comment in our
own source admitting the shortcut — `"recorded for future seek-time simulation"`
named gap 2 years before it was closed. Grepping our own hedges is a productive
place to start.
