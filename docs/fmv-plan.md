# The CD32 FMV module: plan

**Date:** 2026-11-01
**Status:** phase 0 done on paper (2026-11-01); phase 1 passed on hardware
(2026-11-02); phase 2 built, and gate 1 passes on hardware (stream 2026-11-04,
run v21; repeat boots and warm resets 2026-11-05, run v22); gate 2 pending.
Phase 3: the picture works on hardware (Cannon Fodder, HDMI PAL, run v23,
2026-11-05); analog out, NTSC, a Video CD and tearing in motion not yet seen.
Phase 4 (sound) runs on hardware (run v24: every frame played reached the
FIFO), not yet heard. Phase 3 and 4 run on a closed-timing build. Phase 5
(OSD row, the migration, save states outside playback) passes on hardware
(run v25). **Run v26 found the intro playing at 2/3 of its speed**, which
v21-v25 had missed; the stream is not yet at full rate. v28 traced it to
the 68k waiting on ROM fetches from DDR3; a ROM cache is the fix in test.

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

**Running gates 1-2 on the DE10-Nano** (learned on the first run):
- Under `[AmigaCD]` in `MiSTer.ini`: `AMIGACD_FMV=1`, and `DEBUG=2`. With
  `DEBUG=0`, Main's stdout is `/dev/null` (`cfg.cpp`) and the whole FMV log
  is lost; with 2 it goes to `/tmp/debug.txt`.
- A test binary goes in `main=` as the binary itself. Main restarts whenever
  `main=` does not name `/proc/self/exe` (`user_io.cpp`), so a wrapper script
  loops forever.
- The ROM: `CD32FMV.rom` or `fmv.rom` beside the Kickstart the config loads,
  else in `HomeDir()` (`games/Amiga`), else in the root folder. A failed
  search logs where it looked.
- What a good start looks like: `FMV board alive`, then within a moment
  `FMV: the core is reading the update queue`. If instead `the core has not
  taken the update queue in 2 s` appears, the board cannot join the chain,
  and the line gives the three pointers, followed by up to three
  `FMV diag:` lines a second apart: the core's own view, read over SPI.
  After the Amiga reset, the first 68k access shows up as an event.

### Status, 2026-11-02

| Piece | State |
|---|---|
| Contract (`fmv-contract.md`) | v4: a session starts from zero (below). The register file lives in the FPGA, with an update queue from Main (v1's shadow snapshot could not serve CL450 `HOST_raddr`/`HOST_rdata` reads); v3 adds `UQ_RDPTR` and the RDIDX back-pressure |
| MODE2/2336 header fix | done: `akiko_raw_header()`, tested and falsified |
| Chip model (`fmv_model.cpp`) | done: both chips' host protocols, from the manuals; 10 mutations, all caught |
| Main transport (`fmv_transport.cpp`, `fmv.cpp`) | done: maps the window, loads `CD32FMV.rom` or `fmv.rom` (logs the dump, decodes its autoconfig), drains the event ring with the SEQ check, runs the update queue; `AMIGACD_FMV=1` in the ini. Tested against the shared FPGA reference (`tests/fmv_fpga_ref.h`). The OSD row moves to phase 5 |
| Core front end (`rtl/fmv/`, AmigaCD_MiSTer 53e6d3f) | done: register file, both FIFOs, the ROM/RAM window, INT2, the Z2 exclusion, Gary's decode; `tb_fmv.sv` 80 checks, 13 mutations all caught; rtl-sim green |
| Gate 3, the panel | seeds 1-20 (seed-lottery row 42): 2/20 close against row 41's 1/20, median setup -0.164 against -0.2605; no touched family near zero. **Worst hold -0.398 against +0.006**, not yet attributed: the FMV crossings have no SDC exceptions, and one is added only from a path report |
| Gate 1, first hardware run (2026-11-02) | **failed, cause found.** Core seed 11 of fit run 41463624329, Main e2ab3b16. Main loaded ROM 40.30, decoded its autoconfig (`d5`, 1 MB Zorro II, 0202/6A) and queued `ALIVE`; after the reset, expansion.library's BoardList held only the Toccata and the A2065, and Main logged no event at all. Cause: v3 had Main resume from the window's `UQ_RDPTR`/`EV_WPTR`, but DDR3 is not cleared between core loads, so Main queued from a stale index while the fresh core waited at entry 0. `ALIVE` never landed, and `ac_present` stayed low. Fixed by contract v4 on both sides: `test_stale_window` reproduces it on the host and `tb_fmv.sv` M on the RTL (89 checks, 17 FMV mutations caught). Also from this run: the ROM is looked for beside the loaded Kickstart, and the run notes above |
| Gate 1, second run (2026-11-02 evening) | **failed, cause not yet found.** Core seed 4 of fit run 41481408089, Main 8a2713b5. Main's side of v4 is right: the window, read back from the ARM, held 260 queued updates with matching SEQs, at the addresses fmv_ddr uses. The core never wrote `UQ_RDPTR` (0) or an event (`EV_WPTR` 0), and the new 2 s line fired. Ruled out by the owner: status[64] delivery, the clocks, readdatavalid routing, synthesis removing fmv_ddr. Ruled out here: the whole DDR3 path is fine in simulation when otherwise idle (`tb_fmv_chain.sv`: fmv_arb, ddram_ctrl mem2, q2, its arbiter and ddr_cmd_pipe take all 260 entries). Next: the core's FMV state over SPI (hps_ext `0xFA00`, independent of DDR3), printed at the start of a session and three times once the queue stalls. Its fmv_ddr state, dwell count and poll/beat counters say whether the master is stuck waiting on the arbiter, never on, or never polling |
| Gate 1, diag run (2026-11-02 late) | **narrowed.** Core seed 17 of fit run 41503695620, Main b4b4c785. The SPI window shows status[64] reaching fmv_ddr (`on 1`), and its first `UQ_WPTR` read held in `S_QW` with `waitrequest` high from the start: 65535+ clocks, 0 polls accepted, 0 read beats. fmv_ddr and the contract are cleared; a level above it never grants. `tb_fmv_chain.sv` with heavy m0 traffic on fmv_arb still passes, so this is something the bench does not generate. Next: the four arbiters' state (fmv_arb, mirror_arb, dsnd_arb, ddram_ctrl's own and its q2 skid) on the same window, after a `0xA5F5` marker, decoded on the same `FMV diag:` line |
| Gate 1, arbiter diag run (2026-11-02 night) | **the core's whole DDR3 port is wedged, not the FMV path.** Core seed 6 of fit run 41518049646, Main b5968fb9. Since configuration the port has seen five grants, all from the A2065 mailbox (its pointer publish, then INT/CSR polls in the 0x1FF08000 window). Four completed. The fifth read sits in ddr_cmd_pipe (`hs_waitrequest 1`) with the HPS port's waitrequest (`DDRAM_BUSY`) stuck at 1, so the HPS stopped taking commands. No beat was lost: ddr_cmd_pipe is one deep, loads only when empty and clears only once the port takes the command. Reproducible across a second load_core. The FMV read, and an A2065 write behind it in q2, wait for good. Next, on the same bitstream: `AMIGACD_FMV=2` (diag only, the board never on) says whether the port wedges with FMV idle. A new fit adds the HPS port's own counters (commands accepted, beats returned, the last command and the one on the pins, how long BUSY has been high) |
| Gate 1, diag-only run (2026-11-02 ~22:10) | **FMV cleared.** Same seed 6 core, Main 5b3a77b5, `AMIGACD_FMV=2`: status[64] never set and fmv_arb m1 never granted, and the port was wedged at the first sample. That was 4 grants, all A2065 mailbox commands, the 4th read taken by the HPS with no beat back and `DDRAM_BUSY` high from then on. Every DDR3 master in the core is held by `reset_d` from sys `RESET`. Main enables the bridges (`do_bridge(1)`) before the new Main releases that reset, as upstream does, and writes the SDRAM controller nowhere else. Next: the HPS's own view on every diag line (SDR dramsts, fpgaportrst, protportdefault, brgmodrst, fpgaintfgrp_module), and `AMIGACD_FMV=3`, which pulses the f2sdram port reset once at 3 s to see whether the port recovers |
| Gate 1, HPS register and port reset runs (2026-11-02 ~22:35) | Main e315fda9, same seed 6 core. With the port wedged, the HPS registers look normal: dramsts 1 (calibrated), fpgaportrst 0x3FFF, brgmodrst 0, fpgaintf 0, protportdefault 0x3FF. `AMIGACD_FMV=3` (a port reset pulse with the core running) clears `DDRAM_BUSY` and drains q2, but the lost beat never returns, so the core's arbiters stay busy. The board survives the pulse. Protection rules look unlikely: every A2065 poll goes to the same two addresses, and the first ones succeed. Next: the rules dumped anyway, and `AMIGACD_FMV=4` (core reset on, port reset pulse, core reset off). That restarts both sides with the PLL long locked. If traffic then keeps flowing, the port went bad at the bridge enable during the load |
| Gate 1, HPS-counter run (2026-11-02 ~23:00) | Core seed 11 of fit run 41533469818, Main cb626029. **The HPS has never returned one read beat to the core's ram1 port since configuration**: 0 beats against 4 commands accepted, and 0 against 9 after a core and port reset. The reads are to the right place: word 03fe1003, byte 0x1FF08018, the A2065's INT_STATE, inside the one SDR rule's 1 GB. The held command is the mailbox's pointer write to 0x1FF08008. Main's first decode printed 0x80xxxxxx addresses: it had the two address words of each command swapped, and its test shared the mistake; both are fixed. sys was checked: the f2sdram atom has never been changed by the fork, and the terminator passes readdatavalid and waitrequest straight through outside a core reset. In the CD32 preset (no fast RAM) nothing else in the core reads DDR3, so broken ram1 reads would have gone unnoticed. Next: does any read on ram1 return in a config where the CPU uses DDR3 fast RAM? |
| Gate 1, after a power cycle (2026-11-03) | **the DDR3 port is healthy, and the board works up to the first CL450 command.** Same seed 11 core, Main 48571579. With `AMIGACD_FMV=2`, every A2065 read returns (50964 accepted, 50964 beats). Yesterday's dead port was board state: a core reload and a port reset did not clear it, a power cycle did. Main now says so loudly when it sees it again. With `AMIGACD_FMV=1`, Cannon Fodder and an Amiga reset: the queue is taken, the RESET event arrives, and the log shows the IO writes, the CL450 software reset, the ~3300-line microcode upload, the CPU started at CPU_pc 0x005d, initialisation complete, then HOST_control 0x0081, CMEM_dmactrl 4 and `SetThreshold (0x0103) 2000`. After that the log is silent for over a minute and the screen is black. WinUAE has nothing pending at that point either: no NewPacket or Play, interrupt mask 0, no interrupt. So the next host action is the game's, and the question is whether the 68k is alive. Main now prints an activity line every 10 s while the board is on: all accesses, including the HOST_newcmd, CMEM_dmactrl and HOST_control polls the log leaves out, and where the CL450 stands. The diag line follows each one. One oracle difference fixed: the ROM's first IO read was 0xc400, and WinUAE's idle value is 0xc800 (CFLEVEL 1, FALF bit 0) |
| Gate 1, streaming run (2026-11-03 ~19:25) | Main 75f09f17, same seed 11 core. **The board streams.** IO reads 0xc800 now. The ROM sends SetThreshold, SetBorder, SetVideoFormat, SetInterruptMask 0x0d03, SetBlank and Play. 138 NewPacket + InquireBufferFullness pairs follow over about 4 s: about 60k CL450 data words and 3.9 kB of L64111 data a second. The L64111 syncs at 44.1 kHz, and 50 pictures and 12 audio frames are decoded. Then the 68k goes silent: 0 accesses for a minute, so it is asleep. Cause, in the model: UND was raised on the first decode tick without a complete picture, which is right at Play. That latched 0x0100 in Interrupt Status (HMEM[0xA]). This ROM's ISR only rewrites HOST_control and never clears UND, so every later interrupt (RDY, PIC-V: pending 0x0502) queued behind it. WinUAE raises UND only once the buffer has stayed empty. Fixed: UND only after two decode ticks with no picture data. Also from the oracle, HMEM[0xB] is now in bytes, which closes the open unit question. The core and transport held throughout: 18507 updates, 11437 events, no port warning |
| Gate 1, runs v13/v13b (2026-11-03 ~19:50) | Main e42c2e42. Two findings. (1) On a first reset the ROM sometimes stops after its own CL450 init (SetThreshold, then no SetBorder): the game did not reach its FMV path. That is boot or game-path variance, not the model. (2) On a second reset the game streamed for about 6 s: 269 NewPackets, 102 pictures, 27 audio frames. Then it slept the same way. UND still fired at Play, because the buffer is truly empty in the gap before the first NewPacket, which this ROM sends only after servicing Play's interrupt. The driver read HMEM[0xA] = 0x0100 and never cleared it. Fixed: UND only once data has arrived since Play, a flush or a reset. RDY had the same fault. It now follows the oracle: NewPacket mode, and fullness below the threshold (the first version used free space above it). So nothing interrupts at Play before data flows |
| Gate 1, run v14 (2026-11-03 ~20:05) | Main a1379974. The game reached its FMV code on the first reset. With no UND and no RDY at Play, **no interrupt came at all**. The ROM pushed about 1 s of audio, polled IO and the L64111 status about 22 times a second, and sent no NewPacket in 90 s. So it waits for an interrupt after Play before it sends video; v12 and v13b had started from the spurious UND. The oracle explains it. WinUAE posts PIC-D from its display timing every frame, from microcode start, in every state and before any data. Its post writes the enabled pending bits to HMEM[0xA] and asserts INT whenever HMEM[0xA] is 0, even with nothing enabled pending, so this ROM (mask 0x0d03, PIC-D off) gets a status-0 interrupt every frame. It never raises PIC-V or GOP. The model now does the same: WinUAE's post semantics, a 40 ms PIC-D tick while the microcode runs, and no per-picture PIC-V/GOP, which this ROM would never clear. RDY and UND keep their data gating. Status-0 INTs are counted on the activity line |
| Gate 1, run v15 (2026-11-03 ~20:20) | Main 536fa13f. The display tick works: 25 INTs a second, each serviced (R IO, W HOST_control 0x0081, R IO, HOST_raddr 0xA, R HMEM[0xA] = 0). But with status 0 the ISR does nothing, and no NewPacket came in 3 min. In v12/v13b the same ISR read 0x0100 and went straight to NewPacket. The oracle, asked again, settles UND: WinUAE's audio tick (every 64 lines) counts empty-buffer ticks to 2, resets the count on data, and from then on posts UND on every empty tick, not gated on Play. So after Play the first tick posts 0x0100. It stays there, because only a host write clears HMEM[0xA], and this ROM never writes it. That is the start signal, and in WinUAE too the driver then streams by polling (NewPacket + InquireBufferFullness). The data gating of the last two versions took it away; it is gone, replaced by WinUAE's rule, and the decoder now empties the buffer once no picture is in progress. So v12/v13b's stall after 4-6 s is not the CL450 status, which is latched in WinUAE too. Audio stopped at the same moment, so the next suspect is the L64111 side, which the activity line now shows |
| Gate 1, run v16 (2026-11-03 ~20:40) | Main 168b5c54. UND as the start signal works: INT 0x0100 at Play, then about 6 s of stream (257 NewPackets, 97 pictures, 25 audio frames). Then silence. The Akiko log shows the game throttling the CD itself: a burst of about 50 sectors, PAUSE, and a resume about 250 ms later, until the last pause has no resume. RDY was pending but could not post behind the latched 0x0100. WinUAE latches the same 0x0100, so the resume cannot come from the CL450 there either. The oracle's L64111 is the difference: INTR is the level of status & mask, a read clears the whole status register, and PTS AVAILABLE comes with every audio PTS once synchronised. The model raised INTR only at the instant a bit was set with its mask already on, kept the sync and PTS bits through reads, and raised PTS AVAILABLE once per session. At the stall, status 2 was 0x38 and mask 2 was 0x42 (PTS AVAILABLE enabled), with INTR 0. All three now follow the oracle. Seen in passing, not FMV: every PAUSE leaves the Akiko pushed count one ahead of fpga_ctr; worth a look later |
| Gate 1, run v17 (2026-11-03 ~20:55) | Main 7ba6cd25. **The CD resume works now.** The Akiko log shows the throttling loop resuming past v16's death point, presumably on the L64111 PTS interrupt: 270 NewPackets, 103 pictures. The next stop came 17 sectors into the following burst, with 0 accesses after and the model holding L64111 INTR 1 (status 2 0x05: NEW FRAME OUT and FRAME DETECT IN; NEW FRAME OUT is enabled by mask bit 1, so the level is right). The ROM's last 36 IO reads all show the L64111 bit inactive, though, and with INT2 up its ISR would have read IO again. So the model and the FPGA probably disagree about the latch. Main cannot see the FPGA's side, so: the core's diag window (layout 0xF402) now carries INT2, the L64111 latch and the CL450 INT. The model re-sends a raised latch every 20 ms while the level holds: the contract's "raise it again once caught up" that the model never did. It also drops the latch when a mask write disables what is pending, so the line follows the level both ways |
| Gate 1, runs v18/v19 (2026-11-03/04) | v18 (Main 47c17186) stopped at v12's exact point: 138 NewPackets, then InquireBufferFullness 0900 0000 0004 78d0, then nothing, with the CD STALLing at the same moment (the guest stopped asking for sectors). v19 (core 654326a seed 11, Main 95d47bdf) adds the Amiga's side and answers it. **At both stalls INTEN (INTENA bit 14) is clear**: 0x203c, against 0x602c while working. Every Paula source sits pending in INTREQ (PORTS, VERTB, COPER, BLIT), Akiko's IRQ is pending, and the board's INT2 is 0 on all 35 lines. The first-reset stall after SetThreshold has the same signature. So this is no INT2 storm: the 68k is spinning in a Disable()d section, in an Alert (which disables), or stopped. The CD STALL follows from that. Next: the 68k's PC, a ring of four PCs about 1.15 ms apart, and its halt flag on the same window (core ae9912f), decoded with a region (chip RAM, FMV ROM, FMV RAM, ext ROM, Kickstart) |
| Gate 1, run v20 (2026-11-04) | Core ae9912f seed 8, Main e94db5a2. **Found.** Both stall types park at PC 203cfe in the FMV ROM (`move.w $56(a0),d0`, the driver's poll of HOST_newcmd), with all four ring samples on that one instruction. The 68k is stuck inside the read cycle, with fmv_bus IDLE and the update FIFO full, while the driver's own Disable() keeps INTEN off. The cause is the back-pressure deadlocking on itself. When Main is 4095 events behind, fmv_bus holds a ring-bound access until Main's HOST_RDIDX update makes room, but it applied updates only with no access pending, so the held access blocked its own release. A microcode upload or a burst of streaming while Main is busy gets that far behind. That also explains the deterministic stop points, the stall after SetThreshold, and the wedge surviving Amiga resets, since each boot uploads again. tb_fmv.sv had passed only because its bus task timed out and dropped AS. Fixed in AmigaCD_MiSTer 6315b70: updates apply while an access is held. The bench now requires the release before the timeout, and a mutation restoring the old priority is caught |
| Gate 1, run v21 (2026-11-04) | Core 6315b70 seed 8 (setup -0.119, not closed), Main e94db5a2. **The stream part of gate 1 passes.** The first OSD reset reached the game, with no SetThreshold stall; that is one boot, not yet proof it has gone. The intro streamed to its end: 10728 NewPackets, 4142 pictures (about 166 s at 25 fps), 6293 audio frames in and 6252 out. The 68k PC kept moving, and fmv_bus stayed serviceable. At the end the driver shut the board down in order (Reset, FlushBitstream x2, Reset, SetVideoFormat), board accesses fell to 0, and the game loaded and reached its recruit screen, rendering correctly. Not looked into yet: 5 underflow lines, 12 Akiko COUNTER AHEAD lines in the loading after the intro (with no no-FMV control run to compare against), the 41-frame audio in/out gap at the end, and loading time. Next, in order: a closed-timing build (the diag taps of seed-lottery rows 44-50 now behind FMV_DIAG, off by default), repeat boots on it to confirm the SetThreshold stall has gone, then phase 3 |
| Gate 1, run v22: repeat boots (2026-11-05) | Core 039417d seed 13 of fit run 42018582321 (the FMV diag taps behind FMV_DIAG, off; setup -0.017, hold +0.169, not closed by 17 ps), Main 2eb40143. **Gate 1 passes.** 8 of 8 boots (load_core, then one OSD reset) reached the stream, with no SetThreshold stall. 3 of them logged two SetThresholds and SetBorder within 1 s of the reset, read by the owner as the core's own first boot reaching the game before the reset restarted it (not confirmed). Every stream ran at about 51 packets/s. 5 of 5 warm resets in mid-stream recovered (no load_core, five in a row), carrying on at 52-60 packets/s. Before 6315b70 this case wedged 7 of 7 times. One full run: 10647 NewPackets (v21 10728), 4142 pictures (v21 4142), audio 6288 in and 6251 out, driver shutdown 165 s after stream start, recruit screen rendering correctly. The game took 170-270 s to load after the shutdown, and the last Akiko read is at the same base as v21. Of v21's open items, 0 COUNTER AHEAD lines this time (12 in v21), so that varies from run to run; no no-FMV control was run. 5 underflow lines again, 2 at startup (first SetThreshold, Play) and 3 in the shutdown, none mid-stream. The audio in/out gap is 37 frames (v21 41). The NewPacket count differs by 81 while the picture counts match. None of these blocks gate 1. Forty seeds of this netlist (rows 51 and 51b) give no closing build: ascal, upstream, holds 19 of 20 in both panels, so the gate is not held for one. Not tested: a pad in game, any disc but Cannon Fodder |
| Gates 1-2, hardware | gate 1: passes (stream v21; repeat boots and warm resets v22, on a build 17 ps short of closing). Gate 2 (a Video CD) not yet run: Cannon Fodder and a Video CD, with the command log compared against WinUAE |

Unverified until the hardware log: the IO word bits (`fmv_board`; the idle
word now follows WinUAE, 0xc800), L64111 register lanes (low byte),
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

### Status, 2026-11-05: built, not yet run on hardware

Contract v5 ([`fmv-contract.md`](fmv-contract.md), "The picture") is the
interface. What was built against it:

- **Main.** The model hands every bitstream byte it accepts to
  `fmv_decoder.cpp`: libmpeg2 on a thread pinned to core 0. Each picture, in
  display order, is cropped to the CL450's output window and written as
  RGB565 into one of four frame buffers at `0x27200000`. The model's picture
  schedule, the one gate 1 passed on, names the next ready buffer. A buffer
  is not reused until a whole present after it was replaced, so the core
  never shows one being written. `fmv_picture.cpp` turns `SetWindow()`,
  `SetBorder()`, `SetVideoFormat()`, `SetBlank()` and the sequence header
  into the display registers, following the manual's units and its
  auto-size and auto-centre rules (pp. 11-29 to 11-44). The board's "CL450
  video on" IO bit (`0x4000` as written, from WinUAE's names and so
  unverified) switches the picture on. The window is blanked to the border
  colour in IDLE, under `SetBlank(1)`, and until the first picture.
- **Core** (`FringeCoder/AmigaCD_MiSTer` `rtl/fmv/fmv_video.v`). It counts
  from the core's own `_hsync`/`_vsync`, as the CL450 counts from the
  Amiga's. It fetches each window line a line ahead into a two-line buffer,
  shows the border colour outside the window, and latches the registers at
  VSYNC. `minimig.v` selects it over Denise's pixel where Denise's new ZD
  output (Lisa's default mode) is asserted.
- **Tests.** `test_fmv_picture` covers the window rules, the registers, the
  buffer ring, the conversion and the model's hooks. `test_fmv_decoder`
  covers the real decoder on a generated 9-picture I/B/P stream: display
  order, colours, back-pressure, flushes, crops, and the shown buffer kept
  intact. 20 mutations of the Main side, all caught; five were not at
  first and their tests were tightened. In the core, `tb_fmv_video.sv` plus
  13 mutations.

**Calibration, under `[AmigaCD]` in `MiSTer.ini`.** The manual's auto-centre
assumes leftBorder 60 and topBorder 18 for the CL450's own video timing. On
the core that put the picture's left off screen; run v23 measured 93 display
pixels and 3 lines more, now built in (`FMV_CALIB_HOFS`/`VOFS` in
`fmv_picture.h`). The ini values add to those:
- `AMIGACD_FMV_HOFS=n`: display pixels (hires) added to the left edge,
  -512 to 512.
- `AMIGACD_FMV_VOFS=n`: lines added to the top edge, -128 to 128.
- `AMIGACD_FMV_SHOW=1`: the picture on whatever the IO word says (if the
  video-on bit turns out to be another one). `=2` also ignores the ZD key, so
  the whole screen shows the cartridge's output: the window and the border
  colour around it. That is the view to measure the offsets in.

Main prints `FMV picture window: WxH from (x,y) of the picture, at display
pixel X, line Y` whenever the window changes, `FMV picture: on/off` whenever
those change, and a `FMV decoder:` line every 10 s with bytes in, pictures
decoded and shown, ticks with nothing new, and dropped bytes.

### Run v23, 2026-11-05: the picture works

Core dddc7d5 seed 19 of fit run 42047243339 (setup -0.013, hold +0.183, not
closed), Main f4fc3f4c. Cannon Fodder, HDMI, PAL.

- **The intro plays start to end with the picture right**: colours, aspect
  and content correct in about 25 stills, the 352x288 window as expected.
  After the intro the picture goes off and the Amiga's own screens return.
- **`0x4000` is the video-on bit.** `FMV picture: on` came with
  `AMIGACD_FMV_SHOW=0`, no override needed.
- **ZD's default mode is the key this intro needs.** With `SHOW=0` no holes
  or Denise pixels showed in the window, and its bounds match the `SHOW=2`
  run (by eye and by bounds, not pixel by pixel). Outside the window the
  screen reads (4,4,4) while streaming in both, the ROM's border colour.
- **Offsets**: at 0/0 the left was off screen and the top clipped a few
  lines. `HOFS=93 VOFS=3` centres it, 23 px left and 24 px right in a
  720x286 screenshot (window at display pixel 153, line 21). Now the
  built-in default (above).
- **Decoding keeps up**: 24624480 bytes in, 3 sequences, 4168 pictures
  decoded, 4164 shown, 5 late ticks, 0 bytes dropped, 5 flushes. 27 of the
  pictures came from the stream before the OSD reset, leaving 4141 against
  the model's 4142. That one is not looked into. A candidate: the
  driver's closing Reset()/FlushBitstream() drops a picture that was
  decoded and not yet shown.
- **Boots**: 6 of 6 load_core boots reached the picture. **Warm resets**: 3
  of 3 mid-stream OSD resets recovered, the picture back each time.
- **The stream** matches v21/v22: 10711 NewPackets, 4142 pictures, audio
  6292 in and 6251 out. Underflows as in v22: 2 at the start and 3 at the
  end. The 12 Akiko COUNTER AHEAD lines (12 in v21, 0 in v22) are all in
  the post-intro load, so it varies from run to run. Not investigated.
- The game loaded for about 3 minutes after the intro and reached the
  credits. The run was stopped there, before the recruit screen v21/v22
  reached.

Not yet seen: tearing in motion (stills only, none torn), analog out, NTSC,
a Video CD, other discs, a late fetch under load, and the ARM's load while
decoding. Timing: seed-lottery row 52. No seed closes, the DDR3 families
lost about 0.2 ns, and hold fails on 3 of 20 seeds, not yet attributed.

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

### Status, 2026-11-05: built, not yet heard on hardware

No RTL, as planned. Main writes the decoded sound into the existing CDDA
FIFO (`cdda.v`), which plays whatever it is given, mixed with Paula at full
gain in CD32 mode.

- **The model** hands over each whole Layer II frame as the L64111 takes it
  into its channel buffer (header included, across packet boundaries). It
  ticks each time the chip puts a frame out, and flushes on BUF_RST, a soft
  reset or a board reset.
- **`fmv_audio.cpp`** decodes with kjmp2 on a thread on core 0, beside the
  video decoder. Phase 1 measured about 2 ms a frame, which is too much
  for Main's own thread at 38 frames a second. Frames are numbered in
  stream order. **The tick is the clock**: each one adds that frame's 1152
  stereo samples to the output, or 1152 of silence if the frame is not
  decoded yet, the chip is muted, or the stream is not 44.1 kHz (the FIFO's
  only rate). So the sound never drifts from the chip, and the game paces
  its CD reads on the chip's interrupts, which follow the same schedule.
- **Mute**: the L64111's SOFT_MUTE (control 2 bit 5) or the board's IO
  word bit `0x0200` as written (WinUAE's name, unverified). Each plays as
  silence and keeps its time.
- **Into the FIFO**: a 588-frame sector at a time whenever the core asks
  (`cdda_req`) and CD audio is not playing. CD audio owns the FIFO while it
  plays (`akiko_cd32_pcm_room`). More than eight sectors (about 107 ms)
  waiting means the FIFO is not taking them, and the output is dropped
  rather than left to trail the picture.
- **Tests**: `test_fmv_audio` covers decode order and channels against a
  continuous kjmp2 decode, the tick schedule, silence for a missing,
  muted or 48 kHz frame, flush and drop, and the model's frame and tick
  hooks and both mutes, on generated 44.1 and 48 kHz Layer II streams. It
  passes under ASan/UBSan and ThreadSanitizer. 10 mutations, all caught;
  one was not at first, because the flush test had not decoded the frame
  it flushed.

**Sync, a change from the plan.** The plan said to sync audio PTS against
the CL450 SCR in Main. What is built instead paces both halves from the
model's own schedules: pictures at the sequence's frame rate, audio at the
stream's sample rate, from the same clock. Both started together, and in
v23 the picture kept up (5 late ticks in 166 s). That keeps the two in
step as long as neither schedule drops anything. A PTS/SCR loop is what
fixes it if they drift. The gate's 60-minute Video CD will say whether
that is needed.

The FIFO's own latency (up to 46 ms in its normal mode) delays the sound
against the picture by a constant. It is under the gate's one frame at
25 fps only at the FIFO's low end. If it shows on hardware, the fix is to
hold the picture by the same amount, not to cut the FIFO.

Main prints a `FMV audio:` line every 10 s: frames in, decoded, bad,
played (missing, late, muted, not 44.1 kHz), sectors to the FIFO, polls
blocked (FIFO full / CD audio), samples dropped, flushes.

### Run v24, 2026-11-05: the sound path runs; not yet heard

Core 4fd09c1 (RTL e14af8b) seed 5 of fit run 42068080918, **the first
closed-timing build with the picture** (setup +0.195, hold +0.250). Main
676bc001 with the built-in offsets. Cannon Fodder, HDMI, PAL. Read from the
counters only: quality, level, mute and lip sync wait for someone to
listen.

- **Audio**: 6285 frames in, 6274 decoded, 0 bad, 6245 played (12 late,
  0 muted, 0 not 44.1 kHz), 12235 sectors to the FIFO, 0 samples dropped,
  7 flushes. 6245 x 1152 / 588 = 12235: everything played reached the
  FIFO. The 12 "late" counted both kinds of silence. They came 1-6 through
  most of the stream and 6 to 11 in one 29 s stretch near the end. From
  this run on the line splits them: `missing` (the frame had not arrived
  when the chip played it) and `late` (it had, and was not decoded yet).
  697 polls blocked, in bursts; now split into FIFO full and CD audio.
  The 40 frames between in and played at the end match v21/v22's in/out
  gap, read as flushed at shutdown, not confirmed. The stream line's 6286
  in against the audio's 6285 is off by one, not looked at.
- **The picture with sound on**: 4142 pictures, 4140 shown, 2 late, 0 bytes
  dropped. 4142 matches the model exactly this time (v23 was one short).
  The sound cost the picture nothing measurable.
- **Placement**: the built-in offsets put the window at display pixel 153,
  line 21, the same bounds as v23's HOFS 93 / VOFS 3.
- **Boots**: 5 of 5 boots whose reset reached the Amiga got picture and
  sound. 2 of 2 mid-stream warm resets recovered, and a reset from the
  recruit screen reached the stream too.
- Underflows: 4 lines, 1 at the start and 3 at the end. COUNTER AHEAD: 12,
  as in v21 and v23.
- **Without the board**: twice the harness's reset did not reach the Amiga
  ("reset sent", no `FMV Amiga reset`). The board was never configured,
  and the game skipped the intro and reached the recruit screen by itself
  within about 150 s. So the game runs without the board.
- **Harness**: with `DEBUG=2`, each load_core left `/tmp/debug.txt` open
  across Main's re-exec (23 fds on deleted copies after a day), and
  `/tmp` filled. Fixed: the log is opened close-on-exec, as `/dev/null`
  already was.

Not yet: hearing it, A/V sync, either mute path (0 muted frames), CD audio
taking the FIFO back, the 60-minute Video CD gate, analog out, NTSC,
tearing in motion, other discs.


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

### Status, 2026-11-05: built, not yet run on hardware

- **The OSD row** is on the Settings page, not Advanced: the cartridge was a
  real CD32 accessory, and Advanced is for what the machine never had. It
  sits beside CD speed, in the line the spacer above CD speed had (the page
  has no line to spare). `FMV module : Off/On`, or greyed with `N/A (CD32
  only)`, `N/A (ARM CPU)` or `N/A (no CD32FMV.rom)`. The board appears at
  the next Amiga reset, and the toggle says so.
- **The setting** is `minimig_config.fmv_module`, appended to the config,
  which grows 9332 -> 9336 bytes. A 9332-byte file loads through its own
  branch with the module off. The `.mgl` key is `fmv_module`.
  `AMIGACD_FMV=1` in `MiSTer.ini` still turns it on (the test harness), and
  2-4 keep the diag modes.
- **Save states work with the module on, except during playback.** The
  module is left out of the state: the CL450/L64111 model and both
  decoders live on the ARM, and none of it is saved. That is harmless
  while the board is idle, so a save or a restore is refused only while it
  plays (`fmv_playing`): the CL450 out of IDLE, the L64111 started with
  frames buffered, or the cartridge's picture on. The rows grey out then,
  and the hotkeys say `No save states during FMV playback`. Cannon
  Fodder's driver shuts the board down at the end of its intro (Reset(),
  FlushBitstream(), the picture off; runs v21-v24), so saves work again
  from there. (The first version of this phase refused them whenever the
  module was on; the owner's call, 2026-11-05, was to ignore the module
  and state the limitation instead.)

  **Limitations**, by design:
  - A save never holds a video in progress. To save during a title with
    FMV, save before or after it.
  - The board's own memory is not in the state either: the CL450's 512 KB
    DRAM (board RAM in DDR3), the FPGA's register file, and what the model
    knows (microcode loaded, the last SetWindow/SetBorder, the L64111's
    registers). A restore leaves those as the running session has them. A
    game whose driver keeps state on the board between videos could find
    it different after a restore. Cannon Fodder resets the board before
    each use.
  - Restore with the module set as it was when the state was saved. The
    board is configured at reset and the state carries the Amiga's view of
    it, so a state saved with the module on and restored with it off (or
    the other way round) leaves the Amiga's expansion list wrong for the
    hardware. Neither case is detected.
- **The ARM CPU**: the CPU row skips the ARM step while the module is on,
  the module's row greys while the ARM CPU is on, and Main does not start
  the board if a config brings both.
- **Docs**: [`hardware-test-plan.md`](hardware-test-plan.md) section 5, and
  the README row. The release notes are generated from the commits.

Not tested on the host: the config loader (it needs MiSTer headers, as
every rung of it does); static_asserts pin the new size and that the old
layout is a prefix of it.

### Run v25, 2026-11-05: the row, the migration and the save states pass

Core seed 5 of fit run 42068080918 (closed), Main 83eb2d80, Cannon Fodder,
HDMI, PAL, no `AMIGACD_FMV` in `MiSTer.ini`.

- **The old config**: a 9332-byte file loaded through its branch, the row
  Off.
- **The row**: toggled On, the board came up (`FMV board alive`), and after
  one OSD reset the intro streamed. No ini override.
- **Save states**: during the intro the three rows were greyed, and
  Ctrl+Alt+F1 was refused with `No save states during FMV playback`,
  writing nothing. Once the intro had ended the rows were enabled, and the
  same keys wrote a 3.6 MB state in the credits. Restoring it went back
  from the title screen to the earlier credits screen. A restore refused
  during playback was not tried.
- **The module Off**: no FMV line in 45 s after a reset. Whether the boot
  finished, and Zorro II RAM came back, was not checked.
- **The log's fds**: the new log fd is close-on-exec. The 28 older ones
  came from binaries earlier in the exec chain and go at a real restart.
- **Audio**, with the split counters: 6245 played, **1 missing, 16 late**.
  So the silent frames are the decoder being late, not the stream: about
  4-5 early, then climbing through the second half. The audio thread now
  runs at nice -10, ahead of the video decoder on core 0. That is not
  yet measured.
- **Polls blocked**: 3599, all FIFO full, none CD audio, 0 samples dropped.
  That is back-pressure (Main offering a sector before the FIFO has room),
  not a fault. Its size depends on how often Main polls, which is why it
  is 5x v24's lumped count. The audio line now prints the FIFO's own
  underrun and starve counts since the session began, the gate's measure,
  in its place.
- The picture as v24: 4142 pictures, 4140 shown, 2 late, 0 dropped.
- **CI**: the fmvbench qemu step timed out (15 min) for the third time, with
  the binary already uploaded. Its log, readable now that it times out,
  shows the cause: apt fetching 84 MB of ffmpeg's packages from the Azure
  Ubuntu mirror at tens of KB/s, one package stalling and re-fetched
  twice. Now with `--no-install-recommends` and apt retries with a
  per-request timeout, in both jobs that install ffmpeg.

Not tested in v25: the sound heard, tearing, A/V sync, the `.mgl` rows, the
row off a CD32 or with no ROM, the ARM CPU exclusion, a mismatched
restore, CD audio after FMV, a Video CD, analog, NTSC, Real CD.

### Run v26, 2026-11-06: the intro plays at two thirds of its speed

Core seed 5 of fit run 42068080918, Main ba9ed2b7, Cannon Fodder, HDMI, PAL,
two full runs and an A/B with the CD speed.

**A correction to v21-v25.** Each said the intro "streamed to its end", and
v23 that "decoding keeps up". Nobody checked the pictures against the wall
clock. The 166 s in v21's row is what 4142 pictures take at 25 fps, not
what they took. Measured against `date` on the MiSTer: 575 pictures in
31 s, 596 in 34 s, and in v23 3095 in 172 s, about 16.7 a second. The
248 one-second `FMV stream` lines for 4142 pictures agree, and so does the
sound: 25 audio frames a second against the stream's 38.3. Pictures 0.69 of
real time, audio 0.67. The data goes in at about 100 KB/s to the CL450 and
16.5 KB/s to the L64111, against the 148 and 24 the stream needs. The video
buffer sits near empty, so the model is starved, not holding data back.
The late counts could not show any of this: the model schedules only what
has arrived.

- **The sound** then has about a third of its time as silence. The FIFO's
  starve count, about 12 a second, says the same. Its 16-bit underrun
  count wraps, so it could not prove it. Phase 4's gate fails until the
  rate is right.
- **Not the drive.** The game asks for 1x for the stream and reads about 44
  sectors in 0.6 s, then PAUSE, then PLAY again about 0.26 s later: 52
  sectors a second, 0.69 of a 1x drive's 75. With the OSD's CD speed at
  Fast the batches fell to 17 sectors in 0.07 s, PLAY every 0.33 s, and the
  rate stayed exactly 52 sectors, 45 packets and 17-18 pictures a second.
  So the driver takes data at this rate whatever the drive does.
- **What is left:** the 68k's path into the board, or the driver pacing
  itself on something the model gives it (an interrupt rate, the
  InquireBufferFullness replies, the L64111's PTS interrupt). The first
  would show as Main draining the event ring too slowly: the FPGA holds
  the 68k while the ring is full, so the 68k then copies at Main's pace. A
  pass of Main's loop that takes long is enough, and the Akiko sector reads
  run in the same loop.
- **Measured from this run on**: a `FMV rate:` line every 10 s, with pictures
  and audio frames a second against the sequence's and the stream's
  rates, Main's polls, the longest gap between two, the time in them,
  events a second, drains that found the ring near full, and the most
  found waiting. The FIFO's underruns and starves are now 32-bit session
  totals, summed from reads 100 ms apart. If drains near full come with
  long gaps, Main is the limit; if the ring never fills, the driver is
  pacing itself, and the next step is what it waits on between packets.
- The audio thread's priority: 13 and 8 late in the two runs against v25's
  16, with polls blocked at 537 and 502 against 3599. Two runs, and the
  rate problem under them, so it is not shown to be the cause.
- The harness's reset failed once more (reset sent, no `FMV Amiga reset`).


### Runs v27 and v28, 2026-11-06: the 68k waits on the cartridge ROM

v27 (the rate line) confirmed v26's rate, 0.69 of real time, and put Main
out of it: its polls are short and close together, drains seldom find the
ring near full. v28 (a diag build with the 0xF403 perf window) split the
68k's held time a second: the event FIFO 0, the ring under 1 %, and the
ROM and RAM trips to DDR3 about 35 %. The access count saturated at its
17-bit 131071, so the per-access figure I drew from v27 (14 us) was wrong:
ROM reads make no events and were never in it.

So the driver, which runs from the cartridge ROM, spends a third of its
time on instruction fetches across the f2sdram bridge. The fix, in the core:
a 4 KB direct-mapped line cache for the ROM in `fmv_bus` (32-byte lines,
filled by a 4-beat burst in `fmv_ddr`, emptied when the module is off). RAM
is not cached: the 68k and Main both write it. The perf word (diag
0xF404) widens with it: a 1/4 s window, ROM and RAM time counted apart.

**To see in the next run:** the rate line at 25.0 pictures/s and 38.3
audio frames/s, FIFO underruns near 0 during play, and on a diag build the
ROM share well under v28's.


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
