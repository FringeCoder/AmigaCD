# FMV module: the contract between the core and Main

**Status:** phases 2 and 3 of [`fmv-plan.md`](fmv-plan.md), 2026-11-05.
Version 5.

Both halves of the FMV module are built against this file: the core's front
end (`rtl/fmv/` in `FringeCoder/AmigaCD_MiSTer`) and Main's model and transport
(`support/minimig/fmv*.cpp` here). Neither repository's CI can see the other.
So any change on one side that is not also a change here is a change made
blind, and it shows up as a CD32 that hangs at the FMV ROM rather than as a
failed build. Change the layout here first, bump the version, then change
both sides.

## Principles

1. **The 68k never waits for Main.** Register writes are posted, and register
   reads are answered by the FPGA. Main is single-threaded, and the A2065
   found the deadlock that waiting causes (`a2065_ddr3_mailbox.v`, the CMD
   ring comment). The only bus stretches are FPGA-side work: a DDR3 access
   for ROM or RAM, a register-file read, or a ring that is completely full.
2. **The chips' registers live in the FPGA, as a register file both sides
   write.** That is what they are in the silicon. The CL450's HMEM, for one,
   is written by the host and by the chip's own CPU (CL450 manual 8.3.3).
   The host also reads back what it has just selected through `HOST_raddr`.
   A snapshot published by Main would be stale for exactly those reads, or
   would undo a newer 68k write. So:
   - the 68k writes the register file directly, in the bits it owns, and
     every register access also goes to Main through the **event ring**;
   - Main writes it through the **update queue**: masked writes, applied in
     order, last writer wins, as in the chip.
3. **The FPGA knows the board's address map and three chip facts, nothing
   else.** Which bits the 68k owns in each register is a table Main loads. So
   are the reset values, which IO-word bit is which, and every register's
   meaning. Those come from the chip manuals and from phase 2's logs, and
   they will change. Main is a re-copy to fix; the core is a refit. The three
   facts, each cited:
   - a 68k write to CL450 `HOST_rdata` goes to `HMEM[HAddr]` and then
     increments `HAddr` (`HOST_raddr` bits 3:0); a read returns
     `HMEM[HAddr]` (CL450 manual 8.3.3, p. 8-13);
   - the CL450 interrupt pin is asserted while `HOST_control` bit 7 is 0
     (p. 8-12). So the host's acknowledge, writing bit 7 to 1, takes effect
     at once with no special case;
   - a 68k read of L64111 register 4 or 5 deasserts the L64111 interrupt
     (L64111 manual p. 4-9).

## The window

ARM physical **`0x27000000`**, 2 MB, which is Avalon (DDRAM_ADDR, 64-bit word)
**`29'h04E00000`**. It sits below the minimig's own DDR3 users at
`0x27800000` (drive sounds) and `0x27FF4000` (share). It is clear of Z3 fast
RAM (`0x30000000-0x3FFFFFFF`), the save state windows (`0x3C000000`), the
A2065 (`0x1FF00000`) and the scaler and framebuffer at `0x20000000-0x22xxxxxx`.
`0x27200000-0x277FFFFF` holds phase 3's frame buffers (v5, "The picture" below).

| Offset | Size | Who writes | What |
|---|---|---|---|
| `0x000000` | 1 MB | Main (ROM, decoder variables), 68k (RAM) | **Board image**: byte offset = offset within the 1 MB board |
| `0x100000` | 8 | FPGA | `EV_WPTR`: event ring write count |
| `0x100008` | 8 | FPGA | `UQ_RDPTR`: update queue entries applied |
| `0x100040` | 8 | Main | `UQ_WPTR`: update queue write count |
| `0x101000` | 16 KB | Main | **Update queue**, 1024 entries x 16 bytes |
| `0x110000` | 32 KB | FPGA | **Event ring**, 4096 entries x 8 bytes |

### The board image, `0x000000-0x0FFFFF`

The board's own map (from WinUAE `cd32_fmv.cpp`; see the plan's reference
section), with its offsets from the board base, which autoconfig sets to
`$200000`:

| Board offset | What | Served from |
|---|---|---|
| `0x00000-0x3FFFF` | ROM, 256 KB | DDR3 image, read-only to the 68k |
| `0x40000-0x4FFFF` | IO / status word | register file |
| `0x50000-0x5FFFF` | L64111 registers | register file |
| `0x60000-0x6FFFF` | CL450 data port (CMEM, write only) | event ring; reads return `0xFFFF` |
| `0x70000-0x7FFFF` | CL450 registers | register file |
| `0x80000-0xFFFFF` | RAM, 512 KB: the CL450's local DRAM (bank 0, CL450 manual table 4-1) | DDR3 image, read/write |

- **Byte order:** board byte offset `o` lives at window byte `o`, in the
  file's order, exactly as `fmv.rom` is laid out. For the FPGA, Avalon word
  `o >> 3` holds byte `o` in bits `8*(o&7)+7 : 8*(o&7)`. A 68k word at even
  `o` is `{byte o, byte o+1}`. UDS selects byte `o` and LDS byte `o+1`.
- ROM and RAM accesses are real DDR3 reads and writes, with DTACK stretched
  for them (the A2065 boardram path). They do not go into the event ring. A
  write into the ROM range is dropped, and is posted to the ring so it shows
  in the log.
- The RAM is the decoder's own memory, and its microcode keeps variables
  there that the host reads (CL450 manual ch. 14, appendix A). Main writes
  those into the image directly.
- Main writes the ROM image **before** it sets `ALIVE` (below).

### The register file (FPGA)

256 registers of 16 bits, each with a 16-bit **68k write mask** beside it.
Indices:

| Index | Register | 68k address (board offset) |
|---|---|---|
| `0x00-0x7F` | CL450 register at byte offset `2i` | `0x70000 + 2i`, mirrored every 256 bytes |
| `0x80-0x9F` | L64111 register `i - 0x80` | `0x50000 + 2(i - 0x80)`, mirrored every 64 bytes |
| `0xA0` | IO word | `0x40000`, mirrored through `0x4FFFF` |
| `0xB0-0xBF` | CL450 HMEM 0-15 | through `HOST_rdata` only |
| `0xC0` | `CTRL`: bit 0 `ALIVE` | none |
| `0xC1` | `L64_INT`: bit 0, the L64111 interrupt latch (guarded; see the queue) | none |
| `0xC2`, `0xC3` | `HOST_RDIDX` bits 15:0, 31:16: event-ring entries Main has consumed | none |
| `0xC4` | `IO_MAP_CL`: IO-word bits that show the CL450 interrupt | none |
| `0xC5` | `IO_MAP_L64`: IO-word bits that show the L64111 interrupt | none |
| `0xC6` | `IO_POL`: those bits' value while asserted | none |
| `0xD0-0xDF` | `AUTOCFG`: 64 nibbles (below) | none |
| `0xE0-0xE6` | the display registers (v5, "The picture" below) | none |
| `0xE7-0xFF` | reserved | none |

`AUTOCFG` uses `0xD0-0xDF`: 16 registers of 4 nibbles. Nibble `n` (what the
68k reads as the high nibble at autoconfig offset `2n`, `n` = 0..63) is bits
`4(n&3)+3 : 4(n&3)` of register `0xD0 + n/4`.

- **68k write** to index `i` (word access, or a byte access widened to its
  lanes): `R[i] = (R[i] & ~M[i]) | (data & M[i])`. `M` is the write mask.
  Except for `HOST_rdata` (`i = 0x46`), which writes `R[0xB0 + HAddr]` the
  same way, using `M[0xB0 + HAddr]`, and then increments `HAddr`, the low
  four bits of `R[0x44]`. Every write also goes into the event ring.
- **68k read** of index `i` returns `R[i]`, except: `HOST_rdata` returns
  `R[0xB0 + HAddr]`; and in the IO word, the bits in `IO_MAP_CL` and
  `IO_MAP_L64` read as `IO_POL` while their interrupt is asserted and as
  `~IO_POL` while it is not. Every read goes into the event ring with the
  value it returned. A read of L64111 register 4 or 5 also clears `L64_INT`
  and records its ring index as `l64_ack_idx`.
- **The interrupt** to Paula's INT2 is
  `ALIVE & (~R[0x48][7] | L64_INT[0])`. `R[0x48]` is CL450 `HOST_control`
  (byte offset `0x90`).
- At FPGA reset every `R` and `M` is 0, so the 68k can write nothing until
  Main has loaded the masks.
- **`ALIVE`** says Main has loaded the ROM, the masks and `AUTOCFG`. The
  FPGA samples it at Amiga reset: **without it the board does not join the
  autoconfig chain** and its address range is unmapped. So a missing
  `fmv.rom` means no card, not a card with garbage in its autoconfig.
- **`AUTOCFG`** is taken by Main from the ROM's first 128 bytes: nibble `n` =
  `rom[2n] >> 4`. WinUAE does the same (`expamem_init_cd32fmv`), and phase 2
  confirms it on a real dump: the nibbles must decode to a 1 MB Zorro II
  board.

### `EV_WPTR` and the event ring (FPGA to Main)

`EV_WPTR` bits 31:0 hold the number of events ever written, wrapping at 2^32.
The FPGA writes the entry first and `EV_WPTR` after it. Main keeps its own
read count, and reports it back through `HOST_RDIDX`.

Entry `n` lives at `0x110000 + 8 * (n & 4095)`. Every 68k access to board
offsets `0x40000-0x7FFFF` (registers and the data port, in both directions)
and every write into the ROM range makes one entry. So does every Amiga reset
while the board is on (bit 3): the CL450 and L64111 share the system reset,
so Main resets its model and pushes the masks and reset values again.

| Bits | Field |
|---|---|
| 0 | `RW`: 1 = read, 0 = write |
| 1 | `UDS` asserted |
| 2 | `LDS` asserted |
| 3 | `RESET`: the Amiga was reset; every other field is 0 |
| 23:4 | board byte offset 19:0 (always even) |
| 31:24 | `HADDR`: for a `HOST_rdata` access, the HMEM index used (bits 27:24); else 0 |
| 47:32 | data: the write data, or the value the read returned |
| 63:48 | `SEQ`: `n & 0xFFFF` |

**`SEQ` is not redundant.** A DDR3 write from one HPS port can become visible
to another port later than a write issued after it (`arm-cpu-plan.md`, steps
1b and 1e). So Main can see `EV_WPTR` advance before the entry itself has
landed. Main accepts entry `n` only when its `SEQ` equals `n & 0xFFFF`, and
otherwise re-reads it on the next pass. It never skips an entry.

**Back-pressure:** the FPGA stretches DTACK on a ring-bound access while
`(n - HOST_RDIDX) mod 2^16 >= 4095`, i.e. while the ring is full. Only
`HOST_RDIDX`'s low half (`0xC2`) takes part. The ring is far smaller than
2^16, so the modular difference is exact, and a read torn between the two
halves cannot matter. Normal play never gets
there: a Video CD streams about 90 000 data-port words a second, so 4096
entries is 45 ms of Main not draining.

### `UQ_WPTR` and the update queue (Main to FPGA)

Entry `k` lives at `0x101000 + 16 * (k & 1023)`, two 64-bit words. Main
writes word 1, then word 0, which carries `SEQ`, with a barrier between, and
then `UQ_WPTR` (= entries ever written). The FPGA polls `UQ_WPTR`, then reads
each new entry as a 2-beat burst. Writes from one HPS port reach DDR3 in the
order they were issued, so a matching `SEQ` in word 0 means word 1 has landed
too.

After applying entry `k`, the FPGA writes `UQ_RDPTR = k + 1` (at least every
64 entries, and whenever the queue goes empty). Main never lets
`UQ_WPTR - UQ_RDPTR` exceed 1024. Updates that do not fit wait in Main.

| Word | Bits | Field |
|---|---|---|
| 0 | 7:0 | register index |
| 0 | 8 | `MASKFILE`: 1 = write `M[index]` instead of `R[index]` |
| 0 | 9 | `GUARD`: apply only if `RDIDX` (below) is past `l64_ack_idx` |
| 0 | 47:16 | reserved, 0 |
| 0 | 63:48 | `SEQ`: `k & 0xFFFF` |
| 1 | 15:0 | value |
| 1 | 31:16 | bit mask: `R[i] = (R[i] & ~mask) \| (value & mask)` |
| 1 | 63:32 | `RDIDX`: Main's event-ring read count when it made this update |

- The FPGA applies entry `k` only once its `SEQ` reads back as `k & 0xFFFF`,
  for the same cross-port reason. It waits and re-reads; it never skips.
- **`GUARD`** is for raising the L64111 latch (`0xC1`). Main may decide to
  raise it before it has seen the 68k's acknowledging read. A guarded update
  made with `RDIDX - l64_ack_idx <= 0` (as a signed 32-bit difference) is
  dropped, because the 68k has acknowledged since then. Main raises it again if the source is still
  active once it has caught up.
- `HOST_RDIDX` (`0xC2`/`0xC3`) is written through the queue like anything
  else. Main writes it at least every time it has drained 1024 events, so
  back-pressure never sees a stale count for long.

### A session starts from zero

DDR3 is not cleared between core loads, so the window can hold garbage or the
pointers of a session that ended. Neither side may read the other's counters
from it. Instead:

- **While `status[64]` is clear**, the FPGA does not touch the window (no
  event writes, no queue polls), drops anything still in its crossings, and
  holds its counters at 0: events written, queue entries applied, its
  back-pressure count, `HOST_RDIDX`, the L64111 acknowledge index, the
  L64111 latch and `ALIVE`. The rest of the register file keeps its values.
- **Main starts a session** with `status[64]` clear: it clears the bit (a
  saved config can bring it up with the core) and waits long enough for the
  core to see it, then writes `EV_WPTR = UQ_RDPTR = UQ_WPTR = 0`, writes the
  ROM, queues the masks, reset values, `AUTOCFG` and `ALIVE` from entry 0,
  and only then sets `status[64]`.
- A Main that restarts under a running core does the same: it starts a new
  session rather than resuming one. It would have lost the model's state
  anyway.

v3 resumed from the pointers instead. On hardware (2026-11-02, Cannon
Fodder), a fresh core started at entry 0 while Main queued from whatever
`UQ_RDPTR` held. `ALIVE` never landed, and the board never joined the chain.

## The picture (v5, phase 3)

The CL450 makes no syncs. It takes the Amiga's HSYNC and VSYNC, and outputs
one display pixel per VCLK: the decoded picture inside an output window, and
the border colour everywhere else (plan, phase 0 item 6; CL450 manual,
`SetBorder()` and `SetWindow()`, pp. 11-30 to 11-44). The cartridge switches
between that output and the Amiga's own per pixel, on the genlock key. The
FPGA does the switching and the placing; Main does the decoding and works
out the window from the macro commands.

### Frame buffers, `0x27200000-0x273FFFFF`

Four buffers of 512 KB. Buffer `b` (0-3) starts at ARM `0x27200000 +
b * 0x80000`, Avalon `29'h04E40000 + b * 29'h10000`. Pixel `(x, y)` of the
output window is the 16-bit word at byte `y * 1024 + 2 * x` of its buffer,
RGB565 little-endian (bits 15:11 red, 10:5 green, 4:0 blue), so Avalon word
`y * 128 + x / 4` holds pixels `4(x/4)` to `4(x/4)+3`, pixel `x` in bits
`16(x&3)+15 : 16(x&3)`. At most 384 pixels by 512 lines.

A buffer holds the output window only, already cropped (`SetWindow`'s
`xOffset`, `yOffset`), so the FPGA never needs the picture size.

### The display registers, `0xE0-0xE6`

Flops in the FPGA, written by Main through the update queue like any other
index. The 68k cannot reach them.

| Index | Name | Bits |
|---|---|---|
| `0xE0` | `DISP_CTRL` | 0 `ON`: the cartridge's output is selected where the key is asserted. 1 `BLANK`: the window shows the border colour (`SetBlank(1)`, and before the first picture). 3:2 `BUF`: the buffer shown. 4 `KEY_ALL`: ignore the key and select the cartridge everywhere (for calibration). |
| `0xE1` | `DISP_X` | window left edge, in display pixels after the falling edge of `_HSYNC`. A display pixel is two `clk_sys` cycles, a hires pixel. |
| `0xE2` | `DISP_Y` | window top line, in lines after the falling edge of `_VSYNC` |
| `0xE3` | `DISP_W` | window width in source pixels, a multiple of 4, at most 384. Each is shown for two display pixels (CL450 p. 11-44) |
| `0xE4` | `DISP_H` | window height in lines, at most 512 |
| `0xE5` | `BORDER_RG` | border red in 15:8, green in 7:0 |
| `0xE6` | `BORDER_B` | border blue in 7:0 |

- The FPGA copies all seven into the set it displays from on the falling
  edge of `_VSYNC`. New `SetBorder()`/`SetWindow()` values take effect on
  VSYNC in the chip too, and this way a change of `BUF` never tears.
- **Main writes a buffer completely before naming it in `BUF`, and does not
  write a buffer again until a whole frame after it last named another.**
  That is four buffers: shown, just replaced (maybe still shown until the
  next VSYNC), ready, and being decoded.
- **The key** is Lisa's ZD in its default mode: asserted on a pixel whose
  colour-table select is 0 (background, no sprite), and in the border unless
  `BPLCON3` `BRDNTRAN` is set (plan, phase 0 item 5, an inference not yet
  confirmed). ZD's other modes (`ZDBPEN`, `ZDCTEN`, `ZDCLKEN`) are not
  modelled.
- **Fetching:** at each `_HSYNC` the FPGA reads the next line's window row
  (`DISP_W / 4` words) into the other half of a two-line buffer. If DDR3 is
  late, the line shows what the buffer last held. The FPGA never waits on it.
- With `status[64]` clear, or `ON` clear, the Amiga's picture goes through
  unchanged and nothing is fetched.

## Option and exclusions

- One OSD status bit, **FMV module Off/On**, allocated in the core's
  `CONF_STR` in phase 2. With it off, the FPGA never joins the chain and
  never touches the window, and Main maps nothing.
- **Zorro II fast RAM is forced off while FMV is on.** The core decodes Z2
  RAM at the fixed range `$200000-$9FFFFF` whatever base autoconfig gives it
  (`memory_router.v:78`), so the two cannot coexist. A real CD32 with the
  cartridge had no Z2 RAM either. The core masks the Z2 board out of the
  chain, and Main greys the row out. Z3 RAM is unaffected.
- **The FMV board is first in the autoconfig chain**, so it is given
  `$200000`, the base the ROM expects (WinUAE warns otherwise). The FPGA
  decodes by the base it was given, not by a constant.
- **Save states are refused while FMV is on** (plan, phase 5).

## Version history

| Version | Date | Change |
|---|---|---|
| 5 | 2026-11-05 | The picture: four RGB565 frame buffers at `0x27200000`, display registers `0xE0-0xE6` latched at VSYNC, the ZD key. Nothing in v4 changes. |
| 4 | 2026-11-02 | A session starts from zero: off, the FPGA holds its counters at 0 and leaves the window alone; Main zeroes the pointers before setting `status[64]`. Replaces v3's resume-from-pointers, which failed on hardware. |
| 1 | 2026-11-02 | First draft, phase 2: posted ring plus a shadow block |
| 3 | 2026-11-02 | `UQ_RDPTR`, so Main cannot overrun the update queue; update-queue word order; back-pressure on `HOST_RDIDX`'s low half only; the guard compare made wrap-safe. Nothing was yet built on v2. |
| 2 | 2026-11-02 | The `RESET` event (bit 3). Shadow block replaced by a register file in the FPGA and an update queue from Main, before anything was built on v1. CL450 `HOST_raddr`/`HOST_rdata` reads depend on writes still in the ring, and HMEM has two writers (manual 8.3.3), which a snapshot cannot serve. |
