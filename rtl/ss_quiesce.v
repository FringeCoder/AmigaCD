`timescale 1ns/1ns

// Save state quiesce sequencer.
//
// Does not drain the machine. It waits for a quiet instant that happens on its
// own: blitter idle, no disk or audio DMA word in flight, CPU at a
// no-memory-access boundary. If it does not arrive within `frame_limit` video
// frames the machine is wedged, and we abandon the request rather than capture
// a state that looks fine and restores into garbage.
//
// "Within microseconds on a running Amiga" was the original estimate and it is
// wrong. cpu_boundary is itself four terms (Minimig.sv), so `quiet` is seven
// conditions holding at once, and on hardware three frames was routinely not
// enough: saving a running game failed three attempts out of four.
//
// The limit is an input, not a parameter, because the two directions want
// different answers and there is one instance:
//
//  - A SAVE should wait as long as it takes. The wait costs nothing but time;
//    the machine runs untouched and the capture happens at whatever instant it
//    finally gets. Refusing early just makes the feature feel broken.
//  - A RESTORE should not wait long. Its wait runs with the 68k already parked
//    by ss_arm while the chipset keeps going, which is exactly the drift that
//    reset the Amiga (see ss_ctrl.v's S_IDLE load branch). Every frame spent
//    here is a frame the state being restored goes further out of date, so a
//    restore is better off refusing and being retried.
//
// Once frozen, the busy inputs are meaningless -- the machine is stopped -- so
// freeze latches until the requester drops req.

module ss_quiesce
(
	input       clk,
	input       rst_n,
	input       req,

	// Frames to wait before giving up, sampled continuously while waiting.
	// Zero means "one frame", the same as one: the compare below is against
	// frame_limit - 1 and frames starts at zero.
	input [7:0] frame_limit,

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
			if (frames >= (frame_limit - 8'd1)) timeout <= 1'b1;
			frames <= frames + 8'd1;
		end
	end
end

endmodule
