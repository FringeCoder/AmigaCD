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

print("answered", answered, "-> RUN:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
