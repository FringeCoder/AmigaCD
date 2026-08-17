`timescale 1ns/1ns

// ss_state_fanout: the restore's register-write sequencer.
//
// This is the half of a restore that puts the programmer's model back. If it
// writes A3 where D3 belongs, or drops the re-seed, or lets the CPU go before
// the last register lands, the machine comes back looking restored and behaving
// like something else -- and the payload it came from is provably correct, so
// every other bench in the suite still passes.
//
// The vector is built here from a per-field pattern, one distinguishable value
// per field, so a swap between any two fields shows up as a value in the wrong
// place rather than as a plausible number.

module ss_state_fanout_tb;

integer errors = 0;

task check(input [511:0] name, input [31:0] actual, input [31:0] want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %08h, want %08h", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

reg clk   = 1'b0;
reg rst_n = 1'b0;
always #5 clk = ~clk;

localparam integer STATE_W = 632;

// The vector, in SS_STATE_LIST order. The list is a concatenation, so the
// FIRST name in it occupies the HIGH bits -- which is why this is assembled by
// concatenation here too rather than by bit arithmetic that could disagree
// with it.
localparam [31:0] D0 = 32'hD000_0000, D1 = 32'hD100_0001;
localparam [31:0] D2 = 32'hD200_0002, D3 = 32'hD300_0003;
localparam [31:0] D4 = 32'hD400_0004, D5 = 32'hD500_0005;
localparam [31:0] D6 = 32'hD600_0006, D7 = 32'hD700_0007;
localparam [31:0] A0 = 32'hA000_0008, A1 = 32'hA100_0009;
localparam [31:0] A2 = 32'hA200_000A, A3 = 32'hA300_000B;
localparam [31:0] A4 = 32'hA400_000C, A5 = 32'hA500_000D;
localparam [31:0] A6 = 32'hA600_000E, A7 = 32'hA700_000F;
localparam [31:0] PC   = 32'h00FC_00D2;
localparam [31:0] USP  = 32'h0007_FFF0;
localparam [31:0] VBR  = 32'h0000_0400;
localparam [15:0] SR   = 16'h2700;
localparam  [3:0] CACR = 4'b1010;
localparam        OVL = 1'b1, ROM_RO = 1'b0, K1MB = 1'b1, K256 = 1'b1;

wire [STATE_W-1:0] state = {
	D0, D1, D2, D3, D4, D5, D6, D7,
	A0, A1, A2, A3, A4, A5, A6, A7,
	PC, USP, VBR, SR, CACR,
	OVL, ROM_RO, K1MB, K256 };

// The same sixteen values in register-index order, which is the order the
// sequencer must write them in: 0-7 = D0-D7, 8-15 = A0-A7. Written out
// separately from the vector above on purpose -- an off-by-eight is exactly
// the mistake this is here to catch, and it cannot be caught by a table
// derived from the thing under test.
reg [31:0] want_reg [0:15];
initial begin
	want_reg[0]  = D0; want_reg[1]  = D1; want_reg[2]  = D2; want_reg[3]  = D3;
	want_reg[4]  = D4; want_reg[5]  = D5; want_reg[6]  = D6; want_reg[7]  = D7;
	want_reg[8]  = A0; want_reg[9]  = A1; want_reg[10] = A2; want_reg[11] = A3;
	want_reg[12] = A4; want_reg[13] = A5; want_reg[14] = A6; want_reg[15] = A7;
end

reg req = 1'b0;

wire        ack;
wire  [3:0] cpu_wr_index;
wire [31:0] cpu_wr_data;
wire        cpu_wr_en;
wire        cpu_pc_wr, cpu_sr_wr, cpu_usp_wr, cpu_vbr_wr, cpu_cacr_wr;
wire        cpu_resume;
wire  [3:0] map_in;
wire        map_we;
wire        busy;

ss_state_fanout #(.STATE_W(STATE_W)) dut
(
	.clk          (clk         ),
	.rst_n        (rst_n       ),
	.req          (req         ),
	.ack          (ack         ),
	.state        (state       ),
	.cpu_wr_index (cpu_wr_index),
	.cpu_wr_data  (cpu_wr_data ),
	.cpu_wr_en    (cpu_wr_en   ),
	.cpu_pc_wr    (cpu_pc_wr   ),
	.cpu_sr_wr    (cpu_sr_wr   ),
	.cpu_usp_wr   (cpu_usp_wr  ),
	.cpu_vbr_wr   (cpu_vbr_wr  ),
	.cpu_cacr_wr  (cpu_cacr_wr ),
	.cpu_resume   (cpu_resume  ),
	.map_in       (map_in      ),
	.map_we       (map_we      ),
	.busy         (busy        )
);

// ------------------------------------------------------------------ observers
//
// Recorded by the write port, not by the step counter: what the CPU sees is
// the enable and the bus in the same cycle, and that pairing is the thing that
// has to be right.

reg [31:0] got_reg [0:15];
reg [15:0] got_reg_seen;
integer    reg_writes  = 0;
integer    pc_writes   = 0;
integer    sr_writes   = 0;
integer    usp_writes  = 0;
integer    vbr_writes  = 0;
integer    cacr_writes = 0;
integer    map_writes  = 0;
integer    resumes     = 0;
reg [31:0] got_pc, got_sr, got_usp, got_vbr, got_cacr;
reg  [3:0] got_map;

// Order, as an ordinal per event, so "the re-seed came last" and "the map went
// in before the CPU was let go" are checkable rather than inferred.
integer    ev = 0;
integer    ev_last_reg = 0, ev_pc = 0, ev_map = 0, ev_resume = 0;

// Did the CPU stay parked for the whole sequence? busy must still be high on
// the cycle the re-seed is issued: cpu_wrapper holds ss_arm on it.
reg busy_at_resume = 1'b0;

integer k;
initial begin
	got_reg_seen = 16'd0;
	for (k = 0; k < 16; k = k + 1) got_reg[k] = 32'hDEADBEEF;
end

always @(posedge clk) if (rst_n) begin
	if (cpu_wr_en) begin
		ev = ev + 1;
		ev_last_reg = ev;
		got_reg[cpu_wr_index]       = cpu_wr_data;
		got_reg_seen[cpu_wr_index]  = 1'b1;
		reg_writes = reg_writes + 1;
	end
	if (cpu_pc_wr)   begin ev = ev + 1; ev_pc = ev; got_pc  = cpu_wr_data; pc_writes  = pc_writes + 1;  end
	if (cpu_sr_wr)   begin ev = ev + 1; got_sr  = cpu_wr_data; sr_writes  = sr_writes + 1;  end
	if (cpu_usp_wr)  begin ev = ev + 1; got_usp = cpu_wr_data; usp_writes = usp_writes + 1; end
	if (cpu_vbr_wr)  begin ev = ev + 1; got_vbr = cpu_wr_data; vbr_writes = vbr_writes + 1; end
	if (cpu_cacr_wr) begin ev = ev + 1; got_cacr= cpu_wr_data; cacr_writes= cacr_writes+ 1; end
	if (map_we)      begin ev = ev + 1; ev_map = ev; got_map = map_in; map_writes = map_writes + 1; end
	if (cpu_resume)  begin
		ev = ev + 1; ev_resume = ev; resumes = resumes + 1;
		busy_at_resume = busy;
	end
end

// No two enables may be high together: cpu_wr_data is one shared bus, so a
// second enable in the same cycle writes the wrong register with whatever the
// first one put there.
integer overlap = 0;
always @(posedge clk) if (rst_n) begin
	if (({3'd0, cpu_wr_en} + {3'd0, cpu_pc_wr} + {3'd0, cpu_sr_wr} +
	     {3'd0, cpu_usp_wr} + {3'd0, cpu_vbr_wr} + {3'd0, cpu_cacr_wr} +
	     {3'd0, cpu_resume}) > 1)
		overlap = overlap + 1;
end

initial begin
	#50000;
	$display("FAIL watchdog: simulation did not finish");
	$fatal(1);
end

integer i;

initial begin
	repeat (4) @(posedge clk);
	rst_n = 1'b1;
	repeat (2) @(posedge clk);

	check("idle: not busy", {31'd0, busy}, 32'd0);
	check("idle: no ack",   {31'd0, ack},  32'd0);

	// ss_ctrl holds req until ack, so this does too.
	@(negedge clk);
	req = 1'b1;
	wait (ack);
	@(posedge clk);
	@(negedge clk);
	req = 1'b0;
	repeat (4) @(posedge clk);

	// ------------------------------------------------------ what was written
	for (i = 0; i < 16; i = i + 1)
		check(i < 8 ? "data register restored" : "address register restored",
		      got_reg[i], want_reg[i]);

	check("all sixteen registers were written", {16'd0, got_reg_seen}, 32'h0000FFFF);
	check("exactly sixteen register writes",    reg_writes[31:0],     32'd16);

	check("PC restored",   got_pc,   PC);
	check("SR restored",   got_sr,   {16'd0, SR});
	check("USP restored",  got_usp,  USP);
	check("VBR restored",  got_vbr,  VBR);
	check("CACR restored", got_cacr, {28'd0, CACR});

	// Bit order is ss_map's, in both directions. [3] is ovl and [2] is gary's
	// rom_readonly; swapping them puts the wrong thing at address zero after a
	// restore and nothing says so until the machine runs.
	check("map bits in ss_map order", {28'd0, got_map}, {28'd0, {OVL, ROM_RO, K1MB, K256}});

	// -------------------------------------------------------------- how many
	check("PC written once",     pc_writes[31:0],   32'd1);
	check("SR written once",     sr_writes[31:0],   32'd1);
	check("USP written once",    usp_writes[31:0],  32'd1);
	check("VBR written once",    vbr_writes[31:0],  32'd1);
	check("CACR written once",   cacr_writes[31:0], 32'd1);
	check("map written once",    map_writes[31:0],  32'd1);
	check("resumed once",        resumes[31:0],     32'd1);
	check("no two enables at once", overlap[31:0],  32'd0);

	// --------------------------------------------------------------- ordering
	//
	// The re-seed restarts the CPU's fetch from whatever PC it holds, so it has
	// to come after the PC write and after every register write -- and while
	// the CPU is still parked, which is what busy says.
	check("re-seed after the register writes",
	      {31'd0, (ev_resume > ev_last_reg)}, 32'd1);
	check("re-seed after the PC write", {31'd0, (ev_resume > ev_pc)}, 32'd1);
	check("re-seed is the last event",  {31'd0, (ev_resume == ev)},   32'd1);
	check("map written before the re-seed", {31'd0, (ev_map < ev_resume)}, 32'd1);
	check("still parked when the re-seed goes out", {31'd0, busy_at_resume}, 32'd1);

	// ------------------------------------------------------------- handshake
	check("ack held until req drops", {31'd0, ack}, 32'd0);
	check("not busy once done",       {31'd0, busy}, 32'd0);

	// A second request must run the whole sequence again -- a restore can
	// follow a restore, and Task 8's second-restore case in ss_ctrl_tb depends
	// on this side behaving the same way twice.
	reg_writes = 0; resumes = 0; map_writes = 0;
	@(negedge clk);
	req = 1'b1;
	wait (ack);
	@(posedge clk);
	@(negedge clk);
	req = 1'b0;
	repeat (4) @(posedge clk);

	check("second run wrote sixteen registers", reg_writes[31:0], 32'd16);
	check("second run resumed once",            resumes[31:0],    32'd1);
	check("second run wrote the map once",      map_writes[31:0], 32'd1);

	// Reset in the middle of a run must drop everything, including busy: the
	// CPU is released by reset, and a sequencer still walking into a reset
	// machine would write registers behind it.
	@(negedge clk);
	req = 1'b1;
	repeat (6) @(posedge clk);
	check("busy mid-run", {31'd0, busy}, 32'd1);
	@(negedge clk);
	rst_n = 1'b0;
	@(posedge clk);
	@(negedge clk);
	check("reset drops busy",       {31'd0, busy},       32'd0);
	check("reset drops the enables",
	      {25'd0, cpu_wr_en, cpu_pc_wr, cpu_sr_wr, cpu_usp_wr, cpu_vbr_wr,
	       cpu_cacr_wr, map_we}, 32'd0);
	req = 1'b0;

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
