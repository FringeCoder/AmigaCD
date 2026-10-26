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

## Requirement (Mark, 2026-10-25)

The ARM CPU is **one more CPU choice**, beside the 68000 (fx68k), 68010,
68020 and 68030 (TG68K). The existing choices stay exactly as they are,
cycle-exact where they are today; nothing of this plan may change how they
run. Only the new choice uses the ARM core for the CPU and the ARM's memory
(Kickstart and Fast RAM on the ARM side); chip RAM and the chipset stay in
the FPGA.

How the build so far meets that:

- The proxy (step 2) sits behind a mux on the 68000 path's pins and only
  drives them while armcpu_bridge's OWN is set; with OWN clear the fx68k's
  signals pass through unchanged, and the TG68K path has no change at all.
- For the measurements OWN can only be set with the 68000 selected. The
  finished option replaces that with its own CPU setting (a new value
  beside cpucfg's four, carried in the OSD config and the .mgl), which
  selects the proxy instead of the fx68k from reset.
- The mux is one more level of logic on the fx68k's outputs, so every fit
  has to keep closing timing; behaviour with OWN clear is identical by
  construction, and the existing CPU benches run unchanged.

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

## Step 1e: DDR3 write visibility, each direction on its own

The question behind keeping chip RAM in DDR3: when one side writes a word,
how long before the other side's read returns it? `visprobe_ddr3.v`
(AmigaCD_MiSTer `claude/lwbridge-echo` 2053af4) sits on the A2065's DDR3
port beside the step 1 probe and is started and stopped over the
lightweight bridge, which for this build runs on the core's DDRAM_CLK
(113.5 MHz) and passes registers 4-7 into emu. `latprobe -V` (eb97be70)
drives it:

- **A, ARM to FPGA:** the ARM writes word A, then A_GO over the bridge; the
  FPGA reads A back to back from the cycle A_GO arrives and counts reads
  that return the old value.
- **B, FPGA to ARM:** B_GO makes the FPGA write word B; the ARM polls B, then
  writes B_SEEN, and the FPGA times from its accepted write to B_SEEN. The
  same trial without polling gives the bridge's fixed cost. Two controls
  read a different word instead of B, and read B a second time.

**Result, 2026-10-25:** fit seed 14, setup +0.093, every family positive.
Cpu 0, A500 defaults, five runs of 5,000-10,000 trials (one on cpu 1); the
medians repeated to the nanosecond.

| trial | median | p99 | stale reads |
|---|---|---|---|
| A, word written 50 us before A_GO | 203-220 ns | 652-740 ns | none |
| A, word written just before | 211-220 ns | 608-678 ns | none |
| A, written and read back by the ARM first | 185-203 ns | 564-652 ns | none |
| B, B_SEEN without polling | 1551-1568 ns | | -- |
| B, polled until visible | 2740-2758 ns | | none |
| B, one read of another word instead | 1683 ns | | -- |
| B, polled, then word B again | 2872 ns | | none |

`-L` on this build: read 322 ns, write 337 ns, write and read back 610 ns.

What it means:

- **No stale data, in either direction, in any trial.** The controller is
  coherent across its ports as far as either side can observe.
- **ARM to FPGA is immediate.** The FPGA's first read after A_GO already
  returns the new word, and ~210 ns is that read itself. The ARM's write has
  landed before its bridge write arrives (~340 ns), so the visibility delay
  is under that, and invisible from the fabric.
- **FPGA to ARM stalls the ARM's read instead.** Reading another word in its
  place costs 132 ns, and so does reading B a second time; only the first
  ARM read of a word the FPGA just wrote takes ~1.19 us, ~1.06 us more. The
  read waits for the FPGA's write to land rather than returning old data.
- **That is the step 1 round trip.** Its 2.5 us was the ARM's read of the
  FPGA's answer stalling (plus the FPGA's polling), not the ARM's request
  being late; step 1c's read-back could not help because it was on the
  wrong side.
- **For chip RAM in DDR3:** CPU writes would reach chipset DMA at once, but
  every CPU read of a word DMA just wrote (a blit result, a disk buffer)
  would cost ~1.2 us, against a 68000's 280-560 ns. BigMig's answer below,
  chip RAM in SDRAM behind the snooped CPU cache, avoids the question.

## Step 2: the FPGA bus proxy, driven from the ARM

The FPGA half of the ARM CPU. AmigaCD_MiSTer `claude/lwbridge-echo`
571d1f2:

- `sys/lw_echo.sv` forwards lightweight-bridge offsets 0x080000-0x1FFFFF to
  the core, one request per beat, and answers only when the core does.
- `rtl/armcpu_bridge.v`: control registers at 0x080000 (MAGIC "ACPU", OWN,
  BANK, STATUS with Paula's interrupt level, CYCLES, LAST, WORDS) and a 1 MB
  window at 0x100000 onto the 68k's address space, in the 68k's byte order
  (the ARM byte-swaps halfwords and words). An access is one or two 68000
  word cycles; a byte read keeps to its own data strobe; writes are posted.
- `rtl/proxy68k.v` plays each word cycle on the fx68k's pins with fx68k
  busControl's phase timing. cpu_wrapper muxes it in on the 68000 path only
  while OWN is set and holds the fx68k in reset meanwhile, so gary, the chip
  bus, autoconfig and the CDTV bridge all see a 68000; with OWN clear the
  fx68k's pins pass straight through. Nothing can hang the ARM: no DTACK for
  ~9 ms ends a cycle, and a reset mid-cycle still answers.
- `rtl/sim/armcpu/tb_armcpu.sv`: AXI to a 68000 bus slave that checks every
  pin edge against fx68k's rules; 5832 checks, 17 mutations caught.
- `latprobe -P` (AmigaCD 3326acf7) takes the bus, checks chip RAM in every
  size, reads VHPOSR live, times each access kind and gives the bus back.

The bridge, lw_echo and armcpu_bridge run on clk_sys (28.4 MHz). On clk_114
the new logic joined the core's near-critical paths there and no seed of 19
closed; on clk_sys it is out of the failing paths entirely (lw_echo only
shows in the hold report, with slack to spare).

**Result, 2026-10-25:** seed 9 of 20, setup -0.158 ns (no seed closes; see
below). CPU set to 68000, cpu 0, two runs of 20000:

| per access, chip RAM | ARM clock | by the bridge (request to last cycle) |
|---|---|---|
| read byte | 1709-1761 ns | 881 ns |
| read halfword | 1696-1781 ns | 881-916 ns |
| read word (two 68k cycles) | 2719-2778 ns | 1727 ns |
| write byte | 1043-1207 ns | 811 ns |
| write halfword | 1062-1161 ns | 811-1093 ns |
| write word | 2019-2306 ns | 1656 ns |

0 of 20000 random words read back wrong in either run, every size and the
byte order were right, VHPOSR moved between reads, and after the bus was
given back the A500 came up at the Kickstart 1.3 insert-disk screen.

What it means:

- **It works:** the ARM can be the CPU of the existing machine, through the
  existing 68000 path, without touching the other CPUs.
- **It is slow:** a chip-RAM halfword costs ~1.7 us from the ARM, three times
  a real 68000's 564 ns cycle and BigMig's 607 ns. Two parts:
  - ~0.88 us in the FPGA. A 68000-timed cycle is 564 ns at the least, and
    the proxy waits for the next enPhi1 and for the CPU's chip-bus slot.
  - ~0.85 us in the bridge. Step 1d read in ~320 ns with the bridge on a
    100-114 MHz clock; on clk_sys every AXI handshake costs a 35 ns cycle.
- **Timing does not close.** The best of 20 seeds is -0.158 ns. The new
  logic is no longer in the failing paths; what fails is the usual clk_114
  population (the TG68K read gate, ascal, the DDR3 controller, SDRAM). This
  build still carries step 1's and 1e's probes, and the requirement above
  says the existing CPUs must keep closing, so this comes first.

Levers before step 3:

1. Timing: drop the step 1 and 1e probes (latprobe_ddr3, visprobe_ddr3)
   from the build, then fit again.
2. Bridge latency: the bridge back on a fast clock, with its paths
   pipelined, or the full h2f AXI bridge as BigMig uses.
3. Bus cycle: for the ARM CPU, chip RAM through the SDRAM CPU port and its
   snooped cache instead of a 68000-timed chip-bus cycle (BigMig's route),
   keeping the 68000 timing as a "compat" setting.

## Step 2b: timing closed, a faster bridge, and FAST reads

The three levers above, done (AmigaCD_MiSTer `claude/lwbridge-echo`):

1. **Probes out** (7b5aae6): latprobe_ddr3 and visprobe_ddr3 are no longer
   built; a2065.v and minimig.v are back to e89ad97. Their sources and
   benches stay.
2. **Bridge on clk_100m** (7b5aae6): lw_echo and armcpu_bridge run on the
   HPS's own clock, its own group in the SDC, so none of their logic is on
   clk_114. The crossing to the proxy on clk_sys is asynchronous: two flops
   on px_tgl, px_ack_tgl, px_own, the CPU setting and the interrupt level,
   with every field held from its toggle to its answer.
3. **FAST reads** (9d4a752): armcpu_bridge CTRL bit 1. While the proxy owns
   the bus with FAST, cpu_wrapper sets turbochip_d / turbokick_d, so reads of
   chip RAM, Kickstart and fast RAM go through the SDRAM CPU port and its
   cache -- the 68020's turbo chip/kick route, whose cache snoops every
   chipset write into both halves. A second memory_router decodes the held
   request so the proxy knows before it starts; such a read skips the 68000
   phases and ends on the port's ready. Writes, and every access to the
   custom chips, CIAs and autoconfig, keep the 68000 timing. FAST is off by
   default.

tb_armcpu runs its random pass with FAST off and on and the bridge on an
unrelated 100 MHz clock: 9648 checks, five FAST mutations caught.

**Timing:** seed 2 of 20 closes, setup +0.034 ns (run 40218489738). That is
the core's normal rate, not a cost of this work. The same CI fit on commits
without it:

| commit | what | seeds closing | best |
|---|---|---|---|
| 79669d2 | before any ARM-CPU work | 2 of 20 | +0.035 |
| b570234 | step 1 probe only | 0 of 20 | -0.071 |
| 7b5aae6 | levers 1 and 2 | 0 of 20 | -0.012 |
| 9d4a752 | levers 1, 2 and 3 | 1 of 20 | +0.034 |

What fails, on every build with or without the ARM CPU, is the usual
clk_114 population, led by Q.rdgate_to_cpu (the TG68K read gate); the
cpu_wrapper families keep 0.55-1.7 ns of slack.

**Result, 2026-10-25:** CPU set to 68000, cpu 0, two runs of 20000; the
bridge clock now measures 100.0 MHz.

| per access, chip RAM | 68000 timing | FAST | step 2 (clk_sys bridge) |
|---|---|---|---|
| read byte | 1330-1355 ns | 651-652 ns | 1709-1761 ns |
| read halfword | 1353-1398 ns | **652 ns** | 1696-1781 ns |
| read word | 2372-2438 ns | 1060-1129 ns | 2719-2778 ns |
| write byte | 978-1069 ns | 1065-1095 ns | 1043-1207 ns |
| write halfword | 983-1039 ns | 1020-1078 ns | 1062-1161 ns |
| write word | 1998-2002 ns | 2061-2074 ns | 2019-2306 ns |

By the bridge's own clock a FAST halfword read is 260-290 ns in the FPGA,
against 720-820 ns for a 68000-timed one. 0 of 20000 random words wrong in
each run; with FAST, 0 of 512 reads stale after the ARM's own writes and 0
of 256 after a blitter clear the ARM started itself -- the snooped cache
keeps up with both. After the bus was given back the A500 came up at the
Kickstart 1.3 screen.

What it means:

- **Reads are now at BigMig's figure:** 652 ns a halfword from the ARM
  against its quoted 607 ns, and faster than a real 68000's 564 ns cycle
  once the ARM's own instruction time is counted in.
- **Writes are the slow side now:** ~1.0 us each, because every write is a
  68000-timed chip-bus cycle (posted, but the next one waits for it). The
  next lever is writes to chip RAM through the SDRAM port as well, which the
  68020 path never does today, so it needs its own coherence argument
  (chipset DMA reads the SDRAM directly; the CPU cache is write-through).
- **The existing CPUs are untouched:** with OWN clear the fx68k's pins pass
  through, FAST changes nothing, and the TG68K path has no change.

## Step 2c: FASTW, chip RAM writes through the SDRAM port

AmigaCD_MiSTer `claude/lwbridge-echo` 6441bb5. armcpu_bridge CTRL bit 2
(FASTW): the proxy's writes to chip RAM, fast RAM and RTG go to the SDRAM
CPU port instead of a 68000-timed chip-bus cycle -- AS, R/W, the strobes and
the data on the next edge, ending when the port's write buffer takes the
word. cpu_wrapper's cchip covers writes while the proxy owns the bus with
FASTW (the 68020's turbo chip only ever sends reads there). Writes to the
custom chips and CIAs keep the 68000 timing; FASTW is off by default.

Coherence, from sdram_ctrl: the write updates the CPU cache (write-through)
and waits in a one-deep buffer for the next free slot; a chipset read of the
same 8-byte block meanwhile has the buffered data forwarded into it
(fwd_en). A chipset write to the same word inside that window would be
overtaken, but starting any DMA takes the ARM chip-bus register writes of
~1 us each, far longer than the buffer holds a word.

tb_armcpu: a third random pass with FASTW, 13360 checks, four FASTW
mutations caught.

**Result, 2026-10-25:** fit seed 5 of 20, +0.046 ns at the slow 100 C corner
the project times at (multi-corner analysis is off; the families script
sees the HDMI scaler at -0.258 at another corner, not this logic). Two runs
of 20000, cpu 0:

| per access, chip RAM | 68000 timing | FAST | FAST + FASTW |
|---|---|---|---|
| read halfword | 1336-1348 ns | 655-656 ns | 655-659 ns |
| read word | 2346-2383 ns | 1050-1059 ns | 1048-1097 ns |
| write halfword | 1041-1054 ns | 1035-1097 ns | **344 ns** |
| write word | 2005-2015 ns | 2042-2105 ns | **604-607 ns** |

By the bridge's clock a FASTW halfword write is 260-280 ns. Checks, both
runs: 0 of 20000 random words wrong; FAST reads 0 of 512 stale after the
ARM's writes and 0 of 256 after a blitter clear; the blitter copying 256
words just written with FASTW (the last one written right before BLTSIZE)
0 wrong; FASTW writes over the blitter's result 0 wrong. The A500 came back
at the Kickstart 1.3 screen.

Where this leaves the bus: a chip-RAM halfword costs ~655 ns to read and
~344 ns to write from the ARM, a word about twice that; registers in the
custom chips and CIAs ~1.0-1.35 us. Those are the costs step 3's emulator
pays for every chip access, with Kickstart and fast RAM on the ARM side.

## Step 3: a 68000 on the ARM boots Kickstart

**armcpu** (AmigaCD `tools/armcpu`, 367d606e): Musashi v4.60 (MIT, vendored
unmodified with its generated opcode table) runs a 68000 on ARM core 0.
Kickstart is loaded into ARM memory; every other access goes through the
proxy with FAST and FASTW. The emulator follows the ROM overlay itself
(CIA-A PRA bit 0 through DDRA, as Gary does), hands Paula's interrupt level
to Musashi every 500 cycles, and executes RESET through a new CTRL bit 3
that pulls the CPU's reset output (AmigaCD_MiSTer f7b4328, fit seed 9,
+0.127 ns). It cold-starts by clearing the ExecBase pointer at $4.

**Result, 2026-10-25:** with the A500 config (Kickstart
`1.3-scsi-patched.rom`) and both floppy drives empty, Kickstart 1.3 boots on
the ARM's 68000 to the insert-disk screen, with the fx68k held in reset
throughout. Effective speed, from Musashi's cycle count against the clock:
15-19 MHz of 68000 while it runs from the ROM copy in ARM memory, ~6.8 MHz
in the insert-disk loop, which is chip-bound (~400,000 bus reads and
~257,000 writes a second).

What the first runs found:

- **Interrupts are level-triggered; Musashi's are not.** Without an
  acknowledge callback Musashi drops its pending level when it takes the
  interrupt, so a level handed over only on change was taken once. A disk
  loader waiting for DSKBLK with level 6 up hung. Handing the level over
  every slice fixes it (host test: a level that stays up is taken again).
- **Stale RAM looks like a warm start.** Taking over a machine the fx68k
  had booted, Kickstart trusted the old ExecBase and its capture vectors.
- **The disk then in DF0 does not boot on the fx68k either** on this core
  and binary (a white screen), so it is no test of the ARM CPU; the boot was
  checked with the drives empty, and from floppy with Juggler (below).
- **Single-stepping hides timing faults:** the debug trace now uses
  Musashi's instruction hook so a traced run keeps normal timing.

**Floppy, 2026-10-26:** Workbench 1.3 boots from floppy on the ARM's
68000: `Juggler.adf` (games/Amiga/shared/26042026/DEMOS) in DF0, the screen
white at 3 s as the boot starts from the ARM's own RESET, and the Workbench
screen with the disk's "Movie" icon by ~10 s, the same as the fx68k on the
same disk. The armcpu log shows the stages: Kickstart clearing memory in the
first second, disk loads in bursts between idle-loop waits, Workbench
loading, then idle.

**Fast RAM in ARM memory, 2026-10-26** (AmigaCD 69bc8dc9): armcpu presents
an emulated Zorro II memory board, 8 MB by default, first in the autoconfig
chain; Kickstart configures it at $200000 and it joins exec's memory list,
its memory never touching the bus. Two fixes came with it:

- **Chip RAM size.** FAST and FASTW reach chip RAM through the SDRAM CPU
  port, which answers all of $000000-$1FFFFF, so Kickstart sized chip RAM at
  2 MB where the chip bus has 512 KB on the A500 profile (screens could land
  where Agnus cannot fetch). armcpu measures the size over the chip bus at
  start, with the overlay off as Kickstart has it, and wraps chip addresses
  to it: Kickstart now finds 512 KB, as on the fx68k.
- **A stale crash record.** A crashed run leaves 'HELP' at $0 and its alert
  at $100, which exec shows after the next reset; the cold start now clears
  them along with ExecBase.

Workbench 1.3 boots with the memory list chip 512 KB (pri -10), slow RAM
512 KB at $C00000 (pri 0) and the ARM board 8 MB at $200000 (pri 0). With
equal priority Kickstart 1.3 allocates from slow RAM first, as a real A500
with both does, so the board is barely used during the boot; a
CPU-bound benchmark that runs from it is the next measurement.

**Benchmark, 2026-10-26:** SysInfo 4.4 (the author's ADF, linked from
Aminet util/moni/SysInfo) boots on Kickstart 1.3 on the ARM's 68000. Its
SPEED test needs a click, which armcpu now scripts (`-M t:x:y`) by changing
what the CPU reads from JOY0DAT and CIA-A PRA. `-P` puts the ARM board
ahead of slow RAM in exec's list, as Workbench 1.3's FastMemFirst does.

| SysInfo SPEED | program in slow RAM (over the bus) | program in ARM fast RAM (`-P`) |
|---|---|---|
| Dhrystones | 388 | 3665 |
| MIPS | 0.40 | 3.82 |
| vs A600 (68000, 7 MHz) | 0.73x | 6.92x |
| vs A1200 (68EC020, 14 MHz) | 0.31x | 3.01x |
| vs A3000 (68030, 25 MHz) | 0.08x | 0.72x |
| chip speed vs A600 | 1.16x | 1.20x |

Code that lives in ARM memory runs at about seven times an A600 under
Musashi; code in chip or slow RAM pays the bus for every fetch and runs
below a real 68000. That is the case for keeping programs in fast RAM, and
for a JIT later rather than more bus speed.

Where it stands against the goal: the ARM is the CPU of an otherwise
unchanged machine, the other CPU choices are untouched, and Kickstart and
ROM code run at 2-3 times a 7 MHz 68000. Chip-bound code runs at about a
68000's speed, as the bus costs predict. Not yet: fast RAM on the ARM side
(an emulated autoconfig board), the CPU choice in the OSD, and the ARM core
to itself (maxcpus=1, as BigMig does) -- every run here shares Linux's
scheduler.

## Step 3c: the OSD option, the reset freeze, and Moira, 2026-10-26

**OSD option** (AmigaCD a2b3e02e, 3991a9d9): on a core with the proxy (an
`.rbf` under a path containing `lwbridge`, and `/media/fat/armcpu` present)
the Settings page's CPU row gets one more step, "ARM 68020", after the
machine's own CPUs; on the 68000 machines it is the only other step, so the
row is live there too. Main then starts armcpu as its own process
(`support/minimig/minimig_armcpu.cpp`: double fork, `-c 0 -R -P -m 6 -F 8
-C 68020`) and tells the core "68000". The flag is `minimig_config.armcpu`,
in the struct's tail padding, so the file size is unchanged. Save states are
refused while it is on.

**The reset freeze.** The first OSD Reset with the ARM CPU running froze the
whole board: no ping, no ssh, a power cycle. `ApplyConfiguration` halts the
CPU (`SPI_CPU_HLT`) while armcpu was still running. The halt stops the phase
enables, so the proxy's own no-DTACK timeout (counted in enPhi1) never
fires, armcpu's bridge read is never answered, and an unanswered
lightweight-bridge read stops the HPS. Two fixes:

- Main (b5d2066a): armcpu is stopped before every CPU halt
  (`ApplyConfiguration`, `BootInit`) and before the core's CPU setting
  changes; the existing `armcpu_restart()` calls start it again afterwards.
- Core (AmigaCD_MiSTer 338c026): a watchdog in `armcpu_bridge.v` on
  clk_100m. After 2^20 clocks (~10 ms) of a word cycle it answers a read
  with 0xFFFFFFFF, drops OWN and sets STATUS[9]. `tb_armcpu` stops the phase
  enables mid-cycle and checks the result; with the watchdog removed, the
  bench hangs as the board did. Fit seed 4, +0.015 ns slow 100C.

With both, two OSD Resets with the ARM CPU running passed, and armcpu was
restarted each time.

A false lead on the way: "Error validating disk / Key 880 checksum error"
on the SysInfo disk came from the VD demo disk in **DF1**. It appeared on
every CPU and on the live pair; with DF1 ejected, SysInfo boots. Eject DF1
before benchmarking.

**Moira** (AmigaCD 1c6bcf07): `cpu_moira.cpp` puts Moira (vAmiga's core,
MIT, vendored in `tools/armcpu/moira` with `MoiraConfig.h` set for speed)
behind the Musashi calls armcpu.c makes; `make armcpu-moira` builds it, and
`test_armcpu.py moira` runs the host tests on it. Moira executes the 68000,
68010, 68EC020 and 68020 only; its 68030/68040 exist for the disassembler.
SysInfo, `-P -R -C 68020`, both builds gcc 10.2 `-O3 -flto`:

| core | Dhrystones | MIPS | vs A600 |
|---|---|---|---|
| Musashi | 5685 | 5.99 | 10.74x |
| Moira | 3970 | 4.14 | 7.50x |

Moira runs Workbench and SysInfo correctly, but it is 30% slower here, and
on the host (a register loop from ROM, x86 `-O3`) it is slower still: 87 vs
120 M instructions/s as a 68000, 64 vs 126 as a 68020. Musashi stays the
default. The Musashi figure is up from 4783 with the gcc 13 build; the
toolchain alone moves it by about 19%. A 68040/68060 needs a JIT
(Amiberry's ARM32 UAE JIT, or Emu68), not another interpreter.

**Against the core's own 68030** (live pair, A1200 profile, KS 3.1, 8 MB
FastRAM, SysInfo's SPEED clicked through a uinput mouse,
`scripts/mister-vmouse.py`):

| CPU | Dhrystones | MIPS | vs A600 |
|---|---|---|---|
| FPGA 68030 50 MHz (SysInfo: 55.8) | 7857 | 8.28 | 14.85x |
| FPGA 68030 Fast (SysInfo: 60.6) | 7857 | 8.28 | 14.85x |
| ARM 68020, Musashi | 5685 | 5.99 | 10.74x |
| ARM 68020, Moira | 3970 | 4.14 | 7.50x |

The interpreter reaches about 72% of the core's 68030 on code in fast RAM,
and much less on anything that touches chip RAM or the chipset. It has no
MMU and no save states either. It does not replace the 68030; only a JIT
would. 50 MHz and Fast score the same, so the FPGA 68030 is bound by its
fast RAM there, not by its clock.

## Step 4: a JIT, Amiberry's, measured on the ARM, 2026-10-26

The interpreters top out at about a 25 MHz 68030 (step 3c). Amiberry's
32-bit ARM JIT is the candidate for more, and `tools/armuae` runs it on its
own: Amiberry v5.7.4's CPU core and JIT (fetched at 6d672cd, GPL-3, not
vendored), with the whole 24-bit space as host memory and no chipset. It is
a prototype to answer one question before the FPGA is touched: is the JIT
worth the bus window it needs?

**Why v5.7.4:** it is the last release that builds the 32-bit ARM JIT. 7.x
enables the JIT only for x86-64 and AArch64, and its 32-bit ARM JIT no
longer compiles (it refers to `regs` fields and 64-bit helpers the 32-bit
path does not have).

**What it took**, each found on the board:

- `regs.natmem_offset` must be set: 5.7.4's JIT reads `NATMEM_OFFSET` from
  `regs`, and with zero there the first compiled store faults at the 68k
  address itself.
- `pissoff` and `natmem_offset` must lie within 4 KB of `regs`
  (`patch_newcpu.py`): the JIT loads them as `[regs, #imm12]`. Out of range
  it reads the wrong words, and compiled code never pays out its cycles.
- No `-DARM_HAS_DIV`: the Cortex-A9 has no SDIV/UDIV, unlike the Pi's A7
  and A53, and the first 68k divide died on SIGILL.
- The JIT translates only while the 68k's instruction cache is on (CACR),
  as Kickstart sets it. Without it every block is interpreted while it is
  recorded, which is slower than the plain interpreter.
- Time is paid out of `pissoff`, as `events.cpp` does: compiled code calls
  `do_cycles(0)` and leaves the debt there.

**Measured** on the MiSTer's ARM, one core, no FPGA access, 68020, code in
ROM and data in fast RAM (`bench/bench.c`, a Dhrystone-shaped workload of
record copies, string work, calls, arrays and a divide):

| core | bench.c passes/s | vs Musashi | register loop passes/s |
|---|---|---|---|
| Musashi (armcpu) | 10,767 | 1x | |
| Amiberry 5.7.4 interpreter | 21,332 | 2.0x | 1,220,231 |
| Amiberry 5.7.4 JIT | 255,607 | 23.7x | 66,652,456 |

All three write the same checksum of the benchmark's state at pass 20000
(`3787132a`), so the JIT computes this workload correctly.

Scaled to SysInfo's 5685 Dhrystones for Musashi, 23.7x would be on the
order of 130,000, beyond an A4000/060. That holds only for code in ARM
memory. Chip RAM and the chipset stay at the bus's 0.35-0.65 us an access.

**What it means for the FPGA side:** the JIT fetches opcodes through host
pointers and has no fallback for code in a bank without host memory, so
code in chip RAM needs chip RAM as a host mapping: a flat 16 MB window
through the full h2f AXI bridge, mapped into natmem, instead of the 1 MB
banked lightweight-bridge window. The byte order already matches (the
window is in 68k order and UAE's natmem is big-endian). Misaligned 68020
accesses on device memory would go through the kernel's alignment fixup.

Not yet: a 68040 or 68060 model (5.7.4 has the interpreter tables; the JIT
runs the same 32-bit code), the FPU in the JIT, Kickstart on armuae, and the
armcpu bus behind it.

## Step 4b: the flat window, and the JIT on the real Amiga, 2026-10-26

**The window** (AmigaCD_MiSTer `claude/lwbridge-echo` 628d728): the full
HPS-to-FPGA bridge, 32 bits wide, maps the 68k's 24-bit space flat at ARM
physical 0xC0000000. A second `lw_echo` (FLAT, no registers) turns its
accesses into bus requests, and `sys/bus_merge.sv` joins them with the
lightweight bridge's for the core; bit 24 of the core's bus address marks a
flat access. `tb_armcpu` covers it (17310 checks). The fit missed timing on
8 seeds (best −0.281 ns, in the scaler and the TG68K read gate, not the new
logic); the lab core is `_Test/lwbridge-flat/amigacd.rbf` (32253156).

**armuae `-b`** (AmigaCD 058cabd7) runs Amiberry's core on that window:
- natmem *is* the window, with the 8 MB fast RAM board and the ROM copy
  mapped over it in ARM memory;
- chip and slow RAM are direct banks, the I/O areas bank functions;
- the overlay is emulated on the ARM, because the RESET line does not bring
  it back on this chipset;
- every access is a real 16- or 32-bit load or store.

Amiberry 5.7.4's own `do_put_mem_word` works a byte at a time, and the
custom chips decode the whole data bus on every cycle, so a word written as
two bytes reached INTENA as `$40FF` for `$C020`.

On the way, a false alarm: Python probes made every INTENA/DMACON set-write
set all bits. That was ctypes reading before it writes (a read at
`$DFF09A` is a `$FFFF` write on this chipset). Two benches,
`tb_proxy_custom` and `tb_proxy_minimig` (the proxy against the whole
`minimig.v`), proved the proxy correct; C probes are in
`tools/armuae/probe`.

**Result:** Kickstart 1.3 reaches the insert-disk screen on the interpreter.
Kickstart 3.1 (AmigaVision.rom) does on the JIT, running from the ARM fast
RAM. SysInfo 4.4 on the JIT (68020, 24-bit, caches on):

| CPU | Dhrystones | MIPS | vs A600 |
|---|---|---|---|
| core's 68030, 50 MHz or Fast | 7,857 | 8.28 | 14.85x |
| ARM, Musashi | 5,685 | 5.99 | 10.74x |
| **ARM, Amiberry JIT** | **180,644** | **188.56** | **341.48x** |

That is 9.9x SysInfo's A4000/040 reference, for code in fast RAM. Chip RAM
and the chipset still cost a bus cycle each (SysInfo's chip speed: 2.83x an
A600).

**Workbench from floppy:** with the Workbench 1.3 Juggler disk in DF0 (DF1
ejected), Kickstart 3.1 on the JIT boots it to the Workbench screen with the
disk's Movie icon in about 40 s. Opening Movie went to a black screen; the
run's time limit ended before it could be judged either way.

Not yet: the fit closing, armuae started from Main's OSD instead of armcpu,
a 68040/68060 model with 32-bit addressing (natmem is 16 MB, so the JIT's
direct accesses above it need guarding), the FPU in the JIT, and a
Workbench boot from disk.

## Prior art: BigMig (raparici/BigMig-Emu68A9, read 2026-10-25 at 3d5d4d1)

BigMig is this plan, built and shipping: the Minimig chipset in the fabric and
Emu68-A9 -- an ARMv7 port of Michal Schulz's Emu68 JIT -- running bare-metal on
the HPS's second ARM core. It quotes ~840 MIPS and chip RAM at **607 ns an
access (7.6 MB/s), chip-bus cycle included**. It confirms step 1d's direction
and adds what our measurements do not cover:

- **A bridge, not DDR3**, as step 1d found -- but the full h2f AXI bridge
  (0xC0000000, through `sysmem.sv`), not the lightweight one:
  `sys/h2f_axi3_to_lite.v` converts it to AXI-Lite and `sys/axi_seam_slave.v`
  with `seam_engine` turns each access into a word transaction on the chip
  bus (`rtl/minimig_m68k_hybrid_bridge.v`). Its 607 ns against step 1d's
  ~320 ns bridge read is the chip-bus cycle on top of the bridge.
- **Chip RAM reads through the FPGA's existing CPU cache.** The seam reaches
  chip RAM by `sdram_ctrl`'s CPU port, and `cpu_cache_new` snoops chipset
  writes (`snoop_act` from `chipWE`), so the cache stays coherent with DMA in
  the fabric. That answers step 4's hardest problem with no ARM-side copy of
  chip RAM. Its OSD has a live Chip Cache switch.
- **No Linux on the CPU core.** A per-core `.txt` sidecar beside the `.rbf`
  (MiSTer's official per-core boot arguments) boots Linux with `maxcpus=1`,
  so core #1 runs only the JIT. That removes the tail every run here shows
  (p99 ~20-23 us on cpu 0, and step 1d's wandering cpu-1 figures with Main on
  it). Loading the core costs one board restart.
- **Posted writes and fused longwords**: a request FIFO (`REQFIFO_LOG2`) and
  a native longword path (`LONGWORD_EN`) in the seam -- the write posting that
  step 1d found missing through `/dev/mem`, done in the fabric instead.
- A **"Compat" chip speed** that slows chip-RAM access to roughly a 68000's
  pace, for OCS/ECS software that times itself against the CPU.

What it does not do: CD32 / CDTV (Akiko's C2P only, no CD drive model), a
cycle-exact CPU, or the soft CPUs -- TG68K and fx68k are removed, which is what
frees its logic and timing. It ships its own Main and its own `sys/`.
Licences: the seam and core are GPLv3, as ours; Emu68-A9 is MPL 2.0 (source:
raparici/Emu68-A964), its firmware image as a whole GPLv3.

**What this means for this plan.** The engineering questions are answered:
the bridge (step 1d here, the h2f AXI seam there), coherency (cache in the
fabric, snooped), jitter (`maxcpus=1`, bare metal), and the CPU itself
(Emu68-A9 exists; writing our own JIT would duplicate it). Steps 2-4 as
written would rebuild BigMig. What is left is a product choice:

1. Leave the ARM CPU to BigMig, and keep AmigaCD the cycle-exact CD32/CDTV
   machine it is.
2. Offer AmigaCD's CD32/CDTV work (Akiko's CD path, the CDTV bridge) to
   BigMig, so the fast machine gains CD32 titles that tolerate a non-exact CPU.
3. An AmigaCD build variant with BigMig's seam and Emu68-A9 in place of the
   soft CPUs: the most work, a second core to maintain, and the CD32 library's
   timing-sensitive titles would still need the cycle-exact build.

## Proposed 2026-10-25, not adopted: leave the ARM CPU to BigMig

A session proposed option 1 -- AmigaCD stays the cycle-exact CD32/CDTV
machine and a fast ARM CPU is BigMig's job -- and recorded it here as a
decision. It was not the project's decision: Mark chose to continue with
step 2 (2026-10-25). The BigMig survey above stays as prior art: its seam,
its snooped chip-RAM cache and its `maxcpus=1` CPU core are the reference
for steps 2-4. The notes below, written for picking the work up again, are
the same reference.

### Where things are, and what to reuse

Where things are:

| what | where |
|---|---|
| this document, `tools/latprobe` (DDR3 echo `latprobe`, ARM-side `-r`, `-F`, the lightweight-bridge `-L`) | AmigaCD `master` |
| the DDR3 echo probe, `rtl/A2065/latprobe_ddr3.v`, its bench `rtl/A2065/sim/tb_latprobe.v`, wired into `a2065.v` behind the mailbox | AmigaCD_MiSTer commit `b570234` |
| the lightweight bridge in `sys/sys_top.v` and its register echo `sys/lw_echo.sv`, bench `rtl/sim/lw_echo/tb_lw_echo.sv` (on top of `b570234`) | AmigaCD_MiSTer branch **`claude/lwbridge-echo`**, commit `81f9909` -- the one branch that holds both; never merged, on purpose |
| the DDR3 visibility probe `rtl/A2065/visprobe_ddr3.v` (bench `rtl/A2065/sim/tb_visprobe.v`), the bridge moved to DDRAM_CLK with registers 4-7 passed into emu; `latprobe -V` | AmigaCD_MiSTer `claude/lwbridge-echo` commit `2053af4`; AmigaCD `eb97be70` |
| the 68000 bus proxy: `rtl/proxy68k.v`, `rtl/armcpu_bridge.v`, lw_echo's bus port, bench `rtl/sim/armcpu/tb_armcpu.sv`; bridge on clk_sys; `latprobe -P` | AmigaCD_MiSTer `claude/lwbridge-echo` commit `571d1f2`; AmigaCD `3326acf7` |
| probes out, bridge on clk_100m, FAST reads; `latprobe -P` with FAST and the blitter check | AmigaCD_MiSTer `claude/lwbridge-echo` commit `9d4a752` (fit run 40218489738, seed 2); AmigaCD `326a8224` |
| FASTW writes; `latprobe -P` with FASTW and its blitter checks | AmigaCD_MiSTer `claude/lwbridge-echo` commit `6441bb5` (fit run 40229429510, seed 5); AmigaCD `36a7d358` |
| the 68000 on the ARM: `tools/armcpu` (Musashi, the overlay, interrupts, RESET, the debug trace), `test_armcpu.py`; the RESET bit | AmigaCD `367d606e`; AmigaCD_MiSTer `claude/lwbridge-echo` commit `f7b4328` (fit run 40234323873, seed 9) |
| the measured numbers | the step 1, 1b, 1c, 1d, 1e, 2, 2b, 2c and 3 sections above |

What to start from, in order:

1. **Reuse, do not rebuild.** BigMig's seam (`sys/h2f_axi3_to_lite.v`,
   `sys/axi_seam_slave.v`, `seam_engine`, `seam_ipl`, `seam_cpuregs`,
   `rtl/minimig_m68k_hybrid_bridge.v`; GPLv3, compatible with this project
   with attribution) and Emu68-A9 (MPL 2.0, raparici/Emu68-A964). A JIT of
   our own would duplicate years of work.
2. **Bridge:** the full h2f AXI bridge, as BigMig uses, or the lightweight one
   step 1d measured (~320 ns a read on cpu 0, writes not posted through
   `/dev/mem`). Either is a `sys/` change that every Template_MiSTer sync must
   carry; record it beside the SNAC `hps_io` lines.
3. **Chip RAM:** through `sdram_ctrl`'s CPU port and `cpu_cache_new`, which
   already snoops chipset writes -- no ARM-side copy, no coherency protocol.
4. **CPU core:** bare metal with `maxcpus=1` from a per-core `.txt` sidecar;
   Linux on the CPU core is what gives the 20-23 us p99 every run here shows.
5. **Not** the DDR3 mailbox: 2.5 us a round trip, and nothing on the ARM side
   shortens it (steps 1b, 1c).
6. **Logic and timing:** the core has no room to add a seam beside TG68K and
   fx68k (it closes on a thin margin: SEED 20, +0.035 ns, and the read gate
   into the TG68K is its wall). An ARM-CPU AmigaCD would be a separate build
   with the soft CPUs removed, as BigMig does -- and the timing-sensitive part
   of the CD32 library would still need the cycle-exact build.
7. **Where AmigaCD adds value instead:** its CD32/CDTV work (Akiko's CD path,
   the CDTV bridge, CD32 save states) is what BigMig lacks. Offering it there
   (option 2) is the cheaper route to a fast CD32-capable machine.
