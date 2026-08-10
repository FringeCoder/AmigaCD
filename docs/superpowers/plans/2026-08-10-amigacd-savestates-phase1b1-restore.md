# AmigaCD Save States — Phase 1B-1: the restore path

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a saved state loadable, so a CD32 machine's CPU registers, chip RAM and memory-map state can be rewound across a power cycle.

**Architecture:** Phase 1A built the capture half and left every restore input tied off. This plan builds the other direction through the same modules: a DDR3 read path into `ss_ctrl`, header validation before anything is touched, `ss_serdes`'s load side, a write direction on `ss_dma`, and write ports on the registers that need to be reloaded — including a new external write port on the TG68K register file.

**Tech Stack:** Verilog-2005 (Icarus for simulation, Quartus Prime Lite 17.0.2 for synthesis), VHDL for the TG68K change, C++14 for host code cross-built for ARM, GNU Make, GitHub Actions.

## Global Constraints

- **Verilog-2005 only.** CI compiles with `iverilog -g2005 -Wall`. No SystemVerilog under `rtl/`.
- Every RTL change must build **warning-free** under `iverilog -g2005 -Wall`.
- Canonical RTL lives in `AmigaCD/rtl/`, testbenches in `AmigaCD/rtl/tb/`, with a **copy** in `upstream/core-cd32-cdtv/rtl/`. The core is a **separate git repository** on branch `feature/misterfloppy`; commit there separately.
- Host C++ is `-std=gnu++14`, `-Wall -Wextra`, **zero warnings**. There is no local ARM toolchain — CI is the compile gate.
- Save state format is **version `0x00010000`**, magic `0x53534341`. Header words 0..7: counter, length, magic, version, core_words, host_words, core_crc32, host_crc32. `length` counts words after word 1. Core payload starts at word 8. Authoritative definition: `support/minimig/minimig_savestate.h`.
- DDR3 window is `0x3E000000`, slot size `0x400000`, four slots.
- **Validate before freezing.** A rejected restore must be a no-op on a still-running machine. There is no best-effort path: a partially restored Amiga is a hung Amiga with no diagnosis.
- Save states are disabled when `cpucfg == 0` (fx68k has no state export). Already enforced by `ss_supported` in `Minimig.sv`; do not weaken it.
- The core currently closes timing at **+0.313 ns**. Every phase reports worst-case slack; a task that does not report slack has not been verified.
- Icarus is at `C:\iverilog\bin` (off PATH). Local C++ testing uses `clang++` at `C:\Program Files\LLVM\bin` via `make -C tests CXX=...`.
- **Quartus hygiene:** never commit `Minimig.qsf` drift — `git diff --stat Minimig.qsf` and revert after every run. Clear `db/` before a fit to stop include-expansion. Check `git diff --stat` before committing; editor round-trips can normalise line endings into thousand-line noise diffs.

## Why Phase 1B is split

The design doc's Phase 1B bundles three separable bodies of work. Bundled, the first testable deliverable is far away and restore bugs arrive tangled with inventory bugs.

| Plan | Content | Deliverable |
|---|---|---|
| **1B-1 (this plan)** | The restore path | A machine whose CPU registers, chip RAM and map state provably rewind — the hardware test Phase 1A could not complete |
| 1B-2 | Chipset inventory: Agnus, Denise, Paula, CIA A/B, Akiko | A real CD32 game resumes |
| 1B-3 | Host block: CD session state, config fingerprint, three-way restore outcome | Correct CD resume and the safety gates |

1B-1 first because it closes the loop. Until restore works, **every** register added in 1B-2 is unverifiable on hardware — you could only check that it was written to a file, not that it came back.

## What Phase 1A left stubbed

Read these before starting; they define the work.

- `rtl/ss_ctrl.v:152` — `.load_start(1'b0), .word_in_valid(1'b0), .word_in(32'd0)`. The serdes load side is tied off.
- `rtl/ss_ctrl.v:123` — `assign ddr_read = 1'b0;`. `ddr_readdata` and `ddr_readdatavalid` are inputs with no reader.
- `rtl/ss_dma.v` — reads only. `sd_state` is hardwired `2'd2` ("read data"); there is no write direction.
- `TG68KdotC_Kernel.vhd:583-584` — `regfile` is written only under `Wwrena`, from microcode. Phase 1A added an asynchronous **read** port; there is no way to write a register from outside.
- `rtl/minimig.v:995` — `ovl` is a plain `reg` driven by normal logic, as are the other three map bits. Nothing can load them from outside.

## File Structure

| File | Change |
|---|---|
| `rtl/ss_dma.v` | Add a write direction (SDRAM writeback) |
| `rtl/tb/ss_dma_tb.v` | Cover the write direction against the same level-ack mock |
| `rtl/ss_ctrl.v` | Chip RAM byte order (Task 0); DDR3 read path, header validation, restore FSM |
| `rtl/tb/ss_ctrl_tb.v` | Restore sequence, and refusal for each gate |
| `rtl/ss_state.vh` | Note that the list is now an lvalue as well as an rvalue |
| `upstream/…/rtl/tg68k/TG68KdotC_Kernel.vhd` | External register-file write port |
| `upstream/…/rtl/cpu_wrapper.v` | Plumb the write port |
| `upstream/…/rtl/minimig.v` | Make the map bits loadable |
| `upstream/…/Minimig.sv` | Restore request, state fan-out, Kickstart CRC |
| `menu.cpp` | Restore row, edge-triggered like the save row |
| `docs/savestates-bringup.md` | Bring-up results |

---

### Task 0: Fix the chip RAM byte order at capture — do this first

Found on hardware, 2026-08-10, by analysing the first real save state
(`Flink (1994)_1.ss`). `ss_ctrl` packs two SDRAM 16-bit words into one
little-endian 32-bit DDR3 word, so **each Amiga word's two bytes land reversed**
relative to 68k big-endian order. Nothing in the payload is readable as Amiga
memory until you swap them back.

This has to be fixed before the restore path exists, not after:

- Restore must write chip RAM back in the order the machine expects. If capture
  and restore are both wrong in the same way they cancel out — until anything
  else touches the file, at which point the bug is baked into the format.
- A state file that matches Amiga byte order is directly inspectable, which is
  the only reason the analysis that found this was possible at all.
- Exactly one v1.0 file exists in the world. Fixing it now costs nothing;
  fixing it later means a format version bump and migration.

**Files:** modify `rtl/ss_ctrl.v` and `rtl/tb/ss_ctrl_tb.v`; copy to
`upstream/core-cd32-cdtv/rtl/`.

- [ ] **Step 1: Write the failing test**

In `rtl/tb/ss_ctrl_tb.v`, the mock SDRAM returns `16'hD000 + addr`. Assert the
packed DDR3 word carries those two 16-bit values **in Amiga byte order** — the
first word's high byte at the lowest file offset:

```verilog
	// Chip RAM must land in the file in 68k byte order, so a state file can be
	// read as Amiga memory. Word N's high byte belongs at the lower address.
	check("chip pair byte order", ddr_word(10),
	      {byteswap16(16'hD001), byteswap16(16'hD000)});
```

Add `byteswap16` as a testbench function so the expectation states the intent
rather than a pre-swapped magic number.

- [ ] **Step 2: Run to verify it fails**

```bash
export PATH="$PATH:/c/iverilog/bin" && iverilog -g2005 -Wall -o build/ss_ctrl_tb rtl/ss_ctrl.v rtl/ss_crc32.v rtl/ss_quiesce.v rtl/ss_serdes.v rtl/ss_dma.v rtl/tb/ss_ctrl_tb.v && vvp build/ss_ctrl_tb
```
Expected: FAIL on `chip pair byte order`, with the current packing.

- [ ] **Step 3: Swap the bytes as the pair is queued**

In `ss_ctrl`'s `S_CHIP` state, byte-swap each 16-bit word as it goes into the
32-bit pair. Do it here rather than in `ss_dma`, which should stay a plain
memory reader with no opinion about endianness.

- [ ] **Step 4: Verify, and re-check the CRC still agrees**

Re-run the bench. Then confirm the payload CRC still matches a host-side
`zlib.crc32` over the same bytes — the CRC is computed over the payload as
written, so changing the byte order changes it, and the host/RTL agreement
proven on hardware must still hold.

- [ ] **Step 5: Commit and refresh the core copy**

- [ ] **Step 6: Add the verification this exposed**

The same analysis raised something unresolved, and it belongs in this plan's
acceptance criteria rather than being forgotten:

**Address 4 of the captured chip RAM was zero.** On a booted AmigaOS that holds
`ExecBase`, and the string `"chip memory"` — the name Exec gives its chip
MemHeader — appeared nowhere in 2 MB. Against that, exception vectors at
addresses 8 and 12 held `0x00F80B1A`, a valid Kickstart ROM address, and game
data was present once byte order was corrected.

So the chip RAM path is **not independently verified** even though the header,
CRC and CPU registers are. Add to Task 8's hardware acceptance: dump a state
from a booted machine and confirm

- address 4 holds a plausible `ExecBase` pointer into chip RAM, and
- Exec's chip `MemHeader` is locatable, with `mh_Free` a sane figure.

If those still fail after the byte-order fix, the chip RAM capture is reading
the wrong thing and that must be resolved before any restore is trusted —
restoring a wrong memory image is worse than not restoring at all.

---

### Task 1: Write direction for `ss_dma`

Restore has to put chip RAM back. The module currently only reads, and the same per-word chip-select discipline applies — the cache behind the CPU port holds its ack as a level and only clears it when chip select drops (`cpu_cache_new.v:490`), so a held CS repeats one address forever.

**Files:**
- Modify: `rtl/ss_dma.v`, `rtl/tb/ss_dma_tb.v`
- Copy to: `upstream/core-cd32-cdtv/rtl/ss_dma.v`

**Interfaces:**
- Produces: `ss_dma` gains `input write_mode`, `input [15:0] word_in`, `output word_req`. With `write_mode` high, `sd_state` becomes `2'd3` (write data, per `sdram_ctrl.v:120` `cpu_we = (cpustate == 3)`) and `sd_wr[15:0]` carries data out. `word_req` pulses one cycle before each word is needed so the caller can present it.
- Unchanged in read mode: every existing port keeps its meaning, and `write_mode` low must produce byte-identical behaviour to today.

- [ ] **Step 1: Write the failing test**

Extend `rtl/tb/ss_dma_tb.v`. Keep the existing read tests untouched — they are the proof that the write direction did not regress reads. Add, in the same `check` idiom:

```verilog
	// Write direction. The mock records what lands at each address so the
	// test can prove the right data reached the right place, not merely that
	// the right number of cycles elapsed.
	got_count = 0;
	write_mode = 1;
	base_addr  = 24'h000040;
	word_count = 24'd4;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	wait (done);
	@(negedge clk);

	check("write: word count", wr_count, 4);
	check("write: first address", wr_addr[0], 24'h000040);
	check("write: last address",  wr_addr[3], 24'h000043);
	check("write: data 0", wr_data[0], 16'hAA00);
	check("write: data 3", wr_data[3], 16'hAA03);
	check("write: cs released", sd_cs, 1'b0);
	check("write: state was 3", saw_state_3, 1'b1);
	check("write: never used state 2", saw_state_2_in_write, 1'b0);
```

Add to the mock an address/data recorder active when `sd_state == 2'd3`, and a `word_req`-driven producer supplying `16'hAA00 + i`. Then re-run the existing read block afterwards and assert it still passes, so a regression in read mode is caught in the same run.

- [ ] **Step 2: Run to verify it fails**

```bash
export PATH="$PATH:/c/iverilog/bin" && mkdir -p build && iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v && vvp build/ss_dma_tb
```
Expected: FAIL — `write_mode` is not a port of `ss_dma`.

- [ ] **Step 3: Implement**

Add to the port list:

```verilog
	input             write_mode,
	input      [15:0] word_in,
	output reg        word_req,
	output     [15:0] sd_wr,
```

Change the fixed state assignment to follow the mode, and drive the write data:

```verilog
assign sd_state = write_mode ? 2'd3 : 2'd2;
assign sd_wr    = word_in;
```

Keep the per-word CS handshake exactly as it is — assert, wait `sd_ready`, deassert, re-assert — and pulse `word_req` in the gap cycle so the caller has the next word ready before CS rises. Do not add a second FSM; the existing one already sequences addresses correctly and the only difference is the direction of the data.

- [ ] **Step 4: Run to verify it passes**

Same command. Expected: all read checks still pass, plus the eight new write checks, then `ALL TESTS PASSED`, no `-Wall` warnings.

- [ ] **Step 5: Commit**

```bash
cp rtl/ss_dma.v upstream/core-cd32-cdtv/rtl/ss_dma.v
git add rtl/ss_dma.v rtl/tb/ss_dma_tb.v && git commit -m "feat: write direction for the chip RAM DMA"
```

---

### Task 2: External write port on the TG68K register file

Restoring `D0`–`A7` means writing sixteen registers the CPU core only ever writes from its own microcode.

**Files:**
- Modify: `upstream/core-cd32-cdtv/rtl/tg68k/TG68KdotC_Kernel.vhd`, `rtl/cpu_wrapper.v`

**Interfaces:**
- Produces on the kernel: `ss_wr_index : in std_logic_vector(3 downto 0)`, `ss_wr_data : in std_logic_vector(31 downto 0)`, `ss_wr_en : in std_logic`. Plus `ss_pc_wr`, `ss_sr_wr`, `ss_usp_wr` with their own enables for `TG68_PC`, `FlagsSR`/`Flags` and `USP`.
- All inputs get `:= (OTHERS => '0')` / `:= '0'` defaults so an instantiation that does not connect them is unchanged.

- [ ] **Step 1: Understand what verifies this**

There is still no VHDL simulator here (Icarus cannot read VHDL; ModelSim is not installed). This task has **no unit test and none is possible**. It is verified by Quartus elaboration in Step 4 and by Task 8's hardware round-trip, where a wrong index or a missed enable shows up as a register that fails to come back. Do not claim more coverage than that.

- [ ] **Step 2: Add the write port to the entity**

After the Phase 1A export ports, extend the port list (note the preceding port needs a trailing semicolon):

```vhdl
		ss_wr_index				: in  std_logic_vector( 3 downto 0) := (OTHERS => '0');
		ss_wr_data				: in  std_logic_vector(31 downto 0) := (OTHERS => '0');
		ss_wr_en					: in  std_logic := '0';
		ss_pc_wr					: in  std_logic := '0';
		ss_sr_wr					: in  std_logic := '0';
		ss_usp_wr				: in  std_logic := '0'
```

- [ ] **Step 3: Write into the register file**

In the register-file process, alongside the existing `IF Wwrena='1' THEN regfile(RDindex_A) <= regin; END IF;` (line ~583), add a second, higher-priority write. The savestate write only happens while the CPU is frozen, so `Wwrena` cannot be asserted at the same time — but make the priority explicit rather than relying on that:

```vhdl
				IF ss_wr_en='1' THEN
					regfile(conv_integer(ss_wr_index)) <= ss_wr_data;
				ELSIF Wwrena='1' THEN
					regfile(RDindex_A) <= regin;
				END IF;
```

Note this sits inside the `IF clkena_lw='1'` guard. **That guard is a problem:** while frozen, the CPU's clock enable is held low, so this branch never executes. Move the savestate write outside the `clkena_lw` guard but inside the `rising_edge(clk)` block, and say in your report that you did — this is the one place the freeze and the restore path conflict, and getting it wrong means the registers are silently never written.

Apply the same treatment to `TG68_PC`, `FlagsSR`/`Flags` and `USP` at their own assignment sites.

- [ ] **Step 4: Verify elaboration**

```bash
cd "C:/Users/Admin/Documents/RetroCoding/AmigaCD/upstream/core-cd32-cdtv" && export PATH="$PATH:/c/intelFPGA_lite/17.0/quartus/bin64" && quartus_map Minimig
```
Expected: `Analysis & Synthesis was successful`, 0 errors. Then `git diff --stat Minimig.qsf` and revert any drift.

- [ ] **Step 5: Plumb through `cpu_wrapper` and commit**

Mirror the Phase 1A export plumbing: declare the inputs on `cpu_wrapper`, connect them to `cpu_inst_p` only (fx68k is out of scope and locked out), then commit in the core repo.

---

### Task 3: Make the restored state writable

`SS_STATE_LIST` is currently only an rvalue. Restore needs it to be an lvalue too — that is what makes "saved but never restored" a compile error rather than a silent bug.

**Files:**
- Modify: `upstream/core-cd32-cdtv/rtl/minimig.v`, `Minimig.sv`, `rtl/ss_state.vh`

- [ ] **Step 1: Make the map bits loadable**

`ovl` (`minimig.v:995`) and the other three map bits are plain regs driven by normal logic. Add a load override in the same always block that drives each:

```verilog
	if (ss_map_we) ovl <= ss_map_in[3];
	else if (...) // existing logic unchanged
```

`minimig.v` already exports these as `ss_map[3:0]`; add the matching `ss_map_in[3:0]` and `ss_map_we` inputs and keep the bit order identical in both directions. A mismatch here silently swaps two map bits and produces a machine with the wrong thing at address zero.

- [ ] **Step 2: Fan the restored vector out**

In `Minimig.sv`, unpack `ss_state_out` with the **same macro** used to pack it:

```systemverilog
// The restore side expands from the same list as the capture side, so a
// register added to one is a width mismatch unless it is added to both.
wire [`SS_STATE_W-1:0] ss_state_out;
assign {ss_restore_cpu_d0, ss_restore_cpu_d1, /* … */} = ss_state_out;
```

Rather than writing that concatenation by hand — which reintroduces exactly the ordering bug the macro exists to prevent — reuse `` `SS_STATE_LIST `` by declaring the restore targets with the same names in a generate-free wrapper, or split the list into a second macro that names restore-side wires in the identical order. Choose one and state in your report why it cannot drift from the capture order.

- [ ] **Step 3: Sequence the register writes**

The CPU register file has one write port, so the sixteen registers are written one per cycle, mirroring the Phase 1A read sweep. Reuse that sweep's counter and drive `ss_wr_en` from it while `ss_restoring` is high.

- [ ] **Step 4: Verify elaboration and commit**

`quartus_map Minimig`, 0 errors, revert qsf drift, commit in the core repo.

---

### Task 4: DDR3 read path and header validation in `ss_ctrl`

**Files:**
- Modify: `rtl/ss_ctrl.v`, `rtl/tb/ss_ctrl_tb.v`

**Interfaces:**
- Produces: `ss_ctrl` gains `input load_req`, `output load_busy`, `output load_ok`, `output load_fail`, `output [3:0] load_fail_code`.
- `load_fail_code` distinguishes the refusal reasons so the OSD and the log can say which gate rejected the file: 1 = bad magic, 2 = bad version, 3 = length out of range, 4 = payload CRC mismatch, 5 = Kickstart CRC mismatch.

- [ ] **Step 1: Write the failing test**

Add to `rtl/tb/ss_ctrl_tb.v` a mock DDR3 that returns a caller-supplied window, then:

```verilog
	// A well-formed window restores and reports success.
	build_valid_window();
	@(negedge clk); load_req = 1;
	wait (load_ok);
	check("restored state vector", restored_state, state_in);
	check("no failure reported", load_fail, 1'b0);
	load_req = 0;

	// Each gate refuses, and refusing must not freeze the machine.
	build_valid_window(); win[2] = 32'hDEADBEEF;   // magic
	run_load(); check("bad magic refused", load_fail_code, 4'd1);
	check("bad magic did not freeze", saw_freeze, 1'b0);

	build_valid_window(); win[3] = 32'h00020000;   // version
	run_load(); check("bad version refused", load_fail_code, 4'd2);

	build_valid_window(); win[1] = 32'hFFFFFFFF;   // length
	run_load(); check("bad length refused", load_fail_code, 4'd3);

	build_valid_window(); win[9] = win[9] ^ 32'd1; // payload
	run_load(); check("bad CRC refused", load_fail_code, 4'd4);
```

`saw_freeze` must be a sticky flag set by the `freeze` output. **The assertion that no gate freezes the machine is the point of this task** — a refusal that stops the Amiga is worse than the corrupt file it was protecting against.

- [ ] **Step 2: Run to verify it fails**

```bash
iverilog -g2005 -Wall -o build/ss_ctrl_tb rtl/ss_ctrl.v rtl/ss_crc32.v rtl/ss_quiesce.v rtl/ss_serdes.v rtl/ss_dma.v rtl/tb/ss_ctrl_tb.v && vvp build/ss_ctrl_tb
```
Expected: FAIL — `load_req` is not a port.

- [ ] **Step 3: Implement the read path and gates**

Replace `assign ddr_read = 1'b0;` with a real read master, and add validation states that run **before** `save_busy`-style freezing. Read the header words, check magic, version and length, then stream the payload through `ss_crc32` and compare against word 6 — all with the machine still running. Only once every gate passes may the freeze be requested.

Order the gates cheapest-first so a wrong file is rejected on a handful of DDR3 reads rather than a full payload scan.

- [ ] **Step 4: Run to verify it passes**

Same command. Expected: all save-side checks still pass, plus the restore and four refusal checks, then `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cp rtl/ss_ctrl.v upstream/core-cd32-cdtv/rtl/ss_ctrl.v
git add rtl/ss_ctrl.v rtl/tb/ss_ctrl_tb.v && git commit -m "feat: DDR3 read path and restore validation gates"
```

---

### Task 5: The restore sequence

**Files:**
- Modify: `rtl/ss_ctrl.v`, `rtl/tb/ss_ctrl_tb.v`

- [ ] **Step 1: Write the failing test**

Assert the ordering, because getting it wrong produces a machine that resumes and then misbehaves:

```verilog
	// Chip RAM must be written before the CPU is released, and the CPU
	// registers must be loaded before the freeze drops -- otherwise the CPU
	// executes one or more instructions against a half-restored machine.
	check("chip RAM written before release", chip_wr_done_before_release, 1'b1);
	check("regs loaded before unfreeze",     regs_done_before_unfreeze,   1'b1);
	check("freeze dropped exactly once",     freeze_fall_count,           1);
```

- [ ] **Step 2: Run to verify it fails**, then implement, then re-run.

- [ ] **Step 3: Implement**

Sequence: validate (machine running) → quiesce and freeze → stream the payload from DDR3 into `ss_serdes`'s load side → `state_we` fans the vector out → sweep the sixteen CPU register writes → stream chip RAM back through `ss_dma` in `write_mode` → release the freeze. The counter at word 0 is **not** touched on restore; it belongs to the host's save handshake.

- [ ] **Step 4: Commit** as in Task 4.

---

### Task 6: Kickstart fingerprint

The design doc requires it and Phase 1A skipped it. Kickstart is only conditionally read-only (`gary.v:110`), so it is genuinely mutable state, and restoring a state made under a different ROM produces a guru with no explanation.

**Files:**
- Modify: `rtl/ss_ctrl.v`, `Minimig.sv`, `rtl/tb/ss_ctrl_tb.v`

- [ ] **Step 1: Capture on save**

While frozen and already dumping memory, run a second `ss_dma` pass over the Kickstart region through `ss_crc32` and place the result in the payload. Add it to the header region, not the state vector — it is metadata about the machine, not machine state.

- [ ] **Step 2: Compare on restore**

Recompute over the running machine's ROM during validation, before the freeze, and refuse with `load_fail_code = 5` on mismatch.

- [ ] **Step 3: Test both directions**

A matching ROM restores; a mismatched ROM refuses and does not freeze. Assert both.

- [ ] **Step 4: Commit.**

---

### Task 7: OSD restore row and integration

**Files:**
- Modify: `menu.cpp`, `upstream/core-cd32-cdtv/Minimig.sv`

- [ ] **Step 1: Add the row**

A `Restore state` row next to `Save state` on the System page, greyed out by the same `minimig_ss_blocked()` condition. Renumber the rows that follow and widen `menumask` — count every index and check the key handler matches the display, exactly as the save row required.

- [ ] **Step 2: Edge-trigger it**

Claim the next free status bit and edge-detect it **in RTL**, mirroring `ss_save_edge`. A level here would restore over and over. Same reasoning as the save request: userspace clearing a bit must not be what stands between the user and a wedged machine.

- [ ] **Step 3: Surface the outcome**

`load_fail_code` should reach the user. An `Info()` toast naming the reason — wrong version, corrupt file, wrong Kickstart — turns a silent no-op into a diagnosis. Phase 1A left `save_ok`/`save_fail` unconnected; wire those at the same time.

- [ ] **Step 4: Full fit and the timing gate**

```bash
cd "C:/Users/Admin/Documents/RetroCoding/AmigaCD/upstream/core-cd32-cdtv" && rm -rf db incremental_db && export PATH="$PATH:/c/intelFPGA_lite/17.0/quartus/bin64" && quartus_sh --flow compile Minimig
```

Then record worst-case slack:

```bash
grep "Worst-case setup slack" output_files/Minimig.sta.rpt | head -1
```

Expected: **slack ≥ 0.000 ns**, TNS 0.000, zero "Timing requirements not met". The baseline going in is +0.313 ns. Revert qsf drift, verify the `.rbf` mtime and size are fresh, and commit.

---

### Task 8: Hardware bring-up — the test Phase 1A could not finish

**Files:**
- Modify: `docs/savestates-bringup.md`

- [ ] **Step 1: Regression check first**

Boot a normal CD32 game and confirm it still runs cleanly. The core carries a memory-controller change with no hardware verification behind it; if this fails, the problem is not save states.

- [ ] **Step 2: The round trip**

With HRTmon enabled: write `$DEADBEEF` to `$00080000`, set `D0` to `$11111111` and `A3` to `$22222222`, exit, save to slot 1, re-enter and overwrite all three, exit, restore slot 1, and read them back.

Expected: all three hold their saved values. **Check a data register and an address register** — they are separate halves of the register-file index range and an off-by-eight would pass a D-register-only test.

- [ ] **Step 3: Across a power cycle**

Repeat through the save, power cycle, reload the core, restore slot 1. Expected: all three return. **This is Phase 1B-1's gate.**

- [ ] **Step 4: The refusal paths**

After the happy path works, not interleaved with it: restore a slot never written; corrupt a `.ss1` byte and restore; restore a state made under a different Kickstart. Each must refuse with a clear reason **and leave the machine running**.

- [ ] **Step 5: Record results**

Slack, freeze duration, whether the display glitches across the freeze, and every refusal's behaviour. Commit.

---

## What 1B-2 and 1B-3 add

**1B-2 — chipset inventory.** One task per chip, each extending `` `SS_STATE_LIST `` and verified by the `ss_serdes` round-trip bench before hardware. Derive each chip's list from its register-write decoder, which is the authoritative enumeration — for Agnus that is the `reg_address` case block, and likewise for Denise, Paula and the CIAs. Audit each against WinUAE's corresponding `.uss` chunk (source at `C:\Users\Admin\Documents\mcp-winuae-emu\src`) and record the comparison. Expect the state vector to grow from 632 bits to several thousand; watch the fit each time. Gate: a real CD32 game resumes and keeps playing.

**1B-3 — host block.** CD session state from `akiko_cd32.cpp` (the CDDA cursors, play range and pending-push flags), the `mm_configTYPE` fingerprint, disc identity via the existing `physdisc/<identity>` mechanism, and the three-way restore outcome — silent when the config matches and images are unchanged, warn-with-override when a writable image changed, hard refuse when the config differs. Gate: resume a game from a physical disc, and the writable-media warning fires when it should.
