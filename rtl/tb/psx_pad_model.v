// Required, and load-bearing: `#4000` below is the ACK pulse width. Without this
// directive the delay is not interpreted in nanoseconds and ACK never deasserts.
`timescale 1ns/1ns

// Simulation model of a PSX controller. Answers the 0x01/0x42 poll with a
// configurable device ID and payload, and pulses ACK after every byte except
// the last, exactly as real hardware does.
module psx_pad_model #(
	parameter [7:0] DEVICE_ID = 8'h41,   // 0x41 digital, 0x73 DualShock
	parameter integer PAYLOAD_BYTES = 2
) (
	input       att,      // active low
	input       clk,      // bus clock from the master
	input       cmd,
	output reg  dat,
	output reg  ack,
	input [7:0] btn0,     // raw PSX byte 0, active low
	input [7:0] btn1,     // raw PSX byte 1, active low
	input [7:0] ax_rx, input [7:0] ax_ry, input [7:0] ax_lx, input [7:0] ax_ly
);

integer byte_idx;
integer bit_idx;
reg [7:0] rx;
reg [7:0] tx;

function [7:0] reply(input integer idx);
	case (idx)
		0: reply = 8'hFF;
		1: reply = DEVICE_ID;
		2: reply = 8'h5A;
		3: reply = btn0;
		4: reply = btn1;
		5: reply = ax_rx;
		6: reply = ax_ry;
		7: reply = ax_lx;
		8: reply = ax_ly;
		default: reply = 8'hFF;
	endcase
endfunction

initial begin
	dat = 1'b1; ack = 1'b1; byte_idx = 0; bit_idx = 0; rx = 0; tx = 8'hFF;
end

always @(negedge att) begin
	byte_idx = 0;
	bit_idx  = 0;
	tx       = reply(0);
	dat      = tx[0];
end

always @(posedge att) begin
	dat = 1'b1;
	ack = 1'b1;
end

// Master drives CMD on the falling edge and samples DAT on the rising edge, so
// the pad does the mirror image: shift out on falling, sample CMD on rising.
always @(posedge clk) if (!att) begin
	rx[bit_idx] = cmd;
end

always @(negedge clk) if (!att) begin
	if (bit_idx == 7) begin
		bit_idx  = 0;
		byte_idx = byte_idx + 1;
		tx       = reply(byte_idx);
		dat      = tx[0];
		// ACK after every byte but the last of the transfer.
		if (byte_idx < (3 + PAYLOAD_BYTES)) begin
			ack = 1'b0;
			#4000 ack = 1'b1;   // ~4 us low, well inside the real 2-10 us
		end
	end
	else begin
		bit_idx = bit_idx + 1;
		dat     = tx[bit_idx];
	end
end

endmodule
