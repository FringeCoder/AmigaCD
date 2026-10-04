# What is actually inside the CD32 extended ROM

**Date:** 2026-10-04
**Occasion:** a proposal that the ROM contains two independent consumers of the
CD drive — a boot-ROM animation talking to Akiko directly, and `cd.device` —
which would explain the two `READ TOC` scans seen in every boot.
**Method:** read out of the ROM on the test machine rather than from
documentation.

`/media/fat/_Computer/_CD32/Bios/CD32 Extended-ROM rev 40.60 (1993)(Commodore)(CD32).rom`,
512 KB. AmigaOS resident modules are found by scanning for the `$4AFC`
matchword and keeping those whose self-pointer equals their own address; that
puts the ROM at base **`$E00000`**.

| offset | pri | module |
|---|---|---|
| `$00010` | 49 | `checkdisk` |
| `$00170` | −58 | `cdstrap` |
| `$024c4` | 0 | `cdui.library` |
| `$57888` | −50 | `reqzapper` |
| `$579c8` | 8 | **`cd.device`** |
| `$5acd2` | 2 | `cdfs.library` |
| `$5f17c` | 0 | `player.library` |
| `$611ec` | 0 | `nonvolatile.library` |
| `$62c30` | 1 | `lowlevel.library` |
| `$6459c` | 0 | `freeanim.library` |
| `$64788` | 7 | `cdg.library` |
| `$67e50` | 63 | `debox.library` |
| `$69198` | −59 | `kludges` |
| `$69d92` | 8 | `cdtv.device` |
| `$6b42e` | 64 | `bookmark.device` |
| `$6bae4` | 0 | `playerprefs.library` |

## There is one driver, not two

Only `cd.device` contains instruction-level references to the Akiko register
page. Searching for `$00B8xxxx` constants and keeping those that are genuine 68k
operands — preceded by `movea.l #imm,An`, `move.x abs.l`, `btst abs.l` and the
like — leaves **six hits, `$B80000` and `$B80008`, all inside `cd.device`**.

A loose byte-pattern scan also "finds" `cdui.library` twenty-four times. That is
animation data, not code, and it is why the loose scan cannot be used to answer
this question.

So the splash animation (`cdui`, `freeanim`, `player`), the boot strap
(`cdstrap`) and the early `checkdisk` resident are **clients of `cd.device`**.
Nothing else drives the hardware.

## The two TOC scans are two clients, not two drivers

The prediction was right and the mechanism was not. Two `READ TOC` commands per
boot is normal, and both come from one driver serving different callers at
different times, which is exactly what our logs show:

- `checkdisk`, priority 49, runs before almost anything else and probes for a
  disc — this is the first scan, which arrives while the drive still reports
  empty;
- after the media-change, `cdstrap` and `cdfs.library` mount the disc — the
  second scan.

The distinction matters for us. With a single driver there is no second entity
holding its own state to fall out of step: every answer we give lands on
`cd.device`. That is precisely how one rejected frame killed a boot, see
`first-boot-after-flash.md`.

## Method caveat

Absence under the strict test is not proof of absence. A module that loads the
Akiko base into a register once, or computes it, would not match the operand
filter. `nonvolatile.library` appears only in the loose scan and probably does
reach Akiko that way, since the NVRAM lives behind it.

## What `cd.device` does with our answer, from its own code

Gemini supplied a disassembly of the command-4 path and it checks out against
the ROM byte for byte, so it is quoted here as verified rather than claimed:

| address | bytes | instruction |
|---|---|---|
| `$E58C7A` | `1d7c 0004 0137` | `move.b #4,311(a6)` — cmd[0] = 4 |
| `$E58C80` | `1d7c 0003 013e` | `move.b #3,318(a6)` — cmd[7] = 3 |
| `$E58C98` | `4eb9 00e5abb4` | `jsr` — write the command to the DMA buffer |
| `$E58C9E` | `4eb9 00e59428` | `jsr` — await the response, status in d0 |
| `$E58CA4` | `6a00 000e` | `bpl.w` — **bit 7 set means abort** |
| `$E58D0A` | `203c 02900000` | wait mask: bit 23 subcode, bit 20 status |
| `$E58D16` | `4eae fec2` | `jsr -318(a6)` — Exec `Wait()` |
| `$E58D1C` | `0800 0017` | `btst #23,d0` — did subcode arrive? |

So an error status to command 4 makes `cd.device` give up immediately, and a
non-error status makes it wait for subcode frames that an empty drive will never
send, until its own timeout.

## And why we still answer `0x01` to an empty drive

Three commands — STOP, PAUSE, UNPAUSE — answer a probe of an empty drive with
`CH_ERR_NODISK | cd_door` (`0xf9`), matching WinUAE's `checkerr()`. The multi
command answers `0x01`: door closed, no error. That looks like an oversight, and
`checkdisk` (priority 49) hits it on every cold boot while the announce is held.

Changing it to `0xf9` was built and measured, and it is **worse**:

| announce hold | `0x01` | `0xf9` |
|---|---|---|
| 8 s | **16/16** | **0/6**, no read ever armed |
| 14 s (shipped) | boots, opens at lba 14 | boots, opens at lba 14 |

The disassembly explains it exactly: `0xf9` sets bit 7, `cd.device` takes the
abort branch at `$E58CA4` and never comes back; `0x01` leaves it waiting on
subcode until a timeout it recovers from. So the faithful-looking answer narrows
the window the boot survives in, and the shipped value is kept deliberately.

What would make `0xf9` safe is not known: presumably the recovery after that
abort depends on a media-change edge we deliver in a window the ROM is not
listening to, which is the same shape as the problem in
`first-boot-after-flash.md`. Worth revisiting only with a measurement, never on
the grounds that it looks more correct.