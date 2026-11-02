# The CD32 FMV module: plan

**Date:** 2026-11-01
**Status:** phase 0 done on paper (2026-11-01); phase 1 passed on hardware
(2026-11-02); phase 2 in progress.

The CD32 FMV cartridge plugs into the CD32's expansion slot. It adds a C-Cube
CL450 MPEG-1 video decoder, an LSI L64111 MPEG audio decoder, 256 KB of ROM
holding the Video CD player, and 512 KB of RAM. Its picture is mixed over the
Amiga's. With it a CD32 plays Video CDs and the handful of titles that used
the cartridge.

This plan adds it to the AmigaCD core as one more option, off by default, in
phases. Each phase ends at a go / no-go gate, like
[`arm-cpu-plan.md`](arm-cpu-plan.md). A phase that fails its gate stops the
plan there, and the reason gets written down here.

## The shape of it

MPEG decoding runs on the ARM, in Main. The FPGA gets only a register front
end, a frame buffer reader and an overlay mux.

| Part | Where | What it does |
|---|---|---|
| Board decode, autoconfig, ROM and RAM | core | One more Zorro II card in `cpu_wrapper.v`'s autoconfig chain |
| CL450 / L64111 / IO registers | core front end, userspace model | The CPU's writes are posted to a DDR3 ring. Its reads come from shadow registers that Main keeps current, so a read never waits on the ARM. |
| MPEG-1 video decode | userspace | libmpeg2 (GPL-2+) |
| MPEG audio (layer II) decode | userspace | kjmp2 (zlib) |
| Frame display and genlock | core | Reads a DDR3 frame buffer and muxes it in before `video_mixer` |
| Audio out | userspace + core | PCM into the existing CDDA FIFO (see phase 4) |

### Why not decode in the FPGA

The fabric has no room for it. An MPEG-1 decoder needs a VLD, an IDCT, motion
compensation and frame stores. The core is at about 83-84 % of its ALMs
(`seed-lottery.md` rows 23 and 28 in the core repo). Since the Action Replay
sync it closes timing on 4 seeds in 20 (ledger row 39). The ledger's own notes
say timing headroom, not area, is what the fit is short of, and a block that
size would cost both. On the other side, the ARM is a dual 800 MHz Cortex-A9
with NEON. MPEG-1 at 352x288 and 25 fps is a light load for it. Phase 1
measures that before anything is built.

### Why the register model lives in Main

The front end could forward every register access to the ARM and wait for the
answer. One DDR3 round trip costs a median of 2.5 us, with 1 % of them over
22 us (`arm-cpu-plan.md`, step 1). The FMV ROM polls status registers, and each
of those reads would stall the 68020 that long. So:

- **Writes are posted.** The front end appends `{address, data, size}` to a ring
  in DDR3 and lets the CPU go at once. The CL450 data port at `$260000` takes a
  word per write and is the busiest register by far. The CPU copies sectors
  into it, so it goes through the same ring.
- **Reads come from FPGA shadow registers.** These are the IO status word, the
  CL450 and L64111 interrupt and status registers, and buffer fullness. Main
  writes them through the same DDR3 mailbox the A2065 uses
  (`rtl/A2065/a2065_ddr3_mailbox.v` in the core).
- **The interrupt line is the shadow's OR**, raised from FPGA logic. Per
  WinUAE it is INT2 (PORTS), shared with Akiko and CDTV
  (`IRQ_SOURCE_CD32CDTV`).

The RTL then stays small and dumb. All the CL450 command semantics are C,
where they can be unit-tested.

## Reference: what WinUAE models (an oracle, not a source)

From `cd32_fmv.cpp` and `cd32_fmv_genlock.cpp` in
[tonioni/WinUAE](https://github.com/tonioni/WinUAE), read 2026-11-01:

```
$200000-$23FFFF  ROM, 256 KB (user-supplied fmv.rom; WinUAE ROMTYPE_CD32CART)
$240000          IO / status word
$2500xx          L64111 audio decoder, word registers
$260000          CL450 data port (word; the MPEG stream)
$2700xx          CL450 video decoder, word registers
$280000-$2FFFFF  RAM, 512 KB
```

- It is a **1 MB Zorro II autoconfig board**. WinUAE expects it at `$200000`
  and logs "unexpected base address" otherwise. So it has to be **first** in
  the autoconfig chain, ahead of Z2 Fast RAM, Toccata and the A2065
  (`cpu_wrapper.v:1140-1160` in the core).
- IO status bits, as read: `$8000` CL450 IRQ, `$4000` L64111 IRQ, `$0800` CL450
  FIFO level, `$0400` the L64111 FIFO flags. As written: `$4000` CL450 video on,
  `$0200` L64111 mute.
- The CL450 is driven by host commands (`CL_Play`, `CL_SetWindow`,
  `CL_SetBorder`, `CL_SetBlank`, `CL_NewPacket`, `CL_InquireBufferFullness`
  and others). The ROM also uploads CL450 microcode (`CPU_imem`). WinUAE
  accepts that and ignores it, and so would we.
- Pacing: a 90 kHz SCR advances with each hsync while playing. A decoded
  picture is presented every `max_sync_vpos` lines, doubled below 40 fps,
  and `CL_INT_PIC_D` is raised then.
- **Genlock is guesswork in WinUAE as well.** It shows the Amiga pixel when its
  value is at least a threshold (`GENLOCK_VAL_32 0x20`) and the MPEG pixel
  otherwise. Its own comment says "this is probably not how it actually works".
  The MPEG picture is centred, and the CL450's border colour fills the rest.
  The schematics show genlock-mode bits that the ROM never sets. Phase 0
  item 5 has a better-grounded rule.

This repository has one stale note: `docs/reference/cd32-probe-copperline.tsv`
row 24 calls `$A80000` the "unmapped FMV window". It is not the FMV window. In
the core that range is now the Kickstart mirror or the Action Replay
(`rtl/gary.v:160`, `rtl/cart.v:90`). Fix the label when the probe is next
touched.

## Phase 0: questions answered on paper

No code in this phase. Its gate: every item below answered here, with a
source.

**Result, 2026-11-01: items 1, 2 and 4 are closed (item 1 by Mark's decision,
and both chip manuals now in hand). Item 3 is decided, Cannon Fodder
first, with its MPEG track still to identify. Items 5 and 6 are answered by
inference, and each still needs the confirmation named below.** None of the open parts blocks phase 1, which needs only a Video
CD image. So phase 1 can start while they are closed.

Sources used:

- WinUAE `cd32_fmv.cpp`, `cd32_fmv_genlock.cpp`, `rommgr.cpp` and `COPYING`:
  [tonioni/WinUAE](https://github.com/tonioni/WinUAE) `master`, read
  2026-11-01.
- *Commodore Amiga CD32 Engineering Change Release*, 1993-07-20, "External
  Connector Pinouts", p. 4-10
  ([amiga.net.au](https://amiga.net.au/files/Tech_Amiga/Commodore_AmigaCD32_Engineering_Change_Release_1993-07-20_ocr.pdf)).
- *Lisa specification* 391227
  ([oldcrap.org](https://oldcrap.org/wp-content/uploads/2023/04/amiga-all-spec-lisa-391227.pdf)).
- *CL450 MPEG Video Decoder User's Manual*, C-Cube 90-1450-101, 1994
  ([bitsavers](https://bitsavers.mirrorservice.org/components/c-cube/90-1450-101_CL450_MPEG_Video_Decoder_Users_Manual_1994.pdf)),
  sections 1.2.1, 3 (pin descriptions) and 6 (video display interface).
- Clark Alexander, *REVIEW: CD32 Full Motion Video module*,
  comp.sys.amiga.reviews, 1998-11-20
  ([math.uh.edu](https://www.math.uh.edu/~barrett/data/all/CD32FMV)).
- Cannon Fodder and the module: [MobyGames trivia](https://www.mobygames.com/game/210/cannon-fodder/trivia/),
  [IMDb alternate versions](https://www.imdb.com/title/tt0452596/alternateversions).
- L64111 manual listing: [alldatasheet 137324](https://www.alldatasheet.net/datasheet-pdf/pdf/137324/LSI/L64111.html)
  (not downloadable from this environment).

### 1. Licence: decided, the model is written from the manuals

- **libmpeg2** is GPL-2.0-or-later, so it can go into a GPL-3 Main.
  **kjmp2** (Martin Fiedler, as vendored in WinUAE `archivers/mp2/kjmp2.cpp`)
  is under the zlib licence, which is compatible.
  **libavcodec** is LGPL-2.1-or-later, which is also fine if phase 1 needs it.
- **WinUAE** ships the GPL-2 text in `COPYING`. Its file headers, including
  `cd32_fmv.cpp` ("Copyright 2008-2014 Toni Wilen"), carry a copyright line and
  no version statement. GPL-2 section 9 lets a program that does not name a
  version be taken under any version, but a `COPYING` holding v2 is arguably
  naming one. So whether WinUAE code may go into GPL-3 Main is unsettled.
- **This is not a new question for this repository.**
  `support/minimig/akiko_cd32.cpp:3` "Mirrors the WinUAE Akiko CD command
  interpreter (akiko.cpp @ SHA 2c7f8581)". That came in from kblood's fork and
  cites WinUAE line numbers throughout.

**Decision (Mark, 2026-11-01): write the model from the manuals.** No WinUAE
code goes into `fmv.cpp` or the RTL. WinUAE is used only as a **behavioural
oracle**: run the same disc in it, then compare its logs and output with ours.
That rules out transcribing it, its constants included. Facts about the
hardware that WinUAE happens to record (the board map, ROM checksums, the
autoconfig source in item 3) are still usable, each cited to a primary source
where one exists.

- **CL450:** the *CL450 MPEG Video Decoder User's Manual* covers the host
  registers, the command set (chapter 11, with each command's code and
  arguments), interrupts, buffer fullness and SCR/PTS sync (chapter 13). Write
  from that.
- **L64111:** LSI Logic's *L64111 MPEG Audio Decoder Technical Manual*,
  First Edition, 1993 (SHA-256 `0e94030c...`), supplied by Mark on
  2026-11-01. The manual is not committed: it is © LSI, all rights reserved.
  Our notes, with page references, are in
  [`reference/l64111-notes.md`](reference/l64111-notes.md). The register map,
  control bits, interrupt layout, FIFO flags and channel buffer are all
  covered, so the audio half can be written under this rule. The manual
  contradicts itself on one point: where the Channel Buffer read and write
  counters live (19/20 in its table, 25/26 on their own register pages). The
  phase 2 log settles it, and a Video CD player is unlikely to touch them.
- **Behaviour the manuals leave open:** wiring on the cartridge, such as the
  IO word at `$240000`, its bits, and which chip interrupts on which bit, is
  board design, not chip behaviour. It is taken from what the ROM does in
  WinUAE and on our core (phase 2 logs), and from the CD32 ECR. Each such fact
  is written down in this file with how it was established, so the boundary
  stays visible.

### 2. The ROM: two known dumps

From WinUAE `rommgr.cpp:334-337`. Both are 262 144 bytes.

| Version | Part | CRC32 | SHA-1 |
|---|---|---|---|
| 40.22 | 391777-01 | `f11158eb` | `94e469a76030dcb299ebc7520aaeef9db54284cf` |
| 40.30 | -- | `c35c37bf` | `03ca81c7a7b259cf64bc9582863eca0f6529f435` |

Like `CD32.rom`, it is user-supplied and never committed. Main should check
the size and log which of the two it is, or "unknown dump", without refusing
it.

### 3. Test material: Cannon Fodder first, then Video CD

- The 1998 review confirms that the cartridge plays **Video CD** movies and
  music videos, **Karaoke CDs**, and what it calls "Digital Video CDs"
  (presumably CD-i Digital Video). The test set needs at least one of each
  that exists: a VCD 2.0, a VCD 1.1 and a karaoke VCD. Add a CD-i DV disc only
  once someone confirms on a real cartridge or in WinUAE that it plays.
- **Primary test title: *Cannon Fodder* (CD32, Virgin 1994)** (Mark,
  2026-11-01). It plays a live-action MPEG intro, about 3 minutes, if the
  module is detected, and skips it otherwise. MobyGames' trivia calls it the
  only CD32 game that supports the module, and IMDb's alternate-versions entry
  for the intro says the CD32 release carries it as "a special MPEG track".
  This is a good first target for three reasons:
  - The project already boots it from CHD and from Real CD. It is the title
    the Akiko bring-up was done against (`akiko_cd32.cpp:4`).
  - It has two data tracks, which already needed the per-track CHD offset fix
    (`ide_cdrom.cpp:1452`). **Confirmed 2026-11-02:** track 2 is the MPEG
    track, stored `MODE2_RAW` (2352 bytes), 12675 frames with a 225-frame
    pregap: 352x288 at 25 fps, 166 s, Layer II audio at 44.1 kHz (phase 1
    result). Being raw 2352, it does not depend on item 4's 2336-byte
    header fix.
  - Being a game, it drives the cartridge through its own code path, not
    only through the FMV ROM's Video CD player. So it is a second,
    independent test of the model.

  How it detects the module is still to find out, most likely an
  `expansion.library` lookup of the board's autoconfig ID. In WinUAE the
  board's autoconfig bytes are the **first 128 bytes of `fmv.rom`**
  (`expansion.cpp`, `expamem_init_cd32fmv`, which loads them from the ROM).
  If that is how the real cartridge is wired, the core serves autoconfig
  straight from the ROM image, and the ID is whatever the user's dump holds.
  To confirm, read the ROM's first 128 bytes as autoconfig nibbles: they
  should decode to a valid 1 MB Zorro II board.
- Wikipedia's CD32 list also marks titles such as *Microcosm* and *Chuck
  Rock II* "FMV". That is in-game video they stream themselves (CDXL). No
  source says they use the cartridge.
- **Discs on hand:** Cannon Fodder, as CHD and physical. The Video CD set
  above is still to be found, and phase 1 needs one Video CD image, or the
  MPEG track ripped from Cannon Fodder, which serves the same purpose.

### 4. Mode 2 Form 2 sectors: no format problem in raw images and physical discs; 2336-byte images lose their header

The Akiko path ships whole 2352-byte frames (`akiko_cd32.cpp:180`), so the
form only matters where a frame is built:

- **Physical disc.** READ CD is sent with byte 1 = 0, meaning any sector type,
  and byte 9 = `0xF8`, meaning sync, all headers, user data and EDC/ECC
  (`physical_disc.cpp:211-232`, flags set at `:277`). The drive returns the full
  raw frame for Form 1 and Form 2 alike. The no-sync check in
  `cdrom_read_raw_sector` (`ide_cdrom.cpp:1396-1440`) applies to data tracks
  only (attr `0x40`), and a Form 2 frame has sync too, so it passes. *To
  confirm on hardware:* some drives refuse Form 2 with an error-flag field
  they dislike. The fallback, `CDROMREADRAW`, would then still return the
  frame.
- **CHD and BIN, 2352-byte (`MODE2/2352`, chdman's `MODE2_RAW`).** The frame
  is copied as it is (`ide_cdrom.cpp:1459-1466`, `:1511-1516`). That is the
  normal form of a Video CD rip.
- **CHD and BIN, 2336-byte (`MODE2/2336`).** `cdrom_read_raw_sector` zeroes
  bytes 0-15 and copies the 2336 bytes after them (`ide_cdrom.cpp:1468-1476`,
  `:1518-1523`). That leaves **no sync, no MSF and mode 0**. The cooked
  `MODE1/2048` branch right below synthesises a Mode 1 header; the 2336 branch
  should synthesise a Mode 2 one the same way (sync, BCD MSF, mode `0x02`). It
  is not an FMV-only problem, but FMV is where 2336-byte images turn up. Do it
  in phase 2, with a unit test.

### 5. Genlock: an analog RGB switch keyed by `_PIXELSW`, most likely Lisa's ZD

The CD32's expansion connector (ECR p. 4-10) carries everything a genlock
needs, and nothing that suggests WinUAE's threshold rule:

| Pin | Signal | ECR description |
|---|---|---|
| 151 | `PIXELSW_EXT` | External Genlock Pixel Switch (Video) |
| 152 | `_PIXELSW` | Genlock Pixel Switch (Video) |
| 153 | `_BLANK` | Video Blanking Signal |
| 154 | `PIXELCLK` | Pixel Clock |
| 159, 160 | `_HSYNC`, `_VSYNC` | |
| 163-168 | `AR_EXT`, `AR`, `AG_EXT`, `AG`, `AB_EXT`, `AB` | External Analog Red, Analog Red, ... |
| 174 | `_EXT_VIDEO` | Enable External Video |

So the cartridge receives the Amiga's analog RGB and a per-pixel switch, and
hands back RGB on the `_EXT` pins, with `_EXT_VIDEO` selecting it. That is a
per-pixel 2:1 mux between the Amiga's picture and the CL450's, which is what
phase 3 planned to build.

What drives `_PIXELSW`: Lisa has exactly one genlock output, **ZD** (pin 28).
By default ZD marks colour-0 (background) pixels, it carries the GAUD level
during blanking, and BPLCON3's `BRDNTRAN` makes the border opaque. `ZDBPEN`
/ `ZDBPSELx` make it follow one bitplane instead, `ZDCTEN` makes it follow
bit 15 of the colour table entry, and `ZDCLKEN` turns it into a clock and
overrides the rest (Lisa spec, BPLCON0/2/3). The Amiga RGB port's `/PIXELSW`
pin is the same signal under the same name. **Inference, not yet confirmed:**
`_PIXELSW` on the CD32 is Lisa ZD, possibly gated with blanking, and the
cartridge shows the CL450 picture where it is asserted. That reads as
"transparent colour 0", which is also what WinUAE's comment suspects its own
rule is a stand-in for.

**Consequences for the core:**

- Denise in the core has no ZD output (`rtl/denise.v:212`, a commented-out
  `zdclken`). Phase 3 needs one, computed from what Denise already knows:
  background pixel, `BRDNTRAN`, `ZDBPEN` / `ZDBPSELx`, `ZDCTEN`. `ZDCTEN` also
  needs the colour table's 25th bit, which the core may not store; check
  `denise_colortable.v` in phase 3.
- **Phase 2 logs BPLCON0/2/3 writes** made while the FMV ROM is running. That
  shows which ZD mode the player actually uses. If it uses none of the special
  modes, plain colour-0 transparency is the whole rule.
- **To confirm:** the CD32 main board schematic (ZD to `_PIXELSW`), or a scope
  on pin 152 of a real CD32 showing a colour-0 test screen.

### 6. CL450 output geometry: slaved to the Amiga's syncs, window set by the ROM

From the CL450 manual:

- The CL450 **generates no syncs**. It takes HSYNC and VSYNC as inputs
  (section 6.3) and outputs one pixel per VCLK on a 24-bit RGB or YCbCr bus.
  VCLK may be at most half of GCLK, and below 15 MHz it can be asynchronous to
  it (section 3, "VCLK - Video Clock").
- It decodes SIF (352x288 at 25 Hz, 352x240 at 30 Hz) and can **double it
  horizontally to 704 pixels**, repeating frames to 50/60 Hz (section 1.2.1).
- **The ROM places the picture.** `SetWindow()` (command `0x0406`, section
  11-42) sets the active window in VCLKs from the HSYNC edge and in lines from
  VSYNC, and outside it the CL450 outputs the `SetBorder()` colour
  (sections 6.3, 11). The phase 2 log captures both commands' arguments.

**Inference:** the connector offers `PIXELCLK`, `_HSYNC` and `_VSYNC`, so the
cartridge probably clocks the CL450 from the Amiga's pixel clock. 704 pixels
at the 14.19 MHz PAL hires clock is 49.6 us, about the visible part of a
52 us PAL line, which fits a hires-rate VCLK showing 352 pixels doubled across
the screen. If so, the core places the frame in hires pixels from its own
hsync, using the window the ROM sets, and doubles each MPEG pixel. WinUAE
instead centres the picture and ignores `SetWindow`.

**To confirm:** which clock reaches the CL450's VCLK on the FMV board, from its
schematic or a probe. Until then phase 3 uses the inference above, and the
phase 2 `SetWindow` log will show whether the numbers fit a 704-wide line.

### Also learned

- **Audio pins.** The connector carries the CD-DAC's serial stream (`DA_DATA`,
  `DA_LRCLK`, `DA_BCLK`, `17MHZ`), `_EXT_AUDIO` "External Audio Enable",
  `_MUTE`, and the audio pairs `LEFT`/`LEFT_EXT` and `RIGHT`/`RIGHT_EXT`
  (ECR p. 4-10). So the cartridge most likely substitutes its own analog audio
  for the console's while active, possibly with CD audio mixed in. Phase 4's
  "MPEG audio into the CDDA FIFO" matches the case where FMV audio replaces CD
  audio. Whether the cartridge also mixes in the CD audio it taps is unknown;
  it only matters if a disc plays both, and a Video CD does not.
- **The interrupt.** The connector has `/INT2` (pin 119) and `/INT6`
  (pin 120). WinUAE raises INT2 for FMV, which agrees with section 2 of this
  plan. Nothing here contradicts it.

## Phase 1: decode benchmark, userspace only

Build a standalone `tools/fmvbench`: libmpeg2 and kjmp2 cross-compiled with
the existing toolchain. It decodes a Video CD's MPEG track (a `.dat`, or
sectors read from a CHD) to RGB565 and PCM in memory, and times every frame.

Run it on the DE10-Nano pinned to ARM core 0 (Main pins itself to core 1),
twice: once idle, and once while the core runs a CD32 game that streams from
CHD.

**Gate:** PAL 352x288 at 25 fps, with the p99 decode time per picture under
30 ms, using no more than about 60 % of core 0, both runs. Also record the
worst picture and what it was (an I-frame after a seek, probably).

If it fails, try libavcodec's mpeg1video before giving up. If that fails too,
the plan stops: the only path left would be an FPGA decoder, and the fit has
no room for one.

### Result, 2026-11-02: PASS on the MiSTer

**Test stream: Cannon Fodder's own MPEG track.** The project's CHD
(`Cannon Fodder (1993).chd`) holds two tracks: track 1 `MODE1_RAW`, 27234
frames, and track 2 `MODE2_RAW` (2352 bytes), 12675 frames with a 225-frame
pregap. Track 2 is the MPEG track, which confirms the hypothesis in phase 0
item 3. It is raw 2352, so this title does not depend on item 4's 2336-byte
header fix. Extracted with `chdman extractcd` and cut at frame 27234;
fmvbench reads it as `raw-2352`. It holds a 352x288, 25 fps MPEG-1 stream:
4142 pictures (360 I, 1059 P, 2723 B), 166 s, with MPEG-1 Layer II audio at
44.1 kHz (6358 frames). It decodes cleanly on the build machine, every picture
displayed.

**Build:** the project toolchain (`arm-none-linux-gnueabihf` gcc 10.2, the
one `setup_default_toolchain.sh` fetches), `make` as written, static.

**Setup:** core `_Test/amigacd.rbf` loaded with `load_core`. Each run pinned to
core 0, `-n 3` (12426 pictures). For the idle run the config's bay was emptied
(`hardfile[0].filename[0] = 0`), so the core sat at the CD32 insert-disc
screen. For the busy run the config named Cannon Fodder's CHD at power-on.
Without a module the game skips its intro, and it was running its credits
sequence from CD throughout. The config was restored afterwards.

```
idle  FMVBENCH VIDEO accel=arm pictures=12426 displayed=12426 fps=25.000 p50_ms=7.863 p90_ms=9.297 p99_ms=11.558 max_ms=21.460 worst_index=10376 worst_type=I
idle  FMVBENCH LOAD content_s=497.04 video_work_s=100.047 audio_work_s=39.585 load_pct=28.1 wall_s=139.87 cpu_s=139.38 paced=0 late=0
idle  FMVBENCH GATE: PASS (p99 11.56 ms < 30 ms; load 28.1 % <= 60 % of one core)
idle  FMVBENCH VIDEO accel=c pictures=12426 displayed=12426 fps=25.000 p50_ms=7.945 p90_ms=9.328 p99_ms=11.639 max_ms=21.152 worst_index=5618 worst_type=B
idle  FMVBENCH LOAD content_s=497.04 video_work_s=101.004 audio_work_s=39.521 load_pct=28.3 wall_s=140.76 cpu_s=140.29 paced=0 late=0
idle  FMVBENCH GATE: PASS (p99 11.64 ms < 30 ms; load 28.3 % <= 60 % of one core)
busy  FMVBENCH VIDEO accel=arm pictures=12426 displayed=12426 fps=25.000 p50_ms=7.859 p90_ms=9.284 p99_ms=11.449 max_ms=19.738 worst_index=4958 worst_type=B
busy  FMVBENCH LOAD content_s=497.04 video_work_s=99.886 audio_work_s=38.942 load_pct=27.9 wall_s=139.06 cpu_s=138.52 paced=0 late=0
busy  FMVBENCH GATE: PASS (p99 11.45 ms < 30 ms; load 27.9 % <= 60 % of one core)
busy  FMVBENCH VIDEO accel=c pictures=12426 displayed=12426 fps=25.000 p50_ms=7.931 p90_ms=9.330 p99_ms=11.359 max_ms=17.903 worst_index=10856 worst_type=I
busy  FMVBENCH LOAD content_s=497.04 video_work_s=100.791 audio_work_s=39.128 load_pct=28.2 wall_s=140.15 cpu_s=139.68 paced=0 late=0
busy  FMVBENCH GATE: PASS (p99 11.36 ms < 30 ms; load 28.2 % <= 60 % of one core)
busy  FMVBENCH VIDEO accel=arm pictures=4142 displayed=4142 fps=25.000 p50_ms=7.891 p90_ms=9.508 p99_ms=12.109 max_ms=14.486 worst_index=2136 worst_type=B
busy  FMVBENCH LOAD content_s=165.68 video_work_s=33.597 audio_work_s=13.899 load_pct=28.7 wall_s=179.62 cpu_s=47.32 paced=1 late=0
busy  FMVBENCH GATE: PASS (p99 12.11 ms < 30 ms; load 28.7 % <= 60 % of one core; none late)
```

What this says:
- **Margin is wide.** p99 is about 11.5 ms against a 30 ms limit and a 40 ms
  picture period. Load is about 28 % of one core against 60 %. The paced run
  missed no display deadline.
- **The streaming game costs the decoder nothing measurable.** Idle and busy
  agree within run-to-run noise. Main stays on core 1, and Akiko's sector
  traffic does not reach core 0 in a way this measures.
- **libmpeg2's ARM assembler is worth nothing here.** `-a arm` and `-a c` are
  equal within noise. GCC 10 at `-O3 -mfpu=neon` matches the hand-written
  motion compensation, so phase 3 can take the plain C build.
- **The worst picture is not reliably an I-picture.** The maxima, 18 to 21 ms,
  land on I and B pictures at varying indices, which looks like scheduling
  noise rather than content. Only the max moves: p99 is steady.
- **Audio is a quarter of the work.** kjmp2 takes about 2 ms per Layer II
  frame, 39 s of the 140 s total. Video is about 8 ms per picture, of which
  the RGB565 conversion is 3.6 ms (p50). If phase 3 ever needs headroom, the
  conversion (NEON) and the audio decoder are where it is.
- **Akiko logged `STALL 1500 ms` lines throughout the busy run**, but at the
  same rate in the 50 s before fmvbench started. They are Cannon Fodder's own
  read pattern (the guest stops part-way through a multi-sector read), not
  something the benchmark caused.

**Phase 1 gate: PASS.** libavcodec is not needed. These lines are the
baseline phase 3 compares against, once frames go to uncached DDR3.

### Status, 2026-11-01: tool built and verified

**Built:** `tools/fmvbench` (fmvbench.c, with libmpeg2 0.5.1 vendored in
`lib/libmpeg2` and kjmp2 1.1 in `lib/kjmp2`; each `README.vendor` gives the
source, checksum, licence and the exact files taken). Like `latprobe` it is
static and comes out of the CI artifact next to `MiSTer`.

What it measures, per run:
- each **decoded picture**: libmpeg2 time plus the YUV 4:2:0 to RGB565
  conversion, by picture type (I/P/B), as p50/p90/p99/max, with the worst
  picture's index and type;
- each **audio frame** (kjmp2, Layer II);
- **load**: all decode work as a share of the content's own play time, i.e. %
  of one core at the stream's frame rate;
- with `-p`, **paced** playback in real time: pictures that missed their
  display deadline, and the real CPU share of playing it.

It ends with `FMVBENCH GATE: PASS|FAIL`, judged as above: p99 under 30 ms and
load at most 60 %.

**Verified** against ffmpeg (`tools/fmvbench/test_fmvbench.py`, in CI):
- the demuxed elementary streams are byte-identical to ffmpeg's for an
  MPEG-1 program stream, a hand-built one using every packet-header form, an
  MPEG-2 program stream, a VCD `.dat`, and raw Mode 2 Form 2 and Form 1
  tracks;
- I, P and B pictures match ffmpeg's decode in display order (PSNR about
  68 dB, i.e. IDCT rounding only);
- RGB565 is within one LSB of BT.601 limited range;
- every picture and audio frame is counted, and 25 and 29.97 fps are read
  correctly.

The ARM binary passes the same checks under qemu with both motion
compensation paths (`-a c`, `-a arm`). The test was falsified with ten
mutations of fmvbench.c, and each one failed the check that names it. One of
them (STD buffer fields) first survived, because ffmpeg's VCD muxer never
writes those fields; the hand-built stream was added for that reason.

**Host numbers are not the gate.** On the x86 build machine, a noisy 8 s PAL
VCD stream decodes at p99 0.8 ms per picture with 1.7 % load. That says the
tool works, not what an 800 MHz Cortex-A9 does.

**To run on the MiSTer** (the gate run):

1. Take `fmvbench` from the CI artifact, and a test stream: any Video CD
   `.dat` (`MPEGAV/AVSEQ01.DAT`), or a VCD-standard file made on a PC from real
   footage (synthetic content is too easy), for example
   `ffmpeg -i film.mkv -t 120 -target pal-vcd -bf 2 test.mpg`. Cannon
   Fodder's MPEG track is the real target; rip it as a raw `.bin` in the
   local PC session, and fmvbench reads it directly.
2. **Idle run**, core loaded, nothing playing:
   `./fmvbench -c 0 -n 3 test.mpg`, then the same with `-a c`, to see what
   libmpeg2's ARM assembler is worth.
3. **Busy run**, with Cannon Fodder or another CD32 game streaming from CHD:
   the same two commands, then once paced: `./fmvbench -c 0 -p test.mpg`.
4. Paste the `FMVBENCH` lines into this section.

**Not measured by this tool:**
- **Writing the frames into the core's DDR3.** fmvbench converts into
  ordinary memory. The core's frame buffer will be an uncached `/dev/mem`
  mapping. `arm-cpu-plan.md` step 1b measured a posted uncached write at
  8.6 ns, so a 352x288 RGB565 frame (50 688 32-bit writes) costs about
  0.44 ms, about 1 % of a core at 25 fps. Phase 3 measures it for real.
- **Demuxing.** It is done before timing and is trivial next to decoding
  (copies, and a start-code scan).

## Phase 2: the board, with a logging model

The interface between the two halves is fixed in
[`fmv-contract.md`](fmv-contract.md): the DDR3 window, the ring entry format,
the shadow block, local acknowledges and the Z2 RAM exclusion.

**Core** (`FringeCoder/AmigaCD_MiSTer`):
- `rtl/fmv.v`: the FMV card in the autoconfig chain, first. It decodes
  `$200000-$2FFFFF` once configured.
- The ROM and RAM live in DDR3, behind the existing DDR3 arbitration. They
  can't go in BRAM: 768 KB is more than the M10K left at ~50 % use. The ROM is
  loaded the way the other user-supplied ROMs are.
- The posted-write ring and the shadow registers as described above, and the
  INT2 request.
- Enabled by one OSD status bit, allocated when the phase starts (status[63]
  is the Action Replay's). With it off, the card is absent from the chain and
  the decode compiles to nothing that touches existing paths.
- A bench under `rtl/sim/fmv/`, printing `RUN: PASS`. It checks autoconfig
  placement at `$200000` with and without Z2 Fast RAM, write ordering through
  the ring, and that a shadow read never waits. Falsify it as the core's
  CLAUDE.md requires.

**Userspace** (this repo):
- `support/minimig/fmv.cpp`: drains the ring and runs the CL450 / L64111 model,
  written from the two chips' manuals (phase 0, item 1) and carrying no
  WinUAE code.
  It updates the shadows and logs every host command. No decoding yet: the
  model reports pictures as decoded, on schedule, without making any.
- Unit tests in `tests/` for the command parser and buffer fullness, fed with
  logged sequences.

**Gate:**
1. **Cannon Fodder** with the module on detects it and streams the whole
   intro into the model: the command log matches WinUAE's run of the same
   disc, compared by sequence, not by code. The game then reaches its title
   screen. With the module off it boots exactly as it does today, with no
   intro.
2. With a Video CD inserted, the FMV ROM boots into its player and runs a
   track to completion, with the same comparison, and the CPU doesn't hang.
3. With the bit off, the seed panel is unchanged. With it on, the panel is no
   worse than the noise line (~0.3 ns per family, `seed-lottery.md`), and no
   family that closed now fails.

### Status, 2026-11-02

| Piece | State |
|---|---|
| Contract (`fmv-contract.md`) | v2. The register file lives in the FPGA, with an update queue from Main; v1's shadow snapshot could not serve CL450 `HOST_raddr`/`HOST_rdata` reads |
| MODE2/2336 header fix | done: `akiko_raw_header()`, tested and falsified |
| Chip model (`fmv_model.cpp`) | done: both chips' host protocols, from the manuals; 10 mutations, all caught |
| Main transport (`fmv.cpp`) | next: map the window, load `fmv.rom` (log which dump; decode its autoconfig nibbles), drain the event ring, run the update queue, the OSD row |
| Core front end (`rtl/fmv/`) | next: the register file, both rings, the ROM/RAM window, INT2, the Z2 exclusion, and a bench that matches `tests/test_fmv_model.cpp`'s FPGA reference |

Unverified until the hardware log: the IO word bits (`fmv_board`), the
unit of HMEM `0xB` (words, as modelled), L64111 register lanes (low byte),
and the L64111 channel buffer counter addresses (19/20 or 25/26).

## Phase 3: picture

**Userspace:** feed the stream to libmpeg2 and write the pictures as RGB565
into a triple-buffered frame area in DDR3. Present them on the SCR schedule
above, and write the index of the current buffer, the border colour and blank
into shadows.

**Core:**
- A frame buffer reader that fetches one MPEG line ahead into a line buffer
  (352 x 16 bits, one M10K pair), at about 5 MB/s.
- The genlock mux, using the rule phase 0 found. Insert it where the test
  pattern is muxed in (`AmigaCD.sv` ~2965), on the mixer's input, so the
  scaler, shadow masks and analog out see one picture.
- If the rule is ZD, Denise needs a background-pixel output. That is an
  upstream-shaped file, so keep the change to one added output port and record
  it for the next sync.

**Gate:** each test disc plays with the picture correct, centred, with the
right border and no tearing, on HDMI and on analog out, PAL and NTSC. Timing
gets the same panel check as phase 2.

## Phase 4: sound

The L64111 outputs 44.1 kHz stereo, the same as CD audio, and a Video CD's
MPEG track has no CD audio playing alongside it. So the first thing to try
needs no RTL: Main writes kjmp2's PCM into the **existing CDDA FIFO**
(`cdda.v`, fed with `cdda_wr`). The L64111 mute and volume registers act on
that stream in Main.

If a title plays CD audio and FMV audio together, this doesn't work, and a
second stream into the mixer is needed. Phase 0's disc list will say whether
any title does.

**Sync:** audio PTS against the CL450 SCR, in Main, as the CL450 manual's
chapter 13 describes. Video is the side that gets dropped or
repeated: never stretch audio.

**Gate:** lips stay in sync over a full Video CD (>= 60 min), audio and video
drift under one frame, and the CDDA underrun and starve counters stay at
zero.

## Phase 5: product

- An OSD row in the AmigaCD console: **FMV module: Off / On**. It is greyed
  out unless `fmv.rom` is present. Add an `.mgl` key ([`mgl.md`](mgl.md)).
- **Save states are refused while the module is on.** The MPEG decoder state
  lives on the ARM and isn't in the save vector. The 68030 MMU was handled the
  same way before it fitted.
- The decoder runs on core 0 alongside the ARM CPU option's emulator, and both
  can't run at once. Refuse the combination in the OSD.
- Entries in [`hardware-test-plan.md`](hardware-test-plan.md), release notes,
  and a status line in the core's README feature table.

**Gate:** the hardware test plan rows pass on the test machine, from CHD and
from Real CD.

## Risks, ranked

1. **Genlock fidelity.** Nobody seems to know the rule, WinUAE included. A
   wrong rule shows as menus over video drawn wrongly, not as a crash, and
   some titles may depend on it.
2. **Timing closure.** Every RTL phase re-rolls placement. The mux on the
   video path and the DDR3 reader are the likely costs. Judge them by the
   panel, never by the best seed.
3. **The model is new code.** It is written from the manuals, so it has none
   of WinUAE's years of fixes, and the cartridge's board wiring is not in any
   manual. WinUAE as an oracle and the phase 2 logs are the safety net. Two
   titles that drive the cartridge differently (the Cannon Fodder intro and
   the ROM's Video CD player) make a gap visible early.
4. **The L64111 manual is preliminary** (its own words), and inconsistent
   in one place (the Channel Buffer counter addresses). Its interrupt status
   and mask bits sit in different positions, which is easy to get wrong.
   The notes file flags both, and phase 2's logs check them against what the
   ROM actually does.
5. **Core 0 contention.** Linux timer and interrupt work already land there
   (`arm-cpu-plan.md`, step 1). Phase 1's busy run is what tests this.
