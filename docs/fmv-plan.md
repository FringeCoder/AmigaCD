# The CD32 FMV module: plan

**Date:** 2026-11-01
**Status:** plan only. No code in either repository yet.

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
| MPEG-1 video decode | userspace | libmpeg2, as WinUAE uses |
| MPEG audio (layer II) decode | userspace | kjmp2, as WinUAE uses |
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

## Reference: what WinUAE models

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
  The schematics show genlock-mode bits that the ROM never sets.

This repository has one stale note: `docs/reference/cd32-probe-copperline.tsv`
row 24 calls `$A80000` the "unmapped FMV window". It is not the FMV window. In
the core that range is now the Kickstart mirror or the Action Replay
(`rtl/gary.v:160`, `rtl/cart.v:90`). Fix the label when the probe is next
touched.

## Phase 0: questions answered on paper

No code in this phase. Its gate: every item below answered here, with a
source.

1. **Licence.** Main is GPL-3. The licences that matter are libmpeg2's (GPL-2
   or later), kjmp2's, and WinUAE's for `cd32_fmv.cpp`. The plan is to port
   WinUAE's register model, not just to read it, so its licence has to allow
   GPL-3. If it does not, write the model from the CL450 and L64111 datasheets
   and use WinUAE only as a behavioural oracle.
2. **The ROM.** Identify the known `fmv.rom` dumps by CRC/SHA-1 (WinUAE's
   `rommgr.cpp` lists them). Like `CD32.rom`, it is user-supplied and never
   committed.
3. **Test material.** List the discs: at least one Video CD, and the CD32
   titles that use the cartridge. Each needs a CHD and, for Real CD, a
   physical copy. The gates below are judged on these.
4. **Mode 2 Form 2 sectors.** Video CD MPEG tracks are Mode 2 Form 2 (2324
   bytes of user data). Akiko ships raw 2352-byte frames
   (`support/minimig/akiko_cd32.cpp:180`), so form 2 probably arrives intact
   from CHD. Confirm it for CHD and for the physical-disc path. Some drives
   need a different READ CD sector type for form 2.
5. **Genlock.** Find out how the real cartridge decides between Amiga and MPEG
   pixels: Lisa's ZD pin (colour-0 / background detect), a colour key, or
   WinUAE's threshold. The expansion connector's pinout and the FMV schematics
   are the source. The core's Denise has no ZD output today (`rtl/denise.v:212`
   is a commented-out `zdclken`).
6. **CL450 output geometry.** Find the CL450's output pixel clock and active
   window relative to the Amiga's PAL/NTSC timing. This decides how a 352-wide
   picture lands on a lores or hires line.

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

## Phase 2: the board, with a logging model

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
- `support/minimig/fmv.cpp`: drains the ring and runs the CL450 / L64111 model.
  It updates the shadows and logs every host command. No decoding yet: the
  model reports pictures as decoded, on schedule, without making any.
- Unit tests in `tests/` for the command parser and buffer fullness, fed with
  logged sequences.

**Gate:**
1. With a Video CD inserted, the FMV ROM boots into its player and runs a
   track to completion: the commands are the same as WinUAE's log of the same
   disc, and the CPU doesn't hang.
2. With the bit off, the seed panel is unchanged. With it on, the panel is no
   worse than the noise line (~0.3 ns per family, `seed-lottery.md`), and no
   family that closed now fails.

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

**Sync:** audio PTS against the CL450 SCR, in Main, the way WinUAE's
`l64111_get_frame` / `cl450_scr` do. Video is the side that gets dropped or
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
3. **The CL450 interface.** It is reverse-engineered from one emulator. A
   title that uses a command WinUAE doesn't model will fail in our model too,
   and our log will show which one.
4. **Core 0 contention.** Linux timer and interrupt work already land there
   (`arm-cpu-plan.md`, step 1). Phase 1's busy run is what tests this.
