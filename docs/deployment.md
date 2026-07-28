# Deployment

This build ships as an **alternative main binary**, selected per-core from
`MiSTer.ini`. Stock MiSTer stays installed and untouched; loading the AmigaCD
core swaps to our binary, and loading anything else swaps back.

No code change was needed for this — `main=` and wildcard section names are
both stock upstream features.

## Setup

**1. Put the binary on the SD card** as `/media/fat/MiSTer_AmigaCD`:

```bash
./scripts/wsl-build.sh deploy      # needs ./host or $MISTER_HOST
```

**2. Add to `/media/fat/MiSTer.ini`:**

```ini
[AmigaCD]
main=MiSTer_AmigaCD
```

That's it. Load the AmigaCD core and it restarts into our binary.

## Why `[AmigaCD]`

Section names match the **core's `CONF_STR` name, not the `.rbf` filename**.
This is the detail most likely to waste an hour. `ini_get_section` in `cfg.cpp`
compares against `user_io_get_core_name()`, which `user_io_read_core_name()`
fills from `user_io_get_confstr(0)` — the first field of the core's `CONF_STR`.

On the `amiga-console` branch, `Minimig.sv:173` declares:

```verilog
localparam CONF_STR = {
    "AmigaCD;UART115200:230400,MIDI;",
```

so the section is `[AmigaCD]` regardless of what the `.rbf` is called. If you
build from `cd32-native-mvp` instead, the name there is `MinimigCD`, so use
`[MinimigCD]` — or `[*]`-style wildcards to cover both, see below.

## How it works

`user_io.cpp:1466`, right after the core name is read and `cfg_parse()` has run:

```c
const char *main = getFullPath(cfg.main);
if (strcasecmp(main, getappname()) && FileExists(main))
    app_restart(path, xml, main);
```

`cfg.main` defaults to `"MiSTer"`, so cores with no `main=` restart back into
the stock binary. There's no restart loop: once running as `MiSTer_AmigaCD`,
`getappname()` matches `main` and the check falls through.

A bare filename resolves against the **current storage root**, not a fixed
path — `getFullPath` → `getRootDir` → `getStorageDir`, which is `/media/fat`
for SD and `/media/usbN` when running from USB. If you boot from USB, the
binary has to be there too, or use an absolute `main=/media/fat/MiSTer_AmigaCD`.

## Wildcards, and the trap with `[CD-*]`

Wildcards are supported: `[Amiga*]` matches any core whose `CONF_STR` name
starts with `Amiga`. `ini_get_section` records the `*` position and switches to
`strncasecmp` up to that point.

Anime0t4ku's physical-disc build uses the same mechanism:

```ini
[CD-*]
main=MiSTer_Physical-CD
```

which matches his renamed `CD-MegaCD`, `CD-PSX`, etc. Our core is called
`AmigaCD`, so the two do not overlap and both can be installed side by side.

**But** if you ever rename our core to fit that convention — `CD-Amiga`,
say — `[CD-*]` would capture it and route it to `MiSTer_Physical-CD`, which has
no Akiko or CDTV bridge. The CD32 core would come up with no CD at all.

All matching sections are applied in file order and the last assignment to a
key wins, so if both are present, put the more specific one second:

```ini
[CD-*]
main=MiSTer_Physical-CD

[CD-Amiga]
main=MiSTer_AmigaCD
```

## Verifying

The restart is logged on the serial console / `MiSTer` stdout:

```
Core name is "AmigaCD"
Current exec is /media/fat/MiSTer, core requires exec /media/fat/MiSTer_AmigaCD
```

If that line never appears, the section name doesn't match the `CONF_STR`
name, or the binary isn't where `main=` resolves to. `FileExists(main)` failing
is silent by design — it falls through to the stock binary rather than
bricking the core load.
