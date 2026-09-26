#!/usr/bin/env python3
"""Interleaved A/B boot-rate harness for the MiSTer.

The boot rate of a CD32 title on this core is not stable enough to compare
two builds by running a batch of each an hour apart. Measured on 2026-09-26:
one bitstream gave 6 of 8 in the afternoon and 1 of 4 in the evening with
nothing changed but the clock. Two conclusions were drawn from batches like
that and both were wrong, in opposite directions.

So this alternates the builds within one run -- flash A, boot, flash B, boot,
repeat -- and reports each build's rate over the same window. Everything else
(binary, disc, room temperature, how long the board has been on) is shared by
construction.

    python scripts/boot-ab.py --rounds 20 \
        --core pbx=/path/AmigaCD_pbx.rbf --core cmd=/path/AmigaCD_cmd.rbf

A boot counts as successful when the game's own loader starts reading, which
shows up as `start_lba=12` in the Akiko log. That is the only reliable test:
a screenshot can be black during a normal intro fade, and the CD32 splash
looks identical whether the disc is about to load or has already given up.

Writes a CSV as it goes, so a run that is interrupted still leaves its rounds
behind. Reads nothing back from the machine except the log counters.
"""

import argparse
import csv
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REMOTE = os.path.join(HERE, "mister-remote.py")

DEFAULT_HOST = os.environ.get("MISTER_HOST", "192.168.2.103")
SSH_KEY = os.path.expanduser("~/.ssh/id_ed25519")
CORE_PATH = "/media/fat/_Test/amigacd.rbf"
BIN_PATH = "/media/fat/MiSTer_AmigaCD"
LOG = "/tmp/akiko_dbg.log"


def ssh(host, cmd, timeout=60):
    r = subprocess.run(
        ["ssh", "-o", "ConnectTimeout=10", "-i", SSH_KEY, "root@" + host, cmd],
        capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def scp(host, local, remote, timeout=300):
    subprocess.run(["scp", "-q", "-i", SSH_KEY, local, "root@%s:%s" % (host, remote)],
                   check=True, timeout=timeout)


def key(host, name, repeat=1, gap=0.35):
    for _ in range(repeat):
        subprocess.run([sys.executable, REMOTE, "--host", host, "key", name],
                       capture_output=True, text=True, timeout=30)
        time.sleep(gap)


def flash(host, rbf, settle):
    """Put a bitstream on the machine and load it."""
    scp(host, rbf, CORE_PATH + ".new")
    ssh(host, "mv %s.new %s && sync; echo load_core %s > /dev/MiSTer_cmd"
        % (CORE_PATH, CORE_PATH, CORE_PATH))
    time.sleep(settle)


def install_binary(host, elf, settle):
    """Replace the running core binary and reload so it takes effect.

    Temp name then mv, because the live binary is the file being replaced --
    writing it in place is what makes app_restart come back as "(deleted)".
    """
    scp(host, elf, BIN_PATH + ".new")
    ssh(host, "mv %s.new %s && chmod +x %s && sync; echo load_core %s > /dev/MiSTer_cmd"
        % (BIN_PATH, BIN_PATH, BIN_PATH, CORE_PATH))
    time.sleep(settle)


def mount_and_wait(host, wait):
    """Mount the disc from the OSD picker and wait for the title to load.

    Polls rather than sampling once at a fixed time. A fixed window looked
    fine until the machine slowed down mid-run on 2026-09-26 and scored six
    rounds as failures whose logs plainly contained the success marker --
    the boot had simply arrived after the deadline. Returns how long the
    title took, or None if it never got there, so a slow boot is visible as
    a slow boot rather than a hang.
    """
    ssh(host, "cp /dev/null %s" % LOG)
    key(host, "F12"); time.sleep(1.0)
    # Nudge the cursor so the mirror repaints, then land on the CD row.
    key(host, "DOWN"); key(host, "UP")
    key(host, "ENTER"); time.sleep(1.4)      # open the picker
    key(host, "ENTER"); time.sleep(2.0)      # take the highlighted disc
    key(host, "F12")                         # close the OSD

    # Success is the game's own loader reading (start_lba=12), but only when
    # THIS boot put it there: a reset that did not take leaves the previous
    # title running, and its reads would otherwise be counted as an instant
    # boot. So require the BIOS's own first read (start_lba=14) to appear
    # first, in this round's log, and the loader's read after it.
    deadline = time.time() + wait
    t0 = time.time()
    while time.time() < deadline:
        order = ssh(host, "grep -n -E 'start_lba=(14|12) ' %s | head -40" % LOG)
        seen_bios = False
        for line in order.splitlines():
            if "start_lba=14 " in line:
                seen_bios = True
            elif "start_lba=12 " in line and seen_bios:
                return time.time() - t0
        time.sleep(4)
    return None


def reset_machine(host):
    """Eject, then reset, and leave the machine on the splash.

    Reset always ejects, so the disc has to be re-picked for the next round;
    doing it explicitly keeps every round starting from the same place.
    """
    key(host, "F12"); time.sleep(1.0)
    key(host, "DOWN"); key(host, "UP")       # cursor onto the CD row
    key(host, "BACKSPACE"); time.sleep(2.0)  # eject
    key(host, "DOWN", repeat=9)              # down to Reset
    key(host, "ENTER"); time.sleep(1.2)
    key(host, "DOWN")                        # No -> Yes
    key(host, "ENTER")
    time.sleep(24)                           # splash


def measure(host):
    phantoms = int(ssh(host, "grep -c 'phantom command frame' %s" % LOG) or 0)
    checksum = int(ssh(host, "grep -c 'CHECKSUM FAIL' %s" % LOG) or 0)
    resyncs = int(ssh(host, "grep -c 'frame resync' %s" % LOG) or 0)
    return phantoms, checksum, resyncs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--core", action="append", default=[],
                    metavar="LABEL=PATH", help="bitstreams to compare")
    ap.add_argument("--binary", action="append", default=[],
                    metavar="LABEL=PATH", help="core binaries to compare")
    ap.add_argument("--fixed-core", metavar="PATH",
                    help="bitstream to install once, when comparing binaries")
    ap.add_argument("--wait", type=int, default=90,
                    help="seconds to wait for the title to load before calling it a hang")
    ap.add_argument("--settle", type=int, default=24,
                    help="seconds after load_core before touching the OSD")
    ap.add_argument("--out", default="boot-ab.csv")
    args = ap.parse_args()

    def parse(specs, what):
        out = []
        for spec in specs:
            label, _, path = spec.partition("=")
            if not path or not os.path.exists(path):
                sys.exit("%s %r: no such file" % (what, spec))
            out.append((label, path))
        return out

    cores = parse(args.core, "core")
    binaries = parse(args.binary, "binary")
    if bool(cores) == bool(binaries):
        sys.exit("give either --core (two or more) or --binary (two or more), not both")

    # Comparing binaries: the bitstream is the constant, installed once.
    if binaries and args.fixed_core:
        if not os.path.exists(args.fixed_core):
            sys.exit("--fixed-core: no such file")
        print("installing fixed bitstream %s" % os.path.basename(args.fixed_core), flush=True)
        flash(args.host, args.fixed_core, args.settle)

    variants = cores if cores else binaries
    install = flash if cores else install_binary

    tally = {label: [0, 0] for label, _ in variants}   # [good, total]

    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["round", "variant", "booted", "seconds", "phantoms",
                    "checksum_fails", "resyncs", "unix_time"])
        fh.flush()

        for rnd in range(1, args.rounds + 1):
            for label, path in variants:
                install(args.host, path, args.settle)
                took = mount_and_wait(args.host, args.wait)
                phantoms, checksum, resyncs = measure(args.host)
                booted = 0 if took is None else 1
                tally[label][0] += booted
                tally[label][1] += 1
                w.writerow([rnd, label, booted, "" if took is None else round(took, 1),
                            phantoms, checksum, resyncs, int(time.time())])
                fh.flush()
                print("round %2d  %-8s booted=%d in %-5s phantoms=%d resync=%d   (%d/%d)"
                      % (rnd, label, booted,
                         "-" if took is None else "%.0fs" % took,
                         phantoms, resyncs,
                         tally[label][0], tally[label][1]), flush=True)
                reset_machine(args.host)

    print("\n--- totals ---")
    for label, _ in variants:
        good, total = tally[label]
        pct = (100.0 * good / total) if total else 0.0
        print("%-6s %d/%d  (%.0f%%)" % (label, good, total, pct))
    print("\ncsv: %s" % args.out)


if __name__ == "__main__":
    main()
