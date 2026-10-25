# A 68k on the ARM: plan and measurements

The idea: a CPU option where the 68k runs on the DE10-Nano's ARM (Cortex-A9),
the way a PiStorm's does, with a small bus proxy in the FPGA doing the chipset
accesses for it. Kickstart and Fast RAM would live on the ARM side and run at
ARM speed; every chip RAM and custom register access would cross to the FPGA.

The MiSTer framework gives a core no AXI bridge to the ARM. The two paths are
`hps_io` (microseconds per word) and DDR3, which both sides reach; the A2065
already talks to Main through a DDR3 mailbox (`a2065_ddr3_mailbox.v` in the
core). So the cost of one chipset access is the DDR3 round trip, and that is
what step 1 measures.

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

## Step 1b: the ARM half on its own

`latprobe -r` times the ARM's own uncached read, write, write + barrier and
write-then-read-back on an unused word of the window (0x1FF08090); it needs no
FPGA, so any AmigaCD core will do. `-S` maps without `O_SYNC` and `-B` drops
the barriers from the echo loop. If the ARM's own accesses come out near
1 us each, the round trip is bounded by the ARM's path to DDR3, not by the FPGA
or this protocol, and the next lever is the mapping (or a different bridge),
not the proxy.
