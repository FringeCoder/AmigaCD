#!/usr/bin/env python3
"""Host test for armcpu: a hand-assembled ROM on a plain file standing in for
the bridge (MEASUREMENT ONLY, see armcpu.c).

    python3 test_armcpu.py            # builds armcpu for the host and runs it
    python3 test_armcpu.py moira      # the same on Moira (cpu_moira.cpp)

The file is the bridge's 2 MB: armcpu_bridge's MAGIC and the 68000 bit set by
hand at 0x080000, the 68k window at 0x100000 in the 68k's byte order (the
bench keeps every address in bank 0 or banks that alias harmlessly). The ROM
checks what only the emulator decides: the reset vectors and the overlay come
from the ROM, a write lands big-endian, the overlay goes off when CIA-A's
DDRA and PRA say so, RESET puts it back and pulses CTRL bit 3, and the bus is
given back at the end.
"""
import mmap, os, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
moira = "moira" in sys.argv[1:]
exe = os.path.join(tempfile.gettempdir(), "armcpu_host_moira" if moira else "armcpu_host")
m = os.path.join(here, "musashi")
if not moira:
    subprocess.check_call(["gcc", "-O2", "-Wall", "-std=gnu99", "-I", here, "-I", m,
                           '-DMUSASHI_CNF="armcpu_conf.h"', "-o", exe,
                           os.path.join(here, "armcpu.c"), os.path.join(m, "m68kcpu.c"),
                           os.path.join(m, "m68kops.c"), os.path.join(m, "m68kdasm.c"),
                           os.path.join(m, "softfloat", "softfloat.c"), "-lm"])
else:
    mo = os.path.join(here, "moira")
    obj = exe + ".o"
    subprocess.check_call(["gcc", "-O2", "-Wall", "-std=gnu99", "-I", here, "-I", m,
                           '-DMUSASHI_CNF="armcpu_conf.h"', '-DARMCPU_CORE="Moira"', "-c", "-o", obj,
                           os.path.join(here, "armcpu.c")])
    subprocess.check_call(["g++", "-O1", "-std=c++20", "-I", here, "-I", m, "-I", mo,
                           "-o", exe, obj, os.path.join(here, "cpu_moira.cpp")] +
                          [os.path.join(mo, f) for f in ("Moira.cpp", "MoiraCore68000.cpp",
                           "MoiraCore68010.cpp", "MoiraCore68020.cpp", "MoiraDebugger.cpp")] + ["-lm"])

def w(*words):
    return b"".join(struct.pack(">H", x) for x in words)

code = (
    struct.pack(">II", 0x00080000, 0x00F80008) +   # SSP, PC
    w(0x2239, 0x0000, 0x0000) +                   # move.l $0,d1        (overlay: the ROM)
    w(0x23C1, 0x0000, 0x0300) +                   # move.l d1,$300
    w(0x33FC, 0x1234, 0x0000, 0x0100) +           # move.w #$1234,$100
    w(0x13FC, 0x0003, 0x00BF, 0xE201) +           # move.b #3,$BFE201   (DDRA: OVL an output)
    w(0x13FC, 0x0000, 0x00BF, 0xE001) +           # move.b #0,$BFE001   (PRA: OVL off)
    w(0x33FC, 0x5678, 0x0000, 0x0000) +           # move.w #$5678,$0    (chip RAM)
    w(0x3039, 0x0000, 0x0000) +                   # move.w $0,d0        (chip RAM now)
    w(0x33C0, 0x0000, 0x0200) +                   # move.w d0,$200
    w(0x4E70) +                                   # reset               (overlay back on)
    w(0x2439, 0x0000, 0x0004) +                   # move.l $4,d2        (the ROM again)
    w(0x23C2, 0x0000, 0x0400) +                   # move.l d2,$400
    w(0x60FE)                                     # bra.s *
)
rom = tempfile.NamedTemporaryFile(delete=False, suffix=".rom")
rom.write(code + b"\0" * (0x80000 - len(code))); rom.close()

f = tempfile.NamedTemporaryFile(delete=False)
f.write(b"\0" * 0x200000); f.flush()
mm = mmap.mmap(f.fileno(), 0x200000)
struct.pack_into("<I", mm, 0x80000, 0x41435055)      # MAGIC
struct.pack_into("<I", mm, 0x8000C, 0x100)           # STATUS: the CPU is a 68000
mm.flush()

out = subprocess.run([exe, "-d", f.name, "-r", rom.name, "-t", "0.3", "-q"],
                     capture_output=True, text=True, timeout=60)
print(out.stdout, out.stderr)

W = 0x100000
def at(a, n): return bytes(mm[W + a:W + a + n])
checks = {
    "reset vectors from the ROM": "SSP 00080000 PC 00f80008" in out.stdout,
    "overlay read of $0 is the ROM": at(0x300, 4) == bytes([0x00, 0x08, 0x00, 0x00]),
    "a word write lands big-endian": at(0x100, 2) == bytes([0x12, 0x34]),
    "overlay off: the write to $0 is chip RAM": at(0x000, 2) == bytes([0x56, 0x78]),
    "overlay off: the read of $0 is chip RAM": at(0x200, 2) == bytes([0x56, 0x78]),
    "RESET puts the overlay back": at(0x400, 4) == bytes([0x00, 0xF8, 0x00, 0x08]),
    "the bus given back (CTRL 0)": struct.unpack_from("<I", mm, 0x80004)[0] == 0,
    "ran to the end": out.returncode == 0 and "bus given back" in out.stdout,
}
ok = True
for name, good in checks.items():
    print(("ok   " if good else "FAIL ") + name)
    ok = ok and good

# A level that stays up is taken again and again, as on a 68000: Paula's
# lines are level-triggered, and Musashi drops a level it has taken unless it
# is handed the level again. The handler at $1000 counts, never clears.
irq = (struct.pack(">II", 0x00080000, 0x00F80008) +
    w(0x13FC, 0x0003, 0x00BF, 0xE201) +           # DDRA
    w(0x13FC, 0x0000, 0x00BF, 0xE001) +           # OVL off
    w(0x23FC, 0x0000, 0x1000, 0x0000, 0x0078) +   # move.l #$1000,$78 (level 6)
    w(0x33FC, 0x5279, 0x0000, 0x1000) +           # $1000: addq.w #1,$2000
    w(0x33FC, 0x0000, 0x0000, 0x1002) +
    w(0x33FC, 0x2000, 0x0000, 0x1004) +
    w(0x33FC, 0x4E73, 0x0000, 0x1006) +           #        rte
    w(0x46FC, 0x2000) +                           # move #$2000,sr
    w(0x60FE))                                    # bra.s *
rom2 = tempfile.NamedTemporaryFile(delete=False, suffix=".rom")
rom2.write(irq + b"\0" * (0x80000 - len(irq))); rom2.close()
mm[:] = b"\0" * 0x200000
struct.pack_into("<I", mm, 0x80000, 0x41435055)
struct.pack_into("<I", mm, 0x8000C, 0x106)          # a 68000, interrupt level 6
mm.flush()
r = subprocess.run([exe, "-d", f.name, "-r", rom2.name, "-t", "0.2", "-q"], capture_output=True, text=True, timeout=60)
taken = struct.unpack_from(">H", mm, W + 0x2000)[0]
good = r.returncode == 0 and taken > 100
print(("ok   " if good else "FAIL ") + "a level that stays up is taken again (%d times)" % taken)
ok = ok and good

# Fast RAM in ARM memory: the emulated Zorro II board answers $E80000 with its
# ROM nibbles until Kickstart -- here the ROM -- writes its base, keeps its
# memory off the bus, then leaves $E80000 to the bus; RESET puts it back.
ac = (struct.pack(">II", 0x00080000, 0x00F80008) +
    w(0x13FC, 0x0003, 0x00BF, 0xE201) +           # DDRA
    w(0x13FC, 0x0000, 0x00BF, 0xE001) +           # OVL off
    w(0x1039, 0x00E8, 0x0000, 0x13C0, 0x0000, 0x0300) +   # er_Type high nibble -> $300
    w(0x1039, 0x00E8, 0x0002, 0x13C0, 0x0000, 0x0301) +   # er_Type low nibble  -> $301
    w(0x1039, 0x00E8, 0x0004, 0x13C0, 0x0000, 0x0302) +   # er_Product high (inverted)
    w(0x1039, 0x00E8, 0x0006, 0x13C0, 0x0000, 0x0303) +   # er_Product low (inverted)
    w(0x13FC, 0x0000, 0x00E8, 0x004A) +           # base low nibble
    w(0x13FC, 0x0020, 0x00E8, 0x0048) +           # base high nibble: $200000
    w(0x33FC, 0xBEEF, 0x0020, 0x0000) +           # move.w #$BEEF,$200000
    w(0x3239, 0x0020, 0x0000, 0x33C1, 0x0000, 0x0304) +   # read it back -> $304
    w(0x1039, 0x00E8, 0x0000, 0x13C0, 0x0000, 0x0306) +   # $E80000 is the bus now
    w(0x4E70) +                                   # reset: the board is back
    w(0x1039, 0x00E8, 0x0000, 0x13C0, 0x0000, 0x0307) +
    w(0x60FE))
rom3 = tempfile.NamedTemporaryFile(delete=False, suffix=".rom")
rom3.write(ac + b"\0" * (0x80000 - len(ac))); rom3.close()
mm[:] = b"\0" * 0x200000
struct.pack_into("<I", mm, 0x80000, 0x41435055)
struct.pack_into("<I", mm, 0x8000C, 0x100)
mm.flush()
r = subprocess.run([exe, "-d", f.name, "-r", rom3.name, "-t", "0.2"], capture_output=True, text=True, timeout=60)
print(r.stdout[-400:], r.stderr[-300:])
fastchecks = {
    "fast RAM: er_Type nibbles E, 0 (8 MB Zorro II memory)": at(0x300, 2) == bytes([0xE0, 0x00]),
    "fast RAM: er_Product nibbles read inverted": at(0x302, 2) == bytes([0xF0, 0xE0]),
    "fast RAM: a write at its base reads back": at(0x304, 2) == bytes([0xBE, 0xEF]),
    "fast RAM: its memory never reaches the bus": at(0x000, 2) != bytes([0xBE, 0xEF]),
    "fast RAM: configured, $E80000 is the bus's": at(0x306, 1) == bytes([0x00]),
    "fast RAM: RESET puts the board back": at(0x307, 1) == bytes([0xE0]),
    "fast RAM: the run reports the board at $200000": "board last at 200000" in r.stdout,
}
for name, good in fastchecks.items():
    print(("ok   " if good else "FAIL ") + name)
    ok = ok and good

# A run that Musashi ends itself still gives the bus back: as a 68040, FSAVE
# (A0) is one of the FPU forms Musashi does not handle, and it calls exit(1).
die = (struct.pack(">II", 0x00080000, 0x00F80008) + w(0xF310, 0x60FE))
rom4 = tempfile.NamedTemporaryFile(delete=False, suffix=".rom")
rom4.write(die + bytes(0x80000 - len(die))); rom4.close()
mm[:] = bytes(0x200000)
struct.pack_into("<I", mm, 0x80000, 0x41435055)
struct.pack_into("<I", mm, 0x8000C, 0x100)
mm.flush()
r = subprocess.run([exe, "-d", f.name, "-r", rom4.name, "-t", "1", "-C", "68040", "-q"], capture_output=True, text=True, timeout=60)
if not moira:
    good = r.returncode != 0 and "FSAVE" in r.stderr and struct.unpack_from("<I", mm, 0x80004)[0] == 0
    print(("ok   " if good else "FAIL ") + "a run Musashi ends itself still gives the bus back (exit %d)" % r.returncode)
else:
    # Moira has no 68040 to run: refused, and the bus is not left held
    good = r.returncode == 2 and "68020 only" in r.stderr and struct.unpack_from("<I", mm, 0x80004)[0] == 0
    print(("ok   " if good else "FAIL ") + "Moira refuses a 68040 and leaves the bus free (exit %d)" % r.returncode)
ok = ok and good

# refusals: no MAGIC, not a 68000, and no lwbridge core without -d
struct.pack_into("<I", mm, 0x8000C, 0); mm.flush()
r = subprocess.run([exe, "-d", f.name, "-r", rom.name, "-t", "0.1"], capture_output=True, text=True, timeout=30)
ok = ok and r.returncode == 1 and "not set to 68000" in r.stderr
struct.pack_into("<I", mm, 0x80000, 0); mm.flush()
r = subprocess.run([exe, "-d", f.name, "-r", rom.name, "-t", "0.1"], capture_output=True, text=True, timeout=30)
ok = ok and r.returncode == 1 and "no armcpu_bridge" in r.stderr
r = subprocess.run([exe, "-r", rom.name, "-t", "0.1"], capture_output=True, text=True, timeout=30)
ok = ok and r.returncode == 2 and "lwbridge" in r.stderr

print("RUN:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
