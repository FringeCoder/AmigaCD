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

Boot the title first, and leave it somewhere the machine cannot reach by itself
in ten seconds, so that a restored picture means something. Then:

    ./scripts/savestate-cold-test.py
    ./scripts/savestate-cold-test.py --core /media/fat/_Test/fmv/amigacd.rbf --shot out.png

The title comes from the disc Main has open, or from --title. A save
already in the slot is kept beside it and put back at the end, so the test leaves
the card as it found it; --keep leaves the test's save instead.

What it cannot do: judge the picture, though --shot takes one. Nor read back CPU
registers -- A6 = $00DFF000 is the documented sanity check for a correctly decoded
state vector, but A6 lives in the vector itself, not in memory the live peek can
reach, so checking it means decoding the .ss file rather than asking the machine.
Worth adding; not pretended at here. It checks that the save is genuinely fresh,
that the machine really went cold, that the core reported the restore, and that
chip RAM matches the file. Pierre le Chef passes all of that with the wrong
colours on screen (docs/fmv-plan.md, run v40), so a pass here is not a good
picture. An in-game check still needs a real pad -- the companion pad moves the
OSD but games ignore it.
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
SS_DIR = "/media/fat/savestates/AmigaCD"
CONFIG = "/media/fat/config/AmigaCD_CD32.cfg"
CONFIG_DISC_OFFSET = 1052      # the disc path in the config, NUL terminated
SAVE_KEYS = ("LEFTCTRL", "LEFTALT", "F1")
RESTORE_KEYS = ("LEFTCTRL", "LEFTALT", "F2")
RESTORE_AFTER = 10             # seconds from load_core: before any title has booted
CHIP_OFFSET = 0x76C            # a chip RAM address sits this far into a .ss
# Spread over chip RAM. $4 is ExecBase and must match; the others can be live data
# in a running title, which is why most of them, not all, have to.
CHIP_PROBES = (0x4, 0x1000, 0x20000, 0x80000, 0x100000, 0x1F0000)


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


def chord(host, keys):
    subprocess.run([sys.executable, REMOTE, "--host", host, "chord"] + list(keys),
                   capture_output=True, text=True, timeout=30)


def title_of(host):
    """The title the save file will be named after: the disc Main has open.

    Not the disc in the config -- a disc picked from the OSD this session never
    reaches it, and a save lands under the mounted title whatever the config says.
    """
    out = ssh(host, "ls -l /proc/[0-9]*/fd 2>/dev/null | grep -o '/media/fat/games/.*[.]chd' | head -1")
    if not out:
        out = ssh(host, "dd if=%s bs=1 skip=%d count=256 2>/dev/null | tr '\000' '\n' | head -1"
                  % (CONFIG, CONFIG_DISC_OFFSET))
    return os.path.splitext(out.split("/")[-1])[0] if out else None


def load_core(host, core):
    """Load the core and wait until a Main is running it.

    A load that does not take says nothing, and every later step then reads
    nothing, so stop here instead. From Git Bash, MSYS_NO_PATHCONV=1 keeps --core
    from turning into a Windows path.
    """
    for _ in range(6):
        ssh(host, "echo load_core %s > /dev/MiSTer_cmd" % core)
        for _ in range(5):
            time.sleep(2)
            if core in ssh(host, "ps | grep MiSTer | grep -v grep"):
                return True
    return False


def log_lines(host):
    out = ssh(host, "wc -l < /tmp/ss_dbg.log 2>/dev/null")
    return int(out) if out.isdigit() else 0


def core_said(host, text, since_lines):
    """Did ss_dbg.log gain this outcome after its first since_lines lines?"""
    return ssh(host, "tail -n +%d /tmp/ss_dbg.log 2>/dev/null | grep -c '%s'"
               % (since_lines + 1, text)) not in ("", "0")


def chip_matches(host, path):
    """Peek each probe and compare with the file. Returns matches and whether $4 did."""
    same, exec_base = 0, False
    for addr in CHIP_PROBES:
        r = subprocess.run([sys.executable, PEEK, "--host", "root@" + host, "read", hex(addr)],
                           capture_output=True, text=True, timeout=60)
        m = re.search(r"^\$[0-9A-Fa-f]+:((?:\s+[0-9A-Fa-f]{8})+)", r.stdout, re.M)
        live = "".join(m.group(1).split()).lower() if m else ""
        saved = ssh(host, "dd if='%s' bs=1 skip=%d count=%d 2>/dev/null | od -An -tx1 | tr -d ' \\n'"
                    % (path, addr + CHIP_OFFSET, len(live) // 2))
        ok = bool(live) and live == saved
        print("   $%06X %s" % (addr, "matches" if ok else "live %s, file %s" % (live, saved)))
        same += ok
        exec_base = exec_base or (ok and addr == 0x4)
    return same, exec_base


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


def test(host, args, path, before_mtime):
    rc = 0

    print("== 2. save")
    chord(host, SAVE_KEYS)
    time.sleep(2)

    mtime, size, crc = wait_for_fresh_save(host, path, before_mtime)
    if crc is None:
        print("FAIL the save never settled -- mtime did not move, or the size "
              "or CRC kept changing. Trap 1: the host poller had not finished "
              "writing, so a restore now would test the PREVIOUS file. A wrong "
              "--slot or --title looks the same.")
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

    print("== 5. reload the core and restore, before the title can boot")
    if not load_core(host, args.core):
        print("FAIL Main never took %s" % args.core)
        return 1
    time.sleep(RESTORE_AFTER)
    lines = log_lines(host)
    chord(host, RESTORE_KEYS)
    time.sleep(6)
    if core_said(host, "State restored", lines):
        print("   the core says: State restored")
    elif ssh(host, "ls /tmp/ss_dbg.log 2>/dev/null"):
        print("FAIL the core did not report a restore")
        rc = 1
    else:
        print("   no /tmp/ss_dbg.log from this Main, so the restore is unconfirmed")

    print("== 6. chip RAM matches the file")
    same, exec_base = chip_matches(host, path)
    if not exec_base or same * 2 <= len(CHIP_PROBES):
        print("FAIL %d of %d probes match, $4 %s"
              % (same, len(CHIP_PROBES), "among them" if exec_base else "not among them"))
        rc = 1

    print("== 7. the live peek still calibrates")
    r = subprocess.run([sys.executable, PEEK, "--host", "root@" + host, "calibrate"],
                       capture_output=True, text=True, timeout=120)
    print("   calibrate: %s" % (r.stdout.strip().splitlines()[-1]
                                if r.stdout.strip() else "no output"))
    if r.returncode != 0:
        print("FAIL the live peek did not calibrate, so nothing read after a "
              "restore can be trusted")
        rc = 1

    if args.shot:
        r = subprocess.run([sys.executable, REMOTE, "--host", host, "shot", args.shot],
                           capture_output=True, text=True, timeout=60)
        print("   shot: %s" % (r.stdout.strip() or r.stderr.strip()))
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=host_default())
    ap.add_argument("--slot", type=int, default=1,
                    help="the slot the machine is on; this does not change it")
    ap.add_argument("--core", default=CORE, help="the .rbf to load after the reboot")
    ap.add_argument("--title", help="default: the disc Main has open")
    ap.add_argument("--keep", action="store_true",
                    help="leave the test's save, not the one that was there")
    ap.add_argument("--shot", help="take a screenshot after the restore, into this file")
    ap.add_argument("--boot-wait", type=int, default=90,
                    help="seconds to wait for the machine to come back cold")
    args = ap.parse_args()
    host = args.host

    print("== 1. the machine is up and the core is loaded")
    up = ssh(host, "uptime")
    if not up:
        sys.exit("cannot reach %s" % host)
    print("   %s" % up)

    title = args.title or title_of(host)
    if not title:
        sys.exit("no disc mounted; name the title with --title")
    path = "%s/%s_%d.ss" % (SS_DIR, title, args.slot)
    png = os.path.splitext(path)[0] + ".png"
    before_mtime, before_size = stat_of(host, path)
    had_save = before_mtime is not None
    if had_save:
        ssh(host, "cp -p '%s' '%s.coldtest'; cp -p '%s' '%s.coldtest' 2>/dev/null"
            % (path, path, png, png))
        print("   existing save kept as %s.coldtest (%s bytes)" % (path, before_size))
    else:
        print("   no save in the slot yet: %s" % path)

    try:
        rc = test(host, args, path, before_mtime)
    finally:
        # Also on a failure: the slot is the user's, not this test's.
        if args.keep:
            print("   the test's save stays in the slot")
        elif had_save:
            ssh(host, "mv '%s.coldtest' '%s'; mv '%s.coldtest' '%s' 2>/dev/null; sync"
                % (path, path, png, png))
            print("   the earlier save is back in the slot")
        else:
            ssh(host, "rm -f '%s' '%s'; sync" % (path, png))
            print("   the test's save removed")

    print()
    if rc == 0:
        print("cold save/restore: the save was fresh, the machine went cold, the "
              "file survived, the core restored it, and chip RAM matches.")
        print("NOT checked: whether the picture is right, and anything in-game -- "
              "that still needs eyes and a real pad.")
    else:
        print("cold save/restore: FAILED, see above")
    return rc


if __name__ == "__main__":
    sys.exit(main())
