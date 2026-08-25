`timescale 1ns/1ns

// Reads a chip RAM range out through the SDRAM controller's CPU port.
//
// Borrowing the CPU port rather than adding one is deliberate. sdram_ctrl's
// sd_addr register is the core's worst timing path, and widening its arbiter
// would add fan-in exactly where there is none to spare. The port is free
// because the machine is frozen: no CPU cycle can be in flight.
//
// cache_inhibit is asserted for the whole transfer (from the word after
// `start` through the word `done` commits), not just while `sd_cs` happens
// to be high. The CPU port is cached, and chip DMA writes do not pass
// through that cache, so an uninhibited dump could return stale data for
// anything the blitter or copper wrote.
//
// Per-word handshake, not a held burst. `sd_cs` is asserted for exactly one
// address at a time: wait for `sd_ready`, capture the word, deassert
// `sd_cs` for one cycle, then assert again with the next address. This is
// the real bus-cycle protocol `cpu_cache_new.v` implements, confirmed by
// direct inspection:
//   - `cpu_ack` (the presumptive `sd_ready` source) is a *level*: once
//     asserted on a hit or fill completion it stays asserted for as long as
//     `cpu_cs` stays asserted, and is only cleared by
//     `if (!cpu_cs) cpu_ack <= 1'b0` (cpu_cache_new.v:490).
//   - The cache's own state machine sits in `CPU_SM_WAIT`/`CPU_SM_FILLW`
//     until `cpu_cs` drops, and only then returns to `CPU_SM_IDLE`, where a
//     newly asserted `cpu_cs` begins servicing a new address
//     (cpu_cache_new.v:322,325,370,296-309). It never re-services a second
//     address while `cpu_cs` stays up.
// An earlier version of this module held `sd_cs` high for the whole
// multi-word burst, relying on `sd_ready` pulsing once per word on its own.
// Against the real cache that fails silently: `cpu_ack` would stay high
// after the first word forever (never cleared, since `cpu_cs` never drops),
// so every subsequent cycle would look like "another word ready" while the
// cache's state machine -- frozen in `CPU_SM_WAIT` -- never advances to
// look at the next address. The result is not a hang but something worse:
// word 0 read over and over, silently, with a word count that looks correct.
// The one-word-at-a-time handshake below is what makes this module safe to
// wire straight onto the CPU port's `cpuCS`/`cpuAddr`/`ramready` pins.

module ss_dma
(
	input             clk,
	input             rst_n,

	input             start,
	input      [24:1] base_addr,
	input      [23:0] word_count,

	// Borrowed sdram_ctrl CPU port. uds_n/lds_n are active low to match
	// sdram_ctrl.v:119 ({!cpuU, !cpuL}); state 2 is "read data" per
	// sdram_ctrl.v:122 (cpustate == 2).
	output reg [24:1] sd_addr,
	output reg        sd_cs,
	output     [1:0]  sd_state,
	output            sd_uds_n,
	output            sd_lds_n,
	output            sd_cache_inhibit,
	input      [15:0] sd_rd,
	input             sd_ready,

	// Write direction, for restore. write_mode low reproduces the read
	// behaviour byte for byte. The caller presents the first word before
	// `start` and advances on each word_req pulse, so the data for an address
	// is stable for the whole time chip select is asserted for it.
	input             write_mode,
	input      [15:0] word_in,
	output reg        word_req,
	output     [15:0] sd_wr,

	output reg        word_valid,
	output reg [15:0] word_out,
	output reg        done
);

// State 3 is "write data", state 2 "read data" (sdram_ctrl.v:120,122).
assign sd_state         = write_mode ? 2'd3 : 2'd2;
assign sd_wr            = word_in;
assign sd_uds_n         = 1'b0;
assign sd_lds_n         = 1'b0;
assign sd_cache_inhibit = busy;

// xfer_state sequences a single in-flight address at a time:
//   XFER_IDLE - no transfer in progress, sd_cs held low.
//   XFER_REQ  - sd_cs asserted for the current address, waiting for
//               sd_ready.
//   XFER_GAP  - sd_cs deasserted, waiting for the cache to drop sd_ready
//               before the next address is presented.
//
// The gap WAITS for !sd_ready rather than counting a cycle, and that is the
// whole point of it. cpu_cache_new holds cpu_ack asserted until it sees
// !cpu_cs (cpu_cache_new.v:490) and only then returns to CPU_SM_IDLE. A gap of
// exactly one cycle re-asserted chip select while ack could still be high, so
// the next XFER_REQ saw a stale ready and latched the PREVIOUS word -- the
// second read of every two-word transfer returning data from the wrong
// address.
//
// It only showed up on some regions, which is what made it look like an
// address-decode fault rather than a race: a line the CPU has cached (all of
// Kickstart, which runs constantly) acks in a cycle or two and lands inside
// the window, while a sequential chip RAM dump mostly misses and takes long
// enough to settle. Measured against the ROM file on hardware: reads of
// $F80000 came back with words 1 and 3 of each 8-byte group exchanged, while
// an overlap test on chip RAM was clean.
//
// XFER_START does the same wait before the FIRST request, because ss_ctrl
// issues transfers back to back and the tail of the previous one races the
// head of the next in exactly the same way.
localparam [2:0] XFER_IDLE  = 3'd0;
localparam [2:0] XFER_REQ   = 3'd1;
localparam [2:0] XFER_GAP   = 3'd2;
localparam [2:0] XFER_START = 3'd3;

reg [2:0]  xfer_state;
reg [23:0] remaining;
reg        busy;

always @(posedge clk) begin
	if (!rst_n) begin
		xfer_state <= XFER_IDLE;
		sd_cs      <= 1'b0;
		sd_addr    <= 24'd0;
		word_valid <= 1'b0;
		word_req   <= 1'b0;
		done       <= 1'b0;
		busy       <= 1'b0;
		remaining  <= 24'd0;
	end
	else begin
		word_valid <= 1'b0;
		done       <= 1'b0;
		word_req   <= 1'b0;

		if (start) begin
			sd_addr    <= base_addr;
			remaining  <= word_count;
			busy       <= (word_count != 24'd0);
			// Chip select waits for XFER_START to see the port quiet; see the
			// state list above.
			sd_cs      <= 1'b0;
			done       <= (word_count == 24'd0);
			xfer_state <= (word_count != 24'd0) ? XFER_START : XFER_IDLE;
		end
		else begin
			case (xfer_state)
				XFER_REQ: begin
					if (sd_ready) begin
						// A write has no data to return; ask the caller for
						// the next word instead of publishing one.
						word_out   <= sd_rd;
						word_valid <= ~write_mode;
						word_req   <= write_mode;
						sd_cs      <= 1'b0;
						remaining  <= remaining - 24'd1;
						if (remaining == 24'd1) begin
							busy       <= 1'b0;
							done       <= 1'b1;
							xfer_state <= XFER_IDLE;
						end
						else begin
							xfer_state <= XFER_GAP;
						end
					end
				end

				XFER_GAP: begin
					if (!sd_ready) begin
						sd_addr    <= sd_addr + 24'd1;
						sd_cs      <= 1'b1;
						xfer_state <= XFER_REQ;
					end
				end

				XFER_START: begin
					if (!sd_ready) begin
						sd_cs      <= 1'b1;
						xfer_state <= XFER_REQ;
					end
				end

				default: begin // XFER_IDLE
				end
			endcase
		end
	end
end

endmodule
