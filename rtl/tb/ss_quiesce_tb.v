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

// The limit is an input now, not a parameter: a save wants to wait a long time
// and a restore wants to give up quickly, and there is one instance serving
// both (see ss_quiesce's header). Held at 3 here so every existing case below
// keeps testing exactly what it did; frame_limit_drive is varied by the case
// added at the end, which is the one that would catch the input being ignored.
reg [7:0] frame_limit_drive = 8'd3;

ss_quiesce dut
(
	.clk(clk), .rst_n(rst_n), .req(req),
	.frame_limit(frame_limit_drive),
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

// 320 bits (40 bytes): the longest label here, "cpu mid-instruction blocks
// freeze", is 33 characters -- 256 bits silently truncates it to "pu
// mid-instruction blocks freeze" without affecting the pass/fail check itself,
// which only compares actual against want.
task check(input [319:0] name, input actual, input want);
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

	// The limit is honoured as an INPUT, not baked in. Same never-quiet machine
	// as the timeout case above, but with a longer limit: it must still be
	// waiting at the frame count that used to give up, and only time out when
	// its own limit is reached. A design that ignored frame_limit and kept the
	// old constant would fail the first of these two.
	req = 0;
	@(negedge clk); @(negedge clk);
	frame_limit_drive = 8'd8;
	blit_busy = 1;                 // never quiet
	req = 1;
	@(negedge clk);
	for (i = 0; i < 3; i = i + 1) pulse_frame;
	@(negedge clk);
	check("longer limit still waiting at 3 frames", timeout, 1'b0);
	for (i = 0; i < 5; i = i + 1) pulse_frame;
	@(negedge clk);
	check("longer limit times out at its own limit", timeout, 1'b1);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
