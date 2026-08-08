# AmigaCD Save States — Phase 1A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the save state framework end to end and prove it round-trips CPU registers, chip RAM and memory-map state across a power cycle on real CD32 hardware.

**Architecture:** The core is the active party. When the user asks for a save, `ss_ctrl` waits for a naturally quiet instant (blitter idle, no DMA word in flight, CPU at a no-memory-access boundary), holds every clock enable low to stop the machine between ticks, serialises state and chip RAM into a DDR3 window, then bumps a counter that the already-existing host poller notices and writes to disk. Restore reverses it. State is captured as one wide concatenated vector so that save and restore are structurally the same list, which makes "saved but never restored" impossible for anything in the vector.

**Tech Stack:** Verilog-2005 (Icarus for simulation, Quartus Prime Lite 17.0.2 for synthesis), VHDL for the TG68K change, C++14 for host code cross-built with `arm-none-linux-gnueabihf-gcc` 10.2.1, GNU Make, GitHub Actions.

## Global Constraints

- **RTL is Verilog-2005 only.** CI compiles with `iverilog -g2005 -Wall`. No SystemVerilog constructs in any file under `rtl/`.
- **Canonical RTL lives in this repo, copies go to the core.** Per `rtl/README.md`: new modules are created in `AmigaCD/rtl/`, testbenches in `AmigaCD/rtl/tb/`, and a **copy** is placed in `upstream/core-cd32-cdtv/rtl/`. When the canonical file changes, refresh the copy in the same commit.
- **Host C++ is `-std=gnu++14`**, built with `-Wall -Wextra` and must produce **zero warnings**. See `Makefile:56`.
- **`support/minimig/minimig_savestate.cpp` must not include any MiSTer header.** It is compiled natively by the test harness, and MiSTer headers pull in `linux/cdrom.h`, which does not exist on the dev machine. Only `<stdint.h>`, `<string.h>` are permitted.
- **Savestate DDR3 window is `0x3E000000`, slot size `0x400000` (4 MB).** Four slots span `0x3E000000`–`0x3EFFFFFF`, inside the host's required `0x20000000`–`0x40000000` range (`user_io.cpp:775`).
- **State format version is `0x00010000`** (major 1, minor 0). Magic is `0x53534341`, which reads `ACSS` in a byte dump.
- **All multi-byte values in the payload are little-endian**, matching the ARM host.
- **Every task that changes RTL must build clean under `iverilog -g2005 -Wall`** with no warnings before commit.
- **Prefer Serena tools** for reading and editing project files: `find_symbol`, `get_symbols_overview`, `search_for_pattern`, `read_file`, `replace_content`. Use ordinary shell tools for git and for anything under `upstream/`.

## Scope

**In scope for Phase 1A:** the framework, and enough captured state to prove it — CPU architectural registers, chip RAM, and Gary's memory-map state.

**Out of scope, deferred to Phase 1B:** the chipset register inventory (Agnus, Denise, Paula, CIA, Akiko) and the userspace CD session block. Phase 1A defines the header field and reserves space for the host block but writes `host_words = 0`.

Phase 1A's acceptance test is therefore **not** "a CD32 game resumes" — that is Phase 1B's gate. Phase 1A's gate is a purpose-built 68k test program whose execution provably rewinds.

## File Structure

**Created in this repo (`AmigaCD/`):**

| File | Responsibility |
|---|---|
| `rtl/ss_crc32.v` | Byte-wise CRC-32, reflected `0xEDB88320`, matching the host's `crc32_compute` |
| `rtl/ss_quiesce.v` | Watches for a naturally quiet instant; emits `freeze`, or `timeout` after 3 frames |
| `rtl/ss_serdes.v` | Converts a `WIDTH`-bit state vector to and from a 32-bit word stream |
| `rtl/ss_dma.v` | Reads a chip RAM range out through the idle SDRAM CPU port |
| `rtl/ss_ctrl.v` | Top FSM: sequences quiesce, header, registers, memory, CRC, commit; masters the DDR3 window |
| `rtl/tb/ss_crc32_tb.v` | CRC-32 known-answer tests |
| `rtl/tb/ss_quiesce_tb.v` | Quiesce and timeout behaviour |
| `rtl/tb/ss_serdes_tb.v` | Save/restore round-trip over random vectors |
| `rtl/tb/ss_dma_tb.v` | Memory readout against a mock SDRAM |
| `rtl/tb/ss_ctrl_tb.v` | Full save and restore sequence against mocks |
| `rtl/ss_state.vh` | `` `SS_STATE_LIST `` and `` `SS_STATE_W ``, included by both `minimig.v` and `Minimig.sv` |
| `support/minimig/minimig_savestate.h` | Payload format constants and header struct |
| `support/minimig/minimig_savestate.cpp` | Header build, parse and validation. Dependency-free |
| `support/minimig/minimig_crc32.h` | Declaration for the existing `crc32_compute` |
| `tests/Makefile` | Native test harness |
| `tests/test_savestate_format.cpp` | Unit tests for the header library |

**Modified in this repo:**

| File | Change |
|---|---|
| `.github/workflows/build.yml` | Add `host-test` job; extend `rtl-sim` with the five new benches |
| `support/minimig/minimig_a2065_rings.cpp:18` | Replace the local `extern` with the new header |

**Modified in the core (`upstream/core-cd32-cdtv/`):**

| File | Change |
|---|---|
| `rtl/tg68k/TG68KdotC_Kernel.vhd` | Add a state export port over `regfile`, `TG68_PC`, `Flags`, `FlagsSR`, `USP`, `VBR`, `CACR` |
| `rtl/cpu_wrapper.v` | Plumb the export out of the TG68K instance |
| `rtl/minimig.v` | Gate clock enables with `ss_freeze`; expose Gary map state and the CPU register snapshot |
| `rtl/ddram_ctrl.v` | Mux the savestate writer onto the existing master 0 port while frozen |
| `Minimig.sv` | Instantiate `ss_ctrl`, wire the DDR3 port, add `SS3E000000:400000` to `CONF_STR` |
| `rtl/` (copies) | `ss_crc32.v`, `ss_quiesce.v`, `ss_serdes.v`, `ss_dma.v`, `ss_ctrl.v`, `ss_state.vh` |

**Not modified:** `rtl/A2065/a2065_ddram_arbiter.v`. An earlier draft of this plan
added a third master to it. That is unnecessary and actively harmful: master 0 is
the Minimig fast-RAM path, which is provably idle while the machine is frozen,
so the savestate writer borrows it exactly as `ss_dma` borrows the SDRAM CPU
port. Extending a two-master arbiter to three would add fan-in and a wider
`owner` register to a module on the DDR3 critical path, for no benefit.

## Payload Format (version 1.0)

All words are 32-bit little-endian. Word offsets are from the start of the slot's DDR3 window.

| Word | Name | Written by | Meaning |
|---|---|---|---|
| 0 | `counter` | both | Handshake. Host writes `0xFFFFFFFF` after loading a file; core bumps it to request a save |
| 1 | `length` | core, adjusted by host | Number of words following word 1. Host writes `(length + 2) * 4` bytes to disk |
| 2 | `magic` | core | `0x53534341` |
| 3 | `version` | core | `0x00010000` |
| 4 | `core_words` | core | Words of core payload |
| 5 | `host_words` | host | Words of host block. Core writes 0 |
| 6 | `core_crc32` | core | CRC-32 over the `core_words` payload words |
| 7 | `host_crc32` | host | CRC-32 over the host block. Core writes 0 |
| 8… | core payload | core | `core_words` words |
| … | host block | host | `host_words` words, immediately after the core payload |

So `length = 6 + core_words + host_words`, and the core alone sets `length = 6 + core_words`.

---

### Task 1: Payload format library and native test harness

There is no host-side test harness in this repo — no `test` target in `Makefile`, nothing under `scripts/` or `tools/`. This task creates one, and uses it to build the format library that both later host work and the RTL header layout depend on.

**Files:**
- Create: `support/minimig/minimig_savestate.h`
- Create: `support/minimig/minimig_savestate.cpp`
- Create: `support/minimig/minimig_crc32.h`
- Create: `tests/Makefile`
- Create: `tests/test_savestate_format.cpp`
- Modify: `support/minimig/minimig_a2065_rings.cpp:18`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Consumes: `crc32_compute(const uint8_t *data, int len)` from `support/minimig/minimig_a2065_crc32.cpp`
- Produces:
  - `bool ss_header_parse(const uint32_t *win, uint32_t win_words, ss_header_t *out)`
  - `uint32_t ss_host_block_offset(const ss_header_t *h)`
  - `void ss_header_set_host_block(uint32_t *win, uint32_t host_words, uint32_t host_crc)`
  - `struct ss_header_t { uint32_t counter, length, magic, version, core_words, host_words, core_crc32, host_crc32; }`

- [ ] **Step 1: Write the failing test**

Create `tests/test_savestate_format.cpp`:

```cpp
// Native unit tests for the save state payload format.
// Built by tests/Makefile with the host compiler, not the ARM cross-compiler.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../support/minimig/minimig_savestate.h"

static int failures = 0;

static void check(const char *name, bool ok)
{
	if (ok) printf("PASS %s\n", name);
	else { printf("FAIL %s\n", name); failures++; }
}

// Build a minimal well-formed window: 3 words of core payload, no host block.
static void build_window(uint32_t *win, uint32_t core_words)
{
	memset(win, 0, SS_MAX_WORDS * 4);
	win[0] = 0x00000001;
	win[1] = 6 + core_words;
	win[2] = SS_MAGIC;
	win[3] = SS_VERSION;
	win[4] = core_words;
	win[5] = 0;
	win[6] = crc32_compute((const uint8_t *)&win[8], core_words * 4);
	win[7] = 0;
	for (uint32_t i = 0; i < core_words; i++) win[8 + i] = 0xA5A50000 + i;
	// CRC must be taken after the payload is filled.
	win[6] = crc32_compute((const uint8_t *)&win[8], core_words * 4);
}

int main(void)
{
	uint32_t win[SS_MAX_WORDS];
	ss_header_t h;

	build_window(win, 3);
	check("valid header parses", ss_header_parse(win, SS_MAX_WORDS, &h));
	check("core_words read back", h.core_words == 3);
	check("host block follows core payload", ss_host_block_offset(&h) == 11);

	build_window(win, 3);
	win[2] = 0xDEADBEEF;
	check("bad magic rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	win[3] = 0x00020000;
	check("unknown version rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	win[9] ^= 1;
	check("corrupt payload rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	win[1] = SS_MAX_WORDS + 100;
	check("length past window rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	// core_words + host_words must not be allowed to wrap a uint32_t. Here
	// core_words = 0xFFFFFFF9 and host_words = 10 wrap to 3, which matches
	// length - SS_HEADER_TAIL_WORDS (9 - 6) and slips under win_words in the
	// naive additive bounds checks. core_crc32 = 0 matches what the buggy CRC
	// call produces for a negative cast length, so only the overflow guard
	// itself can catch this.
	build_window(win, 3);
	win[1] = 9;
	win[4] = 0xFFFFFFF9u;
	win[5] = 10;
	win[6] = 0;
	check("overflowing core_words+host_words rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	// A truncated file arrives as a zero tail because the host memsets the
	// window before reading (user_io.cpp:1998). That must not parse.
	memset(win, 0, sizeof(win));
	check("all-zero window rejected", !ss_header_parse(win, SS_MAX_WORDS, &h));

	build_window(win, 3);
	ss_header_set_host_block(win, 2, 0x12345678);
	check("host block sets host_words", win[5] == 2);
	check("host block sets host_crc32", win[7] == 0x12345678);
	check("host block extends length", win[1] == 6 + 3 + 2);

	if (failures) { printf("%d FAILURE(S)\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
```

- [ ] **Step 2: Create the test harness**

Create `tests/Makefile`:

```makefile
# Native (host) unit tests. Deliberately separate from the top-level Makefile,
# which cross-builds the whole MiSTer binary for ARM. Only dependency-free
# modules can be tested here: anything including a MiSTer header pulls in
# linux/cdrom.h, which does not exist on the Windows dev machine.

CXX      ?= g++
CXXFLAGS  = -std=gnu++14 -Wall -Wextra -O1 -g
BUILDDIR  = build

SRC = ../support/minimig/minimig_savestate.cpp \
      ../support/minimig/minimig_a2065_crc32.cpp

TESTS = test_savestate_format

.PHONY: all clean
all: $(addprefix $(BUILDDIR)/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; $(BUILDDIR)/$$t || exit 1; done

$(BUILDDIR)/%: %.cpp $(SRC)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(SRC)

clean:
	rm -rf $(BUILDDIR)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `make -C tests`
Expected: FAIL — `fatal error: ../support/minimig/minimig_savestate.h: No such file or directory`

- [ ] **Step 4: Add the CRC32 header**

Create `support/minimig/minimig_crc32.h`:

```cpp
#ifndef MINIMIG_CRC32_H
#define MINIMIG_CRC32_H

#include <stdint.h>

// Standard CRC-32: reflected polynomial 0xEDB88320, init 0xFFFFFFFF,
// final xor 0xFFFFFFFF. Implemented in minimig_a2065_crc32.cpp.
uint32_t crc32_compute(const uint8_t *data, int len);

#endif
```

- [ ] **Step 5: Write the format header**

Create `support/minimig/minimig_savestate.h`:

```cpp
#ifndef MINIMIG_SAVESTATE_H
#define MINIMIG_SAVESTATE_H

#include <stdint.h>

#include "minimig_crc32.h"

// Save state payload format, version 1.0. See
// docs/superpowers/specs/2026-08-08-amigacd-savestates-design.md.
//
// The core writes words 0-6 and the core payload; the host appends its own
// block after the core payload and updates words 1, 5 and 7. All values are
// little-endian.

#define SS_MAGIC    0x53534341u   // reads "ACSS" in a byte dump
#define SS_VERSION  0x00010000u   // major 1, minor 0

// 4 MB slot / 4 bytes per word.
#define SS_MAX_WORDS (0x400000u / 4u)

// Words 2..7 inclusive: the part of the header that follows `length`.
#define SS_HEADER_TAIL_WORDS 6u

typedef struct
{
	uint32_t counter;
	uint32_t length;
	uint32_t magic;
	uint32_t version;
	uint32_t core_words;
	uint32_t host_words;
	uint32_t core_crc32;
	uint32_t host_crc32;
} ss_header_t;

// Validates magic, version, bounds and the core payload CRC. Returns false and
// leaves *out untouched on any failure. A window the host memset and then
// partially filled from a truncated file will fail here rather than restore.
bool ss_header_parse(const uint32_t *win, uint32_t win_words, ss_header_t *out);

// Word offset of the host block: immediately after the core payload.
uint32_t ss_host_block_offset(const ss_header_t *h);

// Records a host block that has already been written at ss_host_block_offset(),
// updating host_words, host_crc32 and length.
void ss_header_set_host_block(uint32_t *win, uint32_t host_words, uint32_t host_crc);

#endif
```

- [ ] **Step 6: Write the implementation**

Create `support/minimig/minimig_savestate.cpp`:

```cpp
// Save state payload format helpers.
//
// Deliberately free of MiSTer headers so tests/Makefile can build it with the
// host compiler. Only <stdint.h> and our own headers.

#include <stdint.h>

#include "minimig_savestate.h"

bool ss_header_parse(const uint32_t *win, uint32_t win_words, ss_header_t *out)
{
	if (!win || !out) return false;
	if (win_words < 8) return false;

	ss_header_t h;
	h.counter    = win[0];
	h.length     = win[1];
	h.magic      = win[2];
	h.version    = win[3];
	h.core_words = win[4];
	h.host_words = win[5];
	h.core_crc32 = win[6];
	h.host_crc32 = win[7];

	if (h.magic != SS_MAGIC) return false;
	if (h.version != SS_VERSION) return false;

	// core_words and host_words are raw, untrusted uint32_t values straight
	// from the file. Adding two of them can wrap a uint32_t, and a wrapped
	// sum would slip past every check below (and later alias the host block
	// onto unrelated memory via ss_host_block_offset()). Do the bounds
	// arithmetic in 64 bits so no attainable pair of values can wrap.
	uint64_t core_words = h.core_words;
	uint64_t host_words = h.host_words;

	// length counts everything after word 1.
	if (h.length < SS_HEADER_TAIL_WORDS) return false;
	if (h.length > win_words - 2) return false;
	if (core_words + host_words != (uint64_t)h.length - SS_HEADER_TAIL_WORDS) return false;

	// Core payload starts at word 8 and must fit.
	if (8u + core_words + host_words > win_words) return false;

	if (crc32_compute((const uint8_t *)&win[8], (int)(h.core_words * 4)) != h.core_crc32)
		return false;

	*out = h;
	return true;
}

uint32_t ss_host_block_offset(const ss_header_t *h)
{
	return 8u + h->core_words;
}

void ss_header_set_host_block(uint32_t *win, uint32_t host_words, uint32_t host_crc)
{
	win[5] = host_words;
	win[7] = host_crc;
	win[1] = SS_HEADER_TAIL_WORDS + win[4] + host_words;
}
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make -C tests`
Expected: nine `PASS` lines then `ALL TESTS PASSED`, exit 0

- [ ] **Step 8: Replace the stray extern with the header**

In `support/minimig/minimig_a2065_rings.cpp`, replace line 18:

```cpp
extern uint32_t crc32_compute(const uint8_t *data, int len);
```

with:

```cpp
#include "minimig_crc32.h"
```

- [ ] **Step 9: Add the CI job**

In `.github/workflows/build.yml`, add a job alongside `rtl-sim`:

```yaml
  # Native unit tests for dependency-free host modules. Separate from the ARM
  # cross-build because these run on the CI host directly.
  host-test:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Run native unit tests
        run: make -C tests
```

- [ ] **Step 10: Commit**

```bash
git add support/minimig/minimig_savestate.h support/minimig/minimig_savestate.cpp support/minimig/minimig_crc32.h support/minimig/minimig_a2065_rings.cpp tests/Makefile tests/test_savestate_format.cpp .github/workflows/build.yml
git commit -m "feat: save state payload format and a native test harness"
```

---

### Task 2: CRC-32 in RTL

The core must compute the same CRC the host validates, so this is a known-answer test against the standard vector rather than against our own implementation.

**Files:**
- Create: `rtl/ss_crc32.v`
- Create: `rtl/tb/ss_crc32_tb.v`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: `ss_crc32` with ports `clk`, `init`, `wr`, `byte_in[7:0]`, `crc_out[31:0]`. One byte per clock. `crc_out` is the finalised value (already inverted), valid the cycle after the last `wr`.

- [ ] **Step 1: Write the failing test**

Create `rtl/tb/ss_crc32_tb.v`:

```verilog
`timescale 1ns/1ns

module ss_crc32_tb;

reg         clk = 0;
reg         init = 0;
reg         wr = 0;
reg  [7:0]  byte_in = 0;
wire [31:0] crc_out;

integer errors = 0;
integer i;

ss_crc32 dut (.clk(clk), .init(init), .wr(wr), .byte_in(byte_in), .crc_out(crc_out));

always #5 clk = ~clk;

// Feed an ASCII string, one byte per clock.
task feed_string(input [71:0] s, input integer len);
	integer k;
begin
	@(negedge clk);
	init = 1;
	@(negedge clk);
	init = 0;
	for (k = len - 1; k >= 0; k = k - 1) begin
		byte_in = s[k*8 +: 8];
		wr = 1;
		@(negedge clk);
	end
	wr = 0;
	@(negedge clk);
end
endtask

task check(input [255:0] name, input [31:0] want);
begin
	if (crc_out !== want) begin
		$display("FAIL %0s: crc=%08h, want %08h", name, crc_out, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	// The canonical CRC-32 check value.
	feed_string("123456789", 9);
	check("check vector 123456789", 32'hCBF43F26);

	// A single byte, verified against the same reflected 0xEDB88320 CRC the
	// host uses in minimig_a2065_crc32.cpp.
	feed_string("a", 1);
	check("single byte a", 32'hE8B7BE43);

	// Empty input: init then no writes, final xor of 0xFFFFFFFF gives zero.
	@(negedge clk); init = 1; @(negedge clk); init = 0; @(negedge clk);
	check("empty input", 32'h00000000);

	// init must be able to restart cleanly after a previous run.
	feed_string("123456789", 9);
	check("restart after reuse", 32'hCBF43F26);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
mkdir -p build && iverilog -g2005 -Wall -o build/ss_crc32_tb rtl/ss_crc32.v rtl/tb/ss_crc32_tb.v
```
Expected: FAIL — `rtl/ss_crc32.v: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `rtl/ss_crc32.v`:

```verilog
// CRC-32, reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final xor
// 0xFFFFFFFF. Byte at a time: eight XOR stages per clock rather than
// thirty-two, because this core closes timing by 0.177 ns and a wide
// combinational CRC is exactly the kind of cloud that displaces the SDRAM
// address path. Four clocks per 32-bit word is ~18 ms for 2 MB, which is
// nothing against a save the user asked for.
//
// Matches crc32_compute() in support/minimig/minimig_a2065_crc32.cpp.

module ss_crc32
(
	input             clk,
	input             init,
	input             wr,
	input      [7:0]  byte_in,
	output     [31:0] crc_out
);

reg [31:0] crc = 32'hFFFFFFFF;

assign crc_out = ~crc;

// One bit of the reflected CRC step.
function [31:0] crc_bit;
	input [31:0] c;
begin
	crc_bit = c[0] ? ((c >> 1) ^ 32'hEDB88320) : (c >> 1);
end
endfunction

integer b;
reg [31:0] next;

always @(posedge clk) begin
	if (init) crc <= 32'hFFFFFFFF;
	else if (wr) begin
		next = crc ^ {24'd0, byte_in};
		for (b = 0; b < 8; b = b + 1) next = crc_bit(next);
		crc <= next;
	end
end

endmodule
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_crc32_tb rtl/ss_crc32.v rtl/tb/ss_crc32_tb.v && vvp build/ss_crc32_tb
```
Expected: four `PASS` lines then `ALL TESTS PASSED`, and no warnings from `iverilog`

- [ ] **Step 5: Add to CI**

In `.github/workflows/build.yml`, append to the `rtl-sim` job:

```yaml
      - name: Build save state CRC testbench
        run: iverilog -g2005 -Wall -o build/ss_crc32_tb rtl/ss_crc32.v rtl/tb/ss_crc32_tb.v

      - name: Run save state CRC testbench
        run: vvp build/ss_crc32_tb
```

- [ ] **Step 6: Copy to the core and commit**

```bash
cp rtl/ss_crc32.v upstream/core-cd32-cdtv/rtl/ss_crc32.v
git add rtl/ss_crc32.v rtl/tb/ss_crc32_tb.v .github/workflows/build.yml
git commit -m "feat: CRC-32 for save state payloads"
```

---

### Task 3: The quiesce sequencer

Rather than draining the machine, this watches for a quiet instant that occurs naturally. On a running Amiga the blitter is idle most of the time, so one arrives within microseconds; if it does not, the machine is wedged and the save must be abandoned rather than capture something inconsistent.

**Files:**
- Create: `rtl/ss_quiesce.v`
- Create: `rtl/tb/ss_quiesce_tb.v`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: `ss_quiesce` with ports `clk`, `rst_n`, `req`, `blit_busy`, `disk_busy`, `audio_busy`, `cpu_boundary`, `frame_tick`, `freeze`, `quiesced`, `timeout`. `freeze` stays asserted until `req` drops. `timeout` latches after `FRAME_LIMIT` frame ticks without a quiet instant, and also drops `req` handling until `req` is deasserted.

- [ ] **Step 1: Write the failing test**

Create `rtl/tb/ss_quiesce_tb.v`:

```verilog
`timescale 1ns/1ns

module ss_quiesce_tb;

reg  clk = 0;
reg  rst_n = 0;
reg  req = 0;
reg  blit_busy = 0;
reg  disk_busy = 0;
reg  audio_busy = 0;
reg  cpu_boundary = 0;
reg  frame_tick = 0;

wire freeze, quiesced, timeout;

integer errors = 0;
integer i;

ss_quiesce #(.FRAME_LIMIT(3)) dut
(
	.clk(clk), .rst_n(rst_n), .req(req),
	.blit_busy(blit_busy), .disk_busy(disk_busy), .audio_busy(audio_busy),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze), .quiesced(quiesced), .timeout(timeout)
);

always #5 clk = ~clk;

task pulse_frame;
begin
	@(negedge clk); frame_tick = 1;
	@(negedge clk); frame_tick = 0;
end
endtask

task check(input [255:0] name, input actual, input want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %b, want %b", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	@(negedge clk); rst_n = 1;

	// Idle: no request, no freeze even though the machine is quiet.
	cpu_boundary = 1;
	@(negedge clk);
	check("idle does not freeze", freeze, 1'b0);

	// Request with the blitter busy: must wait.
	blit_busy = 1;
	req = 1;
	@(negedge clk); @(negedge clk);
	check("busy blitter blocks freeze", freeze, 1'b0);

	// Blitter finishes: freeze on the next quiet cycle.
	blit_busy = 0;
	@(negedge clk); @(negedge clk);
	check("freeze once quiet", freeze, 1'b1);
	check("quiesced asserted", quiesced, 1'b1);
	check("no timeout", timeout, 1'b0);

	// Freeze must hold even if the busy signals wobble, since the machine is
	// stopped and those inputs are now meaningless.
	blit_busy = 1;
	@(negedge clk);
	check("freeze holds while req high", freeze, 1'b1);
	blit_busy = 0;

	// Dropping req releases the freeze.
	req = 0;
	@(negedge clk); @(negedge clk);
	check("req low releases freeze", freeze, 1'b0);

	// Never-quiet machine: three frame ticks then timeout, and no freeze.
	blit_busy = 1;
	req = 1;
	@(negedge clk);
	pulse_frame; pulse_frame; pulse_frame;
	@(negedge clk);
	check("timeout after 3 frames", timeout, 1'b1);
	check("timeout does not freeze", freeze, 1'b0);

	// Timeout clears when the request is withdrawn.
	req = 0;
	@(negedge clk); @(negedge clk);
	check("timeout clears on req low", timeout, 1'b0);

	// A CPU that is mid-instruction blocks the freeze on its own.
	blit_busy = 0;
	cpu_boundary = 0;
	req = 1;
	@(negedge clk); @(negedge clk);
	check("cpu mid-instruction blocks freeze", freeze, 1'b0);
	cpu_boundary = 1;
	@(negedge clk); @(negedge clk);
	check("freeze once cpu reaches boundary", freeze, 1'b1);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_quiesce_tb rtl/ss_quiesce.v rtl/tb/ss_quiesce_tb.v
```
Expected: FAIL — `rtl/ss_quiesce.v: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `rtl/ss_quiesce.v`:

```verilog
// Save state quiesce sequencer.
//
// Does not drain the machine. It waits for a quiet instant that happens on its
// own: blitter idle, no disk or audio DMA word in flight, CPU at a
// no-memory-access boundary. On a running Amiga that arrives within
// microseconds. If it does not arrive within FRAME_LIMIT video frames the
// machine is wedged, and we abandon the save rather than capture a state that
// looks fine and restores into garbage.
//
// Once frozen, the busy inputs are meaningless -- the machine is stopped -- so
// freeze latches until the requester drops req.

module ss_quiesce
#(
	parameter FRAME_LIMIT = 3
)
(
	input       clk,
	input       rst_n,
	input       req,

	input       blit_busy,
	input       disk_busy,
	input       audio_busy,
	input       cpu_boundary,
	input       frame_tick,

	output reg  freeze,
	output reg  quiesced,
	output reg  timeout
);

wire quiet = ~blit_busy & ~disk_busy & ~audio_busy & cpu_boundary;

reg [7:0] frames;

always @(posedge clk) begin
	if (!rst_n) begin
		freeze   <= 1'b0;
		quiesced <= 1'b0;
		timeout  <= 1'b0;
		frames   <= 8'd0;
	end
	else if (!req) begin
		freeze   <= 1'b0;
		quiesced <= 1'b0;
		timeout  <= 1'b0;
		frames   <= 8'd0;
	end
	else if (!freeze && !timeout) begin
		if (quiet) begin
			freeze   <= 1'b1;
			quiesced <= 1'b1;
		end
		else if (frame_tick) begin
			if (frames >= (FRAME_LIMIT - 1)) timeout <= 1'b1;
			frames <= frames + 8'd1;
		end
	end
end

endmodule
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_quiesce_tb rtl/ss_quiesce.v rtl/tb/ss_quiesce_tb.v && vvp build/ss_quiesce_tb
```
Expected: twelve `PASS` lines then `ALL TESTS PASSED`

- [ ] **Step 5: Add to CI**

In `.github/workflows/build.yml`, append to the `rtl-sim` job:

```yaml
      - name: Build quiesce testbench
        run: iverilog -g2005 -Wall -o build/ss_quiesce_tb rtl/ss_quiesce.v rtl/tb/ss_quiesce_tb.v

      - name: Run quiesce testbench
        run: vvp build/ss_quiesce_tb
```

- [ ] **Step 6: Copy to the core and commit**

```bash
cp rtl/ss_quiesce.v upstream/core-cd32-cdtv/rtl/ss_quiesce.v
git add rtl/ss_quiesce.v rtl/tb/ss_quiesce_tb.v .github/workflows/build.yml
git commit -m "feat: quiesce sequencer with a bounded wait for a quiet instant"
```

---

### Task 4: State vector serialiser

This module is the structural answer to the worst bug class in the feature. Because save and restore both operate on one `WIDTH`-bit vector, a register that is captured but never restored is impossible for anything inside that vector — a width mismatch is a compile error, not a silent failure ten seconds into a resumed game.

**Files:**
- Create: `rtl/ss_serdes.v`
- Create: `rtl/tb/ss_serdes_tb.v`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: `ss_serdes #(parameter WIDTH)` with ports `clk`, `rst_n`, `save_start`, `state_in[WIDTH-1:0]`, `word_valid`, `word_out[31:0]`, `save_done`, `load_start`, `word_in_valid`, `word_in[31:0]`, `load_done`, `state_out[WIDTH-1:0]`, `state_we`.
- `WORDS = (WIDTH + 31) / 32`. Words are emitted least-significant first. On load, `state_we` pulses for one cycle with the complete `state_out` once `WORDS` words have arrived.

- [ ] **Step 1: Write the failing test**

Create `rtl/tb/ss_serdes_tb.v`:

```verilog
`timescale 1ns/1ns

// Round-trip test: save a vector, feed the captured words back, and require
// the restored vector to match bit for bit. WIDTH is deliberately not a
// multiple of 32 so the ragged top word is exercised.

module ss_serdes_tb;

localparam WIDTH = 100;
localparam WORDS = (WIDTH + 31) / 32;

reg               clk = 0;
reg               rst_n = 0;
reg               save_start = 0;
reg  [WIDTH-1:0]  state_in = 0;
wire              word_valid;
wire [31:0]       word_out;
wire              save_done;
reg               load_start = 0;
reg               word_in_valid = 0;
reg  [31:0]       word_in = 0;
wire              load_done;
wire [WIDTH-1:0]  state_out;
wire              state_we;

reg  [31:0]       captured [0:WORDS-1];
integer           cap_idx;
integer           errors = 0;
integer           trial;
integer           k;

ss_serdes #(.WIDTH(WIDTH)) dut
(
	.clk(clk), .rst_n(rst_n),
	.save_start(save_start), .state_in(state_in),
	.word_valid(word_valid), .word_out(word_out), .save_done(save_done),
	.load_start(load_start), .word_in_valid(word_in_valid), .word_in(word_in),
	.load_done(load_done), .state_out(state_out), .state_we(state_we)
);

always #5 clk = ~clk;

// Capture every emitted word.
always @(posedge clk) begin
	if (word_valid) begin
		captured[cap_idx] = word_out;
		cap_idx = cap_idx + 1;
	end
end

reg [WIDTH-1:0] restored;
always @(posedge clk) if (state_we) restored = state_out;

task round_trip(input [WIDTH-1:0] value);
begin
	cap_idx = 0;
	state_in = value;
	@(negedge clk); save_start = 1;
	@(negedge clk); save_start = 0;
	wait (save_done);
	@(negedge clk);

	if (cap_idx !== WORDS) begin
		$display("FAIL word count: got %0d, want %0d", cap_idx, WORDS);
		errors = errors + 1;
	end

	restored = {WIDTH{1'bx}};
	@(negedge clk); load_start = 1;
	@(negedge clk); load_start = 0;
	for (k = 0; k < WORDS; k = k + 1) begin
		word_in = captured[k];
		word_in_valid = 1;
		@(negedge clk);
	end
	word_in_valid = 0;
	wait (load_done);
	@(negedge clk);

	if (restored !== value) begin
		$display("FAIL round trip: got %h, want %h", restored, value);
		errors = errors + 1;
	end
	else $display("PASS round trip %h", value);
end
endtask

initial begin
	@(negedge clk); rst_n = 1;

	round_trip({WIDTH{1'b0}});
	round_trip({WIDTH{1'b1}});
	round_trip(100'h0123456789ABCDEF01234567);

	// Random vectors catch ordering and ragged-word errors that fixed
	// patterns miss.
	for (trial = 0; trial < 20; trial = trial + 1) begin
		round_trip({$random, $random, $random, $random});
	end

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_serdes_tb rtl/ss_serdes.v rtl/tb/ss_serdes_tb.v
```
Expected: FAIL — `rtl/ss_serdes.v: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `rtl/ss_serdes.v`:

```verilog
// Converts a wide state vector to and from a 32-bit word stream.
//
// The point of this module is structural: because both directions work on the
// same WIDTH-bit vector, a signal that is saved but never restored cannot
// exist. Adding a register to the concatenation that feeds state_in without
// adding it to the one driven from state_out is a width mismatch, which
// Quartus and Icarus both reject.
//
// Words are least-significant first. The top word is zero-padded when WIDTH is
// not a multiple of 32.

module ss_serdes
#(
	parameter WIDTH = 32
)
(
	input                    clk,
	input                    rst_n,

	// Capture
	input                    save_start,
	input  [WIDTH-1:0]       state_in,
	output reg               word_valid,
	output reg [31:0]        word_out,
	output reg               save_done,

	// Restore
	input                    load_start,
	input                    word_in_valid,
	input  [31:0]            word_in,
	output reg               load_done,
	output reg [WIDTH-1:0]   state_out,
	output reg               state_we
);

localparam WORDS   = (WIDTH + 31) / 32;
localparam PADDED  = WORDS * 32;

reg [PADDED-1:0] shifter;
reg [15:0]       count;
reg              saving;
reg              loading;

always @(posedge clk) begin
	if (!rst_n) begin
		word_valid <= 1'b0;
		save_done  <= 1'b0;
		load_done  <= 1'b0;
		state_we   <= 1'b0;
		saving     <= 1'b0;
		loading    <= 1'b0;
		count      <= 16'd0;
	end
	else begin
		word_valid <= 1'b0;
		save_done  <= 1'b0;
		load_done  <= 1'b0;
		state_we   <= 1'b0;

		if (save_start) begin
			shifter <= {{(PADDED-WIDTH){1'b0}}, state_in};
			count   <= 16'd0;
			saving  <= 1'b1;
		end
		else if (saving) begin
			word_out   <= shifter[31:0];
			word_valid <= 1'b1;
			shifter    <= {32'd0, shifter[PADDED-1:32]};
			if (count == (WORDS - 1)) begin
				saving    <= 1'b0;
				save_done <= 1'b1;
			end
			count <= count + 16'd1;
		end

		if (load_start) begin
			count   <= 16'd0;
			loading <= 1'b1;
		end
		else if (loading && word_in_valid) begin
			shifter <= {word_in, shifter[PADDED-1:32]};
			if (count == (WORDS - 1)) begin
				loading <= 1'b0;
			end
			count <= count + 16'd1;
		end
		else if (!loading && count == WORDS) begin
			state_out <= shifter[WIDTH-1:0];
			state_we  <= 1'b1;
			load_done <= 1'b1;
			count     <= 16'd0;
		end
	end
end

endmodule
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_serdes_tb rtl/ss_serdes.v rtl/tb/ss_serdes_tb.v && vvp build/ss_serdes_tb
```
Expected: twenty-three `PASS round trip` lines then `ALL TESTS PASSED`

- [ ] **Step 5: Add to CI**

In `.github/workflows/build.yml`, append to the `rtl-sim` job:

```yaml
      - name: Build state serialiser testbench
        run: iverilog -g2005 -Wall -o build/ss_serdes_tb rtl/ss_serdes.v rtl/tb/ss_serdes_tb.v

      - name: Run state serialiser testbench
        run: vvp build/ss_serdes_tb
```

- [ ] **Step 6: Copy to the core and commit**

```bash
cp rtl/ss_serdes.v upstream/core-cd32-cdtv/rtl/ss_serdes.v
git add rtl/ss_serdes.v rtl/tb/ss_serdes_tb.v .github/workflows/build.yml
git commit -m "feat: state vector serialiser with a round-trip bench"
```

---

### Task 5: Chip RAM readout

Reads a chip RAM range out through the SDRAM controller's **CPU port**, which is guaranteed idle because the machine is frozen. This is the reuse rule from the spec: no new port on `sdram_ctrl`, and therefore no extra fan-in on `sd_addr`, which is already the core's worst timing path.

The CPU port is behind a cache (`rtl/sdram_ctrl.v:116` wires `cache_inhibit`), and chip DMA writes do not go through that cache, so a dump could otherwise read stale data. `cache_inhibit` is asserted for the whole transfer.

**Files:**
- Create: `rtl/ss_dma.v`
- Create: `rtl/tb/ss_dma_tb.v`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: `ss_dma` with ports `clk`, `rst_n`, `start`, `base_addr[24:1]`, `word_count[23:0]`, `sd_addr[24:1]`, `sd_cs`, `sd_state[1:0]`, `sd_uds_n`, `sd_lds_n`, `sd_cache_inhibit`, `sd_rd[15:0]`, `sd_ready`, `word_valid`, `word_out[15:0]`, `done`.
- `sd_uds_n`/`sd_lds_n` are **active low**, matching `sdram_ctrl.v:119` (`{!cpuU, !cpuL}`). `sd_state` is `2'd2` for a data read, per `sdram_ctrl.v:122`.

- [ ] **Step 1: Write the failing test**

Create `rtl/tb/ss_dma_tb.v`:

```verilog
`timescale 1ns/1ns

// Mock SDRAM: returns a value derived from the address after a variable
// latency, so the DUT cannot pass by assuming a fixed response time.

module ss_dma_tb;

reg         clk = 0;
reg         rst_n = 0;
reg         start = 0;
reg  [24:1] base_addr = 0;
reg  [23:0] word_count = 0;

wire [24:1] sd_addr;
wire        sd_cs;
wire [1:0]  sd_state;
wire        sd_uds_n;
wire        sd_lds_n;
wire        sd_cache_inhibit;
reg  [15:0] sd_rd = 0;
reg         sd_ready = 0;

wire        word_valid;
wire [15:0] word_out;
wire        done;

integer errors = 0;
integer got_count;
integer latency;
integer i;

reg [15:0] got [0:15];

ss_dma dut
(
	.clk(clk), .rst_n(rst_n),
	.start(start), .base_addr(base_addr), .word_count(word_count),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_rd(sd_rd), .sd_ready(sd_ready),
	.word_valid(word_valid), .word_out(word_out), .done(done)
);

always #5 clk = ~clk;

// Mock memory: contents are 0xC000 + address.
always @(posedge clk) begin
	sd_ready <= 1'b0;
	if (sd_cs && !sd_ready) begin
		latency = latency + 1;
		if (latency >= 3) begin
			sd_rd    <= 16'hC000 + {8'd0, sd_addr[8:1]};
			sd_ready <= 1'b1;
			latency  = 0;
		end
	end
end

always @(posedge clk) begin
	if (word_valid) begin
		got[got_count] = word_out;
		got_count = got_count + 1;
	end
end

task check(input [255:0] name, input integer actual, input integer want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %0d, want %0d", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	latency = 0;
	got_count = 0;
	@(negedge clk); rst_n = 1;

	base_addr  = 24'h000010;
	word_count = 24'd4;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;

	wait (done);
	@(negedge clk);

	check("word count", got_count, 4);
	for (i = 0; i < 4; i = i + 1) begin
		check("word contents", got[i], 16'hC000 + 16'h0010 + i);
	end

	check("cs released after done", sd_cs, 1'b0);

	// A second transfer must work without an intervening reset.
	got_count = 0;
	base_addr  = 24'h000020;
	word_count = 24'd2;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	wait (done);
	@(negedge clk);
	check("second transfer count", got_count, 2);
	check("second transfer first word", got[0], 16'hC000 + 16'h0020);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v
```
Expected: FAIL — `rtl/ss_dma.v: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `rtl/ss_dma.v`:

```verilog
// Reads a chip RAM range out through the SDRAM controller's CPU port.
//
// Borrowing the CPU port rather than adding one is deliberate. sdram_ctrl's
// sd_addr register is the core's worst timing path (see
// docs/sdram-timing-headroom.md), and widening its arbiter would add fan-in
// exactly where there is none to spare. The port is free because the machine
// is frozen: no CPU cycle can be in flight.
//
// cache_inhibit is asserted throughout. The CPU port is cached, and chip DMA
// writes do not pass through that cache, so an uninhibited dump could return
// stale data for anything the blitter or copper wrote.

module ss_dma
(
	input             clk,
	input             rst_n,

	input             start,
	input      [24:1] base_addr,
	input      [23:0] word_count,

	// Borrowed sdram_ctrl CPU port. uds_n/lds_n are active low to match
	// sdram_ctrl.v:119; state 2 is "read data" per sdram_ctrl.v:122.
	output reg [24:1] sd_addr,
	output reg        sd_cs,
	output     [1:0]  sd_state,
	output            sd_uds_n,
	output            sd_lds_n,
	output            sd_cache_inhibit,
	input      [15:0] sd_rd,
	input             sd_ready,

	output reg        word_valid,
	output reg [15:0] word_out,
	output reg        done
);

assign sd_state         = 2'd2;
assign sd_uds_n         = 1'b0;
assign sd_lds_n         = 1'b0;
assign sd_cache_inhibit = sd_cs;

reg [23:0] remaining;
reg        busy;

always @(posedge clk) begin
	if (!rst_n) begin
		sd_cs      <= 1'b0;
		sd_addr    <= 24'd0;
		word_valid <= 1'b0;
		done       <= 1'b0;
		busy       <= 1'b0;
		remaining  <= 24'd0;
	end
	else begin
		word_valid <= 1'b0;
		done       <= 1'b0;

		if (start) begin
			sd_addr   <= base_addr;
			remaining <= word_count;
			busy      <= 1'b1;
			sd_cs     <= (word_count != 24'd0);
			if (word_count == 24'd0) begin
				busy <= 1'b0;
				done <= 1'b1;
			end
		end
		else if (busy && sd_ready) begin
			word_out   <= sd_rd;
			word_valid <= 1'b1;
			if (remaining == 24'd1) begin
				sd_cs <= 1'b0;
				busy  <= 1'b0;
				done  <= 1'b1;
			end
			else begin
				sd_addr <= sd_addr + 24'd1;
			end
			remaining <= remaining - 24'd1;
		end
	end
end

endmodule
```

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v && vvp build/ss_dma_tb
```
Expected: nine `PASS` lines then `ALL TESTS PASSED`

- [ ] **Step 5: Add to CI**

In `.github/workflows/build.yml`, append to the `rtl-sim` job:

```yaml
      - name: Build chip RAM readout testbench
        run: iverilog -g2005 -Wall -o build/ss_dma_tb rtl/ss_dma.v rtl/tb/ss_dma_tb.v

      - name: Run chip RAM readout testbench
        run: vvp build/ss_dma_tb
```

- [ ] **Step 6: Copy to the core and commit**

```bash
cp rtl/ss_dma.v upstream/core-cd32-cdtv/rtl/ss_dma.v
git add rtl/ss_dma.v rtl/tb/ss_dma_tb.v .github/workflows/build.yml
git commit -m "feat: chip RAM readout over the idle SDRAM CPU port"
```

---

### Task 6: The save state controller

Ties the previous four modules together and masters the DDR3 window. This is the module that decides the payload layout, so it must emit exactly the header Task 1's parser validates.

**Files:**
- Create: `rtl/ss_ctrl.v`
- Create: `rtl/tb/ss_ctrl_tb.v`
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Consumes: `ss_crc32`, `ss_quiesce`, `ss_serdes`, `ss_dma`
- Produces: `ss_ctrl #(parameter STATE_W, parameter CHIP_WORDS)` with ports listed in the implementation below. `ddr_*` is an Avalon-style master matching `a2065_ddram_arbiter`'s `m*` ports: `ddr_address[28:0]` is a **64-bit word address**, `ddr_byteenable[7:0]`, `ddr_writedata[63:0]`, `ddr_readdata[63:0]`.

Payload layout emitted by this module, in 32-bit words from the window base:

| Word | Content |
|---|---|
| 0 | `counter` |
| 1 | `length` = 6 + `core_words` |
| 2 | `SS_MAGIC` = `32'h53534341` |
| 3 | `SS_VERSION` = `32'h00010000` |
| 4 | `core_words` = `STATE_WORDS + CHIP_WORDS/2` |
| 5 | 0 (`host_words`) |
| 6 | `core_crc32` |
| 7 | 0 (`host_crc32`) |
| 8… | state vector words, then chip RAM as packed 16-bit words, two per 32-bit word |

- [ ] **Step 1: Write the failing test**

Create `rtl/tb/ss_ctrl_tb.v`:

```verilog
`timescale 1ns/1ns

// Drives a save through ss_ctrl against a mock DDR3 and a mock SDRAM, then
// checks the header words the host parser will validate.

module ss_ctrl_tb;

localparam STATE_W    = 64;
localparam CHIP_WORDS = 8;
localparam STATE_WORDS = (STATE_W + 31) / 32;
localparam CORE_WORDS  = STATE_WORDS + CHIP_WORDS / 2;

reg  clk = 0;
reg  rst_n = 0;

reg  save_req = 0;
reg  [STATE_W-1:0] state_in = 64'h0123456789ABCDEF;
wire [STATE_W-1:0] state_out;
wire state_we;

reg  blit_busy = 0;
reg  cpu_boundary = 1;
reg  frame_tick = 0;
wire freeze;
wire save_busy, save_ok, save_fail;

wire [28:0] ddr_address;
wire [63:0] ddr_writedata;
wire [7:0]  ddr_byteenable;
wire        ddr_write;
wire        ddr_read;
reg  [63:0] ddr_readdata = 0;
reg         ddr_readdatavalid = 0;
reg         ddr_waitrequest = 0;

wire [24:1] sd_addr;
wire        sd_cs;
wire [1:0]  sd_state;
wire        sd_uds_n, sd_lds_n, sd_cache_inhibit;
reg  [15:0] sd_rd = 0;
reg         sd_ready = 0;

integer errors = 0;
integer latency = 0;
integer i;

// Mock DDR3: a flat memory of 64-bit words, addressed by ddr_address.
reg [63:0] ddr_mem [0:63];

ss_ctrl #(.STATE_W(STATE_W), .CHIP_WORDS(CHIP_WORDS)) dut
(
	.clk(clk), .rst_n(rst_n),
	.slot_base(29'd0),
	.save_req(save_req), .save_busy(save_busy),
	.save_ok(save_ok), .save_fail(save_fail),
	.state_in(state_in), .state_out(state_out), .state_we(state_we),
	.blit_busy(blit_busy), .disk_busy(1'b0), .audio_busy(1'b0),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze),
	.chip_base(24'h000000),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_rd(sd_rd), .sd_ready(sd_ready),
	.ddr_address(ddr_address), .ddr_writedata(ddr_writedata),
	.ddr_byteenable(ddr_byteenable), .ddr_write(ddr_write),
	.ddr_read(ddr_read), .ddr_readdata(ddr_readdata),
	.ddr_readdatavalid(ddr_readdatavalid), .ddr_waitrequest(ddr_waitrequest)
);

always #5 clk = ~clk;

// Mock SDRAM: contents are 0xD000 + address.
always @(posedge clk) begin
	sd_ready <= 1'b0;
	if (sd_cs && !sd_ready) begin
		latency = latency + 1;
		if (latency >= 2) begin
			sd_rd    <= 16'hD000 + {8'd0, sd_addr[8:1]};
			sd_ready <= 1'b1;
			latency  = 0;
		end
	end
end

// Mock DDR3 write capture.
always @(posedge clk) begin
	if (ddr_write && !ddr_waitrequest) ddr_mem[ddr_address[5:0]] <= ddr_writedata;
end

// Read a 32-bit word out of the mock DDR3 by word index.
function [31:0] ddr_word;
	input integer idx;
begin
	ddr_word = idx[0] ? ddr_mem[idx/2][63:32] : ddr_mem[idx/2][31:0];
end
endfunction

task check(input [255:0] name, input [31:0] actual, input [31:0] want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %08h, want %08h", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	for (i = 0; i < 64; i = i + 1) ddr_mem[i] = 64'd0;
	@(negedge clk); rst_n = 1;

	// Blitter busy at first, so the save has to wait for a quiet instant.
	blit_busy = 1;
	@(negedge clk); save_req = 1;
	@(negedge clk); @(negedge clk);
	if (freeze !== 1'b0) begin
		$display("FAIL froze while blitter busy");
		errors = errors + 1;
	end
	else $display("PASS waits for blitter");

	blit_busy = 0;
	wait (save_ok);
	@(negedge clk);

	check("magic",      ddr_word(2), 32'h53534341);
	check("version",    ddr_word(3), 32'h00010000);
	check("core_words", ddr_word(4), CORE_WORDS);
	check("host_words", ddr_word(5), 32'd0);
	check("host_crc32", ddr_word(7), 32'd0);
	check("length",     ddr_word(1), 6 + CORE_WORDS);

	// State vector words, least significant first.
	check("state word 0", ddr_word(8), 32'h89ABCDEF);
	check("state word 1", ddr_word(9), 32'h01234567);

	// Chip RAM packed two 16-bit words per 32-bit word, low word first.
	check("chip word 0", ddr_word(10), {16'hD001, 16'hD000});

	check("counter bumped", ddr_word(0), 32'd1);

	save_req = 0;
	@(negedge clk); @(negedge clk);
	if (freeze !== 1'b0) begin
		$display("FAIL freeze not released");
		errors = errors + 1;
	end
	else $display("PASS freeze released");

	// A machine that never quiesces must fail rather than save.
	blit_busy = 1;
	@(negedge clk); save_req = 1;
	@(negedge clk);
	for (i = 0; i < 3; i = i + 1) begin
		@(negedge clk); frame_tick = 1;
		@(negedge clk); frame_tick = 0;
	end
	wait (save_fail);
	$display("PASS timeout reports failure");

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_ctrl_tb rtl/ss_ctrl.v rtl/ss_crc32.v rtl/ss_quiesce.v rtl/ss_serdes.v rtl/ss_dma.v rtl/tb/ss_ctrl_tb.v
```
Expected: FAIL — `rtl/ss_ctrl.v: No such file or directory`

- [ ] **Step 3: Write the implementation**

Create `rtl/ss_ctrl.v`:

```verilog
// Save state controller.
//
// Sequences a save: wait for a quiet instant, freeze the machine, serialise
// the state vector, stream chip RAM, CRC the payload, write the header, then
// bump the counter. The host poller (user_io.cpp:1969) notices the counter
// within a second and writes the window to disk.
//
// The counter is bumped LAST and only on success. A save that timed out or
// never completed leaves the previous counter value, so the host never
// persists a partial payload.

module ss_ctrl
#(
	parameter STATE_W    = 64,
	parameter CHIP_WORDS = 24'h100000   // 16-bit words; 0x100000 = 2 MB
)
(
	input                     clk,
	input                     rst_n,

	// Base of this slot's DDR3 window, as a 64-bit word address.
	input      [28:0]         slot_base,

	input                     save_req,
	output reg                save_busy,
	output reg                save_ok,
	output reg                save_fail,

	input      [STATE_W-1:0]  state_in,
	output     [STATE_W-1:0]  state_out,
	output                    state_we,

	input                     blit_busy,
	input                     disk_busy,
	input                     audio_busy,
	input                     cpu_boundary,
	input                     frame_tick,
	output                    freeze,

	input      [24:1]         chip_base,

	output     [24:1]         sd_addr,
	output                    sd_cs,
	output     [1:0]          sd_state,
	output                    sd_uds_n,
	output                    sd_lds_n,
	output                    sd_cache_inhibit,
	input      [15:0]         sd_rd,
	input                     sd_ready,

	output reg [28:0]         ddr_address,
	output reg [63:0]         ddr_writedata,
	output     [7:0]          ddr_byteenable,
	output reg                ddr_write,
	output                    ddr_read,
	input      [63:0]         ddr_readdata,
	input                     ddr_readdatavalid,
	input                     ddr_waitrequest
);

localparam STATE_WORDS = (STATE_W + 31) / 32;
localparam CORE_WORDS  = STATE_WORDS + (CHIP_WORDS / 2);

localparam SS_MAGIC   = 32'h53534341;
localparam SS_VERSION = 32'h00010000;

localparam S_IDLE   = 4'd0;
localparam S_WAIT   = 4'd1;
localparam S_STATE  = 4'd2;
localparam S_CHIP   = 4'd3;
localparam S_HEADER = 4'd4;
localparam S_COUNT  = 4'd5;
localparam S_DONE   = 4'd6;
localparam S_FAIL   = 4'd7;

assign ddr_byteenable = 8'hFF;
assign ddr_read       = 1'b0;

// ---------------------------------------------------------------- quiesce

wire quiesced;
wire timeout;

ss_quiesce quiesce
(
	.clk(clk), .rst_n(rst_n),
	.req(save_busy),
	.blit_busy(blit_busy), .disk_busy(disk_busy), .audio_busy(audio_busy),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze), .quiesced(quiesced), .timeout(timeout)
);

// ---------------------------------------------------------------- serdes

reg               ser_save_start;
wire              ser_word_valid;
wire [31:0]       ser_word_out;
wire              ser_save_done;

ss_serdes #(.WIDTH(STATE_W)) serdes
(
	.clk(clk), .rst_n(rst_n),
	.save_start(ser_save_start), .state_in(state_in),
	.word_valid(ser_word_valid), .word_out(ser_word_out),
	.save_done(ser_save_done),
	.load_start(1'b0), .word_in_valid(1'b0), .word_in(32'd0),
	.load_done(), .state_out(state_out), .state_we(state_we)
);

// ---------------------------------------------------------------- chip dma

reg         dma_start;
wire        dma_word_valid;
wire [15:0] dma_word_out;
wire        dma_done;

ss_dma dma
(
	.clk(clk), .rst_n(rst_n),
	.start(dma_start), .base_addr(chip_base), .word_count(CHIP_WORDS),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_rd(sd_rd), .sd_ready(sd_ready),
	.word_valid(dma_word_valid), .word_out(dma_word_out), .done(dma_done)
);

// ---------------------------------------------------------------- crc

reg        crc_init;
reg        crc_wr;
reg  [7:0] crc_byte;
wire [31:0] crc_value;

ss_crc32 crc (.clk(clk), .init(crc_init), .wr(crc_wr), .byte_in(crc_byte), .crc_out(crc_value));

// A 32-bit payload word is fed to the CRC as four bytes, least significant
// first, so the result matches crc32_compute() over the little-endian buffer.
reg [31:0] crc_shift;
reg [2:0]  crc_phase;
reg        crc_run;

always @(posedge clk) begin
	crc_wr <= 1'b0;
	if (!rst_n) begin
		crc_run   <= 1'b0;
		crc_phase <= 3'd0;
	end
	else if (crc_run) begin
		crc_byte  <= crc_shift[7:0];
		crc_wr    <= 1'b1;
		crc_shift <= {8'd0, crc_shift[31:8]};
		if (crc_phase == 3'd3) crc_run <= 1'b0;
		crc_phase <= crc_phase + 3'd1;
	end
end

// ---------------------------------------------------------------- writer

reg [3:0]  state;
reg [23:0] word_idx;      // 32-bit word index within the window
reg [31:0] pending_word;
reg        pending_valid;
reg        pending_low_held;
reg [15:0] chip_low;
reg [31:0] saved_crc;
reg [1:0]  header_idx;

// Queue a 32-bit payload word: CRC it and write it into DDR3.
task queue_word;
	input [31:0] w;
begin
	pending_word  <= w;
	pending_valid <= 1'b1;
	crc_shift     <= w;
	crc_phase     <= 3'd0;
	crc_run       <= 1'b1;
end
endtask

always @(posedge clk) begin
	if (!rst_n) begin
		state          <= S_IDLE;
		save_busy      <= 1'b0;
		save_ok        <= 1'b0;
		save_fail      <= 1'b0;
		ddr_write      <= 1'b0;
		pending_valid  <= 1'b0;
		ser_save_start <= 1'b0;
		dma_start      <= 1'b0;
		crc_init       <= 1'b0;
	end
	else begin
		ser_save_start <= 1'b0;
		dma_start      <= 1'b0;
		crc_init       <= 1'b0;

		// DDR3 write handshake: hold until the arbiter accepts.
		if (ddr_write && !ddr_waitrequest) begin
			ddr_write     <= 1'b0;
			pending_valid <= 1'b0;
		end
		else if (pending_valid && !ddr_write) begin
			// Two 32-bit words share one 64-bit DDR3 word.
			ddr_address   <= slot_base + {5'd0, word_idx[23:1]};
			ddr_writedata <= word_idx[0] ? {pending_word, 32'd0} : {32'd0, pending_word};
			ddr_write     <= 1'b1;
			word_idx      <= word_idx + 24'd1;
		end

		case (state)
		S_IDLE: begin
			save_ok   <= 1'b0;
			save_fail <= 1'b0;
			if (save_req) begin
				save_busy <= 1'b1;
				state     <= S_WAIT;
			end
		end

		S_WAIT: begin
			if (timeout) state <= S_FAIL;
			else if (quiesced) begin
				crc_init       <= 1'b1;
				word_idx       <= 24'd8;      // payload starts at word 8
				ser_save_start <= 1'b1;
				state          <= S_STATE;
			end
		end

		S_STATE: begin
			if (ser_word_valid) queue_word(ser_word_out);
			else if (ser_save_done) begin
				dma_start        <= 1'b1;
				pending_low_held <= 1'b0;
				state            <= S_CHIP;
			end
		end

		S_CHIP: begin
			if (dma_word_valid) begin
				if (!pending_low_held) begin
					chip_low         <= dma_word_out;
					pending_low_held <= 1'b1;
				end
				else begin
					queue_word({dma_word_out, chip_low});
					pending_low_held <= 1'b0;
				end
			end
			else if (dma_done && !pending_valid && !ddr_write) begin
				saved_crc  <= crc_value;
				word_idx   <= 24'd1;
				header_idx <= 2'd0;
				state      <= S_HEADER;
			end
		end

		// Header words 1..7 are written after the payload, because
		// core_crc32 is not known until the payload is complete. They are
		// not fed to the CRC.
		S_HEADER: begin
			if (!pending_valid && !ddr_write) begin
				case (word_idx)
				24'd1: pending_word <= 6 + CORE_WORDS;
				24'd2: pending_word <= SS_MAGIC;
				24'd3: pending_word <= SS_VERSION;
				24'd4: pending_word <= CORE_WORDS;
				24'd5: pending_word <= 32'd0;
				24'd6: pending_word <= saved_crc;
				default: pending_word <= 32'd0;
				endcase
				pending_valid <= 1'b1;
				if (word_idx == 24'd7) state <= S_COUNT;
			end
		end

		S_COUNT: begin
			if (!pending_valid && !ddr_write) begin
				// Bumping the counter is what publishes the save. Doing it
				// last means a partial payload is never persisted.
				word_idx      <= 24'd0;
				pending_word  <= 32'd1;
				pending_valid <= 1'b1;
				state         <= S_DONE;
			end
		end

		S_DONE: begin
			if (!pending_valid && !ddr_write) begin
				save_ok   <= 1'b1;
				save_busy <= 1'b0;
				state     <= S_IDLE;
			end
		end

		S_FAIL: begin
			save_fail <= 1'b1;
			save_busy <= 1'b0;
			state     <= S_IDLE;
		end

		default: state <= S_IDLE;
		endcase
	end
end

endmodule
```

> **Note on the counter value.** The testbench expects `1`. On hardware the
> counter must differ from whatever the host last saw, and the host initialises
> it to `0xFFFFFFFF` (`user_io.cpp:2003`). Writing a constant `1` is correct for
> the first save of a session; Task 9 replaces it with an incrementing register
> so repeated saves to the same slot are each noticed.

- [ ] **Step 4: Run the test to verify it passes**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_ctrl_tb rtl/ss_ctrl.v rtl/ss_crc32.v rtl/ss_quiesce.v rtl/ss_serdes.v rtl/ss_dma.v rtl/tb/ss_ctrl_tb.v && vvp build/ss_ctrl_tb
```
Expected: thirteen `PASS` lines then `ALL TESTS PASSED`

- [ ] **Step 5: Add to CI**

In `.github/workflows/build.yml`, append to the `rtl-sim` job:

```yaml
      - name: Build save state controller testbench
        run: |
          iverilog -g2005 -Wall -o build/ss_ctrl_tb \
            rtl/ss_ctrl.v rtl/ss_crc32.v rtl/ss_quiesce.v rtl/ss_serdes.v \
            rtl/ss_dma.v rtl/tb/ss_ctrl_tb.v

      - name: Run save state controller testbench
        run: vvp build/ss_ctrl_tb
```

- [ ] **Step 6: Copy to the core and commit**

```bash
cp rtl/ss_ctrl.v upstream/core-cd32-cdtv/rtl/ss_ctrl.v
git add rtl/ss_ctrl.v rtl/tb/ss_ctrl_tb.v .github/workflows/build.yml
git commit -m "feat: save state controller sequencing quiesce, capture and commit"
```

---

### Task 7: TG68K architectural state export

The only VHDL change in the design, and the only place the Verilog/VHDL boundary is crossed. `regin_out` is the register file's **write-data** bus and is no use for reading state out; a read port is needed.

**Files:**
- Modify: `upstream/core-cd32-cdtv/rtl/tg68k/TG68KdotC_Kernel.vhd`
- Modify: `upstream/core-cd32-cdtv/rtl/cpu_wrapper.v`

**Interfaces:**
- Produces on `TG68KdotC_Kernel`: `ss_reg_index : in std_logic_vector(3 downto 0)`, `ss_reg_data : out std_logic_vector(31 downto 0)`, `ss_pc : out std_logic_vector(31 downto 0)`, `ss_sr : out std_logic_vector(15 downto 0)`, `ss_usp : out std_logic_vector(31 downto 0)`.
- Produces on `cpu_wrapper`: `ss_reg_index[3:0]` input, `ss_reg_data[31:0]`, `ss_pc[31:0]`, `ss_sr[15:0]`, `ss_usp[31:0]`, `ss_vbr[31:0]`, `ss_cacr[3:0]` outputs.
- `ss_reg_index` selects `regfile(0..7)` = D0–D7, `regfile(8..15)` = A0–A7.
- `ss_sr` is `FlagsSR & Flags` — high byte `T.S.0III`, low byte `...XNZVC`, per the comments at `TG68KdotC_Kernel.vhd:241-242`.
- `ss_vbr` and `ss_cacr` reuse the kernel's existing `VBR_out` and `CACR_out` ports, already wired at `cpu_wrapper.v:238` and `:241`.

- [ ] **Step 1: Understand what verifies this task**

There is no VHDL simulation available: Icarus does not read VHDL, and ModelSim
is not installed on the dev machine (see the comment at
`.github/workflows/build.yml:95`). So this task has **no unit test**, and
pretending otherwise would be worse than saying so.

Its verification is therefore explicit and deferred: the Quartus
analysis-and-synthesis pass in Step 5 proves it elaborates, and Task 10's HRTmon
comparison proves the exported values are the real ones — a wrong register
index or a stale `Flags` mapping shows up there as a register that fails to
round-trip. Do not mark Task 10 complete without checking the CPU registers
specifically.

- [ ] **Step 2: Add the export ports to the kernel entity**

In `upstream/core-cd32-cdtv/rtl/tg68k/TG68KdotC_Kernel.vhd`, add to the port list after `VBR_out` (line 142):

```vhdl
		VBR_out					: out std_logic_vector(31 downto 0);

		-- Save state export. Read-only view of the architectural registers,
		-- valid whenever the CPU is stopped at an instruction boundary.
		ss_reg_index			: in  std_logic_vector( 3 downto 0) := (OTHERS => '0');
		ss_reg_data				: out std_logic_vector(31 downto 0);
		ss_pc						: out std_logic_vector(31 downto 0);
		ss_sr						: out std_logic_vector(15 downto 0);
		ss_usp					: out std_logic_vector(31 downto 0)
```

Note the existing `VBR_out` line has no trailing semicolon because it is last; add one when appending.

- [ ] **Step 3: Drive the export signals**

In the same file, immediately after the register file process ends (the `END PROCESS;` at line 577), add:

```vhdl
-----------------------------------------------------------------------------
-- Save state export
-----------------------------------------------------------------------------
-- A second, asynchronous read port on the register file. The savestate
-- controller sweeps ss_reg_index while the CPU is frozen, so there is no
-- contention with RDindex_A/RDindex_B and no timing path into the execute
-- pipeline.
ss_reg_data <= regfile(conv_integer(ss_reg_index));
ss_pc       <= TG68_PC;
ss_sr       <= FlagsSR & Flags;
ss_usp      <= USP;
```

- [ ] **Step 4: Plumb the export through cpu_wrapper**

In `upstream/core-cd32-cdtv/rtl/cpu_wrapper.v`, add to the module port list (after `input [2:0] fastramcfg,` at line 37):

```verilog
	input       [3:0] ss_reg_index,
	output     [31:0] ss_reg_data,
	output     [31:0] ss_pc,
	output     [15:0] ss_sr,
	output     [31:0] ss_usp,
	output     [31:0] ss_vbr,
	output      [3:0] ss_cacr,
```

Then in the `TG68KdotC_Kernel` instance, replace:

```verilog
  .vbr_out(vbr_p)
);
```

with:

```verilog
  .vbr_out(vbr_p),

  .ss_reg_index(ss_reg_index),
  .ss_reg_data(ss_reg_data),
  .ss_pc(ss_pc),
  .ss_sr(ss_sr),
  .ss_usp(ss_usp)
);

// VBR and CACR are already brought out of the kernel for the memory map, so
// the savestate view reuses them rather than adding ports.
assign ss_vbr  = vbr_p;
assign ss_cacr = cacr_p;
```

- [ ] **Step 5: Verify it compiles**

Run a Quartus analysis-and-synthesis pass:
```bash
cd upstream/core-cd32-cdtv && quartus_map Minimig
```
Expected: `Analysis & Synthesis was successful` with no new warnings mentioning `ss_reg_index`, `ss_reg_data`, `ss_pc`, `ss_sr` or `ss_usp`

- [ ] **Step 6: Commit**

```bash
cd upstream/core-cd32-cdtv
git add rtl/tg68k/TG68KdotC_Kernel.vhd rtl/cpu_wrapper.v
git commit -m "feat: export TG68K architectural state for save states"
```

---

### Task 8: Core integration

Wires the freeze into the clock enables, defines the Phase 1A state vector, instantiates `ss_ctrl`, and gives it a DDR3 port.

**Files:**
- Create: `rtl/ss_state.vh`, copied to `upstream/core-cd32-cdtv/rtl/ss_state.vh`
- Modify: `upstream/core-cd32-cdtv/rtl/ddram_ctrl.v`
- Modify: `upstream/core-cd32-cdtv/rtl/minimig.v`
- Modify: `upstream/core-cd32-cdtv/Minimig.sv`

**Interfaces:**
- Consumes: `ss_ctrl` (Task 6), `cpu_wrapper`'s `ss_*` outputs (Task 7)
- Produces: `ss_freeze` in `minimig.v`, gating `clk7_en` and `clk7n_en`; `mem3_*` ports on `ddram_ctrl`

- [ ] **Step 1: Define the Phase 1A state vector in a shared include**

The vector is built in `minimig.v` but its width is needed in `Minimig.sv` to
size the `ss_ctrl` instance, and a `localparam` in one module is not visible in
the other. Both therefore include one file, and the width is a macro rather than
a parameter so there is no scope question at all.

Create `rtl/ss_state.vh` (and copy it to `upstream/core-cd32-cdtv/rtl/`):

```verilog
// ---------------------------------------------------------------------------
// Save state vector, Phase 1A.
//
// One macro defines both directions. A register that appears here is captured
// and restored; one that does not appear is neither. Adding to the save side
// alone is impossible, because both sides expand from this list and a width
// mismatch is a synthesis error.
//
// SS_STATE_W must equal the total width of SS_STATE_LIST. Keeping them in one
// file makes that a single-line edit rather than two edits in two modules.
//
// Phase 1B extends this with the chipset inventory (Agnus, Denise, Paula, CIA,
// Akiko). Phase 1A carries only what is needed to prove the framework: the
// CPU's architectural registers and Gary's memory map state.
// ---------------------------------------------------------------------------

`define SS_STATE_LIST { \
	ss_cpu_d0, ss_cpu_d1, ss_cpu_d2, ss_cpu_d3, \
	ss_cpu_d4, ss_cpu_d5, ss_cpu_d6, ss_cpu_d7, \
	ss_cpu_a0, ss_cpu_a1, ss_cpu_a2, ss_cpu_a3, \
	ss_cpu_a4, ss_cpu_a5, ss_cpu_a6, ss_cpu_a7, \
	ss_pc, ss_usp, ss_vbr, ss_sr, ss_cacr, \
	ovl, rom_readonly, sel_kick1mb, sel_kick256kmirror }

// 16 registers + PC + USP + VBR (32 each) + SR (16) + CACR (4) + 4 map bits
`define SS_STATE_W (16*32 + 32 + 32 + 32 + 16 + 4 + 4)
```

Add `` `include "ss_state.vh" `` at the top of both `rtl/minimig.v` and
`Minimig.sv`.

- [ ] **Step 2: Capture the CPU register file into flops**

The export is a single read port, so the sixteen registers are sampled one per
cycle while the machine is frozen. Add to `minimig.v`:

```verilog
// Sweep the TG68K register file read port while the CPU is frozen. Sixteen
// cycles at 114 MHz is 140 ns, which is nothing against the freeze itself.
reg  [3:0]  ss_reg_index;
reg  [31:0] ss_cpu_d0, ss_cpu_d1, ss_cpu_d2, ss_cpu_d3;
reg  [31:0] ss_cpu_d4, ss_cpu_d5, ss_cpu_d6, ss_cpu_d7;
reg  [31:0] ss_cpu_a0, ss_cpu_a1, ss_cpu_a2, ss_cpu_a3;
reg  [31:0] ss_cpu_a4, ss_cpu_a5, ss_cpu_a6, ss_cpu_a7;
reg         ss_regs_valid;

always @(posedge clk) begin
	if (!ss_freeze) begin
		ss_reg_index  <= 4'd0;
		ss_regs_valid <= 1'b0;
	end
	else if (!ss_regs_valid) begin
		case (ss_reg_index)
		4'd0:  ss_cpu_d0 <= ss_reg_data;
		4'd1:  ss_cpu_d1 <= ss_reg_data;
		4'd2:  ss_cpu_d2 <= ss_reg_data;
		4'd3:  ss_cpu_d3 <= ss_reg_data;
		4'd4:  ss_cpu_d4 <= ss_reg_data;
		4'd5:  ss_cpu_d5 <= ss_reg_data;
		4'd6:  ss_cpu_d6 <= ss_reg_data;
		4'd7:  ss_cpu_d7 <= ss_reg_data;
		4'd8:  ss_cpu_a0 <= ss_reg_data;
		4'd9:  ss_cpu_a1 <= ss_reg_data;
		4'd10: ss_cpu_a2 <= ss_reg_data;
		4'd11: ss_cpu_a3 <= ss_reg_data;
		4'd12: ss_cpu_a4 <= ss_reg_data;
		4'd13: ss_cpu_a5 <= ss_reg_data;
		4'd14: ss_cpu_a6 <= ss_reg_data;
		4'd15: ss_cpu_a7 <= ss_reg_data;
		endcase
		if (ss_reg_index == 4'd15) ss_regs_valid <= 1'b1;
		ss_reg_index <= ss_reg_index + 4'd1;
	end
end

output wire [`SS_STATE_W-1:0] ss_state_in;
assign ss_state_in = `SS_STATE_LIST;
```

Add `ss_state_in` to `minimig.v`'s port list so `Minimig.sv` can reach it.

- [ ] **Step 3: Gate the clock enables**

In `upstream/core-cd32-cdtv/rtl/minimig.v`, find where `clk7_en` and `clk7n_en`
enter the module and rename the incoming ports to `clk7_en_i` / `clk7n_en_i`,
then add:

```verilog
// Freezing the machine means holding every clock enable low. The Amiga side is
// entirely enable-driven off clk_114, so there is no clock gate, no PLL work
// and no clock-domain crossing here -- the machine simply stops between ticks.
//
// The RTC is deliberately not gated: it runs off raw clk (see the always block
// around minimig.v:963) and must keep ticking so a state resumed tomorrow sees
// tomorrow's time.
wire clk7_en  = clk7_en_i  & ~ss_freeze;
wire clk7n_en = clk7n_en_i & ~ss_freeze;
```

Apply the same gating to the CPU enables in `Minimig.sv` where `cpu_wrapper`'s
`ph1`, `ph2` and the TG68K `clkena_in` are produced.

- [ ] **Step 4: Borrow DDR3 master 0 while frozen**

Master 0 of `a2065_ddram_arbiter` is the Minimig fast-RAM controller, which
cannot issue a request while the machine is frozen — the same argument that lets
`ss_dma` borrow the SDRAM CPU port. So the savestate writer is muxed onto m0
rather than added as a third master, and the arbiter is not touched.

In `upstream/core-cd32-cdtv/rtl/ddram_ctrl.v`, add to the module port list:

```verilog
	// Save state writer. Takes over master 0 while ss_freeze is asserted; the
	// fast-RAM path is idle then, because the CPU is stopped.
	input             ss_freeze,
	input      [28:0] ss_address,
	input      [63:0] ss_writedata,
	input       [7:0] ss_byteenable,
	input             ss_write,
	output            ss_waitrequest,
```

Then in the `a2065_ddram_arbiter` instance (line 242), replace the `m0_*`
connections with muxed versions:

```verilog
	.m0_address      (ss_freeze ? ss_address     : ram_addr),
	.m0_burstcount   (8'd1),
	.m0_read         (ss_freeze ? 1'b0           : ram_rd),
	.m0_readdata     (),
	.m0_readdatavalid(ram_dout_ready),
	.m0_writedata    (ss_freeze ? ss_writedata   : ram_din),
	.m0_byteenable   (ss_freeze ? ss_byteenable  : ram_be),
	.m0_write        (ss_freeze ? ss_write       : ram_we),
	.m0_waitrequest  (m0_waitrequest_int),
```

and add:

```verilog
wire m0_waitrequest_int;
assign ram_busy       = ss_freeze ? 1'b1 : m0_waitrequest_int;
assign ss_waitrequest = ss_freeze ? m0_waitrequest_int : 1'b1;
```

Holding `ram_busy` high while frozen is belt and braces: the fast-RAM path
should never ask, and if a future change makes it ask, it stalls rather than
corrupting the savestate stream.

- [ ] **Step 5: Instantiate the controller**

In `upstream/core-cd32-cdtv/Minimig.sv`, after the `ddram_ctrl ram2` instance
(line 579):

```systemverilog
// Save state window: 0x3E000000, four 4 MB slots. DDRAM_ADDR is a 64-bit word
// address, so the byte base is shifted right by three.
localparam [28:0] SS_SLOT_BASE = 29'h3E000000 >> 3;
localparam [28:0] SS_SLOT_STRIDE = 29'h400000 >> 3;

ss_ctrl #(.STATE_W(`SS_STATE_W), .CHIP_WORDS(24'h100000)) savestate
(
	.clk          (clk_114),
	.rst_n        (~reset_d),
	.slot_base    (SS_SLOT_BASE + SS_SLOT_STRIDE * ss_slot),
	.save_req     (ss_save_req),
	.save_busy    (ss_save_busy),
	.save_ok      (ss_save_ok),
	.save_fail    (ss_save_fail),
	.state_in     (ss_state_in),
	.state_out    (ss_state_out),
	.state_we     (ss_state_we),
	.blit_busy    (ss_blit_busy),
	.disk_busy    (ss_disk_busy),
	.audio_busy   (ss_audio_busy),
	.cpu_boundary (ss_cpu_boundary),
	.frame_tick   (ss_frame_tick),
	.freeze       (ss_freeze),
	.chip_base    (24'h000000),
	.sd_addr      (ss_sd_addr),
	.sd_cs        (ss_sd_cs),
	.sd_state     (ss_sd_state),
	.sd_uds_n     (ss_sd_uds_n),
	.sd_lds_n     (ss_sd_lds_n),
	.sd_cache_inhibit(ss_sd_cache_inhibit),
	.sd_rd        (ss_sd_rd),
	.sd_ready     (ss_sd_ready),
	.ddr_address  (ss_ddr_address),
	.ddr_writedata(ss_ddr_writedata),
	.ddr_byteenable(ss_ddr_byteenable),
	.ddr_write    (ss_ddr_write),
	.ddr_read     (ss_ddr_read),
	.ddr_readdata (ss_ddr_readdata),
	.ddr_readdatavalid(ss_ddr_readdatavalid),
	.ddr_waitrequest(ss_ddr_waitrequest)
);
```

`ss_cpu_boundary` is `cpu_state == 2'd1` ("no memaccess", per the comment at
`cpu_wrapper.v:236`). `ss_blit_busy` comes from Agnus's `blit_busy`
(`agnus.v:236`). `ss_frame_tick` is the rising edge of vertical blank.

While the machine is frozen, `ss_sd_*` must be multiplexed onto `sdram_ctrl`'s
CPU port in place of the CPU's own signals:

```systemverilog
wire        ram_cs_mux  = ss_freeze ? ss_sd_cs      : (~zram_sel & ram_cs);
wire [24:1] ram_addr_mux = ss_freeze ? ss_sd_addr   : ram_address;
wire  [1:0] ram_state_mux = ss_freeze ? ss_sd_state : cpu_state;
wire        ram_uds_mux = ss_freeze ? ss_sd_uds_n   : ram_uds;
wire        ram_lds_mux = ss_freeze ? ss_sd_lds_n   : ram_lds;
assign ss_sd_rd    = ram_dout1;
assign ss_sd_ready = ram_ready1;
```

- [ ] **Step 6: Declare save states in the config string**

In `upstream/core-cd32-cdtv/Minimig.sv:37`, change:

```systemverilog
	"Minimig;UART115200:230400,MIDI;",
```

to:

```systemverilog
	"Minimig;UART115200:230400,MIDI,SS3E000000:400000;",
```

Config string field 1 is a comma-separated token list — the parser advances with
`p = strchr(p, ','); if (p) p++;` at `user_io.cpp:853` — so appending a token is
all that is required.

- [ ] **Step 7: Verify it fits and meets timing**

Run:
```bash
cd upstream/core-cd32-cdtv && quartus_sh --flow compile Minimig
```
Expected: `Fitter Status : Successful`. Then record worst-case setup slack:
```bash
grep -A3 "Worst-case setup slack" upstream/core-cd32-cdtv/output_files/Minimig.sta.rpt
```
Expected: slack **≥ 0.000 ns**. If negative, stop — phase 0's `sd_addr` fan-in
fix has not bought enough headroom and no further savestate work should land
until it has.

- [ ] **Step 8: Verify the host sees the declaration**

Deploy the core and binary, then check the MiSTer console log.
Expected: `Got save state parameters: base=0x3E000000, size=0x400000`

- [ ] **Step 9: Commit**

```bash
git add rtl/ss_state.vh
git commit -m "feat: shared save state vector definition"
cp rtl/ss_state.vh upstream/core-cd32-cdtv/rtl/ss_state.vh
cd upstream/core-cd32-cdtv
git add rtl/ss_state.vh rtl/ddram_ctrl.v rtl/minimig.v Minimig.sv
git commit -m "feat: wire the save state controller into the AmigaCD core"
```

---

### Task 9: OSD entries and slot handling

**Files:**
- Modify: `upstream/core-cd32-cdtv/Minimig.sv`
- Modify: `menu.cpp`

**Interfaces:**
- Consumes: `ss_ctrl` (Task 6), core integration (Task 8)
- Produces: `ss_slot[1:0]`, `ss_save_req`, `ss_load_req` driven from OSD selections

- [ ] **Step 1: Replace the constant counter with an incrementing one**

In `rtl/ss_ctrl.v`, replace the `S_COUNT` state's constant with a register, so a
second save to the same slot is noticed by the host poller:

```verilog
reg [31:0] save_counter;
```

initialised to `32'd0` in the reset branch, and in `S_COUNT`:

```verilog
		S_COUNT: begin
			if (!pending_valid && !ddr_write) begin
				// The counter must differ from whatever the host last saw. The
				// host initialises it to 0xFFFFFFFF (user_io.cpp:2003), so any
				// increment from zero is distinguishable.
				word_idx      <= 24'd0;
				save_counter  <= save_counter + 32'd1;
				pending_word  <= save_counter + 32'd1;
				pending_valid <= 1'b1;
				state         <= S_DONE;
			end
		end
```

- [ ] **Step 2: Update the controller testbench**

In `rtl/tb/ss_ctrl_tb.v`, the existing check already expects `1` for the first
save. Add a second save and check the counter increments:

```verilog
	// A second save to the same slot must publish a different counter, or the
	// host poller will not notice it.
	@(negedge clk); save_req = 1;
	wait (save_ok);
	@(negedge clk);
	check("counter increments", ddr_word(0), 32'd2);
	save_req = 0;
```

- [ ] **Step 3: Run the testbench**

Run:
```bash
iverilog -g2005 -Wall -o build/ss_ctrl_tb rtl/ss_ctrl.v rtl/ss_crc32.v rtl/ss_quiesce.v rtl/ss_serdes.v rtl/ss_dma.v rtl/tb/ss_ctrl_tb.v && vvp build/ss_ctrl_tb
```
Expected: fourteen `PASS` lines then `ALL TESTS PASSED`

- [ ] **Step 4: Add the OSD entries**

In `menu.cpp`, in `case MENU_MINIMIG_CHIPSET1:` (line 6226), add three rows
after the HRTmon row at `menusub == 10`. Follow the existing idiom exactly:
`OsdWrite(m++, s, menusub == N, disabled)`, where the fourth argument greys the
row out.

Renumber the two rows that follow — Ethernet moves from 11 to 14, `STD_BACK`
from 12 to 15 — and widen `menumask` from `0x1FFF` to `0xFFFF`.

```cpp
		OsdWrite(m++, "", 0, 0);

		// Save states. Four slots, matching the four DDR3 windows process_ss()
		// maps. The core is the active party in the protocol, so these rows
		// only set request bits; the capture itself is entirely in RTL.
		//
		// The window at 0x3E000000 lives inside the same DDR3 region the core
		// maps Z3 fast RAM into (ddram_ctrl.v:307), so the two cannot coexist.
		// Fast RAM index >= 4 means a Z3 board is configured -- see
		// cpu_wrapper.v:500, where ac_memcard[2] selects the 128/256 MB Z3
		// autoconfig entries.
		{
			int fastram = ((minimig_config.memory >> 4) & 0x03)
			            | ((minimig_config.memory & 0x80) >> 5);
			int ss_blocked = (fastram >= 4);

			strcpy(s, " Save slot : ");
			strcat(s, config_ss_slot_msg[minimig_config.ss_slot & 3]);
			OsdWrite(m++, s, menusub == 11, ss_blocked);

			OsdWrite(m++, " Save state", menusub == 12, ss_blocked);
			OsdWrite(m++, " Restore state", menusub == 13, ss_blocked);
		}
```

Add the slot label table near the other `config_*_msg` tables:

```cpp
static const char *config_ss_slot_msg[] = { "1", "2", "3", "4" };
```

Add `unsigned char ss_slot;` to `mm_configTYPE` in
`support/minimig/minimig_config.h`, at the end of the struct so the on-disk
config layout stays backward compatible.

In the key handler for this menu page, handle the three new `menusub` values:
slot cycles `0..3`, and the two actions pulse the core's request bits through
the same status mechanism the page's other rows use.

- [ ] **Step 5: Verify on hardware**

Open the OSD in the AmigaCD core and confirm the three entries appear, the slot
selector cycles 1 through 4, and selecting **Save state** produces the host's
`Saving the state` toast within one second.

- [ ] **Step 6: Commit**

```bash
git add menu.cpp rtl/ss_ctrl.v rtl/tb/ss_ctrl_tb.v
git commit -m "feat: save state OSD entries and per-slot counters"
cd upstream/core-cd32-cdtv && git add rtl/ss_ctrl.v Minimig.sv && git commit -m "feat: save state OSD wiring"
```

---

### Task 10: Hardware bring-up

Phase 1A's acceptance gate. The test program makes the round-trip observable
without needing the chipset inventory that Phase 1B adds.

**Files:**
- Create: `docs/savestates-bringup.md`

- [ ] **Step 1: Enable HRTmon**

No custom test program or cross-assembler is needed: this core already contains
a machine-state monitor. `rtl/cart.v` is an HRTmon implementation, and it is
switched on from the OSD — `MENU_MINIMIG_CHIPSET1` row `menusub == 10`, backed
by `minimig_config.memory & 0x40` (`menu.cpp:6289`).

HRTmon can read and write both chip RAM and the CPU's architectural registers,
which is exactly and only what Phase 1A captures. That makes it a better bring-up
vehicle than a bespoke program: no toolchain, no loader, and it observes the two
things under test directly.

Enable HRTmon in the OSD and confirm its hotkey drops you into the monitor.

- [ ] **Step 2: Run the round-trip test**

1. Boot the AmigaCD core with HRTmon enabled.
2. Enter HRTmon. Write a recognisable longword to a chip RAM address well clear
   of anything in use — `$00080000` is inside 2 MB chip RAM and above the usual
   working set. Write `$DEADBEEF` there. Set `D0` to `$11111111` and `A3` to
   `$22222222`. Note all three.
3. Exit HRTmon so the machine resumes.
4. OSD → Save state, slot 1. Confirm the host's `Saving the state` toast.
5. Enter HRTmon again. Overwrite `$00080000` with `$00000000`, `D0` with
   `$99999999`, `A3` with `$88888888`. Exit.
6. OSD → Restore state, slot 1.
7. Enter HRTmon and read all three back.

Expected: `$00080000` reads `$DEADBEEF`, `D0` reads `$11111111`, `A3` reads
`$22222222`.

Checking a data register **and** an address register matters: they are separate
halves of the `regfile` index range in Task 7 (`0..7` versus `8..15`), and an
off-by-eight in the sweep would pass a D-register-only test.

- [ ] **Step 3: Verify the file on disk**

```bash
ls -l /media/fat/savestates/Minimig/
xxd -l 32 /media/fat/savestates/Minimig/<name>.ss1
```
Expected: a file of `(6 + core_words + 2) * 4` bytes; the first 32 bytes show
counter, length, `41 43 53 53` (`ACSS`), then version `00 00 01 00`.

- [ ] **Step 4: Verify across a power cycle**

Repeat step 2 up to and including the save, then power cycle the MiSTer, reload
the AmigaCD core, and restore slot 1.

Expected: `$00080000`, `D0` and `A3` all read back their saved values. **This is
Phase 1A's gate.**

- [ ] **Step 5: Measure the freeze against video**

Watch the display through a save. The freeze stops the beam counter, which stops
vsync, and some televisions resynchronise noisily after even a brief loss.

Record the observed behaviour in `docs/savestates-bringup.md`: how long the
freeze lasts, and whether the display glitches, flashes or resyncs. If a TV
drops sync, that is a real finding for Phase 1B — it would argue for keeping the
video pipeline running off an ungated enable during the freeze, the same way the
RTC already is.

- [ ] **Step 6: Record the timing result**

```bash
grep -A3 "Worst-case setup slack" upstream/core-cd32-cdtv/output_files/Minimig.sta.rpt
```
Record the figure in `docs/savestates-bringup.md` alongside the pre-savestate
baseline. Per the spec, a phase that does not report slack has not been
verified.

- [ ] **Step 7: Commit**

```bash
git add docs/savestates-bringup.md
git commit -m "docs: phase 1A save state bring-up results"
```

---

## What Phase 1B adds

For the reader picking this up after 1A lands:

1. The chipset inventory — Agnus, Denise, Paula, CIA A and B, Akiko — added to
   `` `SS_STATE_LIST `` one chip at a time, each its own task, each verified by
   the `ss_serdes` round-trip bench before hardware. Each chip's list is audited
   against WinUAE's corresponding `.uss` chunk, per the spec's inventory-oracle
   check; the WinUAE source is checked out at
   `C:\Users\Admin\Documents\mcp-winuae-emu\src`.
2. The **restore** path through `ss_serdes`'s load side, which Phase 1A builds
   but only exercises in simulation.
3. Kickstart CRC capture and the mismatch refusal.
4. The userspace host block: CD session state from `akiko_cd32.cpp`, the
   `mm_configTYPE` fingerprint, and the three-way restore outcome.
5. The refusal and warning paths, tested after the happy path works.
