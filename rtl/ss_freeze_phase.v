`timescale 1ns/1ns

// The two clk_sys-domain signals that carry the save state freeze into the
// Amiga's clock generator.
//
// Both were written inline in Minimig.sv and a second time in the core repo's
// rtl/sim/ssmux/tb_ss_regbus_mux.sv, which reproduces this wiring around a real
// minimig instance. Two copies of a rule the hardware depends on is one copy too
// many -- especially this rule, whose cck polarity reads backwards and was got
// wrong once. With one module both sides instantiate, they cannot disagree.
//
// WHAT THE FREEZE IS. Minimig.sv runs two amiga_clk instances: a master that
// free-runs and drives sdram_ctrl and chipdma_arb, and the Amiga's own, whose
// `ce` is this module's `freeze_7m` inverted. Dropping ce holds clk7_en,
// clk7n_en, c1, c3, cck and eclk where they are, which stops the whole chipset
// at once. It cannot be done by gating those signals: c1/c3/cck/eclk are
// Gray-coded phase LEVELS, and a one-in-four phase decode off a held level is
// true every cycle rather than never.
//
// WHY IT IS SAMPLED. freeze arrives asynchronously to the 7 MHz phase. Sampling
// it on a fixed phase makes the pause a whole number of phases, so the Amiga's
// generator resumes on the phase it stopped on and stays in step with the
// master -- which matters because minimig's chip bus and sdram_ctrl's chip slots
// are both aligned to their own c1, and chipdma_arb samples one with the other.

module ss_freeze_phase
(
	input      clk,          // clk_sys, 28.37516 MHz
	input      rst_n,

	// The MASTER generator's outputs. Not the Amiga's: the Amiga's stop when
	// the freeze takes hold, which would leave nothing to release it.
	input      clk7_en,
	input      cck,

	// ss_ctrl's freeze request, and ss_regshadow's replay write strobe.
	input      freeze,
	input      replay_we,

	// ce for the Amiga's amiga_clk is ~freeze_7m.
	output reg freeze_7m,

	// The register-decode tick for a replay. See minimig.v's ss_replay_tick:
	// a restore's replay runs inside the freeze, so the Amiga's clk7_en is
	// dead and every chipset register decode -- all of which are
	// `always @(posedge clk) if (clk7_en)` -- would never fire. minimig.v ORs
	// this into the clock enable of the modules on the register bus. Gated by
	// replay_we so only the cycles carrying a write produce a tick; the
	// addresses the replay skips step nothing.
	output     replay_tick
);

// Sampled on clk7_en AND cck, which parks the Amiga with cck LOW.
//
// The polarity reads backwards and is not. clk7_en is high on the cycle where
// amiga_clk's phase counter holds 2'b01, and that is the same edge on which
// cck toggles; the freeze only takes hold on the edge after it. So sampling
// while cck is high parks it low.
//
// Parking low is what the replay tick needs. agnus_beamcounter increments hpos
// under `clk7_en && cck`, so with cck parked high the ~490 ticks of a replay
// would walk the beam about two raster lines forward. Parked low they cannot
// move it at all. The core repo's tb_ss_regbus_mux checks the beam position
// across a frozen replay, with the real generator, rather than checking this
// polarity -- so getting it backwards shows up as movement.
//
// This is also where freeze crosses from clk_114 into clk_sys in Minimig.sv;
// the two come from the same PLL at 4:1, so it is a timed path, not a CDC.
always @(posedge clk) begin
	if (!rst_n)               freeze_7m <= 1'b0;
	else if (clk7_en && cck)  freeze_7m <= freeze;
end

assign replay_tick = clk7_en & replay_we;

endmodule
