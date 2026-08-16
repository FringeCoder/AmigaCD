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

integer errors = 0;

ss_regshadow dut
(
	.clk(clk), .clk7_en(clk7_en), .rst_n(rst_n),
	.reg_address_in(reg_address_in), .data_in(data_in),
	.rd_addr(rd_addr), .rd_data(rd_data), .rd_writable(rd_writable),
	.rd_setclear(rd_setclear)
);

always #5 clk = ~clk;

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

task rd(input [8:0] addr);
begin
	rd_addr = addr[8:1];
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
	wr(9'h08A, 16'h0000);   // COPJMP1
	wr(9'h08C, 16'h0000);   // COPJMP2
	wr(9'h038, 16'h0000);   // STREQU
	wr(9'h03C, 16'h0000);   // STRHOR
	rd(9'h08A); check("COPJMP1 not replayable", {31'd0, rd_writable}, 32'd0);
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

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
