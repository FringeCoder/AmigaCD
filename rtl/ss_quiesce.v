`timescale 1ns/1ns

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
