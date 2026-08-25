`timescale 1ns/1ns

module snac_cd32_tb;

reg  [15:0] psx;
wire [10:0] joy;
integer errors = 0;

snac_cd32 dut (.psx(psx), .joy(joy));

task check(input [255:0] name, input [15:0] in, input [10:0] want);
begin
	psx = in;
	#1;
	if (joy !== want) begin
		$display("FAIL %0s: psx=%04h -> joy=%03h, want %03h", name, in, joy, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

initial begin
	check("nothing",   16'h0000, 11'h000);
	check("right",     16'h0001, 11'h001);
	check("left",      16'h0002, 11'h002);
	check("down",      16'h0004, 11'h004);
	check("up",        16'h0008, 11'h008);
	check("cross->red",     16'h0010, 11'h010);
	check("circle->blue",   16'h0020, 11'h020);
	check("square->yellow", 16'h0040, 11'h040);
	check("triangle->green",16'h0080, 11'h080);
	check("L1->reverse",    16'h0100, 11'h200);
	check("R1->forward",    16'h0200, 11'h100);
	check("start->play",    16'h0400, 11'h400);

	// Buttons with no CD32 equivalent must not leak into any joystick bit.
	check("select ignored", 16'h0800, 11'h000);
	check("L2 ignored",     16'h1000, 11'h000);
	check("R2 ignored",     16'h2000, 11'h000);
	check("L3 ignored",     16'h4000, 11'h000);
	check("R3 ignored",     16'h8000, 11'h000);

	// Combinations must not interfere.
	check("all mapped", 16'h07FF, 11'h7FF);
	check("all buttons", 16'hFFFF, 11'h7FF);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
