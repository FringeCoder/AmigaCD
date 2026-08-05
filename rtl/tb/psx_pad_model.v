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
event    byte_done;   // fires once per byte boundary that needs an ACK pulse

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

// Present the current bit, then advance. Bit 0 of byte 0 is already on `dat`
// from the negedge-att handler above, so the first falling edge re-asserts it
// (harmless) before the master's first rising-edge sample; every later bit is
// likewise on the wire for a full clock before the master samples it. Byte
// rollover happens here too, one falling edge later than presenting bit 7 of
// the outgoing byte, so the new byte's bit 0 is only presented starting with
// the next falling edge - exactly mirroring real hardware.
always @(negedge clk) if (!att) begin
	dat = tx[bit_idx];
	if (bit_idx == 7) begin
		bit_idx  = 0;
		byte_idx = byte_idx + 1;
		tx       = reply(byte_idx);
		// ACK after every byte but the last of the transfer. Handled by a
		// separate process (see below) triggered via a named event, so the
		// ~4 us pulse can't stall this always block against the next
		// bit/byte edges.
		if (byte_idx < (3 + PAYLOAD_BYTES)) -> byte_done;
	end
	else begin
		bit_idx = bit_idx + 1;
	end
end

// ACK pulse, decoupled from the bit/byte shift process above so its blocking
// #4000 delay can never stall bit timing.
always @(byte_done) begin
	ack = 1'b0;
	#4000;              // ~4 us low, well inside the real 2-10 us
	ack = 1'b1;
end

endmodule
