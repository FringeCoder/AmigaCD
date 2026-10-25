# A 68k on the ARM: plan and measurements

The idea: a CPU option where the 68k runs on the DE10-Nano's ARM (Cortex-A9),
the way a PiStorm's does, with a small bus proxy in the FPGA doing the chipset
accesses for it. Kickstart and Fast RAM would live on the ARM side and run at
ARM speed; every chip RAM and custom register access would cross to the FPGA.

The MiSTer framework gives a core no AXI bridge to the ARM. The two paths are
`hps_io` (microseconds per word) and DDR3, which both sides reach; the A2065
already talks to Main through a DDR3 mailbox (`a2065_ddr3_mailbox.v` in the
core). So the cost of one chipset access is the DDR3 round trip, and that is
what step 1 measures. (Step 1d then wired the lightweight bridge up after
all; see below.)

## Steps, each a go / no-go gate

1. **Measure the round trip.** `latprobe_ddr3.v` in the core echoes a request
   word through DDR3; `tools/latprobe` here times it. Measurement builds only,
   on the `claude/resync-upstream-xdi173` branches, never merged.
2. FPGA bus proxy as a fourth CPU type.
3. ARM-side emulator (Musashi), Kickstart and Fast RAM local.
4. Only if 1-3 are worth it: a cached chip RAM copy with DMA write
   notification, and a JIT.

## Step 1: first results, 2026-10-25

Probe core: AmigaCD_MiSTer fit run 40169053470, seed 20 (setup -0.071 ns at
the slow 100 C corner). latprobe: AmigaCD build run 40169047321. 100000 round
trips each, pinned to ARM core 0 (Main pins itself to core 1), no Fast RAM.

| run | median | min | p90 | p99 | p99.9 | max | FPGA DDR3 read | empty polls (median / p99) |
|---|---|---|---|---|---|---|---|---|
| quiet: A500, 68000, a floppy demo running | 2.51 us | 1.47 | 2.73 | 22.3 | 34.9 | 0.46 ms | 141 ns (p99 379) | 10 / 75 |
| busy: CD32, 68EC020, Flink streaming from CHD, Main at 30-40 % | 2.51-2.52 us | 1.47-1.48 | 2.72-2.75 | 23.1-24.8 | 36.5-39.7 | 0.97-5.4 ms | 141 ns (p99 335-405) | 10 / 76-78 |

(Busy row: three runs ~8 s apart; ranges across them.)

What they say:

- **One chipset access costs about 2.5 us**: ~400 000 a second. A stock A500
  68000 can make up to ~1.8 million chip bus cycles a second, an A1200 more.
  Code fetched from chip RAM through this path would run several times slower
  than a real A500. **CD32 and A500 games, which run from chip RAM, are out.**
- **The median does not care about Main's load**; the tail does not either,
  up to p99.9. 1 % of accesses take over 22 us in both runs, which points at
  Linux on core 0 (timer and interrupt work), not at Main. The maximum grows
  with Main busy (to 5.4 ms): a stall a timing-sensitive program would show.
- **Most of the round trip is not the FPGA.** Its DDR3 read takes 141 ns and a
  full poll loop ~0.2 us, yet it makes a median of 10 empty polls (~2 us)
  between answering one request and seeing the next. That time is the ARM
  reading the answer and writing the next request through a strongly ordered
  `/dev/mem` mapping with a barrier on each request. An inference from the poll
  count: step 1b measures the ARM side directly.

Verdict so far: the "few microseconds" case. An ARM CPU can only be a Fast-RAM
accelerator (Workbench, RTG, compiled software), with writes posted to the FPGA
without waiting and only reads paying a round trip. Not a CD32 option.

## Step 1b: the ARM half on its own, 2026-10-25

latprobe 161b8283 (build run 40178540711). `-r` on the normal core, the echo
runs on the seed-20 probe core (A500, no Fast RAM, as in step 1).

| `-r` (ns per op, median of 100 batches of 1000) | read | write | write + dmb | write, read back |
|---|---|---|---|---|
| O_SYNC (strongly ordered) | 141.0 | 8.6 | 33.4 | 211.3 |
| no O_SYNC | 9.8 | 8.6 | 12.3 | 14.8 |

| echo | median | min | p99 | empty polls (median) |
|---|---|---|---|---|
| baseline | 2.53 us | 1.47 | 23.4 | 10 |
| `-B` (no barriers) | 2.48 us | 1.52 | 21.0 | 10 |
| `-S`, `-S -B` | ms to tens of ms, most timed out | | | 65535 |

- The ARM's own accesses are cheap: a 141 ns uncached read, an 8.6 ns posted
  write, 33 ns with a barrier. Barriers cost under 2 % of the round trip.
- Without O_SYNC the mapping is cacheable (9.8 ns reads): a request sits in
  the ARM's cache and the round trip never completes. Unusable for sharing.
  The echo loop now refuses `-S` and gives up after 5 consecutive timeouts
  (the user's 100000-round-trip `-S -B` run spun for 4 minutes before this).
- That leaves ~2 us unexplained by anything the ARM executes. Adding up the
  rest: the ARM sees the answer within one 141 ns read and posts the next
  request in ~10-35 ns; the FPGA reads in 141 ns and polls every ~0.2 us. The
  ~2 us sit between "the FPGA's response write was accepted" and "the FPGA's
  poll sees the ARM's next request" -- while each write is in flight between
  two different ports of the HPS SDRAM controller (the ARM's, and the FPGA's
  f2sdram). **Hypothesis:** a write is accepted long before the other port can
  see it; the controller queues writes per port, and nothing makes a read on
  one port wait for a write still queued on another. A read on the same port
  does see its own write (the 211 ns read-back), which fits.

## Step 1c: does a read-back push the write out?

`latprobe -F` reads the request word back right after writing it. If the
round trip drops, the ARM-to-FPGA direction was the slow one and a read-back
is a cheap fix on that side (the FPGA-to-ARM direction would want the same
trick in the core). If it does not, the delay is in the controller and no
amount of software helps.

**Result, 2026-10-25:** no effect. latprobe 2ee19d40 (build run 40179229995)
on the seed-20 probe core, A500, no Fast RAM, runs alternated:

| echo | median | min | p90 | p99 | empty polls (median / p99) |
|---|---|---|---|---|---|
| baseline, run 1 | 2.55 us | 1.48 | 2.69 | 23.4 | 10 / 76 |
| `-F`, run 1 | 2.55 us | 1.49 | 2.69 | 23.3 | 10 / 76 |
| baseline, run 2 | 2.55 us | 1.48 | 2.67 | 20.4 | 10 / 75 |
| `-F`, run 2 | 2.55 us | 1.49 | 2.67 | 21.5 | 10 / 76 |

The read-back adds a 141-211 ns ARM access to every round trip and the median
does not move at all, so the ARM is not on the critical path: it is waiting
for the echo either way. A same-port read does not hurry the ARM's write
towards the FPGA's port, and the FPGA still makes 10 empty polls per request.
Nothing on the ARM side shortens the DDR3 round trip; it stays at ~2.5 us.

## Step 1d: a bridge that does not go through DDR3

The Cyclone V's lightweight HPS-to-FPGA bridge maps FPGA registers straight
into the ARM's address space at physical 0xFF200000: no DDR3, no cross-port
visibility. Main already releases it from reset and makes it visible
(`fpga_io.cpp` `do_bridge()`); the framework just never instantiated it.
AmigaCD_MiSTer `claude/lwbridge-echo` (81f9909) does, in `sys/sys_top.v`, with
`sys/lw_echo.sv` as the only slave: ECHO (rw), MAGIC "LWBR", CYCLES (100 MHz)
and COUNT. `rtl/sim/lw_echo/tb_lw_echo.sv` checks it as an AXI3 slave (IDs,
bursts, strobes, stalls; 10568 checks). `latprobe -L` (167d8369) times it and
refuses to run unless the loaded core's path contains "lwbridge", because an
access with nothing behind the bridge can hang the ARM.

**Result, 2026-10-25:** fit seed 18, setup +0.038. Installed as
`_Test/lwbridge/amigacd.rbf`, A500 defaults, 30000-100000 accesses a run:

| per access, ns | cpu 0 (3 runs) | cpu 1 (3 runs) |
|---|---|---|
| read | 321-324 | 379-492 |
| read, timed by the FPGA's clock | 270-284 | 274-782 |
| write | 356-359 | 437-541 |
| write + dmb | 356-373 | 514-540 |
| write, read back | 655-663 | 971-1185 |

Every one of 260000 echoes came back intact. Main shares cpu 1 (23% CPU in
`top`), which is why its numbers wander; cpu 0 is the honest figure, and it
repeats to within 2%. Timed one at a time, a write-and-read-back takes 1630 ns
median on the ARM's clock, of which 1040 ns is `clock_gettime` itself.

What it means:

- A read costs ~320 ns on the ARM, ~275 ns of it inside the bridge. That is
  inside a real 68000's 280-560 ns bus cycle, where the DDR3 mailbox costs
  ~2.5 us. A read that the proxy holds (rvalid late) until the chipset cycle
  completes would be one bridge access per 68k access, not a mailbox round
  trip. The earlier guess of ~0.5 us was pessimistic.
- Writes are not posted: 356 ns, the ARM waits for the bridge's response.
  `/dev/mem` with O_SYNC maps the bridge strongly ordered. A kernel driver or
  UIO mapping as Device memory would let writes post, and the 12 ns DDR3
  write in step 1b suggests what that buys. Not measured.
- So for step 2 the bridge replaces the DDR3 mailbox as the request path. It
  costs a `sys/` change, which the core carries anyway.

The original proposal, kept for the record: even near 0.5 us a read would
still be slower than a 68000 bus cycle, so it would only make an
accelerator's register reads cheaper.
