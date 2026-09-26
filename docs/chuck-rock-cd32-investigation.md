# Technical Investigation: Chuck Rock CD32 Boot Failure & Black Screen on AmigaCD MiSTer

**Target Audience:** Claude (Co-author on AmigaCD MiSTer core development)  
**Date:** September 26, 2026  
**Subject:** Root cause analysis of intermittent black screen boot hangs in *Chuck Rock* (CD32) on the `AmigaCD` core.

---

## 1. Executive Summary

*Chuck Rock* (CD32) exhibits intermittent boot failures: sometimes it successfully boots into the game, but frequently it hangs on a black screen immediately after the CD32 intro animation.

Live hardware diagnostics, disassembly of the crash state, and git branch analysis reveal that **two distinct, interacting root causes** produce the exact same catastrophic failure in the game's M68000 loader:

1. **Bug A (Data Stream Pause Race — Unmerged):** In [support/minimig/akiko_cd32.cpp](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/support/minimig/akiko_cd32.cpp), `akiko_handle_sec_req()` did not check `cd_paused`. When the CD32 BIOS pauses a read, a trailing sector push lands in PBX slot 0 of the *following* read, corrupting the file header. Commit `70bb7da` (`fix/akiko-pause-stops-data`) proved and solved this, **but it was never merged into `master`**.
2. **Bug B (Command Framer Desynchronization & Phantom Drain):** During idle/pause periods, `AKIKO_STATUS_REQ` (bit 11) asserts with an empty FPGA buffer (`00 00`). 
   - Prior to commit `c1041c0` (PR #77), host sent `CH_ERR_CHECKSUM`, desyncing the BIOS.
   - Even after PR #77, draining the phantom `00 00` pulses `cmd_done` (via `DisableIO()`), which shifts the FPGA command FIFO. When the BIOS sends its next command (e.g., `INFO` `0x27 0xd8`), a leftover checksum byte from a prior command (specifically `0xea` from LED command `15 00 ea`) sits in front of it: `ea 27 d8`. The INFO command fails checksum, is discarded, and the BIOS halts communication.
3. **The Common Failure Sink:** In both cases, the Chuck Rock file loader at `$00100000` either parses a corrupt block offset (`$01E30000`) or attempts to copy from an empty buffer. Its copy loop runs past the end of 2 MB Chip RAM (`$00200000`), overwriting its own code and exception vectors with zeros, deadlocking the 68020 at IPL 7 with all interrupts masked.

---

## 2. The 68020 Crash Breakdown

When Chuck Rock boots:
1. The CD32 ROM executes the game's bootloader.
2. The loader issues `PLAY DATA` / `PAUSE` commands to stream file data into a staging buffer at `$00100000`.
3. It parses the file header at `$00100000` to locate game assets and code.
4. It executes a block copy loop:
   ```m68k
   lea     $00100000, a0        ; Source buffer in Chip RAM
   adda.l  d1, a0               ; Block offset parsed from header
   lsr.l   #2, d2               ; Word/Long count
   loop:
   move.l  (a0)+, (a1)+         ; Copy destination
   subq.l  #1, d2
   bne.s   loop
   ```
5. **The Failure Mode:**
   - If the header is corrupted by an off-by-one sector delivery (Bug A), `d1` contains a garbage block index: `$01E30000` (block 61,824).
   - If the read never completes because BIOS communication stalled (Bug B), `$00100000` contains uninitialized/zero data, also yielding garbage in `d1`.
   - Adding `$01E30000` to `$00100000` puts `a0` at `$01F30000`.
   - The copy loop runs off the top of Chip RAM (`$00200000` on a 2 MB machine) and wraps or overwrites the loader's own code and vectors with zeros.
   - The CPU ends in an infinite branch through the zeros it just wrote at IPL 7 (interrupts masked), resulting in a permanent black screen.

---

## 3. Bug A: Sector Delivery Ignores `cd_paused` (Unmerged Fix)

### The Mechanism
In [support/minimig/akiko_cd32.cpp](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/support/minimig/akiko_cd32.cpp):
```cpp
static bool akiko_handle_sec_req(void)
{
    // MISSING ON MASTER: if (cd_paused) return false;
    ...
```
- `cmd_pause()` sets `cd_paused = 1`.
- However, `akiko_handle_sec_req()` only gated on `cd_data_lba_base == -1`.
- When the BIOS paused a data read, the host continued fulfilling sector requests over the SPI bridge into the FPGA's PBX buffer (`addressdata + (counter & 31) * 4096`).
- When the BIOS re-armed the next read, it reset `cdrom_sector_counter` to 0. A surplus sector arriving around `ENABLE 0->1` was tagged with counter 0, overwriting PBX slot 0 of the *new* read pass.

### Hardware Measurements
From commit `70bb7da39331` (`fix/akiko-pause-stops-data`):
- **Boots:** Read 3 delivered LBA 17..37 (21 sectors).
- **Hangs:** Read 3 delivered LBA 17..38 (22 sectors, exactly 1 extra sector delivered after pause).

> [!WARNING]
> **Status in Git:** Commit `70bb7da` (`if (cd_paused) return false;`) exists **only** on branches `fix/akiko-pause-stops-data` and `debug/trace-plus-fix`. **It was never merged into `master` or included in PR #77.** Master still lacks this guard!

---

## 4. Bug B: Command Framing Desync & The Phantom Drain

### Live Hardware Trace Evidence
Connecting to the test MiSTer unit (`192.168.2.103`) running commit `90b6312` (`debug/phantom-trace`) captured the exact crash sequence in `/tmp/akiko_dbg.log`:

```text
[ 14649.422319] [akiko] CMD op=0x04 n=13 bytes=d4 05 43 47 06 00 64 80 00 00 04 00 ae
[ 14649.561136] [akiko] DRAIN req_before=1 req_after=1 n=3 bytes=e5 00 1a 
[ 14649.561418] [akiko] DRAIN req_before=1 req_after=0 n=2 bytes=f2 0d 
[ 14649.561474] [akiko] CMD op=0x02 n=2 bytes=f2 0d
[ 14652.217392] [akiko] DRAIN req_before=1 req_after=1 n=2 bytes=00 00 
[ 14652.217477] [akiko] phantom command frame (2 zero bytes) -- dropped
[ 14652.217736] [akiko] DRAIN req_before=1 req_after=0 n=2 bytes=00 00 
[ 14652.217789] [akiko] phantom command frame (2 zero bytes) -- dropped
[ 14653.264040] [akiko] DRAIN req_before=1 req_after=0 n=3 bytes=ea 27 d8 
<NO FURTHER LOGS - SYSTEM HUNG>
```

### Forensic Analysis of the Trace

1. **Sequential Sequence Tags:**
   Notice the upper nibbles of incoming commands increment monotonically:
   - `c5 01 39` -> Tag `C`, Op `5` (LED ON)
   - `d4 ... ae` -> Tag `D`, Op `4` (PLAY DATA)
   - `e5 00 1a` -> Tag `E`, Op `5` (LED OFF)
   - `f2 0d`    -> Tag `F`, Op `2` (PAUSE)

2. **The 2.65-Second Pause Window:**
   Between `14649.561474` (PAUSE) and `14652.217392`, the machine sat idle for 2.65 seconds. `akiko_req` was 0 throughout this time.

3. **The Phantom Drains at `14652.217392`:**
   - Suddenly `AKIKO_STATUS_REQ` asserts.
   - HPS drains, but reads `00 00`!
   - Notice: `req_before=1 req_after=1`!
   - In [akiko_drain_command()](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/support/minimig/akiko_cd32.cpp#L688-L725):
     ```cpp
     DisableIO();
     akiko_wait_status_bit(AKIKO_STATUS_REQ, false, 200);
     ```
     `req_after=1` proves that `akiko_wait_status_bit(..., 200)` **timed out**! The FPGA did not clear `REQ` within 200 iterations (~200 µs).
   - 344 µs later (`14652.217736`), HPS drains another `00 00`. Here `req_after=0`.

4. **The Anatomy of `ea 27 d8`:**
   - One second later, the BIOS issues command Tag `2` INFO (`0x07`):
     - Opcode with tag: `0x27`
     - Checksum: `0xff - 0x27 = 0xd8`
     - Expected frame: `27 d8` (2 bytes)
   - What the host drained: `ea 27 d8` (3 bytes)!
   - **Where did `0xea` come from?**
     Following sequence tag `F` (`f2 0d`), the BIOS issued:
     - Tag `0`: LED ON (`05 01 fa`)
     - Tag `1`: LED OFF (`15 00 ea`) -> checksum byte is `0xea` (`0x15 + 0x00 + 0xea = 0xff`)!
   - Because the two phantom drains pulsed `cmd_done` (`DisableIO()`) while the FPGA FIFO was mid-transfer or misaligned, the body of Tag 1 was lost, leaving its trailing checksum byte `0xea` orphaned at the head of the FIFO!
   - When Tag 2 (`27 d8`) arrived, the host read `0xea` as byte 0:
     - `op = 0xea & 0x0f = 0x0a` -> `command_lengths[0x0a] = 2` -> expects 3 bytes total.
     - Sum: `0xea + 0x27 + 0xd8 = 0x2e9` (`sum & 0xff == 0xe9 != 0xff`).
     - Checksum failed! Host called `cmd_bad(cmd, CH_ERR_CHECKSUM)`.
     - The INFO command was swallowed, BIOS received an unexpected error, halted all CD32 communication, and the loader hung on the black screen.

---

## 5. Why PR #77 Was Only a Partial Fix

PR #77 (`c1041c0` and `9624a5a`) added:
1. `if (zeros) { akiko_diag("phantom command frame... dropped"); return; }`
2. `if (!(akiko_read_status() & AKIKO_STATUS_REQ)) return;` before drain.

While this prevented the host from replying with `CH_ERR_CHECKSUM` to an all-zero frame, **it did not prevent the drain itself from corrupting the FPGA FIFO**.
Calling `akiko_drain_command()` initiates an SPI read transaction (`EnableIO(); spi8(UIO_DMA_READ); spi32_w(0xF400); ... DisableIO();`).
On `DisableIO()`, the FPGA bridge pulses `cmd_done`. If the FPGA asserted `REQ` prematurely or if CDC latency caused a phantom read, pulsing `cmd_done` resets or steps the FPGA framer while the 68020 is writing, leaving leftover bytes in the FIFO.

---

## 6. Recommended Next Steps for Claude

### Action 1: Merge the Sector Pause Fix to Master (Immediate)
In [support/minimig/akiko_cd32.cpp](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/support/minimig/akiko_cd32.cpp#L2204), cherry-pick commit `70bb7da` to ensure `akiko_handle_sec_req()` stops delivering sectors when paused:
```diff
 static bool akiko_handle_sec_req(void)
 {
+    // PAUSE stops the data stream, exactly as it stops the CDDA pump.
+    if (cd_paused) return false;
+
     struct timespec ts0, ts1, ts2, ts3;
```
*Rationale:* Without this, even with 100% clean command frames, 3 out of 4 boots fail due to the surplus sector 38 overwriting PBX slot 0.

### Action 2: Protect the Command FIFO from Phantom Drain Disruption
When a drain returns all zeros (`00 00`):
1. **Do not strobe `cmd_done` on an empty buffer**: If `buf[0] == 0`, investigate if the FPGA bridge should refrain from pulsing `cmd_done`, or if the FPGA's `rd_ptr` can be rewound.
2. **Increase `akiko_wait_status_bit` Timeout & Add Backoff**: The trace proved `akiko_wait_status_bit(AKIKO_STATUS_REQ, false, 200)` timed out (`req_after=1`). Increasing the iteration limit or adding a small microsecond delay ensures the FPGA CDC synchronizer deasserts `REQ` before the poll loop can re-enter.

### Action 3: Resynchronize on Checksum Failure (Frame Resync)
If a frame fails checksum and has length >= 3:
Inspect the buffer for an embedded valid command. In the trace, `ea 27 d8` has `27 d8` starting at offset 1 (`0x27 + 0xd8 == 0xff`, valid opcode 7 INFO). If a leading stray byte is detected:
- Resynchronize by dispatching the embedded valid frame (`27 d8`).
- Do not emit `cmd_bad(CH_ERR_CHECKSUM)` when the trailing bytes form a mathematically valid command frame.

### Action 4: Check FPGA `akiko_hps_bridge.v` Framer Logic
Verify why `akiko_req` asserts with zeroes in the buffer:
- Does the FPGA assert `akiko_req` on opcode 0 (`0x00`) expecting only 1 byte instead of 2?
- Is there a race in the FPGA where `cmd_in_count` asserts `akiko_req` before the 68020 finishes writing the checksum byte?
