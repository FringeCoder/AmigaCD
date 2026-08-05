# PSX SNAC — Plan 1: the reader and the menu

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read PSX controllers on the SNAC user port and let them drive the MiSTer OSD,
so a PSX pad alone can pick and launch a core — something no MiSTer core does today.

**Architecture:** A standalone Verilog bus master (`snac_psx.v`) polls both SNAC ports and
publishes pad state; the Menu core answers a new UIO read command with it; userspace polls
that and injects OSD keys through `menu_key_set()`. The module is developed and tested in
*this* repo, where CI lives, then copied into core repos. **No AmigaCD RTL is touched by
this plan** — that is Plan 2.

**Tech Stack:** Verilog-2005, Icarus Verilog (simulation, in CI), Quartus Prime Lite 17.0
(Menu core fit), C++14 (arm-none-linux-gnueabihf 10.2), GitHub Actions.

## Global Constraints

- **PRs target `develop`.** Always `gh pr create --base develop`; the repo default is `master`.
- **Spec:** `docs/superpowers/specs/2026-08-04-psx-snac-design.md`. This plan implements
  steps 1–3 of its Sequencing section only.
- **No AmigaCD RTL changes.** `upstream/core-cd32-cdtv` is not modified by any task here.
  If a task seems to need it, stop — it belongs to Plan 2.
- **Zero new compiler warnings.** The tree builds clean and stays clean.
- **No C++ test framework exists and none is being added.** C++ verification is (a) CI
  cross-build green, (b) a named hardware check. RTL verification is the Icarus testbench.
- **ModelSim is not installed** on the dev machine (`C:\intelFPGA_lite\17.0` has `quartus`
  only). RTL simulation runs under Icarus Verilog, in CI, matching how this repo already
  handles having no local toolchain.
- **PSX bus:** 250 kHz, 8 bits per byte, **LSB first**, CMD driven on the falling edge of
  CLK and DAT sampled on the rising edge. CLK and ATT idle high. All pad button bits are
  **active low**.
- **`SW[1]` on the DE10-Nano must be off** for any hardware check. It steals USER_IO 2/4/5
  — `CMD`, `DAT`, `CLK` — and fails silently.

## Pin map

Fixed by `MiSTer-devel/PSX_MiSTer` `PSX.sv`; every PSX SNAC adapter is wired to it.

| USER_IO | Dir | Signal |
|---|---|---|
| 0 | out | `~ATT` port 2 |
| 1 | out | `~ATT` port 1 |
| 2 | out | `CMD` |
| 3 | in | `ACK` |
| 4 | in | `DAT` |
| 5 | out | `CLK` |
| 6 | out | csync to GunCon (**Plan 2**; driven constant 1 here) |

## File Structure

**This repo — new:**

| File | Responsibility |
|---|---|
| `rtl/snac_psx.v` | The bus master. Canonical copy; core repos take a copy of this file |
| `rtl/tb/psx_pad_model.v` | Simulation model of a PSX pad — protocol, ACK, configurable ID and payload |
| `rtl/tb/snac_psx_tb.v` | Testbench driving the model against the DUT |
| `rtl/README.md` | Why RTL lives in the userspace repo |
| `support/snac/snac_psx.cpp` | UIO read, poll, edge detection, OSD key injection |
| `support/snac/snac_psx.h` | Its interface |

RTL lives here because the module is shared by two core repos and this is the repo with CI.
`support/snac/` is picked up automatically — the Makefile globs `./support/*/*.cpp`
(`Makefile:42`).

**This repo — modified:**

| File | Change |
|---|---|
| `.github/workflows/build.yml` | New `rtl-sim` job running the testbench |
| `user_io.h` | `UIO_MM2_SNAC 0xFD`, `UIO_MM2_SNACRD 0xFE` |
| `user_io.cpp` | Call `snac_psx_poll()` from `user_io_poll()` |
| `cfg.h`, `cfg.cpp` | `snac_psx` ini key |

**Menu core fork — `upstream/core-menu` (separate repo):**

| File | Change |
|---|---|
| `rtl/snac_psx.v` | Copy of ours |
| `files.qip` | Add it to the build |
| `sys/hps_io.sv` | Decode command `'hFE`, return pad state |
| `menu.sv` | Instantiate the reader, wire `USER_IN`/`USER_OUT` and the state to `hps_io` |

---

### Task 1: Simulation harness and a failing test

Build the test rig first, against a port-shaped stub that does nothing. The test must fail
for a behavioural reason, not a compile error.

**Files:**
- Create: `rtl/snac_psx.v` (stub), `rtl/tb/psx_pad_model.v`, `rtl/tb/snac_psx_tb.v`, `rtl/README.md`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: `snac_psx` module with the port list below. Every later RTL task and both core
  repos depend on these exact names and widths.

- [ ] **Step 1: Create the stub with the final port list**

`rtl/snac_psx.v`:

```verilog
// Required: the testbench declares `timescale 1ns/1ns, and this file is compiled
// before it. Without a directive here, Icarus mixes default and timescale-based
// delays and the pad model's microsecond ACK pulse never resolves.
`timescale 1ns/1ns

// PSX controller reader for the MiSTer SNAC user port.
//
// Canonical copy lives in the AmigaCD (Main_MiSTer) repo; core repos take a copy.
// Pin map is fixed by PSX_MiSTer/PSX.sv, which every PSX SNAC adapter is wired to.
//
// Bus: 250 kHz, LSB first, CMD driven on CLK falling edge, DAT sampled on rising.
// Both ports share CLK/CMD/DAT/ACK and are selected by their own ATT line, so one
// master serves both, polled in sequence.
module snac_psx #(
	parameter CLK_MHZ  = 50,   // frequency of clk, MHz
	parameter BAUD_KHZ = 250   // PSX bus clock, kHz
) (
	input             clk,
	input             reset,
	input             enable,     // 0 = idle the bus and report nothing

	input       [6:0] user_in,
	output      [6:0] user_out,

	// Button vectors, active HIGH:
	// [0] RIGHT [1] LEFT [2] DOWN [3] UP [4] X [5] O [6] SQUARE [7] TRIANGLE
	// [8] L1 [9] R1 [10] START [11] SELECT [12] L2 [13] R2 [14] L3 [15] R3
	output reg [15:0] pad0,
	output reg [15:0] pad1,
	// {LY, LX, RY, RX}, 8 bits each, 0x80 = centre
	output reg [31:0] axes0,
	output reg [31:0] axes1,
	// Raw device ID byte: 0x41 digital, 0x73 DualShock analog, 0x63 GunCon.
	// 0x00 means nothing answered.
	output reg  [7:0] id0,
	output reg  [7:0] id1
);

assign user_out = 7'h7F;

always @(posedge clk) begin
	pad0  <= 16'h0000;
	pad1  <= 16'h0000;
	axes0 <= 32'h80808080;
	axes1 <= 32'h80808080;
	id0   <= 8'h00;
	id1   <= 8'h00;
end

endmodule
```

- [ ] **Step 2: Create the pad model**

`rtl/tb/psx_pad_model.v`:

```verilog
// Required, and load-bearing: `#4000` below is the ACK pulse width. Without this
// directive the delay is not interpreted in nanoseconds and ACK never deasserts.
`timescale 1ns/1ns

// Simulation model of a PSX controller. Answers the 0x01/0x42 poll with a
// configurable device ID and payload, and pulses ACK after every byte except
// the last, exactly as real hardware does.
module psx_pad_model #(
	parameter [7:0] DEVICE_ID = 8'h41,   // 0x41 digital, 0x73 DualShock
	parameter integer PAYLOAD_BYTES = 2
) (
	input       att,      // active low
	input       clk,      // bus clock from the master
	input       cmd,
	output reg  dat,
	output reg  ack,
	input [7:0] btn0,     // raw PSX byte 0, active low
	input [7:0] btn1,     // raw PSX byte 1, active low
	input [7:0] ax_rx, input [7:0] ax_ry, input [7:0] ax_lx, input [7:0] ax_ly
);

integer byte_idx;
integer bit_idx;
reg [7:0] rx;
reg [7:0] tx;

function [7:0] reply(input integer idx);
	case (idx)
		0: reply = 8'hFF;
		1: reply = DEVICE_ID;
		2: reply = 8'h5A;
		3: reply = btn0;
		4: reply = btn1;
		5: reply = ax_rx;
		6: reply = ax_ry;
		7: reply = ax_lx;
		8: reply = ax_ly;
		default: reply = 8'hFF;
	endcase
endfunction

initial begin
	dat = 1'b1; ack = 1'b1; byte_idx = 0; bit_idx = 0; rx = 0; tx = 8'hFF;
end

always @(negedge att) begin
	byte_idx = 0;
	bit_idx  = 0;
	tx       = reply(0);
	dat      = tx[0];
end

always @(posedge att) begin
	dat = 1'b1;
	ack = 1'b1;
end

// Master drives CMD on the falling edge and samples DAT on the rising edge, so
// the pad does the mirror image: shift out on falling, sample CMD on rising.
always @(posedge clk) if (!att) begin
	rx[bit_idx] = cmd;
end

always @(negedge clk) if (!att) begin
	if (bit_idx == 7) begin
		bit_idx  = 0;
		byte_idx = byte_idx + 1;
		tx       = reply(byte_idx);
		dat      = tx[0];
		// ACK after every byte but the last of the transfer.
		if (byte_idx < (3 + PAYLOAD_BYTES)) begin
			ack = 1'b0;
			#4000 ack = 1'b1;   // ~4 us low, well inside the real 2-10 us
		end
	end
	else begin
		bit_idx = bit_idx + 1;
		dat     = tx[bit_idx];
	end
end

endmodule
```

- [ ] **Step 3: Create the testbench with the first assertion**

`rtl/tb/snac_psx_tb.v`:

```verilog
`timescale 1ns/1ns

module snac_psx_tb;

reg clk = 0;
reg reset = 1;
reg enable = 1;
wire [6:0] user_out;
reg  [6:0] user_in = 7'h7F;

wire [15:0] pad0, pad1;
wire [31:0] axes0, axes1;
wire  [7:0] id0, id1;

integer errors = 0;

always #10 clk = ~clk;   // 50 MHz

snac_psx #(.CLK_MHZ(50), .BAUD_KHZ(250)) dut (
	.clk(clk), .reset(reset), .enable(enable),
	.user_in(user_in), .user_out(user_out),
	.pad0(pad0), .pad1(pad1), .axes0(axes0), .axes1(axes1),
	.id0(id0), .id1(id1)
);

task check(input [255:0] name, input integer got, input integer want);
begin
	if (got !== want) begin
		$display("FAIL %0s: got %0d (0x%0h), want %0d (0x%0h)", name, got, got, want, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

// Port 1 pad: all buttons released (active low, so 0xFF), sticks centred.
wire att1 = user_out[1];
psx_pad_model #(.DEVICE_ID(8'h41), .PAYLOAD_BYTES(2)) pad_p1 (
	.att(att1), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p1), .ack(ack_p1),
	.btn0(8'hFF), .btn1(8'hFF),
	.ax_rx(8'h80), .ax_ry(8'h80), .ax_lx(8'h80), .ax_ly(8'h80)
);
wire dat_p1, ack_p1;

always @(*) begin
	user_in[4] = dat_p1;
	user_in[3] = ack_p1;
end

initial begin
	$dumpfile("snac_psx_tb.vcd");
	$dumpvars(0, snac_psx_tb);

	repeat (10) @(posedge clk);
	reset = 0;

	// One full poll of both ports at 250 kHz is on the order of 500 us.
	#2000000;

	check("port1 device id", id0, 8'h41);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 4: Document why RTL is in this repo**

`rtl/README.md`:

```markdown
# RTL

`snac_psx.v` is the canonical copy of the PSX SNAC reader. It lives here, in the
userspace repo, for two reasons:

- It is shared by two core repos (the Menu core fork and the AmigaCD core), and a
  module with two consumers needs one home rather than two drifting copies.
- This is the repo with CI, so this is where its testbench can run on every push.

Core repos take a **copy**. When this file changes, both copies must be refreshed —
`upstream/core-menu/rtl/snac_psx.v` and, from Plan 2 onward,
`upstream/core-cd32-cdtv/rtl/snac_psx.v`.

Simulation uses Icarus Verilog, not ModelSim, which is not installed on the dev
machine. See the `rtl-sim` job in `.github/workflows/build.yml`.
```

- [ ] **Step 5: Add the CI simulation job**

Append to `.github/workflows/build.yml`, at the same indent level as `build:` (i.e. a
second entry under `jobs:`):

```yaml
  # RTL simulation. Icarus rather than ModelSim: ModelSim is not installed on the
  # dev machine (C:\intelFPGA_lite\17.0 carries quartus only), and the testbench is
  # plain Verilog with no vendor primitives.
  rtl-sim:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Install Icarus Verilog
        run: sudo apt-get update && sudo apt-get install -y iverilog

      - name: Build testbench
        run: |
          set -euo pipefail
          mkdir -p build
          iverilog -g2005 -Wall -o build/snac_psx_tb \
            rtl/snac_psx.v rtl/tb/psx_pad_model.v rtl/tb/snac_psx_tb.v

      - name: Run testbench
        run: vvp build/snac_psx_tb
```

- [ ] **Step 6: Run the test locally if Icarus is available, otherwise push and read CI**

Local (only if `iverilog` is installed — it is not, as of writing):

```bash
iverilog -g2005 -Wall -o build/snac_psx_tb rtl/snac_psx.v rtl/tb/psx_pad_model.v rtl/tb/snac_psx_tb.v && vvp build/snac_psx_tb
```

Expected: `FAIL port1 device id: got 0 (0x0), want 65 (0x41)` then `1 FAILURE(S)`, and a
non-zero exit from `vvp`. The stub reports nothing, which is the point.

- [ ] **Step 7: Commit**

```bash
git add rtl .github/workflows/build.yml
git commit -m "test: PSX SNAC reader harness, with a stub that fails it"
```

---

### Task 2: The bus master

Make the stub actually talk to the pad: drive ATT/CLK/CMD, shift eight bits per byte LSB
first, handle byte boundaries, and capture the device ID.

> **Corrections applied during execution — the code blocks below are the original, defective
> drafts. The repository is the source of truth; see commits `06e5302` and `2165a8c`.**
>
> - **The pad model in Task 1 Step 2 is one bit ahead of real hardware.** Its
>   `always @(negedge clk)` block advances the bit index and *then* presents the bit; a real
>   pad presents bit N−1 on falling edge N. Against a corrected model the original master
>   read `ff 83 b4 fe …` instead of `ff 41 5a ff …` — every byte shifted left one with a 1
>   injected — while the testbench stayed green. Fixed in the model, not the master.
> - **Step 2 below states a false premise.** There is no same-cycle race on `shift_in[7]`:
>   the nonblocking write and the state transition commit on the same edge, so the next state
>   sees a valid byte. The capture is plain `rx_byte[bytecnt] <= shift_in;` and neither
>   `{dat_in, shift_in[6:0]}` nor a bit-0 latch is needed.
> - **`assign user_out`'s port-select ternaries are inverted** relative to what `reg port`
>   means elsewhere, so the first poll addressed the empty port.
> - **`wire ack_in` has no readers and `ST_ACK` does no ACK waiting** — it is byte-boundary
>   bookkeeping, since renamed `ST_BYTE_DONE`. Detecting an absent pad via ACK was considered
>   and left to Task 5, which uses the idle-bus `0xFF` signature instead.
> - **`div == HALF` gives 101 clocks per half period** (247.5 kHz, not 250); it wants
>   `HALF - 1`. `integer i;` is unused.

**Files:**
- Modify: `rtl/snac_psx.v`

**Interfaces:**
- Consumes: the port list from Task 1.
- Produces: `id0` reporting the real device ID byte; internal `rx_byte[0..8]` capture that
  Tasks 3–4 decode.

- [ ] **Step 1: Replace the stub body with the master**

In `rtl/snac_psx.v`, replace everything between the port list's closing `);` and
`endmodule` with:

```verilog
// Bus clock generation. Half-period counter: at 50 MHz and 250 kHz that is 100
// clocks per half period.
localparam integer HALF = (CLK_MHZ * 1000) / (BAUD_KHZ * 2);

localparam ST_IDLE   = 3'd0;
localparam ST_ATT    = 3'd1;
localparam ST_BYTE   = 3'd2;
localparam ST_ACK    = 3'd3;
localparam ST_DONE   = 3'd4;
localparam ST_GAP    = 3'd5;

reg  [2:0] state;
reg [15:0] div;
reg  [3:0] bitcnt;
reg  [3:0] bytecnt;
reg        port;          // 0 = SNAC port 1, 1 = SNAC port 2
reg        sclk;
reg        scmd;
reg        att_n;
reg  [7:0] shift_out;
reg  [7:0] shift_in;
reg  [7:0] rx_byte [0:8];
reg [15:0] gap;

// Command bytes: 0x01 selects the controller, 0x42 is the poll, then zeroes.
function [7:0] cmd_byte(input [3:0] idx);
	case (idx)
		4'd0: cmd_byte = 8'h01;
		4'd1: cmd_byte = 8'h42;
		default: cmd_byte = 8'h00;
	endcase
endfunction

assign user_out = { 1'b1,                  // [6] csync, Plan 2
                    sclk,                  // [5] CLK
                    1'b1,                  // [4] DAT (input)
                    1'b1,                  // [3] ACK (input)
                    scmd,                  // [2] CMD
                    (port == 1'b0) ? 1'b1 : att_n,   // [1] ~ATT port 1
                    (port == 1'b1) ? 1'b1 : att_n }; // [0] ~ATT port 2

wire dat_in = user_in[4];
wire ack_in = user_in[3];

integer i;

always @(posedge clk) begin
	if (reset || !enable) begin
		state   <= ST_IDLE;
		div     <= 0;
		bitcnt  <= 0;
		bytecnt <= 0;
		port    <= 0;
		sclk    <= 1'b1;
		scmd    <= 1'b1;
		att_n   <= 1'b1;
		gap     <= 0;
		id0     <= 8'h00;
		id1     <= 8'h00;
		pad0    <= 16'h0000;
		pad1    <= 16'h0000;
		axes0   <= 32'h80808080;
		axes1   <= 32'h80808080;
	end
	else case (state)

	ST_IDLE: begin
		att_n     <= 1'b0;
		sclk      <= 1'b1;
		bytecnt   <= 0;
		bitcnt    <= 0;
		shift_out <= cmd_byte(4'd0);
		div       <= 0;
		state     <= ST_ATT;
	end

	// Settle time after ATT falls before the first clock edge.
	ST_ATT: begin
		if (div == HALF) begin
			div   <= 0;
			scmd  <= shift_out[0];
			state <= ST_BYTE;
		end
		else div <= div + 1'b1;
	end

	ST_BYTE: begin
		if (div == HALF) begin
			div <= 0;
			if (sclk) begin
				// Falling edge: drive the next CMD bit.
				sclk <= 1'b0;
				scmd <= shift_out[bitcnt];
			end
			else begin
				// Rising edge: sample DAT.
				sclk            <= 1'b1;
				shift_in[bitcnt] <= dat_in;
				if (bitcnt == 4'd7) begin
					bitcnt <= 0;
					state  <= ST_ACK;
				end
				else bitcnt <= bitcnt + 1'b1;
			end
		end
		else div <= div + 1'b1;
	end

	ST_ACK: begin
		rx_byte[bytecnt] <= { dat_in, shift_in[6:0] };
		// Nine bytes covers ID + 0x5A + six payload bytes, which is every device
		// type we care about. Short replies simply read back as 0xFF.
		if (bytecnt == 4'd8) begin
			state <= ST_DONE;
		end
		else begin
			bytecnt   <= bytecnt + 1'b1;
			shift_out <= cmd_byte(bytecnt + 4'd1);
			div       <= 0;
			state     <= ST_BYTE;
		end
	end

	ST_DONE: begin
		att_n <= 1'b1;
		scmd  <= 1'b1;
		if (port == 1'b0) id0 <= rx_byte[1];
		else              id1 <= rx_byte[1];
		gap   <= 0;
		state <= ST_GAP;
	end

	// Gap between ports, and between polls of the same port.
	ST_GAP: begin
		if (gap == 16'hFFFF) begin
			port  <= ~port;
			state <= ST_IDLE;
		end
		else gap <= gap + 1'b1;
	end

	default: state <= ST_IDLE;
	endcase
end
```

- [ ] **Step 2: Fix the last-bit capture**

`shift_in[bitcnt]` for bit 7 is written in the same cycle `ST_ACK` reads it, so `ST_ACK`
reconstructs the byte as `{dat_in, shift_in[6:0]}` above. Confirm that line reads exactly
that — a plain `shift_in` there drops the MSB, and the device ID would come back as `0x41`
masked to `0x41 & 0x7F`, which happens to look correct and would hide the bug until a pad
with the high bit set (`0x73`, `0xFF`) appears.

- [ ] **Step 3: Run the testbench**

```bash
iverilog -g2005 -Wall -o build/snac_psx_tb rtl/snac_psx.v rtl/tb/psx_pad_model.v rtl/tb/snac_psx_tb.v && vvp build/snac_psx_tb
```

Expected: `PASS port1 device id` then `ALL TESTS PASSED`, exit 0.

- [ ] **Step 4: Commit**

```bash
git add rtl/snac_psx.v
git commit -m "feat: PSX SNAC bus master, device ID readback"
```

---

### Task 3: Digital pad decode

**Files:**
- Modify: `rtl/snac_psx.v`, `rtl/tb/snac_psx_tb.v`

**Interfaces:**
- Consumes: `rx_byte[3]`, `rx_byte[4]` from Task 2.
- Produces: `pad0` / `pad1` in the bit order given in the Task 1 port comment. Plan 2 maps
  these to CD32 buttons; userspace maps them to OSD keys.

- [ ] **Step 1: Add failing assertions to the testbench**

In `rtl/tb/snac_psx_tb.v`, make the port-1 pad's buttons drivable and assert the mapping.
Replace the `psx_pad_model` instantiation for port 1 and add checks:

```verilog
reg [7:0] p1_btn0 = 8'hFF;
reg [7:0] p1_btn1 = 8'hFF;

psx_pad_model #(.DEVICE_ID(8'h41), .PAYLOAD_BYTES(2)) pad_p1 (
	.att(att1), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p1), .ack(ack_p1),
	.btn0(p1_btn0), .btn1(p1_btn1),
	.ax_rx(8'h80), .ax_ry(8'h80), .ax_lx(8'h80), .ax_ly(8'h80)
);
```

and, in the `initial` block after the existing device-ID check:

```verilog
	// Byte 0 bit 5 is RIGHT, active low. Expect pad0[0].
	p1_btn0 = 8'hFF & ~8'h20;
	#2000000;
	check("right pressed", pad0, 16'h0001);

	// Byte 1 bit 6 is X, active low. Expect pad0[4].
	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF & ~8'h40;
	#2000000;
	check("x pressed", pad0, 16'h0010);

	// START is byte 0 bit 3 -> pad0[10]; SELECT is byte 0 bit 0 -> pad0[11].
	p1_btn0 = 8'hFF & ~8'h09;
	p1_btn1 = 8'hFF;
	#2000000;
	check("start+select", pad0, 16'h0C00);

	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF;
	#2000000;
	check("all released", pad0, 16'h0000);
```

- [ ] **Step 2: Run the testbench to verify the new checks fail**

```bash
iverilog -g2005 -Wall -o build/snac_psx_tb rtl/snac_psx.v rtl/tb/psx_pad_model.v rtl/tb/snac_psx_tb.v && vvp build/snac_psx_tb
```

Expected: `PASS port1 device id`, then `FAIL right pressed`, `FAIL x pressed`,
`FAIL start+select`, `4 FAILURE(S)` — `pad0` is still held at zero by the reset branch.
(`all released` passes trivially; that is fine, it guards the opposite direction later.)

- [ ] **Step 3: Decode the buttons**

In `rtl/snac_psx.v`, add above the `always` block:

```verilog
// PSX byte 0: [0] SELECT [1] L3 [2] R3 [3] START [4] UP [5] RIGHT [6] DOWN [7] LEFT
// PSX byte 1: [0] L2 [1] R2 [2] L1 [3] R1 [4] TRIANGLE [5] O [6] X [7] SQUARE
// All active low, hence the inversion into an active-high vector.
function [15:0] decode(input [7:0] b0, input [7:0] b1);
	decode = { ~b0[2], ~b0[1], ~b1[1], ~b1[0],    // R3 L3 R2 L2
	           ~b0[0], ~b0[3], ~b1[3], ~b1[2],    // SELECT START R1 L1
	           ~b1[4], ~b1[7], ~b1[5], ~b1[6],    // TRIANGLE SQUARE O X
	           ~b0[4], ~b0[6], ~b0[7], ~b0[5] };  // UP DOWN LEFT RIGHT
endfunction
```

and in `ST_DONE`, replace the two ID assignments with:

```verilog
		if (port == 1'b0) begin
			id0  <= rx_byte[1];
			pad0 <= decode(rx_byte[3], rx_byte[4]);
		end
		else begin
			id1  <= rx_byte[1];
			pad1 <= decode(rx_byte[3], rx_byte[4]);
		end
```

- [ ] **Step 4: Run the testbench to verify it passes**

Same command. Expected: all five checks `PASS`, `ALL TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add rtl/snac_psx.v rtl/tb/snac_psx_tb.v
git commit -m "feat: decode PSX digital pad buttons"
```

---

### Task 4: DualShock axes and the second port

**Files:**
- Modify: `rtl/snac_psx.v`, `rtl/tb/snac_psx_tb.v`

**Interfaces:**
- Produces: `axes0` / `axes1` as `{LY, LX, RY, RX}`, and `id1` / `pad1` for SNAC port 2.

**Note:** a DualShock only sends axes when the user has pressed its ANALOG button. Forcing
analog mode needs a config-mode command sequence, which is **Plan 2's** problem (the mouse
depends on it). This task reads whatever mode the pad is already in.

- [ ] **Step 1: Add the port-2 pad model and axis assertions**

In `rtl/tb/snac_psx_tb.v`, add a second model and its wiring:

```verilog
wire att2 = user_out[0];
wire dat_p2, ack_p2;
reg [7:0] p2_btn0 = 8'hFF;

psx_pad_model #(.DEVICE_ID(8'h73), .PAYLOAD_BYTES(6)) pad_p2 (
	.att(att2), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p2), .ack(ack_p2),
	.btn0(p2_btn0), .btn1(8'hFF),
	.ax_rx(8'h11), .ax_ry(8'h22), .ax_lx(8'h33), .ax_ly(8'h44)
);
```

Both pads sit on the same wires, and only the selected one drives them — the model releases
`dat`/`ack` high when its ATT is high, so a wired-AND is the correct combination:

```verilog
always @(*) begin
	user_in[4] = dat_p1 & dat_p2;
	user_in[3] = ack_p1 & ack_p2;
end
```

Delete the earlier single-pad version of that `always` block. Then add checks:

```verilog
	// Port 2 carries a DualShock. Axes arrive as {LY,LX,RY,RX}.
	#2000000;
	check("port2 device id", id1, 8'h73);
	check("port2 axes", axes1, 32'h44332211);

	// Port 1 is digital: no axes, so it must stay centred rather than
	// picking up 0xFF padding from the short reply.
	check("port1 axes centred", axes0, 32'h80808080);
```

- [ ] **Step 2: Run to verify the new checks fail**

Expected: `FAIL port2 device id: got 0 (0x0), want 115 (0x73)` and `FAIL port2 axes`.

- [ ] **Step 3: Capture the axes**

In `rtl/snac_psx.v` `ST_DONE`, extend both branches:

```verilog
		if (port == 1'b0) begin
			id0  <= rx_byte[1];
			pad0 <= decode(rx_byte[3], rx_byte[4]);
			// Only a pad in analog mode sends axes; anything else keeps centre,
			// so a digital pad never reads as a stick held hard over.
			axes0 <= (rx_byte[1] == 8'h73)
			         ? { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] }
			         : 32'h80808080;
		end
		else begin
			id1  <= rx_byte[1];
			pad1 <= decode(rx_byte[3], rx_byte[4]);
			axes1 <= (rx_byte[1] == 8'h73)
			         ? { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] }
			         : 32'h80808080;
		end
```

- [ ] **Step 4: Run to verify it passes**

Expected: every check `PASS`, `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add rtl/snac_psx.v rtl/tb/snac_psx_tb.v
git commit -m "feat: DualShock axes and SNAC port 2"
```

---

### Task 5: Absent-pad detection

An unplugged port must report `id == 0x00`, not hang the state machine waiting for a byte
that never comes. Without this the poll loop stalls on an empty port and the other pad dies
with it.

**Files:**
- Modify: `rtl/snac_psx.v`, `rtl/tb/snac_psx_tb.v`

- [ ] **Step 1: Add the assertion**

In `rtl/tb/snac_psx_tb.v`, add a third port state where neither model answers. The simplest
form: force the shared inputs high for a while and check both IDs clear.

```verilog
	// Nothing plugged in: DAT idles high, so every byte reads 0xFF and the
	// device ID must be reported as absent rather than 0xFF.
	force user_in[4] = 1'b1;
	force user_in[3] = 1'b1;
	#4000000;
	check("absent port1", id0, 8'h00);
	check("absent port2", id1, 8'h00);
	release user_in[4];
	release user_in[3];
```

- [ ] **Step 2: Run to verify it fails**

Expected: `FAIL absent port1: got 255 (0xff), want 0 (0x0)` — an idle bus reads as `0xFF`,
which is not a device.

- [ ] **Step 3: Treat 0xFF as absent**

In `rtl/snac_psx.v` `ST_DONE`, wrap the ID assignment:

```verilog
		// An idle bus reads back all ones. 0xFF is not a device ID, so report
		// absence rather than letting a floating bus look like a controller.
		if (port == 1'b0) begin
			id0  <= (rx_byte[1] == 8'hFF) ? 8'h00 : rx_byte[1];
			pad0 <= (rx_byte[1] == 8'hFF) ? 16'h0000 : decode(rx_byte[3], rx_byte[4]);
			axes0 <= (rx_byte[1] == 8'h73)
			         ? { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] }
			         : 32'h80808080;
		end
```

and the mirror image for port 2. Note the button vector must clear too — otherwise
unplugging a pad mid-press leaves that button stuck on forever.

- [ ] **Step 4: Run the full testbench**

Expected: every check `PASS`, `ALL TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add rtl/snac_psx.v rtl/tb/snac_psx_tb.v
git commit -m "feat: report an unplugged SNAC port as absent"
```

---

### Task 6: Menu core integration

> **Rewritten during execution.** The original draft assumed the Menu core's user port was
> free. It is not — `menu.sv` already drives `USER_OUT[1] = UART_RXD` for MIDI and reads
> `USER_IN[2,4,5,6]` for MT32-pi I2S audio, including a straight/crossed cable auto-detect.
> Every one of those collides with the SNAC pin map, so the two are mutually exclusive and
> the port is muxed at runtime on the `snac_psx` setting. The original also specified
> `CLK_MHZ(50)`; `clk_sys` is **100 MHz** here (`rtl/pll/pll_0002.v`:
> `output_clock_frequency0("100.000000 MHz")`), and 50 would have run the bus at 500 kHz.

**Files (in `upstream/core-menu`, a separate repo, branch `feature/snac-psx`):**
- Create: `rtl/snac_psx.v` (copy)
- Modify: `files.qip`, `sys/hps_io.sv`, `menu.sv`

- [x] **Step 1: Fork and clone** — done. `FringeCoder/Menu_MiSTer` forked, cloned to
  `upstream/core-menu` (git-ignored by `.gitignore:7`, `/upstream/*/`), branch
  `feature/snac-psx` created.

- [ ] **Step 2: Copy the reader in and add it to the build**

```bash
cp rtl/snac_psx.v upstream/core-menu/rtl/snac_psx.v
```

Append to `upstream/core-menu/files.qip`, matching that file's existing plain-relative-path
convention (**not** the `[file join $::quartus(qip_path) ...]` form):

```tcl
set_global_assignment -name VERILOG_FILE rtl/snac_psx.v
```

- [ ] **Step 3: Add the UIO read and write to `hps_io`**

In `upstream/core-menu/sys/hps_io.sv`, add two ports beside the existing `status_menumask`
input (line ~122) and `sdram_sz` output (line ~161):

```verilog
	input      [111:0] snac_state,
	output reg         snac_enable = 0,
```

In the command-decode `case` that carries `'h2E` and `'h31` (line ~522), add:

```verilog
				// PSX SNAC: give the user port to the pad reader (1) or leave it
				// to MT32-pi (0). Mutually exclusive -- they share every pin.
				'hFD: if(byte_cnt == 1) snac_enable <= io_din[0];

				// PSX SNAC pad state. Seven words: status, then three per port.
				'hFE: case(byte_cnt)
							1: io_dout <= snac_state[ 15:  0];
							2: io_dout <= snac_state[ 31: 16];
							3: io_dout <= snac_state[ 47: 32];
							4: io_dout <= snac_state[ 63: 48];
							5: io_dout <= snac_state[ 79: 64];
							6: io_dout <= snac_state[ 95: 80];
							7: io_dout <= snac_state[111: 96];
						endcase
```

Both are free: the highest command this file decodes is `'h43`.

- [ ] **Step 4: Instantiate the reader and mux the user port in `menu.sv`**

```verilog
wire [15:0] snac_pad0, snac_pad1;
wire [31:0] snac_axes0, snac_axes1;
wire  [7:0] snac_id0, snac_id1;
wire  [6:0] snac_user_out;

// Word 0 is status (both device IDs), then pad and axes per port. Must match the
// word order userspace reads in snac_psx_poll().
wire [111:0] snac_state = { snac_axes1, snac_pad1, snac_axes0, snac_pad0,
                            snac_id1, snac_id0 };

snac_psx #(.CLK_MHZ(100), .BAUD_KHZ(250)) snac
(
	.clk(clk_sys),
	.reset(~locked),
	.enable(snac_enable),
	.user_in(USER_IN),
	.user_out(snac_user_out),
	.pad0(snac_pad0), .pad1(snac_pad1),
	.axes0(snac_axes0), .axes1(snac_axes1),
	.id0(snac_id0), .id1(snac_id1)
);
```

Then replace the three existing `USER_OUT` assignments (lines ~239-241) with a mux, and
silence the MT32-pi paths while SNAC owns the port — the I2S decoder would otherwise turn
pad traffic into noise on the audio outputs:

```verilog
// The user port has one tenant at a time: PSX pads or MT32-pi. They collide on
// every pin, so this is physics, not policy.
assign USER_OUT = snac_enable ? snac_user_out : {5'b11111, UART_RXD, 1'b1};
assign UART_TXD = snac_enable ? 1'b1 : midi_rx;
assign AUDIO_L  = snac_enable ? 16'd0 : mt32_i2s_l;
assign AUDIO_R  = snac_enable ? 16'd0 : mt32_i2s_r;
```

Fold the existing `AUDIO_L`/`AUDIO_R` right-hand sides into the mux above rather than
duplicating them. The I2S decode and the crossed-cable detect may keep running
unconditionally — they only read `USER_IN`, and reading is harmless once their outputs are
gated.

Pass `.snac_state(snac_state)` to the `hps_io` instantiation.

- [ ] **Step 5: Build and record the fitter time**

```bash
/c/intelFPGA_lite/17.0/quartus/bin64/quartus_sh --flow compile upstream/core-menu/menu.qpf
```

Time it. **Write the measured number into
`docs/superpowers/specs/2026-08-04-psx-snac-design.md` under Open questions**, replacing the
note that it is unmeasured — the plan's sequencing argument rests on it being far shorter
than the AmigaCD core's ~40 minutes.

Expected: compiles with no failing setup paths.

- [ ] **Step 6: Commit**

```bash
git -C upstream/core-menu add rtl/snac_psx.v files.qip sys/hps_io.sv menu.sv
git -C upstream/core-menu commit -m "feat: read PSX pads on the SNAC user port"
```

---

### Task 7: Userspace — the ini key and the poll

**Files:**
- Create: `support/snac/snac_psx.h`, `support/snac/snac_psx.cpp`
- Modify: `user_io.h`, `user_io.cpp`, `cfg.h`, `cfg.cpp`

**Interfaces:**
- Produces: `void snac_psx_poll(void)` and `int snac_psx_enabled(void)`, called from
  `user_io_poll()`. Task 8 adds the OSD-key injection inside `snac_psx_poll()`.

- [ ] **Step 1: Add the UIO command defines**

In `user_io.h`, after `#define UIO_MM2_USRPORT 0xFC` (line 105):

```c
#define UIO_MM2_SNAC    0xFD  // PSX SNAC: port modes / light pen latch (Plan 2)
#define UIO_MM2_SNACRD  0xFE  // PSX SNAC: read pad state
```

- [ ] **Step 2: Add the ini key**

In `cfg.h`, add to `cfg_t` immediately before the closing `} cfg_t;` (line 115):

```c
	uint8_t snac_psx;
```

In `cfg.cpp`, add to `ini_vars[]` immediately before the closing `};` (line 148):

```c
	{ "SNAC_PSX", (void*)(&(cfg.snac_psx)), UINT8, 0, 1 },
```

- [ ] **Step 3: Write the header**

`support/snac/snac_psx.h`:

```c
#ifndef SNAC_PSX_H
#define SNAC_PSX_H

// PSX controllers on the SNAC user port.
//
// The core reads the pads and publishes their state; this reads that state and
// turns it into OSD navigation. Buttons reach the emulated machine in RTL, not
// through here -- see docs/superpowers/specs/2026-08-04-psx-snac-design.md.

// Button bits, matching rtl/snac_psx.v's vector exactly.
#define SNAC_RIGHT   0x0001
#define SNAC_LEFT    0x0002
#define SNAC_DOWN    0x0004
#define SNAC_UP      0x0008
#define SNAC_CROSS   0x0010
#define SNAC_CIRCLE  0x0020
#define SNAC_SQUARE  0x0040
#define SNAC_TRI     0x0080
#define SNAC_L1      0x0100
#define SNAC_R1      0x0200
#define SNAC_START   0x0400
#define SNAC_SELECT  0x0800

// Non-zero when this core is one we read SNAC pads from, and the user asked for it.
int  snac_psx_enabled(void);

// Read pad state and drive the OSD from it. Called once per user_io_poll().
void snac_psx_poll(void);

#endif
```

- [ ] **Step 4: Write the poll**

`support/snac/snac_psx.cpp`:

```c
#include <stdio.h>
#include <string.h>
#include "../../spi.h"
#include "../../user_io.h"
#include "../../cfg.h"
#include "../../menu.h"
#include "snac_psx.h"

static uint16_t pad[2];
static uint8_t  id[2];

// Defined below; snac_psx_poll() calls it and C++ needs the declaration first.
static void snac_osd_keys(void);

int snac_psx_enabled(void)
{
	// Gated on core identity, never on probing the bus: a UIO read to a core
	// with no decode for it returns whatever the bus floats to, and a false
	// positive means phantom menu keys.
	if (!cfg.snac_psx) return 0;
	return is_menu();
}

void snac_psx_poll(void)
{
	if (!snac_psx_enabled()) return;

	// All seven words must be read even though the axes are unused here: the
	// core streams a fixed-length reply, and a short read leaves the next
	// transaction misaligned. Plan 2 is what gives the axes a consumer.
	spi_uio_cmd_cont(UIO_MM2_SNACRD);
	uint16_t status = spi_w(0);
	pad[0] = spi_w(0);
	spi_w(0);              // axes0 low  -- Plan 2
	spi_w(0);              // axes0 high -- Plan 2
	pad[1] = spi_w(0);
	spi_w(0);              // axes1 low  -- Plan 2
	spi_w(0);              // axes1 high -- Plan 2
	DisableIO();

	id[0] = status & 0xFF;
	id[1] = status >> 8;

	// A port with nothing in it reports id 0 and must contribute no buttons,
	// so an unplugged pad cannot hold a menu key down.
	if (!id[0]) pad[0] = 0;
	if (!id[1]) pad[1] = 0;

	snac_osd_keys();
}

// Task 8 fills this in; an empty body keeps this task's build clean and lets the
// poll be verified on hardware before any key injection exists.
static void snac_osd_keys(void)
{
}
```

`spi_w()` is `uint16_t spi_w(uint16_t)` (`spi.h:24`), and reading seven words after
`spi_uio_cmd_cont()` matches the `UIO_GET_OSDMASK` pattern at `user_io.cpp:3778`.

- [ ] **Step 5: Wire it into the poll loop**

In `user_io.cpp`, add the include beside the other support headers:

```c
#include "support/snac/snac_psx.h"
```

and add the call in `user_io_poll()`. The Minimig branch at lines 3216–3234 is AmigaCD's
(Plan 2); this plan needs the Menu core, so place it beside the other unconditional
per-core polls near line 3846, before `if (is_megacd()) mcd_poll();`:

```c
	snac_psx_poll();
```

The function returns immediately unless `snac_psx_enabled()`, so the cost under every other
core is one branch.

- [ ] **Step 6: Build**

```bash
make
```

Expected: builds clean, **zero new warnings**. If the toolchain is unavailable locally,
push and read the `build` job in CI — it is the authority here, as the README records.

- [ ] **Step 7: Commit**

```bash
git add support/snac user_io.h user_io.cpp cfg.h cfg.cpp
git commit -m "feat: read PSX SNAC pad state from the core"
```

---

### Task 8: OSD navigation, and hardware bring-up

**Files:**
- Modify: `support/snac/snac_psx.cpp`, `docs/misterfloppy-hardware.md` (sibling note)
- Create: `docs/psx-snac-hardware.md`

**Interfaces:**
- Consumes: `pad[]` from Task 7, `menu_key_set()` from `menu.h`.

- [ ] **Step 1: Add edge detection and auto-repeat**

Replace the empty `snac_osd_keys()` body from Task 7 with the following, and add the
statics above it. `GetTimer`/`CheckTimer` return `unsigned long` (`hardware.h:6-7`), so the
deadline is typed to match:

```c
// We are not an evdev device, so nothing gives us key repeat -- the repeat a
// keyboard gets comes from EV_REP, which never reaches us. These match how the
// OSD feels under a keyboard.
#define SNAC_REPEAT_DELAY_MS  500
#define SNAC_REPEAT_RATE_MS   100

static uint16_t      prev_pad;
static unsigned long repeat_at;
static uint16_t      repeat_btn;

struct snac_keymap { uint16_t btn; unsigned int key; };

static const snac_keymap osd_keys[] = {
	{ SNAC_UP,     KEY_UP    },
	{ SNAC_DOWN,   KEY_DOWN  },
	{ SNAC_LEFT,   KEY_LEFT  },
	{ SNAC_RIGHT,  KEY_RIGHT },
	{ SNAC_CROSS,  KEY_ENTER },
	{ SNAC_CIRCLE, KEY_ESC   },
};

static void snac_osd_keys(void)
{
	// Both pads drive the menu: whichever one is to hand should work.
	uint16_t now = pad[0] | pad[1];
	uint16_t pressed = now & ~prev_pad;

	// Start+Select opens the OSD, and must not also register as Start.
	if ((now & (SNAC_START | SNAC_SELECT)) == (SNAC_START | SNAC_SELECT))
	{
		if (pressed & (SNAC_START | SNAC_SELECT)) menu_key_set(KEY_F12);
		prev_pad = now;
		return;
	}

	for (unsigned i = 0; i < sizeof(osd_keys) / sizeof(osd_keys[0]); i++)
	{
		if (pressed & osd_keys[i].btn)
		{
			menu_key_set(osd_keys[i].key);
			repeat_btn = osd_keys[i].btn;
			repeat_at  = GetTimer(SNAC_REPEAT_DELAY_MS);
		}
	}

	// Held direction: repeat until released. Buttons deliberately do not repeat.
	if (repeat_btn && (now & repeat_btn) && (repeat_btn & (SNAC_UP | SNAC_DOWN | SNAC_LEFT | SNAC_RIGHT)))
	{
		if (CheckTimer(repeat_at))
		{
			for (unsigned i = 0; i < sizeof(osd_keys) / sizeof(osd_keys[0]); i++)
			{
				if (repeat_btn == osd_keys[i].btn) menu_key_set(osd_keys[i].key);
			}
			repeat_at = GetTimer(SNAC_REPEAT_RATE_MS);
		}
	}
	else if (!(now & repeat_btn)) repeat_btn = 0;

	prev_pad = now;
}
```

Add `#include "../../hardware.h"` for `GetTimer`/`CheckTimer`, and
`#include <linux/input.h>` for the `KEY_*` codes — the same header `input.cpp` uses.

- [ ] **Step 2: Build**

```bash
make
```

Expected: clean, zero new warnings.

- [ ] **Step 3: Commit**

```bash
git add support/snac/snac_psx.cpp
git commit -m "feat: drive the OSD from a PSX pad on SNAC"
```

- [ ] **Step 4: Install and run the hardware checks**

Copy `bin/MiSTer` to `/media/fat/MiSTer` and the built `menu.rbf` to `/media/fat/menu.rbf`.
Add to `/media/fat/MiSTer.ini`:

```ini
snac_psx=1
```

Run these in order and record the result of each:

1. **`SW[1]` is off.** Check the physical slide switch on the DE10-Nano first. It steals
   USER_IO 2/4/5 — `CMD`, `DAT`, `CLK` — and everything below fails silently if it is on.
2. **Digital pad enumerates on port 1.** Plug one in, confirm the menu responds to the
   D-pad.
3. **DualShock enumerates**, in both digital and analog mode (press its ANALOG button).
   Buttons must work in both.
4. **Port 2 works**, with port 1 empty. This is the case that breaks if absent-pad handling
   regressed.
5. **Nothing plugged in: the menu does not move on its own.** Phantom keys are the
   failure this guards.
6. **Start+Select opens the OSD**; directions repeat at a comfortable rate; ✕ enters, ○ goes
   back; a core can be launched with no USB device attached at all.

- [ ] **Step 5: Write the hardware note**

Create `docs/psx-snac-hardware.md` recording: the pin map table from this plan, that `SW[1]`
must be off and why, the measured Menu core fitter time from Task 6, that `update_all`
overwrites `menu.rbf` and it must be re-applied, and anything learned during Step 4 that
would cost the next person time. Follow the shape of `docs/misterfloppy-hardware.md` —
cable/wiring gotcha first, because that is what costs hours.

- [ ] **Step 6: Commit and open the PR**

```bash
git add docs/psx-snac-hardware.md
git commit -m "docs: PSX SNAC hardware notes"
gh pr create --base develop --title "PSX SNAC: reader and menu navigation" --body "Plan 1 of docs/superpowers/specs/2026-08-04-psx-snac-design.md"
```

---

## What this plan deliberately does not do

Each of these is Plan 2. If a task above appears to need one, the task is wrong.

- Any change to `upstream/core-cd32-cdtv`.
- Joystick injection into `JOY0`/`JOY1`, CD32 pad mapping.
- Widening `user_port_mode` past one bit, the 3-way `USER_OUT` mux.
- The analog stick as an Amiga mouse, and the DualShock analog-mode init sequence it needs.
- GunCon decoding, csync on `USER_OUT[6]`, the light pen in `agnus_beamcounter.v`.
- AmigaCD OSD rows and the third config layout.
