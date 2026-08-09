`timescale 1ns/1ns

// Reads a chip RAM range out through the SDRAM controller's CPU port.
//
// Borrowing the CPU port rather than adding one is deliberate. sdram_ctrl's
// sd_addr register is the core's worst timing path, and widening its arbiter
// would add fan-in exactly where there is none to spare. The port is free
// because the machine is frozen: no CPU cycle can be in flight.
//
// cache_inhibit is asserted throughout. The CPU port is cached, and chip DMA
// writes do not pass through that cache, so an uninhibited dump could return
// stale data for anything the blitter or copper wrote.
//
// sd_ready is read fresh every cycle rather than edge-detected, so this
// module makes no assumption about a fixed or minimum response latency: two
// different words back to back are each counted exactly once, in order, no
// gap required.
//
// Caller obligation this relies on: sd_ready must pulse for exactly the
// cycle its word is valid, then drop, even though sd_cs stays asserted for
// the whole multi-word transfer (it only deasserts once, at the end -- see
// below). This is *not* how cpu_cache_new.v's cpu_ack behaves on its own: on
// a cache hit or fill completion cpu_ack is a level that stays high for as
// long as cpu_cs stays high (cleared only by "if (!cpu_cs) cpu_ack <= 0"),
// and the cache's own state machine sits in CPU_SM_WAIT/CPU_SM_FILLW until
// cpu_cs drops -- it never advances to service a second address while cs
// stays up. Wiring ss_dma's sd_cs/sd_addr/sd_ready straight onto sdram_ctrl's
// cpuCS/cpuAddr/ramready as-is would, on word 2, either resample word 1's
// data forever (if ramready is treated as live) or hang (the cache never
// looks at the new address). Whatever connects this module to sdram_ctrl
// must convert per-word semantics into the port's real bus-cycle protocol
// (drop cpuCS for at least one cycle between words) or must present a
// single-shot, non-cached path instead. Not this module's job to decide --
// there is no wiring task yet -- but it is not safe to assume away.

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

	output reg        word_valid,
	output reg [15:0] word_out,
	output reg        done
);

assign sd_state         = 2'd2;
assign sd_uds_n         = 1'b0;
assign sd_lds_n         = 1'b0;
assign sd_cache_inhibit = sd_cs;

reg [23:0] remaining;
reg        busy;

always @(posedge clk) begin
	if (!rst_n) begin
		sd_cs      <= 1'b0;
		sd_addr    <= 24'd0;
		word_valid <= 1'b0;
		done       <= 1'b0;
		busy       <= 1'b0;
		remaining  <= 24'd0;
	end
	else begin
		word_valid <= 1'b0;
		done       <= 1'b0;

		if (start) begin
			sd_addr   <= base_addr;
			remaining <= word_count;
			busy      <= (word_count != 24'd0);
			sd_cs     <= (word_count != 24'd0);
			done      <= (word_count == 24'd0);
		end
		else if (busy && sd_ready) begin
			word_out   <= sd_rd;
			word_valid <= 1'b1;
			if (remaining == 24'd1) begin
				sd_cs <= 1'b0;
				busy  <= 1'b0;
				done  <= 1'b1;
			end
			else begin
				sd_addr <= sd_addr + 24'd1;
			end
			remaining <= remaining - 24'd1;
		end
	end
end

endmodule
