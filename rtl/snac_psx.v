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

// Bus clock generation. Half-period counter: at 50 MHz and 250 kHz that is 100
// clocks per half period.
localparam integer HALF = (CLK_MHZ * 1000) / (BAUD_KHZ * 2);

localparam ST_IDLE      = 3'd0;
localparam ST_ATT       = 3'd1;
localparam ST_BYTE      = 3'd2;
localparam ST_BYTE_DONE = 3'd3;
localparam ST_DONE      = 3'd4;
localparam ST_GAP       = 3'd5;

reg  [2:0] state;
reg [15:0] div;
reg  [3:0] bitcnt;
reg  [3:0] bytecnt;
reg        port;          // 0 = SNAC port 1, 1 = SNAC port 2
reg        sclk;
reg        scmd;
reg        att_n;
reg  [7:0] shift_out;
reg  [7:0] shift_in;
reg  [7:0] rx_byte [0:8];
reg [15:0] gap;

// Command bytes: 0x01 selects the controller, 0x42 is the poll, then zeroes.
function [7:0] cmd_byte(input [3:0] idx);
	case (idx)
		4'd0: cmd_byte = 8'h01;
		4'd1: cmd_byte = 8'h42;
		default: cmd_byte = 8'h00;
	endcase
endfunction

assign user_out = { 1'b1,                  // [6] csync, Plan 2
                    sclk,                  // [5] CLK
                    1'b1,                  // [4] DAT (input)
                    1'b1,                  // [3] ACK (input)
                    scmd,                  // [2] CMD
                    (port == 1'b0) ? att_n : 1'b1,   // [1] ~ATT port 1
                    (port == 1'b1) ? att_n : 1'b1 }; // [0] ~ATT port 2

wire dat_in = user_in[4];

// PSX byte 0: [0] SELECT [1] L3 [2] R3 [3] START [4] UP [5] RIGHT [6] DOWN [7] LEFT
// PSX byte 1: [0] L2 [1] R2 [2] L1 [3] R1 [4] TRIANGLE [5] O [6] X [7] SQUARE
// All active low, hence the inversion into an active-high vector.
function [15:0] decode(input [7:0] b0, input [7:0] b1);
	decode = { ~b0[2], ~b0[1], ~b1[1], ~b1[0],    // R3 L3 R2 L2
	           ~b0[0], ~b0[3], ~b1[3], ~b1[2],    // SELECT START R1 L1
	           ~b1[4], ~b1[7], ~b1[5], ~b1[6],    // TRIANGLE SQUARE O X
	           ~b0[4], ~b0[6], ~b0[7], ~b0[5] };  // UP DOWN LEFT RIGHT
endfunction

always @(posedge clk) begin
	if (reset || !enable) begin
		state   <= ST_IDLE;
		div     <= 0;
		bitcnt  <= 0;
		bytecnt <= 0;
		port    <= 0;
		sclk    <= 1'b1;
		scmd    <= 1'b1;
		att_n   <= 1'b1;
		gap     <= 0;
		id0     <= 8'h00;
		id1     <= 8'h00;
		pad0    <= 16'h0000;
		pad1    <= 16'h0000;
		axes0   <= 32'h80808080;
		axes1   <= 32'h80808080;
	end
	else case (state)

	ST_IDLE: begin
		att_n     <= 1'b0;
		sclk      <= 1'b1;
		bytecnt   <= 0;
		bitcnt    <= 0;
		shift_out <= cmd_byte(4'd0);
		div       <= 0;
		state     <= ST_ATT;
	end

	// Settle time after ATT falls before the first clock edge.
	ST_ATT: begin
		if (div == HALF - 1) begin
			div   <= 0;
			scmd  <= shift_out[0];
			state <= ST_BYTE;
		end
		else div <= div + 1'b1;
	end

	ST_BYTE: begin
		if (div == HALF - 1) begin
			div <= 0;
			if (sclk) begin
				// Falling edge: drive the next CMD bit.
				sclk <= 1'b0;
				scmd <= shift_out[bitcnt];
			end
			else begin
				// Rising edge: sample DAT.
				sclk            <= 1'b1;
				shift_in[bitcnt] <= dat_in;
				if (bitcnt == 4'd7) begin
					bitcnt <= 0;
					state  <= ST_BYTE_DONE;
				end
				else bitcnt <= bitcnt + 1'b1;
			end
		end
		else div <= div + 1'b1;
	end

	ST_BYTE_DONE: begin
		rx_byte[bytecnt] <= shift_in;
		// Nine bytes covers ID + 0x5A + six payload bytes, which is every device
		// type we care about. Short replies simply read back as 0xFF.
		if (bytecnt == 4'd8) begin
			state <= ST_DONE;
		end
		else begin
			bytecnt   <= bytecnt + 1'b1;
			shift_out <= cmd_byte(bytecnt + 4'd1);
			div       <= 0;
			state     <= ST_BYTE;
		end
	end

	ST_DONE: begin
		att_n <= 1'b1;
		scmd  <= 1'b1;
		// An idle bus reads back all ones. 0xFF is not a device ID, so report
		// absence rather than letting a floating bus look like a controller.
		// pad0/pad1 are gated the same way (not just decoded from the raw
		// bytes) so a button still held at the moment a pad is unplugged
		// cannot stay latched: every branch below always assigns a fresh
		// value, never leaves the old one in place.
		if (port == 1'b0) begin
			id0  <= (rx_byte[1] == 8'hFF) ? 8'h00 : rx_byte[1];
			pad0 <= (rx_byte[1] == 8'hFF) ? 16'h0000 : decode(rx_byte[3], rx_byte[4]);
			// Only a pad in analog mode sends axes; anything else keeps centre,
			// so a digital pad never reads as a stick held hard over.
			axes0 <= (rx_byte[1] == 8'h73)
			         ? { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] }
			         : 32'h80808080;
		end
		else begin
			id1  <= (rx_byte[1] == 8'hFF) ? 8'h00 : rx_byte[1];
			pad1 <= (rx_byte[1] == 8'hFF) ? 16'h0000 : decode(rx_byte[3], rx_byte[4]);
			axes1 <= (rx_byte[1] == 8'h73)
			         ? { rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5] }
			         : 32'h80808080;
		end
		gap   <= 0;
		state <= ST_GAP;
	end

	// Gap between ports, and between polls of the same port.
	ST_GAP: begin
		if (gap == 16'hFFFF) begin
			port  <= ~port;
			state <= ST_IDLE;
		end
		else gap <= gap + 1'b1;
	end

	default: state <= ST_IDLE;
	endcase
end

endmodule
