#!/usr/bin/env python3
"""Read the running Amiga's memory over ssh, through ss_ctrl's live peek.

Turns "I think something is eating chip RAM" into a number in about thirty
seconds. Before this existed the only way to ask was to reason about it, which
on 2026-09-02 produced two confident and wrong answers in a row -- a floppy
buffer theory and a chip-size theory, both dead within a minute of measuring.

    ./scripts/amiga-peek.py mem                 free chip RAM, and the largest
                                                contiguous block
    ./scripts/amiga-peek.py read 0x780000       four longwords from an address
    ./scripts/amiga-peek.py read 0x0 --count 8  a longer dump
    ./scripts/amiga-peek.py calibrate           just the byte-order check

Pass --host to override; the default is read from the repo's `host` file.

HOW THE PEEK WORKS
------------------
minimig_ssdiag.cpp polls /tmp/ss_peek on the MiSTer. An Amiga BYTE address
written there is answered in /tmp/ss_dbg.log about a second later, as four
longwords, lowest address first. The core borrows the SDRAM CPU port for a few
microseconds to do it, so the guest sees at most a stalled bus cycle.

THREE THINGS THAT WILL MISLEAD YOU
----------------------------------
1. The log BYTE-SWAPS each 16-bit word. A displayed group AABBCCDD is $BBAADDCC
   in memory. Every read here is decoded, and calibrate() re-checks it against
   the Kickstart header on every run, because getting this wrong returns
   plausible-looking rubbish rather than an error.

2. Only SDRAM is reachable -- chip RAM, and the Kickstart image at byte
   $780000. Z2/Z3 fast RAM lives in DDR3 in this core and peeks back as ZEROS,
   not as an error. So if AbsExecBase (the longword at $4) is at or above
   $200000, exec's structures are in fast RAM and none of this can read them.
   `mem` says so rather than printing nonsense.

3. `grep -E "peek \\$"` through ssh silently matches nothing, because of how the
   shell eats the escape. Use `grep -i peek`.
"""
import argparse
import os
import re
import subprocess
import sys

LOG = "/tmp/ss_dbg.log"
TRIGGER = "/tmp/ss_peek"
CHIP_TOP = 0x200000

# struct ExecBase / MemHeader / MemChunk offsets. AmigaOS ABI, not ours.
EXECBASE_PTR   = 0x4      # AbsExecBase
EB_MAXLOCMEM   = 0x3E
EB_MEMLIST     = 0x142    # struct List; lh_Head first
MH_FIRST       = 0x10     # struct MemChunk *
MH_LOWER       = 0x14
MH_UPPER       = 0x18
MH_FREE        = 0x1C
# struct MemChunk is {mc_Next, mc_Bytes}, so a single peek at the chunk gets both.


def default_host():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(os.path.dirname(here), "host")
    try:
        with open(path) as f:
            return "root@" + f.read().strip()
    except OSError:
        return "root@192.168.1.1"


class Peek:
    def __init__(self, host, settle=3):
        self.host = host
        self.settle = settle

    def _ssh(self, cmd):
        r = subprocess.run(["ssh", "-o", "ConnectTimeout=10", self.host, cmd],
                           capture_output=True, text=True)
        return r.stdout

    def longs(self, addr):
        """Four longwords starting at Amiga byte address `addr`.

        Polls the log rather than sleeping a fixed time. ssdiag services the
        trigger on a 50 ms tick and the core needs a moment to borrow the SDRAM
        port, but the reply can also be delayed behind whatever else the OSD is
        logging -- a fixed sleep that is usually enough will fail a few peeks
        out of a hundred, which is exactly the wrong failure mode for a walk
        that makes dozens of them.
        """
        out = self._ssh(
            "n=$(wc -l < %s); echo 0x%X > %s; "
            "for i in $(seq 1 %d); do sleep 0.5; "
            "  r=$(tail -n +$((n+1)) %s | grep -i peek | head -1); "
            "  [ -n \"$r\" ] && { echo \"$r\"; break; }; done"
            % (LOG, addr, TRIGGER, self.settle * 2, LOG))
        m = re.search(r"peek \$[0-9A-Fa-f]+: ((?:[0-9A-Fa-f]{8} ?){4})", out)
        if not m:
            raise RuntimeError(
                "no peek reply for $%06X.\n"
                "  Is the core running and is ssdiag polling? %s should be growing.\n"
                "  Got: %r" % (addr, LOG, out.strip()))
        vals = []
        for group in m.group(1).split():
            hi, lo = group[:4], group[4:]
            swap = lambda w: w[2:4] + w[0:2]
            vals.append(int(swap(hi) + swap(lo), 16))
        return vals

    def long(self, addr):
        return self.longs(addr)[0]


def calibrate(p, quiet=False):
    """Kickstart's header is the one value always in a known place."""
    v = p.longs(0x780000)
    ok = v[0] == 0x11144EF9          # 512K ROM header; $1111 would be a 256K ROM
    if not quiet:
        print("calibration: $780000 = %08X %08X  %s"
              % (v[0], v[1],
                 "OK (512K Kickstart header)" if ok
                 else "*** BYTE ORDER WRONG - do not trust any reading ***"))
    return ok


def cmd_read(p, args):
    addr = int(args.address, 0)
    for i in range(0, args.count, 4):
        vals = p.longs(addr + i * 4)
        print("$%08X: %s" % (addr + i * 4, " ".join("%08X" % v for v in vals)))


def cmd_mem(p, args):
    execbase = p.long(EXECBASE_PTR)
    where = "chip RAM" if 0 < execbase < CHIP_TOP else "fast RAM (DDR3)"
    print("AbsExecBase = $%08X  (%s)" % (execbase, where))
    if not 0 < execbase < CHIP_TOP:
        print()
        print("ExecBase is not in chip RAM, so exec's memory list cannot be read:")
        print("the peek reaches SDRAM only, and fast RAM lives in DDR3.")
        print("Turn fast RAM off to make the guest's memory list measurable.")
        return 1

    print("MaxLocMem   = $%08X  (%d KB)"
          % (p.long(execbase + EB_MAXLOCMEM), p.long(execbase + EB_MAXLOCMEM) // 1024))

    mh = p.long(execbase + EB_MEMLIST)
    print()
    # Walk MemHeaders. The list tail is not a node, so stop on anything that is
    # not a plausible chip-RAM pointer rather than trusting a terminator.
    regions = []
    while 0 < mh < CHIP_TOP and len(regions) < 8:
        succ = p.long(mh)
        _, lower, upper, free = p.longs(mh + MH_FIRST)
        if not (0 < lower < upper <= CHIP_TOP):
            break
        regions.append((mh, lower, upper, free))
        print("MemHeader $%08X: $%08X-$%08X  size %7.1f KB  free %7.1f KB  used %6.1f KB"
              % (mh, lower, upper, (upper - lower) / 1024.0, free / 1024.0,
                 (upper - lower - free) / 1024.0))
        mh = succ

    if not regions:
        print("no readable MemHeaders")
        return 1

    # The number games actually check. Total free can look healthy while the
    # largest single block is far too small to load anything into.
    _, _, _, _ = regions[0]
    mh0 = regions[0][0]
    chunk = p.long(mh0 + MH_FIRST)
    largest = largest_at = count = total = 0
    while 0 < chunk < CHIP_TOP and count < 400:
        nxt, size = p.longs(chunk)[0:2]
        total += size
        if size > largest:
            largest, largest_at = size, chunk
        chunk = nxt
        count += 1

    print()
    print("free chunks              : %d, %.1f KB total" % (count, total / 1024.0))
    print("LARGEST CONTIGUOUS BLOCK : %.1f KB at $%08X" % (largest / 1024.0, largest_at))
    if largest:
        print("fragmentation            : largest block is %.0f%% of free"
              % (100.0 * largest / total if total else 0))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=None, help="root@ADDRESS of the MiSTer")
    ap.add_argument("--settle", type=int, default=4,
                    help="seconds to wait for each peek reply before giving up (default 4)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("calibrate", help="check the byte order against Kickstart")
    r = sub.add_parser("read", help="dump longwords from an address")
    r.add_argument("address")
    r.add_argument("--count", type=int, default=4, help="longwords, rounded up to 4")
    sub.add_parser("mem", help="free chip RAM and the largest contiguous block")
    args = ap.parse_args()

    p = Peek(args.host or default_host(), args.settle)

    if args.cmd == "calibrate":
        return 0 if calibrate(p) else 1

    if not calibrate(p):
        return 1
    print()
    return cmd_read(p, args) or 0 if args.cmd == "read" else cmd_mem(p, args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:
        sys.exit("error: %s" % e)
