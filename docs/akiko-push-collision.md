# WITHDRAWN: the first boot after a flash is not a push collision

> **This file's conclusion is wrong and is withdrawn.** Everything below about
> the deterministic repro stands; the diagnosis built on it does not.
>
> The argument rested on `CHECKSUM FAIL` counts of 5 and 8 in failing captures.
> Those came from an **uncleaned 75-second window** that spanned the flash,
> several OSD interactions and more than one boot attempt -- not from the boot
> that failed. Scored per round, with the log cleared at the round's start, a
> failing first boot has `CHECKSUM FAIL`=1 and `PHANTOM`=0: **identical to a
> successful round.** There is no frame corruption in the failure at all, so a
> host push cannot be its cause.
>
> **The real signature**, from six failing trials with per-round logs: the BIOS
> issues one read at lba 14, our side then streams 60 sequential sectors
> (`DBG sec_req#N base=14 counter=N -> lba=14,15,16...`), and the loader never
> starts. A successful round has **zero** `DBG sec_req` lines and one
> `start_lba=12`. 60 versus 0 is the cleanest discriminator found so far, and it
> is where the next investigation should start.
>
> | | failed first boot | success |
> |---|---|---|
> | `DBG sec_req#N base=14` | 60 | 0 |
> | `start_lba=12` (loader) | 0 | 1 |
> | `CHECKSUM FAIL` | 1 | 1 |
> | `PHANTOM` | 0 | 0 |
>
> The RTL fix this file pointed to was merged anyway, on its own merits and not
> as a boot fix (FringeCoder/AmigaCD_MiSTer#37): `rx_can_start` genuinely lacked
> a `!tx_busy` term while TX refuses to start under RX in two places, and
> `tb_akiko_push_vs_tx` proves the corruption is reachable with a 3-cycle window.
> Interleaved first-boot trials gave base 0/3 and fix 0/3, identical in every
> column -- no fix and no regression.
>
> The method lesson is the same one this file already records, applied to itself:
> a log-derived difference has to be re-measured with the window scoped to one
> round before anything is concluded from it. The uncleaned trace was the right
> tool for finding what the earlier clearing had hidden, and the wrong tool for
> counting events per boot.

---

## The original write-up follows, for its evidence and its repro


Measured 2026-10-01 on the superstation, core `initdone_on_mainline.rbf` (#32 at
seed 2, setup +0.316 / hold +0.246), binary `040cb131df18`.

## What the failure looks like

The 1-in-12 failure in both 12-round boot runs was not random: **it was always
round 1**, and its log was ~10 lines longer than every successful round
(121-122 vs 111). Three rounds with no flash in front of them booted 3/3
(110, 110, 111 lines). A flash followed by one round reproduced the failure
first time.

The failing round reaches the BIOS's own first read and then dies on a corrupted
command frame, which matches the reported Chuck Rock symptom: the loader runs and
gives up rather than the disc never mounting.

**Correction.** An earlier version of this file argued the failing round "reaches
`PLAY DATA arm: start_lba=25622` -- the title's own data, deep in the disc". That
was wrong and the argument built on it is withdrawn: the highest
`PLAY DATA arm: start_lba=` in a log is 26505 in *every* capture, successes and
failures alike, because that is the lead-out. It is not evidence of anything.

## What ends it

Every corrupted frame is immediately preceded by a HOST PUSH. Three occurrences
in one trace, two distinct shapes:

```
[akiko] MULTI scan-TOC trigger (points=4)
[akiko] TOC push idx=0 point_idx=0 point=0xa0 ctrl=0x04 msf=01:00:00
[akiko] CHECKSUM FAIL n=3 expected=3 sum=b6 bytes=b5 01 00
[akiko] BAD cmd=b5 err=88
[akiko] PHANTOM n=2 bytes=00 00 -- dropped
[akiko] CHECKSUM FAIL n=2 expected=2 sum=49 bytes=00 49
```

```
[akiko] media-status push: opcode=0x0a status=0x01
[akiko] CHECKSUM FAIL n=13 expected=13 sum=47 bytes=44 00 00 00 00 00 00 03 00 00 00 00 00
[akiko] BAD cmd=44 err=88
```

The second one names the mechanism precisely. That 13-byte frame appears
correctly elsewhere in the same trace as

```
[akiko] CMD op=0x04 n=13 bytes=84 00 00 00 00 00 00 03 00 00 00 00 78
```

so the guest wrote the same command, and what arrived had its first byte
`0x84 -> 0x44` and its checksum byte `0x78 -> 0x00`. Bits were lost from a frame
that was in flight while the host wrote a response into the channel.

## Which fields actually discriminate

Across seven captured rounds, a booted round and a failed round differ like this,
consistently:

| field | booted | failed |
|---|---|---|
| `start_lba=12 ` | 1 | 0 |
| `start_lba=14 ` | 2 | 1 |
| log lines | ~110 | 120-400 |
| `CHECKSUM FAIL` | 1 | 5-8 |

So `start_lba=12` -- `boot-ab.py`'s success criterion -- is sound, and a claim
elsewhere that it gave a false negative is withdrawn. `CHECKSUM FAIL` discriminates
with a **baseline of 1**: one per boot is normal and five or more marks the failure,
so reading it as "any at all is the bug" is wrong in the other direction. A log of
about 37 lines means nothing mounted -- a void round, not a failure.

## Why the existing guard does not prevent it

Both push sites already check it:

- `akiko_cd32.cpp:2771` -- the media-status push, `if (cd_media_push_pending &&
  cd_initialized >= 2 && rx_idle ...)`
- `akiko_cd32.cpp:2790` -- the TOC drip, `if (cd_initialized == 2 &&
  toc_push_idx >= 0 && rx_idle)`, with a comment noting it was gated on
  `rx_idle` in Phase 18 because "pushing into a busy buffer would silently drop
  the entry"

`rx_idle` is `!(status & AKIKO_STATUS_RX_BUSY)`, read over SPI. **It is a sample,
not a lock.** Between the host reading the status and the host writing the
response, the guest is free to start a command write -- and if `RX_BUSY` only
asserts once a frame is complete, there is no window in which the flag reports a
write that has begun. Either way the host and the guest can be writing at the
same time, and the guard cannot close that by construction.

Phase 18's reasoning was about the host's own entry being dropped. The damage in
the other direction -- the host's write corrupting the GUEST's frame -- needs a
real interlock, and that belongs in the FPGA, not in a flag the host polls.

## What this is not

- **Not the phantom drain.** #28's scanline inhibit eliminates phantoms
  absolutely (14 rounds: `2,2,2,...` base, `0,0,0,...` fix) and did not change
  the boot rate (p = 1.000). The two `PHANTOM` lines here are consequences of a
  frame that was already corrupt, not the cause.
- **Not the frame resync.** `RESYNC` fired 0 times. #79's resync slides to the
  next correctly-summed frame, which repairs a byte-offset slip; this corruption
  is INSIDE a frame, so there is nothing for it to resynchronise to.
- **Not the per-game NVRAM.** This was the first hypothesis, from four cleaned
  logs in which the failing round had no `set_cd_path` and no `NVR loaded` while
  every passing round had both. **It is wrong.** An uncleaned trace of a failing
  round has `set_cd_path` with the fadvise and the deferred load armed, plus
  `init: scheduling reload ... after BRAM wipe` and `NVR loaded (1024 bytes ...)`
  -- everything a passing round has. The missing lines were an artifact of the
  harness clearing the log after those calls had already run.
- **Not stale host state across a reflash.** Also checked and also wrong:
  `fpga_load_rbf()` ends in `app_restart()`, and the app's PID changes across a
  `load_core` (2569 -> 6716, measured), so every static starts fresh.

## The fix for this, and why it is not the boot fix

`rx_can_start` checked `CDFLAG_RXD`, `receive_length`, the index pair and the
post-write inhibit -- but not `tx_busy`, while TX refuses to start under RX in two
separate places. A `hps_result_done` arriving mid-fetch therefore raised `rx_busy`
under an outstanding TX byte, RX won the DMA address mux and took the ack the fetch
was waiting for, and the command buffer stored RX's byte at the fetch's index with
one byte of the frame never read.

`tb_akiko_push_vs_tx` reproduces exactly that -- first byte `0x84 -> 0x78` (the
displaced byte, carrying another offset's value), last byte `0x78 -> 0x00`, `txinx`
6 -> 5 -- and the one-line gate closes it: 19 checks, 0 errors, including an
every-cycle invariant that `tx_busy` and `rx_busy` are never both high. Reverting
the gate fails the bytes AND the invariant, at a 3-cycle window.

**On hardware it does not fix the first boot.** Fitted at seed 4 and flashed, round
1 still failed with `start_lba=12`=0 and `CHECKSUM FAIL`=8. So the collision is real
and worth closing on its own merits, but it is not what dominates here. The change
is FringeCoder/AmigaCD_MiSTer#37, held rather than merged as a boot fix, with a
matched-seed interleaved A/B outstanding to say whether it moves the rate at all.

## Where to look next

The interlock is the question, and it is in the RTL: what makes a host response
write and a guest command write mutually exclusive on the command channel?
`cmd_pending` gating already exists on the RTL side for a related case (a
`cmd_done` clearing a command mid-write). The shape of the fix is to refuse or
defer the host write while a guest frame is in progress, rather than to sample a
status bit and hope.

The reproduction is cheap and deterministic, which is the part that was missing
before: flash the core, then mount and boot ONCE. `scratchpad/trace_first_mount.py`
does it and dumps the log at three points without ever clearing it -- the
clearing is what hid this for two runs.
