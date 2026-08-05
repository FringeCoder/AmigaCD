`timescale 1ns/1ns

module snac_psx_tb;

reg clk = 0;
reg reset = 1;
reg enable = 1;
wire [6:0] user_out;
reg  [6:0] user_in = 7'h7F;

wire [15:0] pad0, pad1;
wire [31:0] axes0, axes1;
wire  [7:0] id0, id1;

integer errors = 0;

always #10 clk = ~clk;   // 50 MHz

snac_psx #(.CLK_MHZ(50), .BAUD_KHZ(250)) dut (
	.clk(clk), .reset(reset), .enable(enable),
	.user_in(user_in), .user_out(user_out),
	.pad0(pad0), .pad1(pad1), .axes0(axes0), .axes1(axes1),
	.id0(id0), .id1(id1)
);

task check(input [255:0] name, input integer got, input integer want);
begin
	if (got !== want) begin
		$display("FAIL %0s: got %0d (0x%0h), want %0d (0x%0h)", name, got, got, want, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

// Port 1 pad: buttons drivable per-test (active low, so 0xFF = released), sticks centred.
wire att1 = user_out[1];
reg [7:0] p1_btn0 = 8'hFF;
reg [7:0] p1_btn1 = 8'hFF;

psx_pad_model #(.DEVICE_ID(8'h41), .PAYLOAD_BYTES(2)) pad_p1 (
	.att(att1), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p1), .ack(ack_p1),
	.btn0(p1_btn0), .btn1(p1_btn1),
	.ax_rx(8'h80), .ax_ry(8'h80), .ax_lx(8'h80), .ax_ly(8'h80)
);
wire dat_p1, ack_p1;

always @(*) begin
	user_in[4] = dat_p1;
	user_in[3] = ack_p1;
end

initial begin
	$dumpfile("snac_psx_tb.vcd");
	$dumpvars(0, snac_psx_tb);

	repeat (10) @(posedge clk);
	reset = 0;

	// One full poll of both ports at 250 kHz is on the order of 500 us.
	#2000000;

	check("port1 device id", id0, 8'h41);

	// Port 0 (SNAC port 1) is only re-polled roughly every 3.2 ms: the ST_GAP
	// state waits out a full 16-bit rollover (~1.31 ms) after every port poll,
	// twice per double-port cycle. A 2 ms settle window can land in the gap
	// between polls and miss a button-state change entirely (observed: the
	// "x pressed" state below was skipped over by every poll when settled with
	// only #2000000). Settle for 5 ms - comfortably more than one full repoll
	// period - so every state change below is guaranteed to be sampled before
	// its check runs.
	// Byte 0 bit 5 is RIGHT, active low. Expect pad0[0].
	p1_btn0 = 8'hFF & ~8'h20;
	#5000000;
	check("right pressed", pad0, 16'h0001);

	// Byte 1 bit 6 is X, active low. Expect pad0[4].
	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF & ~8'h40;
	#5000000;
	check("x pressed", pad0, 16'h0010);

	// START is byte 0 bit 3 -> pad0[10]; SELECT is byte 0 bit 0 -> pad0[11].
	p1_btn0 = 8'hFF & ~8'h09;
	p1_btn1 = 8'hFF;
	#5000000;
	check("start+select", pad0, 16'h0C00);

	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF;
	#5000000;
	check("all released", pad0, 16'h0000);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
