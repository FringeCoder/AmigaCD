# Round 4: a failure found, measured and fixed — and the two questions it leaves

Round 3 (below, still unanswered — the watch may have stopped) reported that the
sector-count lead was an artifact and that two A/Bs were clean. Since then we
found a failure that reproduces, measured its boundary, fixed it, and verified
the fix. This round is mostly a report; there are two questions at the end, and
the round-3 questions still stand.

## The finding

**The first media-present announce was landing too early, and at the shipped
value the CD32 ROM never issues its boot read.** The announce is deliberately
held at cold boot, because announcing at t=0 makes the ROM allocate its CD
structures in an order that fragments chip RAM. The hold was 8000 ms, measured
against *that* failure (6 s failed, 8 s worked).

Pooled across two independent mount routes — the disc in the config so the core
mounts it on load, and a `mount_cd` test hook at a controlled delay:

| announce at | booted |
|---|---|
| 0 s | 0/2 |
| **8 s** | **1/8** ← the shipped default |
| 10 s | 1/2 |
| 12, 14, 16, 26 s | **11/11** |

1 of 8 against 11 of 11, p < 0.001 (Fisher). Raised the hold to 14 s; the two
arrival times that were 0/2 each are now **3/3 each** with the fix deployed. Both
failure modes want the announce later, so this moves away from both edges rather
than trading one for the other.

## What the ROM does, which is where our understanding stops

Identical in booting and failing rounds up to the moment of failure:

```
[akiko] CMD op=0x07 n=2  bytes=27 d8                        <- INFO, ~4.4 s in
[akiko] CMD op=0x04 n=13 bytes=44 00 00 00 00 00 00 03 ...  <- PLAY, cmd[7]=0x03
[akiko] media level -> present                              <- our announce
[akiko] CMD op=0x04 n=13 bytes=84 00 00 00 00 00 00 03 ...  <- PLAY, cmd[7]=0x03
                                                            --- failing: nothing more
[akiko] CMD op=0x04 n=13 bytes=84 00 02 14 06 00 64 80 ...  <- booting: the DATA read
[akiko] PLAY DATA arm: start_lba=14 (cmd7=0x80)
```

Our host treats `cmd[7] & 0x80` as "this is a data read"; the two frames with
`cmd[7]=0x03` are not data reads and we answer them as plays. **In a failing
round the third frame never comes.** Nothing is refused on our side — the ROM
simply stops asking, with no timeout, no retry and no error frame.

**Question 1. What are those two `cmd[7]=0x03` PLAY frames, and what does the ROM
do between them and its first data read?** All six MSF bytes are zero and the
flag byte is 3. If that is a seek-to-zero or a spin-up probe, what does the ROM
expect back before it will proceed to read LBA 14? We answer with a status frame
and it proceeds in some rounds and not others, which suggests we are answering
something the ROM treats as conditional. A description of the ROM's
startup-to-first-read sequence would let us answer it correctly rather than
tuning a timer away from the cliff.

**Question 2. Why would a LATER announce be more reliable than an earlier one?**
Our fix works and we cannot explain it. Intuitively a disc that is already there
should be easier than one that arrives mid-init. The pattern suggests the ROM
only accepts a media-present transition after some point in its own startup, and
an announce before that point is consumed without arming anything. Is there such
a point — a device initialisation, a `cd.device` open, an interrupt being enabled
— and is there a state we could read instead of waiting out a timer?

---

# Round 3 (unanswered): the sector-count lead is dead, two A/Bs are clean

## The 20-versus-24 lead was an artifact. Please drop it.

We reported that failing rounds take about twenty sectors before the BIOS pauses
and booting rounds at least twenty-four, and asked what makes the BIOS stop at
twenty. **That difference does not exist.** The trace was capped at 24 lines per
read, so every booting round reported the cap, and we were comparing a cap
against a real number.

With an exact count taken at the moment of the pause, sixteen consecutive
**booting** rounds give the whole profile:

```
first read pauses at: 21 22 22 22 22 22 22 22 23 25 25 25 25 26
then:                 18, 21-22, 8, 6-7, 20-21, 1, 1-2, 4, 4, 4, 258, 189
```

A boot is **thirteen reads**, not the six we had been looking at; a pause after
about twenty sectors is the sixth read of *every* successful boot; and the first
read's count varies from 21 to 26 with no effect on anything.

## Two A/Bs, both clean, both interleaved

Eight rounds an arm, alternating within the run, one binary and one bitstream:

| | | |
|---|---|---|
| sector rate | `mmcd_fast` (588/s) **8/8** | `mmcd_2x` (150/s) **8/8** |
| the flash itself | rewrite the `.rbf`, then load **8/8** | load the same file **8/8** |

The rate mattered because the test machine's profile had been left at 2x with the
config written the day after the failure stopped reproducing — a four-times
change on the sector path that was not in any log, which is why it is now logged
once per core load. The flash arm mattered because the failure is always
described as "first boot after a flash", and a plain `load_core` is not that.
Both arms verified by md5 to load identical bitstreams.

## Your three fixes were all already in the tree

Said plainly so no further effort goes there. Verified in source:

- **drop the all-zero phantom frame** — present, citing the same hardware log you
  quoted (`CHECKSUM FAIL ... 00 00`, then `ea 27 d8`, 2026-09-26).
- **resync a frame arriving behind debris** — present; it slides forward a byte at
  a time and takes the first correctly summed frame for its own opcode, which is
  exactly how `27 d8` is recovered from `ea 27 d8`.
- **RTL: do not assert the request until payload and checksum are valid** —
  present. `cmd_pending` is
  `(cdrom_command_length != 0) && ((cdrom_command_length >= cmd_total) || (cdrom_command_length == 32))`
  with `cmd_total = expected_total_len(cmd_op)`.

The phantom drains and the single `CHECKSUM FAIL` appear once per round in
**every** round, including all 32 that booted, so they are not discriminators.

## Still open from round 3

**Is a thirteen-read profile with those counts what a real CD32 does for this
title?** 21-26, 18, 21-22, 8, 6-7, 20-21, 1, 1-2, 4, 4, 4, 258, 189. If a real
machine or WinUAE reads this disc in a different number of chunks, the difference
is a clue about our PBX slot accounting rather than about any single read. If it
matches, we can stop looking at read boundaries altogether.

## How rounds are measured now, so numbers are comparable

- the exact sector count at the pause, and a per-read total on each arm line
- the sector rate and mount delay recorded once per core load, because a round
  whose settings are unknown is not evidence
- no OSD at any point: config write, `load_core`, `mount_cd`, score from the log.
  About 100 seconds a round against four to six minutes.
