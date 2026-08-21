`timescale 1ns/1ns

`include "rtl/ss_state.vh"

/////////////////////////////////////////////////////////////////////////////
// Restore fan-out: the state vector, back into the machine.
//
// This exists as a module rather than as a block inside Minimig.sv for one
// reason: it lets the restore side reuse `SS_STATE_LIST as an LVALUE.
//
//     assign `SS_STATE_LIST = state;
//
// The names in that macro are declared below as nets of this module, so the
// same ordered list that Minimig.sv packs into ss_state_in is what unpacks
// here. There is no second list, so there is nothing to drift: adding a
// register to the vector without giving it a home here is an undeclared
// identifier, and removing one from the middle shifts BOTH directions
// together. Writing the reverse concatenation out by hand -- the obvious
// alternative -- would reintroduce exactly the ordering bug the macro exists
// to prevent, and would do it invisibly: a machine restored with two fields
// transposed resumes and then misbehaves.
//
// What a concatenation assignment does NOT catch is a wrong DECLARED WIDTH:
// it truncates or zero-pads in silence, and every field above the mistake
// shifts. The elaboration guard below closes that hole.
//
// Sequencing. The TG68K register file has one write port, so the sixteen
// registers go in one per cycle, mirroring the Phase 1A read sweep. PC, SR,
// USP, VBR and CACR share the same ss_wr_data bus and take one cycle each on
// their own enable. The Gary map bits go last.
//
// Clock. This runs on clk_sys because everything it drives does: the TG68K
// kernel takes these writes on the CPU clock, and minimig.v's ovl and gary's
// rom_readonly are clk_sys registers. Running it on ss_ctrl's clk_114 instead
// would emit one-cycle pulses that a clk_sys edge sees only one time in four.
/////////////////////////////////////////////////////////////////////////////
//
// The restore's register-write sequencer.
//
// ss_ctrl deserialises the state vector and hands it over as one wide bus; this
// walks it out into cpu_wrapper's TG68K restore port one register per cycle,
// then Gary's memory map, then the sequencer re-seed. It is the whole of the
// register-write half of a restore.
//
// It lived at the bottom of Minimig.sv, which meant no testbench could compile
// it without the entire top level -- so the one part of a restore that decides
// whether the CPU comes back with the right register file had no bench at all,
// and the only extracted copy was a stale lint artefact under build/lint/.
// It is a plain module with a narrow interface; there was never a reason for it
// to be in there.
//
module ss_state_fanout
#(
	parameter STATE_W = 632
)
(
	input                     clk,      // clk_sys
	input                     rst_n,

	// Level-based handshake with the clk_114 side. req is held until ack.
	input                     req,
	output reg                ack,
	input      [STATE_W-1:0]  state,

	// TG68K restore write port, via cpu_wrapper. One shared data bus.
	output reg  [3:0]         cpu_wr_index,
	output reg [31:0]         cpu_wr_data,
	output reg                cpu_wr_en,
	output reg                cpu_pc_wr,
	output reg                cpu_sr_wr,
	output reg                cpu_usp_wr,
	output reg                cpu_vbr_wr,
	output reg                cpu_cacr_wr,
	// Sequencer re-seed. Strobed once, after every register above has
	// landed and while cpu_wrapper still has the CPU parked (busy is still
	// high on this cycle). Restoring the programmer's model without this
	// resumes the CPU part-way through whatever instruction it was frozen
	// in -- see TG68KdotC_Kernel.vhd's ss_resume comment, and the freeze
	// point scan in rtl/sim/tg68k/tg68k_ss_tb.vhd for what that does.
	output reg                cpu_resume,

	// Gary memory map, via minimig.v. Bit order is ss_map's, both ways.
	output reg  [3:0]         map_in,
	output reg                map_we,

	// The restored INTREQ, straight off the unpacked vector rather than
	// written anywhere by this sequencer: ss_regshadow's replay writes it
	// into Paula itself, by the set/clear dance, in its own ordering --
	// after the plain registers, before INTENA. This output is what that
	// replay reads.
	//
	// Combinational, and stable for as long as ss_ctrl holds `state`,
	// which is until the next restore. The replay runs inside this one.
	output     [14:0]         intreq_out,

	// The CIAs, by value, and the pulse that writes them. Unlike the CPU
	// register file these go in as one wide word each rather than a sequence:
	// ciaa.v and ciab.v take the whole thing on one clk edge, so there is
	// nothing to sequence and nothing that can land half-written.
	output    [190:0]         cia_a_out,
	output    [202:0]         cia_b_out,
	output reg                cia_we,

	// Akiko, by value, on the same one-edge terms as the CIAs and on the
	// same step: the three are independent buses and none of them can land
	// half-written, so there is nothing to sequence between them. Separate
	// strobes rather than one shared wire so that a later change can move
	// Akiko without disturbing the CIAs.
	output [`SS_AKIKO_W-1:0]  akiko_out,
	output reg                akiko_we,

	output                    busy
);

// The restore-side view of the vector. Widths here must match the capture
// side's exactly; see the guard below.
wire [31:0] ss_cpu_d0, ss_cpu_d1, ss_cpu_d2, ss_cpu_d3;
wire [31:0] ss_cpu_d4, ss_cpu_d5, ss_cpu_d6, ss_cpu_d7;
wire [31:0] ss_cpu_a0, ss_cpu_a1, ss_cpu_a2, ss_cpu_a3;
wire [31:0] ss_cpu_a4, ss_cpu_a5, ss_cpu_a6, ss_cpu_a7;
wire [31:0] ss_pc, ss_usp, ss_vbr;
wire [15:0] ss_sr;
wire  [3:0] ss_cacr;
wire        ss_ovl, ss_rom_readonly, ss_sel_kick1mb, ss_sel_kick256kmirror;
wire [14:0] ss_intreq;
wire [190:0] ss_cia_a;
wire [202:0] ss_cia_b;
wire [`SS_AKIKO_W-1:0] ss_akiko;

assign `SS_STATE_LIST = state;

// The restored INTREQ, out to ss_regshadow's replay. See the port.
assign intreq_out = ss_intreq;
assign cia_a_out  = ss_cia_a;
assign cia_b_out  = ss_cia_b;
assign akiko_out  = ss_akiko;

// Elaboration guard. A concatenation assignment silently truncates, so a
// mistyped width above would shift every field beyond it and restore a
// plausible-looking machine that is wrong everywhere. An unresolvable module
// instance is a hard Quartus error, which is what this wants to be.
//
// SystemVerilog, hence the .sv: Quartus 17.0 rejects $bits outright in a
// Verilog-2001 file ("system function \"$bits\" is not supported for
// synthesis"), and accepts it in a .sv one. The guard was written inside
// Minimig.sv, which is .sv, so moving this module to a .v file broke a fit
// that had never been run against it -- the extension was carrying the
// feature.
//
// Synthesis only. Icarus evaluates $bits() of a concatenation of NETS as 0
// at elaboration -- measured, not assumed -- so the check would fire on
// every simulation build and no testbench could compile this module at
// all. Quartus evaluates it correctly, and Quartus is where the guard has
// to work: it is a synthesis-time assertion about declared widths, which
// cannot differ between the two tools. The cost is that the bench next
// door cannot exercise the guard itself.
`ifndef __ICARUS__
localparam integer SS_LIST_W = $bits(`SS_STATE_LIST);
generate
	if (SS_LIST_W != STATE_W) begin : gen_ss_state_width_check
		ss_state_list_width_does_not_match_STATE_W u_check ();
	end
endgenerate
`endif

// Index order is the TG68K register file's, which is also the export sweep's:
// 0-7 = D0-D7, 8-15 = A0-A7. Getting this wrong by eight restores the data
// registers into the address registers, which is why Task 8 checks one of
// each rather than only a D register.
//
// A plain combinational block rather than a function: Quartus's 10036 lint
// does not count a read that happens only inside a function body, so the
// function version synthesised correctly but reported all sixteen registers
// as "assigned a value but never read", which is exactly the warning a real
// mistake here would produce. Not worth losing.
reg [3:0]  reg_sel_idx;
reg [31:0] reg_sel;
always @(*) begin
	reg_sel_idx = step[3:0];
	case (reg_sel_idx)
	4'd0:  reg_sel = ss_cpu_d0;
	4'd1:  reg_sel = ss_cpu_d1;
	4'd2:  reg_sel = ss_cpu_d2;
	4'd3:  reg_sel = ss_cpu_d3;
	4'd4:  reg_sel = ss_cpu_d4;
	4'd5:  reg_sel = ss_cpu_d5;
	4'd6:  reg_sel = ss_cpu_d6;
	4'd7:  reg_sel = ss_cpu_d7;
	4'd8:  reg_sel = ss_cpu_a0;
	4'd9:  reg_sel = ss_cpu_a1;
	4'd10: reg_sel = ss_cpu_a2;
	4'd11: reg_sel = ss_cpu_a3;
	4'd12: reg_sel = ss_cpu_a4;
	4'd13: reg_sel = ss_cpu_a5;
	4'd14: reg_sel = ss_cpu_a6;
	default: reg_sel = ss_cpu_a7;
	endcase
end

localparam [4:0] STEP_PC   = 5'd16;
localparam [4:0] STEP_SR   = 5'd17;
localparam [4:0] STEP_USP  = 5'd18;
localparam [4:0] STEP_VBR  = 5'd19;
localparam [4:0] STEP_CACR = 5'd20;
localparam [4:0] STEP_MAP  = 5'd21;
// The CIAs go in before the re-seed and after the map, on their own pulse.
localparam [4:0] STEP_CIA  = 5'd22;
localparam [4:0] STEP_RES  = 5'd23;
// One idle step after the re-seed, so `running` -- and therefore busy, and
// therefore cpu_wrapper's ss_arm -- is still high on the cycle cpu_resume
// goes out. The kernel wants the seed applied before the CPU clock enable
// is released (TG68KdotC_Kernel.vhd's ss_resume comment), and dropping
// running in the same edge that raised cpu_resume left that resting on
// ss_load_busy still being high in Minimig.sv's ss_arm term -- another
// module's timing, for a property this module claims to enforce on its own.
// Caught by ss_state_fanout_tb.
localparam [4:0] STEP_DONE = 5'd24;
localparam [4:0] STEP_LAST = STEP_DONE;

reg [4:0] step;
reg       running;

assign busy = running;

always @(posedge clk) begin
	if (!rst_n) begin
		running      <= 1'b0;
		ack          <= 1'b0;
		step         <= 5'd0;
		cpu_wr_en    <= 1'b0;
		cpu_pc_wr    <= 1'b0;
		cpu_sr_wr    <= 1'b0;
		cpu_usp_wr   <= 1'b0;
		cpu_vbr_wr   <= 1'b0;
		cpu_cacr_wr  <= 1'b0;
		map_we       <= 1'b0;
		cia_we       <= 1'b0;
		akiko_we     <= 1'b0;
		cpu_resume   <= 1'b0;
	end
	else begin
		// Every enable is a one-cycle pulse. The data bus and the index are
		// registered in the same cycle as the enable that consumes them, so
		// they arrive at the kernel's clock edge together.
		cpu_wr_en   <= 1'b0;
		cpu_pc_wr   <= 1'b0;
		cpu_sr_wr   <= 1'b0;
		cpu_usp_wr  <= 1'b0;
		cpu_vbr_wr  <= 1'b0;
		cpu_cacr_wr <= 1'b0;
		map_we      <= 1'b0;
		cia_we      <= 1'b0;
		akiko_we    <= 1'b0;
		cpu_resume  <= 1'b0;

		if (!running) begin
			// ack is held until req drops, so the clk_114 side sees it
			// regardless of the 4:1 ratio between the two clocks.
			if (!req)     ack <= 1'b0;
			else if (!ack) begin
				running <= 1'b1;
				step    <= 5'd0;
			end
		end
		else begin
			case (step)
			STEP_PC:   begin cpu_wr_data <= ss_pc;                 cpu_pc_wr   <= 1'b1; end
			// SR is taken from ss_wr_data[15:0] inside the kernel; the CCR
			// half goes to the flags submodule from the same bus.
			STEP_SR:   begin cpu_wr_data <= {16'd0, ss_sr};        cpu_sr_wr   <= 1'b1; end
			STEP_USP:  begin cpu_wr_data <= ss_usp;                cpu_usp_wr  <= 1'b1; end
			STEP_VBR:  begin cpu_wr_data <= ss_vbr;                cpu_vbr_wr  <= 1'b1; end
			STEP_CACR: begin cpu_wr_data <= {28'd0, ss_cacr};      cpu_cacr_wr <= 1'b1; end
			// Same bit order as minimig.v's ss_map output. Only [3] (ovl) and
			// [2] (rom_readonly) have a target; [1:0] are combinational
			// address decodes -- see rtl/ss_state.vh.
			STEP_MAP:  begin
				map_in <= {ss_ovl, ss_rom_readonly, ss_sel_kick1mb, ss_sel_kick256kmirror};
				map_we <= 1'b1;
			end
			// Last, and after STEP_PC: the re-seed leaves TG68_PC alone and
			// starts the CPU fetching from whatever it already holds.
			// Both CIAs at once: the two buses are independent and each lands
			// whole on one edge.
			STEP_CIA:  begin cia_we <= 1'b1; akiko_we <= 1'b1; end
			STEP_RES:  cpu_resume <= 1'b1;
			// Nothing is strobed here; the step exists to hold busy over the
			// cycle the re-seed is on the wire.
			STEP_DONE: ;
			default: begin
				cpu_wr_index <= step[3:0];
				cpu_wr_data  <= reg_sel;
				cpu_wr_en    <= 1'b1;
			end
			endcase

			if (step == STEP_LAST) begin
				running <= 1'b0;
				ack     <= 1'b1;
			end
			else step <= step + 5'd1;
		end
	end
end

endmodule
