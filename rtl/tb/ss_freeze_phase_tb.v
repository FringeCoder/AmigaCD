`timescale 1ns/1ns

// ss_freeze_phase: the sampling rule and the replay tick.
//
// The bench drives a MODEL of amiga_clk's phase pattern rather than the module
// itself -- amiga_clk lives in the core repo, and copying it here to test a
// consumer would create exactly the second copy this module exists to remove.
// The model reproduces only what the rule depends on: clk7_en is one cycle in
// four, and cck toggles on the same edge, so consecutive clk7_en pulses see
// alternating cck.
//
// The claim that follows from that -- the Amiga's generator parks with cck low,
// so a replay's ticks cannot move the beam -- is checked in the core repo's
// rtl/sim/ssmux/tb_ss_regbus_mux.sv against the real amiga_clk and the real
// agnus_beamcounter. It cannot be checked here and is deliberately not
// restated here: a second model of the parking behaviour would be a second
// thing to be wrong.

module ss_freeze_phase_tb;

integer errors = 0;

task check(input [511:0] name, input actual, input want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %b, want %b", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

reg clk   = 1'b0;
reg rst_n = 1'b0;
always #5 clk = ~clk;

// amiga_clk's phase pattern, reduced to what this module reads. clk7_en is the
// cycle where the counter holds 2'b01; cck flips on that same edge, so the two
// clk7_en pulses in a cck period see cck=0 and cck=1 in turn.
reg [1:0] phase = 2'b00;
reg       cck   = 1'b0;
wire      clk7_en = (phase == 2'b01);

always @(posedge clk) begin
	phase <= phase + 2'd1;
	if (phase == 2'b01) cck <= ~cck;
end

reg freeze    = 1'b0;
reg replay_we = 1'b0;

wire freeze_7m;
wire replay_tick;

ss_freeze_phase dut
(
	.clk         (clk        ),
	.rst_n       (rst_n      ),
	.clk7_en     (clk7_en    ),
	.cck         (cck        ),
	.freeze      (freeze     ),
	.replay_we   (replay_we  ),
	.freeze_7m   (freeze_7m  ),
	.replay_tick (replay_tick)
);

// Every sampling instant the module is allowed to use, counted independently of
// the module, so "it only moved on the right edge" is checkable rather than
// assumed.
integer sample_edges = 0;
always @(posedge clk) if (rst_n && clk7_en && cck) sample_edges = sample_edges + 1;

integer moves = 0;
reg     freeze_7m_d = 1'b0;
always @(posedge clk) begin
	freeze_7m_d <= freeze_7m;
	if (rst_n && freeze_7m !== freeze_7m_d) moves = moves + 1;
end

// Wait for the next instant the module may sample on, then one more clock for
// the registered output.
task wait_sample;
begin
	@(posedge clk);
	while (!(clk7_en && cck)) @(posedge clk);
	@(posedge clk);
end
endtask

initial begin
	#20000;
	$display("FAIL watchdog: simulation did not finish");
	$fatal(1);
end

initial begin
	repeat (4) @(posedge clk);
	rst_n = 1'b1;

	check("freeze_7m low out of reset", freeze_7m, 1'b0);

	// ------------------------------------------------------------- the tick
	//
	// replay_tick is the master's clk7_en gated by replay_we, and nothing
	// else. It must be silent whenever the replay is not writing, because
	// minimig.v ORs it into the chipset's clock enable: a tick outside a
	// replay write steps agnus, paula and denise for no reason.
	replay_we = 1'b0;
	@(posedge clk);
	while (!clk7_en) @(posedge clk);
	check("no tick without replay_we", replay_tick, 1'b0);

	replay_we = 1'b1;
	@(negedge clk);
	while (!clk7_en) @(negedge clk);
	check("tick on clk7_en with replay_we", replay_tick, 1'b1);

	// And only on clk7_en: a level would give the chipset four ticks per 7 MHz
	// phase, which is four writes of the same address, and for a set/clear
	// register four writes is not the same as one.
	@(negedge clk);
	while (clk7_en) @(negedge clk);
	check("no tick between clk7_en pulses", replay_tick, 1'b0);
	replay_we = 1'b0;

	// -------------------------------------------------------- the sampling
	//
	// freeze must not reach freeze_7m on any edge except clk7_en && cck. A
	// design that sampled on clk7_en alone passes the raise and fall checks
	// below and fails this one: freeze goes up in the middle of a phase and
	// must sit there unnoticed until the sampling instant comes round.
	moves  = 0;

	// Raise it just AFTER a sampling instant, so the next clk7_en pulse is the
	// one carrying cck low. Raising it anywhere would test less: half the time
	// the sampling instant comes first and the run proves nothing about which
	// pulse was used.
	@(posedge clk);
	while (!(clk7_en && cck)) @(posedge clk);
	@(posedge clk);
	freeze = 1'b1;
	check("freeze ignored off the sampling phase", freeze_7m, 1'b0);

	// The discriminating check. Half the clk7_en pulses carry cck low, and
	// those must NOT sample: a module written `else if (clk7_en)` takes the
	// request here, one colour clock early, and parks the Amiga on the other
	// phase -- the phase on which a replay's ticks walk the beam. Every other
	// check in this bench passes with that mistake in place.
	@(posedge clk);
	while (!(clk7_en && !cck)) @(posedge clk);
	@(posedge clk);
	check("clk7_en with cck low does not sample", freeze_7m, 1'b0);

	wait_sample();
	check("freeze taken on clk7_en && cck", freeze_7m, 1'b1);
	check("it moved exactly once", (moves == 1), 1'b1);

	// It stays up on every other edge, including the clk7_en pulses where cck
	// is low -- those must not resample.
	repeat (12) @(posedge clk);
	check("freeze_7m holds while freeze holds", freeze_7m, 1'b1);
	check("still exactly one move", (moves == 1), 1'b1);

	// The release takes the same instant, so the pause is a whole number of
	// colour clocks in both directions.
	@(posedge clk);
	while (clk7_en && cck) @(posedge clk);
	freeze = 1'b0;
	@(posedge clk);
	check("release ignored off the sampling phase", freeze_7m, 1'b1);

	wait_sample();
	check("release taken on clk7_en && cck", freeze_7m, 1'b0);
	check("it moved exactly twice in all", (moves == 2), 1'b1);

	// Reset wins over a standing request, which is what a machine being reset
	// mid-save state has to do.
	freeze = 1'b1;
	wait_sample();
	check("freeze up again", freeze_7m, 1'b1);
	// Dropped on a negedge and read on the next one, so the check reads the
	// value the reset edge produced rather than racing it.
	@(negedge clk);
	rst_n = 1'b0;
	@(posedge clk);
	@(negedge clk);
	check("reset clears freeze_7m", freeze_7m, 1'b0);
	rst_n = 1'b1;

	if (sample_edges == 0) begin
		$display("FAIL the bench never reached a sampling instant");
		errors = errors + 1;
	end

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
