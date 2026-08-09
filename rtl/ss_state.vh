// ---------------------------------------------------------------------------
// Save state vector, Phase 1A.
//
// One macro defines the vector. A register that appears here is captured and
// (from Phase 1B, when the restore path is wired) restored; one that does not
// appear is neither. Both directions of ss_serdes expand from the same WIDTH,
// so a save-side-only addition is a width mismatch and a synthesis error.
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
