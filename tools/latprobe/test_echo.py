#!/usr/bin/env python3
"""Host test for latprobe: plays latprobe_ddr3.v on a plain file.

    python3 test_echo.py            # builds latprobe for the host and runs it

Maps a 64 KB file, polls REQ like the FPGA does and answers each new seq with
the same RESP format (rd_lat 7, polls counted), then checks the tool's output.
"""
import mmap, os, struct, subprocess, sys, tempfile, threading, time

REQ, RESP, MAGIC = 0x8080, 0x8088, 0x4C415450
here = os.path.dirname(os.path.abspath(__file__))
exe = os.path.join(tempfile.gettempdir(), "latprobe_host")
subprocess.check_call(["gcc", "-O2", "-Wall", "-Wextra", "-std=gnu99", "-o", exe,
                       os.path.join(here, "latprobe.c")])

f = tempfile.NamedTemporaryFile(delete=False)
f.write(b"\0" * 0x10000); f.flush()
m = mmap.mmap(f.fileno(), 0x10000)
stop = False
answered = 0

def fpga():
    global answered
    last, polls = None, 0
    while not stop:
        lo, hi = struct.unpack_from("<II", m, REQ)
        if hi == MAGIC and lo != last:
            last = lo
            struct.pack_into("<II", m, RESP, (7 << 16) | min(polls, 0xFFFF), lo)
            answered += 1
            polls = 0
        elif hi == MAGIC:
            polls += 1

t = threading.Thread(target=fpga); t.start()
out = subprocess.run([exe, "-n", "2000", "-d", f.name], capture_output=True, text=True, timeout=120)
stop = True; t.join()
print(out.stdout, out.stderr)
ok = (out.returncode == 0 and "2000 round trips" in out.stdout and "0 timed out" in out.stdout
      and "median 7 (" in out.stdout and answered == 2001
      and struct.unpack_from("<I", m, REQ + 4)[0] == 0)
# -B (no barriers) must still be answered every time, and -r must report
# all four access kinds without needing the FPGA.
stop = False; answered = 0
t = threading.Thread(target=fpga); t.start()
outb = subprocess.run([exe, "-n", "500", "-B", "-d", f.name], capture_output=True, text=True, timeout=120)
stop = True; t.join()
ok = ok and outb.returncode == 0 and "500 round trips" in outb.stdout and "no barriers" in outb.stdout
outr = subprocess.run([exe, "-r", "-n", "20000", "-d", f.name], capture_output=True, text=True, timeout=120)
print(outr.stdout)
ok = ok and outr.returncode == 0 and all(k in outr.stdout for k in ("read ", "write ", "write + dmb", "write, read back"))
# -F (read-back after the request write) is answered every time too.
stop = False; answered = 0
t = threading.Thread(target=fpga); t.start()
outf = subprocess.run([exe, "-n", "500", "-F", "-d", f.name], capture_output=True, text=True, timeout=120)
stop = True; t.join()
ok = ok and outf.returncode == 0 and "500 round trips" in outf.stdout and "read-back flush" in outf.stdout

# A probe that stops answering mid-run must not keep the tool spinning: it
# gives up after a few consecutive timeouts (100 ms each), well inside 10 s.
limit = 300
def fpga_dies():
    global answered
    last = None
    while not stop and answered < limit:
        lo, hi = struct.unpack_from("<II", m, REQ)
        if hi == MAGIC and lo != last:
            last = lo
            struct.pack_into("<II", m, RESP, 7 << 16, lo)
            answered += 1
stop = False; answered = 0
t = threading.Thread(target=fpga_dies); t.start()
t0 = time.time()
outd = subprocess.run([exe, "-n", "100000", "-d", f.name], capture_output=True, text=True, timeout=60)
stop = True; t.join()
gave_up = "giving up" in outd.stderr and time.time() - t0 < 10
print(outd.stdout, outd.stderr)
ok = ok and gave_up
# -L (step 1d, the lightweight bridge) on a plain file standing in for the
# lw_echo registers: MAGIC in register 1, so ECHO in register 0 reads back
# what was written. Without -d it must refuse to touch 0xFF200000 unless a
# core with "lwbridge" in its path is running, which it is not here.
struct.pack_into("<I", m, 4, 0x4C574252); m.flush()
outl = subprocess.run([exe, "-L", "-n", "20000", "-d", f.name], capture_output=True, text=True, timeout=120)
print(outl.stdout, outl.stderr)
ok = ok and outl.returncode == 0 and "0 of 20000 read back a different value" in outl.stdout
struct.pack_into("<I", m, 4, 0); m.flush()
outn = subprocess.run([exe, "-L", "-n", "100", "-d", f.name], capture_output=True, text=True, timeout=60)
ok = ok and outn.returncode == 1 and "no lw_echo behind the bridge" in outn.stderr
outg = subprocess.run([exe, "-L", "-n", "100"], capture_output=True, text=True, timeout=60)
print(outg.stderr)
ok = ok and outg.returncode == 2 and "lwbridge" in outg.stderr

# -V (step 1e, DDR3 write visibility) on a file holding the bridge registers
# at 0 and the DDR3 window at 0x1000. The stand-in plays visprobe_ddr3.v:
# an A_GO is answered once word A holds the value (word A is made to go
# stale for the first few reads of every other trial, so stale reads are
# counted), a B_GO writes word B late enough that the ARM polls, and B_SEEN
# reports b_cycles. Every trial must complete, in all five measurements.
LW, DD = 0, 0x1000
VA, VB = DD + 0x8090, DD + 0x8098
g = tempfile.NamedTemporaryFile(delete=False)
g.write(b"\0" * (DD + 0x10000)); g.flush()
w = mmap.mmap(g.fileno(), DD + 0x10000)
def rd(o): return struct.unpack_from("<I", w, o)[0]
def wr(o, v): struct.pack_into("<I", w, o, v)
wr(LW + 4, 0x4C574252)
a_trials = b_trials = 0
def visprobe():
    global a_trials, b_trials
    t_start = time.time()
    mine4, mine5, mine6 = 0, 0, 0
    while not stop:
        wr(LW + 8, int((time.time() - t_start) * 113.5e6) & 0xFFFFFFFF)
        r4 = rd(LW + 16)
        if r4 != mine4:                       # A_GO
            misses = 0
            while rd(VA) != r4 or (a_trials % 2 and misses < 3):
                misses += 1
                if stop: return
            a_trials += 1
            mine5 = 16 + 20 * misses; wr(LW + 20, mine5)
            mine4 = 0x80000000 | misses; wr(LW + 16, mine4)
        r5 = rd(LW + 20)
        if r5 != mine5:                       # B_GO
            mine6 = 0x40000000 | 5; wr(LW + 24, mine6)
            time.sleep(0.0002)
            wr(VB, r5)
            mine5 = r5
            b_trials += 1
        r6 = rd(LW + 24)
        if r6 != mine6:                       # B_SEEN
            wr(LW + 28, 300 if b_trials % 2 == 0 else 100)
            mine6 = 0x80000000 | 5; wr(LW + 24, mine6)
stop = False
t = threading.Thread(target=visprobe); t.start()
outv = subprocess.run([exe, "-V", "-n", "200", "-d", g.name], capture_output=True, text=True, timeout=120)
stop = True; t.join()
print(outv.stdout, outv.stderr)
ok = ok and outv.returncode == 0 and a_trials == 600 and b_trials == 800 and "timed out" not in outv.stdout
ok = ok and all(k in outv.stdout for k in ("A, word written 50 us earlier", "A, written and read back first",
                                           "B, polled until visible", "B_GO arriving to the write accepted",
                                           "B, one read of word A instead", "B, polled, then word B again", "controls:",
                                           "B_GO arriving to the write accepted"))
# b_cycles 300 polled against 100 not: 200 cycles at whatever clock it measured
import re
mv = re.search(r"bridge clock ([0-9.]+) MHz", outv.stdout)
mr = re.search(r"reaches the ARM ~([0-9]+) ns", outv.stdout)
ok = ok and mv and mr and abs(int(mr.group(1)) - 200 * 1000 / float(mv.group(1))) <= 1
wr(LW + 4, 0)
outvn = subprocess.run([exe, "-V", "-n", "10", "-d", g.name], capture_output=True, text=True, timeout=60)
ok = ok and outvn.returncode == 1 and "no lw_echo behind the bridge" in outvn.stderr
outvg = subprocess.run([exe, "-V", "-n", "10"], capture_output=True, text=True, timeout=60)
ok = ok and outvg.returncode == 2 and "lwbridge" in outvg.stderr

# -P (step 2, the 68000 bus proxy) on a plain 2 MB file standing in for the
# bridge: memory order is the file's byte order, so every size must read
# back what it wrote; MAGIC and the 68000 bit are set by hand, and the
# counters stand still (VHPOSR "NOT MOVING" is the right answer here).
h = tempfile.NamedTemporaryFile(delete=False)
h.write(b"\0" * 0x200000); h.flush()
pm = mmap.mmap(h.fileno(), 0x200000)
struct.pack_into("<I", pm, 0x80000, 0x41435055)
struct.pack_into("<I", pm, 0x8000C, 0x100)
pm.flush()
outp = subprocess.run([exe, "-P", "-n", "2000", "-d", h.name], capture_output=True, text=True, timeout=120)
print(outp.stdout, outp.stderr)
# A file has no blitter, so the blitter clear is "stale" and -P reports a
# failure (exit 1); the ARM's own writes must still read back through FAST.
ok = ok and outp.returncode == 1 and "2000 random words: 0 wrong" in outp.stdout and "NOT MOVING" in outp.stdout
ok = ok and all(k in outp.stdout for k in ("read byte", "write word", "bus given back (OWN 0)", "FAST (reads",
                                           "after ARM writes: 0 of 512 stale", "blitter clear: 256 of 256 stale"))
# its 68k write of 0x11223344 at $070000 must be in the file big-endian
ok = ok and pm[0x100000 + 0x70000:0x100000 + 0x70004] == bytes([0x11, 0x22, 0x33, 0x44])
struct.pack_into("<I", pm, 0x8000C, 0); pm.flush()
outq = subprocess.run([exe, "-P", "-n", "10", "-d", h.name], capture_output=True, text=True, timeout=60)
ok = ok and outq.returncode == 1 and "not set to 68000" in outq.stderr
outr2 = subprocess.run([exe, "-P", "-n", "10"], capture_output=True, text=True, timeout=60)
ok = ok and outr2.returncode == 2 and "lwbridge" in outr2.stderr

print("answered", answered, "-> RUN:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
