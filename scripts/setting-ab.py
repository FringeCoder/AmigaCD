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

    python setting-ab.py <rounds-per-arm> <offset> <armA> <armB>
    python setting-ab.py 8 7281 fast=0 2x=1           # the sector rate
    python setting-ab.py 6 7280 delay0=0 delay8=8     # the mount delay
    python setting-ab.py 8 7281 flash noflash         # the flash itself
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


# There are TWO paths into a working boot, and scoring only one of them cost a
# whole conclusion.
#
# The familiar path opens at lba 14, the CD32 boot block. But when the disc is
# announced early the ROM opens at lba 16, the ISO primary volume descriptor,
# mounts the disc as a data volume and loads the game from there -- a different
# route to the same place, and it reaches the loader at lba 12 and the game data
# exactly like the other one.
#
# Requiring lba 14 first therefore scored every one of those as a FAILURE. That
# is how "a 0 ms announce never boots, 0 of 10" was reported here, when three
# rounds checked by hand booted 3 of 3 with the loader read plainly in the log.
#
# What actually distinguishes a boot is the loader read happening after SOME
# opening read in this round's log, and the log is cleared at the start of each
# round, so anything in it belongs to this round.
def boot_order(text):
    """True when the game's loader read follows an opening read in this round."""
    opened = False
    for line in text.splitlines():
        if "start_lba=14 " in line or "start_lba=16 " in line:
            opened = True
        elif "start_lba=12 " in line and opened:
            return True
    return False


# Copy the bitstream back over itself before loading.
#
# "First boot after a flash" is the failure this project has been chasing, and
# a plain load_core is NOT that: the .rbf is already on the card and in the
# card's cache. The flash arm does what a deploy does -- writes the file, syncs,
# then loads -- so the two arms differ only in whether the bitstream was just
# written. Same bytes either way, verified by md5, so a difference cannot be the
# contents of the core.
def flash_core():
    before = ssh("md5sum %s | cut -d\" \" -f1" % CORE)
    ssh("cp %s %s.reflash && mv %s.reflash %s && sync" % (CORE, CORE, CORE, CORE))
    after = ssh("md5sum %s | cut -d\" \" -f1" % CORE)
    return before == after and bool(after)


def round_once(wait=100, reflash=False):
    if reflash and not flash_core():
        print("      (reflash did not verify; round not scored)", flush=True)
        return None
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
    # An arm is either a config value (label=N) or the word "flash" /
    # "noflash", which leaves the setting alone and varies whether the
    # bitstream is rewritten before the load.
    arms = []
    for spec in sys.argv[3:]:
        if spec in ("flash", "noflash"):
            arms.append((spec, None))
            continue
        label, _, val = spec.partition("=")
        arms.append((label, int(val)))
    if len(arms) < 2:
        sys.exit("give at least two arms, e.g. fast=0 2x=1")

    ssh("for f in %s; do [ -f $f.mrbak ] || cp $f $f.mrbak; done" % " ".join(CFGS))
    out = os.path.join(SCRATCH, "mr_%d.csv" % offset)
    tally = {l: [0, 0] for l, _ in arms}
    print("offset %d, arms %s, %d rounds each"
          % (offset, ", ".join(l if v is None else "%s=%d" % (l, v)
                               for l, v in arms), per_arm), flush=True)

    try:
        with open(out, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["round", "arm", "value", "booted", "seconds",
                        "sectors_at_pause", "prev_counts", "phantoms",
                        "checksum_fails", "suspect", "unix_time"])
            fh.flush()
            for rnd in range(1, per_arm + 1):
                for label, value in arms:
                    if value is not None and not write_setting(offset, value):
                        print("round %2d  %-7s SKIPPED (setting did not land)"
                              % (rnd, label), flush=True)
                        continue
                    took = round_once(reflash=(label == "flash"))
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
                    w.writerow([rnd, label, "" if value is None else value, booted,
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
