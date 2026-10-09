# Launching a game directly: `.mgl` files

An `.mgl` is a small XML file that MiSTer's own menu treats as a launchable
item. Selecting it loads a core **and mounts media into it**, with no OSD
navigation. It is how every other MiSTer CD core is launched from a game list,
a favourites folder or `bootcore`.

This core did not support them at all until 2026-09-16: `MENU_MINIMIG_MAIN1`
has had `.mgl` handling since floppies, but it jumps straight to the AmigaCD
console OSD and returned before reaching it, so an `.mgl` opened an invisible
menu and mounted nothing. It now works, and understands the CD and the hard
disks as well as the floppies.

## Let the OSD write it

With a disc image mounted, select **Write .mgl for this disc** on the
Advanced page. That writes an `.mgl` beside the disc, named after it, and toasts the
filename.

**The file carries the whole machine.** Every setting the OSD saves -- system,
CPU, chip/fast RAM, Kickstart and extended ROM, the disc, the hard disks,
the user port, CD speed, CD32 audio buffer, the save state slot, all of it --
goes into an `<amigacd_config>` block in the file. Launching it gives that
machine regardless of what the startup config on the card says, and leaves
the startup config untouched.

**The disc is in the drive at power-on.** The file names the disc in its
settings, not as a `<file index="4">` mounted a couple of seconds after the core
starts. That is the authentic route, and for some titles the only one that
works: a disc that arrives late sends the CD32 ROM down a path that costs chip
RAM, and Banshee and Super Stardust then stop with "not enough memory" -- on a
real CD32 too. Mounted floppies are written as ordinary `<file>` items.

It is also worth preferring over writing one by hand because the OSD knows
which core is *running*. `<rbf>` is filled from `get_rbf_dir()` and
`get_rbf_name()`, so it names the exact build you are using -- which is the part
of a hand-written file that goes stale at the next deploy (see below). After a
deploy, press the row again and the file is correct once more.

**An existing file is edited, not replaced.** The `<rbf>` text and the
`<amigacd_config>` block are rewritten, and a CD `<file index="4">` line is
removed (it would remount the disc late); a `<reset/>`, another `<file>`, a
comment -- anything else you added by hand -- stays. The toast says which of
three things happened: `Wrote` for a new file, `Updated` for an edited one,
`Already current` when nothing changed.

**Save settings keeps it current.** While a game launched from a file with a
settings block is running, Save settings on the main page updates that block
as well as the startup config.

The row greys out when there is no disc image mounted -- including when the
**physical drive** is selected, since a real disc in a tray has no path to put
in a file.

Move the result into `/media/fat/_Console/` to have it appear on the main
menu's front page; nothing in the file depends on where it lives.

## The settings block

```xml
<mistergamedescription>
    <rbf>_Test/amigacd</rbf>
    <amigacd_config version="1">
        <setting name="cpu" value="35"/>
        <setting name="hdf0_path" value="games/AmigaCD32/Banshee (1994).chd"/>
        <setting name="cd_audio_buffer" value="1"/>
        ...
    </amigacd_config>
</mistergamedescription>
```

One element per setting, named after the field of `mm_configTYPE` in
`support/minimig/minimig_config.h` it sets (`hdf0_path` is
`hardfile[0].filename`, `snac0` is `snac[0]`, `extrom` is the extended ROM).
Numbers are decimal or `0x` hex; paths are XML-escaped (`&amp;`, `&quot;`).

A setting the file does not name keeps the startup config's value, so a block
can be cut down by hand to just what a game needs -- say only
`cd_audio_buffer`. A name this build does not know, or a value that does not
fit, is skipped and the rest still applies. The machine facts the startup
config is corrected for (chip RAM and floppy count per system) are corrected
for a file's settings in the same way.

The block is read once, at the launch. MiSTer's own `.mgl` scanner ignores it,
so a file with a block still launches on a build without this support -- it
just gets the startup config.

## The file

Put it anywhere the menu browses — `/media/fat/_Console/`, or beside your
games. The name shown in the menu is the filename.

```xml
<mistergamedescription>
    <rbf>_Test/AmigaCD_20260916</rbf>
    <file delay="2" type="f" index="4" path="Amiga/Games/MyGame.chd"/>
</mistergamedescription>
```

| attribute | what it means |
|---|---|
| `delay` | seconds to wait before this item, counted from the core loading. Give the core time to come up — 2 is a reasonable floor, more for a slow card. |
| `type` | `f` for a file. Not actually checked by this core, matching the Minimig tree; every `.mgl` in the wild says `f`. |
| `index` | **which drive** — see below |
| `path` | absolute, or `../` from the storage root, or relative to the core's home directory (`games/Amiga/`) |

### `index` — which drive

| index | drive |
|---|---|
| 0, 1 | df0, df1 |
| 4 | the CD |
| 5, 6 | Hard Disk 0, Hard Disk 1 |

0 and 1 are the floppies because that is what they have always been on the
Minimig side, so an `.mgl` written for Minimig keeps working. 2 and 3 are
floppy drives this machine does not have. An index with no drive behind it —
including 4 on the A1200 profile, which has no CD — is **skipped with a line in
the log**, and the rest of the file still runs. A typo costs one mount, not the
launch.

Up to six items per file, and they run in order, so a game that wants a disc
and a save volume is:

```xml
<mistergamedescription>
    <rbf>_Test/AmigaCD_20260916</rbf>
    <file delay="2" type="f" index="4" path="Amiga/Games/MyGame.chd"/>
    <file delay="1" type="f" index="5" path="Amiga/Saves.hdf"/>
</mistergamedescription>
```

## Resetting

Nothing in the list above resets the machine. That stays the `.mgl` author's
decision, and the format has an element for it:

```xml
    <reset delay="1" hold="1"/>
```

**A CD usually does not need one.** The Akiko bridge sees `cd_is_mounted()` go
false → true and handles the TOC rebuild and its own deferred reset itself —
the same path that makes mounting a disc from the OSD work without a manual
reset. A hard disk or a floppy mounted after the machine has already booted is
a different matter, and that is what `<reset/>` is for.

## The `<rbf>` line and dated cores

`<rbf>` is a path under `/media/fat/`, without the `.rbf`. Since cores are
staged as `AmigaCD_YYYYMMDD.rbf` (see [`deployment.md`](deployment.md)), that
name changes every build, and every `.mgl` naming the old one stops working.

Two ways to live with that:

- Name the **directory and stem** of the core you keep long-term and accept
  editing your `.mgl` files when you deploy. Fine for a handful.
- Or keep an undated `AmigaCD.rbf` alongside for `.mgl` use. Note the cost:
  the menu shows it as a second entry with `--.--.--` for its date, because the
  date comes from the filename, and `scripts/deploy-core.sh` will report it
  every time (it never removes one unless you pass `--include-undated`).

## What is not supported

- **Settings outside the config.** The MT32-pi page's synth, ROM and
  soundfont, the volume, and MiSTer.ini video options are not part of the
  machine config and are not carried. Everything the OSD's Save settings
  stores is.
- **The full Minimig OSD.** Only the AmigaCD console variant learned the CD and
  hard disk indices. A stock Minimig core still handles floppies only.
- **Recording the launch as "recent".** A game launched by `.mgl` does not
  appear in the row's recent list, matching every other MGL path in MiSTer:
  that list is of things you chose in this OSD.
