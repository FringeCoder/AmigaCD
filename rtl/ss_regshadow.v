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
	output            rd_writable
);

// Register addresses are 9 bits ($000-$1FE) and always even, so reg_address_in
// carries [8:1] and the shadow is indexed by those 8 bits: 256 entries.
reg [15:0] shadow [0:255];

wire [7:0] wr_idx = reg_address_in;

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
		// Set/clear registers. Captured by value via their read counterparts;
		// the last write is not the contents. See the header.
		(a != 9'h096) &&   // DMACON
		(a != 9'h09A) &&   // INTENA
		(a != 9'h09C) &&   // INTREQ
		(a != 9'h09E) &&   // ADKCON
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

integer i;

always @(posedge clk) begin
	if (!rst_n) begin
		for (i = 0; i < 256; i = i + 1) shadow[i] <= 16'd0;
	end
	else if (clk7_en && writable(wr_idx)) begin
		shadow[wr_idx] <= data_in;
	end
end

assign rd_data     = shadow[rd_addr];
assign rd_writable = writable(rd_addr);

endmodule
