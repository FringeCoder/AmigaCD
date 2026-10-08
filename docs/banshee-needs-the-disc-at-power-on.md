# Banshee: "not enough memory available" is authentic

**Date:** 2026-10-08
**Question:** Banshee complains `bans.exe: not enough memory available` on our
core and boots once fast RAM is added. Is that our bug?
**Answer:** no. A real CD32 does exactly the same thing under the same
conditions, measured, not argued.

## What is on the screen

Our core, shipped defaults (announce hold 0, 2 MB chip, no fast RAM), disc
mounted through `mount_cd` after the core started:

```
AmigaDOS
AMIGA ROM Operating System and Libraries
Copyright (c) 1985-1993 Commodore-Amiga, Inc.
All Rights Reserved.
bans.exe: not enough memory available
bans.exe failed returncode 10
1>
```

`output/banshee-now.png`. The giveaway is the **AmigaDOS shell**: this is not
the CD32 boot-block route at all. The ROM mounted the disc as a data volume and
ran `bans.exe` from a CLI, and the console plus its structures cost more than
Banshee's margin -- it needs 1731 KB contiguous and a 2 MB machine has about
1728 KB free (`chip-ram-shortfall.md`).

## The reference machine, both ways

Copperline, stock CD32, same ROMs, same CHD, `chip = "2M"`, `fast = "0"`:

| tray at power-on | result |
|---|---|
| disc present (`[cd] image`) | **boots** -- intro, "Sven has good reason to hate the Styx invaders" |
| disc inserted at 20 s (`--insert-cd-after 20`) | **the same two lines at the same AmigaDOS prompt** |

So the variable is not the emulator, the hold, or anything in our Akiko: it is
**whether the disc is in the tray when the machine comes out of reset.** With
the disc present the ROM boots the boot block and never builds a DOS
environment. With the disc late it mounts a volume and goes through the shell.

## Why this matters beyond Banshee

**The OSD route is the late route.** It mounts after the core has started, so
every disc mounted that way arrives late. Titles that fit do not care: Chuck
Rock reaches its loader in 13 s (lba 14, then 12) and Flink reads game data and
starts CD audio. A title with no headroom cares a great deal.

## The config route works, and it runs Banshee

`setting-ab.py`'s header says a path left in the config "does not boot at all --
0 of 3". **That is now false**, and it was measured before the unwritten-checksum
fix (PR #110) changed which path the ROM takes. On 2026-10-08, with the title
written into `hardfile[0].filename` and a plain `load_core`:

```
[akiko] serving: CD32 slot drive, tracks=4 track0_len=1773 chd=yes f=yes
```

and Banshee reaches its Core Design intro (`output/banshee-ideoff-config.png`).
It is served through the **CD32 slot drive**, not the IDE path that `mount_cd`
uses. Run twice, once with the stock-CD32 IDE bit cleared and once without: the
same screenshot, byte for byte. So the IDE bit is not what decides Banshee --
the route is.

That makes the config route the one to prefer, and it is also the authentic one.
It remains worth closing the gap properly so that an OSD-mounted disc behaves
like a disc in the tray, because that is what a user actually does.

## What not to conclude

- **Not** that the announce hold breaks Banshee. An earlier comment in
  `minimig_config.h` said so; it was wrong and has been corrected. Banshee fails
  at hold 0 and at hold 14, for the same reason, and so does the reference.
- **Not** that fast RAM is a fix to ship. It works (the user found it) because it
  moves the allocation off chip RAM entirely, and a stock CD32 has none.

## Method notes, for the next round

`mount_cd` after a `load_core` is **intermittent**: several rounds sat at
`media level -> absent` with `TOC build SKIP (no drive or no tracks)` and no
error anywhere, while the identical command typed at the same machine minutes
later mounted immediately. Rounds must assert the mount landed -- a
`TOC drip complete` in the log -- before scoring anything, or an unmounted round
is indistinguishable from a failed boot. Two separate wrong conclusions were
reached today before that check existed:

1. the CD slot in the config still held a disc, so three titles all "measured"
   that one disc -- three identical LBA walks, which is the tell;
2. clearing the slot's cfg byte as well as its filename **disables** the drive
   rather than emptying it, and `mount_cd` then has nothing to attach to.

Scoring on "lba 12 follows lba 14 or 16" is also Chuck-Rock-specific. Banshee
and Flink never read lba 12; for them the framebuffer is the only honest judge.
