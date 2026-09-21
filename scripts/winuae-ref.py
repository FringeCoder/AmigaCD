"""Measure free chip RAM in WinUAE, for comparison with the MiSTer core.

The reference machine. scripts/amiga-peek.py measures ours; this measures one
that works, so a difference becomes a number instead of an argument. See
docs/chip-ram-shortfall.md for what it found.

WinUAE runs Flink on a stock CD32 (2 MB chip, no fast RAM); our core does not.
Both machines are supposed to be the same machine, so the difference has to
show up as a number. This gets WinUAE's number the same way scripts/amiga-peek.py
gets ours: AbsExecBase -> exec MemList -> the free-chunk list.

Uses the Bartman/axewater WinUAE fork's GDB server directly over RSP rather than
through the MCP wrapper, so it is self-contained.
"""
import os
import re
import socket
import subprocess
import sys
import time

WINUAE_DIR = r"C:\Users\Admin\Documents\mcp-winuae-emu"
EXE = os.path.join(WINUAE_DIR, "winuae-gdb.exe")
CFG = os.path.join(WINUAE_DIR, "default.uae")
ROMS = r"C:\Users\Admin\Downloads\cd32 kickstart"
CHD = r"C:\Users\Admin\Downloads\Flink (1994).chd"
PORT = 2345

CONFIG = """; Generated for a stock CD32 reference measurement
config_description=CD32 reference
use_gui=no
win32.start_not_captured=yes
win32.nonotificationicon=yes
boot_rom_uae=none
cpu_speed=max
cpu_type=68ec020
cpu_model=68020
chipset=aga
chipset_compatible=CD32
cachesize=0
chipmem_size=4
fastmem_size=0
z3mem_size=0
bogomem_size=0
nr_floppies=0
floppy0type=-1
floppy1type=-1
kickstart_rom_file=%s\\kick40060.CD32
kickstart_ext_rom_file=%s\\kick40060.CD32.ext
cd32cd=true
cd32c2p=true
cd32nvram=true
cdimage0=%s
""" % (ROMS, ROMS, CHD)


class Rsp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.s.settimeout(15)
        self.buf = b""

    def _send(self, payload):
        csum = sum(payload.encode()) & 0xFF
        self.s.sendall(("$%s#%02x" % (payload, csum)).encode())

    def _read_packet(self):
        while True:
            while b"#" not in self.buf or self.buf.index(b"#") + 2 >= len(self.buf):
                chunk = self.s.recv(4096)
                if not chunk:
                    raise RuntimeError("gdb server closed the connection")
                self.buf += chunk
            start = self.buf.index(b"$") if b"$" in self.buf else 0
            end = self.buf.index(b"#", start)
            body = self.buf[start + 1:end].decode(errors="replace")
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            return body

    def cmd(self, payload):
        self._send(payload)
        # Swallow the ack if the server sends one.
        while self.buf[:1] == b"+":
            self.buf = self.buf[1:]
        return self._read_packet()

    def cont(self):
        """Start the target running and do NOT wait for a stop reply.

        The fork halts the CPU at launch and holds it until a client says go,
        which is why an early measurement read the reset-time ROM overlay at $4
        instead of AbsExecBase: nothing had executed yet.
        """
        self._send("c")

    def interrupt(self):
        self.s.sendall(b"\x03")
        try:
            return self._read_packet()
        except Exception:
            return ""

    def drain(self):
        """Swallow unsolicited packets (stop replies) so replies line up again."""
        self.s.settimeout(0.4)
        try:
            while True:
                self._read_packet()
        except Exception:
            pass
        finally:
            self.s.settimeout(15)

    def read(self, addr, length):
        want = length * 2
        # A stop reply arriving late lands in this slot and is not hex. Read on
        # until the packet actually looks like the memory we asked for; this is
        # cheaper and far more robust than trying to predict the server's
        # unsolicited traffic.
        for _ in range(4):
            r = self.cmd("m%x,%x" % (addr, length))
            if os.environ.get("RSP_DEBUG"):
                print("  m%x,%x -> %r" % (addr, length, r[:80]))
            if r.startswith("E"):
                raise RuntimeError("memory read refused at $%08X: %r" % (addr, r))
            if len(r) == want and re.fullmatch(r"[0-9a-fA-F]+", r):
                return bytes.fromhex(r)
            self.drain()
        raise RuntimeError("no usable reply for $%08X: %r" % (addr, r))

    def long(self, addr):
        return int.from_bytes(self.read(addr, 4), "big")


def walk(r):
    execbase = r.long(0x4)
    print("AbsExecBase = $%08X" % execbase)
    print("MaxLocMem   = $%08X" % r.long(execbase + 0x3E))
    mh = r.long(execbase + 0x142)
    print()
    total_free = largest = 0
    while 0 < mh < 0x200000:
        succ = r.long(mh)
        lower, upper, free = r.long(mh + 0x14), r.long(mh + 0x18), r.long(mh + 0x1C)
        if not (0 < lower < upper <= 0x200000):
            break
        print("MemHeader $%08X: $%08X-$%08X  size %7.1f KB  free %7.1f KB  used %6.1f KB"
              % (mh, lower, upper, (upper - lower) / 1024.0, free / 1024.0,
                 (upper - lower - free) / 1024.0))
        total_free += free
        chunk = r.long(mh + 0x10)
        n = 0
        while 0 < chunk < 0x200000 and n < 400:
            nxt = r.long(chunk)
            size = r.long(chunk + 4)
            largest = max(largest, size)
            chunk = nxt
            n += 1
        mh = succ
    print()
    print("free                     : %.1f KB" % (total_free / 1024.0))
    print("LARGEST CONTIGUOUS BLOCK : %.1f KB" % (largest / 1024.0))


def main():
    wait = int(sys.argv[1]) if len(sys.argv) > 1 else 75
    for f in (EXE, CHD, os.path.join(ROMS, "kick40060.CD32")):
        if not os.path.exists(f):
            sys.exit("missing: %s" % f)

    if os.path.exists(CFG) and "Generated for a stock CD32" not in open(CFG, encoding="utf-8", errors="replace").read():
        os.replace(CFG, CFG + ".bak-ref")
        print("backed up existing default.uae -> default.uae.bak-ref")
    open(CFG, "w", encoding="utf-8", newline="\r\n").write(CONFIG)

    args = [EXE, "-portable", "-G",
            "-s", "debugging_features=gdbserver",
            "-s", "debugging_trigger=",
            "-s", "floppy_speed=0"]
    print("launching winuae-gdb...")
    proc = subprocess.Popen(args, cwd=WINUAE_DIR,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(10)
        r = Rsp(PORT)
        sup = r.cmd("qSupported:swbreak+;hwbreak+")
        if os.environ.get("RSP_DEBUG"):
            print("qSupported -> %r" % sup[:120])
        r.drain()
        print("continuing, giving the CD32 %ds to boot and load Flink..." % wait)
        # This fork did not resume on a bare "c". Try the vCont form, then
        # verify by watching the ROM overlay at $0 clear -- once exec runs, $0
        # holds RAM rather than the Kickstart header.
        for form in ("vCont;c", "c"):
            r._send(form)
            time.sleep(6)
            r.drain()
            try:
                v = r.long(0x0)
            except Exception:
                v = None
            print("after %-8s $0 = %s" % (form, "%08X" % v if v is not None else "?"))
            if v is not None and v != 0x11144EF9:
                print("  overlay cleared - the machine is running")
                break
        time.sleep(wait)
        stop = r.interrupt()
        r.drain()
        if os.environ.get("RSP_DEBUG"):
            print("interrupt  -> %r" % stop[:120])
        print()
        # Is this a missing ROM or a CPU that never ran? The Kickstart header at
        # $F80000 answers it in one read: 11144EF9 means the ROM is there.
        for probe in (0xF80000, 0x0, 0x4):
            try:
                print("probe $%08X = %08X" % (probe, r.long(probe)))
            except Exception as e:
                print("probe $%08X failed: %s" % (probe, e))
        try:
            regs = r.cmd("?")
            print("stop reason: %r" % regs[:80])
        except Exception:
            pass
        print()
        walk(r)
        try:
            r.cmd("D")
        except Exception:
            pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        print("\nwinuae closed")


if __name__ == "__main__":
    main()
