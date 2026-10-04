# Round 6: solved, and your two-consumer idea was right in effect and wrong in mechanism

The stall is fixed. Your round 5 answer arrived before the measurement that found
it, so two of its three main claims are falsified — and the third led somewhere
useful. All of it below, with the evidence.

## The bug: a command frame read one byte too early

Every stalled boot ends with the same frame, 14 rounds out of 14:

```
CHECKSUM FAIL n=13 bytes=44 00 00 00 00 00 00 03 00 00 00 00 00
BAD cmd=44 err=88
```

against a valid `44 00 00 00 00 00 00 03 00 00 00 00 b8`. That is the `READ TOC`
you identified, with **only its checksum byte missing**. We answered
`CH_ERR_CHECKSUM`, and the ROM stopped talking to us for ever.

Taking such a frame — trailing byte zero, and the implied checksum would make it
valid — gives, at the 8 s announce that used to fail:

| | reaches the boot block | completes the boot |
|---|---|---|
| before | 0/8 | 0/8 |
| **after** | **8/8** | **8/8** |

16 of 16 with the re-announce hook both on and off, against 1 of 8 before.

## What that falsifies in round 5

**"The LBA 25622 stall is the removal edge invalidating the mounted volume, so
the loader's next access fails with ERROR_DEVICE_NOT_MOUNTED."** No. Rounds that
use the removal-then-arrival pair now boot 8 of 8 with no other change. The
removal edge does not break the volume.

**"The silence after a completed TOC drip is `cd.device` asleep, its change
interrupt never having fired."** No. The silence was our own error answer to the
frame above. The ROM was awake and asking; we were refusing.

**"At 8 s the arrival edge is premature; 14 s is the hardware-accurate
solution."** Partly. The hold still matters for chip RAM, but after this fix the
announce timing stops mattering for the boot: 0, 1, 2, 4, 8 and 14 second holds
all boot.

## Where an early announce really leads, and a correction of our own

An early announce makes the ROM open at **LBA 16**, the ISO primary volume
descriptor, instead of **LBA 14**, the CD32 boot block. Every read shifts:

```
boot-block path:  14, 19, 17, 25621, 26505, 25622, 12, ...
data-mount path:  16, 21, 19, 25623, 26507, 25624, 12, ...
```

All three titles tested do it, so it is the ROM and not one disc. **It is not a
failure** — that route reaches the loader at LBA 12 and the game data just the
same. We had reported "a 0 ms announce never boots, 0 of 10"; that was our own
harness scoring a boot as "LBA 14 and then LBA 12", which the data-mount path can
never satisfy. Withdrawn.

## Your two-consumer idea: right effect, wrong mechanism, and we checked

You proposed two independent consumers — a boot-ROM animation talking to Akiko
directly, and `cd.device` — each issuing its own scan. We read the extended ROM
off the machine rather than reasoning about it. At base `$E00000` its residents
are:

```
checkdisk (49)  cdstrap (-58)  cdui.library  reqzapper (-50)  cd.device (8)
cdfs.library (2)  player.library  nonvolatile.library  lowlevel.library
freeanim.library  cdg.library  debox.library  kludges  cdtv.device (8)
bookmark.device  playerprefs.library
```

**Only `cd.device` contains instruction-level references to the Akiko register
page.** Filtering `$00B8xxxx` constants down to genuine 68k operands leaves six
hits — `$B80000` and `$B80008` — all inside `cd.device`. (A loose byte scan also
"finds" `cdui.library` 24 times; that is animation data.)

So there is one driver. The animation, the player, the strap and `checkdisk` are
its clients. Two scans per boot is real and normal — **two clients of one
driver**: `checkdisk` at priority 49 probes before anything else, while the drive
still reports empty, and `cdstrap`/`cdfs` scan again after the media change. That
matches our logs exactly.

## The question this leaves, and it is a fidelity question

Following your lead to `checkdisk` turned up something in our own code. Three
commands answer a probe of an empty drive the way WinUAE's `checkerr()` does:

```c
r[1] = CH_ERR_NODISK | cd_door;     // STOP, PAUSE, UNPAUSE   -> 0xf9
```

but the multi command — `PLAY`, `READ`, and the `READ TOC` scan — does not:

```c
if (!cd_media_present()) { r[1] = 0x01; ... }   // door closed, NO error
```

That `0x01` is inherited code, not a considered choice, and it is what
`checkdisk` gets from us when it probes during the announce hold: "fine".

**Question: what should a real drive answer to a `READ TOC` (op 0x04, seekpos <
0) when the tray holds no disc?** We assume `CH_ERR_NODISK | door`, matching the
other three commands and `checkerr()`. If that is right, we have been telling the
earliest-running ROM module that an empty drive is fine, for every boot — and we
would like to know whether that is what steers it onto the LBA 16 path later,
since the two events are in the same window.

We are about to test it. If you have a reason to expect a different answer for
the scan case specifically — a drive reporting `CH_ERR_NODISK` to PAUSE but
something else to a TOC scan — that would be worth knowing before we measure.
