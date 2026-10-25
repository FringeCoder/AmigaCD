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
print("answered", answered, "-> RUN:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
