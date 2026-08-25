`timescale 1ns/1ns

// Mock SDRAM CPU port. Models cpu_cache_new.v's actual bus-cycle protocol,
// not just a convenient stand-in: `sd_ready` is a *level*, asserted after a
// per-word programmable latency and held for as long as `sd_cs` stays
// asserted, and it only clears when `sd_cs` drops (matching
// cpu_cache_new.v:490, `if (!cpu_cs) cpu_ack <= 1'b0`). While `sd_ready` is
// already up and `sd_cs` is still asserted, the mock refuses to service a
// new address -- it holds the same `sd_rd`/`committed_addr` regardless of
// what the DUT's `sd_addr` does underneath it, matching the real cache's
// state machine sitting in CPU_SM_WAIT/CPU_SM_FILLW until `sd_cs` drops.
//
// This replaces an earlier mock that pulsed `sd_ready` for one cycle and
// re-armed on `sd_cs` staying high across word boundaries. That mock could
// not catch a DUT that (incorrectly) holds `sd_cs` high for a whole burst:
// it would happily keep pulsing a fresh `sd_ready` every cycle regardless
// of `sd_cs`, hiding exactly the defect this bench now exists to catch.
//
// Because the real protocol requires `sd_cs` to drop for at least one cycle
// between words (see rtl/ss_dma.v's header comment), and the DUT already
// has the new address ready the same cycle it reasserts `sd_cs`, latency 0
// is physically fine for *every* word, not just the first -- there is no
// same-edge race with the previous word's ready the way there was under
// the old held-`sd_cs` protocol.

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
integer     rel_hold = 0;   // cycles sd_ready lingers after sd_cs drops
integer     rel_cnt  = 0;

wire        word_valid;
wire [15:0] word_out;
wire        done;

integer errors = 0;
reg         write_mode = 0;
reg  [15:0] word_in = 0;
wire        word_req;
wire [15:0] sd_wr;
integer     wr_seq = 0;
integer     wr_count = 0;
reg         wr_logged = 0;
reg         saw_state2 = 0;
reg         saw_state3 = 0;
reg  [24:1] wr_addr [0:15];
reg  [15:0] wr_data [0:15];
integer got_count;
integer i;

reg [15:0] got [0:15];

// Per-word extra wait cycles beyond the first opportunity a fresh access
// (sd_cs asserted, sd_ready not yet up) can be serviced. Index resets to 0
// on every `start` pulse so each transfer's plan is independent.
reg [3:0] lat_plan [0:15];
integer   word_idx;
integer   wait_cnt;

// The address the mock most recently committed a word to, latched only
// when a fresh access begins servicing (sd_cs high, sd_ready not yet up)
// and held constant thereafter even if sd_cs stays up and sd_addr keeps
// moving underneath it -- exactly the real cache's behaviour.
reg [24:1] committed_addr;

// Tracks how many times sd_cs has fallen from 1 to 0 since the last
// `start`. Under the per-word handshake this must happen exactly once per
// word (every word's capture drops sd_cs, whether into the inter-word gap
// or into the final idle) and never mid-word.
reg     prev_cs = 0;
integer cs_drop_count;

// Records the committed address behind each word the DUT actually claims
// to have captured (`word_valid`). A regression to holding sd_cs across a
// whole burst makes the mock freeze on the first address forever (see
// mock rationale above), so every word_valid pulse after the first would
// replay the same committed_addr -- caught here as a duplicate, loudly,
// rather than surfacing only as a subtly wrong word count.
reg [24:1] svc_hist [0:31];
integer    svc_count;
integer    j;

ss_dma dut
(
	.clk(clk), .rst_n(rst_n),
	.start(start), .base_addr(base_addr), .word_count(word_count),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_rd(sd_rd), .sd_ready(sd_ready),
	.write_mode(write_mode), .word_in(word_in), .word_req(word_req),
	.sd_wr(sd_wr),
	.word_valid(word_valid), .word_out(word_out), .done(done)
);

always #5 clk = ~clk;

// ---- write-direction plumbing -------------------------------------------
// The producer advances on word_req, which the DUT pulses once each accepted
// write. The first word must be present before start, mirroring the contract
// ss_ctrl will use.
always @(posedge clk) begin
	if (!rst_n) begin
		wr_seq <= 0;
	end
	else if (start) begin
		wr_seq  <= 0;
		word_in <= 16'hAA00;
	end
	else if (word_req) begin
		wr_seq  <= wr_seq + 1;
		word_in <= 16'hAA00 + wr_seq + 1;
	end
end

// Record what the DUT writes, so the test proves the right data reached the
// right address rather than merely that the cycle count was plausible.
always @(posedge clk) begin
	if (sd_cs && sd_ready && sd_state == 2'd3 && !wr_logged) begin
		wr_addr[wr_count] <= sd_addr;
		wr_data[wr_count] <= sd_wr;
		wr_count          <= wr_count + 1;
		wr_logged         <= 1'b1;
	end
	if (sd_cs && write_mode && sd_state == 2'd3) saw_state3 <= 1'b1;
	if (sd_cs && write_mode && sd_state == 2'd2) saw_state2 <= 1'b1;
	if (!sd_cs) wr_logged <= 1'b0;
	if (start)  begin wr_count <= 0; wr_logged <= 1'b0; end
end

// Mock memory: contents are 0xC000 + address. sd_ready is a level: it goes
// up once the current access's latency target is met and stays up until
// sd_cs drops. While sd_cs is low, the access is over: clear sd_ready and
// reset the latency counter so the next access starts a fresh countdown.
// While sd_cs is high and sd_ready is already up, do nothing -- refuse to
// service a new address, matching the real cache's refusal to leave
// CPU_SM_WAIT/CPU_SM_FILLW while cpu_cs stays asserted.
always @(posedge clk) begin
	if (!rst_n) begin
		word_idx <= 0;
		wait_cnt <= 0;
		sd_ready <= 1'b0;
	end
	else if (start) begin
		word_idx <= 0;
		wait_cnt <= 0;
		rel_cnt  <= 0;
		sd_ready <= 1'b0;
	end
	else if (!sd_cs) begin
		// The real cache does not drop its ack the instant chip select goes
		// away: cpu_cache_new sits in CPU_SM_WAIT until it SEES !cpu_cs and
		// only then returns to idle, and sdram_ctrl's ramready and the port
		// mux add more. rel_hold models that lag. With it at 0 this is the
		// original mock; raising it is what exposes a requester that assumes
		// a fixed-length gap is enough.
		rel_cnt <= rel_cnt + 1;
		if (rel_cnt >= rel_hold) begin
			sd_ready <= 1'b0;
			wait_cnt <= 0;
		end
	end
	else if (!sd_ready) begin
		rel_cnt <= 0;
		if (wait_cnt >= lat_plan[word_idx]) begin
			committed_addr <= sd_addr;
			sd_rd          <= 16'hC000 + {8'd0, sd_addr[8:1]};
			sd_ready       <= 1'b1;
			wait_cnt       <= 0;
			word_idx       <= word_idx + 1;
		end
		else wait_cnt <= wait_cnt + 1;
	end
	// else: sd_cs && sd_ready both already up -- hold everything.
end

always @(posedge clk) begin
	if (word_valid) begin
		got[got_count] = word_out;
		got_count = got_count + 1;
	end
end

// cs-glitch/cadence watchdog: count 1->0 transitions of sd_cs, reset per
// transfer.
always @(posedge clk) begin
	if (start) cs_drop_count <= 0;
	else if (prev_cs && !sd_cs) cs_drop_count <= cs_drop_count + 1;
	prev_cs <= sd_cs;
end

// Duplicate-address watchdog: reset per transfer, check every captured
// word's committed_addr against every prior one this transfer.
always @(posedge clk) begin
	if (start) svc_count <= 0;
	else if (word_valid) begin
		for (j = 0; j < svc_count; j = j + 1) begin
			if (svc_hist[j] === committed_addr) begin
				$display("FAIL duplicate address serviced in one transfer: 0x%06h", committed_addr);
				errors = errors + 1;
			end
		end
		svc_hist[svc_count] <= committed_addr;
		svc_count <= svc_count + 1;
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
	check("cs drops once per word", cs_drop_count, 4);

	// -- Transfer 2: a second transfer must work without an intervening reset.
	for (i = 0; i < 16; i = i + 1) lat_plan[i] = 3;
	run_transfer(24'h000020, 24'd2);

	check("second transfer count", got_count, 2);
	check("second transfer first word", got[0], 16'hC000 + 16'h0020);
	check("second transfer cs drops once per word", cs_drop_count, 2);

	// -- Transfer 3: minimum-turnaround ready -- every word serviced at
	// latency 0. Under the per-word handshake this is physically fine for
	// every word (not just the first): the DUT already holds the next
	// address the same cycle it reasserts sd_cs after the mandatory
	// inter-word gap, so there is no same-edge race with the previous
	// word's ready the way there was when sd_cs stayed up for the whole
	// burst (see file header). A DUT that drops a word, double-counts, or
	// regresses to holding sd_cs across words fails the per-word content
	// check or the duplicate-address watchdog below.
	for (i = 0; i < 16; i = i + 1) lat_plan[i] = 0;
	// Address kept under 24'h100: the mock derives sd_rd from sd_addr[8:1],
	// an 8-bit slice, so an address at or past 0x100 wraps to 0 there and
	// would desync this check's independently-computed expectations from
	// what the mock actually returns -- a bench bug, not a DUT one.
	run_transfer(24'h000030, 24'd5);

	check("min-turnaround transfer count", got_count, 5);
	for (i = 0; i < 5; i = i + 1)
		check("min-turnaround word contents", got[i], 16'hC000 + 16'h0030 + i);
	check("min-turnaround cs drops once per word", cs_drop_count, 5);

	// A cache that is slow to release its ack. Every word must still come from
	// its own address: on hardware this is what went wrong, and it went wrong
	// selectively -- a line already in the CPU's cache acks almost instantly
	// and lands inside the window, so reads of Kickstart came back with words
	// 1 and 3 of each 8-byte group exchanged while chip RAM was clean.
	for (rel_hold = 1; rel_hold <= 3; rel_hold = rel_hold + 1) begin
		run_transfer(24'h000040, 24'd6);
		check("slow-release transfer count", got_count, 6);
		for (i = 0; i < 6; i = i + 1)
			check("slow-release word contents", got[i], 16'hC000 + 16'h0040 + i);
		check("slow-release cs drops once per word", cs_drop_count, 6);
	end
	rel_hold = 0;

	// -- Transfer 4: mixed per-word latency, to confirm words are entirely
	// independent under the per-word handshake (no interaction between one
	// word's latency and the next's, since sd_cs always fully drops and
	// re-asserts between them).
	lat_plan[0] = 2;
	lat_plan[1] = 0;
	lat_plan[2] = 4;
	lat_plan[3] = 1;
	run_transfer(24'h000060, 24'd4);

	check("mixed-latency transfer count", got_count, 4);
	for (i = 0; i < 4; i = i + 1)
		check("mixed-latency word contents", got[i], 16'hC000 + 16'h0060 + i);
	check("mixed-latency cs drops once per word", cs_drop_count, 4);

	// -- Transfer 5: zero-length transfer must terminate, not hang, and must
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

	// -- cache_inhibit must stay asserted through the inter-word gap, not
	// just while sd_cs happens to be high. This is the direct regression
	// test for the defect this bench was rewritten to catch: a burst-style
	// implementation would never produce a gap to sample here, and a
	// per-word implementation that mistakenly tied sd_cache_inhibit to
	// sd_cs (instead of "transfer in progress") would read 0 at this point.
	for (i = 0; i < 16; i = i + 1) lat_plan[i] = 1;
	got_count = 0;
	base_addr  = 24'h000070;
	word_count = 24'd2;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	@(negedge sd_cs);
	check("sd_cache_inhibit stays asserted during inter-word gap", sd_cache_inhibit, 1'b1);
	check("sd_cs is actually low during the sampled gap", sd_cs, 1'b0);
	wait_done;
	@(negedge clk);

	// ---- write direction ------------------------------------------------
	// Restore has to put chip RAM back. Same per-word chip-select discipline:
	// the cache behind the CPU port holds its ack as a level and only clears
	// it when chip select drops, so a held CS would rewrite one address.
	write_mode = 1;
	base_addr  = 24'h000040;
	word_count = 24'd4;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	wait (done);
	@(negedge clk);

	check("write: word count",     wr_count,    4);
	check("write: first address",  wr_addr[0],  24'h000040);
	check("write: last address",   wr_addr[3],  24'h000043);
	check("write: data 0",         wr_data[0],  16'hAA00);
	check("write: data 3",         wr_data[3],  16'hAA03);
	check("write: cs released",    sd_cs,       1'b0);
	check("write: state is 3",     saw_state3,  1'b1);
	check("write: never state 2",  saw_state2,  1'b0);

	// Reads must still work afterwards, and must go back to state 2.
	write_mode = 0;
	got_count  = 0;
	base_addr  = 24'h000010;
	word_count = 24'd2;
	@(negedge clk); start = 1;
	@(negedge clk); start = 0;
	wait (done);
	@(negedge clk); @(negedge clk);
	check("read after write: count", got_count, 2);
	check("read after write: data",  got[0],    16'hC000 + 16'h0010);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
