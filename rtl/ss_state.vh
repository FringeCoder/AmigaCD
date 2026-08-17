`ifndef SS_STATE_VH
`define SS_STATE_VH
// Guarded because two files include it now: Minimig.sv and
// ss_state_fanout.v. Without this the second include redefines both
// macros and Quartus warns on every build.

// ---------------------------------------------------------------------------
// Save state vector, Phase 1A.
//
// One macro defines the vector. A register that appears here is captured and
// restored; one that does not appear is neither. Both directions of ss_serdes
// expand from the same WIDTH, so a save-side-only addition is a width mismatch
// and a synthesis error.
//
// SS_STATE_W must equal the total width of SS_STATE_LIST. Keeping them in one
// file makes that a single-line edit rather than two edits in two modules.
//
// Phase 1B extends this with the chipset inventory (Agnus, Denise, Paula, CIA,
// Akiko). Phase 1A carries only what is needed to prove the framework: the
// CPU's architectural registers and Gary's memory map state.
//
// Every name below is a wire in Minimig.sv. The plan put the vector in
// minimig.v, but the CPU register file export lives on cpu_wrapper, which is
// instantiated in Minimig.sv -- routing 632 bits down into minimig.v and the
// register read port back out again buys nothing. minimig.v exports the four
// memory-map bits as ss_map[3:0] instead, and Minimig.sv unpacks them into
// the four names used here.
//
// The list is used as an LVALUE as well as an rvalue (Phase 1B-1). Minimig.sv
// packs it into ss_state_in; ss_state_fanout, a module at the bottom of the
// same file, declares nets of the same names and drives them with
//
//     assign `SS_STATE_LIST = state;
//
// There is exactly one ordered list, so the two directions cannot disagree
// about order: there is no second list to drift from. What a concatenation
// assignment does NOT catch is a declared width that is wrong -- it truncates
// or zero-pads silently -- so ss_state_fanout carries an elaboration guard
// comparing $bits of this list against SS_STATE_W. Do not remove it.
//
// Two entries are captured but deliberately NOT restored, and there is no
// honest way to change that: ss_sel_kick1mb and ss_sel_kick256kmirror are not
// state at all. gary.v drives both from a combinational always @(*) address
// decode (gary.v:192-193), so their value is a function of whatever address
// happens to be on the CPU bus in the cycle the snapshot is taken, and a
// "restored" value would be overwritten combinationally in the same cycle it
// was written. Only ss_ovl (minimig.v) and ss_rom_readonly (gary.v) are real
// registers behind ss_map. The two decode bits stay in the vector for now
// because removing them changes SS_STATE_W and therefore the on-disk payload
// length; drop them when 1B-2 grows the vector and the format moves anyway.
// ---------------------------------------------------------------------------

`define SS_STATE_LIST { \
	ss_cpu_d0, ss_cpu_d1, ss_cpu_d2, ss_cpu_d3, \
	ss_cpu_d4, ss_cpu_d5, ss_cpu_d6, ss_cpu_d7, \
	ss_cpu_a0, ss_cpu_a1, ss_cpu_a2, ss_cpu_a3, \
	ss_cpu_a4, ss_cpu_a5, ss_cpu_a6, ss_cpu_a7, \
	ss_pc, ss_usp, ss_vbr, ss_sr, ss_cacr, \
	ss_ovl, ss_rom_readonly, ss_sel_kick1mb, ss_sel_kick256kmirror }

// 16 registers + PC + USP + VBR (32 each) + SR (16) + CACR (4) + 4 map bits
`define SS_STATE_W (16*32 + 32 + 32 + 32 + 16 + 4 + 4)

`endif
