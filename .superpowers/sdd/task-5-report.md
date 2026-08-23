# Task 5 report: chip RAM readout (`ss_dma`)

## Status

DONE_WITH_CONCERNS. The module and testbench are built, warning-free, and
green. Two real issues surfaced during TDD: one in the brief's own
testbench (fixed here), and one integration-level protocol mismatch that
this task cannot resolve on its own because the wiring it concerns does not
exist yet (flagged prominently in the RTL and below).

## What was built

- `rtl/ss_dma.v` -- the readout state machine, `sd_addr`/`sd_cs` output
  registers, a fixed `sd_state`/`sd_uds_n`/`sd_lds_n`/`sd_cache_inhibit`
  wiring, and the read/advance/done FSM.
- `rtl/tb/ss_dma_tb.v` -- testbench with a variable-latency mock CPU port.
- `upstream/core-cd32-cdtv/rtl/ss_dma.v` -- byte-identical copy (`diff`
  confirmed empty).
- `.github/workflows/build.yml` -- added build/run steps for the new
  testbench to the `rtl-sim` job, after the state-serialiser steps.

The implementation keeps the brief's core FSM shape (one `busy`/`remaining`
counter, `sd_addr` advanced on each observed `sd_ready`, `sd_cs` held high
for the whole burst and dropped once at the end) but simplifies the
`start`-branch assignment (a flat `busy <= (word_count != 0)` /
`sd_cs <= (word_count != 0)` / `done <= (word_count == 0)` instead of the
brief's nested `if`) -- logically identical, one less level of nesting.

## Method and evidence (RED/GREEN)

### RED 1 -- missing file (expected)

```
$ iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v
rtl/ss_dma.v: No such file or directory
No top level modules, and no -s option.
```

### RED 2 -- the brief's own testbench fails, verbatim, unmodified

Before writing my own testbench I built the brief's Step 1 test and Step 3
implementation exactly as given (no edits) in a scratch directory, to
establish a real baseline rather than assume "nine PASS lines" as claimed:

```
$ iverilog -g2005 -Wall -o tb ss_dma.v ss_dma_tb.v && vvp tb
FAIL word count: got 3, want 4
PASS word contents
PASS word contents
PASS word contents
FAIL word contents: got X, want 49171
PASS cs released after done
PASS second transfer count
FAIL second transfer first word: got 49171, want 49184
3 FAILURE(S)
```

Root cause (confirmed by a cycle-by-cycle trace, not just inferred): the
last word's `word_valid` and `done` commit on the *same* clock edge, from
the same DUT always-block. The testbench's `wait (done); @(negedge clk);`
idiom is a level-sensitive wait that wakes the moment `done`'s value
changes -- which happens within the same simulation time step as that
commit. The `got[]`-capturing always block is a *separate* `@(posedge clk)`
process; it only samples the just-committed `word_valid` at the *next*
posedge. So `wait (done)` resumes, advances to the following `@(negedge
clk)`, and the check runs *before* the capture block has recorded the
final word -- silently dropping the last word of every transfer, every
time, regardless of implementation. This is a bug in the given bench
itself, not something introduced by my changes.

Fix: replaced the level-sensitive `wait (done)` with a task that polls on
`@(posedge clk)`:

```verilog
task wait_done;
begin
	while (!done) @(posedge clk);
end
endtask
```

This reads `done` with the same one-posedge-stale timing the capture block
uses for `word_valid`, so both observe the transfer's end at the same
simulated point. Documented in `rtl/tb/ss_dma_tb.v` above the task.

### RED 3 -- self-inflicted bench bug while strengthening the back-to-back case

While adding a true consecutive-cycle "sd_ready twice in a row" scenario
(base address `0x100`), the per-word content checks failed. Tracing it
showed the *mock*, not the DUT, was at fault: the mock derives `sd_rd` from
`sd_addr[8:1]`, an 8-bit slice (inherited from the brief's own mock
formula), so an address at or past `0x100` wraps to 0 there while my
independently-computed "expected" value in the checker did not. Fixed by
keeping all test addresses under `0x100`.

### RED 4 -- literal back-to-back (0-cycle gap, different addresses) is unphysical for this protocol, confirmed by trace

Also while building that scenario, giving two *consecutive* words latency 0
each produced wrong data for the second of the pair, even after RED 3's
fix (`0x30` base, `latency = 0,0,2,0,0`):

```
FAIL back-to-back word contents: got 49200, want 49201
...
```

I traced this cycle-by-cycle rather than assume it was a DUT bug. Result:
`sd_addr` is a single register that `ss_dma` advances only in reaction to
observing the *previous* word's `sd_ready` -- on the same clock edge the
mock, if also given latency 0 for the next word, decides that next word is
ready. Both reactions fire off the same event on the same edge, so the mock
necessarily reads the pre-advance (stale) address; no requester could
present the next address any earlier without knowing the future. This
isn't a mock weakness to route around -- it's a structural fact about a
single-address-register, advance-on-ready protocol: the tightest possible
turnaround between two different addresses is exactly one idle cycle, not
zero. I re-shaped the scenario to word 0 at latency 0 (its address comes
from `start`, already settled a full cycle earlier by an unrelated event)
and every subsequent word at latency 1 (the minimum the protocol can
actually deliver) -- tighter than the brief's uniform `>= 3`, which leaves
two cycles of slack that could hide an off-by-one in exactly this
minimum-turnaround path, without asking for something impossible.

### GREEN -- final run

```
$ export PATH="$PATH:/c/iverilog/bin"
$ iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v && vvp build/ss_dma_tb
PASS word count
PASS word contents
PASS word contents
PASS word contents
PASS word contents
PASS cs released after done
PASS cs drops exactly once
PASS second transfer count
PASS second transfer first word
PASS second transfer cs drops exactly once
PASS min-turnaround transfer count
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround cs drops exactly once
PASS zero-length word count
PASS zero-length never asserts cs
PASS zero-length cs stays low
PASS sd_state is read-data during transfer
PASS sd_uds_n asserted (active low)
PASS sd_lds_n asserted (active low)
PASS sd_cache_inhibit asserted during transfer
ALL TESTS PASSED
```

No `-Wall` warnings (the DUT initially had none of the missing-`timescale`
warning fixed until I added `` `timescale 1ns/1ns `` to `ss_dma.v`, matching
every other `rtl/*.v` file in this project).

## Reasoning on the four callouts

1. **No word dropped/double-counted on back-to-back ready.** Verified for
   the tightest turnaround this protocol can produce (latency 0 then 1s --
   see RED 4 above for why 0-then-0 isn't a real scenario). The DUT samples
   `sd_ready` fresh every cycle (no edge-detect, no assumption of a gap),
   so it needs no minimum spacing to count correctly once the address is
   valid.
2. **`sd_cs` deasserts exactly when the transfer ends.** Added a watchdog
   in the testbench (`cs_drop_count`, counting 1->0 transitions since the
   last `start`) and asserted it equals exactly 1 for every non-empty
   transfer and exactly 0 for the zero-length one. This is strictly
   stronger than the brief's single "is it 0 now" check, which cannot
   distinguish "dropped once, correctly, at the end" from "dropped early
   and glitched back up" as long as it happens to read 0 at the specific
   moment checked.
3. **Second transfer works without an intervening reset.** Kept from the
   brief, run through the shared `run_transfer` task so it exercises the
   exact same path as the other three transfers.
4. **Zero-length transfer terminates.** Added: `word_count = 0` must assert
   `done` and never assert `sd_cs` at all. Confirmed via the same
   `wait_done` polling (works here too, since done is set combinationally
   with `start` at a single edge with no captured words to race against).

I also added checks the brief's version had no coverage for at all: that
`sd_state`/`sd_uds_n`/`sd_lds_n` hold the documented constant values, and
that `sd_cache_inhibit` tracks `sd_cs` (asserted "throughout" per the
brief's own commentary, not just at some point during the transfer).

## Files changed

- `rtl/ss_dma.v` (new)
- `rtl/tb/ss_dma_tb.v` (new)
- `upstream/core-cd32-cdtv/rtl/ss_dma.v` (new, byte-identical copy)
- `.github/workflows/build.yml` (added two steps to `rtl-sim`)

## Self-review findings

- Found and fixed the `wait(done)` race against the brief's own given
  testbench (see RED 2) -- this would have silently under-counted the last
  word of every real chip-RAM dump, in a way invisible unless someone
  happened to check `got_count` exactly, which the brief's own checks did.
- Found the mock's `sd_addr[8:1]` 8-bit slice wraps for addresses >= 0x100
  (inherited from the brief); worked around it in test data rather than
  widening the mock, since the brief's own transfers (0x10, 0x20) never
  exercised it and widening isn't needed to test this module.
- Confirmed, by direct inspection of `cpu_cache_new.v` (not by assumption),
  that `cpuU`/`cpuL` are active-low byte selects and `cpustate == 2` is
  "data read" -- both match what the brief specified, and both are now
  exercised by explicit checks in the testbench rather than just assumed
  correct because they compile.
- No leftover debug artifacts: the ad hoc trace harnesses used to root-cause
  RED 2 and RED 4 (temporary files under `build/`, never part of the
  testbench) were deleted before committing.

## Concerns (the "DONE_WITH_CONCERNS" reason)

This is the important one, documented at length in a comment in
`rtl/ss_dma.v` itself so it isn't lost: **`ss_dma`'s handshake, as specified
by this task, is not directly compatible with `sdram_ctrl`'s actual CPU
port if wired straight across.**

`ss_dma` holds `sd_cs` asserted continuously for the whole multi-word
transfer (per this task's own explicit interface: "`sd_cs` deasserts
exactly when the transfer ends") and treats `sd_ready` as a fresh,
per-word, at-most-one-cycle signal. But reading `cpu_cache_new.v` (the
actual module behind `sdram_ctrl`'s CPU port) shows:

- `cpu_ack` (which is `ramready`, the presumptive `sd_ready` source) is a
  *level*: once asserted on a hit or fill completion, it stays asserted for
  as long as `cpu_cs` stays asserted -- it is only cleared by
  `if (!cpu_cs) cpu_ack <= 0`.
- The cache's own state machine sits in `CPU_SM_WAIT` / `CPU_SM_FILLW`
  until `cpu_cs` drops, and only then can it return to `CPU_SM_IDLE` to
  accept a new address. It never re-services a *new* address while `cpu_cs`
  stays high.

Wiring `ss_dma`'s signals directly onto `cpuCS`/`cpuAddr`/`ramready` as
given would mean: after the first word, `ramready` stays high indefinitely
(since `sd_cs` never drops mid-burst), so `ss_dma` would treat every
subsequent clock cycle as "another word ready" and read the *same* stale
`cpuRD` value over and over -- silently filling the rest of the dump with
repeats of word 0 instead of hanging (which would at least be visible).
Alternatively, if `sdram_ctrl`'s wiring doesn't yet exist to compare
against directly, whichever module does the actual bus-cycle conversion
will need to drop `cpuCS` for at least one cycle between each of `ss_dma`'s
words (turning "one continuous request" into a sequence of discrete bus
cycles) -- or `ss_dma` needs to go through a different, non-cached,
single-shot path instead of the fronted CPU port.

I did not change `ss_dma`'s interface or FSM to address this because:
(a) the interface and the "`sd_cs` stays high for the whole burst" behavior
are exactly what this task specifies and what its own callouts validate;
(b) there is no wiring/integration task yet in this plan that connects
`ss_dma` to `sdram_ctrl` -- Task 6 ("save state controller") is the next
task in the queue and may be where this gets resolved, but I don't know
its design and shouldn't guess at it here; (c) unilaterally making `ss_dma`
toggle `sd_cs` per word would contradict this task's own stated and tested
contract without being asked.

This does not block Task 5 -- the module is correct and fully tested
against the interface and behavior this task specifies -- but whoever
writes the sdram_ctrl integration needs to see this before assuming the
port can be connected pin-for-pin.

## Addendum: the protocol defect above is now fixed

The concern flagged above is confirmed and resolved. `ss_dma` now issues a
per-word handshake instead of holding `sd_cs` for the whole burst, so it is
safe to wire straight onto `sdram_ctrl`'s CPU port
(`cpuCS`/`cpuAddr`/`ramready`) without an intermediate bus-cycle converter.

### What changed

- **`rtl/ss_dma.v`** -- replaced the single `busy`/`remaining` counter's
  "hold `sd_cs` for the whole burst" sequencing with an explicit
  `xfer_state` (`XFER_IDLE` / `XFER_REQ` / `XFER_GAP`). `sd_cs` is now
  asserted for exactly one address at a time: on `sd_ready`, the word is
  captured and `sd_cs` drops; one cycle later (`XFER_GAP`), the address
  increments and `sd_cs` reasserts for the next word (or the transfer ends,
  if that was the last word). `sd_cache_inhibit` was re-derived from a new
  `busy` semantics that covers the whole transfer -- including the
  inter-word gap cycles where `sd_cs` is momentarily low -- instead of being
  tied directly to `sd_cs`, per the task's explicit requirement that it not
  pulse per word. `sd_state`/`sd_uds_n`/`sd_lds_n` and the port list are
  unchanged. The file's header comment was rewritten to describe the new
  protocol and the defect it replaces (kept for posterity, since the old
  comment's warning is exactly what this addendum resolves).

- **`rtl/tb/ss_dma_tb.v`** -- the mock CPU port now models `cpu_ack` as a
  *level*, asserted after a per-word programmable latency and held until
  `sd_cs` drops (matching `cpu_cache_new.v:490`,
  `if (!cpu_cs) cpu_ack <= 1'b0`), and refuses to service a new address
  while its own ack is still up (matching the cache's state machine sitting
  in `CPU_SM_WAIT`/`CPU_SM_FILLW` until `cpu_cs` drops). This replaces the
  old one-cycle-pulse mock, which could never distinguish a compliant
  per-word DUT from a burst-style one holding `sd_cs` high, since it simply
  re-armed a fresh pulse every cycle `sd_cs` stayed asserted regardless of
  whether a real bus cycle had actually started.

  Added a duplicate-address watchdog: every `word_valid` pulse's underlying
  `committed_addr` (the address the mock most recently latched data for,
  frozen while its ack stays up) is checked against every prior one this
  transfer and flagged as a `FAIL duplicate address serviced in one
  transfer` if it repeats. Also updated the `cs_drop_count` checks (now
  expected once *per word*, not once per transfer, since `sd_cs` legitimately
  drops between every pair of words) and added a direct regression test that
  samples `sd_cache_inhibit` during the inter-word gap itself
  (`@(negedge sd_cs)`) to confirm it does not pulse low there. Latency 0 is
  now valid for every word (not just the first), since the mandatory
  inter-word gap removes the same-edge race that made 0-then-0 latency
  physically impossible under the old held-`sd_cs` protocol -- documented in
  both files' headers.

- **`upstream/core-cd32-cdtv/rtl/ss_dma.v`** -- refreshed on disk to match
  (`diff` confirmed empty). This could **not** actually go into the same
  git commit, and I want to correct the record rather than repeat the
  earlier report's claim: `upstream/core-cd32-cdtv/` is not a plain
  directory in this repo, it is a *separate git repository* (it has its own
  `upstream/core-cd32-cdtv/.git`) with no `.gitmodules` entry registering it
  as a submodule. `.gitignore:7`'s `/upstream/*/` rule is a symptom of the
  same fact, not the cause. `git add -f` on a path inside a nested `.git`
  is a silent no-op in the outer repo -- it reports success but stages
  nothing, which is exactly what happened here and, per `git log --all`,
  also explains why the original report's claim that this file was part of
  commit `5c95c28` does not hold up: `git ls-files` shows it was never
  tracked by this repo at any point in its history. The file on disk is
  correct and byte-identical to `rtl/ss_dma.v`; it just isn't, and can't be,
  version-controlled from here without first turning that directory into a
  real submodule or dropping the nested `.git`.

### Proof the new bench actually catches the old defect

Reverted `rtl/ss_dma.v` to the pre-fix burst-style implementation
(`git show 5c95c28:rtl/ss_dma.v`) and ran it against the new
`rtl/tb/ss_dma_tb.v`, unmodified:

```
$ export PATH="$PATH:/c/iverilog/bin"
$ iverilog -g2005 -Wall -o build/ss_dma_tb_old /path/to/ss_dma_old.v rtl/tb/ss_dma_tb.v && vvp build/ss_dma_tb_old
FAIL duplicate address serviced in one transfer: 0x000010
FAIL duplicate address serviced in one transfer: 0x000010
FAIL duplicate address serviced in one transfer: 0x000010
FAIL duplicate address serviced in one transfer: 0x000010
FAIL duplicate address serviced in one transfer: 0x000010
FAIL duplicate address serviced in one transfer: 0x000010
PASS word count
PASS word contents
FAIL word contents: got 49168, want 49169
FAIL word contents: got 49168, want 49170
FAIL word contents: got 49168, want 49171
PASS cs released after done
FAIL cs drops once per word: got 1, want 4
FAIL duplicate address serviced in one transfer: 0x000020
...
FAIL urnaround cs drops once per word: got 1, want 5
...
FAIL s asserted during inter-word gap: got 0, want 1
38 FAILURE(S)
FATAL: rtl/tb/ss_dma_tb.v:310:
       Time: 550  Scope: ss_dma_tb
```

This confirms the old implementation fails hard and immediately -- the
duplicate-address watchdog fires on the very first transfer's second word,
word contents freeze at word 0's value for the rest of every transfer, the
`cs_drop_count` cadence check shows only one drop per transfer instead of
one per word, and the new `sd_cache_inhibit`-during-gap regression test
fails because a burst-style DUT never produces a gap to sample. The old bug
is caught loudly and in multiple independent ways, not just via a
word-count mismatch.

### GREEN -- fixed implementation against the new bench

```
$ export PATH="$PATH:/c/iverilog/bin"
$ iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v && vvp build/ss_dma_tb
PASS word count
PASS word contents
PASS word contents
PASS word contents
PASS word contents
PASS cs released after done
PASS cs drops once per word
PASS second transfer count
PASS second transfer first word
PASS  transfer cs drops once per word
PASS min-turnaround transfer count
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS min-turnaround word contents
PASS urnaround cs drops once per word
PASS mixed-latency transfer count
PASS mixed-latency word contents
PASS mixed-latency word contents
PASS mixed-latency word contents
PASS mixed-latency word contents
PASS d-latency cs drops once per word
PASS zero-length word count
PASS zero-length never asserts cs
PASS zero-length cs stays low
PASS ate is read-data during transfer
PASS sd_uds_n asserted (active low)
PASS sd_lds_n asserted (active low)
PASS inhibit asserted during transfer
PASS s asserted during inter-word gap
PASS ually low during the sampled gap
ALL TESTS PASSED
```

No `-Wall` warnings. (Several PASS labels above are visibly truncated in
the console output -- `check`'s `name` argument is a 256-bit/32-character
`reg`, and a few of the newer, more descriptive names exceed that; this is
a pre-existing cosmetic limit inherited from the original bench, not a
correctness issue, since `$display` still prints PASS/FAIL and the actual
numeric comparison is unaffected. Left as-is rather than widening the
parameter for a cosmetic-only fix out of this task's scope.)

### Status update

This task's status is now **DONE** for the concern raised in the original
report. The interface (port list, `sd_state`/`sd_uds_n`/`sd_lds_n`
semantics) is unchanged, so nothing downstream that assumed those specific
signals needs to change; only the internal `sd_cs` sequencing (now per-word)
and `sd_cache_inhibit`'s derivation (now from transfer-in-progress rather
than `sd_cs` directly) changed.
