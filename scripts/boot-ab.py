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
import re
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


# ---------------------------------------------------------------- the OSD
#
# Everything below replaces navigating by counting key presses. The binary
# mirrors every OSD paint to /tmp/osd.txt while /tmp/osd_mirror exists
# (osd.cpp, osd_mirror_flush), marking the selected row `>` and a greyed one
# `~`. So the menu can be READ, and there is no reason to guess where the
# cursor is.
#
# Blind navigation cost real measurements. `DOWN` nine times to reach Reset
# assumes a page layout; when it drifted, one run scored every round of both
# builds as a failed boot with an empty Akiko log, and on another occasion an
# ENTER landed on the System row, switched the machine to CDTV and wedged the
# core. Three separate guards were added to DETECT that damage afterwards --
# retry on no-mount, check set_cd_path, check the disc name. Reading the cursor
# removes the cause instead.
OSD_MIRROR = "/tmp/osd_mirror"
OSD_TEXT = "/tmp/osd.txt"


def osd_enable(host):
    """Turn the mirror on. Harmless if it is already on."""
    ssh(host, "touch %s" % OSD_MIRROR)


# The real format, read off the machine rather than assumed:
#
#     [AmigaCD]
#      0
#      1 >  CD       : * no disc *
#      2    Real CD  : mount drive
#      3 ~  Floppy 0 : * no disk *
#     ...
#     14    Reset
#     15               exit
#
# A leading row NUMBER, then the marker column -- `>` selected, `~` greyed,
# space otherwise -- then the text. The first line is a title, not a row.
OSD_ROW = re.compile(r"^\s*(\d+)\s(.)\s*(.*?)\s*$")


# F12 is a TOGGLE, and the harness used to press it as though it were "open".
#
# When the OSD was already open -- which it is whenever a round bailed out
# through a path that pressed F12 an odd number of times -- that press CLOSED
# it, and every key after it went to the running game instead of the menu.
# Nothing mounted, and the symptom was "Chuck not on the picker page" or a bare
# "mount did not take", which reads like OSD drift and is not: the menu was
# never up. Worse, the keys still land somewhere, so a stray ENTER arrives at
# whatever the game does with it.
#
# The mirror already answers the question -- it writes the literal "[closed]"
# when the OSD is down -- so the state is checked rather than assumed. osd_rows
# cannot tell "[closed]" from "not painted yet", which is why that distinction
# lives here.
def osd_raw(host):
    """The mirror file as written, title line included."""
    return ssh(host, "cat %s 2>/dev/null" % OSD_TEXT)


# Wait for a NEW page rather than sleeping a guessed interval.
#
# The mirror is written on a PAINT, so a read taken between the keypress and the
# paint returns the PREVIOUS page -- and every decision made from it is made
# about a page that is no longer on screen. That is the whole of the OSD
# flakiness here: a picker read too early looks like the main menu, so the disc
# "is not on the page"; a main menu read too early looks like the picker, so
# ENTER lands on whatever row the stale page called selected. Fixed sleeps made
# it rarer and could not remove it.
def osd_wait_page(host, before, timeout=6.0):
    """True once the mirror differs from `before`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if osd_raw(host) != before:
            time.sleep(0.25)          # let the paint finish
            return True
        time.sleep(0.3)
    return False


def osd_is_open(host):
    return "[closed]" not in ssh(host, "cat %s 2>/dev/null" % OSD_TEXT)


def osd_ensure(host, want_open, tries=3):
    """Get the OSD to the wanted state, pressing F12 only when it is wrong."""
    for _ in range(tries):
        if osd_is_open(host) == want_open:
            return True
        key(host, "F12")
        time.sleep(1.2)
    state = "open" if want_open else "closed"
    print("      (could not get the OSD %s)" % state, flush=True)
    return False


def osd_rows(host):
    """[(row_number, text, selected)], or [] if nothing has been painted yet.

    The file is only written on a paint, so a freshly created marker leaves it
    empty until the next one -- callers provoke a paint and retry rather than
    treating empty as a real page.
    """
    raw = ssh(host, "cat %s 2>/dev/null" % OSD_TEXT)
    rows = []
    for line in raw.splitlines():
        m = OSD_ROW.match(line)
        if m:
            rows.append((int(m.group(1)), m.group(3), m.group(2) == ">"))
    return rows


def osd_find(rows, want):
    """Row number of the first row containing `want`, or -1.

    Matches on the text only, so a page title cannot be mistaken for a row --
    which matters here, because the title is literally "[AmigaCD]" and a naive
    search for "CD" would find it first.
    """
    for num, text, _ in rows:
        if want.lower() in text.lower():
            return num
    return -1


def osd_selected(rows):
    for num, _, sel in rows:
        if sel:
            return num
    return -1


def osd_goto(host, want, max_steps=24):
    """Move the cursor onto the row named `want`. True when it is there.

    One step at a time, re-reading after each. That is deliberate and it is the
    whole point of this function: the cursor SKIPS greyed and blank rows, and
    which rows are greyed depends on machine state. On this page rows 3 to 6 are
    greyed because no floppy or hard disk is attached -- attach one and they
    become selectable, so any fixed number of presses lands somewhere different.
    The old code pressed DOWN nine times from the CD row to reach Reset; Reset is
    row 14, and how many presses that actually takes depends on what is greyed
    at the time. That is the drift that voided one whole run and, on another
    occasion, put an ENTER on the System row and switched the machine to CDTV.
    """
    for _ in range(max_steps):
        rows = osd_rows(host)
        if not rows:
            key(host, "DOWN"); key(host, "UP")   # provoke a paint
            time.sleep(0.3)
            continue
        target = osd_find(rows, want)
        if target < 0:
            return False                          # not on this page at all
        cur = osd_selected(rows)
        if cur == target:
            return True
        if cur < 0:
            key(host, "DOWN"); key(host, "UP")
            time.sleep(0.3)
            continue
        key(host, "DOWN" if target > cur else "UP")
        time.sleep(0.25)
    return False


# The picker opens where it was last left, and that is not a property of this
# harness: another session browsing for a test image moves it, and it had moved
# to a directory holding nothing but subdirectories. "Take whatever is
# highlighted" then pressed ENTER on `..` and walked UP a level, mounting
# nothing -- which the run reported as a flaky mount, every round, with no hint
# that the picker was somewhere else entirely.
#
# So the file is found by name, descending into `dir_name` when it is on the
# page and going up through `..` when it is not. Bounded, because a picker that
# shows neither is a page this does not understand, and guessing further only
# presses ENTER on something unknown.
def picker_page(host):
    """The current picker page as a comparable tuple of its row texts."""
    return tuple(text for _, text, _ in osd_rows(host))


def picker_find(host, want, dir_name=None, max_pages=12):
    """Scroll the open picker until `want` or `dir_name` is ON THE PAGE.

    Returns the name found, or None. Paging is the point: osd_find only sees
    the SIXTEEN rows the OSD has painted, and /media/fat/games holds about a
    hundred directories -- so AmigaCD32 was simply off-screen, and a search
    that only ever read the visible window reported it absent. There is no
    page-down key in the remote's table, so this scrolls with DOWN and stops
    when the page stops changing, which is the bottom.
    """
    seen = None
    for _ in range(max_pages):
        rows = osd_rows(host)
        if not rows:
            key(host, "DOWN"); key(host, "UP")      # provoke a paint
            time.sleep(0.4)
            continue
        if osd_find(rows, want) >= 0:
            return want
        if dir_name and osd_find(rows, dir_name) >= 0:
            return dir_name
        page = picker_page(host)
        if page == seen:
            return None                             # bottom, and not here
        seen = page
        before = osd_raw(host)
        key(host, "DOWN", repeat=14, gap=0.12)
        osd_wait_page(host, before, timeout=4.0)
    return None


# The picker opens where it was last left, and that is not a property of this
# harness: another session browsing for a test image moves it, and it had moved
# to /media/fat/games/Amiga, which holds nothing but subdirectories. "Take
# whatever is highlighted" then pressed ENTER on `..` and walked UP a level,
# mounting nothing -- which the run reported as a flaky mount, every round, with
# no hint that the picker was somewhere else entirely.
#
# So the file is found by name: scroll the directory looking for the disc or for
# `dir_name`, descend into that directory when it turns up, and go up through
# `..` when neither does. Bounded, because a picker showing none of the three is
# a page this does not understand, and guessing further only presses ENTER on
# something unknown.
def picker_select(host, want, dir_name=None, max_steps=8):
    """Select `want` in the open picker and press ENTER. True when it mounted."""
    for _ in range(max_steps):
        found = picker_find(host, want, dir_name)
        if found == want:
            if not osd_goto(host, want, max_steps=60):
                return False
            before = osd_raw(host)
            key(host, "ENTER")
            osd_wait_page(host, before)
            return True
        if found == dir_name:
            if not osd_goto(host, dir_name, max_steps=60):
                return False
            before = osd_raw(host)
            key(host, "ENTER")                      # descend
            osd_wait_page(host, before)
            continue
        # Neither is here. Go up a level and look again -- the picker is rooted
        # at a per-core base, so `..` at the top does nothing and the page stops
        # changing, which the bound below catches.
        rows = osd_rows(host)
        if osd_find(rows, "..") < 0:
            return False
        if not osd_goto(host, "..", max_steps=60):
            return False
        before = osd_raw(host)
        key(host, "ENTER")                          # up a level
        osd_wait_page(host, before)
    return False

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


# ------------------------------------------------- comparing SETTINGS
#
# The third thing worth A/B-ing is neither a bitstream nor a binary: it is a
# config byte. Two of them change the timing of the sector path, and neither was
# in the log until 2026-10-03, so every round recorded before that date is
# unclassifiable --
#
#   cd_mount_delay (offset 7280)  seconds the first media announce is held
#   cd_speed       (offset 7281)  0 = mmcd_fast (~588 sectors/s), 1 = mmcd_2x (150)
#
# Those offsets are documented in minimig_config.h, where the fields were
# deliberately appended in this order. They are read once at akiko_cd32_init(),
# so a setting takes effect on a core LOAD and not before -- which is why this
# writes the file and then reloads, exactly as the binary arm does.
#
# Writing the running machine's config behind its back is safe in one direction
# only: the value is read into minimig_config at startup, so a later OSD-driven
# save writes the same byte back. Changing the setting from the OSD DURING a run
# would fight this, so do not.
CFG_FILES = ("/media/fat/config/AmigaCD.cfg",
             "/media/fat/config/AmigaCD_CD32.cfg")
CFG_DELAY_OFF = 7280
CFG_SPEED_OFF = 7281

# label -> (offset, value). Add a pair here rather than teaching the CLI
# arithmetic; a run should name what it is testing.
SETTINGS = {
    "fast":    (CFG_SPEED_OFF, 0),    # mmcd_fast, the rate the core shipped with
    "2x":      (CFG_SPEED_OFF, 1),    # mmcd_2x, what a CD32 actually is
    "delay0":  (CFG_DELAY_OFF, 0),    # announce the disc at once, as before the fix
    "delay8":  (CFG_DELAY_OFF, 8),    # the default, and the value measured to work
}


def cfg_backup(host):
    """Copy every config this harness writes, once, before the first round."""
    for f in CFG_FILES:
        ssh(host, "[ -f %s.abbak ] || cp %s %s.abbak" % (f, f, f))


def cfg_restore(host):
    """Put them back. A run that changes a user's setting and leaves it changed
    is a run that poisons whatever is measured next -- which is how a 2x profile
    came to be live on the test machine without anyone choosing it."""
    for f in CFG_FILES:
        ssh(host, "[ -f %s.abbak ] && mv %s.abbak %s && sync" % (f, f, f))


def cfg_read(host, offset):
    """The byte at `offset` in each config, for reporting. Not for control
    flow: a config that does not exist yet reads as empty, and that is normal
    on a profile the machine has not saved."""
    out = []
    for f in CFG_FILES:
        v = ssh(host, "od -An -tu1 -j %d -N 1 %s 2>/dev/null" % (offset, f)).strip()
        out.append(v or "?")
    return ",".join(out)


def install_setting(host, name, settle):
    """Write one config byte to every profile, then reload the core."""
    if name not in SETTINGS:
        print("      (unknown setting %r)" % name, flush=True)
        return False
    offset, value = SETTINGS[name]
    for f in CFG_FILES:
        # conv=notrunc, or dd truncates the file to one byte and the next core
        # load migrates a 1-byte config through the loader's size dispatch.
        ssh(host, "printf '\\%03o' %d | dd of=%s bs=1 seek=%d count=1 conv=notrunc "
                  "2>/dev/null" % (value, value, f, offset))
    ssh(host, "sync")
    got = cfg_read(host, offset)
    if got != ",".join([str(value)] * len(CFG_FILES)):
        # Report, do not retry silently. A write that did not land scores the
        # round under the wrong label, which is worse than losing the round.
        print("      (setting %s: wrote %d at %d, read back %s)"
              % (name, value, offset, got), flush=True)
        return False
    ssh(host, "echo load_core %s > /dev/MiSTer_cmd" % CORE_PATH)
    time.sleep(settle)
    return True


def mount_and_wait(host, wait, expect_disc=None, disc_dir=None):
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
        osd_enable(host)
        if not osd_ensure(host, True):
            continue
        # Land on the CD row BY NAME, the way reset_machine does.
        #
        # This used to nudge DOWN/UP to provoke a repaint and then press ENTER
        # where it stood, with a comment claiming it had landed on the CD row.
        # It had not: the cursor is wherever the previous round's reset left it,
        # and ENTER on the wrong row does whatever that row does. On the System
        # row ENTER CYCLES THE MACHINE, so a drifted cursor does not merely
        # fail to mount -- it silently retunes the machine under test and every
        # round after it. The eject in reset_machine has navigated by name
        # since the day that cost a run; the mount never did.
        if not osd_goto(host, "CD"):
            print("      (no CD row on the OSD; not pressing ENTER)", flush=True)
            osd_ensure(host, False)
            continue
        before = osd_raw(host)
        key(host, "ENTER")                       # open the picker
        osd_wait_page(host, before)
        # Pick the title BY NAME too, when the caller named one.
        #
        # Taking "whatever is highlighted" worked only because the picker
        # remembers the last selection -- and a core LOAD resets it to the top
        # of the directory, which is a folder. ENTER there walks into the
        # folder instead of mounting, nothing is mounted, and the round is
        # skipped as a flaky mount. Every round of a --setting run reloads the
        # core, so this was every round.
        if expect_disc:
            if not picker_select(host, expect_disc, disc_dir):
                print("      (could not find %r in the picker)" % expect_disc,
                      flush=True)
                key(host, "ESC"); time.sleep(0.6)
                osd_ensure(host, False)
                continue
        else:
            key(host, "ENTER"); time.sleep(2.0)  # take the highlighted disc
        osd_ensure(host, False)                  # close the OSD
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
        osd_ensure(host, False)

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
    osd_enable(host)
    osd_ensure(host, True)

    # Eject, by name rather than by position.
    if osd_goto(host, "CD"):
        key(host, "BACKSPACE"); time.sleep(2.0)
    else:
        print("      (no CD row on the OSD; skipping the eject)", flush=True)

    # Reset. The old code pressed DOWN nine times to get here.
    if osd_goto(host, "Reset"):
        key(host, "ENTER"); time.sleep(1.2)
        # The confirmation defaults to No. Move to Yes by name if the page
        # names it, and fall back to one DOWN if it does not.
        if not osd_goto(host, "Yes"):
            key(host, "DOWN")
        key(host, "ENTER")
    else:
        print("      (no Reset row found; the machine may not be reset)", flush=True)

    time.sleep(24)                           # splash


def measure(host):
    # Both spellings, old and new, because the binary's log lines were
    # renamed when they gained their byte dumps: "phantom command frame"
    # became "PHANTOM n=.. bytes=.." and "frame resync" became
    # "RESYNC skip=..". Counting only the old names made a whole 12-round run
    # report phantoms=0 and resync=0 for every round -- numbers that looked
    # like findings and were artifacts of grepping for strings the binary no
    # longer prints. A harness that reads a log has to be updated with the
    # log, and matching both spellings means an older binary still measures.
    phantoms = int(ssh(host, "grep -cE 'phantom command frame|\\[akiko\\] PHANTOM' %s" % LOG) or 0)
    checksum = int(ssh(host, "grep -c 'CHECKSUM FAIL' %s" % LOG) or 0)
    resyncs = int(ssh(host, "grep -cE 'frame resync|\\[akiko\\] RESYNC' %s" % LOG) or 0)
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
    ap.add_argument("--setting", action="append", default=[], metavar="LABEL",
                    help="config settings to compare, from %s; the bitstream and "
                         "binary stay as they are" % "/".join(sorted(SETTINGS)))
    ap.add_argument("--fixed-core", metavar="PATH",
                    help="bitstream to install once, when comparing binaries")
    ap.add_argument("--expect-disc", metavar="SUBSTRING",
                    help="require the mounted path to contain this, so a drifted "
                         "OSD picking another title is skipped rather than scored")
    ap.add_argument("--disc-dir", metavar="NAME", default="AmigaCD32",
                    help="directory the picker descends into to find the disc")
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
    # A setting arm carries its own label as its "path", so the round loop
    # below stays one loop over (label, path) pairs.
    settings = [(s, s) for s in args.setting]
    for label, _ in settings:
        if label not in SETTINGS:
            sys.exit("setting %r: not one of %s" % (label, ", ".join(sorted(SETTINGS))))
    chosen = [bool(cores), bool(binaries), bool(settings)]
    if sum(chosen) != 1:
        sys.exit("give exactly one of --core, --binary or --setting (two or more of it)")

    # Comparing binaries: the bitstream is the constant, installed once.
    if binaries and args.fixed_core:
        if not os.path.exists(args.fixed_core):
            sys.exit("--fixed-core: no such file")
        print("installing fixed bitstream %s" % os.path.basename(args.fixed_core), flush=True)
        flash(args.host, args.fixed_core, args.settle)

    if settings:
        variants, install = settings, install_setting
        cfg_backup(args.host)
        print("comparing settings %s (configs backed up to *.abbak)"
              % ", ".join(l for l, _ in settings), flush=True)
    elif cores:
        variants, install = cores, flash
    else:
        variants, install = binaries, install_binary

    tally = {label: [0, 0] for label, _ in variants}    # [good, total]
    flagged = {label: [0, 0] for label, _ in variants}  # the flagged subset
    suspect_count = {label: 0 for label, _ in variants}

    try:
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
                                                   args.expect_disc,
                                                   args.disc_dir)
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

    finally:
        # Even on Ctrl-C. A half-finished run that leaves cd_speed
        # flipped silently retunes whatever is measured next.
        if settings:
            cfg_restore(args.host)
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
