// armcpu on Moira instead of Musashi. MEASUREMENT BUILD -- see armcpu.c and
// docs/arm-cpu-plan.md.
//
// Moira (moira/, MIT, Dirk W. Hoffmann's core from vAmiga) is put behind the
// part of Musashi's C API that armcpu.c uses, so the same armcpu.c builds
// against either core: `make` gives armcpu (Musashi), `make armcpu-moira`
// this one. Moira's client API is linked statically (MOIRA_VIRTUAL_API
// false): the bus functions below are Moira's own members, and each is a
// direct call into armcpu.c's m68k_read_memory_* / m68k_write_memory_*.
//
// What differs from Musashi:
//   - Moira executes the 68000, 68010, 68EC020 and 68020 only. Asking for
//     a 68030 or 68040 is refused at start-up.
//   - Its IPL input is a level, sampled as the real pins are; Musashi's
//     m68k_set_irq is an edge that has to be re-asserted (armcpu.c does that
//     every slice, which is harmless here).
//   - There is no per-instruction hook (Moira's are compile-time), so -D,
//     the trace of the last instructions before a stop, is empty.

#include <stdio.h>
#include <stdlib.h>

#include "Moira.h"

extern "C" {
#include "m68k.h"
}

using namespace moira;

static Moira cpu;
static void (*reset_instr_cb)(void);

// ---- the bus: straight into armcpu.c -----------------------------------------

u8  Moira::read8(u32 addr) const  { return (u8)m68k_read_memory_8(addr & 0xFFFFFF); }
u16 Moira::read16(u32 addr) const { return (u16)m68k_read_memory_16(addr & 0xFFFFFF); }
u32 Moira::read32(u32 addr) const { return m68k_read_memory_32(addr & 0xFFFFFF); }
u16 Moira::read16OnReset(u32 addr) const { return read16(addr); }
u16 Moira::read16Dasm(u32 addr) const { return read16(addr); }
void Moira::write8(u32 addr, u8 val) const   { m68k_write_memory_8(addr & 0xFFFFFF, val); }
void Moira::write16(u32 addr, u16 val) const { m68k_write_memory_16(addr & 0xFFFFFF, val); }
void Moira::write32(u32 addr, u32 val) const { m68k_write_memory_32(addr & 0xFFFFFF, val); }
u8  Moira::dsack(u32) const { return DSACK_16; }
u16 Moira::readIrqUserVector(u8) const { return 0; }

void Moira::sync(int cycles) { clock += cycles; }

// ---- delegates: only RESET matters --------------------------------------------

void Moira::cpuDidReset() { }
void Moira::cpuDidHalt() { fprintf(stderr, "armcpu: Moira halted (double bus fault) at PC %06x\n", reg.pc0 & 0xFFFFFF); }
void Moira::willExecute(const char *, Instr, Mode, Size, u16) { }
void Moira::didExecute(const char *, Instr I, Mode, Size, u16)
{
	// MOIRA_DID_EXECUTE is I == Instr::RESET: the instruction, not the CPU
	if (I == Instr::RESET && reset_instr_cb) reset_instr_cb();
}
void Moira::willExecute(M68kException, u16) { }
void Moira::didExecute(M68kException, u16) { }
void Moira::willInterrupt(u8) { }
void Moira::didJumpToVector(int, u32) { }
void Moira::didChangeCACR(u32) { }
void Moira::didChangeCAAR(u32) { }
void Moira::didReachSoftstop(u32) { }
void Moira::didReachBreakpoint(u32) { }
void Moira::didReachWatchpoint(u32) { }
void Moira::didReachCatchpoint(u8) { }
void Moira::didReachSoftwareTrap(u32) { }
void Moira::didLogInstruction(const Registers &) { }

// ---- Musashi's API, the part armcpu.c uses ------------------------------------

extern "C" {

void m68k_init(void) { }

void m68k_set_cpu_type(unsigned int type)
{
	switch (type) {
	case M68K_CPU_TYPE_68000:   cpu.setModel(Model::M68000); break;
	case M68K_CPU_TYPE_68010:   cpu.setModel(Model::M68010); break;
	case M68K_CPU_TYPE_68EC020: cpu.setModel(Model::M68EC020); break;
	case M68K_CPU_TYPE_68020:   cpu.setModel(Model::M68020); break;
	default:
		fprintf(stderr, "armcpu: Moira executes the 68000, 68010, 68EC020 and 68020 only\n");
		exit(2);
	}
}

void m68k_set_reset_instr_callback(void (*callback)(void)) { reset_instr_cb = callback; }

void m68k_pulse_reset(void) { cpu.reset(); }

int m68k_execute(int num_cycles)
{
	i64 c0 = cpu.getClock();
	cpu.execute(num_cycles);
	return (int)(cpu.getClock() - c0);
}

void m68k_set_irq(unsigned int int_level) { cpu.setIPL((u8)int_level); }

unsigned int m68k_get_reg(void *, m68k_register_t r)
{
	switch (r) {
	case M68K_REG_PC:  return cpu.getPC();
	case M68K_REG_PPC: return cpu.getPC0();
	case M68K_REG_SR:  return cpu.getSR();
	case M68K_REG_SP:
	case M68K_REG_A7:  return cpu.getA(7);
	default:
		if (r >= M68K_REG_D0 && r <= M68K_REG_D7) return cpu.getD(r - M68K_REG_D0);
		if (r >= M68K_REG_A0 && r <= M68K_REG_A6) return cpu.getA(r - M68K_REG_A0);
		return 0;
	}
}

unsigned int m68k_disassemble(char *str_buff, unsigned int pc, unsigned int)
{
	return (unsigned int)cpu.disassemble(str_buff, pc);
}

}
