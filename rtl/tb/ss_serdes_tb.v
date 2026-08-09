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
