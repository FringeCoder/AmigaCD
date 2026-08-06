// Required, and load-bearing: the ACK delay and pulse width below are in
// nanoseconds. Without this directive they are not interpreted as such and ACK
// never resolves.
`timescale 1ns/1ns

// Simulation model of a PSX controller. Answers the 0x01/0x42 poll with a
// configurable device ID and payload, and pulses ACK after every byte except
// the last, exactly as real hardware does.
//
// It is also the oracle for the master's transmit side: a real pad answers the
// poll sequence and nothing else, so the model checks every command byte it
// receives and counts a mismatch in `cmd_errors`, which the testbench asserts
// on. Without that, the master could shift out arbitrary garbage and every
// receive-side assertion would still pass.
module psx_pad_model #(
	parameter [7:0] DEVICE_ID = 8'h41,   // 0x41 digital, 0x73 DualShock
	parameter integer PAYLOAD_BYTES = 2,
	// ACK asserts some microseconds after the byte's last clock edge, not
	// instantly: that latency is the whole reason the master has to wait rather
	// than free-run, so it must be modelled or the wait is untested.
	parameter integer ACK_DELAY_NS = 12000,
	parameter integer ACK_WIDTH_NS = 4000
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
integer rx_bit;       // bit position of the command bit arriving on this posedge
integer rx_idx;       // index of the command byte being assembled
integer cmd_errors;   // command-stream mismatches; the testbench asserts this is 0
reg [7:0] rx;
reg [7:0] tx;
event    byte_done;   // fires once per byte boundary that needs an ACK pulse

// The poll a pad will answer: 0x01 selects it, 0x42 requests the pad state,
// and every further byte is padding while the pad talks back.
function [7:0] expect_cmd(input integer idx);
	case (idx)
		0: expect_cmd = 8'h01;
		1: expect_cmd = 8'h42;
		default: expect_cmd = 8'h00;
	endcase
endfunction

// Frame is: 0=0xFF, 1=DEVICE_ID, 2=0x5A, then PAYLOAD_BYTES payload bytes at
// indices 3 .. (2 + PAYLOAD_BYTES). A digital pad's frame ends there and it
// releases DAT, so the master reads 0xFF for anything beyond it - a short
// pad must not appear to keep supplying analog axis bytes it doesn't have.
function [7:0] reply(input integer idx);
	if (idx > (2 + PAYLOAD_BYTES))
		reply = 8'hFF;
	else case (idx)
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
	rx_bit = 0; rx_idx = 0; cmd_errors = 0;
end

always @(negedge att) begin
	byte_idx = 0;
	bit_idx  = 0;
	rx_bit   = 0;
	rx_idx   = 0;
	rx       = 0;
	tx       = reply(0);
	dat      = tx[0];
end

always @(posedge att) begin
	dat = 1'b1;
	ack = 1'b1;
end

// Master drives CMD on the falling edge and samples DAT on the rising edge, so
// the pad does the mirror image: shift out on falling, sample CMD on rising.
//
// The command bit arriving on a given rising edge is the one the master drove
// on the preceding falling edge, and that same falling edge has already
// advanced `bit_idx` for the outgoing stream - so the receive side keeps its
// own counter rather than borrowing `bit_idx`, which lags it by one.
always @(posedge clk) if (!att) begin
	rx[rx_bit] = cmd;
	if (rx_bit == 7) begin
		if (rx !== expect_cmd(rx_idx)) begin
			$display("FAIL psx_pad_model(%0h) cmd byte %0d: got 0x%02h, want 0x%02h (t=%0t)",
			         DEVICE_ID, rx_idx, rx, expect_cmd(rx_idx), $time);
			cmd_errors = cmd_errors + 1;
		end
		rx_bit = 0;
		rx_idx = rx_idx + 1;
	end
	else rx_bit = rx_bit + 1;
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
		// ACK after every byte but the last of the transfer. The absence
		// of that final ACK is what tells the master the frame is over, so
		// this condition is load-bearing, not cosmetic: a digital pad's
		// five-byte frame acks four times and then goes quiet. Handled by a
		// separate process (see below) triggered via a named event, so its
		// ACK latency and pulse width can't stall this always block against
		// the next bit/byte edges.
		if (byte_idx < (3 + PAYLOAD_BYTES)) -> byte_done;
	end
	else begin
		bit_idx = bit_idx + 1;
	end
end

// ACK pulse, decoupled from the bit/byte shift process above so its blocking
// delays can never stall bit timing. The event fires on the byte's last falling
// edge, half a bus-clock period (2 us at 250 kHz) before the master finishes
// sampling the byte, so the master observes ACK_DELAY_NS - 2 us of wait.
// Delay plus width stays well inside one byte time (32 us at 250 kHz), so this
// process is always idle again before the next byte_done fires.
always @(byte_done) begin
	#ACK_DELAY_NS;      // ~10-15 us after the last clock edge, as real pads do
	ack = 1'b0;
	#ACK_WIDTH_NS;      // ~4 us low, well inside the real 2-10 us
	ack = 1'b1;
end

endmodule
