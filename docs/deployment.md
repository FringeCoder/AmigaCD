# Deployment

This build ships as an **alternative main binary**, selected per-core from
`MiSTer.ini`. Stock MiSTer stays installed and untouched; loading the AmigaCD
core swaps to our binary, and loading anything else swaps back.

No code change was needed for this — `main=` and wildcard section names are
both stock upstream features.

## Setup

**1. Stage the files.** `./scripts/stage-output.sh --binary` collects everything
into `output/`, always overwriting — both cores plus the binary from CI. The
binary comes from CI because the Windows dev machine has no ARM cross-toolchain,
so `make` cannot run there; `scripts/deploy-from-ci.sh` exists for the same
reason. To build and push from WSL instead:

```bash
./scripts/wsl-build.sh deploy      # needs ./host or $MISTER_HOST
```

**2. Copy to the SD card:**

| File | Goes to |
|---|---|
| `MiSTer_AmigaCD` | `/media/fat/MiSTer_AmigaCD` |
| `MiSTer` | `/media/fat/MiSTer` — replaces the stock main binary |
| `AmigaCD_<date>.rbf` | wherever you keep cores — **on the dev machine that is `/media/fat/_Test/`** |
| `menu.rbf` | `/media/fat/menu.rbf` |

### The date beside the core name

The MiSTer menu's core browser shows a date next to each core, and it gets it
from the **filename**, not from anything inside the bitstream — there is nowhere
in a `.rbf` for a build date to live. `get_display_name()` in `file_io.cpp`
looks for `_20` in the name and takes the six characters after it; when that
fails it writes `------`, which `menu.cpp` renders as `--.--.--`. That dash row
is the only symptom, and it looks like a build problem rather than a naming one.

So the core is staged as `AmigaCD_YYYYMMDD.rbf`, dated from the fit's mtime, and
is copied to the card **under that name**. Dropping the date on the way puts the
dashes back.

Nothing resolves the core by filename, so the dated name is safe: `MiSTer.ini`
sections match `CONF_STR` (see below), and `bootcore` already understands
`<core>_YYYYMMDD.rbf` and picks the newest — `matchesCore_yyyyMMdd_rbf()` in
`bootcore.cpp` exists for this convention.

The one cost is that dated names accumulate. A new build does not replace the
old one, and every `AmigaCD*.rbf` in the directory is its own menu entry, so the
previous ones have to go at some point. `stage-output.sh` keeps the **staging**
directory to a single core; `deploy-core.sh`, below, does the same for the card.

### Copying a core: `scripts/deploy-core.sh`

```bash
./scripts/deploy-core.sh                    # newest staged core, keep 2 on the card
./scripts/deploy-core.sh --dry-run          # say what it would do, touch nothing
./scripts/deploy-core.sh --keep 3
./scripts/deploy-core.sh --include-undated  # also remove an undated AmigaCD.rbf
./scripts/deploy-core.sh output/AmigaCD_20260901.rbf
```

It does the two things a plain `scp` gets wrong.

**It copies under a name the menu cannot see.** An `.rbf` is a few megabytes
over the network, and while it is in flight a partially written file sits in a
directory the core browser scans; selecting it loads a truncated bitstream. So
the copy lands as `.AmigaCD_<date>.rbf.part` and is renamed into place once
complete — the same shape `wsl-build.sh deploy` already uses for the binary, and
for the same reason. That name is invisible twice over: `ScanDirectory` skips
dotfiles outright (`file_io.cpp:1754`) before it ever tests the extension, and
the extension it would test is taken after the **last** dot, so the file is a
`.part` and not an `.rbf` either way.

**It prunes the cores the new one supersedes** — but not down to one. The
previous core is the way back when a build misbehaves (see below), and the
deploy most likely to need it is the one that just replaced it, so the default
is `--keep 2`: the new core and the one before it. `--keep 1` works and says
what it is costing you.

It will only ever remove files named exactly `AmigaCD_<8 digits>.rbf`, the form
this project's own staging produces. A hand-named core, a core from somewhere
else, a `.bak`, a leftover `.part`: reported, never removed. An undated
`AmigaCD.rbf` is the one exception you can ask for, with `--include-undated`,
because it is the likeliest cause of a duplicate menu entry showing `--.--.--`.

That rule is the only thing in this repository that deletes files, so it is
tested rather than trusted: `tests/test_deploy_core_prune.sh` feeds the decision
function synthetic listings and checks exactly which lines come back. It runs
under `make -C tests`, and its header records the mutations it was falsified
against.

### Which core file is actually running

The table says "wherever you keep cores" because the answer is per machine and
getting it wrong is silent: the copy succeeds, the version numbers all look
right, and the machine keeps running the core it was already running.

On the development MiSTer the live core is under `/media/fat/_Test/`. There is
also a `/media/fat/_Computer/Minimig.rbf`, which is an older AmigaCD build and
is **not** what gets loaded. Ask the machine rather than guessing:

```bash
ssh root@<mister> "ps aux | grep [M]iSTer_AmigaCD"
```

The running process shows both the binary and the `.rbf` path it was given:

```
13989 root  /media/fat/MiSTer_AmigaCD /media/fat/_Test/AmigaCD_20260915.rbf
```

`_Test/` accumulates `amigacd.rbf.<label>` backups from before the dated naming
— keep the previous core around one way or the other, since it is the only way
back if the new one misbehaves. With dated names that happens by itself, which
is the other half of why they are worth the clutter, and why `deploy-core.sh`
defaults to keeping two rather than one.

### The binary cannot be overwritten while it is running

`scp` straight onto `/media/fat/MiSTer_AmigaCD` fails with a bare
`dest open "...": Failure` whenever the AmigaCD core is loaded — that is
ETXTBSY, since the file being written is the running executable. Copy beside it
and rename:

```bash
scp output/MiSTer_AmigaCD root@<mister>:/media/fat/MiSTer_AmigaCD.new
ssh root@<mister> "mv -f /media/fat/MiSTer_AmigaCD.new /media/fat/MiSTer_AmigaCD && chmod +x /media/fat/MiSTer_AmigaCD"
```

The running process keeps the old image until the core is reloaded, so **a
deploy is not live until the core is reloaded**.

### Checking what is actually on the card

Both files, against the local copies in `output/`:

```bash
ssh root@<mister> "md5sum /media/fat/MiSTer_AmigaCD /media/fat/_Test/AmigaCD_*.rbf"
md5sum output/MiSTer_AmigaCD output/AmigaCD_*.rbf
```

If the card side lists more than one core, that is the accumulation the dated
names cause — the extra ones are old builds still offered by the menu, and the
core actually loaded is the one `ps aux` named above, not necessarily the
newest.

The binary also carries a build date — `grep -a -oE "[$]VER:[0-9]+"` prints
`$VER:YYMMDD` — which is worth a glance but does not identify a commit: two
builds from the same day share it. The md5 against the artifact
`stage-output.sh --binary` fetched is what ties the card to a specific CI run,
and that run's commit is printed by the staging script.

**3. Add to `/media/fat/MiSTer.ini`:**

```ini
[AmigaCD]
main=MiSTer_AmigaCD

snac_psx=1
```

Load the AmigaCD core and it restarts into our binary.

### Getting the machine's state off it

`scripts/collect-diag.sh` pulls everything worth reading into one tarball:

```bash
./scripts/collect-diag.sh                # logs, config, what is installed
./scripts/collect-diag.sh --with-images  # plus save state thumbnails and screenshots
./scripts/collect-diag.sh --clear        # truncate the device logs first
./scripts/collect-diag.sh --full         # whole logs, not the last 5000 lines each
```

Host comes from `./host` or `$MISTER_HOST`, same as the deploy scripts. It is
read-only on the device except for `--clear`, which truncates the logs under
`/tmp` and nothing else.

The useful sequence for a specific fault is `--clear`, reproduce it, then run it
again — what comes back then contains that run and nothing before it.

What it collects, and what each is good for:

| file | answers |
|---|---|
| `report.txt` | uname, uptime, free space, what is running, md5s of the installed binary and cores, `MiSTer.ini`, the contents of `savestates/`, dmesg tail |
| `akiko_dbg.log` | `sec_req timing` — the real per-sector delivery period, which is the number `AKIKO_SECTOR_PERIOD_US` was set against without ever being measured on hardware. Also `sec_req GAP` |
| `ss_dbg.log` | the save state sequence, including whether each toast was drawn or **dropped** by the `menustate <= MENU_INFO` gate |
| `cdtv_dbg.log` | the CR-511 command stream |
| `physical_disc_stats.log` | drive hit rate and worst-case I/O latency |
| `images/` | the save state thumbnails, which are what the preview shows, and the newest screenshots |

A missing log is not a failure: these are written on first use, so one that is
absent means that subsystem never ran. The script says which.

**Screenshots beat photographs of the screen**, and not only for convenience: a
capture off the device isolates what the core produced from what the display did
with it. The one question it cannot answer is whether the picture is *stable* —
flicker, resync, or a mode change on the monitor leaves no trace in any file, so
that part still has to be watched and described.

### Why the binary is installed twice

`MiSTer` and `MiSTer_AmigaCD` are the **same build under two names**, and both
are needed once PSX SNAC is in play.

`main=` only redirects the core whose section it appears under. Everything else
— including the **boot menu** — keeps running whatever is at `/media/fat/MiSTer`.
The Menu core's SNAC pad navigation lives in userspace, so installing only
`MiSTer_AmigaCD` gives you a pad that works inside AmigaCD and does nothing in
the menu, with no error to explain the difference.

If you do not use the SNAC pad in the boot menu, the stock `MiSTer` binary is
fine and only `MiSTer_AmigaCD` is required.

## Testing with a pre-rename core

If your core's OSD title says **Minimig** rather than **AmigaCD**, its
`CONF_STR` is not `AmigaCD` and neither the `.ini` match nor the console OSD
will fire — see the next section for why the `.rbf` filename is irrelevant.

kblood's CD32 RTL declared `CONF_STR "Minimig"` until `6f8abca` (2026-05-23,
→ `MinimigCD`) and `c5eb180` (2026-06-13, → `AmigaCD`). A core built before
that has the complete Akiko bridge — it will boot a CD32 game from a CHD — but
it cannot reach the AmigaCD console OSD, and that OSD's **Real CD** row is the
only caller of `PHYSICAL_DISC_SENTINEL` anywhere in the tree. So the entire
physical-disc feature is unreachable from such a core.

To test anyway, two steps:

```ini
[Minimig]
main=MiSTer_AmigaCD
```

```bash
ssh root@<mister> touch /media/fat/amigacd_force
```

The marker file makes `is_amigacd()` return true for any Minimig-family core.
On success the log prints:

```
AmigaCD: /media/fat/amigacd_force present -- forcing console OSD for core "Minimig"
```

Both steps are blunt on purpose. `[Minimig]` also routes *stock* Minimig to
this binary, and the marker also forces stock Minimig into the console OSD.
That is fine while bringing the feature up on one machine, and is why the
override is a file you have to go and create rather than a setting. **Delete
the marker and restore `[AmigaCD]` once you are running a core that declares
`AmigaCD`** — the shim is not a substitute for building the right core, which
also carries the two post-February Deep Core sector-delivery fixes
(`32c0a3c`, `fce5cc5`) that a pre-rename build lacks.

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
