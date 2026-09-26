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


def mount_and_wait(host, wait):
    """Mount the disc from the OSD picker and give the title time to load."""
    ssh(host, "cp /dev/null %s" % LOG)
    key(host, "F12"); time.sleep(1.0)
    # Nudge the cursor so the mirror repaints, then land on the CD row.
    key(host, "DOWN"); key(host, "UP")
    key(host, "ENTER"); time.sleep(1.4)      # open the picker
    key(host, "ENTER"); time.sleep(2.0)      # take the highlighted disc
    key(host, "F12")                         # close the OSD
    time.sleep(wait)


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
    phase2 = int(ssh(host, "grep -c 'start_lba=12 ' %s" % LOG) or 0)
    phantoms = int(ssh(host, "grep -c 'phantom command frame' %s" % LOG) or 0)
    checksum = int(ssh(host, "grep -c 'CHECKSUM FAIL' %s" % LOG) or 0)
    return phase2, phantoms, checksum


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--core", action="append", required=True,
                    metavar="LABEL=PATH", help="repeat for each build to compare")
    ap.add_argument("--wait", type=int, default=38,
                    help="seconds to let the title load before measuring")
    ap.add_argument("--settle", type=int, default=24,
                    help="seconds after load_core before touching the OSD")
    ap.add_argument("--out", default="boot-ab.csv")
    args = ap.parse_args()

    cores = []
    for spec in args.core:
        label, _, path = spec.partition("=")
        if not path or not os.path.exists(path):
            sys.exit("core %r: no such file" % spec)
        cores.append((label, path))

    tally = {label: [0, 0] for label, _ in cores}   # [good, total]

    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["round", "core", "booted", "phantoms", "checksum_fails", "unix_time"])
        fh.flush()

        for rnd in range(1, args.rounds + 1):
            for label, path in cores:
                flash(args.host, path, args.settle)
                mount_and_wait(args.host, args.wait)
                phase2, phantoms, checksum = measure(args.host)
                booted = 1 if phase2 else 0
                tally[label][0] += booted
                tally[label][1] += 1
                w.writerow([rnd, label, booted, phantoms, checksum, int(time.time())])
                fh.flush()
                print("round %2d  %-6s booted=%d phantoms=%d checksum=%d   (%d/%d)"
                      % (rnd, label, booted, phantoms, checksum,
                         tally[label][0], tally[label][1]), flush=True)
                reset_machine(args.host)

    print("\n--- totals ---")
    for label, _ in cores:
        good, total = tally[label]
        pct = (100.0 * good / total) if total else 0.0
        print("%-6s %d/%d  (%.0f%%)" % (label, good, total, pct))
    print("\ncsv: %s" % args.out)


if __name__ == "__main__":
    main()
