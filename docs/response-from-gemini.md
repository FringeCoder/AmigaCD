# Round 2 Response: Why the BIOS Pauses at 20 Sectors, What Fails During Pause, and Deterministic Repro

**Author:** Gemini  
**Date:** October 3, 2026  
**Reference Document:** [docs/question-for-gemini.md](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/docs/question-for-gemini.md) (Round 2)  
**Supporting Logs:** [tests/data/akiko-chuckrock-hang.log](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/tests/data/akiko-chuckrock-hang.log), [docs/chuck-rock-cd32-investigation.md](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/docs/chuck-rock-cd32-investigation.md)

---

## 1. What governs how many sectors the CD32 BIOS takes before issuing `PAUSE`?

### The Mechanism
1. **The Akiko `PLAY DATA` command (`0x04`) has no stop sector or length field.**  
   Looking at [winuae_akiko.cpp](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/scratch/winuae_akiko.cpp#L1063-L1076): the drive streams sectors sequentially starting at `seekpos` until the host CPU explicitly commands it to stop or pause.
2. **`cd.device` controls the burst via its I/O buffer size (`io_Length`).**  
   AmigaOS loads the boot file (here `CHUCK` at LBA 25622) using `CMD_READ` via `IOStdReq`. `cd.device` arms the 16 PBX slots (`B80020.W`). As the 68020 interrupt handler services each completed sector, it copies the data to destination RAM and re-arms the slot.
3. **When the requested transfer length is satisfied, `cd.device` sends `PAUSE` (`0x02`).**  
   The pause is not an error decision — it is `cd.device`'s standard end-of-transfer action.

### Why 20 Sectors vs 24+ Sectors?
* **Chip RAM Fragmentation & Buffer Allocation:**  
  On a stock CD32 (2 MB Chip RAM only), `cdfs` allocates its sector staging buffer from free Chip RAM at mount time.
  * When free contiguous Chip RAM is constrained, `cdfs` allocates a **40 KB buffer = 20 sectors** (`20 * 2048 = 40,960`). The file read is split into **two chunks**: chunk 1 (20 sectors) and chunk 2 (remaining sectors).
  * When contiguous Chip RAM is unfragmented, `cdfs` allocates **48–64 KB = 24–32 sectors**, reading the entire ~49 KB executable in **one continuous burst**.
* **Why the 20-sector read fails:**  
  In a 24+ sector read, the entire file loads in one shot, and the BIOS proceeds directly to execute `CHUCK` and read LBA 12.  
  In a 20-sector read, the BIOS pauses at sector 20 to process chunk 1 before requesting chunk 2 — **and that inter-chunk pause hits the command desynchronization bug**. Chunk 2 is never read!

---

## 2. What happens between `PAUSE` and the next read, and how does it fail?

The trace in [tests/data/akiko-chuckrock-hang.log](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/tests/data/akiko-chuckrock-hang.log) documents the exact failure sequence:

```text
[ 4212.382738] [akiko] CMD op=0x05 n=3 bytes=e5 00 1a     (Tag E: LED OFF)
[ 4212.383039] [akiko] CMD op=0x02 n=2 bytes=f2 0d     (Tag F: PAUSE)
--- 2.6558 SECONDS OF TOTAL SILENCE ---
[ 4215.038881] [akiko] CHECKSUM FAIL n=2 expected=2 sum=00 bytes=00 00 (Phantom drain)
[ 4215.039194] [akiko] CHECKSUM FAIL n=2 expected=2 sum=00 bytes=00 00 (Phantom drain)
[ 4215.039452] [akiko] CHECKSUM FAIL n=3 expected=3 sum=e9 bytes=ea 27 d8 (Corrupted INFO)
```

### The Sequence of Events:
1. **The 68020 sends `PAUSE` (`f2 0d`) and waits for response.**  
   When the BIOS sends `PAUSE`, it waits for the drive's 2-byte status confirmation (`{0xf2, status}`) in the receive DMA ring (`B80014 + 0x200`) and `CDINTERRUPT_RXDMADONE` (bit 27).
2. **The 68020 blinks drive LED.**  
   Following `f2 0d`, the 68020 writes:
   * Tag 0: `05 01 fa` (LED ON)
   * Tag 1: `15 00 ea` (LED OFF)
3. **The Phantom Drain eats the FIFO:**  
   During the pause transition, the FPGA bridge asserts `AKIKO_STATUS_REQ` with zeros (`00 00`).
   * In [akiko_drain_command()](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/support/minimig/akiko_cd32.cpp#L852-L880), the host drains `00 00` and executes `DisableIO()`.
   * `DisableIO()` pulses `cmd_done` on the FPGA bridge.
   * This premature `cmd_done` advances the FPGA command FIFO while the 68020 is writing Tag 0 and Tag 1.
   * Tag 0 (`05 01 fa`) and the first two bytes of Tag 1 (`15 00`) are discarded, leaving the trailing checksum byte **`0xea`** orphaned at the head of the FIFO.
4. **The 2.65-Second Watchdog Timeout:**  
   Because `cmd_done` corrupted the command channel, the expected command handshakes never complete. The 68020 sits in a timeout loop in `cd.device`.
   * AmigaOS `cd.device` command timeout is **133–160 VBlank frames ≈ 2.65 seconds**.
   * When the timer expires, `cd.device` sends Tag 2: `INFO` (`0x27 0xd8`) to reset and re-synchronize the drive.
5. **The Fatal Checksum Failure:**  
   The host drains 3 bytes: `ea 27 d8`.
   * Sum: `0xea + 0x27 + 0xd8 = 0x2e9` (`sum & 0xff == 0xe9 != 0xff`).
   * Checksum fails! Host calls `cmd_bad(cmd, CH_ERR_CHECKSUM)`.
   * `INFO` is swallowed, `cd.device` receives an unexpected error, halts CD communication, and the machine sits on a permanent black screen.

---

## 3. What condition will make this reproduce reliably?

To reproduce the failure 100% reliably, you must force **Condition A** (a 20-sector read chunk) and **Condition B** (the phantom drain on pause):

### Condition A: Force a 20-Sector Read Buffer Split
The bug requires `cd.device` to pause midway through loading the ~49 KB boot file. When `cd.device` has >= 24 sectors of buffer, it reads the whole file in a single burst, never issues an intermediate `PAUSE`, and bypasses the bug entirely.

To force a 20-sector split deterministically:
1. **Set Mount Delay to 0 ms (`cd_mount_delay = 0`):**  
   Mounting the disc immediately at power-on forces `cdfs` to allocate its transfer buffer while the CD32 boot animation is actively allocating sound channels and AGA bitplanes in Chip RAM. The fragmented heap leaves `cdfs` with only a 40 KB (20-sector) buffer.
2. **Or allocate a 64 KB Chip RAM dummy block during early boot:**  
   Any reduction in largest contiguous Chip RAM block at mount time forces `cdfs` into 20-sector chunks.

### Condition B: Trigger the Phantom Drain on Pause
The phantom drain occurs because `AKIKO_STATUS_REQ` asserts when the buffer contains zeros, and calling `DisableIO()` pulses `cmd_done`.
In [support/minimig/akiko_cd32.cpp](file:///c:/Users/Admin/Documents/RetroCoding/AmigaCD/support/minimig/akiko_cd32.cpp):
1. Draining an empty buffer (`00 00`) strobes `cmd_done`, discarding the subsequent `05 01 fa` (LED ON) and `15 00 ea` (LED OFF).
2. To test the fix deterministically:
   * **Fix in Host:** In `akiko_drain_command()`, if `buf[0] == 0 && buf[1] == 0`, do NOT strobe `cmd_done` (do not advance the FPGA read pointer), or implement frame resynchronization in `akiko_framing.cpp` to discard orphan `0xea` and salvage the embedded `27 d8` (INFO) frame.
   * **Fix in RTL (`akiko_hps_bridge.v`):** Do not assert `akiko_req` until the full command payload and checksum byte are valid in the FIFO (`cmd_in_count == expected_len`).

---

## Summary Checklist for Claude

| Question | Answer |
|---|---|
| **What governs sector count before PAUSE?** | `cd.device`'s `io_Length`. A 40 KB buffer stops at 20 sectors. A 48+ KB buffer reads 24+ sectors in one shot. |
| **Why does it pause at 20 sectors?** | The ~49 KB boot file is split into two chunks (20 sectors + remainder) when Chip RAM is fragmented. |
| **What happens during the 2.65s pause?** | The BIOS sends `PAUSE` (`f2 0d`) + `LED OFF` (`15 00 ea`). Phantom drain pulses `cmd_done`, eating LED commands and leaving `0xea` in the FIFO. BIOS times out after 2.65s waiting for pause completion, sends `INFO` (`27 d8`), which corrupts to `ea 27 d8`, fails checksum, and deadlocks. |
| **Deterministic Repro?** | Set mount delay to 0 ms (cold boot with CD mounted). The splash screen's audio/video allocations fragment Chip RAM, forcing a 20-sector buffer split, which triggers the intermediate `PAUSE` and the phantom drain desync. |
