#!/usr/bin/env python3
"""Save, cold reboot, reload, restore -- the save-state acceptance test.

The procedure this automates was written down after most of a day was lost to two
traps on 2026-08-22/23, and it has been executed by hand from a memory note ever
since. A test that depends on remembering is a test that gets skipped.

TRAP 1: A SAVE TAKES ABOUT THREE SECONDS TO REACH THE CARD.
The core captures into its DDR3 window immediately and reports "Save state:
saved", but a 1 Hz host poller writes the ~12 MB file afterwards. Resetting
between those two moments leaves the OLD file on disk and the next restore tests
a stale save -- which once produced a "fresh save still fails" result that was
really the previous file. So this waits for the mtime to move AND for the size to
stop changing AND for two consecutive CRCs to agree, and refuses to go on
otherwise.

TRAP 2: WARM TESTS CANNOT SEE MOST OF THESE BUGS.
Three separate faults were invisible in-session and after an Amiga reboot, and
appeared only after a cold FPGA reload: stale CPU cache lines, the chipset shadow
and colour table dropping most of their writes, and the shadow capture saving the
table shifted by one entry. Each hid for the same reason -- in a warm machine the
thing being restored is already nearly correct, so a restore that does almost
nothing still looks like it worked. Worse, the broken capture and the broken load
CANCELLED, so fixing only the load turned garbling into an outright hang.

Hence: the reboot here is a real `reboot`, not an Amiga reset and not a core
reload. An in-session pass means very little on its own.

    ./scripts/savestate-cold-test.py --slot 1

What it cannot do: judge the picture. Nor read back CPU registers -- A6 =
$00DFF000 is the documented sanity check for a correctly decoded state
vector, but A6 lives in the vector itself, not in memory the live peek can
reach, so checking it means decoding the .ss file rather than asking the
machine. Worth adding; not pretended at here. It checks that the save is genuinely fresh,
that the machine really went cold, that the restore was accepted, and that the
register state after restore is self-consistent. Whether the display is right
still needs eyes, and an in-game check still needs a real pad -- the companion
pad moves the OSD but games ignore it.
"""

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REMOTE = os.path.join(HERE, "mister-remote.py")
PEEK = os.path.join(HERE, "amiga-peek.py")

SSH_KEY = os.path.expanduser("~/.ssh/id_ed25519")
CORE = "/media/fat/_Test/amigacd.rbf"


def host_default():
    p = os.path.join(os.path.dirname(HERE), "host")
    try:
        with open(p) as fh:
            return fh.read().strip()
    except OSError:
        return os.environ.get("MISTER_HOST", "192.168.2.103")


def ssh(host, cmd, timeout=60, tries=3):
    argv = ["ssh", "-o", "ConnectTimeout=10", "-i", SSH_KEY, "root@" + host, cmd]
    last = None
    for attempt in range(tries):
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            # 255 is ssh itself failing to reach the machine; anything else is the
            # remote command's own status and is an answer.
            if r.returncode != 255:
                return r.stdout.strip()
            last = r.stderr.strip()
        except subprocess.TimeoutExpired as exc:
            last = str(exc)
        if attempt + 1 < tries:
            time.sleep(3)
    print("    (ssh gave up: %s)" % last)
    return ""


def key(host, name, repeat=1, gap=0.35):
    for _ in range(repeat):
        subprocess.run([sys.executable, REMOTE, "--host", host, "key", name],
                       capture_output=True, text=True, timeout=30)
        time.sleep(gap)


def ss_path(host, slot):
    """The save file for a slot, whatever the mounted title is called."""
    out = ssh(host, "ls -1 /media/fat/saves/Minimig/*.ss 2>/dev/null")
    files = [l for l in out.splitlines() if l.strip()]
    if not files:
        return None
    # Slot suffix if the naming carries one, else the only file there.
    for f in files:
        if re.search(r"[_-]%d\.ss$" % slot, f):
            return f
    return files[0] if len(files) == 1 else None


def stat_of(host, path):
    out = ssh(host, "stat -c '%%Y %%s' '%s' 2>/dev/null" % path)
    parts = out.split()
    if len(parts) != 2:
        return None, None
    return int(parts[0]), int(parts[1])


def crc_of(host, path):
    out = ssh(host, "cksum '%s' 2>/dev/null" % path)
    return out.split()[0] if out.split() else None


def wait_for_fresh_save(host, path, before_mtime, timeout=30):
    """Trap 1. The file must move, settle, and read the same twice."""
    deadline = time.time() + timeout
    last_size, stable_since = None, None
    while time.time() < deadline:
        mtime, size = stat_of(host, path)
        if mtime is None:
            time.sleep(1)
            continue
        moved = before_mtime is None or mtime > before_mtime
        if moved:
            if size == last_size:
                if stable_since is None:
                    stable_since = time.time()
                # A 12 MB write at 1 Hz: a second of no growth is not enough,
                # two consecutive equal CRCs are.
                elif time.time() - stable_since >= 1.5:
                    a = crc_of(host, path)
                    time.sleep(1.0)
                    b = crc_of(host, path)
                    if a and a == b:
                        return mtime, size, a
                    stable_since = None
            else:
                stable_since = None
            last_size = size
        time.sleep(1)
    return None, None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=host_default())
    ap.add_argument("--slot", type=int, default=1)
    ap.add_argument("--boot-wait", type=int, default=90,
                    help="seconds to wait for the machine to come back cold")
    args = ap.parse_args()
    host = args.host
    rc = 0

    print("== 1. the machine is up and the core is loaded")
    up = ssh(host, "uptime")
    if not up:
        sys.exit("cannot reach %s" % host)
    print("   %s" % up)

    path = ss_path(host, args.slot)
    before_mtime, before_size = (None, None)
    if path:
        before_mtime, before_size = stat_of(host, path)
        print("   existing save: %s (%s bytes)" % (path, before_size))
    else:
        print("   no existing save; the first one written will be taken as ours")

    print("== 2. save")
    # F6 is the save-state key in this core's OSD mapping.
    key(host, "F6")
    time.sleep(2)
    if path is None:
        path = ss_path(host, args.slot)
        if path is None:
            sys.exit("FAIL no .ss appeared after the save")

    mtime, size, crc = wait_for_fresh_save(host, path, before_mtime)
    if crc is None:
        print("FAIL the save never settled -- mtime did not move, or the size "
              "or CRC kept changing. Trap 1: the host poller had not finished "
              "writing, so a restore now would test the PREVIOUS file.")
        return 1
    print("   fresh: %s bytes, cksum %s" % (size, crc))

    print("== 3. cold reboot -- a real one, not an Amiga reset")
    ssh(host, "sync; (sleep 1; reboot) >/dev/null 2>&1 &", timeout=15)
    time.sleep(10)
    deadline = time.time() + args.boot_wait
    back = False
    while time.time() < deadline:
        if ssh(host, "echo ok", timeout=8, tries=1) == "ok":
            back = True
            break
        time.sleep(5)
    if not back:
        print("FAIL the machine did not come back within %ds" % args.boot_wait)
        return 1
    new_up = ssh(host, "uptime")
    print("   back: %s" % new_up)

    # An uptime that did not reset means we never went cold, which would make the
    # whole test a warm one -- trap 2, and the reason three faults hid for a day.
    m = re.search(r"up\s+(\d+):(\d+)", new_up or "")
    if m and (int(m.group(1)) > 0 or int(m.group(2)) > 5):
        print("FAIL uptime is %s:%s -- the machine did not actually reboot, so "
              "this would have been a warm test" % (m.group(1), m.group(2)))
        return 1

    print("== 4. the save survived the reboot unchanged")
    after_crc = crc_of(host, path)
    if after_crc != crc:
        print("FAIL cksum was %s before the reboot and %s after" % (crc, after_crc))
        rc = 1
    else:
        print("   cksum %s, unchanged" % after_crc)

    print("== 5. reload the core and restore")
    ssh(host, "echo load_core %s > /dev/MiSTer_cmd" % CORE)
    time.sleep(25)
    key(host, "F9")          # restore
    time.sleep(6)

    print("== 6. the restored state is self-consistent")
    r = subprocess.run([sys.executable, PEEK, "--host", host, "calibrate"],
                       capture_output=True, text=True, timeout=120)
    print("   calibrate: %s" % (r.stdout.strip().splitlines()[-1]
                                if r.stdout.strip() else "no output"))
    if r.returncode != 0:
        print("FAIL the live peek did not calibrate, so nothing read after a "
              "restore can be trusted")
        return 1

    print()
    if rc == 0:
        print("cold save/restore: the save was fresh, the machine went cold, the "
              "file survived, and the peek calibrates.")
        print("NOT checked: whether the picture is right, and anything in-game -- "
              "that still needs eyes and a real pad.")
    else:
        print("cold save/restore: FAILED, see above")
    return rc


if __name__ == "__main__":
    sys.exit(main())
