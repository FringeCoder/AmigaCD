`timescale 1ns/1ns

// Custom chipset register shadow.
//
// Phase 1A's state vector is the CPU and chip RAM and nothing else, which is
// why a restore put a correct CPU and a correct memory into a chipset that
// still belonged to the moment before -- CIA timers already overflowed, INTREQ
// holding bits for interrupts the restored code has no idea about, Denise and
// Agnus configured for a different frame. The Amiga reset every time.
//
// The chipset has no read path for most of its registers: Denise's colours,
// the bitplane control words, the sprite and audio pointers are all write-only
// in hardware, and this core is faithful about that. So they cannot simply be
// exported the way the CPU's register file was. What CAN be observed is the
// bus every one of those writes arrives on.
//
// minimig.v routes exactly two wires to agnus, paula, denise and both CIAs:
// reg_address (agnus's reg_address_out) and custom_data_in (gary's). Every
// chipset register write in the machine is a value on custom_data_in while
// reg_address holds that register's address, taken on clk7_en. There is no
// write strobe and none is needed -- the address IS the strobe, which is why
// the real modules decode writes with a bare address compare (see
// paula_intcontroller.v:52). Snooping those two wires therefore sees every
// write, using the same decode the hardware uses, with no change to any
// chipset module.
//
// WHAT THIS DOES NOT CAPTURE, and why that is survivable:
//
//  - State the chipset advances on its own. A bitplane pointer moved by DMA,
//    an audio pointer walked by Paula, a CIA timer counting down. The shadow
//    holds what was last WRITTEN, not where the hardware has since got to.
//    Bitplane and audio pointers are rewritten every frame by the copper in
//    practically all software, so they re-converge within a frame of the
//    restore. CIA timers do not, and are captured by value in a later step --
//    the CIAs, unlike Denise, do have readable registers.
//
//  - Set/clear registers. DMACON, INTENA, INTREQ and ADKCON take bit 15 as
//    "set the bits I have raised" and otherwise "clear them", so the last
//    value written is not the register's contents and replaying it verbatim
//    would be wrong. They are deliberately NOT shadowed here; they have read
//    counterparts (DMACONR, INTENAR, INTREQR, ADKCONR) and are captured by
//    value elsewhere. `writable` below excludes them by address.
//
//  - Strobes. STREQU, STRVBL, STRHOR, STRLONG, COPJMP1/2 do something when
//    written rather than storing anything, so replaying one would fire it at
//    the worst possible moment. Also excluded by address.
//
// Reads are unambiguous for free: the Amiga puts read and write registers at
// different addresses (INTENAR $01C reads what INTENA $09A writes), so a read
// cycle never looks like a write to the register it reads.

module ss_regshadow
(
	input             clk,
	input             clk7_en,
	input             rst_n,

	// The two buses, tapped exactly as the chipset modules see them.
	input       [8:1] reg_address_in,
	input      [15:0] data_in,

	// Readback for the save. Combinational on addr, one entry per register.
	input       [7:0] rd_addr,
	output     [15:0] rd_data,

	// Whether the entry at rd_addr is one this module tracks. The save side
	// streams every entry regardless -- a fixed-length section is far easier
	// to validate than a sparse one -- but the restore side must only replay
	// the tracked ones, and this is the same predicate both sides use so they
	// cannot disagree about which those are.
	output            rd_writable,

	// Whether replaying rd_addr needs the set/clear dance rather than a plain
	// write: first a write of 16'h7FFF to clear every bit, then 16'h8000 with
	// the wanted bits raised. Writing rd_data straight back would set the bits
	// that happen to be 1 and clear nothing, so a register that should have
	// gone from 0x03FF to 0x0060 would keep every bit it already had.
	output            rd_setclear,

	// ------------------------------------------------------------- replay
	//
	// Writes the shadow back into the chipset by driving the same two buses the
	// machine writes them on. Minimig.sv muxes reg_address and custom_data_in
	// onto these while replay_active is high; the chipset cannot tell the
	// difference, because there is nothing to tell -- an address and a value on
	// a clk7_en tick is exactly what a CPU or copper write is.
	//
	// Must only be started while the machine is frozen. Nothing here checks
	// that; ss_ctrl owns the freeze and the ordering.
	// Load path, used by a restore to put the saved shadow back before replaying
	// it. Separate from the snoop because the two run at different times and for
	// different reasons: the snoop follows the machine, this overwrites it. It
	// bypasses writable() deliberately -- the payload is a fixed 256-entry
	// section, so every index is written, and the excluded ones simply hold
	// values the replay will never look at.
	input             ld_we,
	input       [7:0] ld_addr,
	input      [15:0] ld_data,

	input             replay_start,
	input      [14:0] intreq_in,     // by value; see the header
	output reg        replay_active,

	// High only on the cycles carrying an actual write. replay_active alone is
	// not enough to mux on: the sequencer skips every excluded address, and
	// while it skips, the registered outputs still hold whatever they held
	// last -- address $000 at the start of a replay. Muxing on the level would
	// therefore drive a read-window address onto the bus for those cycles,
	// which is exactly what excluding them was for. Caught by the bench.
	output reg        replay_we,
	output reg  [8:1] replay_addr,
	output reg [15:0] replay_data,
	output reg        replay_done
);

// Register addresses are 9 bits ($000-$1FE) and always even, so reg_address_in
// carries [8:1] and the shadow is indexed by those 8 bits: 256 entries.
reg [15:0] shadow [0:255];

wire [7:0] wr_idx = reg_address_in;

// DMACON, INTENA and ADKCON live OUTSIDE the array, in three named registers.
//
// Not tidiness: they are why the array would not infer as a memory. As entries,
// the set/clear rule made every write a read-modify-write on the array, and
// sc_value() read three more fixed addresses out of it, giving the array four
// read ports. Quartus built it from flip-flops instead -- ~4700 ALMs for four
// kilobytes, and two fits that missed timing.
localparam [7:0] IDX_DMACON = 8'h4B;   // $096 >> 1
localparam [7:0] IDX_INTENA = 8'h4D;   // $09A >> 1
localparam [7:0] IDX_ADKCON = 8'h4F;   // $09E >> 1

reg [15:0] r_dmacon;
reg [15:0] r_intena;
reg [15:0] r_adkcon;

initial begin
	r_dmacon = 16'd0;
	r_intena = 16'd0;
	r_adkcon = 16'd0;
end

// Value of a set/clear register by index.
//
// Deliberately NOT a function. A function called from a continuous assignment
// is re-evaluated when its ARGUMENTS change, not when signals it reads inside
// change -- so a `wire cur = sc_reg(wr_idx)` held its value for as long as the
// address stayed put, and DMACON accumulated exactly once and then froze. The
// directed accumulate-then-read test caught it at the first step; the original
// checks only showed it three steps later, looking like an ordinary mismatch.
`define SC_REG(idx) ((idx) == IDX_DMACON ? r_dmacon :                      (idx) == IDX_INTENA ? r_intena : r_adkcon)

// ---------------------------------------------------------------- exclusions
//
// Held as a function rather than a table so the reasons stay next to the
// addresses. Both the snoop and the readback use it, so an address can never
// be recorded but not replayed, or replayed from an entry nothing maintains.
function writable(input [7:0] idx);
	reg [8:0] a;
begin
	a = {idx, 1'b0};
	writable =
		// INTREQ is the one set/clear register hardware also writes: Paula
		// raises a bit whenever it wants an interrupt. Accumulating writes
		// here would drift from the real register within a frame, so it is
		// captured from paula_intcontroller's own intreq instead.
		(a != 9'h09C) &&
		// Strobes. Writing one is an event, not a value.
		(a != 9'h038) &&   // STREQU
		(a != 9'h03A) &&   // STRVBL
		(a != 9'h03C) &&   // STRHOR
		(a != 9'h03E) &&   // STRLONG
		(a != 9'h08A) &&   // COPJMP1
		(a != 9'h08C) &&   // COPJMP2
		// Below $020 is the read window (DMACONR, INTENAR, JOYxDAT, the disk
		// and blitter status). Nothing writes there, so an entry would only
		// ever be noise -- and replaying into it would drive a read address
		// onto the bus for a cycle.
		(a >= 9'h020);
end
endfunction

// DMACON, INTENA and ADKCON take bit 15 as "set the bits I have raised" and
// otherwise "clear them", so the last value written is not the register's
// contents. They are still shadowed here rather than exported from paula and
// agnus, because the same rule the hardware applies can be applied to the
// shadow entry and the result IS the contents.
//
// Two reasons that is better than exporting the real registers. DMACON is
// split -- paula keeps bits [4:0] and dmaen, agnus keeps the bitplane, copper,
// blitter and sprite bits -- so an export would have to gather and reassemble
// two partial copies. And an export means new ports through paula.v, agnus.v
// and minimig.v, which is precisely the plumbing this approach exists to
// avoid.
//
// INTREQ is excluded from this and from the shadow entirely: Paula raises its
// bits in hardware, not only by write, so an accumulator would drift from the
// real register within a frame.
function setclear(input [7:0] idx);
	reg [8:0] a;
begin
	a = {idx, 1'b0};
	setclear = (a == 9'h096) ||   // DMACON
	           (a == 9'h09A) ||   // INTENA
	           (a == 9'h09E);     // ADKCON
end
endfunction

wire [15:0] cur = `SC_REG(wr_idx);

// The hardware's own rule, bit for bit: see paula.v:167 and
// paula_intcontroller.v:54. Bit 15 is the direction and never stored.
wire [15:0] applied = data_in[15] ? (cur |  {1'b0, data_in[14:0]})
                                  : (cur & ~{1'b0, data_in[14:0]});


// Zeroed at power-up rather than on reset. An `initial` block is the memory's
// initial contents to Quartus -- it costs no logic and does not stop inference
// -- whereas a reset over the array does both.
//
// The contents matter, so this is not decoration: a save streams all 256
// entries, and an entry for a register the game never wrote would otherwise be
// undefined, get written into the payload, and be replayed into the chipset on
// restore. Zero is the value the machine powers up with.
initial begin : shadow_init
	integer k;
	for (k = 0; k < 256; k = k + 1) shadow[k] = 16'd0;
end

// No reset over the array, deliberately. Clearing 256 entries on reset stops
// Quartus inferring a memory for them: it built 4096 flip-flops and a 256-way
// read mux instead, which cost ~5000 ALMs (63% -> 75% of the device) and put
// the design 0.357 ns behind timing. Nothing needs the reset either -- a
// restore's load writes every entry of the section before the replay reads
// any of them, and outside a restore the machine's own writes fill it.
always @(posedge clk) begin
	if (ld_we) begin
		// A restore loading the saved section. Takes precedence over the snoop:
		// the machine is frozen while this runs, so there is nothing legitimate
		// for the snoop to see, and if there were, the payload is what the
		// restore is here to install.
		// Loaded values are contents, not writes, so the set/clear registers
		// take them verbatim rather than through the accumulate rule.
		if      (ld_addr == IDX_DMACON) r_dmacon <= ld_data;
		else if (ld_addr == IDX_INTENA) r_intena <= ld_data;
		else if (ld_addr == IDX_ADKCON) r_adkcon <= ld_data;
		else                            shadow[ld_addr] <= ld_data;
	end
	else if (clk7_en && writable(wr_idx)) begin
		if      (wr_idx == IDX_DMACON) r_dmacon <= applied;
		else if (wr_idx == IDX_INTENA) r_intena <= applied;
		else if (wr_idx == IDX_ADKCON) r_adkcon <= applied;
		else                           shadow[wr_idx] <= data_in;
	end
end

// ONE read port on the array, shared between the save readback and the replay
// walk. They never run together: a save is not a restore.
wire [7:0]  mem_addr = replay_active ? ridx[7:0] : rd_addr;
wire [15:0] mem_q    = shadow[mem_addr];

assign rd_data     = setclear(rd_addr) ? `SC_REG(rd_addr) : mem_q;
assign rd_writable = writable(rd_addr);
assign rd_setclear = setclear(rd_addr);

// ------------------------------------------------------------------- replay
//
// Order is the whole design here. Replaying the shadow in address order would
// bring DMA and interrupts up partway through, against a chipset that is half
// old and half new, and the resulting misbehaviour would look like a game bug
// rather than a restore bug.
//
//   1. every plain register, in address order
//   2. ADKCON      -- audio/disk modulation, wanted before DMA starts
//   3. INTREQ      -- the pending interrupts, from the exported value
//   4. INTENA      -- enables interrupts, so after the requests are in place
//   5. DMACON      -- starts DMA, so last of all
//
// Steps 2-5 are set/clear registers and take two writes each: 0x7FFF to clear
// every bit, then 0x8000 with the wanted bits raised.
//
// Strobes and the read window are never replayed -- writable() excludes them,
// and step 1 skips anything it excludes, so the two sides cannot disagree.
localparam [2:0] R_IDLE   = 3'd0;
localparam [2:0] R_PLAIN  = 3'd1;
localparam [2:0] R_ADKCON = 3'd2;
localparam [2:0] R_INTREQ = 3'd3;
localparam [2:0] R_INTENA = 3'd4;
localparam [2:0] R_DMACON = 3'd5;
localparam [2:0] R_DONE   = 3'd6;

reg [2:0] rstate;
reg [8:0] ridx;      // 9 bits so it can pass 255 and terminate
reg       rphase;    // 0 = the clearing write, 1 = the setting write

// The value each set/clear step restores. INTREQ is the exported register
// rather than a shadow entry, for the reason in the header.
function [15:0] sc_value(input [2:0] st);
begin
	case (st)
		R_ADKCON: sc_value = r_adkcon;
		R_INTREQ: sc_value = {1'b0, intreq_in};
		R_INTENA: sc_value = r_intena;
		default:  sc_value = r_dmacon;
	endcase
end
endfunction

function [8:1] sc_addr(input [2:0] st);
begin
	case (st)
		R_ADKCON: sc_addr = 8'h4F;
		R_INTREQ: sc_addr = 8'h4E;
		R_INTENA: sc_addr = 8'h4D;
		default:  sc_addr = 8'h4B;
	endcase
end
endfunction

// One step per clk7_en, because that is the tick the chipset decodes writes on.
// Holding an address for several clk7_en ticks would write it several times,
// which is harmless for a plain register and wrong for a set/clear one.
always @(posedge clk) begin
	if (!rst_n) begin
		rstate        <= R_IDLE;
		ridx          <= 9'd0;
		rphase        <= 1'b0;
		replay_active <= 1'b0;
		replay_we     <= 1'b0;
		replay_done   <= 1'b0;
		replay_addr   <= 8'd0;
		replay_data   <= 16'd0;
	end
	else begin
		replay_done <= 1'b0;

		case (rstate)
		R_IDLE: begin
			if (replay_start) begin
				rstate        <= R_PLAIN;
				ridx          <= 9'd0;
				rphase        <= 1'b0;
				replay_active <= 1'b1;
				replay_we     <= 1'b0;
			end
		end

		R_PLAIN: if (clk7_en) begin
			if (ridx == 9'd256) begin
				rstate    <= R_ADKCON;
				rphase    <= 1'b0;
				replay_we <= 1'b0;
			end
			else if (writable(ridx[7:0]) && !setclear(ridx[7:0])) begin
				replay_addr <= ridx[7:0];
				replay_data <= mem_q;
				replay_we   <= 1'b1;
				ridx        <= ridx + 9'd1;
			end
			else begin
				replay_we <= 1'b0;      // excluded: skipped, nothing driven
				ridx      <= ridx + 9'd1;
			end
		end

		R_ADKCON, R_INTREQ, R_INTENA, R_DMACON: if (clk7_en) begin
			replay_addr <= sc_addr(rstate);
			replay_we   <= 1'b1;
			// Clear everything first, then set what the state says. Both writes
			// are needed: without the clear, bits the machine has set now but
			// the state did not would survive the restore.
			replay_data <= rphase ? (16'h8000 | (sc_value(rstate) & 16'h7FFF))
			                      :  16'h7FFF;
			rphase      <= ~rphase;
			if (rphase) rstate <= rstate + 3'd1;
		end

		// Wait one more clk7_en before dropping the mux. The outputs are
		// registered, so the value set up on the last clk7_en is not sampled by
		// the chipset until the next one -- and the last value is DMACON's set
		// write, the single most ordering-critical write in the sequence.
		// Dropping replay_active first would lose it silently and leave DMA
		// off in a machine that looks entirely restored.
		R_DONE: if (clk7_en) begin
			replay_active <= 1'b0;
			replay_we     <= 1'b0;
			replay_done   <= 1'b1;
			rstate        <= R_IDLE;
		end

		default: rstate <= R_IDLE;
		endcase
	end
end

endmodule
