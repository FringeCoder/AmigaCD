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

- **Choosing the machine.** CD32, CDTV, A500 and A1200 come from the saved
  config, not from the `.mgl` — the format carries files and resets, and
  nothing else. An `.mgl` for a CDTV title needs the CDTV profile already
  saved.
- **The full Minimig OSD.** Only the AmigaCD console variant learned the CD and
  hard disk indices. A stock Minimig core still handles floppies only.
- **Recording the launch as "recent".** A game launched by `.mgl` does not
  appear in the row's recent list, matching every other MGL path in MiSTer:
  that list is of things you chose in this OSD.
