# Round 3: the sector-count lead is dead, two A/Bs are clean, and one failure reproduces 3 of 3

The previous round asked what governs the sector count before `PAUSE`, what
happens during the pause, and what condition would reproduce the failure. We
built the instrument, ran four experiments, and the answer to the first two is
"nothing is wrong there". The third has produced a reproducible failure, but not
the one we were hunting.

## The 20-versus-24 lead was an artifact. Please drop it.

We reported that failing rounds take about twenty sectors before the BIOS pauses
and booting rounds at least twenty-four, and asked what makes the BIOS stop at
twenty. **That difference does not exist.** It was an artifact of the instrument:
the trace was capped at 24 lines per read, so every booting round reported the
cap, and we were comparing a cap against a real number.

We replaced it with an exact count taken at the moment of the pause
(`cd_sec_pushed`, reported from `cmd_pause()`), and a per-read total on each arm
line. Sixteen consecutive **booting** rounds now give the whole profile:

```
first read pauses at: 21, 22, 22, 22, 22, 22, 22, 22, 23, 25, 25, 25, 25, 26
then:                 18, 21-22, 8, 6-7, 20-21, 1, 1-2, 4, 4, 4, 258, 189
```

Three things follow:

1. **A boot is thirteen reads, not six.** We had been looking at a prefix.
2. **A pause after about twenty sectors is normal.** It is the sixth read in
   every booting round, and the first read pauses anywhere from 21 to 26.
3. **The count varies round to round with no effect on the boot.** 22 and 25 both
   boot, repeatedly, on identical bytes.

So the buffer-split theory as a *failure* mechanism has nothing to attach to. The
underlying mechanism may still be right -- `cd.device` pausing at the end of a
transfer, the count set by the buffer it got -- but a short first read is not what
distinguishes a failure from a boot.

## Two A/Bs, both clean, both interleaved

Each is eight rounds an arm, alternating within the run, same binary, same
bitstream, same disc:

| | | |
|---|---|---|
| sector rate | `mmcd_fast` (588/s) **8/8** | `mmcd_2x` (150/s) **8/8** |
| the flash itself | rewrite the `.rbf`, then load **8/8** | load the same file **8/8** |

The rate mattered because the test machine's profile had been left at 2x with the
config written the day after the failure stopped reproducing -- a four-times
change on the sector path that was not in the log, which is why we added it
there. It is not the explanation.

The flash arm mattered because the failure was always reported as "first boot
after a flash", and a plain `load_core` is not that: the file is already on the
card. Writing the same bytes back and syncing before the load changes nothing.
Verified by md5 that both arms load identical bitstreams.

That is 32 consecutive boots today. The failure we have been chasing did not
appear once.

## What DOES reproduce, 3 of 3

While building an OSD-free round loop we tried the obvious thing: put the disc
**path in the config** so the core mounts it on load, making the bay occupied
from power-on. **It never boots.** Three of three, at `cd_mount_delay=0` and at
`8`. The mount itself is fine -- the TOC is built, and our own ISO walk reads the
root directory and lists `CHUCK`, `CHUCK.A`, `CHUCK.B`:

```
[akiko] boot settle: holding first media announce 8000 ms
[akiko] media level -> absent (cd_init=0, announce pending)
[akiko] CMD op=0x07 n=2 bytes=27 d8            <- INFO, 4.4 s in
[akiko] CMD op=0x04 n=13 bytes=44 00 ...       <- the guest arms a read ALREADY
[akiko] CMD op=0x03 n=2 bytes=53 ac            <- UNPAUSE
[akiko] media level -> present (cd_init=2, announce pending)   <- 8 s, the hold
[akiko] CMD op=0x04 n=13 bytes=84 00 ...       <- arms again
[akiko] CMD op=0x03 n=2 bytes=b3 4c
[akiko] root dir: LBA 22, 6 entries ... CHUCK CHUCK.A CHUCK.B
(nothing further: no sector request, ever)
```

The same disc, same binary, same bitstream boots 32 times out of 32 when it
arrives **after** the boot starts (`mount_cd`, which is the boot-with-no-CD-then-
mount path the OSD produces). So:

**Question 1. What does the CD32 ROM do differently when the bay is occupied at
power-on, versus when a media-change arrives a few seconds in?** We can see the
guest issue `PLAY DATA` (op 0x04) twice, before and after our media announce, and
then stop asking. Our host never arms a read for either -- which is consistent
with the guest's own flags not being set yet -- and the guest never retries. Is
there a handshake we are failing to answer for a disc that was *already there*,
or an announce we owe the ROM before it will accept an arm? This is the one
failure we can reproduce on demand, so it is where we can actually test an
answer.

## Your three fixes were all already in the tree

Said plainly so the next round does not spend effort there. All three had been
implemented before your answer, and we verified each in the source:

- **host, drop the all-zero phantom frame.** Present, and it cites the same
  hardware log you quoted (`CHECKSUM FAIL ... 00 00` then `ea 27 d8`,
  2026-09-26).
- **host, resync a frame arriving behind debris.** Present: it slides forward a
  byte at a time and takes the first correctly summed frame for its own opcode,
  which recovers exactly the `27 d8` INFO from `ea 27 d8`.
- **RTL, do not assert the request until the payload and checksum are valid.**
  Present. `cmd_pending` in `akiko.v` is
  `(cdrom_command_length != 0) && ((cdrom_command_length >= cmd_total) || (cdrom_command_length == 32))`,
  where `cmd_total` is `expected_total_len(cmd_op)` -- the length gate you
  described, with the 32-byte buffer-full case as the escape hatch.

The phantom drains and the single `CHECKSUM FAIL` still appear once per round in
**every** round, including all 32 that booted, so they are not the discriminator
either.

## The other open question

**Question 2. Is a thirteen-read profile with those counts what a real CD32 does
for this title?** 21-26, 18, 21-22, 8, 6-7, 20-21, 1, 1-2, 4, 4, 4, 258, 189.
If a real machine (or WinUAE) reads this disc in a different number of chunks,
the difference is a clue about our PBX slot accounting rather than about any
single read. If it matches, we can stop looking at read boundaries altogether.

## What changed in the instrument, so numbers are comparable from here

- the exact sector count at the pause, and a per-read total (`prev=`) on each arm
- the sector rate and mount delay recorded once per core load, because a round
  whose settings are unknown cannot be used as evidence
- rounds now run without touching the OSD at all: config write, `load_core`,
  `mount_cd`, score from the log. About 100 seconds a round against four to six
  minutes, and none of the OSD's failure modes.
