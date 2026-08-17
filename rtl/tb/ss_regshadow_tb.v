`timescale 1ns/1ns

// ss_regshadow: does it see every chipset register write, and does it refuse
// the ones that must not be replayed?
//
// The interesting failures here are all "recorded something it should not
// have" rather than "missed something", because a missed colour register is a
// wrong colour for one frame while a replayed COPJMP1 fires the copper at an
// arbitrary point in a restore. So the exclusion cases below are the ones that
// matter, and each names why that address is excluded rather than just
// asserting a number.

module ss_regshadow_tb;

reg clk = 0;
reg clk7_en = 0;
reg rst_n = 0;

reg  [8:1] reg_address_in = 8'd0;
reg [15:0] data_in = 16'd0;

reg  [7:0] rd_addr = 8'd0;
wire [15:0] rd_data;
wire        rd_writable;
wire        rd_setclear;

reg         ld_we = 0;
reg   [7:0] ld_addr = 0;
reg  [15:0] ld_data = 0;
reg         replay_start = 0;
reg  [14:0] intreq_in = 0;
wire        replay_active;
wire        replay_we;
wire  [8:1] replay_addr;
wire [15:0] replay_data;
wire        replay_done;

integer errors = 0;
integer i;

ss_regshadow dut
(
	.clk(clk), .clk7_en(clk7_en), .rst_n(rst_n),
	.reg_address_in(reg_address_in), .data_in(data_in),
	.rd_addr(rd_addr), .rd_data(rd_data), .rd_writable(rd_writable),
	.rd_setclear(rd_setclear),
	.ld_we(ld_we), .ld_addr(ld_addr), .ld_data(ld_data),
	.replay_start(replay_start), .intreq_in(intreq_in),
	.replay_active(replay_active), .replay_we(replay_we), .replay_addr(replay_addr),
	.replay_data(replay_data), .replay_done(replay_done)
);

always #5 clk = ~clk;

// Replay monitor. Records every write the sequencer drives, in order, exactly
// as the chipset would take them: on a clk7_en tick while the mux is handed
// over. Order is what the checks below are about, so a monitor that only kept
// final values would miss the failure this design exists to avoid.
integer rec_n = 0;
reg     recording = 0;
reg  [8:1] rec_addr [0:1023];
reg [15:0] rec_data [0:1023];
reg saw_strobe  = 0;
reg saw_readwin = 0;

always @(posedge clk) begin
	if (recording && replay_we && clk7_en) begin
		rec_addr[rec_n] = replay_addr;
		rec_data[rec_n] = replay_data;
		rec_n = rec_n + 1;
		if (replay_addr == 8'h45 || replay_addr == 8'h46 ||   // COPJMP1/2
		    replay_addr == 8'h1C || replay_addr == 8'h1E)     // STREQU/STRHOR
			saw_strobe <= 1'b1;
		if (replay_addr < 8'h10) saw_readwin <= 1'b1;
	end
end

// First recorded index for a register address, or -1.
function integer idx_of(input [8:0] addr);
	integer k;
begin
	idx_of = -1;
	for (k = rec_n - 1; k >= 0; k = k - 1)
		if (rec_addr[k] == addr[8:1]) idx_of = k;
end
endfunction

function [15:0] nth_val(input integer k);
begin
	nth_val = (k >= 0 && k < rec_n) ? rec_data[k] : 16'hDEAD;
end
endfunction

// Value written for a plain register.
function [15:0] val_of(input [8:0] addr);
begin
	val_of = nth_val(idx_of(addr));
end
endfunction

task check(input [511:0] name, input [31:0] actual, input [31:0] want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %08h, want %08h", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

// One chipset register write, exactly as the machine performs one: the address
// on the bus and the value on custom_data_in, taken on a clk7_en tick.
task wr(input [8:0] addr, input [15:0] val);
begin
	@(negedge clk);
	reg_address_in = addr[8:1];
	data_in        = val;
	clk7_en        = 1'b1;
	@(negedge clk);
	clk7_en        = 1'b0;
end
endtask

// The array read is REGISTERED -- it was the critical path of the whole design
// as a 256-deep async mux -- so a read costs a clock: drive the address, let
// mem_q follow it, then sample. The set/clear registers are combinational and
// would not need this, but reading them the same way keeps one task.
task rd(input [8:0] addr);
begin
	rd_addr = addr[8:1];
	@(negedge clk);
	#1;
end
endtask

initial begin
	@(negedge clk); rst_n = 1;

	// A plain write-only register. COLOR00 is the canonical one: Denise has no
	// read path for it at all, which is the whole reason this module exists.
	wr(9'h180, 16'h0123);
	rd(9'h180);
	check("COLOR00 recorded",        {16'd0, rd_data},      32'h00000123);
	check("COLOR00 is replayable",   {31'd0, rd_writable},  32'd1);

	// Overwritten by a later write, like a copper rewriting it mid-frame.
	wr(9'h180, 16'h0456);
	rd(9'h180);
	check("COLOR00 takes the last write", {16'd0, rd_data}, 32'h00000456);

	// No clk7_en means no write. The chipset decodes on that tick, so a shadow
	// that ignored it would record bus values the machine never committed --
	// every intermediate address the bus passes through between real cycles.
	@(negedge clk);
	reg_address_in = 9'h182 >> 1;
	data_in        = 16'hDEAD;
	@(negedge clk); @(negedge clk);
	rd(9'h182);
	check("no write without clk7_en", {16'd0, rd_data},     32'h00000000);

	// Set/clear registers. Bit 15 is the direction and is never stored, so the
	// entry must accumulate the way the hardware does (paula.v:167) rather
	// than record the last value written. A shadow that stored writes verbatim
	// would report 0x8200 here instead of 0x0200.
	wr(9'h096, 16'h8200);   // DMACON: set bit 9
	rd(9'h096); check("DMACON accumulates a set",   {16'd0, rd_data}, 32'h00000200);
	wr(9'h096, 16'h8060);   // set bits 5 and 6, keeping bit 9
	rd(9'h096); check("DMACON keeps earlier bits",  {16'd0, rd_data}, 32'h00000260);
	wr(9'h096, 16'h0200);   // clear bit 9 only
	rd(9'h096); check("DMACON clears without bit15",{16'd0, rd_data}, 32'h00000060);
	check("DMACON needs set/clear replay", {31'd0, rd_setclear}, 32'd1);
	check("DMACON is replayable",          {31'd0, rd_writable}, 32'd1);

	// Directed: accumulate, then read, repeatedly, on every set/clear register.
	//
	// The three checks above pass if the FIRST write lands and the register
	// never moves again -- a restructure that broke the read-modify-write left
	// DMACON reading 0x0200 for all three and only the third check's value made
	// that obvious. This walks each register through four writes, reading
	// between every one, so a register that stops accumulating is caught at the
	// step it stops on rather than at the end.
	wr(9'h096, 16'h0FFF);                                    // clear everything
	rd(9'h096); check("acc: DMACON starts clear",   {16'd0, rd_data}, 32'h00000000);
	wr(9'h096, 16'h8001);
	rd(9'h096); check("acc: DMACON after set 1",    {16'd0, rd_data}, 32'h00000001);
	wr(9'h096, 16'h8002);
	rd(9'h096); check("acc: DMACON after set 2",    {16'd0, rd_data}, 32'h00000003);
	wr(9'h096, 16'h8004);
	rd(9'h096); check("acc: DMACON after set 3",    {16'd0, rd_data}, 32'h00000007);
	wr(9'h096, 16'h0002);
	rd(9'h096); check("acc: DMACON after clear 2",  {16'd0, rd_data}, 32'h00000005);

	wr(9'h09E, 16'h0FFF);
	wr(9'h09E, 16'h8010);
	rd(9'h09E); check("acc: ADKCON after set 1",    {16'd0, rd_data}, 32'h00000010);
	wr(9'h09E, 16'h8020);
	rd(9'h09E); check("acc: ADKCON after set 2",    {16'd0, rd_data}, 32'h00000030);

	// A write to one set/clear register must not disturb another. Three named
	// registers behind one interface can alias if the write decode is wrong.
	wr(9'h09A, 16'h0FFF);
	wr(9'h09A, 16'h8100);
	rd(9'h09A); check("acc: INTENA took its write", {16'd0, rd_data}, 32'h00000100);
	rd(9'h096); check("acc: DMACON undisturbed",    {16'd0, rd_data}, 32'h00000005);
	rd(9'h09E); check("acc: ADKCON undisturbed",    {16'd0, rd_data}, 32'h00000030);

	// A plain register interleaved with them must also be unaffected, and must
	// still take values verbatim rather than accumulating.
	wr(9'h180, 16'h0111);
	wr(9'h180, 16'h0222);
	rd(9'h180); check("acc: plain reg replaced",    {16'd0, rd_data}, 32'h00000222);
	rd(9'h096); check("acc: DMACON still intact",   {16'd0, rd_data}, 32'h00000005);

	// Leave the values the later checks expect.
	wr(9'h096, 16'h0FFF); wr(9'h096, 16'h8060);
	wr(9'h09E, 16'h0FFF); wr(9'h09E, 16'h8800);
	wr(9'h09A, 16'h0FFF);
	wr(9'h180, 16'h0456);

	wr(9'h09A, 16'hC000);   // INTENA: set INTEN
	rd(9'h09A); check("INTENA accumulates",        {16'd0, rd_data}, 32'h00004000);
	wr(9'h09E, 16'h8800);   // ADKCON
	rd(9'h09E); check("ADKCON accumulates",        {16'd0, rd_data}, 32'h00000800);

	// INTREQ is the exception among the four: Paula raises its bits in
	// hardware, so accumulating writes would drift from the real register
	// within a frame. It comes from paula_intcontroller's own intreq instead.
	wr(9'h09C, 16'h8020);
	rd(9'h09C); check("INTREQ not shadowed",   {16'd0, rd_data},     32'd0);
	            check("INTREQ not replayable", {31'd0, rd_writable}, 32'd0);

	// A plain register must NOT take the set/clear path, or every colour write
	// would OR into the previous colour instead of replacing it.
	rd(9'h180); check("COLOR00 is a plain write", {31'd0, rd_setclear}, 32'd0);

	// Strobes. Writing one is an event. A replayed COPJMP1 restarts the copper
	// at COP1LC in the middle of a restore, which is the kind of bug that
	// surfaces as "the game glitches sometimes after loading".
	wr(9'h088, 16'h0000);   // COPJMP1 -- agnus_copper.v:84
	wr(9'h08A, 16'h0000);   // COPJMP2 -- agnus_copper.v:85
	wr(9'h08C, 16'h0000);   // COPINS  -- agnus_copper.v:83
	wr(9'h038, 16'h0000);   // STREQU
	wr(9'h03C, 16'h0000);   // STRHOR
	// The write-to-act registers that are not called strobes and behave like
	// them anyway. BLTSIZE is the one that mattered: agnus_blitter.v:501 raises
	// busy on any write, every blit ends by writing it, so the entry is always
	// populated -- and a replay therefore STARTED A BLIT during the restore,
	// with restored pointers, into freshly restored chip RAM. Found on
	// hardware as a restore that reported success and then reset the machine.
	wr(9'h058, 16'h0140);   // BLTSIZE
	wr(9'h05C, 16'h0040);   // BLTSIZV (ECS)
	wr(9'h05E, 16'h0040);   // BLTSIZH (ECS)
	wr(9'h030, 16'h00FF);   // SERDAT  -- starts a transmission
	wr(9'h026, 16'hFFFF);   // DSKDAT  -- writes into the disk buffer
	wr(9'h0AA, 16'h1234);   // AUD0DAT -- a sample event for the channel
	rd(9'h058); check("BLTSIZE not shadowed",  {31'd0, rd_writable}, 32'd0);
	rd(9'h05C); check("BLTSIZV not shadowed",  {31'd0, rd_writable}, 32'd0);
	rd(9'h05E); check("BLTSIZH not shadowed",  {31'd0, rd_writable}, 32'd0);
	rd(9'h030); check("SERDAT not shadowed",   {31'd0, rd_writable}, 32'd0);
	rd(9'h026); check("DSKDAT not shadowed",   {31'd0, rd_writable}, 32'd0);
	rd(9'h0AA); check("AUD0DAT not shadowed",  {31'd0, rd_writable}, 32'd0);

	// Addressed by number as well as by name: these three were named one
	// register out for a while, and the test agreed with the code.
	rd(9'h088); check("COPJMP1 ($088) not replayable", {31'd0, rd_writable}, 32'd0);
	rd(9'h08A); check("COPJMP2 ($08A) not replayable", {31'd0, rd_writable}, 32'd0);
	rd(9'h08C); check("COPINS ($08C) not replayable",  {31'd0, rd_writable}, 32'd0);
	rd(9'h08C); check("COPJMP2 not replayable", {31'd0, rd_writable}, 32'd0);
	rd(9'h038); check("STREQU not replayable",  {31'd0, rd_writable}, 32'd0);
	rd(9'h03C); check("STRHOR not replayable",  {31'd0, rd_writable}, 32'd0);

	// The read window below $020 is not written by anything, and replaying
	// into it would put a READ address on the bus for a cycle.
	wr(9'h002, 16'hFFFF);   // DMACONR
	rd(9'h002); check("read window not shadowed",   {16'd0, rd_data},     32'd0);
	            check("read window not replayable", {31'd0, rd_writable}, 32'd0);

	// The registers that carry a running game's look and layout, spot-checked
	// across the address space so an exclusion that was too broad shows up.
	wr(9'h100, 16'h4200);   // BPLCON0
	wr(9'h108, 16'h0004);   // BPL1MOD
	wr(9'h0E0, 16'h0007);   // BPL1PTH
	wr(9'h120, 16'h1234);   // SPR0PTH
	wr(9'h0A6, 16'h00FA);   // AUD0PER
	wr(9'h19E, 16'h0FFF);   // COLOR31
	rd(9'h100); check("BPLCON0 recorded", {16'd0, rd_data}, 32'h00004200);
	rd(9'h108); check("BPL1MOD recorded", {16'd0, rd_data}, 32'h00000004);
	rd(9'h0E0); check("BPL1PTH recorded", {16'd0, rd_data}, 32'h00000007);
	rd(9'h120); check("SPR0PTH recorded", {16'd0, rd_data}, 32'h00001234);
	rd(9'h0A6); check("AUD0PER recorded", {16'd0, rd_data}, 32'h000000FA);
	rd(9'h19E); check("COLOR31 recorded", {16'd0, rd_data}, 32'h00000FFF);

	// The load path a restore uses to put a saved section back. It must beat the
	// snoop -- the machine is frozen while it runs -- and it must reach entries
	// the snoop refuses, because the payload is a fixed 256-entry section and
	// the restore streams all of it.
	@(negedge clk);
	ld_addr = 8'hC0;              // COLOR00 >> 1
	ld_data = 16'h0777;
	ld_we   = 1'b1;
	@(negedge clk);
	ld_we   = 1'b0;
	rd(9'h180);
	check("load overwrites the shadow", {16'd0, rd_data}, 32'h00000777);

	// ------------------------------------------------------------- replay
	//
	// Ordering is the point. Replaying in address order would bring DMA and
	// interrupts up partway through, against a chipset that is half old and
	// half new, and that misbehaves later rather than failing here -- which is
	// why the monitor records every write and the checks below interrogate the
	// sequence rather than the final state.
	@(negedge clk);
	rec_n  = 0;
	recording = 1;
	intreq_in = 15'h0011;
	replay_start = 1;
	@(negedge clk);
	replay_start = 0;

	// clk7_en is driven from here rather than free-running, so the replay has
	// to be clocked by hand. Bounded rather than a bare wait: a sequencer that
	// never terminates is a plausible bug and should fail the run, not hang it.
	for (i = 0; i < 4000 && !replay_done; i = i + 1) begin
		@(negedge clk); clk7_en = 1'b1;
		@(negedge clk); clk7_en = 1'b0;
	end
	check("replay finished", {31'd0, replay_done}, 32'd1);
	@(negedge clk);
	recording = 0;

	// Nothing excluded may ever be driven onto the bus.
	check("replay drove no strobe",      {31'd0, saw_strobe},   32'd0);
	check("replay drove no read window", {31'd0, saw_readwin},  32'd0);

	// Plain registers come back with their values.
	check("replay wrote the loaded COLOR00", {16'd0, val_of(9'h180)}, 32'h00000777);
	check("replay wrote BPLCON0", {16'd0, val_of(9'h100)}, 32'h00004200);
	check("replay wrote COLOR31", {16'd0, val_of(9'h19E)}, 32'h00000FFF);

	// The set/clear four are each a clearing write then a setting write.
	check("DMACON cleared first",  {16'd0, nth_val(idx_of(9'h096))},     32'h00007FFF);
	check("DMACON set after",      {16'd0, nth_val(idx_of(9'h096) + 1)}, 32'h00008060);
	check("INTENA set after clear",{16'd0, nth_val(idx_of(9'h09A) + 1)}, 32'h0000C000);
	check("ADKCON set after clear",{16'd0, nth_val(idx_of(9'h09E) + 1)}, 32'h00008800);

	// INTREQ is replayed from the exported value, not from a shadow entry.
	check("INTREQ from the export", {16'd0, nth_val(idx_of(9'h09C) + 1)}, 32'h00008011);

	// And the order that matters: DMA and interrupts last, DMACON after INTENA,
	// every plain register before any of them.
	check("ADKCON before INTREQ", {31'd0, (idx_of(9'h09E) < idx_of(9'h09C))}, 32'd1);
	check("INTREQ before INTENA", {31'd0, (idx_of(9'h09C) < idx_of(9'h09A))}, 32'd1);
	check("INTENA before DMACON", {31'd0, (idx_of(9'h09A) < idx_of(9'h096))}, 32'd1);
	check("colours before DMACON",{31'd0, (idx_of(9'h180) < idx_of(9'h096))}, 32'd1);
	check("DMACON set is the last write",
	      {31'd0, ((idx_of(9'h096) + 1) == (rec_n - 1))}, 32'd1);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
