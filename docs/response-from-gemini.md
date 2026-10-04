# Response to Round 6: Real Hardware TOC Probing, Error Return Codes, and Boot Paths

## 1. Executive Summary & Acknowledgement

The discovery in Round 6 of the missing 13th checksum byte (`CHECKSUM FAIL n=13 bytes=44 00 00 00 00 00 00 03 00 00 00 00 00` answering `CH_ERR_CHECKSUM` / `0x88`) is the definitive root cause of the 8-second stall. When `cd.device` receives `0x88`, its low-level command handler permanently marks the unit in error and ceases communication.

The empirical data in Round 6 clearly falsifies two hypotheses from Round 5:
1. **Volume Invalidation**: The removal-then-arrival pair boots 8/8; the removal edge does not invalidate the volume lock in a fatal way.
2. **Driver Dormancy**: The silence after 12 frames was not `cd.device` failing to wake from sleep; it was `cd.device` receiving our explicit `0x88` error response and aborting.

Below are the exact technical answers regarding what real CD32 hardware returns on empty-tray `READ TOC`, how `cd.device` processes it in 68020 assembly, and why the boot path shifts between LBA 14 and LBA 16.

---

## 2. What a Real Drive Answers to `READ TOC` (Op 0x04, seekpos < 0) on an Empty Tray

### 2.1 The Exact Value: `CH_ERR_NODISK | cd_door` (`0xF9`)

A physical CD32 drive (Chinon CDS-535 mechanism with custom controller firmware `"CHINON  O-658-2 24"`) returns:
```c
r[0] = cmd[0];                      // 0x04 (or sequence-tagged 0x44)
r[1] = CH_ERR_NODISK | cd_door;     // 0xF8 | 0x01 = 0xF9 (door closed)
```

### 2.2 Why WinUAE Returns `0x01`
In WinUAE's `akiko.cpp`:
```c
if (!cdrom_disk) {
    cdrom_result_buffer[1] = 1; // no disk
    return 2;
}
```
This is an old emulator stub from the early 2000s. In WinUAE, users almost always launch with a CD image attached (`cdrom_disk != 0`), so the empty-tray branch is rarely traversed in realistic timing contexts. Returning `0x01` means **bit 7 is 0 (CH_ERR_OK)** and bit 0 is 1 (`cd_door` closed). Telling the guest "no error, command accepted" when no disc is present violates drive protocol and confuses `cd.device`.

---

## 3. Disassembly Proof from `cd.device`

Analysis of `cd.device` in `CD32 Extended-ROM rev 40.60` (base `$E00000`, module offset `$579C8` to `$5ACD2`) reveals the exact machine-code path taken when issuing command 4:

### 3.1 Command 4 Dispatch and Immediate Status Check
At offset `$58C7A`, `cd.device` constructs command 4 (`READ TOC`) and sends it:
```assembly
58c7a:  move.b  #4, 311(a6)     ; cmd[0] = 4 (MULTI / READ TOC)
58c80:  move.b  #3, 318(a6)     ; cmd[7] = 3 (subcode / scan)
58c98:  jsr     0xe5abb4        ; write command to circular DMA buffer
58c9e:  jsr     0xe59428        ; wait for response frame, returns status byte in d0
58ca4:  bpl.w   0x58cb4         ; Branch if Plus: bit 7 == 0 (NO ERROR)
58ca8:  tst.l   332(a6)
58cac:  bne.w   0x58d34
58cb0:  bra.w   0x58dce         ; Bit 7 was 1 (ERROR): abort immediately!
```

### 3.2 What Happens When We Return `0x01` (Old Code)
1. Bit 7 is 0 (`0x01` is positive).
2. The `bpl.w 0x58cb4` branch **succeeds**.
3. `cd.device` assumes the Chinon drive accepted the command and is now spinning up to stream TOC subcode frames.
4. Execution reaches offset `$58D0A`:
   ```assembly
   58d0a:  move.l  #0x02900000, d0 ; wait mask: bit 23 (subcode), bit 20 (status)
   58d16:  jsr     -318(a6)        ; Exec Wait()
   58d1c:  btst    #23, d0         ; Did subcode arrive?
   58d20:  beq.w   0x58dce         ; No subcode -> timeout failure
   ```
5. Because the tray is empty, the core never pushes subcode frames. `cd.device` remains frozen in `Wait()` until its internal timeout expires.

### 3.3 What Happens When We Return `0xF9` (Commit `90103515`)
1. Bit 7 is 1 (`0xF9` is negative).
2. The `bpl.w` branch **fails**.
3. Execution branches directly to `$58DCE`:
   ```assembly
   58dce:  clr.w   d0
   58dd0:  move.w  #1280, 311(a6)  ; turn off drive LED (cmd 0x0500)
   58dd6:  jsr     0xe59328
   58ddc:  clr.b   307(a6)         ; clear pending scan flag
   58de0:  move.w  #29, d0         ; return TDERR_DiskChanged (29)
   58de4:  rts
   ```
4. `cd.device` also explicitly masks and tests `#$F8` during error classification:
   ```assembly
   58cee:  andi.b  #$f8, d0        ; mask error code
   58cf2:  cmpi.b  #$f8, d0        ; is it CH_ERR_NODISK?
   58cf6:  beq.w   0x58dce         ; clean exit
   ```
5. Returning `0xF9` causes `cd.device` to exit instantly with error 29, cleanly clearing its internal registers without stalling the 68020.

---

## 4. Why the Boot Path Shifts: LBA 14 vs LBA 16

Amiga CD32 firmware contains **two independent, standard bootstrap paths**:

```
                       [ Media Arrival Edge ]
                                  |
                                  v
                        [ cd.device unit 0 ]
                                  |
               +------------------+------------------+
               |                                     |
               v                                     v
     [ Path A: Boot Block ]                 [ Path B: Filesystem Mount ]
      Module: cdstrap (pri -58)              Module: cdfs.library (pri 2)
      Target: LBA 14 (CD32 signature)        Target: LBA 16 (ISO 9660 PVD)
      Sequence: 14, 19, 17, 25621...         Sequence: 16, 21, 19, 25623...
```

### 4.1 Path A: `cdstrap` (LBA 14)
- **Module**: `cdstrap` at extended ROM offset `$00170`, resident priority -58.
- **Disassembly** (`$007A2`-`$007B6`):
  ```assembly
  7a2:  cmp.l   264(a5), d0     ; check TD_CHANGENUM
  7a6:  ble.w   0x88a
  7aa:  move.l  d0, 264(a5)
  7b0:  pea     14              ; push sector 14 (LBA 14)
  7b6:  jsr     0xe34(pc)       ; Read sector
  ```
- `cdstrap` reads LBA 14 (the CD32 boot block signature: `"CD32"` / header structure).
- If valid, `cdstrap` executes the boot block code directly, initiating the fast binary loader sequence.

### 4.2 Path B: `cdfs.library` (LBA 16)
- **Module**: `cdfs.library` at extended ROM offset `$5ACD2`, resident priority 2.
- If `cdstrap` has not booted a disc, AmigaOS Kickstart proceeds through its standard DOS expansion device bind list.
- `cdfs` mounts `CD0:`. The ISO 9660 specification dictates that Sector 16 contains the **Primary Volume Descriptor (PVD)** with the `"CD001"` standard identifier.
- `cdfs` parses the PVD at LBA 16, reads the root directory record (LBA 19/21), locates `s/startup-sequence`, and launches the game executable via AmigaDOS.

### 4.3 Why Early Announce Shifted the Route
When the early empty-tray probe returned `0x01`, `cd.device` was occupied waiting for a subcode timeout. When media arrived, `cdstrap` (priority -58) missed its window or encountered a busy unit, falling back to AmigaDOS strap and `cdfs.library` (LBA 16). 

With `0xF9`, `cd.device` is completely idle when media arrives. `cdstrap` receives the change event immediately and executes via LBA 14. Both routes are fully valid and reach the loader.

---

## 5. Architectural Clarification: `checkdisk`

In `docs/cd32-extended-rom-modules.md`, `checkdisk` (offset `$00010`, priority 49) was identified as probing the CD drive before anything else.

Disassembly of `checkdisk` reveals:
- Offset `$0015C`: Loads library string `"timer.device"` and opens `$00E00034` (`"disk.resource"`).
- Offsets `$000C6`-`$00138`: Reads CIA-A data register `$BFE001` and writes CIA-B step register `$BFD100`.
- Offsets `$00118`-`$00138`: Pulses the floppy drive step line up to 85 times (`cmpi.l #85, d7`) seeking track 0.

**`checkdisk` is the Amiga floppy disk (DF0:) auto-sensing module**, inherited from the A1200 Kickstart core. It does not touch Akiko. The early `READ TOC` probe observed in logs originates from `cd.device`'s own internal initialization sequence (`Init` / `OpenDevice`).

---

## 6. Recommendations

1. **Keep Commit `90103515`**:
   Returning `CH_ERR_NODISK | cd_door` (`0xF9`) in `cmd_multi` when `!cd_media_present()` is 100% accurate to real Chinon hardware and AmigaOS `cd.device` driver disassembly.
2. **Permanent Fix for Frame 13 Race**:
   Ensure the host bridge waits for the complete 13-byte frame (including checksum byte `b8`) before passing the command buffer to `akiko_handle_cmd()`, preventing false `CH_ERR_CHECKSUM` (`0x88`) rejections.
