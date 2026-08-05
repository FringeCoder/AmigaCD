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

localparam ST_IDLE   = 3'd0;
localparam ST_ATT    = 3'd1;
localparam ST_BYTE   = 3'd2;
localparam ST_ACK    = 3'd3;
localparam ST_DONE   = 3'd4;
localparam ST_GAP    = 3'd5;

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
// Bit 0 of the byte currently being shifted in. The pad model advances its
// output on every CLK falling edge starting from the very first one, having
// preloaded bit 0 before any edge at all (at ATT falling, or at the previous
// byte's 8th falling edge). So by the time our first rising-edge sample
// lands, the pad has already moved on to bit 1: the ST_BYTE loop's
// shift_in[k] actually captures reply bit (k+1), and bit 0 is only ever
// observable in the window before that byte's first falling edge. Latch it
// there (see ST_ATT and ST_ACK below) instead of trying to recover it from
// shift_in.
reg        bit0_latch;

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
wire ack_in = user_in[3];

integer i;

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
		bit0_latch <= 1'b0;
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
		if (div == HALF) begin
			div        <= 0;
			scmd       <= shift_out[0];
			bit0_latch <= dat_in;   // byte 0's bit 0, valid before the first falling edge
			state      <= ST_BYTE;
		end
		else div <= div + 1'b1;
	end

	ST_BYTE: begin
		if (div == HALF) begin
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
					state  <= ST_ACK;
				end
				else bitcnt <= bitcnt + 1'b1;
			end
		end
		else div <= div + 1'b1;
	end

	ST_ACK: begin
		rx_byte[bytecnt] <= { shift_in[6:0], bit0_latch };
		// Nine bytes covers ID + 0x5A + six payload bytes, which is every device
		// type we care about. Short replies simply read back as 0xFF.
		if (bytecnt == 4'd8) begin
			state <= ST_DONE;
		end
		else begin
			bytecnt    <= bytecnt + 1'b1;
			shift_out  <= cmd_byte(bytecnt + 4'd1);
			bit0_latch <= dat_in;   // next byte's bit 0, valid before its first falling edge
			div        <= 0;
			state      <= ST_BYTE;
		end
	end

	ST_DONE: begin
		att_n <= 1'b1;
		scmd  <= 1'b1;
		if (port == 1'b0) id0 <= rx_byte[1];
		else              id1 <= rx_byte[1];
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
