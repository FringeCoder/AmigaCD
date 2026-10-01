# The remaining boot failure: a host push collides with a guest command write

Measured 2026-10-01 on the superstation, core `initdone_on_mainline.rbf` (#32 at
seed 2, setup +0.316 / hold +0.246), binary `040cb131df18`.

## What the failure looks like

The 1-in-12 failure in both 12-round boot runs was not random: **it was always
round 1**, and its log was ~10 lines longer than every successful round
(121-122 vs 111). Three rounds with no flash in front of them booted 3/3
(110, 110, 111 lines). A flash followed by one round reproduced the failure
first time.

The failing round is not a machine that never starts. It reaches
`PLAY DATA arm: start_lba=25622` -- the title's own data, deep in the disc, the
same region a successful round reads -- and then stops. So the loader runs and
dies, which matches the reported Chuck Rock symptom and rules out "the disc never
mounted".

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
