# Running an AmigaOS CD that will not boot on a CD32

Simon the Sorcerer II does not boot on this core, and should not: the disc is not
a CD32 title. This is how to tell, and how to run such a disc anyway.

## Telling a data CD from a CD32 title

Measured on the drive the Amiga actually reads (`serving: ... track0_len=86170`,
so the right disc), with Simon the Sorcerer II mounted and the machine reset:

```
[akiko] serving: CD32 slot drive, tracks=2 track0_len=86170 chd=no f=yes
[akiko] root dir: LBA 18, 1 of 1 sectors, 12 entries, CD32.TM=0 CDTV.TM=0
[akiko] root dir: 10 shown: AHI/ AHI.info CD data/ Simon2Install Simon2Install.info
                            SimonII SimonII.info TheFinalWord ...
arms=0  lba14=0  loader(lba12)=0
```

**The ROM issues no sector read at all.** With a bootable title it reads lba 14
within seconds of the announce, every time. So the BIOS looks at the disc, finds
nothing it can start, and stops -- which is the correct behaviour for an AmigaOS
CD-ROM, not a defect.

The root directory says the same thing plainly: `Simon2Install` is a Workbench
installer, `AHI` is the AmigaOS audio driver, `BGUI`/`bgui.library` is an AmigaOS
GUI toolkit, and every entry has a matching `.info` icon. This is a disc you
install from Workbench.

Two things that do NOT distinguish a bootable disc, both tried and both wrong:

- **boot code in the ISO9660 system area.** Sectors 0-15 are entirely zero on
  Chuck Rock, which boots, exactly as they are on this disc. A check built on
  that flagged every disc including the one in front of it.
- **the `CD32.TM` trademark file.** Chuck Rock has no `CD32.TM` either. Its root
  is `CHUCK`, `CHUCK.A`, `CHUCK.B` and `S/`.

The `S/` directory -- the Amiga startup-sequence directory -- is present on Chuck
Rock and absent here, which is the obvious next hypothesis. It is written down as
a hypothesis and not used as a test, because it rests on one bootable title.

## Running it: System = CD32+, plus a Workbench hardfile

On the **CD32** profile the Floppy and Hard Disk rows are greyed and the cursor
cannot reach them, so there is nowhere to put an AmigaOS system. The profile list
and what each allows:

| System | Hard Disk | Floppy |
|---|---|---|
| CD32 | greyed | greyed |
| **CD32+** | **selectable** | **selectable** |
| CDTV | selectable | greyed |
| A500 | selectable | selectable |
| A1200 | selectable | selectable |

`CD32+` is the one to use: it keeps the CD32 ROM and its CD filesystem while
allowing a hardfile, which is exactly the combination an AmigaOS CD needs.

Steps, all from the OSD:

1. **System** -- press ENTER on the row to cycle it to `CD32+`.
2. **Hard Disk 0** -- pick a bootable AmigaOS image. `AmigaVision.hdf` works and
   is already on this card (it attaches `AmigaVision-Saves.hdf` to Hard Disk 1 by
   itself). The `CD32Emu.hdf` / `CD32.hdf` images in `games/AmigaCD32` are also
   bootable Workbench volumes -- `DOS\03` FFS with SetPatch, LoadWB, a
   startup-sequence and `$VER: CD32-Emulator 107.18 (18.08.99)`.
3. **CD** -- mount the disc LAST. Switching the System profile applies a preset
   that brings its own disc with it, so a disc mounted before the switch is
   replaced.
4. **Reset.**

AmigaVision boots in well under 90 s and is keyboard-driven (UP/DOWN navigate,
ENTER selects, ESCAPE quits to Workbench). On the Workbench that follows, the
disc is mounted as a normal volume -- `SIMONII` appears on the desktop next to
`Data`, `Amiga`, `MiSTer` and a `Shell` icon -- so `Simon2Install` or `SimonII`
can be run from it like any other AmigaOS program.

Verified end to end on hardware 2026-10-03; screenshots of the AmigaVision menu
and of the Workbench with the disc mounted were taken at that time.

**The remaining gap is input, not the core.** Launching an icon needs a mouse,
and the remote control here provides a keyboard and a pad but no mouse, so the
last double-click has to happen at the machine. Anyone wanting it hands-free can
put the launch into the hardfile's `s/startup-sequence` instead; that needs an
Amiga filesystem writer (`amitools`' `xdftool` handles `DOS\03` images) and
should be done on a COPY of the hardfile.

## For the OSD

Nothing here needs a code change. If one is ever wanted, the useful one is a
message when a mounted disc has no boot code, so an endless splash is
distinguishable from a broken core. The host already reads and logs the root
directory once per disc for exactly this reason; what it lacks is a sound test
for bootability, and the two obvious candidates are falsified above.
