`timescale 1ns/1ns

// Overridable from the command line (iverilog -Psnac_psx_tb.CLK_KHZ=100000) so
// the same testbench covers every clk rate the module is instantiated at. The
// module derives all of its timing from CLK_KHZ, and a bug that only shows up
// at one rate - the inter-poll gap used to be a raw counter, so poll cadence
// halved between 50 and 100 MHz - is invisible to a testbench pinned to one.
module snac_psx_tb #(
	parameter integer CLK_KHZ = 50000
);

localparam integer CLK_HALF_NS = 500000 / CLK_KHZ;
localparam integer BAUD_KHZ    = 250;
localparam integer BUS_PERIOD_NS = 1000000 / BAUD_KHZ;

reg clk = 0;
reg reset = 1;
reg enable = 1;
wire [6:0] user_out;
reg  [6:0] user_in = 7'h7F;

wire [15:0] pad0, pad1;
wire [31:0] axes0, axes1;
wire  [7:0] id0, id1;

integer errors = 0;

always #CLK_HALF_NS clk = ~clk;

snac_psx #(.CLK_KHZ(CLK_KHZ), .BAUD_KHZ(BAUD_KHZ)) dut (
	.clk(clk), .reset(reset), .enable(enable),
	.user_in(user_in), .user_out(user_out),
	.pad0(pad0), .pad1(pad1), .axes0(axes0), .axes1(axes1),
	.id0(id0), .id1(id1)
);

task check(input [255:0] name, input integer got, input integer want);
begin
	if (got !== want) begin
		$display("FAIL %0s: got %0d (0x%0h), want %0d (0x%0h)", name, got, got, want, want);
		errors = errors + 1;
	end
	else $display("PASS %0s", name);
end
endtask

// Wait for n complete port-1 polls. Synchronising on the ATT line instead of a
// fixed delay keeps these tests correct when later tasks change poll cadence --
// a hardcoded settle time silently stops sampling instead of failing loudly.
// The trailing #1 lets the per-frame monitors below settle before anything is
// sampled: they trigger on the same posedge, and event order within one time
// step is not defined.
task wait_polls(input integer n);
	integer k;
	begin
		for (k = 0; k < n; k = k + 1) @(posedge att1);
		#1;
	end
endtask

// Port 1 pad: buttons drivable per-test (active low, so 0xFF = released), sticks centred.
wire att1 = user_out[1];
wire dat_p1, ack_p1;
reg [7:0] p1_btn0 = 8'hFF;
reg [7:0] p1_btn1 = 8'hFF;

psx_pad_model #(.DEVICE_ID(8'h41), .PAYLOAD_BYTES(2)) pad_p1 (
	.att(att1), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p1), .ack(ack_p1),
	.btn0(p1_btn0), .btn1(p1_btn1),
	.ax_rx(8'h80), .ax_ry(8'h80), .ax_lx(8'h80), .ax_ly(8'h80)
);

// Port 2 pad: a DualShock, fixed analog axes, buttons drivable per-test.
wire att2 = user_out[0];
wire dat_p2, ack_p2;
reg [7:0] p2_btn0 = 8'hFF;

psx_pad_model #(.DEVICE_ID(8'h73), .PAYLOAD_BYTES(6)) pad_p2 (
	.att(att2), .clk(user_out[5]), .cmd(user_out[2]),
	.dat(dat_p2), .ack(ack_p2),
	.btn0(p2_btn0), .btn1(8'hFF),
	.ax_rx(8'h11), .ax_ry(8'h22), .ax_lx(8'h33), .ax_ly(8'h44)
);

// Both pads sit on the same wires; only the selected one drives them. The
// model releases dat/ack high when its own ATT is high, so wired-AND is
// the correct combination.
//
// pads_present gates that wired-AND so a test can simulate both pads being
// physically unplugged: the bus idles high (as a floating/pulled-up bus
// does) regardless of what the still-running pad models are trying to
// drive. Set to 0 to unplug, back to 1 if a later assertion needs the pads.
reg pads_present = 1'b1;

// Test-only ACK glitch injection. Driven from the always @(*) block's own
// sensitivity list rather than via force/release on user_in[3] directly:
// release does not reliably re-trigger a reg driven by always @(*) in Icarus
// (the block only re-evaluates on an event in its sensitivity list), so a
// force/release pair left ACK stuck low well past the intended 20 ns and the
// stuck-low level was long enough for the glitch filter to legitimately
// accept it -- the first version of this test passed for the wrong reason
// (it never actually exercised a 20 ns glitch). Gating through this reg
// guarantees both edges of the pulse retrigger the block immediately.
reg ack_glitch_n = 1'b1;   // 0 = inject a glitch (active low, like ACK itself)

always @(*) begin
	user_in[4] = pads_present ? (dat_p1 & dat_p2) : 1'b1;
	user_in[3] = ack_glitch_n & (pads_present ? (ack_p1 & ack_p2) : 1'b1);
end

wire sclk = user_out[5];

// --- Bus monitors ---------------------------------------------------------
// Clock edges inside one ATT-low window, latched at the end of the frame. This
// is how "the master stopped when the pad stopped acking" is observable: a
// five-byte digital frame is 40 falling edges, a nine-byte DualShock frame is
// 72, and a port with nothing plugged in is 8 (one byte, then the ACK timeout).
// A master that free-runs a fixed nine bytes reads 72 for all three.
integer p1_edges = 0, p1_frame = 0;
integer p2_edges = 0, p2_frame = 0;

always @(negedge sclk) begin
	if (!att1) p1_edges = p1_edges + 1;
	if (!att2) p2_edges = p2_edges + 1;
end
always @(posedge att1) begin p1_frame = p1_edges; p1_edges = 0; end
always @(posedge att2) begin p2_frame = p2_edges; p2_edges = 0; end

// Both ports share CLK/CMD/DAT/ACK, so two ATT lines low at once would have
// both pads answering onto the same wires. Latched rather than checked inline
// so a single glitch anywhere in the run is still reported at the end.
reg both_att_low = 1'b0;
always @(att1 or att2) if (att1 === 1'b0 && att2 === 1'b0) both_att_low = 1'b1;

// Any bus activity at all, for the enable=0 test.
integer bus_edges = 0;
always @(sclk or att1 or att2 or user_out[2]) bus_edges = bus_edges + 1;

// Measure one complete port-1 frame's timing from the wires. Everything here
// is derived from BAUD_KHZ and the 20 us setup the module promises, so the
// same numbers must come out at every clk rate.
task measure_frame;
	time t_att, t_c1, t_c2, t_last, t_next;
	integer k;
	begin
		@(negedge att1);          t_att  = $time;
		@(negedge sclk);          t_c1   = $time;   // first clock edge of byte 0
		@(negedge sclk);          t_c2   = $time;   // second
		for (k = 0; k < 6; k = k + 1) @(negedge sclk);
		@(posedge sclk);          t_last = $time;   // byte 0's last clock edge
		@(negedge sclk);          t_next = $time;   // byte 1's first clock edge
		$display("TIMING clk=%0d kHz: ATT->CLK %0d ns, bus period %0d ns, byte gap %0d ns",
		         CLK_KHZ, t_c1 - t_att, t_c2 - t_c1, t_next - t_last);
		check("att setup ns", t_c1 - t_att, 20000);
		check("bus clock period ns", t_c2 - t_c1, BUS_PERIOD_NS);
		// The pad model acks 12 us after a byte's last falling edge, i.e. 10 us
		// after the master finishes the byte. A free-running master starts the
		// next byte one half-period (2 us) later regardless; only one that
		// waits for ACK lands in this window.
		check("byte gap is ACK-driven",
		      ((t_next - t_last) > 8000) && ((t_next - t_last) < 16000), 1);
	end
endtask

// Permanent regression guard for finding (b) of the 2026-08-06 re-review: a
// single-clock read of raw ACK let a 20 ns noise pulse mid-wait be mistaken
// for the real acknowledgement, accepting it ~8 us early and shortening the
// byte gap from ~10 us to ~2 us. Inject exactly that glitch -- 20 ns low, 2 us
// into byte 0's ACK wait, well under the ~200 ns glitch filter width and far
// ahead of the pad's real ACK (~10 us later) -- and confirm the byte gap is
// still governed by that real ACK, not the glitch.
task measure_frame_with_ack_glitch;
	time t_att, t_c1, t_c2, t_last, t_next;
	integer k;
	begin
		@(negedge att1);          t_att  = $time;
		@(negedge sclk);          t_c1   = $time;
		@(negedge sclk);          t_c2   = $time;
		for (k = 0; k < 6; k = k + 1) @(negedge sclk);
		@(posedge sclk);          t_last = $time;   // byte 0's last clock edge; ACK wait begins here

		#2000;                    // 2 us into the wait window
		ack_glitch_n = 1'b0;
		#20;                      // 20 ns glitch
		ack_glitch_n = 1'b1;

		@(negedge sclk);          t_next = $time;   // byte 1's first clock edge
		$display("GLITCH TEST byte gap with injected 20 ns ACK glitch: %0d ns (baseline ~10000 ns, glitch-accepted would read ~2000 ns)",
		          t_next - t_last);
		check("ack glitch rejected (byte gap still ACK-driven, not glitch-driven)",
		      ((t_next - t_last) > 8000) && ((t_next - t_last) < 16000), 1);
	end
endtask

initial begin
	$dumpfile("snac_psx_tb.vcd");
	$dumpvars(0, snac_psx_tb);

	repeat (10) @(posedge clk);
	reset = 0;

	// Bus timing, measured off the wires on the first frame.
	measure_frame;

	// ACK glitch rejection (finding (b), 2026-08-06 re-review): repeat the
	// same measurement on the very next frame, but with a 20 ns glitch
	// injected mid-wait. The byte gap it measures is the "after" figure to
	// compare against measure_frame's "before" -- both must land in the same
	// ACK-driven ~10 us window for the glitch to have been proven rejected.
	measure_frame_with_ack_glitch;
	@(posedge att1); #1;      // let the glitched frame finish and p1_frame latch
	check("port1 frame length unchanged by ACK glitch (clock edges)", p1_frame, 40);

	// One full poll of both ports at 250 kHz is on the order of 500 us.
	wait_polls(2);

	check("port1 device id", id0, 8'h41);

	// The digital pad's frame is five bytes and it acks four times. The master
	// must stop there, not clock a fixed nine bytes at a device that has
	// already released the bus.
	check("port1 frame length (clock edges)", p1_frame, 40);
	check("port2 frame length (clock edges)", p2_frame, 72);
	$display("FRAME port1 rx = %02h %02h %02h %02h %02h %02h %02h %02h %02h",
	         dut.rx_byte[0], dut.rx_byte[1], dut.rx_byte[2], dut.rx_byte[3],
	         dut.rx_byte[4], dut.rx_byte[5], dut.rx_byte[6], dut.rx_byte[7],
	         dut.rx_byte[8]);

	// Port 0 (SNAC port 1) is only re-polled roughly every 3.3 ms: the ST_GAP
	// state waits out ~1.3 ms after every port poll, twice per double-port
	// cycle, on top of the two frames. A 2 ms settle window can land in the gap
	// between polls and miss a button-state change entirely (observed: the
	// "x pressed" state below was skipped over by every poll when settled with
	// only #2000000). Settle for 5 ms - comfortably more than one full repoll
	// period - so every state change below is guaranteed to be sampled before
	// its check runs.
	// Byte 0 bit 5 is RIGHT, active low. Expect pad0[0].
	p1_btn0 = 8'hFF & ~8'h20;
	wait_polls(2);
	check("right pressed", pad0, 16'h0001);

	// Byte 1 bit 6 is X, active low. Expect pad0[4].
	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF & ~8'h40;
	wait_polls(2);
	check("x pressed", pad0, 16'h0010);

	// START is byte 0 bit 3 -> pad0[10]; SELECT is byte 0 bit 0 -> pad0[11].
	p1_btn0 = 8'hFF & ~8'h09;
	p1_btn1 = 8'hFF;
	wait_polls(2);
	check("start+select", pad0, 16'h0C00);

	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF;
	wait_polls(2);
	check("all released", pad0, 16'h0000);

	// Port 2 carries a DualShock. Axes arrive as {LY,LX,RY,RX}.
	wait_polls(2);
	check("port2 device id", id1, 8'h73);
	check("port2 axes", axes1, 32'h44332211);

	// Port 1 is digital: no axes, so it must stay centred rather than
	// picking up 0xFF padding from the short reply.
	check("port1 axes centred", axes0, 32'h80808080);

	// --- Unplug: both pads absent -------------------------------------
	// Hold a button on port 1, let it register, then simulate both pads
	// being unplugged (idle bus reads all 0xFF) and confirm both IDs report
	// absent (0x00) and the button vector clears. A button still held at
	// the moment of removal must not stay latched forever - in the
	// finished system that is a stuck menu key.
	p1_btn0 = 8'hFF;
	p1_btn1 = 8'hFF & ~8'h40;   // X held
	wait_polls(2);
	check("x pressed before unplug", pad0, 16'h0010);

	// Port 2's absent handling was previously only verified by assumed
	// symmetry with port 1: p2_btn0 never changed anywhere, so pad1 read
	// 0x0000 on absence whether or not the port-2 clearing logic actually
	// worked. Hold a button here too so the clear check below is
	// discriminating rather than vacuously true.
	p2_btn0 = 8'hFF & ~8'h20;   // RIGHT held
	wait_polls(2);
	check("right pressed before unplug p2", pad1, 16'h0001);

	pads_present = 0;
	wait_polls(2);
	check("absent port1 id", id0, 8'h00);
	check("absent port2 id", id1, 8'h00);
	check("absent port1 buttons clear", pad0, 16'h0000);
	check("absent port2 buttons clear", pad1, 16'h0000);
	// An absent port must time out, not hang: exactly one byte is clocked and
	// then the ACK wait expires. wait_polls() returning at all is the other
	// half of that proof - it is an unbounded wait on the next ATT edge.
	check("absent p1 frame clock edges", p1_frame, 8);
	check("absent p2 frame clock edges", p2_frame, 8);

	pads_present = 1;
	p1_btn1 = 8'hFF;
	p2_btn0 = 8'hFF;

	// --- Replug: the bus recovers when a pad is plugged back in ------
	// pads_present going back to 1 above was previously only claimed by
	// comment, not proven by any assertion, even though hot replug is a
	// completely normal user action. Confirm both ports come back and
	// report their real device IDs.
	wait_polls(2);
	check("replug port1 id", id0, 8'h41);
	check("replug port2 id", id1, 8'h73);

	// --- Transmit side ------------------------------------------------
	// Both pad models have been checking every command byte the master sent
	// for the whole run. Receive-side assertions cannot see a garbled poll
	// sequence at all: a real pad would simply not answer.
	check("port1 command stream", pad_p1.cmd_errors, 0);
	check("port2 command stream", pad_p2.cmd_errors, 0);

	// --- Bus discipline -----------------------------------------------
	// Both ports share CLK/CMD/DAT/ACK; selecting both at once would put two
	// devices on the same wires.
	check("only one ATT low at a time", both_att_low, 1'b0);

	// --- enable = 0 ---------------------------------------------------
	// The documented contract is "idle the bus and report nothing". Nothing
	// asserted it, so a version that kept polling with the outputs frozen, or
	// kept ATT asserted, would have passed.
	enable = 0;
	#50000;                       // let the in-flight frame unwind
	bus_edges = 0;
	#3000000;                     // ~2 full poll cycles' worth of wall clock
	check("disabled: bus silent", bus_edges, 0);
	check("disabled: ATT port1 high", att1, 1'b1);
	check("disabled: ATT port2 high", att2, 1'b1);
	check("disabled: CLK idle high", sclk, 1'b1);
	check("disabled: port1 id cleared", id0, 8'h00);
	check("disabled: port2 id cleared", id1, 8'h00);
	check("disabled: port1 buttons clear", pad0, 16'h0000);

	// ...and it must come back when re-enabled.
	enable = 1;
	wait_polls(2);
	check("re-enabled port1 id", id0, 8'h41);

	if (errors) begin
		$display("%0d FAILURE(S)", errors);
		$fatal(1);
	end
	$display("ALL TESTS PASSED");
	$finish;
end

endmodule
