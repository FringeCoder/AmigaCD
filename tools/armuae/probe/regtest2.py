import mmap, os, ctypes, time
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
lw = mmap.mmap(fd, 0x200000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xFF200000)
hf = mmap.mmap(fd, 0x1000000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=0xC0000000)
def ctl(n, v=None):
    c = ctypes.c_uint32.from_buffer(lw, 0x80000 + 4 * n)
    if v is not None: c.value = v
    return c.value
sw16 = lambda v: ((v & 0xFF) << 8) | (v >> 8)
def rd16(a): return sw16(ctypes.c_uint16.from_buffer(hf, a).value)
def wr16(a, v): ctypes.c_uint16.from_buffer(hf, a).value = sw16(v)
ctl(1, 1 | 6)
try:
    for prev in (0x0000, 0x00F0, 0x0F00, 0x1234, 0xFFFF):
        wr16(0xDFF09A, 0x7FFF)
        wr16(0xDFF180, prev)          # COLOR00: a write with known data just before
        wr16(0xDFF09A, 0x8001)
        a = rd16(0xDFF01C)
        wr16(0xDFF09A, 0x7FFF)
        wr16(0x7F300, prev)           # a chip RAM write instead
        wr16(0xDFF09A, 0x8001)
        b = rd16(0xDFF01C)
        wr16(0xDFF09A, 0x7FFF)
        x = rd16(0x7F300)              # a read just before
        wr16(0xDFF09A, 0x8001)
        c = rd16(0xDFF01C)
        print('prev %04x: after COLOR00 %04x, after chip write %04x, after chip read %04x' % (prev, a, b, c))
    wr16(0xDFF09A, 0x7FFF); wr16(0xDFF180, 0)
    print('mode 2 (FAST only):'); ctl(1, 1 | 2)
    wr16(0xDFF09A, 0x7FFF); wr16(0xDFF09A, 0x8001); print('  INTENAR %04x' % rd16(0xDFF01C))
    print('mode 4 (FASTW only):'); ctl(1, 1 | 4)
    wr16(0xDFF09A, 0x7FFF); wr16(0xDFF09A, 0x8001); print('  INTENAR %04x' % rd16(0xDFF01C))
    wr16(0xDFF09A, 0x7FFF)
finally:
    ctl(1, 0)
