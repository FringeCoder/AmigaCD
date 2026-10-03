"""Interleaved A/B over a CONFIG SETTING, with no OSD at all.

The round is: write the setting into the config, cold-load the core with no disc,
mount the disc through the binary's own `mount_cd` test hook, and score the boot
from the Akiko log. That is the boot-with-no-CD-then-mount path, the same one the
OSD route produces, and it takes about a hundred seconds a round instead of the
four to six minutes the OSD route costs -- with none of its failure modes. The
OSD was the entire reason the A/B harness could not get a round today.

Two things it does NOT do, deliberately:

  - it does not reset between rounds. load_core IS the reset, and it is a colder
    one: the FPGA is reconfigured, so every round is a first boot after a load.
  - it does not put the disc in the config. The boot starts with an empty bay and
    the disc arrives afterwards, which is what the guest sees on the OSD route.
    Putting the path in the config instead was tried and does not boot at all --
    0 of 3, at both mount delays -- so it is not a usable measurement path.

Alternates the arms within the run, like boot-ab.py, because this core's boot
rate drifts over an afternoon by more than most effects being measured.

    python mount_round.py <rounds-per-arm> <offset> <labelA=valA> <labelB=valB>
    python mount_round.py 8 7281 fast=0 2x=1          # the sector rate
    python mount_round.py 6 7280 delay0=0 delay8=8    # the mount delay
"""
import csv
import io
import os
import subprocess
import sys
import time

HOST = "192.168.2.103"
KEY = r"C:\Users\Admin\.ssh\id_ed25519"
CFGS = ("/media/fat/config/AmigaCD.cfg", "/media/fat/config/AmigaCD_CD32.cfg")
CORE = "/media/fat/_Test/amigacd.rbf"
LOG = "/tmp/akiko_dbg.log"
DISC = "/media/fat/games/AmigaCD32/Chuck Rock (1994).chd"
SIZE = 7284
SCRATCH = os.path.dirname(os.path.abspath(__file__))

# A boot takes about twenty seconds from the mount. Anything much faster is the
# previous round still resident, so it is flagged rather than trusted -- the same
# rule boot-ab.py learned the hard way.
MIN_BOOT_S = 3.0


def ssh(cmd, timeout=90):
    try:
        r = subprocess.run(["ssh", "-o", "ConnectTimeout=10", "-i", KEY,
                            "root@" + HOST, cmd],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip()
    except subprocess.TimeoutExpired:
        return ""


def fetch(remote, local):
    subprocess.run(["scp", "-q", "-i", KEY, "root@%s:%s" % (HOST, remote), local],
                   capture_output=True, timeout=120)


def put(local, remote):
    subprocess.run(["scp", "-q", "-i", KEY, local, "root@%s:%s" % (HOST, remote)],
                   capture_output=True, timeout=120)


def write_setting(offset, value):
    """Set one config byte in both profiles, and read it back before trusting it."""
    src = os.path.join(SCRATCH, "mr_src.cfg")
    fetch(CFGS[1], src)
    b = bytearray(io.open(src, "rb").read())
    if len(b) != SIZE:
        sys.exit("config is %d bytes, expected %d -- the offsets would be wrong"
                 % (len(b), SIZE))
    b[offset] = value
    out = os.path.join(SCRATCH, "mr_out.cfg")
    io.open(out, "wb").write(bytes(b))
    put(out, CFGS[0] + ".new")
    ssh("mv %s.new %s && cp %s %s && sync" % (CFGS[0], CFGS[0], CFGS[0], CFGS[1]))
    got = ssh("od -An -tu1 -j %d -N 1 %s" % (offset, CFGS[1])).strip()
    return got == str(value)


def boot_order(text):
    """True when the game's loader read AFTER the BIOS's own first read.

    Both reads matter: start_lba=12 on its own can be the previous round still
    running, and the BIOS read at 14 is what proves this boot got that far.
    """
    seen_bios = False
    for line in text.splitlines():
        if "start_lba=14 " in line:
            seen_bios = True
        elif "start_lba=12 " in line and seen_bios:
            return True
    return False


def round_once(wait=100):
    ssh("cp /dev/null %s" % LOG)
    ssh("echo load_core %s > /dev/MiSTer_cmd" % CORE)
    time.sleep(26)                                  # FPGA reconfig + binary re-exec
    ssh('echo "mount_cd %s" > /dev/MiSTer_cmd' % DISC)
    t0 = time.time()
    while time.time() - t0 < wait:
        time.sleep(6)
        order = ssh("grep -n -E 'start_lba=(14|12) ' %s | head -40" % LOG)
        if boot_order(order):
            return time.time() - t0
    return None


def main():
    per_arm = int(sys.argv[1])
    offset = int(sys.argv[2])
    arms = []
    for spec in sys.argv[3:]:
        label, _, val = spec.partition("=")
        arms.append((label, int(val)))
    if len(arms) < 2:
        sys.exit("give at least two arms, e.g. fast=0 2x=1")

    ssh("for f in %s; do [ -f $f.mrbak ] || cp $f $f.mrbak; done" % " ".join(CFGS))
    out = os.path.join(SCRATCH, "mr_%d.csv" % offset)
    tally = {l: [0, 0] for l, _ in arms}
    print("offset %d, arms %s, %d rounds each"
          % (offset, ", ".join("%s=%d" % a for a in arms), per_arm), flush=True)

    try:
        with open(out, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["round", "arm", "value", "booted", "seconds",
                        "sectors_at_pause", "prev_counts", "phantoms",
                        "checksum_fails", "suspect", "unix_time"])
            fh.flush()
            for rnd in range(1, per_arm + 1):
                for label, value in arms:
                    if not write_setting(offset, value):
                        print("round %2d  %-7s SKIPPED (setting did not land)"
                              % (rnd, label), flush=True)
                        continue
                    took = round_once()
                    # Keep the whole log: the sector counts this run exists to
                    # compare are only in there, and a round scored without its
                    # log cannot be re-examined.
                    tag = os.path.join(SCRATCH, "mr_%s_r%d.log" % (label, rnd))
                    fetch(LOG, tag)
                    body = io.open(tag, encoding="utf-8", errors="replace").read()
                    at_pause = [l.split("read: ")[1].split(" sectors")[0]
                                for l in body.splitlines()
                                if "PAUSE during a data read:" in l]
                    prevs = [l.split("prev=")[1].strip()
                             for l in body.splitlines() if "prev=" in l]
                    phantoms = body.count("[akiko] PHANTOM")
                    checksum = body.count("CHECKSUM FAIL")
                    booted = 1 if took else 0
                    suspect = ("boot-too-fast" if took and took < MIN_BOOT_S else "")
                    tally[label][0] += booted
                    tally[label][1] += 1
                    w.writerow([rnd, label, value, booted,
                                "" if took is None else round(took, 1),
                                ";".join(at_pause), ";".join(prevs),
                                phantoms, checksum, suspect, int(time.time())])
                    fh.flush()
                    print("round %2d  %-7s booted=%d in %-5s pause_sectors=%-10s "
                          "prev=%-14s phantom=%d cksum=%d  (%d/%d)%s"
                          % (rnd, label, booted,
                             "-" if took is None else "%.0fs" % took,
                             ",".join(at_pause) or "-", ",".join(prevs) or "-",
                             phantoms, checksum,
                             tally[label][0], tally[label][1],
                             "  SUSPECT: " + suspect if suspect else ""), flush=True)
    finally:
        ssh("for f in %s; do [ -f $f.mrbak ] && mv $f.mrbak $f; done; sync"
            % " ".join(CFGS))
        print("")
        print("--- totals ---")
        for label, _ in arms:
            good, total = tally[label]
            print("%-7s %d/%d" % (label, good, total))
        print("configs restored", flush=True)


if __name__ == "__main__":
    main()
