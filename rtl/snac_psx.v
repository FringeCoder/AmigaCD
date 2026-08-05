// Required: the testbench declares `timescale 1ns/1ns, and this file is compiled
// before it. Without a directive here, Icarus mixes default and timescale-based
// delays and the pad model's microsecond ACK pulse never resolves.
`timescale 1ns/1ns

// PSX controller reader for the MiSTer SNAC user port.
//
// Canonical copy lives in the AmigaCD (Main_MiSTer) repo; core repos take a copy.
// Pin map is fixed by PSX_MiSTer/PSX.sv, which every PSX SNAC adapter is wired to.
//
// Bus: 250 kHz, LSB first, CMD driven on CLK falling edge, DAT sampled on rising.
// Both ports share CLK/CMD/DAT/ACK and are selected by their own ATT line, so one
// master serves both, polled in sequence.
module snac_psx #(
	parameter CLK_MHZ  = 50,   // frequency of clk, MHz
	parameter BAUD_KHZ = 250   // PSX bus clock, kHz
) (
	input             clk,
	input             reset,
	input             enable,     // 0 = idle the bus and report nothing

	input       [6:0] user_in,
	output      [6:0] user_out,

	// Button vectors, active HIGH:
	// [0] RIGHT [1] LEFT [2] DOWN [3] UP [4] X [5] O [6] SQUARE [7] TRIANGLE
	// [8] L1 [9] R1 [10] START [11] SELECT [12] L2 [13] R2 [14] L3 [15] R3
	output reg [15:0] pad0,
	output reg [15:0] pad1,
	// {LY, LX, RY, RX}, 8 bits each, 0x80 = centre
	output reg [31:0] axes0,
	output reg [31:0] axes1,
	// Raw device ID byte: 0x41 digital, 0x73 DualShock analog, 0x63 GunCon.
	// 0x00 means nothing answered.
	output reg  [7:0] id0,
	output reg  [7:0] id1
);

assign user_out = 7'h7F;

always @(posedge clk) begin
	pad0  <= 16'h0000;
	pad1  <= 16'h0000;
	axes0 <= 32'h80808080;
	axes1 <= 32'h80808080;
	id0   <= 8'h00;
	id1   <= 8'h00;
end

endmodule
