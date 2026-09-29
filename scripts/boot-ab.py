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

# A cold boot of this title takes five seconds at the very fastest; anything
# quicker is the previous round still resident. The freshness check (a BIOS read
# before the loader's read) catches most of that, but not the case where
# remounting makes the BIOS rescan while the old title is still running -- so
# treat a very fast "boot" as unscoreable rather than as a success.
MIN_BOOT_S = 4.0

# There is no plausibility test on the phantom count, and the first attempt at
# one was wrong in a way worth writing down. Rounds with 128 phantom drains
# against the one or two others show looked pathological, so they were discarded
# -- until the log line count went into the CSV and showed 128 phantoms landing
# on exactly the rounds that BOOTED (293 lines) while single-phantom rounds were
# the ones that hung (112 lines). The count scales with how long the machine kept
# running, so a high one is a symptom of success, not of a broken measurement.
# Discarding it threw away the successes.
#
# The lesson generalised: flag a round, never silently drop it. Everything is
# counted, the totals are reported both ways, and the analysis decides.
CDTV_LOG = "/tmp/cdtv_dbg.log"


# The link to the machine drops occasionally -- "scp: Connection closed" in the
# middle of a copy, or an ssh that times out while the core is reloading. An
# hour-long run should not die of one: the first version of this did exactly
# that, four rounds in, and the traceback was the only thing left of it.
# Only a transport failure is worth retrying. ssh exits 255 when it cannot
# reach the machine; anything else is the remote command's own status, and
# "grep found nothing" (exit 1) is an answer, not an error -- retrying it four
# times just burns twelve seconds per measurement.
def _run(argv, timeout, tries=4):
    last = None
    for attempt in range(tries):
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            if r.returncode != 255:
                return r
            last = "exit 255: %s" % r.stderr.strip()
        except subprocess.TimeoutExpired as exc:
            last = exc
        if attempt + 1 < tries:
            time.sleep(3 * (attempt + 1))
    print("      (giving up on %s after %d tries: %s)"
          % (argv[0], tries, last), flush=True)
    return None


def ssh(host, cmd, timeout=60):
    r = _run(["ssh", "-o", "ConnectTimeout=10", "-i", SSH_KEY, "root@" + host, cmd],
             timeout)
    return r.stdout.strip() if r else ""


def scp(host, local, remote, timeout=300):
    r = _run(["scp", "-q", "-i", SSH_KEY, local, "root@%s:%s" % (host, remote)], timeout)
    return r is not None and r.returncode == 0


def key(host, name, repeat=1, gap=0.35):
    for _ in range(repeat):
        subprocess.run([sys.executable, REMOTE, "--host", host, "key", name],
                       capture_output=True, text=True, timeout=30)
        time.sleep(gap)


def flash(host, rbf, settle):
    """Put a bitstream on the machine and load it."""
    if not scp(host, rbf, CORE_PATH + ".new"):
        return False
    ssh(host, "mv %s.new %s && sync; echo load_core %s > /dev/MiSTer_cmd"
        % (CORE_PATH, CORE_PATH, CORE_PATH))
    time.sleep(settle)
    return True


def install_binary(host, elf, settle):
    """Replace the running core binary and reload so it takes effect.

    Temp name then mv, because the live binary is the file being replaced --
    writing it in place is what makes app_restart come back as "(deleted)".
    """
    if not scp(host, elf, BIN_PATH + ".new"):
        return False

    # Put it in place and let the card settle BEFORE asking for a reload.
    #
    # app_restart() execl()s the binary it is replacing, and fpga_io.cpp's
    # grandchild falls straight through to reboot(1) when that execl fails --
    # so a file the FAT driver has not finished committing costs a hardware
    # reboot, not a failed round. The board did exactly that twice on
    # 2026-09-27 during a run that swapped binaries every ninety seconds.
    ssh(host, "mv %s.new %s && chmod +x %s && sync" % (BIN_PATH, BIN_PATH, BIN_PATH))
    time.sleep(3)
    size = ssh(host, "stat -c %%s %s 2>/dev/null" % BIN_PATH)
    if not size or size == "0":
        print("      (binary did not land, skipping round)", flush=True)
        return False
    ssh(host, "echo load_core %s > /dev/MiSTer_cmd" % CORE_PATH)
    time.sleep(settle)
    return True


def mount_and_wait(host, wait, expect_disc=None):
    """Mount the disc from the OSD picker and wait for the title to load.

    Polls rather than sampling once at a fixed time. A fixed window looked
    fine until the machine slowed down mid-run on 2026-09-26 and scored six
    rounds as failures whose logs plainly contained the success marker --
    the boot had simply arrived after the deadline. Returns how long the
    title took, or None if it never got there, so a slow boot is visible as
    a slow boot rather than a hang.
    """
    # Clear the Akiko log and check the clear took. A round scored against a log
    # left from the previous round is not a measurement, and the ssh that clears
    # it can fail silently -- ssh() returns "" on a transport failure.
    ssh(host, "cp /dev/null %s" % LOG)
    left = ssh(host, "wc -l < %s 2>/dev/null || echo -1" % LOG).strip()
    if left != "0":
        print("      (log did not clear: %r, skipping round)" % left, flush=True)
        return None, False
    # How far the mount log had got before this round, so a mount from an
    # earlier round is never mistaken for this one's.
    before = int(ssh(host, "grep -c 'set_cd_path: /media' %s" % CDTV_LOG) or 0)

    # The OSD picker takes "the highlighted disc", and the highlight drifts.
    # Counting set_cd_path lines proves SOMETHING mounted, not that it was the
    # title under test -- and a different game boots happily, satisfying the
    # freshness check and scoring as a success for whichever build was loaded.
    # Nothing in this harness would have noticed. Checking the path closes it.
    mounted = False
    wrong_disc = None
    for attempt in (1, 2):
        key(host, "F12"); time.sleep(1.0)
        # Nudge the cursor so the mirror repaints, then land on the CD row.
        key(host, "DOWN"); key(host, "UP")
        key(host, "ENTER"); time.sleep(1.4)      # open the picker
        key(host, "ENTER"); time.sleep(2.0)      # take the highlighted disc
        key(host, "F12")                         # close the OSD
        time.sleep(2.0)
        now = int(ssh(host, "grep -c 'set_cd_path: /media' %s" % CDTV_LOG) or 0)
        if now > before:
            if expect_disc:
                # The last path the core actually mounted.
                got = ssh(host, "grep -o 'set_cd_path: /media.*' %s | tail -1"
                          % CDTV_LOG)
                if expect_disc.lower() not in got.lower():
                    wrong_disc = got
                    print("      (mounted the wrong disc: %r, wanted %r)"
                          % (got, expect_disc), flush=True)
                    break
            mounted = True
            break
        # The OSD key sequence drifts out of step now and then -- when it does,
        # nothing is mounted and nothing can boot. Scoring that as a failed
        # boot is how a whole run of both builds came back 0 on 2026-09-26
        # with an empty Akiko log. Close whatever is open and try once more.
        print("      (mount did not take, retrying)", flush=True)
        key(host, "ESC"); time.sleep(0.6)
        key(host, "F12"); time.sleep(1.0)

    if not mounted:
        # A wrong disc is not a failed boot and not a flaky mount: it is a round
        # that measured the wrong thing, so it is skipped either way.
        return None, False
    if wrong_disc:
        return None, False

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
                return time.time() - t0, True
        time.sleep(4)
    return None, True


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
    # Carried into the CSV because it is the cheapest way to see, after the
    # fact, that a round was scored against a log that was not this round's.
    lines = int(ssh(host, "wc -l < %s" % LOG) or 0)
    return phantoms, checksum, resyncs, lines


# Fisher's exact test, two sided, on the 2x2 of booted against not booted.
#
# Here because the alternative is computing it by hand afterwards, which happened
# four times in three days -- and three of the boot rates quoted in this project
# had to be retracted. Two of those retractions were not arithmetic errors: a
# difference was called real when the sample could not support it. A harness that
# reports a rate and leaves significance to the reader invites exactly that.
#
# Exact rather than chi-square because the counts are tiny: fourteen rounds an arm
# at best, and cells of zero are normal.
def _fisher_two_sided(a, b, c, d):
    """a,b = first arm booted/failed; c,d = second arm booted/failed."""
    from math import comb

    def p_table(w, x, y, z):
        n = w + x + y + z
        if n == 0:
            return 1.0
        return (comb(w + x, w) * comb(y + z, y)) / comb(n, w + y)

    observed = p_table(a, b, c, d)
    row1, row2, col1 = a + b, c + d, a + c
    total = 0.0
    for w in range(row1 + 1):
        x = row1 - w
        y = col1 - w
        z = row2 - y
        if y < 0 or z < 0:
            continue
        pt = p_table(w, x, y, z)
        # Everything at least as extreme as what was seen.
        if pt <= observed * (1 + 1e-9):
            total += pt
    return min(1.0, total)


def report_significance(variants, tally):
    """Only meaningful for a two-arm comparison, which is what this tool is for."""
    if len(variants) != 2:
        return
    la = variants[0][0]
    lb = variants[1][0]
    ga, ta = tally[la]
    gb, tb = tally[lb]
    if ta == 0 or tb == 0:
        return
    p = _fisher_two_sided(ga, ta - ga, gb, tb - gb)
    print("")
    print("Fisher exact, two sided: p = %.6f" % p)
    if p < 0.01:
        print("  a difference this large is unlikely to be chance")
    elif p < 0.05:
        print("  suggestive, and thin -- another run before quoting it")
    else:
        print("  NOT significant: this sample does not support calling the arms "
              "different")
    # The thing that actually went wrong before: a rate quoted off too few rounds.
    if min(ta, tb) < 10:
        print("  note: %d and %d scoreable rounds. Under ten an arm, even a clear "
              "p is worth repeating." % (ta, tb))


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
    ap.add_argument("--expect-disc", metavar="SUBSTRING",
                    help="require the mounted path to contain this, so a drifted "
                         "OSD picking another title is skipped rather than scored")
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

    tally = {label: [0, 0] for label, _ in variants}    # [good, total]
    flagged = {label: [0, 0] for label, _ in variants}  # the flagged subset
    suspect_count = {label: 0 for label, _ in variants}

    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["round", "variant", "booted", "seconds", "phantoms",
                    "checksum_fails", "resyncs", "log_lines", "suspect",
                    "unix_time"])
        fh.flush()

        for rnd in range(1, args.rounds + 1):
            for label, path in variants:
                if not install(args.host, path, args.settle):
                    print("round %2d  %-8s SKIPPED (install failed)" % (rnd, label),
                          flush=True)
                    continue
                took, mounted = mount_and_wait(args.host, args.wait,
                                               args.expect_disc)
                if not mounted:
                    # Not a result: the disc never went in. Leave it out of the
                    # tally rather than scoring a boot that was never attempted.
                    print("round %2d  %-8s SKIPPED (no mount)" % (rnd, label), flush=True)
                    w.writerow([rnd, label, "", "", "", "", "", "",
                                "no-mount", int(time.time())])
                    fh.flush()
                    reset_machine(args.host)
                    continue
                phantoms, checksum, resyncs, lines = measure(args.host)
                booted = 0 if took is None else 1

                # Flag, do not drop. Only a success can be implausibly fast; a
                # failure waited out the whole window either way.
                suspect = ""
                if booted and took is not None and took < MIN_BOOT_S:
                    suspect = "boot-too-fast"

                tally[label][0] += booted
                tally[label][1] += 1
                if suspect:
                    suspect_count[label] += 1
                    flagged[label][0] += booted
                    flagged[label][1] += 1

                w.writerow([rnd, label, booted, "" if took is None else round(took, 1),
                            phantoms, checksum, resyncs, lines, suspect,
                            int(time.time())])
                fh.flush()
                print("round %2d  %-8s booted=%d in %-5s phantoms=%d resync=%d lines=%d   (%d/%d)%s"
                      % (rnd, label, booted,
                         "-" if took is None else "%.0fs" % took,
                         phantoms, resyncs, lines,
                         tally[label][0], tally[label][1],
                         "  SUSPECT: " + suspect if suspect else ""), flush=True)
                reset_machine(args.host)

    print("")
    print("--- totals ---")
    for label, _ in variants:
        good, total = tally[label]
        pct = (100.0 * good / total) if total else 0.0
        print("%-6s %d/%d  (%.0f%%)" % (label, good, total, pct))

    # Same numbers with the flagged rounds taken out, so a reader can see
    # whether the flags carry the result rather than being told they do not.
    if any(suspect_count.values()):
        print("")
        print("--- totals excluding flagged rounds ---")
        for label, _ in variants:
            good = tally[label][0] - flagged[label][0]
            total = tally[label][1] - flagged[label][1]
            pct = (100.0 * good / total) if total else 0.0
            print("%-6s %d/%d  (%.0f%%)   [%d flagged]"
                  % (label, good, total, pct, suspect_count[label]))
    report_significance(variants, tally)
    print("")
    print("csv: %s" % args.out)


if __name__ == "__main__":
    main()
