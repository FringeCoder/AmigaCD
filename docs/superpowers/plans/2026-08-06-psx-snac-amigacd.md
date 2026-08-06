# PSX SNAC — Plan 2: the AmigaCD core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the hardware-proven PSX pad reader into the AmigaCD core, so a PlayStation
pad drives the CD32 controller at SNAC latency, its analog stick drives an Amiga mouse, and
a GunCon drives the Amiga light pen.

**Architecture:** `snac_psx.v` — merged and verified on hardware in Plan 1 — is instantiated
in `Minimig.sv` at the Amiga's 28.6875 MHz. Its button vectors are reshaped to CD32 order
and OR'd into `JOY0`/`JOY1`; the analog axes and gun coordinates go to the HPS over the
existing UIO read and come back as mouse motion and a light-pen latch. The user port becomes
a three-way mux: MT32-pi, MiSTer Floppy, or PSX SNAC.

**Tech Stack:** Verilog-2005 / SystemVerilog (Quartus Prime Lite 17.0), Icarus Verilog
(simulation, in CI), C++14 (arm-none-linux-gnueabihf 10.2), GitHub Actions, MiSTer DE10-Nano.

## Global Constraints

- **PRs target `develop`.** Always `gh pr create --base develop`; the repo default is `master`.
- **Spec:** `docs/superpowers/specs/2026-08-04-psx-snac-design.md`, Sequencing steps 4–7.
- **Core branch is `feature/misterfloppy`** in `upstream/core-cd32-cdtv`. Do not move off it.
- **CONF_STR must stay `"AmigaCD;..."`** — `is_amigacd()` prefix-matches it, and the console
  OSD, per-game NVRAM and physical-disc paths all depend on that resolving.
- **Every AmigaCD RTL change costs a ~40 minute Quartus run.** Batch RTL edits ruthlessly.
  This plan is structured around exactly **two** fitter runs; adding a third is a planning
  failure, not a fact of life.
- **Timing gate is binary.** Quartus must report no failing setup paths. Baseline for
  `feature/misterfloppy` is **+0.177 ns** worst-case slack at 23,555 ALMs, stock fitter
  settings (`docs/upstream-pins.md:105-108`). A failing path means re-run once; if it
  reproduces, stop — do not tune around it.
- **Zero new compiler warnings**, gcc and iverilog alike. Both are enforced in CI.
- **No C++ test framework exists and none is being added.** C++ verification is (a) CI
  cross-build green, (b) a named hardware check.
- **`rtl/snac_psx.v` is a published interface** shared with the Menu core fork. Changing its
  ports or parameters means updating `upstream/core-menu` in lockstep and re-fitting it.
- **`SW[1]` on the DE10-Nano must be off** for any hardware check — it steals USER_IO 2/4/5
  (`CMD`, `DAT`, `CLK`) and fails silently.
- **The user port has one tenant at a time.** PSX pads, MT32-pi and MiSTer Floppy collide on
  every pin. Mutual exclusion is physics, not policy.

## Facts established before writing this plan

Verified by reading the tree, not assumed:

| Fact | Source |
|---|---|
| Minimig `clk_sys` is **28.6875 MHz** | `Minimig.sv:375` — `.clk_28 (clk_sys), // input clock c1 ( 28.687500MHz)` |
| `user_port_mode` is 1 bit, originating in `userio.v` | `rtl/userio.v:59` (`output reg`), `:472` (`IO_DIN[0]`) |
| It reaches the top through `minimig.v` | `rtl/minimig.v:343`, and gates the floppy at `:628` |
| `USER_OUT` is a 2-way mux | `Minimig.sv:28` |
| Joysticks arrive as `JOY0..JOY3` from `hps_io` | `Minimig.sv:102-107`, consumed at `:984-987` |
| Video sync is available as `hs` / `vs`, active low | `Minimig.sv:1008-1009` |
| CD32 pad shift registers already consume joystick bits 4–10 | `rtl/userio.v:210-230` |
| Light-pen latch is stubbed out, not absent | `rtl/denise.v:131,154`; `rtl/agnus_beamcounter.v:126` |
| `VPOSR`/`VHPOSR` read paths already exist at 280 ns resolution | `rtl/agnus_beamcounter.v:104-109` |

**Bus rate at 28.6875 MHz:** `HALF = CLK_KHZ / (BAUD_KHZ*2) = 28688/500 = 57` (integer
division), giving 28 687 500 / (2×57) = **251.6 kHz**, 0.6% above nominal. Well inside a
PSX pad's tolerance, and the reason `CLK_MHZ` was renamed to `CLK_KHZ` in Plan 1 — at
integer MHz this would have been 28 MHz and 2.5% out.

## Two decisions this plan makes

**1. The two cores use different commands, and the merged Menu core is not touched.**
Plan 1 shipped `UIO_MM2_SNAC` (0xFD) as a one-bit enable that the *merged, hardware-verified*
Menu core reads as `io_din[0]`. Changing that would force a re-verification of working
hardware for no gain.

So AmigaCD does not use 0xFD at all. It extends `UIO_MM2_USRPORT` (0xFC), which already
carries the tenant, from 8 bits to 16:

```
bits  1:0 : tenant -- 0 MT32-pi, 1 MiSTer Floppy, 2 PSX SNAC   (was the whole payload)
bits 10:8 : SNAC port 1 mode
bits 13:11: SNAC port 2 mode
```

Values 0 and 1 keep their meaning, so no config migration is needed for the tenant itself,
and `spi_uio_cmd8` simply becomes `spi_uio_cmd16`. The Menu core keeps 0xFD, unchanged and
unre-verified. Two cores, two commands, no shared payload to drift.

**2. DualShock analog mode is entered by the user pressing the pad's ANALOG button.** The
alternative — the master sending the `0x43`/`0x44`/`0x4F` config sequence to force it — is
real RTL work in a module shared with another core, and would need its own testbench, its
own hardware verification, and a re-fit of both cores. The stick-as-mouse feature is
useless without analog mode either way, so the cost is one button press, documented. If
bring-up shows pads that will not stay in analog mode, forcing it becomes its own task with
its own fitter run — deliberately not smuggled into this plan.

## File Structure

**Canonical repo (`AmigaCD`) — new:**

| File | Responsibility |
|---|---|
| `rtl/snac_cd32.v` | Pure combinational reshape: PSX button vector → CD32-ordered joystick vector |
| `rtl/tb/snac_cd32_tb.v` | Exhaustive testbench for that mapping — all 16 bits, both directions |

**Canonical repo — modified:**

| File | Change |
|---|---|
| `.github/workflows/build.yml` | Add `snac_cd32` to the `rtl-sim` job |
| `support/minimig/minimig_config.h` | `mm_snacPortMode` enum, `mmup_snacpsx`, config fields |
| `support/minimig/minimig_config.cpp` | Third on-disk layout + migration, `minimig_ConfigSnac()` |
| `support/snac/snac_psx.{cpp,h}` | Axes stored again, mouse conversion, light-pen latch write, AmigaCD gating |
| `menu.cpp` | Advanced page: 3-way User Port row, two per-port rows, dynamic `menumask` |
| `user_io.cpp` | Call `snac_psx_poll()` for AmigaCD as well as the Menu core |
| `user_io.h` | `UIO_MM2_LPEN` command |
| `docs/psx-snac-hardware.md` | Plan 2 bring-up checks |

**Core repo (`upstream/core-cd32-cdtv` @ `feature/misterfloppy`) — modified:**

| File | Change |
|---|---|
| `rtl/snac_psx.v`, `rtl/snac_cd32.v` | Copies of ours |
| `Minimig.sv` | Instantiate reader + mapper, 3-way `USER_OUT` mux, csync, JOY injection |
| `rtl/minimig.v` | `user_port_mode` widened to `[1:0]`, light-pen trigger plumbing |
| `rtl/userio.v` | `user_port_mode` widened, SNAC mode decode |
| `rtl/agnus_beamcounter.v` | Light-pen capture registers, read mux, `BPLCON0` bit-3 decode |
| `Minimig.qsf` or `files.qip` | Add the two new RTL files |

---

### Task 1: CD32 button mapping, with an exhaustive test

The reader emits a PSX-native 16-bit vector. Minimig's CD32 pad shift registers expect
joystick bits 4–10 in CD32 order. This task is the reshape and nothing else — pure
combinational logic, fully testable in simulation, no fitter run.

**Files:**
- Create: `rtl/snac_cd32.v`, `rtl/tb/snac_cd32_tb.v`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Consumes: the `snac_psx` button vector — `[0] RIGHT [1] LEFT [2] DOWN [3] UP [4] CROSS
  [5] CIRCLE [6] SQUARE [7] TRIANGLE [8] L1 [9] R1 [10] START [11] SELECT [12] L2 [13] R2
  [14] L3 [15] R3`, active high.
- Produces: `snac_cd32` module, `output [10:0] joy`, in MiSTer/Minimig joystick order.

**The target order, derived from `rtl/userio.v:213`:**

```verilog
cd32pad1_reg <= {_djoy1[5], _djoy1[4], _djoy1[6], _djoy1[7], _djoy1[8], _djoy1[9], _djoy1[10], 1'b1};
```

That register shifts out MSB first, and the CD32 pad protocol sends Blue, Red, Yellow,
Green, Forward, Reverse, Play. Matching them up — and cross-checked against
`rtl/userio.v:334`, where bit 4 is the plain-joystick fire, which on a CD32 pad is Red:

| joy bit | CD32 button | PSX button |
|---|---|---|
| 0–3 | Right, Left, Down, Up | D-pad |
| 4 | **Red** (fire) | ✕ Cross |
| 5 | **Blue** | ○ Circle |
| 6 | **Yellow** | □ Square |
| 7 | **Green** | △ Triangle |
| 8 | **Forward** (right shoulder) | R1 |
| 9 | **Reverse** (left shoulder) | L1 |
| 10 | **Play/Pause** | Start |

**This table is a hypothesis until hardware confirms it.** It is derived, not documented,
and Task 7 checks every button in a real game. If it is wrong it will be wrong in a
self-consistent way that simulation cannot detect — only a human pressing ✕ and seeing Red
can settle it.

- [ ] **Step 1: Write the failing test**

`rtl/tb/snac_cd32_tb.v`:

```verilog
`timescale 1ns/1ns

module snac_cd32_tb;

reg  [15:0] psx;
wire [10:0] joy;
integer errors = 0;

snac_cd32 dut (.psx(psx), .joy(joy));

task check(input [255:0] name, input [15:0] in, input [10:0] want);
begin
	psx = in;
	#1;
	if (joy !== want) begin
		$display("FAIL %0s: psx=%04h -> joy=%03h, want %03h", name, in, joy, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	check("nothing",   16'h0000, 11'h000);
	check("right",     16'h0001, 11'h001);
	check("left",      16'h0002, 11'h002);
	check("down",      16'h0004, 11'h004);
	check("up",        16'h0008, 11'h008);
	check("cross->red",     16'h0010, 11'h010);
	check("circle->blue",   16'h0020, 11'h020);
	check("square->yellow", 16'h0040, 11'h040);
	check("triangle->green",16'h0080, 11'h080);
	check("L1->reverse",    16'h0100, 11'h200);
	check("R1->forward",    16'h0200, 11'h100);
	check("start->play",    16'h0400, 11'h400);

	// Buttons with no CD32 equivalent must not leak into any joystick bit.
	check("select ignored", 16'h0800, 11'h000);
	check("L2 ignored",     16'h1000, 11'h000);
	check("R2 ignored",     16'h2000, 11'h000);
	check("L3 ignored",     16'h4000, 11'h000);
	check("R3 ignored",     16'h8000, 11'h000);

	// Combinations must not interfere.
	check("all mapped", 16'h07FF, 11'h7FF);
	check("all buttons", 16'hFFFF, 11'h7FF);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 2: Create a stub so it compiles and fails behaviourally**

`rtl/snac_cd32.v`:

```verilog
`timescale 1ns/1ns

// Reshape a PSX-native button vector into Minimig's joystick order, so the
// CD32 pad shift registers in userio.v consume it unchanged.
//
// Pure combinational: no clock, no state. Its whole job is a wiring decision
// that is easy to get wrong and impossible to spot at runtime.
module snac_cd32 (
	input  [15:0] psx,
	output [10:0] joy
);

assign joy = 11'h000;

endmodule
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
mkdir -p build && /c/iverilog/bin/iverilog -g2005 -Wall -o build/snac_cd32_tb rtl/snac_cd32.v rtl/tb/snac_cd32_tb.v && /c/iverilog/bin/vvp build/snac_cd32_tb
```

Expected: `PASS nothing`, then `FAIL right`, `FAIL left`, … and `18 FAILURE(S)`, non-zero
exit. The "nothing"/"ignored" checks pass trivially against a stub that outputs zero — that
is expected and is why the other 18 exist.

- [ ] **Step 4: Implement the mapping**

Replace the `assign` in `rtl/snac_cd32.v` with:

```verilog
// Directions pass straight through; buttons are reordered to CD32's.
// Bit 4 is the plain-joystick fire (userio.v:334), which on a CD32 pad is Red,
// so Cross lands there -- the button under the thumb in both worlds.
assign joy = { psx[10],   // [10] Play/Pause  <- Start
               psx[8],    //  [9] Reverse     <- L1
               psx[9],    //  [8] Forward     <- R1
               psx[7],    //  [7] Green       <- Triangle
               psx[6],    //  [6] Yellow      <- Square
               psx[5],    //  [5] Blue        <- Circle
               psx[4],    //  [4] Red / fire  <- Cross
               psx[3],    //  [3] Up
               psx[2],    //  [2] Down
               psx[1],    //  [1] Left
               psx[0] };  //  [0] Right
```

- [ ] **Step 5: Run the test to verify it passes**

Same command. Expected: all 20 checks `PASS`, `ALL TESTS PASSED`, exit 0, zero warnings.

- [ ] **Step 6: Add it to CI**

In `.github/workflows/build.yml`, in the `rtl-sim` job, add a build and run alongside the
existing `snac_psx_tb` steps:

```yaml
      - name: Build CD32 mapping testbench
        run: iverilog -g2005 -Wall -o build/snac_cd32_tb rtl/snac_cd32.v rtl/tb/snac_cd32_tb.v

      - name: Run CD32 mapping testbench
        run: vvp build/snac_cd32_tb
```

- [ ] **Step 7: Commit**

```bash
git add rtl/snac_cd32.v rtl/tb/snac_cd32_tb.v .github/workflows/build.yml
git commit -m "feat: map PSX buttons to CD32 pad order"
```

---

### Task 2: Core RTL — mode widening, reader, injection, csync

Everything the AmigaCD core needs for **pad input**, in one batch, ending in the first of
this plan's two fitter runs. The light pen is deliberately not here: it touches the beam
counter, and mixing it with this would make a timing regression impossible to attribute.

**Files (in `upstream/core-cd32-cdtv` @ `feature/misterfloppy`):**
- Create: `rtl/snac_psx.v`, `rtl/snac_cd32.v` (copies)
- Modify: `rtl/userio.v`, `rtl/minimig.v`, `Minimig.sv`, `files.qip`

**Interfaces:**
- Consumes: `snac_psx` (`CLK_KHZ`, `BAUD_KHZ`, `enable`, `user_in`, `user_out`, `pad0/1`,
  `axes0/1`, `id0/1`) and `snac_cd32` (`psx`, `joy`) from Task 1 and Plan 1.
- Produces: `user_port_mode[1:0]` where 0 = MT32-pi, 1 = MiSTer Floppy, 2 = PSX SNAC.

- [ ] **Step 1: Copy both modules in and add them to the build**

```bash
cp rtl/snac_psx.v rtl/snac_cd32.v upstream/core-cd32-cdtv/rtl/
```

Add to `upstream/core-cd32-cdtv/files.qip`, matching that file's existing line format
(read it first — do not assume it matches the Menu core's):

```tcl
set_global_assignment -name VERILOG_FILE rtl/snac_psx.v
set_global_assignment -name VERILOG_FILE rtl/snac_cd32.v
```

- [ ] **Step 2: Widen `user_port_mode` in `userio.v`**

At `rtl/userio.v:59`, change the port declaration:

```verilog
	output reg  [1:0] user_port_mode,	// 0 = MT32-pi, 1 = MiSTer Floppy, 2 = PSX SNAC
```

At `rtl/userio.v:472`, take two bits instead of one:

```verilog
				if (userport_cfg_sel)   user_port_mode         <= IO_DIN[1:0];
```

Add a SNAC mode register beside it, decoded from the same command's upper bits (see the
payload layout in "Two decisions this plan makes"):

```verilog
reg [5:0] snac_mode;   // {port2[2:0], port1[2:0]}
```

and inside the same `if (userport_cfg_sel)` block:

```verilog
				if (userport_cfg_sel)   snac_mode              <= IO_DIN[13:8];
```

Declare `output reg [5:0] snac_mode` in the port list so it can reach the top level.

- [ ] **Step 3: Widen the signal through `minimig.v`**

At `rtl/minimig.v:343`:

```verilog
	output  [1:0] user_port_mode,
	output  [5:0] snac_mode,
```

At `rtl/minimig.v:628`, the floppy enable becomes an explicit comparison — a bare
`user_port_mode` would now be a 2-bit value where a 1-bit signal is expected, which Quartus
accepts silently by truncation:

```verilog
	.enable_mister_floppy(user_port_mode == 2'd1),
```

At `rtl/minimig.v:667`, pass both through to `userio`.

- [ ] **Step 4: Instantiate the reader and mapper in `Minimig.sv`**

Replace the declarations at `Minimig.sv:24-28` with:

```verilog
wire  [1:0] user_port_mode;              // 0 = MT32-pi, 1 = MiSTer Floppy, 2 = PSX SNAC
wire  [5:0] snac_mode;                   // {port2[2:0], port1[2:0]}
wire  [2:0] mister_floppy_status;        // {cable, drive type, detected}
wire  [6:0] IndirectUserOutmt32;
wire  [6:0] IndirectUserOutFlop;
wire  [6:0] IndirectUserOutSnac;

// One tenant at a time -- they collide on every user-port pin.
assign USER_OUT = (user_port_mode == 2'd2) ? IndirectUserOutSnac :
                  (user_port_mode == 2'd1) ? IndirectUserOutFlop :
                                             IndirectUserOutmt32;
```

Add near the joystick wiring:

```verilog
wire [15:0] snac_pad0, snac_pad1;
wire [31:0] snac_axes0, snac_axes1;
wire  [7:0] snac_id0, snac_id1;
wire [10:0] snac_joy0, snac_joy1;
wire  [6:0] snac_user_out_raw;

snac_psx #(.CLK_KHZ(28688), .BAUD_KHZ(250)) snac
(
	.clk(clk_sys),
	.reset(reset),
	.enable(user_port_mode == 2'd2),
	.user_in(USER_IN),
	.user_out(snac_user_out_raw),
	.pad0(snac_pad0), .pad1(snac_pad1),
	.axes0(snac_axes0), .axes1(snac_axes1),
	.id0(snac_id0), .id1(snac_id1)
);

snac_cd32 snac_map0 (.psx(snac_pad0), .joy(snac_joy0));
snac_cd32 snac_map1 (.psx(snac_pad1), .joy(snac_joy1));

// A GunCon derives its timing from the displayed raster, so it needs composite
// sync fed back. hs/vs are active low (Minimig.sv:1008-1009), so a wired-AND is
// the classic composite. Only driven when a port is actually in light-gun mode;
// otherwise the reader's own idle level goes out.
wire snac_lightgun = (snac_mode[2:0] == 3'd4) || (snac_mode[5:3] == 3'd4);
assign IndirectUserOutSnac = { snac_lightgun ? (hs & vs) : snac_user_out_raw[6],
                               snac_user_out_raw[5:0] };
```

- [ ] **Step 5: Inject the pads into the joystick vectors**

At `Minimig.sv:102-105`, the `hps_io` instantiation currently passes `JOY0`..`JOY3`
straight through. Leave `hps_io` alone and combine downstream instead, so a USB pad keeps
working alongside a SNAC pad. Add after the joystick wires are declared:

```verilog
// OR rather than replace: a USB pad and a SNAC pad can both be connected, and
// locking one out would be a surprise. Bits are active high here; they are
// inverted at the minimig instantiation below.
wire [15:0] JOY0_MUX = JOY0 | {5'd0, snac_joy0};
wire [15:0] JOY1_MUX = JOY1 | {5'd0, snac_joy1};
```

Then at `Minimig.sv:984-985`, use them:

```verilog
	._joy1        (~JOY0_MUX        ), // joystick 1 [fire4,fire3,fire2,fire,up,down,left,right] (default mouse port)
	._joy2        (~JOY1_MUX        ), // joystick 2 [fire4,fire3,fire2,fire,up,down,left,right] (default joystick port)
```

Check the declared width of `JOY0` at `Minimig.sv:102` before writing this and match it —
if it is not 16 bits, adjust the concatenation so the widths line up exactly rather than
letting Quartus pad or truncate silently.

- [ ] **Step 6: Reset mt32-pi when the tenant changes**

`Minimig.sv:1420-1421` already resets mt32-pi on any change of `user_port_mode`. Confirm
that comparison still works now the signal is 2 bits — `last_userport_mode` must be widened
to `[1:0]` too, or the comparison truncates and a 0→2 switch will not reset mt32-pi.

- [ ] **Step 7: Build, and record the numbers**

```bash
/c/intelFPGA_lite/17.0/quartus/bin64/quartus_sh --flow compile upstream/core-cd32-cdtv/Minimig.qpf
```

Expected: Fitter successful, **no failing setup paths**. Record duration, worst-case setup
slack and ALM count. Baseline to beat: +0.177 ns, 23,555 ALMs.

If any setup path fails: re-run once (17 ps of margin is thin enough that fitter placement
alone can tip it). If it reproduces, **stop and report** — do not tune settings.

- [ ] **Step 8: Commit**

```bash
git -C upstream/core-cd32-cdtv add rtl/snac_psx.v rtl/snac_cd32.v rtl/userio.v rtl/minimig.v Minimig.sv files.qip
git -C upstream/core-cd32-cdtv commit -m "feat: read PSX pads on the SNAC user port"
```

---

### Task 3: Userspace — modes, config migration, OSD rows

**Files:**
- Modify: `support/minimig/minimig_config.h`, `support/minimig/minimig_config.cpp`,
  `menu.cpp`, `user_io.cpp`, `support/snac/snac_psx.{cpp,h}`

**Interfaces:**
- Consumes: `user_port_mode[1:0]` and the `snac_mode[5:0]` payload from Task 2.
- Produces: `void minimig_ConfigSnac(void)`, `mm_snacPortMode`, `mmup_snacpsx`.

- [ ] **Step 1: Add the mode enum and config fields**

In `support/minimig/minimig_config.h`, extend the existing enum at line 49:

```c
enum mm_userportMode {
	mmup_mp32pi       = 0,
	mmup_misterfloppy = 1,
	mmup_snacpsx      = 2
};

// What a SNAC port drives on the Amiga side. Pad and mouse are independent
// signals in userio.v, so "both" is not a compromise -- it is the classic
// Amiga arrangement of mouse in port 1, joystick in port 2.
enum mm_snacPortMode {
	mmsp_off       = 0,
	mmsp_pad       = 1,
	mmsp_mouse     = 2,
	mmsp_pad_mouse = 3,
	mmsp_lightgun  = 4
};
```

Append to `mm_configTYPE`, after `userport`:

```c
	unsigned char   snac[2];   // mm_snacPortMode per SNAC port
```

- [ ] **Step 2: Add the third on-disk layout**

`support/minimig/minimig_config.cpp` already carries `mm_configTYPE_NoExtFloppy` (lines
59–79) as a frozen snapshot of the pre-MiSTer-Floppy layout. Add a second snapshot of
**today's** layout — the comment at line 41 explains why these must be frozen structs and
never aliases of the live type:

```c
// The config layout as it was before PSX SNAC, i.e. every AmigaCD.cfg written
// between the MiSTer Floppy change and this one. snac[2] went onto the end, so
// a file of this size loaded into the new struct would leave snac[] as garbage
// rather than misaligning -- but the size check is what tells the two apart,
// and without this struct there is nothing to compare against.
typedef struct
{
	char             id[8];
	unsigned short   version;
	unsigned short   ext_cfg2;
	char             kickstart[992];
	char             label[32];
	unsigned short   ext_cfg;
	unsigned char    memory;
	unsigned char    chipset;
	mm_floppyTYPE    floppy;
	unsigned char    disable_ar3;
	unsigned char    ide_cfg;
	unsigned char    scanlines;
	unsigned char    audio;
	mm_hardfileTYPE  hardfile[4];
	unsigned char    cpu;
	unsigned char    autofire;
	char             info[64];
	mm_userportMode  userport;
} mm_configTYPE_NoSnac;
```

In `minimig_cfg_load`, add a branch for `size == sizeof(mm_configTYPE_NoSnac)` that copies
field by field and then sets both ports off:

```c
			minimig_config.snac[0] = mmsp_off;
			minimig_config.snac[1] = mmsp_off;
```

- [ ] **Step 3: Send the config to the core**

In `support/minimig/minimig_config.cpp`, beside `minimig_ConfigUserPort()` (line 1191):

```c
// One command carries both the tenant and the per-port modes, so they can never
// be set inconsistently. Bits 1:0 keep their old meaning, which is why widening
// the payload needs no config migration for the tenant itself.
void minimig_ConfigSnac(void)
{
	unsigned char p1 = minimig_config.snac[0] & 0x07;
	unsigned char p2 = minimig_config.snac[1] & 0x07;
	uint16_t param = (uint16_t)((p2 << 11) | (p1 << 8) | (minimig_config.userport & 0x03));
	spi_uio_cmd16(UIO_MM2_USRPORT, param);
}
```

Replace the `minimig_ConfigUserPort()` call in the config-apply path (line 717) with
`minimig_ConfigSnac()`, and delete `minimig_ConfigUserPort()` — it is now a strict subset
and two functions writing the same register is how they drift apart.

- [ ] **Step 4: Gate the poll on AmigaCD too**

In `support/snac/snac_psx.cpp`, `snac_psx_enabled()` currently returns `is_menu()`. Extend
it, and store the axes this time — Plan 1 deliberately discarded them because nothing
consumed them:

```c
int snac_psx_enabled(void)
{
	if (is_menu()) return cfg.snac_psx;
	// AmigaCD asks the core, not the ini: the modes live in AmigaCD.cfg so they
	// can differ per saved configuration.
	if (is_amigacd()) return minimig_config.userport == mmup_snacpsx;
	return 0;
}
```

Restore `static uint32_t axes[2];` and assign it from the two axis words per port.

- [ ] **Step 5: Add the OSD rows**

In `menu.cpp`, `MENU_AMIGACD_ADVANCED1` (around line 6980). The `User Port:` row becomes a
3-way cycle, and two port rows appear only in SNAC mode. **`menumask` stops being the
constant `0x7` at line 6985** and the Back row's `menusub` index moves:

```c
		int snac = (minimig_config.userport == mmup_snacpsx);
		menumask = snac ? 0x1F : 0x7;
```

Draw the rows:

```c
		strcpy(s, " User Port: ");
		strcat(s, (minimig_config.userport == mmup_snacpsx)      ? "PSX SNAC" :
		          (minimig_config.userport == mmup_misterfloppy) ? "MiSTer Floppy" : "MT32-Pi");
		OsdWrite(m++, s, menusub == 0, 0);

		if (snac)
		{
			static const char *modes[] = { "Off", "Pad", "Mouse", "Pad + Mouse", "Light Gun" };
			for (int p = 0; p < 2; p++)
			{
				sprintf(s, "   Port %d  : %s", p + 1, modes[minimig_config.snac[p] & 0x07]);
				OsdWrite(m++, s, menusub == (1 + p), 0);
			}
		}
```

In the `MENU_AMIGACD_ADVANCED2` handler, the existing `menusub == 0/1/2` tests are literal
and must become mode-dependent. Done carelessly, "Back" reaches the Ethernet handler.

- [ ] **Step 6: Build**

```bash
make
```

Expected: clean, zero new warnings. No local ARM toolchain — push and read the `build` job
in CI, which is the authority here.

- [ ] **Step 7: Commit**

```bash
git add support/minimig menu.cpp user_io.cpp support/snac
git commit -m "feat: AmigaCD OSD rows and config for PSX SNAC"
```

---

### Task 4: The analog stick as an Amiga mouse

**Files:**
- Modify: `support/snac/snac_psx.cpp`

**Interfaces:**
- Consumes: `axes[2]` stored in Task 3, `minimig_config.snac[]`.

- [ ] **Step 1: Convert stick deflection to mouse motion**

Add to `support/snac/snac_psx.cpp`, called from `snac_psx_poll()` after the read:

```c
// Stick deflection is a velocity, not a position: subtract centre, apply a
// deadzone, scale, and accumulate the fraction so slow movement does not
// quantise to zero. The pointer tolerates a frame of lag where buttons do not,
// which is why this half lives here and the buttons live in RTL.
#define SNAC_MOUSE_DEADZONE 24
#define SNAC_MOUSE_DIV      12

static int mouse_frac_x, mouse_frac_y;

static int snac_axis_delta(uint8_t v, int *frac)
{
	int d = (int)v - 128;
	if (d > -SNAC_MOUSE_DEADZONE && d < SNAC_MOUSE_DEADZONE) return 0;
	*frac += d;
	int out = *frac / SNAC_MOUSE_DIV;
	*frac -= out * SNAC_MOUSE_DIV;
	return out;
}

static void snac_mouse(void)
{
	for (int p = 0; p < 2; p++)
	{
		unsigned char mode = minimig_config.snac[p] & 0x07;
		if (mode != mmsp_mouse && mode != mmsp_pad_mouse) continue;
		if (id[p] != 0x73) continue;   // axes are only valid in analog mode

		// axes are {LY, LX, RY, RX}; the left stick is the pointer.
		uint8_t lx = (axes[p] >> 8) & 0xFF;
		uint8_t ly = (axes[p] >> 0) & 0xFF;

		int dx = snac_axis_delta(lx, &mouse_frac_x);
		int dy = snac_axis_delta(ly, &mouse_frac_y);
		if (dx || dy) user_io_mouse(0, dx, dy, 0);
	}
}
```

Check the axis packing against `Minimig.sv`'s `snac_state` concatenation before trusting
the shifts above — the reader documents `axes` as `{LY, LX, RY, RX}`, so `LY` is the low
byte and `LX` the next. Getting this wrong swaps or inverts pointer axes, which is obvious
on hardware and invisible here.

- [ ] **Step 2: Build and commit**

```bash
make && git add support/snac/snac_psx.cpp && git commit -m "feat: analog stick as an Amiga mouse"
```

---

### Task 5: The light pen

The second and final fitter run. `agnus_beamcounter.v` already has the read paths and the
`lpendis` stub; this adds capture registers, a read mux and a `BPLCON0` decode, and touches
**none** of the counter increment logic.

**Files:**
- Modify: `upstream/core-cd32-cdtv/rtl/agnus_beamcounter.v`, `rtl/minimig.v`, `Minimig.sv`
- Modify: `user_io.h`, `support/snac/snac_psx.cpp`

- [ ] **Step 1: Add the capture registers and read mux**

In `rtl/agnus_beamcounter.v`, uncomment line 126 and add:

```verilog
wire lpendis      = beamcon0_reg[13];

// BPLCON0 bit 3 (LPEN) enables the light-pen latch. Agnus does not decode
// BPLCON0 today, but the register write bus is already here for BEAMCON0.
parameter BPLCON0 = 9'h100;
reg lpen_en;
always @(posedge clk)
	if (reg_address_in[8:1] == BPLCON0[8:1]) lpen_en <= data_in[3];

// Loaded directly by userspace rather than by beam coincidence: the gun
// reports an absolute position, and converting it here would need a comparator
// and would inherit any mismatch between our timing and the CRT's.
reg [10:0] lpen_vpos;
reg  [8:0] lpen_hpos;
```

Then change the two read expressions at lines 106–109 to select the latched values when the
light pen is enabled:

```verilog
	if (reg_address_in[8:1]==VPOSR[8:1] || reg_address_in[8:1]==VPOSW[8:1])
		data_out[15:0] = {long_frame,1'b0,ecs,ntsc,2'b00,{2{aga}},long_line,4'b0000,
		                  (lpen_en & ~lpendis) ? lpen_vpos[10:8] : vpos[10:8]};
	else if (reg_address_in[8:1]==VHPOSR[8:1] || reg_address_in[8:1]==VHPOSW[8:1])
		data_out[15:0] = (lpen_en & ~lpendis) ? {lpen_vpos[7:0], lpen_hpos[8:1]}
		                                      : {vpos[7:0], hpos[8:1]};
```

**The mux must be a pure pass-through unless `lpen_en & ~lpendis`.** Verify a normal game
still runs before testing any gun — a mistake here breaks every title that reads the beam
position, which is most of them.

- [ ] **Step 2: Decode the latch write in `userio.v`**

The latch is loaded by userspace, not by beam coincidence, so it arrives as a UIO write like
every other MM2 command. In `rtl/userio.v`, beside `userport_cfg_sel` (line 435):

```verilog
wire lpen_cfg_sel = (cmd[3:0] == 15);   // UIO_MM2_LPEN, 0xFF
```

and in the same decode block as line 472:

```verilog
				if (lpen_cfg_sel)     {lpen_vpos, lpen_hpos} <= {IO_DIN[15:8], IO_DIN[7:0]};
```

Two 16-bit words are needed for 11 + 8 bits, so send `vpos` first then `hpos`, latching on
the second — or widen to a single word by dropping `vpos[10:8]`, which only matter above
line 255 and are outside any light-gun-usable display. **Take the two-word form**: the
narrow one silently breaks on tall PAL screens.

Declare `output reg [10:0] lpen_vpos` and `output reg [8:0] lpen_hpos` in the port list, and
pass both through `rtl/minimig.v` to `agnus`, following the `floppy_ext_drive` path added
for MiSTer Floppy — that is the closest existing example of a userspace-written value
reaching deep into the core.

- [ ] **Step 3: Add the UIO command**

In `user_io.h`, after `UIO_MM2_SNACRD`:

```c
#define UIO_MM2_LPEN    0xFF  // PSX SNAC: light-pen latch, vpos then hpos
```

- [ ] **Step 4: Convert gun coordinates in userspace**

**The GunCon reply layout is confirmed** from `MiSTer-devel/PSX_MiSTer`'s
`rtl/joypad_pad.vhd`: device ID `0x63`, six payload bytes in the order buttons-LSB,
buttons-MSB, X-LSB, X-MSB, Y-LSB, Y-MSB. X is 9 bits in **8 MHz dot-clock units**, Y is 9
bits in **scanlines**, and an off-screen shot returns the sentinel `X = 0x0001, Y = 0x000A`.
Trigger is bit 5 of buttons-MSB, active low; the B button is bit 6.

In `support/snac/snac_psx.cpp`:

```c
// GunCon X is in 8 MHz dot-clock ticks (125 ns each); the Amiga's VHPOSR reads
// hpos[8:1], one step of which is 280 ns. So one gun tick is 125/280 of an
// Amiga step. Y is already in scanlines and needs only an offset.
//
// Calibration lives here rather than in RTL precisely so it can be adjusted
// without a 40-minute Quartus run.
#define SNAC_GUN_OFFSCREEN_X 0x0001
#define SNAC_GUN_OFFSCREEN_Y 0x000A

static void snac_lightpen(int port)
{
	uint16_t gx = (uint16_t)(axes[port] & 0x01FF);          // X, 9 bits
	uint16_t gy = (uint16_t)((axes[port] >> 16) & 0x01FF);  // Y, 9 bits

	// Pointing off the screen is a real state, not an error: the Amiga side
	// must simply not latch, leaving the last position alone.
	if (gx == SNAC_GUN_OFFSCREEN_X && gy == SNAC_GUN_OFFSCREEN_Y) return;

	int hpos = ((int)gx * 125) / 280 + cfg_gun_offset_x;
	int vpos = (int)gy + cfg_gun_offset_y;
	if (hpos < 0) hpos = 0; if (hpos > 0xFF) hpos = 0xFF;
	if (vpos < 0) vpos = 0; if (vpos > 0x7FF) vpos = 0x7FF;

	spi_uio_cmd_cont(UIO_MM2_LPEN);
	spi_w((uint16_t)vpos);
	spi_w((uint16_t)hpos);
	DisableIO();
}
```

`cfg_gun_offset_x` / `cfg_gun_offset_y` are per-display calibration, stored alongside the
SNAC port modes in `AmigaCD.cfg`. Default both to 0 and adjust on hardware.

**Check the axis packing before trusting the shifts above.** The reader packs gun
coordinates into the same 32-bit `axes` word it uses for analog sticks, and which bytes
land where depends on the concatenation in `Minimig.sv` — read it rather than assuming
this code's `& 0x01FF` and `>> 16` are right.

- [ ] **Step 5: Build the core**

```bash
/c/intelFPGA_lite/17.0/quartus/bin64/quartus_sh --flow compile upstream/core-cd32-cdtv/Minimig.qpf
```

Expected: Fitter successful, no failing setup paths. Compare slack against Task 2's result,
not against the original baseline — Task 2 moved it.

- [ ] **Step 6: Commit both repos**

---

### Task 6: Hardware bring-up

**Files:**
- Modify: `docs/psx-snac-hardware.md`

These cannot be simulated. Run them in order and record the result of each; do not tick a
box that was not exercised.

- [ ] **Step 1: `SW[1]` off.** First, always, and silent when wrong.
- [ ] **Step 2: Reverse regression — MT32-pi.** Set the User Port row back to MT32-Pi,
      confirm MIDI still works. A widened mux is exactly where old tenants break.
- [ ] **Step 3: Reverse regression — MiSTer Floppy.** Set it to MiSTer Floppy, confirm the
      drives still detect. `enable_mister_floppy` changed from a bare signal to a
      comparison in Task 2.
- [ ] **Step 4: Old config loads.** A pre-change `AmigaCD.cfg` must migrate with both ports
      Off, not misparse. Third on-disk layout, third chance at that trap.
- [ ] **Step 5: CD32 pad, every button.** In a game that uses all seven — confirm Red,
      Blue, Yellow, Green, Forward, Reverse and Play each do what the label says. **This is
      the check that validates Task 1's mapping table, which is derived rather than
      documented.** Also confirm a USB pad still works simultaneously.
- [ ] **Step 6: Stick as mouse.** Workbench or a CDTV title, with the pad's ANALOG button
      pressed. Confirm both axes move the right way.
- [ ] **Step 7: Light pen.** Last: needs a CRT on direct analog output, the scaler off, and
      a Phazer-compatible game.

---

## What this plan deliberately does not do

- **Force DualShock analog mode** via the config-mode command sequence. See "Two decisions".
- **Amiga light pen on the joystick port's FIRE line.** The latch is loaded directly, so a
  physical Phazer is not covered.
- **PSX memory cards, rumble, NeGcon, Jogcon, multitap.**
- **Change `rtl/snac_psx.v`.** If it needs changing, the Menu core fork must be updated and
  re-fitted in lockstep, and that is a separate piece of work.
