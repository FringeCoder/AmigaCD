`timescale 1ns/1ns

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
//
// Load completion is flagged by a one-shot `load_latch` register set the same
// cycle the last word is shifted in, not by comparing `count` against WORDS
// after the fact. An earlier version used `else if (!loading && count ==
// WORDS)` to detect completion, but count is shared with the save path: a
// save leaves count sitting at WORDS once it finishes (its own last cycle
// does count <= count + 1 with count == WORDS-1, landing on WORDS), and
// loading is 0 whenever no load is in progress. So that condition was true
// for exactly one cycle after every save completed, firing state_we/load_done
// spuriously with a zeroed shifter -- a real defect (it would blast the
// restore-side registers with zero right after every save) that a round-trip
// test alone does not catch, because the following genuine load overwrites
// the corrupted value before the testbench ever compares it. A one-shot flag
// set only at the instant the last load word is accepted has no dependency on
// count's resting value and cannot be retriggered by unrelated activity.
//
// save_done/load_done are each held back one extra cycle behind the data
// event they report (the last word_valid, and state_we respectively), rather
// than pulsing in the same cycle. This is not cosmetic: a consumer -- this
// module's own testbench included -- typically mirrors word_valid/state_we
// into its own registers (e.g. "always @(posedge clk) if (word_valid)
// capture(...)"), which by ordinary flip-flop-to-flip-flop timing only
// reflects a given cycle's pulse starting the *next* edge. `done`, read via a
// level-sensitive `wait()`, has no such lag -- it unblocks the instant its
// register updates. Assert done in the same cycle as the data pulse and the
// wait() resumes a full cycle before the mirroring register has caught up,
// so a comparison made right after wait(done) sees stale (pre-final-word)
// data. Confirmed empirically: with same-cycle done, the round-trip
// testbench's word counter was reliably one word short and `restored` never
// updated at all. Delaying done by one cycle relative to its data gives the
// mirroring register's active-region update time to land before done's NBA
// update fires the wait().

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

// Despite the name, this is not physically shifted: each word is read from
// or written to shifter[count*32 +: 32] directly. An actual shift-by-32
// register (`shifter <= {32'd0, shifter[PADDED-1:32]}` /
// `shifter <= {word_in, shifter[PADDED-1:32]}`) was the first cut, but for
// WORDS == 1 (WIDTH <= 32, which includes this module's own default
// parameter) PADDED == 32 and PADDED-1:32 is 31:32 -- an out-of-order
// part-select that Icarus rejects at elaboration, not just at run time, so
// the single-word case could never even compile. Indexed part-selects
// addressed by `count` have no such degenerate width.
reg [PADDED-1:0] shifter;
reg [15:0]       count;
reg              saving;
reg              save_finish;   // last word_valid emitted; save_done due next cycle
reg              loading;
reg              load_latch;    // last word captured; state_out/state_we due next cycle
reg              load_finish;   // state_we emitted; load_done due next cycle

always @(posedge clk) begin
	if (!rst_n) begin
		word_valid  <= 1'b0;
		save_done   <= 1'b0;
		load_done   <= 1'b0;
		state_we    <= 1'b0;
		saving      <= 1'b0;
		save_finish <= 1'b0;
		loading     <= 1'b0;
		load_latch  <= 1'b0;
		load_finish <= 1'b0;
		count       <= 16'd0;
	end
	else begin
		word_valid <= 1'b0;
		save_done  <= 1'b0;
		load_done  <= 1'b0;
		state_we   <= 1'b0;

		// Save path. Guarded against starting while a load is in flight so
		// the two directions never drive `shifter`/`count` in the same
		// cycle.
		if (save_start && !loading) begin
			shifter     <= {{(PADDED-WIDTH){1'b0}}, state_in};
			count       <= 16'd0;
			saving      <= 1'b1;
			save_finish <= 1'b0;
		end
		else if (saving) begin
			word_out   <= shifter[count*32 +: 32];
			word_valid <= 1'b1;
			count      <= count + 16'd1;
			if (count == (WORDS - 1)) begin
				saving      <= 1'b0;
				save_finish <= 1'b1;
			end
		end
		else if (save_finish) begin
			save_done   <= 1'b1;
			save_finish <= 1'b0;
		end

		// Load path. Guarded against starting while a save is in flight for
		// the same reason. Completion is a two-stage one-shot pipeline
		// (capture -> latch state_out/state_we -> pulse load_done), armed
		// the cycle the last word is captured, not a level check on `count`
		// (see module header for why that distinction matters, and why the
		// pipeline has the extra load_finish stage).
		if (load_start && !saving) begin
			count       <= 16'd0;
			loading     <= 1'b1;
			load_latch  <= 1'b0;
			load_finish <= 1'b0;
		end
		else if (loading && word_in_valid) begin
			shifter[count*32 +: 32] <= word_in;
			count                   <= count + 16'd1;
			if (count == (WORDS - 1)) begin
				loading    <= 1'b0;
				load_latch <= 1'b1;
			end
		end
		else if (load_latch) begin
			state_out   <= shifter[WIDTH-1:0];
			state_we    <= 1'b1;
			load_latch  <= 1'b0;
			load_finish <= 1'b1;
		end
		else if (load_finish) begin
			load_done   <= 1'b1;
			load_finish <= 1'b0;
		end
	end
end

endmodule
