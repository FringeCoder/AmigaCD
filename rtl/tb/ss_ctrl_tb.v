`timescale 1ns/1ns

// Drives a save through ss_ctrl against a mock DDR3 and a mock SDRAM, then
// checks the header words and payload the host parser will validate.
//
// core_crc32 is checked against an independently computed reference, not
// just structurally assumed correct. ss_crc32 itself is already proven
// correct against known vectors (ss_crc32_tb.v); what this bench needs to
// catch is ss_ctrl feeding it the wrong bytes, the wrong order, or gaps that
// corrupt the running value -- exactly the kind of bug a final-result-only
// check can miss if it happens to be self-consistent (see this module's
// header for the two real bugs of that shape found while building it). This
// also exercises the wr-deasserted-without-init concern directly: ss_ctrl
// only calls crc init once per save, and every word after the first state
// word gets fed to the CRC with wr low (and no init) for the cycles it
// spends waiting on the DDR3 write or the next chip DMA pair. A wrong CRC
// here would mean either that gap handling is broken, or that a word was
// duplicated/dropped/reordered.

module ss_ctrl_tb;

localparam STATE_W    = 64;
localparam CHIP_WORDS = 8;
localparam STATE_WORDS = (STATE_W + 31) / 32;
localparam CORE_WORDS  = STATE_WORDS + CHIP_WORDS / 2;

reg  clk = 0;
reg  rst_n = 0;

reg  save_req = 0;
reg  [STATE_W-1:0] state_in = 64'h0123456789ABCDEF;
wire [STATE_W-1:0] state_out;
wire state_we;

reg  blit_busy = 0;
reg  cpu_boundary = 1;
reg  frame_tick = 0;
wire freeze;
wire save_busy, save_ok, save_fail;

wire [28:0] ddr_address;
wire [63:0] ddr_writedata;
wire [7:0]  ddr_byteenable;
wire        ddr_write;
wire        ddr_read;
reg  [63:0] ddr_readdata = 0;
reg         ddr_readdatavalid = 0;
reg         ddr_waitrequest = 0;

wire [24:1] sd_addr;
wire        sd_cs;
wire [1:0]  sd_state;
wire        sd_uds_n, sd_lds_n, sd_cache_inhibit;
reg  [15:0] sd_rd = 0;
reg         sd_ready = 0;

integer errors = 0;
integer latency = 0;
integer i;

// Mock DDR3: a flat memory of 64-bit words, addressed by ddr_address.
reg [63:0] ddr_mem [0:63];

ss_ctrl #(.STATE_W(STATE_W), .CHIP_WORDS(CHIP_WORDS)) dut
(
	.clk(clk), .rst_n(rst_n),
	.slot_base(29'd0),
	.save_req(save_req), .save_busy(save_busy),
	.save_ok(save_ok), .save_fail(save_fail),
	.state_in(state_in), .state_out(state_out), .state_we(state_we),
	.blit_busy(blit_busy), .disk_busy(1'b0), .audio_busy(1'b0),
	.cpu_boundary(cpu_boundary), .frame_tick(frame_tick),
	.freeze(freeze),
	.chip_base(24'h000000),
	.sd_addr(sd_addr), .sd_cs(sd_cs), .sd_state(sd_state),
	.sd_uds_n(sd_uds_n), .sd_lds_n(sd_lds_n),
	.sd_cache_inhibit(sd_cache_inhibit),
	.sd_rd(sd_rd), .sd_ready(sd_ready),
	.ddr_address(ddr_address), .ddr_writedata(ddr_writedata),
	.ddr_byteenable(ddr_byteenable), .ddr_write(ddr_write),
	.ddr_read(ddr_read), .ddr_readdata(ddr_readdata),
	.ddr_readdatavalid(ddr_readdatavalid), .ddr_waitrequest(ddr_waitrequest)
);

always #5 clk = ~clk;

// Mock SDRAM: contents are 0xD000 + address. Latency is two extra wait
// cycles beyond the first opportunity, comparable to a real cache fill, and
// slow enough relative to the CRC feeder's four-cycles-per-word rate that a
// design which merely happened to survive a zero-latency mock would not be
// exercised any harder here -- this bench is about correctness of the
// sequencing, not about hunting the tightest possible timing corner
// (ss_dma_tb.v already covers ss_dma's own protocol exhaustively).
always @(posedge clk) begin
	sd_ready <= 1'b0;
	if (sd_cs && !sd_ready) begin
		latency = latency + 1;
		if (latency >= 2) begin
			sd_rd    <= 16'hD000 + {8'd0, sd_addr[8:1]};
			sd_ready <= 1'b1;
			latency  = 0;
		end
	end
end

// Mock DDR3 write capture. Honours ddr_byteenable with a per-byte merge
// rather than overwriting the full 64-bit word: two 32-bit payload words
// share one 64-bit slot here (word_idx bit 0 selects the half), and a mock
// that ignored byteenable could not catch a controller that clobbers one
// half writing the other -- which is exactly the bug this bench exists to
// catch (see ss_ctrl.v's header for how "state word 0" read back as zero
// until ddr_byteenable was wired up for real).
always @(posedge clk) begin
	if (ddr_write && !ddr_waitrequest) begin
		if (ddr_byteenable[0]) ddr_mem[ddr_address[5:0]][7:0]   <= ddr_writedata[7:0];
		if (ddr_byteenable[1]) ddr_mem[ddr_address[5:0]][15:8]  <= ddr_writedata[15:8];
		if (ddr_byteenable[2]) ddr_mem[ddr_address[5:0]][23:16] <= ddr_writedata[23:16];
		if (ddr_byteenable[3]) ddr_mem[ddr_address[5:0]][31:24] <= ddr_writedata[31:24];
		if (ddr_byteenable[4]) ddr_mem[ddr_address[5:0]][39:32] <= ddr_writedata[39:32];
		if (ddr_byteenable[5]) ddr_mem[ddr_address[5:0]][47:40] <= ddr_writedata[47:40];
		if (ddr_byteenable[6]) ddr_mem[ddr_address[5:0]][55:48] <= ddr_writedata[55:48];
		if (ddr_byteenable[7]) ddr_mem[ddr_address[5:0]][63:56] <= ddr_writedata[63:56];
	end
end

// Read a 32-bit word out of the mock DDR3 by word index.
function [15:0] bswap16;
	input [15:0] v;
	bswap16 = {v[7:0], v[15:8]};
endfunction

function [31:0] ddr_word;
	input integer idx;
begin
	ddr_word = idx[0] ? ddr_mem[idx/2][63:32] : ddr_mem[idx/2][31:0];
end
endfunction

task check(input [255:0] name, input [31:0] actual, input [31:0] want);
begin
	if (actual !== want) begin
		$display("FAIL %0s: got %08h, want %08h", name, actual, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

// Independent CRC-32 reference (same algorithm as ss_crc32.v, which is
// itself proven against known vectors in ss_crc32_tb.v). Computed here over
// the exact byte sequence the payload is expected to contain, so this
// checks that ss_ctrl presents the right bytes in the right order, not
// merely that some CRC came out.
reg [31:0] crc_acc;

function [31:0] crc_byte_step;
	input [31:0] c;
	input [7:0]  b;
	integer k;
	reg [31:0] cur;
begin
	cur = c ^ {24'd0, b};
	for (k = 0; k < 8; k = k + 1)
		cur = cur[0] ? ((cur >> 1) ^ 32'hEDB88320) : (cur >> 1);
	crc_byte_step = cur;
end
endfunction

task crc_word_step(input [31:0] w);
	integer k;
begin
	for (k = 0; k < 4; k = k + 1)
		crc_acc = crc_byte_step(crc_acc, w[k*8 +: 8]);
end
endtask

reg [31:0] expect_crc;

initial begin
	for (i = 0; i < 64; i = i + 1) ddr_mem[i] = 64'd0;
	@(negedge clk); rst_n = 1;

	// Blitter busy at first, so the save has to wait for a quiet instant.
	blit_busy = 1;
	@(negedge clk); save_req = 1;
	@(negedge clk); @(negedge clk);
	if (freeze !== 1'b0) begin
		$display("FAIL froze while blitter busy");
		errors = errors + 1;
	end
	else $display("PASS waits for blitter");

	blit_busy = 0;
	wait (save_ok);
	@(negedge clk);

	check("magic",      ddr_word(2), 32'h53534341);
	check("version",    ddr_word(3), 32'h00010000);
	check("core_words", ddr_word(4), CORE_WORDS);
	check("host_words", ddr_word(5), 32'd0);
	check("host_crc32", ddr_word(7), 32'd0);
	check("length",     ddr_word(1), 6 + CORE_WORDS);

	// State vector words, least significant first.
	check("state word 0", ddr_word(8), 32'h89ABCDEF);
	check("state word 1", ddr_word(9), 32'h01234567);

	// Chip RAM packed two 16-bit words per 32-bit word, low word first, in
	// ascending address order.
	// Chip RAM must land in the file in 68k byte order, so a state file can be
	// read directly as Amiga memory. Each SDRAM word's high byte belongs at the
	// lower file offset; without the swap every Amiga word reads reversed and
	// nothing in the payload is interpretable (measured on hardware,
	// 2026-08-10, on the first real save state).
	check("chip word 0", ddr_word(10), {bswap16(16'hD001), bswap16(16'hD000)});
	check("chip word 1", ddr_word(11), {bswap16(16'hD003), bswap16(16'hD002)});
	check("chip word 2", ddr_word(12), {bswap16(16'hD005), bswap16(16'hD004)});
	check("chip word 3", ddr_word(13), {bswap16(16'hD007), bswap16(16'hD006)});

	// core_crc32 against an independently computed reference over the exact
	// bytes above, least-significant byte of each word first. A design that
	// dropped, duplicated, reordered a word, or re-armed the CRC mid-stream
	// would produce a different value here even if every word above landed
	// in the right DDR3 slot.
	crc_acc = 32'hFFFFFFFF;
	crc_word_step(32'h89ABCDEF);
	crc_word_step(32'h01234567);
	crc_word_step({bswap16(16'hD001), bswap16(16'hD000)});
	crc_word_step({bswap16(16'hD003), bswap16(16'hD002)});
	crc_word_step({bswap16(16'hD005), bswap16(16'hD004)});
	crc_word_step({bswap16(16'hD007), bswap16(16'hD006)});
	expect_crc = ~crc_acc;
	check("core_crc32", ddr_word(6), expect_crc);

	check("counter bumped", ddr_word(0), 32'd1);

	save_req = 0;
	@(negedge clk); @(negedge clk);
	if (freeze !== 1'b0) begin
		$display("FAIL freeze not released");
		errors = errors + 1;
	end
	else $display("PASS freeze released");

	// A machine that never quiesces must fail rather than save.
	blit_busy = 1;
	@(negedge clk); save_req = 1;
	@(negedge clk);
	for (i = 0; i < 3; i = i + 1) begin
		@(negedge clk); frame_tick = 1;
		@(negedge clk); frame_tick = 0;
	end
	wait (save_fail);
	$display("PASS timeout reports failure");

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
