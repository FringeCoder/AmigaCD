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

// Port 1 pad: all buttons released (active low, so 0xFF), sticks centred.
wire att1 = user_out[1];
psx_pad_model #(.DEVICE_ID(8'h41), .PAYLOAD_BYTES(2)) pad_p1 (
	.att(att1), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p1), .ack(ack_p1),
	.btn0(8'hFF), .btn1(8'hFF),
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

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
