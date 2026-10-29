# First test of the flat window (ARM physical 0xC0000000): with the bus owned,
# the ROM read through it must match the ROM file, and chip RAM written
# through it must read back through it and through the lightweight bridge's
# banked window.
import mmap, os, struct, sys, ctypes, time
ROM = sys.argv[1]
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
lw = mmap.mmap(fd, 0x200000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xFF200000)
def ctl(n, v=None):
    c = ctypes.c_uint32.from_buffer(lw, 0x80000 + 4 * n)
    if v is not None: c.value = v
    return c.value
print('MAGIC %08x STATUS %08x' % (ctl(0), ctl(3)))
assert ctl(0) == 0x41435055 and ctl(3) & 0x100
hf = mmap.mmap(fd, 0x1000000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xC0000000)
def rd32(m, a): return ctypes.c_uint32.from_buffer(m, a).value
def wr32(m, a, v): ctypes.c_uint32.from_buffer(m, a).value = v
def be(v): return struct.unpack('>I', struct.pack('<I', v))[0]     # window is 68k byte order
ctl(1, 1 | 6)                                                       # OWN, FAST, FASTW
try:
    rom = open(ROM, 'rb').read()
    if len(rom) == 0x40000: rom = rom + rom
    t = time.time(); bad = 0
    for a in range(0, 0x80000, 0x1000):
        if be(rd32(hf, 0xF80000 + a)) != struct.unpack('>I', rom[a:a + 4])[0]: bad += 1
    print('ROM through the window: %d of 128 longs differ (%.1f ms)' % (bad, (time.time() - t) * 1e3))
    # chip RAM: write through the window, read through it and through bank 0
    pat = [0x12345678, 0x9ABCDEF0, 0xDEADBEEF, 0x0F1E2D3C]
    for i, p in enumerate(pat): wr32(hf, 0x7F000 + 4 * i, be(p))
    back = [be(rd32(hf, 0x7F000 + 4 * i)) for i in range(4)]
    ctl(2, 0)
    banked = [be(rd32(lw, 0x100000 + 0x7F000 + 4 * i)) for i in range(4)]
    print('chip $7F000 flat back:   ' + ' '.join('%08x' % x for x in back))
    print('chip $7F000 banked back: ' + ' '.join('%08x' % x for x in banked))
    print('OK' if back == pat and banked == pat else 'MISMATCH')
    # timing: 1000 flat chip reads
    t = time.time()
    for i in range(1000): rd32(hf, 0x7F000)
    print('1000 flat long reads of chip RAM: %.0f us each (python included)' % ((time.time() - t) * 1e3))
    print('STATUS %08x' % ctl(3))
finally:
    ctl(1, 0)
    print('bus given back')
