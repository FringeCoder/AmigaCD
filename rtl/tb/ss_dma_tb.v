`timescale 1ns/1ns

// Mock SDRAM CPU port. Returns a value derived from the address after a
// per-word programmable latency, so the DUT cannot pass by assuming a fixed
// response time. Unlike a naive "if (sd_cs && !sd_ready)" latency counter
// (which can never re-arm on the cycle it fires, since it gates on its own
// previous output), this one can assert sd_ready on the clock edge right
// after the previous one -- latency 0 for word 0 does exactly that.
//
// Latency 0 for word 1 (or any word after the first) was tried and
// deliberately rejected: it is not just untested, it is unphysical for this
// protocol shape. sd_addr is a single register that ss_dma only advances in
// response to observing the previous word's ready -- one edge after that
// ready commits. A mock that also decides "is word N+1 ready" on that same
// edge necessarily reads the pre-advance (stale) address, because both
// reactions fire off the same event in the same clock edge; no requester
// could present the new address any earlier without seeing the future. Model
// this with `sd_addr` directly (address 0x30, latency 0,0,2,0,0) and the
// mock itself desyncs from the address it is supposed to be reading --
// confirmed by a cycle-by-cycle trace, not just asserted. So word 0 gets
// latency 0 (its address comes from `start`, set up a full cycle earlier by
// an unrelated event, so it is already settled) and every later word gets
// the tightest latency the protocol can actually deliver, 1, instead of the
// brief's uniformly generous >=3 -- which leaves two cycles of slack that
// could hide an off-by-one in exactly this minimum-turnaround path.

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
integer i;

reg [15:0] got [0:15];

// Per-word extra wait cycles beyond the first opportunity. Index resets to 0
// on every `start` pulse so each transfer's plan is independent.
reg [3:0] lat_plan [0:15];
integer   word_idx;
integer   wait_cnt;

// Tracks how many times sd_cs has fallen from 1 to 0 since the last `start`,
// to confirm it deasserts exactly once, at the end of the transfer, and
// never glitches low mid-burst.
reg     prev_cs = 0;
integer cs_drop_count;

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

// Mock memory: contents are 0xC000 + address. Ready fires as soon as
// wait_cnt reaches the plan's target for the current word; unlike a
// counter gated on the previous ready value, this can re-fire on the very
// next cycle when the next word's target is also 0.
always @(posedge clk) begin
	sd_ready <= 1'b0;
	if (!rst_n) begin
		word_idx <= 0;
		wait_cnt <= 0;
	end
	else if (start) begin
		word_idx <= 0;
		wait_cnt <= 0;
	end
	else if (sd_cs) begin
		if (wait_cnt >= lat_plan[word_idx]) begin
			sd_rd    <= 16'hC000 + {8'd0, sd_addr[8:1]};
			sd_ready <= 1'b1;
			wait_cnt <= 0;
			word_idx <= word_idx + 1;
		end
		else wait_cnt <= wait_cnt + 1;
	end
	else wait_cnt <= 0;
end

always @(posedge clk) begin
	if (word_valid) begin
		got[got_count] = word_out;
		got_count = got_count + 1;
	end
end

// cs-glitch watchdog: count 1->0 transitions of sd_cs, reset per transfer.
always @(posedge clk) begin
	if (start) cs_drop_count <= 0;
	else if (prev_cs && !sd_cs) cs_drop_count <= cs_drop_count + 1;
	prev_cs <= sd_cs;
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

// Synchronises on `done` via posedge polling rather than a level-sensitive
// `wait (done)`. `done` and the last word's `word_valid` commit on the same
// clock edge, from the same always block. A level-sensitive `wait` wakes on
// that same simulation time step -- ahead of the *next* posedge at which the
// got[]-capturing always block (below) actually samples that final
// word_valid -- so a `wait (done); @(negedge clk);` idiom checks got_count
// one word too early and silently drops the last word of every transfer.
// Verified against the brief's own verbatim testbench and implementation,
// unmodified: it fails this exact way ("word count: got 3, want 4") before
// any change made here. Polling on `@(posedge clk)` reads `done` with the
// same one-cycle-stale timing the capture block uses for word_valid, so both
// observe the transfer's end at the same simulated point.
task wait_done;
begin
	while (!done) @(posedge clk);
end
endtask

task run_transfer(input [24:1] addr, input [23:0] count);
begin
	got_count = 0;
	base_addr  = addr;
	word_count = count;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	wait_done;
	@(negedge clk);
end
endtask

initial begin
	@(negedge clk); rst_n = 1;

	// -- Transfer 1: fixed latency 3 for every word (the original scenario).
	for (i = 0; i < 16; i = i + 1) lat_plan[i] = 3;
	run_transfer(24'h000010, 24'd4);

	check("word count", got_count, 4);
	for (i = 0; i < 4; i = i + 1)
		check("word contents", got[i], 16'hC000 + 16'h0010 + i);
	check("cs released after done", sd_cs, 1'b0);
	check("cs drops exactly once", cs_drop_count, 1);

	// -- Transfer 2: a second transfer must work without an intervening reset.
	for (i = 0; i < 16; i = i + 1) lat_plan[i] = 3;
	run_transfer(24'h000020, 24'd2);

	check("second transfer count", got_count, 2);
	check("second transfer first word", got[0], 16'hC000 + 16'h0020);
	check("second transfer cs drops exactly once", cs_drop_count, 1);

	// -- Transfer 3: minimum-turnaround ready. Word 0 fires the instant its
	// address is presented (latency 0); every later word fires at latency 1,
	// the tightest spacing this protocol can physically deliver (see the
	// file header). A DUT that drops a word, or that double-counts because
	// it re-samples a word it already consumed, fails the per-word content
	// check below (each expected value is computed independently from the
	// known address sequence, not from whatever the mock happened to last
	// drive).
	lat_plan[0] = 0;
	lat_plan[1] = 1;
	lat_plan[2] = 1;
	lat_plan[3] = 1;
	lat_plan[4] = 1;
	// Address kept under 24'h100: the mock derives sd_rd from sd_addr[8:1],
	// an 8-bit slice, so an address at or past 0x100 wraps to 0 there and
	// would desync this check's independently-computed expectations from
	// what the mock actually returns -- a bench bug, not a DUT one.
	run_transfer(24'h000030, 24'd5);

	check("min-turnaround transfer count", got_count, 5);
	for (i = 0; i < 5; i = i + 1)
		check("min-turnaround word contents", got[i], 16'hC000 + 16'h0030 + i);
	check("min-turnaround cs drops exactly once", cs_drop_count, 1);

	// -- Transfer 4: zero-length transfer must terminate, not hang, and must
	// never assert sd_cs at all.
	got_count = 0;
	base_addr  = 24'h000040;
	word_count = 24'd0;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	wait_done;
	@(negedge clk);

	check("zero-length word count", got_count, 0);
	check("zero-length never asserts cs", cs_drop_count, 0);
	check("zero-length cs stays low", sd_cs, 1'b0);

	// -- Protocol constants, sanity-checked against sdram_ctrl.v: uds_n/lds_n
	// are active low and both asserted (full word access), and sd_state is
	// 2'd2 ("read data") throughout -- checked once more, after a normal
	// transfer, while cs is asserted.
	for (i = 0; i < 16; i = i + 1) lat_plan[i] = 1;
	base_addr  = 24'h000050;
	word_count = 24'd1;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	@(negedge clk);
	check("sd_state is read-data during transfer", sd_state, 2'd2);
	check("sd_uds_n asserted (active low)", sd_uds_n, 1'b0);
	check("sd_lds_n asserted (active low)", sd_lds_n, 1'b0);
	check("sd_cache_inhibit asserted during transfer", sd_cache_inhibit, 1'b1);
	wait_done;
	@(negedge clk);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
