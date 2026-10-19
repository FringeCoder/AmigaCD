# AmigaCD

Work towards running the MiSTer **Amiga CD32 / CDTV cores from a real CD-ROM drive**
attached to the MiSTer, instead of from a CHD/CUE image on the SD card.

**This is the userspace half** — a `Main_MiSTer` fork, which builds
`MiSTer_AmigaCD`. The core half, the RTL that Quartus fits into the `.rbf`,
is [`FringeCoder/AmigaCD_MiSTer`](https://github.com/FringeCoder/AmigaCD_MiSTer),
and its README carries the feature list and its status. Several features here
are split across the two repositories, so it is worth knowing which one you are
looking at. This repository is also the home of `rtl/snac_psx.v`, which both
core repositories vendor a copy of; see [`rtl/README.md`](rtl/README.md).

## Upstreams

Three we are built from, two we track for improvements:

| Role | Repo | Branch |
|---|---|---|
| Physical disc userspace | [`Anime0t4ku/Main_MiSTer_Physical_Disc`](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) | `master` |
| CD32/CDTV userspace driver | [`kblood/Main_MiSTer-cd32-cdtv`](https://github.com/kblood/Main_MiSTer-cd32-cdtv) | `amiga-console` |
| CD32/CDTV core RTL | [`kblood/Minimig-AGA-cd32-cdtv-native-akiko`](https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko) | `amiga-console` |
| *Watched* — parallel disc effort | [`theshaneobrien/mister-disc-drive-support`](https://github.com/theshaneobrien/mister-disc-drive-support) | `physcd` |
| *Watched* — the root upstream | [`MiSTer-devel/Main_MiSTer`](https://github.com/MiSTer-devel/Main_MiSTer) | `master` |

The last two are not merged from automatically. `MiSTer-devel` matters because
nobody is rebasing the forks between us and it, so keeping current is our job.
See [`docs/upstream-pins.md`](docs/upstream-pins.md) for the pins, the sync
procedure, and what is worth taking from each.

Fetch them into `upstream/` with `scripts/fetch-upstream.sh` (not tracked), or
use the git remotes: `physdisc`, `kblood`, `discdrive`, `main-upstream`.

Two of those four names were wrong until 2026-09-18 -- the remotes are `kblood`
and `main-upstream`, not `cd32-main` and `mister-upstream` -- and `discdrive` did
not exist at all, so the one repo this table calls *watched* could not be fetched
by anyone following these instructions. It is
`https://github.com/theshaneobrien/mister-disc-drive-support.git`.

## Key finding

**No RTL changes are required.** The Akiko and CR-511 bridges in the core are a pure
UIO transport — every CD sector is fetched by userspace and pushed over SPI. Physical
disc support is entirely a `Main_MiSTer` change.

The blocker is that the two userspace forks meet MiSTer's CD subsystem at different
abstractions:

```
/dev/sr0 -> physical_disc.cpp -> toc_t   (cd.h)
                                   |
                              [ MISSING ]  load_physical_disc()
                                   |
             akiko_cd32.cpp <- drive_t   (ide.h)  <- cdrom_read_raw_sector()
```

`ide_cdrom.cpp::load_chd_file()` already performs a `toc_t` -> `drive_t` translation,
so the adapter is roughly 30 lines of lifted code.

See [`docs/integration-plan.md`](docs/integration-plan.md) for the step-by-step plan and
[`docs/analysis.md`](docs/analysis.md) for the upstream teardown.

## Branches

- `master` — known-good, buildable states only.
- `develop` — integration branch; feature work branches off here.

## Status

This repo *is* Main_MiSTer @ `amiga-console`, with the physical-disc module
grafted on. Phases 0-3 and the phase 4a OSD trigger are done: the adapter, the
bridge changes, the mount plumbing, a **Real CD** row in the AmigaCD console
OSD, seek hints and the collapsed prefetch cache. Per-game NVRAM (item 11) and
hot swap (item 12) are done too, the latter alongside auto-boot-on-insert.

The tree is current with all three upstreams, including 58 commits of
`MiSTer-devel/Main_MiSTer` — nobody between us and it is rebasing, so that is
ours to keep up.

See [`docs/integration-plan.md`](docs/integration-plan.md) for what each phase
actually proved, and what is left.

**It compiles.** [`.github/workflows/build.yml`](.github/workflows/build.yml)
cross-builds on GitHub Actions with the exact pinned toolchain (ARM GNU-A
10.2-2020.11), producing a 1.06 MB stripped `ELF 32-bit LSB executable, ARM,
EABI5` with **zero** compiler warnings. The binary needs at most `GLIBC_2.28`.

CI carries the build because there is no local toolchain: this is a Windows
machine without WSL, and ARM never shipped a Windows-hosted build of the
pinned version.

**It boots a CD32 game from a real disc.** Verified on hardware 2026-07-31:
mount, TOC read, and a game running from the optical drive. The drive measured
~720 kB/s (~351 sectors/s, ~4.7× red book), well above what the CD32 consumes —
so throughput, the plan's headline risk, is retired.

**Per-game NVRAM works too.** Two discs on hardware resolved to distinct save
slots via both identity paths — one by ISO9660 volume label (`ISSUE7`), one by
TOC-geometry UUID because it has no label — and the old save is flushed before
the slot changes.

**Audio CDs play.** That exercises the CDDA path end to end, including the
byte-order normalisation where a mistake would have been unmistakable — white
noise rather than silence.

**Insert a disc and it mounts; take it out and it unmounts.** One drive watcher
covers both auto-boot-on-insert and hot swap, so changing discs mid-session no
longer needs the OSD.

Not yet exercised on hardware: the Real CD reset paths, and everything CDTV.
See "Still to do" in [`docs/integration-plan.md`](docs/integration-plan.md).

## Using it

| | |
|---|---|
| Building and copying to the card | [`docs/deployment.md`](docs/deployment.md) |
| Launching a game straight from the MiSTer menu | [`docs/mgl.md`](docs/mgl.md) |
| Comparing the core against real hardware | [`docs/vamigats.md`](docs/vamigats.md), [`docs/timing-probe.md`](docs/timing-probe.md) |
| What still needs testing on a machine | [`docs/hardware-test-plan.md`](docs/hardware-test-plan.md) |
| Open items that are not hardware verification | [`docs/state-todo.md`](docs/state-todo.md) |

## Getting online over serial (PPP)

The cardless way online: the Amiga's serial port (Paula) is wired to MiSTer's
UART, and MiSTer's Linux runs the other end of a PPP link. It is much slower
than the core's A2065 Ethernet card (see "Ethernet (A2065)" in
[FringeCoder/AmigaCD_MiSTer](https://github.com/FringeCoder/AmigaCD_MiSTer)'s
README) -- the port runs at 115200 or 230400 baud, about 11 or 23 KB/s before
PPP's overhead -- but it needs nothing in the Ethernet menu and works on any
MiSTer.

On MiSTer, in the OSD:
- open the core's system settings page (the one with "Define buttons" and
  "Button/Key remap") and choose **UART mode**;
- set **Connection** to **PPP** and **Baud** to 115200 or 230400 (the core
  offers those two);
- the choice is saved per core (`uartmode.AmigaCD` in MiSTer's config folder).
  "Reset UART connection" restarts the Linux side if the link hangs.

Choosing a mode makes the menu run MiSTer Linux's `uartmode` script
(`user_io.cpp`, `SetUARTMode`), which starts the Linux end of the link. What
that script runs, and the addresses it hands out, belong to the MiSTer Linux
image rather than to this repository -- see the MiSTer documentation for its
PPP mode. [`docs/timing-probe.md`](docs/timing-probe.md) shows how to check on
the MiSTer which mode is active and what holds the serial device.

On Amiga:
- install a TCP/IP stack with PPP over `serial.device`: MiamiDX has PPP built
  in; Roadshow or AmiTCP need a SANA-II PPP driver from Aminet;
- set the serial device to the same baud as the OSD, 8N1, with RTS/CTS
  hardware handshaking (the core wires RTS and CTS through);
- dial with no phone number or modem init string (it is a direct link) and
  let PPP negotiate the address.

Not yet verified on this fork: which Amiga PPP stacks negotiate cleanly with
MiSTer's end, and whether 230400 holds without overruns on a 68000. Record
what works here.
