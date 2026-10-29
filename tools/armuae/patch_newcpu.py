# armcpu's local patch to Amiberry's newcpu.cpp: the ARM32 JIT addresses
# countdown (pissoff) and NATMEM_OFFSET as [R_REGSTRUCT, #imm12] from &regs,
# so both must lie within 4095 bytes of regs. One section keeps them together.
import sys
p = sys.argv[1]
s = open(p, newline='').read()
def rep(a, c):
    global s
    assert s.count(a) == 1, a
    s = s.replace(a, c)
rep('int pissoff = 0;', '#define JITREGS __attribute__((section(".bss.jitregs")))\nint pissoff JITREGS = 0;\nuae_u8 *natmem_offset JITREGS;')
rep('struct regstruct regs;', 'struct regstruct regs JITREGS;')
open(p, 'w', newline='').write(s)
print('patched', p)
