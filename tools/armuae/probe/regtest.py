import mmap, os, struct, ctypes
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
    seq = [0x7FFF, 0x8001, 0x0001, 0x8002, 0x8004, 0x0006, 0x8400, 0x4000, 0xC000]
    for v in seq:
        wr16(0xDFF09A, v)
        print('INTENA <- %04x   INTENAR %04x' % (v, rd16(0xDFF01C)))
    for v in (0x7FFF, 0x8001, 0x8010, 0x0001):
        wr16(0xDFF096, v)
        print('DMACON <- %04x   DMACONR %04x' % (v, rd16(0xDFF002)))
    wr16(0xDFF09A, 0x7FFF); wr16(0xDFF096, 0x7FFF)
finally:
    ctl(1, 0)
