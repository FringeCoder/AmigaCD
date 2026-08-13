`timescale 1ns/1ns

// Drives a save through ss_ctrl against a mock DDR3 and a mock SDRAM, then
// checks the header words and payload the host parser will validate.
//
// core_crc32 is checked against an independently computed reference, not
// just structurally assumed correct. ss_crc32 itself is already proven
// correct against known vectors (ss_crc32_tb.v); what this bench needs to
// catch is ss_ctrl feeding it the wrong bytes, the wrong order, or gaps that
// corrupt the running value -- exactly the kind of bug a final-result-only
// check can miss if it happens to be self-consistent (see this module's
// header for the two real bugs of that shape found while building it). This
// also exercises the wr-deasserted-without-init concern directly: ss_ctrl
// only calls crc init once per save, and every word after the first state
// word gets fed to the CRC with wr low (and no init) for the cycles it
// spends waiting on the DDR3 write or the next chip DMA pair. A wrong CRC
// here would mean either that gap handling is broken, or that a word was
// duplicated/dropped/reordered.

module ss_ctrl_tb;

localparam STATE_W    = 64;
localparam CHIP_WORDS = 8;

// Kickstart fingerprint. A region distinct from chip RAM (which lives at word
// addresses 0..7 here), so a scan that walked the wrong base would fingerprint
// chip RAM and be caught rather than accidentally agreeing with itself.
localparam KICK_WORDS = 8;
localparam [23:0] KICK_BASE = 24'h000080;

localparam STATE_WORDS = (STATE_W + 31) / 32;

// Payload word 0 is the Kickstart CRC, then the state vector, then chip RAM.
localparam CORE_WORDS  = 1 + STATE_WORDS + CHIP_WORDS / 2;

reg  clk = 0;
reg  rst_n = 0;

reg  save_req = 0;
reg  load_req = 0;
wire load_busy, load_ok, load_fail;
wire [3:0] load_fail_code;
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
wire [15:0] sd_wr;
reg  [15:0] sd_rd = 0;
reg         sd_ready = 0;

integer errors = 0;
integer latency = 0;
integer i;

// Chip RAM as the restore leaves it. The read model below is a pure function
// of the address and is deliberately NOT fed from this array: it is a
// write-capture buffer only, pre-poisoned so that a restore which never wrote
// chip RAM back leaves the poison visible rather than the value it was
// supposed to write.
reg [15:0] chip_mem [0:CHIP_WORDS-1];

// Mock DDR3: a flat memory of 64-bit words, addressed by ddr_address.
reg [63:0] ddr_mem [0:63];

// A byte-for-byte copy of the window the save leaves behind, so every refusal
// case below starts from a known-good file and corrupts exactly one field.
reg [63:0] win_golden [0:63];

wire rom_scan;

// Short enough to simulate, long enough that it cannot fire on the mock's own
// two-cycle latency. The synthesised default is 65536.
localparam [23:0] SCAN_TIMEOUT = 24'd64;

ss_ctrl #(.STATE_W(STATE_W), .CHIP_WORDS(CHIP_WORDS), .KICK_WORDS(KICK_WORDS),
          .SCAN_TIMEOUT(SCAN_TIMEOUT)) dut
(
	.clk(clk), .rst_n(rst_n),
	.slot_base(29'd0),
	.save_req(save_req), .save_busy(save_busy),
	.save_ok(save_ok), .save_fail(save_fail),
	.load_req(load_req), .load_busy(load_busy),
	.load_ok(load_ok), .load_fail(load_fail),
	.load_fail_code(load_fail_code),
	.state_in(state_in), .state_out(state_out), .state_we(state_we),
	.blit_busy(blit_busy), .disk_busy(1'b0), .audio_busy(1'b0),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze),
	.chip_base(24'h000000),
	.kick_base(KICK_BASE), .rom_scan(rom_scan),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_wr(sd_wr), .sd_rd(sd_rd), .sd_ready(sd_ready),
	.ddr_address(ddr_address), .ddr_writedata(ddr_writedata),
	.ddr_byteenable(ddr_byteenable), .ddr_write(ddr_write),
	.ddr_read(ddr_read), .ddr_readdata(ddr_readdata),
	.ddr_readdatavalid(ddr_readdatavalid), .ddr_waitrequest(ddr_waitrequest)
);

always #5 clk = ~clk;

// Mock SDRAM: contents are 0xD000 + address. Latency is two extra wait
// cycles beyond the first opportunity, comparable to a real cache fill, and
// slow enough relative to the CRC feeder's four-cycles-per-word rate that a
// design which merely happened to survive a zero-latency mock would not be
// exercised any harder here -- this bench is about correctness of the
// sequencing, not about hunting the tightest possible timing corner
// (ss_dma_tb.v already covers ss_dma's own protocol exhaustively).
//
// Writes are captured too: sd_state 3 is "write data" (sdram_ctrl.v:120), so
// the same accept instant that would return a word on a read commits sd_wr to
// chip_mem instead. Keying the capture on sd_state, not merely on sd_cs, means
// a restore that drove the read encoding would write nothing at all and be
// caught rather than silently accepted.
//
// The Kickstart region is served out of the same mock, from a different value
// series, so a fingerprint pass that walked chip RAM by mistake would produce a
// different CRC than the reference below rather than quietly agreeing with
// itself. rom_alt swaps the ROM for a different one without touching the state
// file: that is exactly "a state made under a different Kickstart", which is
// the case the fingerprint exists to catch.
//
// sd_stall models the one failure this mock can produce that the real hardware
// can too: the CPU port is not routed here, so sd_ready never comes. Chip
// select is still accepted, so a controller with no bound on the wait sits in
// S_KICK_WAIT forever.
integer chip_wr_count = 0;

reg rom_alt = 0;
reg sd_stall = 0;

wire [23:0] sd_word  = sd_addr;
wire        in_kick  = (sd_word >= KICK_BASE) && (sd_word < KICK_BASE + KICK_WORDS);
wire [15:0] kick_off = sd_word[15:0] - KICK_BASE[15:0];

always @(posedge clk) begin
	sd_ready <= 1'b0;
	if (sd_cs && !sd_ready && !sd_stall) begin
		latency = latency + 1;
		if (latency >= 2) begin
			sd_rd    <= in_kick ? ((rom_alt ? 16'hE100 : 16'hE000) + kick_off)
			                    : (16'hD000 + {8'd0, sd_addr[8:1]});
			sd_ready <= 1'b1;
			latency  = 0;
			if (sd_state == 2'd3) begin
				chip_mem[sd_addr[3:1]] = sd_wr;
				chip_wr_count = chip_wr_count + 1;
			end
		end
	end
end

// Mock DDR3 write capture. Honours ddr_byteenable with a per-byte merge
// rather than overwriting the full 64-bit word: two 32-bit payload words
// share one 64-bit slot here (word_idx bit 0 selects the half), and a mock
// that ignored byteenable could not catch a controller that clobbers one
// half writing the other -- which is exactly the bug this bench exists to
// catch (see ss_ctrl.v's header for how "state word 0" read back as zero
// until ddr_byteenable was wired up for real).
always @(posedge clk) begin
	if (ddr_write && !ddr_waitrequest) begin
		if (ddr_byteenable[0]) ddr_mem[ddr_address[5:0]][7:0]   <= ddr_writedata[7:0];
		if (ddr_byteenable[1]) ddr_mem[ddr_address[5:0]][15:8]  <= ddr_writedata[15:8];
		if (ddr_byteenable[2]) ddr_mem[ddr_address[5:0]][23:16] <= ddr_writedata[23:16];
		if (ddr_byteenable[3]) ddr_mem[ddr_address[5:0]][31:24] <= ddr_writedata[31:24];
		if (ddr_byteenable[4]) ddr_mem[ddr_address[5:0]][39:32] <= ddr_writedata[39:32];
		if (ddr_byteenable[5]) ddr_mem[ddr_address[5:0]][47:40] <= ddr_writedata[47:40];
		if (ddr_byteenable[6]) ddr_mem[ddr_address[5:0]][55:48] <= ddr_writedata[55:48];
		if (ddr_byteenable[7]) ddr_mem[ddr_address[5:0]][63:56] <= ddr_writedata[63:56];
	end
end

// DDR3 arbiter backpressure. waitrequest is high one cycle in four, so
// neither direction can assume its first cycle is taken: a write has to hold
// ddr_write, and a read has to hold ddr_read until the cycle it is actually
// accepted on. A controller that issued a read and moved on regardless would
// either lose the request or take the following cycle's acceptance as a
// second one.
reg [1:0] stall_ctr = 0;

always @(posedge clk) begin
	stall_ctr       <= stall_ctr + 2'd1;
	ddr_waitrequest <= (stall_ctr == 2'd0);
end

// Mock DDR3 read service, out of the same array the writes above land in.
// Avalon-style: a read is accepted on the cycle `ddr_read` is high with
// `ddr_waitrequest` low, and the data comes back later on its own
// `ddr_readdatavalid` beat -- one cycle here. A controller that captured
// `ddr_readdata` on the accept rather than on the valid beat would read the
// previous word every time.
always @(posedge clk) begin
	ddr_readdatavalid <= 1'b0;
	if (ddr_read && !ddr_waitrequest) begin
		ddr_readdata      <= ddr_mem[ddr_address[5:0]];
		ddr_readdatavalid <= 1'b1;
	end
end

// Refusing a state file must never stop the Amiga. saw_freeze is sticky from
// the moment the restore tests begin: any gate that quiesced before deciding
// would trip it, and a validator that stops the machine to reject a file it
// was never going to load is worse than the corrupt file it was guarding
// against.
reg saw_freeze   = 0;
reg watch_freeze = 0;

always @(posedge clk) if (watch_freeze && freeze) saw_freeze <= 1'b1;

// The SDRAM CPU port is not ours to take. Minimig.sv routes it to ss_ctrl while
// the machine is frozen; the restore-side fingerprint scan needs it while the
// machine is still running, which is what rom_scan exists to announce. Any cycle
// of chip select outside those two windows is a cycle contending with the live
// 68k, and would corrupt the running machine rather than merely fail. Sticky for
// the whole simulation: one such cycle anywhere is a failure.
// One documented exception, and only one: when the ROM scan is abandoned on
// SCAN_TIMEOUT, ss_dma is still mid-transfer and holds its chip select up.
// ss_ctrl drops rom_scan anyway, because the whole point of the timeout is that
// the port was never being routed to it in the first place -- Minimig.sv muxes
// on (ss_freeze | ss_rom_scan), so nothing of that stuck select reaches the
// machine. watch_bus is lowered for exactly the cleanup window of that one
// test and raised again as soon as ss_dma has been let finish.
reg bus_stolen = 0;
reg watch_bus  = 1;

always @(posedge clk) if (watch_bus && sd_cs && !freeze && !rom_scan) bus_stolen <= 1'b1;

// ------------------------------------------------------- ordering observers
//
// A restore that puts the machine back together in the wrong order resumes and
// then misbehaves, which reads like a game bug rather than a save state bug.
// Checking that each event merely happened is not enough: the events are all
// present in a broken ordering too. So the counts are SNAPSHOT at the instant
// the freeze falls. "Chip RAM written before release" then means "all
// CHIP_WORDS writes had already been counted when the machine was let go",
// which is false for any design that writes even one word afterwards, and
// likewise for the state vector's state_we pulse.
integer state_we_count      = 0;
integer ddr_write_count     = 0;
integer freeze_fall_count   = 0;
integer freeze_rise_count   = 0;
integer chip_wr_at_release  = 0;
integer state_we_at_release = 0;
reg     freeze_d            = 0;

always @(posedge clk) if (state_we) state_we_count = state_we_count + 1;

always @(posedge clk) if (ddr_write && !ddr_waitrequest) ddr_write_count = ddr_write_count + 1;

always @(posedge clk) begin
	freeze_d <= freeze;
	if (freeze && !freeze_d) freeze_rise_count = freeze_rise_count + 1;
	if (!freeze && freeze_d) begin
		freeze_fall_count   = freeze_fall_count + 1;
		chip_wr_at_release  = chip_wr_count;
		state_we_at_release = state_we_count;
	end
end

task reset_observers;
begin
	chip_wr_count       = 0;
	state_we_count      = 0;
	ddr_write_count     = 0;
	freeze_fall_count   = 0;
	freeze_rise_count   = 0;
	chip_wr_at_release  = 0;
	state_we_at_release = 0;
end
endtask

// Read a 32-bit word out of the mock DDR3 by word index.
function [15:0] bswap16;
	input [15:0] v;
	bswap16 = {v[7:0], v[15:8]};
endfunction

// The mock's Kickstart contents, as a pure function of the word offset, so the
// reference CRC below is computed from the same definition the mock serves.
function [15:0] rom_word;
	input integer k;
	rom_word = (rom_alt ? 16'hE100 : 16'hE000) + k[15:0];
endfunction

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

// Overwrite one 32-bit word of the window, addressed the same way ddr_word()
// reads it: bit 0 of the index selects the half of the 64-bit DDR3 word.
task poke_word(input integer idx, input [31:0] v);
begin
	if (idx[0]) ddr_mem[idx/2][63:32] = v;
	else        ddr_mem[idx/2][31:0]  = v;
end
endtask

task restore_window;
	integer k;
begin
	for (k = 0; k < 64; k = k + 1) ddr_mem[k] = win_golden[k];
end
endtask

// Drive one restore attempt to completion. load_req is edge-triggered, so it
// is dropped once the controller has taken it; waiting on load_busy rather
// than on load_ok/load_fail keeps this robust against those two being levels
// that persist from the previous attempt.
task run_load;
	integer s;
begin
	@(negedge clk); load_req = 1;
	wait (load_busy);
	@(negedge clk); load_req = 0;
	wait (!load_busy);
	// A restore attempt is not over when the controller goes idle: ss_quiesce
	// drops `freeze` one cycle behind the request, so returning any earlier
	// would leave the machine still stopped and the falling edge uncounted.
	// (For a refusal `freeze` was never raised and this returns immediately.)
	@(negedge clk);
	wait (!freeze);
	// Then settle. Two cycles are needed because the edge observers below
	// sample `freeze` on posedge and so see a transition driven by another
	// posedge block one cycle after it happens; the rest is so that "the
	// restore wrote nothing to DDR3" is a real observation rather than a race
	// with a write that simply had not been accepted yet -- ddr_waitrequest is
	// high one cycle in four, so a queued write can take several cycles to
	// land. Without this a controller that bumped the counter on restore went
	// undetected (mutation M14).
	for (s = 0; s < 10; s = s + 1) @(negedge clk);
end
endtask

// Independent CRC-32 reference (same algorithm as ss_crc32.v, which is
// itself proven against known vectors in ss_crc32_tb.v). Computed here over
// the exact byte sequence the payload is expected to contain, so this
// checks that ss_ctrl presents the right bytes in the right order, not
// merely that some CRC came out.
reg [31:0] crc_acc;

function [31:0] crc_byte_step;
	input [31:0] c;
	input [7:0]  b;
	integer k;
	reg [31:0] cur;
begin
	cur = c ^ {24'd0, b};
	for (k = 0; k < 8; k = k + 1)
		cur = cur[0] ? ((cur >> 1) ^ 32'hEDB88320) : (cur >> 1);
	crc_byte_step = cur;
end
endfunction

task crc_word_step(input [31:0] w);
	integer k;
begin
	for (k = 0; k < 4; k = k + 1)
		crc_acc = crc_byte_step(crc_acc, w[k*8 +: 8]);
end
endtask

reg [31:0] expect_crc;
reg [31:0] expect_kick_crc;

// Reference fingerprint over the mock ROM, in the same 68k byte order the chip
// RAM payload uses: each Amiga word's high byte first. Computed here from the
// mock's own contents rather than read back out of the file, so a capture that
// scanned the wrong region, the wrong length, or in the wrong byte order fails
// rather than agreeing with itself.
task compute_kick_crc;
	integer k;
begin
	crc_acc = 32'hFFFFFFFF;
	for (k = 0; k < KICK_WORDS; k = k + 2)
		crc_word_step({bswap16(rom_word(k + 1)), bswap16(rom_word(k))});
	expect_kick_crc = ~crc_acc;
end
endtask

// A restore that never completes would otherwise park run_load() on its
// wait() forever with no output at all.
initial begin
	#500000;
	$display("FAIL watchdog: simulation did not finish");
	$fatal(1);
end

initial begin
	for (i = 0; i < 64; i = i + 1) ddr_mem[i] = 64'd0;
	for (i = 0; i < CHIP_WORDS; i = i + 1) chip_mem[i] = 16'hBAD0 + i[15:0];
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

	// Payload word 0 is the Kickstart fingerprint. It sits at the start of the
	// core payload rather than in the eight-word header, so the header contract
	// the host parser knows (minimig_savestate.h) is unchanged: the host sees
	// only a core_words that is one larger, which it already reads from the file
	// rather than assuming.
	compute_kick_crc();
	check("kickstart crc", ddr_word(8), expect_kick_crc);

	// State vector words, least significant first.
	check("state word 0", ddr_word(9),  32'h89ABCDEF);
	check("state word 1", ddr_word(10), 32'h01234567);

	// Chip RAM packed two 16-bit words per 32-bit word, low word first, in
	// ascending address order.
	// Chip RAM must land in the file in 68k byte order, so a state file can be
	// read directly as Amiga memory. Each SDRAM word's high byte belongs at the
	// lower file offset; without the swap every Amiga word reads reversed and
	// nothing in the payload is interpretable (measured on hardware,
	// 2026-08-10, on the first real save state).
	check("chip word 0", ddr_word(11), {bswap16(16'hD001), bswap16(16'hD000)});
	check("chip word 1", ddr_word(12), {bswap16(16'hD003), bswap16(16'hD002)});
	check("chip word 2", ddr_word(13), {bswap16(16'hD005), bswap16(16'hD004)});
	check("chip word 3", ddr_word(14), {bswap16(16'hD007), bswap16(16'hD006)});

	// The same two words again, as literals, against the EXTERNAL contract
	// rather than against bswap16(). The file's byte at offset 4*10+0 must be
	// the high byte of the Amiga word at chip address 0, and so on; a 32-bit
	// word is stored little-endian, so the four bytes D0,00,D0,01 read back as
	// 32'h01D000D0. Written out this way, capture and restore cannot agree with
	// each other on a swap that is wrong in both directions -- a matched pair
	// of errors round-trips perfectly but fails here.
	check("chip word 0 in 68k byte order", ddr_word(11), 32'h01D000D0);
	check("chip word 3 in 68k byte order", ddr_word(14), 32'h07D006D0);

	// core_crc32 against an independently computed reference over the exact
	// bytes above, least-significant byte of each word first. A design that
	// dropped, duplicated, reordered a word, or re-armed the CRC mid-stream
	// would produce a different value here even if every word above landed
	// in the right DDR3 slot.
	crc_acc = 32'hFFFFFFFF;
	crc_word_step(expect_kick_crc);
	crc_word_step(32'h89ABCDEF);
	crc_word_step(32'h01234567);
	crc_word_step({bswap16(16'hD001), bswap16(16'hD000)});
	crc_word_step({bswap16(16'hD003), bswap16(16'hD002)});
	crc_word_step({bswap16(16'hD005), bswap16(16'hD004)});
	crc_word_step({bswap16(16'hD007), bswap16(16'hD006)});
	expect_crc = ~crc_acc;
	check("core_crc32", ddr_word(6), expect_crc);

	check("counter bumped", ddr_word(0), 32'd1);

	save_req = 0;
	@(negedge clk); @(negedge clk);
	if (freeze !== 1'b0) begin
		$display("FAIL freeze not released");
		errors = errors + 1;
	end
	else $display("PASS freeze released");

	// ------------------------------------------------------------- restore
	//
	// The window sitting in the mock DDR3 is a real state file this very
	// module just wrote, so validating it is a genuine round trip at the
	// format level -- header, length arithmetic, payload CRC and all. A
	// hand-built window would only ever agree with whatever the validator
	// happened to check; this one agrees with what the save path actually
	// produces, which is the thing that has to be loadable.
	wait (!save_busy);
	@(negedge clk);
	for (i = 0; i < 64; i = i + 1) win_golden[i] = ddr_mem[i];

	// From here on state_in is a different value than the one that was saved,
	// so a state_out that comes back holding the ORIGINAL vector can only have
	// come out of the file. Without this a restore that simply latched its own
	// live input would pass.
	state_in = 64'hFEDCBA9876543210;

	// The counter belongs to the host's save handshake, not to us. Poking a
	// value the save path would never write makes "restore left word 0 alone"
	// checkable: if the restore touched it at all it would land back on 1.
	poke_word(0, 32'hA5A5A5A5);
	for (i = 0; i < 64; i = i + 1) win_golden[i] = ddr_mem[i];

	reset_observers();
	run_load();

	check("round trip validates",  {31'd0, load_ok},   32'd1);
	check("round trip no failure", {31'd0, load_fail}, 32'd0);

	// The vector that comes back out is the one that went in, not the live
	// state_in above.
	check("restored state low",  state_out[31:0],  32'h89ABCDEF);
	check("restored state high", state_out[63:32], 32'h01234567);

	// Chip RAM back in SDRAM, in SDRAM's own word order -- the inverse of the
	// 68k-order file contract asserted above, pinned to literals for the same
	// reason. chip_mem was poisoned with 0xBAD0+n, so an unwritten word is
	// unmistakable rather than accidentally right.
	check("chip RAM restored word 0", {16'd0, chip_mem[0]}, 32'h0000D000);
	check("chip RAM restored word 1", {16'd0, chip_mem[1]}, 32'h0000D001);
	check("chip RAM restored word 6", {16'd0, chip_mem[6]}, 32'h0000D006);
	check("chip RAM restored word 7", {16'd0, chip_mem[7]}, 32'h0000D007);

	// Chip RAM must be written before the CPU is released, and the state
	// vector must be applied before the freeze drops -- otherwise the CPU
	// executes against a half-restored machine. Both counts are snapshots
	// taken at the falling edge of freeze, so they establish the order, not
	// merely that the events occurred.
	check("chip RAM written before release", chip_wr_at_release,  CHIP_WORDS);
	check("regs loaded before unfreeze",     state_we_at_release, 32'd1);
	check("freeze dropped exactly once",     freeze_fall_count,   32'd1);
	check("freeze raised exactly once",      freeze_rise_count,   32'd1);

	// A restore is a read of the window, never a write. Anything else would
	// eventually reach the counter.
	check("restore writes no DDR3",  ddr_write_count, 32'd0);
	check("counter untouched",       ddr_word(0),     32'hA5A5A5A5);

	// Every refusal below must leave the machine running. saw_freeze is sticky
	// across all of them; the successful restores above and below are outside
	// the watched window, because those are supposed to freeze.
	saw_freeze   = 0;
	watch_freeze = 1;

	// Each gate refuses with its own code, and none of them may stop the
	// machine to do it.
	restore_window(); poke_word(2, 32'hDEADBEEF);
	run_load();
	check("bad magic refused",     {28'd0, load_fail_code}, 32'd1);
	check("bad magic not ok",      {31'd0, load_ok},        32'd0);
	check("bad magic no freeze",   {31'd0, saw_freeze},     32'd0);

	restore_window(); poke_word(3, 32'h00020000);
	run_load();
	check("bad version refused",   {28'd0, load_fail_code}, 32'd2);
	check("bad version not ok",    {31'd0, load_ok},        32'd0);
	check("bad version no freeze", {31'd0, saw_freeze},     32'd0);

	restore_window(); poke_word(1, 32'hFFFFFFFF);
	run_load();
	check("long length refused",   {28'd0, load_fail_code}, 32'd3);
	check("long length not ok",    {31'd0, load_ok},        32'd0);
	check("long length no freeze", {31'd0, saw_freeze},     32'd0);

	// The lower bound matters as much as the upper: a length that cannot even
	// cover this core's payload describes a file that is not ours.
	restore_window(); poke_word(1, 32'd5);
	run_load();
	check("short length refused",   {28'd0, load_fail_code}, 32'd3);
	check("short length no freeze", {31'd0, saw_freeze},     32'd0);

	// A payload of a different size is a different core's state file, even
	// when every other header word looks right.
	restore_window(); poke_word(4, CORE_WORDS + 32'd1);
	run_load();
	check("bad core_words refused", {28'd0, load_fail_code}, 32'd3);
	check("core_words no freeze",   {31'd0, saw_freeze},     32'd0);

	// One flipped bit anywhere in the payload, caught by the CRC and nothing
	// else -- every header word here is still perfectly well formed.
	restore_window(); poke_word(9, ddr_word(9) ^ 32'd1);
	run_load();
	check("bad payload refused",    {28'd0, load_fail_code}, 32'd4);
	check("bad payload not ok",     {31'd0, load_ok},        32'd0);
	check("bad payload no freeze",  {31'd0, saw_freeze},     32'd0);

	// A flipped bit in the last payload word is the case a CRC sampled one
	// cycle early would miss: the feeder is still folding that word's final
	// byte when the comparison is made.
	restore_window(); poke_word(8 + CORE_WORDS - 1, ddr_word(8 + CORE_WORDS - 1) ^ 32'h80000000);
	run_load();
	check("last word refused",      {28'd0, load_fail_code}, 32'd4);
	check("last word no freeze",    {31'd0, saw_freeze},     32'd0);

	// A state file made under a different Kickstart. Every header word is right
	// and the payload CRC still matches -- the FILE is untouched. What changed is
	// the machine: its ROM now reads back different words, so the fingerprint the
	// controller recomputes over the live ROM no longer matches the one stored in
	// the payload. Nothing else in the format can catch this, which is the whole
	// reason the fingerprint exists: without it the restore succeeds and the
	// Amiga gurus with no explanation.
	restore_window();
	for (i = 0; i < CHIP_WORDS; i = i + 1) chip_mem[i] = 16'hBAD0 + i[15:0];
	reset_observers();
	rom_alt = 1;
	run_load();
	check("wrong kickstart refused",   {28'd0, load_fail_code}, 32'd5);
	check("wrong kickstart not ok",    {31'd0, load_ok},        32'd0);
	check("wrong kickstart no freeze", {31'd0, saw_freeze},     32'd0);
	// The refusal is a no-op: the ROM scan reads SDRAM but writes nothing, and
	// nothing of the state vector or chip RAM was applied on the way to saying no.
	check("wrong kickstart chip untouched", {16'd0, chip_mem[0]}, 32'h0000BAD0);
	check("wrong kickstart writes no DDR3",     ddr_write_count,      32'd0);
	check("wrong kickstart never froze",        freeze_rise_count,    32'd0);

	// Gate order: the payload CRC is a verdict on the file and costs the machine
	// nothing, while the fingerprint costs a full ROM scan over the running
	// machine's bus. A file that is BOTH corrupt and from another Kickstart must
	// therefore report 4, not 5 -- both because "corrupt" is the more actionable
	// diagnosis, and because a payload that has not been CRC-checked cannot be
	// trusted to carry a meaningful fingerprint in the first place.
	restore_window(); poke_word(9, ddr_word(9) ^ 32'd1);
	run_load();
	check("payload CRC precedes fingerprint", {28'd0, load_fail_code}, 32'd4);
	check("both-bad no freeze",               {31'd0, saw_freeze},     32'd0);
	rom_alt = 0;

	// The SDRAM CPU port is not routed to ss_ctrl at all: chip select is taken
	// but sd_ready never comes. This is what a missing or wrong port mux in
	// Minimig.sv looks like from in here, and before SCAN_TIMEOUT existed it
	// parked the controller in S_KICK_WAIT forever -- on the restore path with
	// the CPU held off its own bus by ss_arm, i.e. a machine that looks alive
	// and never runs another instruction, with nothing anywhere saying why.
	// Bounding the wait is safe precisely here and nowhere else: the last gate
	// has not passed, so not one byte of the Amiga has been touched.
	restore_window();
	for (i = 0; i < CHIP_WORDS; i = i + 1) chip_mem[i] = 16'hBAD0 + i[15:0];
	reset_observers();
	watch_bus = 0;
	sd_stall  = 1;
	run_load();
	check("dead port refused",        {28'd0, load_fail_code}, 32'd6);
	check("dead port not ok",         {31'd0, load_ok},        32'd0);
	check("dead port no freeze",      {31'd0, saw_freeze},     32'd0);
	check("dead port chip untouched", {16'd0, chip_mem[0]},    32'h0000BAD0);
	check("dead port writes no DDR3", ddr_write_count,         32'd0);
	check("dead port never froze",    freeze_rise_count,       32'd0);
	check("dead port dropped rom_scan", {31'd0, rom_scan},     32'd0);
	// Let the abandoned ss_dma transfer finish so the port is clean again, then
	// put the watch back for the rest of the run.
	sd_stall = 0;
	for (i = 0; i < 20; i = i + 1) @(negedge clk);
	check("dead port released on retry", {31'd0, sd_cs}, 32'd0);
	watch_bus = 1;

	check("no freeze at any point",  {31'd0, saw_freeze}, 32'd0);
	watch_freeze = 0;

	// A perfectly good file against a machine that will not stand still. The
	// freeze is requested, never granted, and given up on. Nothing has been
	// written by then, so this is as much a no-op as the gate refusals above:
	// the Amiga is left exactly as it was found, and the user can try again.
	restore_window();
	for (i = 0; i < CHIP_WORDS; i = i + 1) chip_mem[i] = 16'hBAD0 + i[15:0];
	reset_observers();
	blit_busy = 1;
	@(negedge clk); load_req = 1;
	wait (load_busy);
	@(negedge clk); load_req = 0;
	while (load_busy) begin
		@(negedge clk); frame_tick = 1;
		@(negedge clk); frame_tick = 0;
	end
	blit_busy = 0;
	@(negedge clk); @(negedge clk); @(negedge clk);
	check("wedged machine refused",    {28'd0, load_fail_code}, 32'd6);
	check("wedged machine not ok",     {31'd0, load_ok},        32'd0);
	check("wedged chip RAM untouched", {16'd0, chip_mem[0]},    32'h0000BAD0);
	check("wedged machine never froze", freeze_rise_count,      32'd0);

	// And the good file still restores after all of that -- the CRC is
	// re-armed per attempt, not left carrying the previous scan's residue, and
	// the whole sequence repeats cleanly rather than working exactly once.
	restore_window();
	for (i = 0; i < CHIP_WORDS; i = i + 1) chip_mem[i] = 16'hBAD0 + i[15:0];
	reset_observers();
	run_load();
	check("revalidates after fails", {31'd0, load_ok},   32'd1);
	check("revalidated no failure",  {31'd0, load_fail}, 32'd0);
	check("second restore chip RAM", {16'd0, chip_mem[5]}, 32'h0000D005);
	check("second restore ordering", chip_wr_at_release, CHIP_WORDS);
	check("second restore one fall", freeze_fall_count,  32'd1);
	check("second restore counter",  ddr_word(0),        32'hA5A5A5A5);

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

	check("never steals the CPU port", {31'd0, bus_stolen}, 32'd0);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
