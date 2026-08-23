`timescale 1ns/1ns

module ss_crc32_tb;

reg         clk = 0;
reg         init = 0;
reg         wr = 0;
reg  [7:0]  byte_in = 0;
wire [31:0] crc_out;

integer errors = 0;
integer i;

ss_crc32 dut (.clk(clk), .init(init), .wr(wr), .byte_in(byte_in), .crc_out(crc_out));

always #5 clk = ~clk;

// Feed an ASCII string, one byte per clock.
task feed_string(input [71:0] s, input integer len);
	integer k;
begin
	@(negedge clk);
	init = 1;
	@(negedge clk);
	init = 0;
	for (k = len - 1; k >= 0; k = k - 1) begin
		byte_in = s[k*8 +: 8];
		wr = 1;
		@(negedge clk);
	end
	wr = 0;
	@(negedge clk);
end
endtask

task check(input [255:0] name, input [31:0] want);
begin
	if (crc_out !== want) begin
		$display("FAIL %0s: crc=%08h, want %08h", name, crc_out, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	// The canonical CRC-32 check value.
	feed_string("123456789", 9);
	check("check vector 123456789", 32'hCBF43926);

	// A single byte, verified against the same reflected 0xEDB88320 CRC the
	// host uses in minimig_a2065_crc32.cpp.
	feed_string("a", 1);
	check("single byte a", 32'hE8B7BE43);

	// Empty input: init then no writes, final xor of 0xFFFFFFFF gives zero.
	@(negedge clk); init = 1; @(negedge clk); init = 0; @(negedge clk);
	check("empty input", 32'h00000000);

	// init must be able to restart cleanly after a previous run.
	feed_string("123456789", 9);
	check("restart after reuse", 32'hCBF43926);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
