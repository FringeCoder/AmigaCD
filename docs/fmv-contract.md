# FMV module: the contract between the core and Main

**Status:** draft for phase 2 of [`fmv-plan.md`](fmv-plan.md), 2026-11-02.
Version 1.

Both halves of the FMV module are built against this file: the core's front
end (`rtl/fmv/` in `FringeCoder/AmigaCD_MiSTer`) and Main's model and transport
(`support/minimig/fmv*.cpp` here). Neither repository's CI can see the other.
So any change on one side that is not also a change here is a change made
blind, and it shows up as a CD32 that hangs at the FMV ROM rather than as a
failed build. Change the layout here first, bump the version, then change
both sides.

## Principles

1. **The 68k never waits for Main.** Register writes are posted into a ring
   in DDR3, and register reads are answered from shadows the FPGA holds.
   Main is single-threaded, and the A2065 found the deadlock that waiting
   causes (`a2065_ddr3_mailbox.v`, the CMD ring comment). The only bus
   stretches are FPGA-side work: a DDR3 access for ROM or RAM, or a ring
   that is completely full.
2. **The FPGA knows the board's address map and two chip facts, nothing
   else.** Which IO-word bit means what, the reset values, and every
   register's meaning live in Main. They come from the chip manuals and from
   phase 2's logs, and they will change. Main is a re-copy to fix; the core
   is a refit.
3. **Interrupt acknowledges are applied by the FPGA at once.** The host is
   told later, through the ring. This is the A2065's CSR0 rule, for the same
   reason: a handler that acknowledges and then re-reads must see the
   acknowledge, or it loops until Main catches up.

## The window

ARM physical **`0x27000000`**, 2 MB, which is Avalon (DDRAM_ADDR, 64-bit word)
**`29'h04E00000`**. It sits below the minimig's own DDR3 users at
`0x27800000` (drive sounds) and `0x27FF4000` (share). It is clear of Z3 fast
RAM (`0x30000000-0x3FFFFFFF`), the save state windows (`0x3C000000`), the
A2065 (`0x1FF00000`) and the scaler and framebuffer at `0x20000000-0x22xxxxxx`.
`0x27200000-0x277FFFFF` is reserved for phase 3's frame buffers.

| Offset | Size | Who writes | What |
|---|---|---|---|
| `0x000000` | 1 MB | Main (ROM), 68k (RAM) | **Board image**: byte offset = offset within the 1 MB board |
| `0x100000` | 8 | FPGA | `WPTR`: ring write count |
| `0x100100` | 512 | Main | **Shadow block**, 64 words (below) |
| `0x110000` | 32 KB | FPGA | **Ring**, 4096 entries x 8 bytes |

### The board image, `0x000000-0x0FFFFF`

The board's own map (from WinUAE `cd32_fmv.cpp`; see the plan's reference
section), with its offsets from the board base, which autoconfig sets to
`$200000`:

| Board offset | What | Served from |
|---|---|---|
| `0x00000-0x3FFFF` | ROM, 256 KB | DDR3 image, read-only to the 68k |
| `0x40000-0x4FFFF` | IO / status word | shadow (read), ring (write) |
| `0x50000-0x5FFFF` | L64111 registers | shadow (read), ring (write) |
| `0x60000-0x6FFFF` | CL450 data port | ring (write); reads return `0xFFFF` and are posted |
| `0x70000-0x7FFFF` | CL450 registers | shadow (read), ring (write) |
| `0x80000-0xFFFFF` | RAM, 512 KB | DDR3 image, read/write |

- **Byte order:** board byte offset `o` lives at window byte `o`, in the
  file's order, exactly as `fmv.rom` is laid out. For the FPGA, Avalon word
  `o >> 3` holds byte `o` in bits `8*(o&7)+7 : 8*(o&7)`. A 68k word at even
  `o` is `{byte o, byte o+1}`. UDS selects byte `o` and LDS byte `o+1`.
- ROM and RAM accesses are real DDR3 reads and writes, with DTACK stretched
  for them (the A2065 boardram path). They do not go into the ring. A write
  into the ROM range is dropped, and is posted to the ring so it shows in the
  log.
- Main writes the ROM image **before** it sets `ALIVE` (below).

### `WPTR`, `0x100000` (FPGA to Main)

Bits 31:0 hold the number of ring entries ever written, wrapping at 2^32. The
FPGA writes the entry first and `WPTR` after it. Main keeps its own read
count.

### The ring, `0x110000` (FPGA to Main)

Entry `n` lives at `0x110000 + 8 * (n & 4095)`. Every 68k access to board
offsets `0x40000-0x7FFFF` (registers, in both directions) and every write into
the ROM range makes one entry:

| Bits | Field |
|---|---|
| 0 | `RW`: 1 = read, 0 = write |
| 1 | `UDS` asserted |
| 2 | `LDS` asserted |
| 3 | reserved, 0 |
| 23:4 | board byte offset 19:0 (always even) |
| 31:24 | reserved, 0 |
| 47:32 | data: the write data, or the value the read returned |
| 63:48 | `SEQ`: `n & 0xFFFF` |

**`SEQ` is not redundant.** A DDR3 write from one HPS port can become visible
to another port later than a write issued after it (`arm-cpu-plan.md`, steps
1b and 1e). So Main can see `WPTR` advance before the entry itself has landed.
Main accepts entry `n` only when its `SEQ` equals `n & 0xFFFF`, and otherwise
re-reads it on the next pass. It never skips an entry.

**Back-pressure:** the FPGA stretches DTACK on a register access while
`n - HOST_RDIDX >= 4095`, i.e. while the ring is full. Normal play never gets
there: a Video CD streams about 90 000 data-port words a second, so 4096
entries is 45 ms of Main not draining.

### The shadow block, `0x100100` (Main to FPGA)

64 words, Avalon `29'h04E20020 + i`. Main writes words 1-63, then word 0. The
FPGA reads word 0 and, if it changed, reads the whole block again as one
64-beat burst. Word 0 is read before the burst, so the block it fetches is at
least as new as the generation it saw.

| Word | Field |
|---|---|
| 0 | `GEN`: bits 31:0, incremented by Main after every update of 1-63 |
| 1 | `HOST_STATE`: bit 0 `ALIVE`, bit 1 `CL450_INT`, bit 2 `L64111_INT`; bits 63:32 `HOST_RDIDX` (ring entries consumed) |
| 2 | `IO_IRQ_MAP`: bits 15:0 the IO-word bits that mirror the CL450 interrupt, 31:16 the bits that mirror the L64111 interrupt, 47:32 those bits' value **while asserted** |
| 3 | reserved |
| 4-7 | `AUTOCFG`: 64 nibbles; nibble `i` (bits `4i+3:4i` of the 256-bit field, word 4 lowest) is what the 68k reads as the high nibble at autoconfig offset `2i` |
| 8 | `IO`: bits 15:0, the IO word as read at board `0x40000` |
| 9-11 | reserved |
| 12-19 | `L64111`: 32 x 16 bits; register `r` (board `0x50000 + 2r`, mirrored every 64 bytes) is bits `16*(r&3)+15 : 16*(r&3)` of word `12 + r/4` |
| 20-51 | `CL450`: 128 x 16 bits; the register at board `0x70000 + o` (`o` even, `0x00-0xFE`, mirrored every 256 bytes) is bits `16*(k&3)+15 : 16*(k&3)` of word `20 + k/4`, `k = o/2` |
| 52-63 | reserved |

- **`ALIVE`** says Main has loaded the ROM and filled `AUTOCFG`. The FPGA
  samples it at Amiga reset: **without it the board does not join the
  autoconfig chain** and its address range is unmapped. So a missing
  `fmv.rom` means no card, not a card with garbage in its autoconfig.
- **`AUTOCFG`** is taken by Main from the ROM's first 128 bytes: nibble `i` =
  `rom[2i] >> 4`. WinUAE does the same (`expamem_init_cd32fmv`), and phase 2
  confirms it on a real dump: the nibbles must decode to a 1 MB Zorro II
  board.
- **The interrupt** goes to Paula's INT2:
  `ALIVE & ((CL450_INT & ~cl_ack) | (L64111_INT & ~l64_ack))`.
- `IO_IRQ_MAP` exists so the IO word can follow a local acknowledge
  without the FPGA knowing which bit is which: while a source is
  acknowledged locally, its mapped IO bits read as the *de*asserted value.

### Local acknowledges (FPGA)

Two chip facts, from the manuals:

| Source | Acknowledged by | Manual |
|---|---|---|
| L64111 | a **read** of register 4 or 5 (board `0x50008`, `0x5000A`, and their mirrors) | L64111 manual p. 4-9: reading either status register deasserts `INTR` |
| CL450 | a **write** to `HOST_control` (board `0x70090` and mirrors) with bit 7 set | CL450 manual 4.8.1 and p. 8-12: the host clears `Int` (bit 7) in `HOST_control` |

On such an access the FPGA sets that source's `ack` and records the ring index
of the access. It clears `ack` once `HOST_RDIDX` has passed that index. By
then Main has seen the access and lowered the source itself, or raised it
again for a new interrupt, which is then real.

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
| 1 | 2026-11-02 | First draft, phase 2 |
