# L64111 MPEG audio decoder: notes for the FMV model

The source for the L64111 half of `support/minimig/fmv.cpp` (see
[`../fmv-plan.md`](../fmv-plan.md), phase 0 item 1: the model is written from
the manuals, not from WinUAE). These are our notes in our own words, with page
references. **The manual itself is not committed:** it is © 1993 LSI Logic
Corporation, "All rights reserved".

## The document

| | |
|---|---|
| Title | *L64111 MPEG Audio Decoder Technical Manual* |
| Edition | First Edition, MD72-000101-99 A, © 1993 LSI Logic. Marked *preliminary* ("derived from functional simulations"). Applies to revision B of the chip and later. |
| Copy used | alldatasheet.com scan, 77 pages, image only (no text layer) |
| SHA-256 | `0e94030cea74511b7f0a17d9560dc5dfd4daa1ccd5cd43e64b4f119a10549b36` |
| SHA-1 | `bc1e832eb428fd045f715124dd2209fbdcc21878` |

Page numbers below are the manual's own (`4-9` means chapter 4, page 9). In
the scan, chapter 4 is PDF pages 33-51.

## What the chip does (ch. 2)

- **Data path** (fig. 2.1, p. 2-4): controller writes, then a 128-byte **Input
  Data FIFO**, then the pre-parser, then the **Channel Buffer** in an external
  256K x 4 DRAM, then the decoder, then the PCM serial converter, then the
  output mux.
- **Pre-parser** (p. 2-5): in system-stream mode it finds the pack and
  system-header start codes, identifies audio packets, discards everything
  else, checks CRC, and stores the audio frames in the Channel Buffer.
- **Input rates** (p. 2-8, at a 30 MHz SYSCLK): the FIFO takes bursts of up to
  7.5 MB/s, and the pre-parser drains it at 15 Mbit/s. So the FIFO is only
  ever briefly full, and the Channel Buffer is what does the real buffering.
- **Capacity** (pp. 4-18, 6-11, 6-12): the Channel Buffer holds 128 Layer I
  frames or **64 Layer II frames**, up to about one second of audio. Video CD
  audio is Layer II.
- Layer I and II only. A Layer III header is a syntax error (p. 4-14).
  Sample rates are 32, 44.1 and 48 kHz, at up to 384 kbit/s (p. 2-6).

## Controller interface (ch. 3.3, p. 3-4)

- `A[4:0]` selects one of 32 registers. Every control register is 8 bits, on
  `D[7:0]`. Only the Data register can be 16 bits wide (p. 6-1).
- `INTR` is open drain and active low (p. 3-5). It stays asserted until the
  controller reads a status register (p. 4-9, below).
- `DTACK` is asserted even when a write overflows the input FIFO (p. 3-5). So
  overflow does not stall the bus: **the controller is expected to throttle on
  the FIFO flags**.

## FIFO flags (ch. 3.4, p. 3-5; fig. 3.2, p. 3-6)

These are pins, not register bits. The cartridge's IO word is where the FMV
ROM sees them; which bit carries which flag is board wiring, to be
established in phase 2.

| Pin | Asserted (low) when | Meaning for the controller |
|---|---|---|
| `FALF` | 32 or fewer bytes free of 128 | **Stop writing.** An overflow is an error. |
| `FALE` | 32 or fewer bytes left | Getting low. Running empty is *not* an error. |
| `FEMPTY` | 0 bytes left | Empty. Not an error either. |

The input FIFO flags are unrelated to the Channel Buffer (p. 3-5).

## Register map (table 4.1, p. 4-3)

| A[4:0] | Name | R/W | Notes |
|---|---|---|---|
| 0 | Data | W | MPEG input. 8-bit (`D[7:0]`) by default and after reset; 16-bit when CR2 `SEL_16/8_BIT` is set (p. 4-4) |
| 1 | Control 1 | R/W | p. 4-4 |
| 2 | Control 2 | R/W | p. 4-6 |
| 3 | Control 3 | R/W | p. 4-8 |
| 4 | Int mask 1 (W) / Status 1 (R) | R/W | p. 4-9 |
| 5 | Int mask 2 (W) / Status 2 (R) | R/W | p. 4-10 |
| 6 | TCR | R/W | SCLKO terminal count, bits [3:0] (p. 4-12) |
| 7 | TORH | R/W | Fraction, high byte; the binary point sits above bit 7 (p. 4-13) |
| 8 | TORL | R/W | Fraction, low byte |
| 9-11 | Parametric data 1-3 | R | The 20 audio-header bits after the 12-bit sync (pp. 4-13 to 4-17) |
| 12-16 | PTS 1-5 | R | PTS[7:0] ... PTS[31:24], then PTS5 bit 0 = PTS[32] (p. 4-17) |
| 17 | Ancillary data FIFO | R | 16 x 8, read the top byte, MSB first (p. 4-17) |
| 18 | Channel Buffer status counter | R | Bits [6:0]: Layer II 0-63, Layer I 0-127 (p. 4-18) |
| 19 | Channel Buffer write counter | R/W | **See the conflict below** |
| 20 | Channel Buffer read counter | R/W | **See the conflict below** |
| 21-31 | reserved | | |

"Read-only must not be written": some read-only addresses have test
functions behind them (p. 4-2). The model ignores such writes and logs them.

### Conflict in the manual: where the Channel Buffer counters live

Table 4.1 gives the write counter as 19 and the read counter as 20, and
reserves 21-31. The register pages say write counter `A[4:0] = 11010₂` (26,
p. 4-19) and read counter `11001₂` (25, p. 4-19). Every other register's page
agrees with the table. Two readings:

- The table is right, and the two page diagrams are typos. Two adjacent typos
  would be odd, but both diagrams were probably drawn from one template.
- The pages are right, and the table is wrong.

Cue and review is the only use of these registers (pp. 4-18, 6-12), and a
Video CD player that only plays forward never needs them. The model decodes
19/20 as the table says and logs any access to 19, 20, 25 or 26. The phase 2
log of the FMV ROM and of Cannon Fodder then settles which addresses are
used, if any.

## Control registers

**Control 1** (p. 4-4, 4-5), with reset values:

| Bit | Name | Reset | Meaning |
|---|---|---|---|
| 7 | ANC DATA FIFO RST | 0 | 1 empties the ancillary FIFO; the controller must write 0 again to re-enable it |
| 6 | SER/PAR | 0 | 1 = serial input; ORed with the SSEL pin. 0 = parallel, through register 0 |
| 5 | BYPASS | 0 | 1 = PCM passthrough, ORed with the pin |
| 4:3 | reserved | 0 | |
| 2 | BUF_RST | 0 | 1 empties the Channel Buffer; it does **not** clear itself |
| 1 | SOFT_RST | 0 | 1 resets the chip next clock, then **clears itself** |
| 0 | STARTI | STARTI pin | 1 = start decoding; it waits for data if the buffer is empty. 0 = reset the decoder |

**Control 2** (p. 4-6, 4-7):

| Bit | Name | Reset | Meaning |
|---|---|---|---|
| 7:6 | MUTE[1:0] | 00 | On error: 00 ignore, 01 mute, 10 repeat the last good frame, 11 reserved |
| 5 | SOFT_MUTE | 0 | 1 = PCM output forced to zero. Reads back as SOFT_MUTE OR the HDMUTE pin |
| 4 | SEL_16/8_BIT | 0 | Data register width, 1 = 16-bit |
| 3 | AUDIO ONLY | pin A4 | 1 = elementary audio stream, 0 = ISO 11172-1 system stream |
| 2 | I2S | 0 | PCM framing |
| 1:0 | PCM MODE | -- | 00 32fs, 01 64fs, 10 48fs, 11 reserved |

**Control 3** (p. 4-8):

| Bit | Name | Reset | Meaning |
|---|---|---|---|
| 7 | AUDIO_STREAM_ID_IGNORE | **1** | 1 = decode every audio stream |
| 6:5 | reserved | 0 | |
| 4:0 | AUDIO_STREAM_IDENTIFIER | -- | With bit 7 clear, decode only audio stream `0xC0 | id`. Resyncs within three frames of finding a real sync |

## Interrupts (p. 4-9 to 4-12)

- Writing to address 4 or 5 sets **masks**. Reading them returns **status**.
  1 enables a source. All sources are disabled after reset.
- **Reading either status register deasserts `INTR`**, so the controller must
  read both (p. 4-9).

**Status 1 / Mask 1** (address 4), same bit positions in both:

| Bit | Name | When it interrupts |
|---|---|---|
| 7 | ANC DATA VALID | Once, when the ancillary FIFO goes from empty to non-empty |
| 6 | ANC DATA FIFO OVRFL | Once per overflow episode |
| 5 | ANC DATA FIFO HFF | Once per upward crossing of half full |
| 3 | ERR BUF OVFL | Channel Buffer overflow: data lost |
| 4, 2:0 | reserved, read 0 | |

**Status 2 / Mask 2** (address 5). **The bit positions differ between status
and mask**, the one trap in this chip:

| Status bit | Mask bit | Name | When it interrupts |
|---|---|---|---|
| 7 | -- | reads 0 | |
| 6 | 7 | SYNTAX ERR DET | System or audio stream syntax error |
| 5 | 6 | PTS AVAILABLE | 0 to 1: a PTS has been captured into regs 12-16 |
| 4 | 5 | SYNC AUD | 0 to 1: three successive audio syncs in place |
| 3 | 4 | SYNC SYS | 0 to 1: three successive system syncs in place |
| 2 | 3 | FRAME DETECT IN | New frame being processed; the parametric words are valid for at least 7 ms |
| 1 | 2 | CRC ERR | A frame failed CRC (only if `protection_bit` = 0) |
| 0 | 1 / 0 | NEW FRAME OUT / LAST FRAME | One status bit, two masks: mask bit 1 = a frame has gone to the PCM output, mask bit 0 = only one frame is left in the Channel Buffer |

So a status-2 bit `n` in 1..6 is enabled by mask-2 bit `n + 1`. Status bit 0
is enabled by mask bit 1 or mask bit 0, depending on which of the two events
set it. The model has to remember which event that was.

## Parametric data words (p. 4-13 to 4-17)

These are the 20 header bits after the 12-bit `0xFFF` sync, laid out exactly as
ISO 11172-3 orders them:

- **P1:** `ID`, `layer[1:0]`, `protection_bit`, `bit_rate_index[3:0]`
- **P2:** `sampling_frequency[1:0]`, `padding_bit`, `extension_bit`,
  `mode[1:0]`, `mode_extension[1:0]`
- **P3:** `copyright`, `original/home`, `emphasis[1:0]`, then four reserved
  bits

They hold one frame's header. The next frame overwrites them, at least 7 ms
after FRAME DETECT IN rises. The model fills them from the header of the frame
it is about to report.

## PCM clock (p. 4-12, 4-13; ch. 6.3)

`TCR` (an integer) and `TORH:TORL` (a 16-bit fraction) divide SYSCLK down to
the PCM bit clock. The FMV ROM programs these for its DAC and SYSCLK. The model
records them but does not use them, because Main plays PCM at the stream's
own rate. Phase 4 can still read them if the ROM uses the fraction to nudge
the rate for A/V sync, which p. 6-11 mentions as one way to do it.

## A/V sync, as the manual intends it (ch. 6.4, p. 6-10 to 6-12)

- The controller reads the PTS from both decoders (the L64111 registers 12-16,
  and the CL450), and starts both when they coincide by asserting STARTI on
  each.
- After that it checks the PTS now and then for drift. The manual says to
  **correct video to audio, never audio to video**, because pitch changes
  are audible. That matches phase 4's rule.
- Throttling: stop writing on `FALF`, and watch the Channel Buffer status
  counter, so that the buffer neither underruns (which mutes, and needs a
  decoder reset and refill) nor overruns (which clicks) (p. 6-11).
- Cue and review means rewriting the Channel Buffer read and write pointers.
  Frames still play forward internally (p. 6-12).

## What this means for the model

- **The input FIFO:** 128 bytes, flags at 32 and 96. The DDR3 write ring sits
  in front of it, so the shadow flags must reflect **ring plus FIFO** fill, not
  just a 128-byte counter. Otherwise the ROM keeps writing while Main is
  behind. The pre-parser is fast (15 Mbit/s), so in practice the flags track
  how far Main's draining lags.
- **The Channel Buffer counter** (register 18) is the ROM's real flow-control
  signal: 64 Layer II frames. The model counts parsed, undecoded Layer II
  frames and caps the counter at 63. Overflow sets ERR BUF OVFL.
- **Reset:** CR3 resets to `0x80` (ignore the stream ID). CR1 STARTI and CR2
  AUDIO ONLY reset to pin levels, which only the board knows. The model
  starts both at 0 (system stream, stopped) and records the assumption here;
  a phase 2 trace of the ROM's first writes will show whether it ever relies
  on the reset values.
- **Read-to-clear:** a read of address 4 or 5 deasserts `INTR`. The status
  bits stay set while their condition holds (pp. 4-9, 4-10). Edge-triggered
  sources (PTS AVAILABLE, SYNC AUD, SYNC SYS, NEW FRAME OUT, LAST FRAME)
  interrupt once per rising edge.
