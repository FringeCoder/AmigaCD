"""What actually runs between a flash and the first mount?

Measurement, not inference. Two explanations have already been wrong:

  1. "the swap path is the discriminator" -- no, every round takes it;
  2. "host state survived the reflash" -- no, load_core re-execs the app
     (PID 2569 -> 6716 across one flash), so every static starts fresh.

What IS established: the failing round has no akiko set_cd_path and no NVR load,
and every passing round has both.

So this dumps the akiko log at three points without ever clearing it -- after the
flash, after the OSD pick, and after the boot window -- and the three dumps say
which calls happen where. Nothing is cleared, because clearing is what hid the
ordering in the first place.
"""
import importlib.util
import os
import sys
import time

REPO = r'C:\Users\Admin\Documents\RetroCoding\AmigaCD'
S = (r'C:\Users\Admin\AppData\Local\Temp\claude'
     r'\C--Users-Admin-Documents-RetroCoding-AmigaCD'
     r'\5d9bd86e-b658-4fe9-9ced-04c13766adae\scratchpad')
RBF = os.path.join(S, 'initdone_on_mainline.rbf')
H = "192.168.2.103"

spec = importlib.util.spec_from_file_location(
    "bab", os.path.join(REPO, 'scripts', 'boot-ab.py'))
m = importlib.util.module_from_spec(spec)
sys.modules["bab"] = m
spec.loader.exec_module(m)

INTERESTING = ("set_cd_path", "NVR", "load_chd", "init:", "boot settle",
               "media level", "swap:", "auto-init", "CHECKSUM", "BAD cmd",
               "PLAY DATA arm", "TOC built")


def dump(tag):
    body = m.ssh(H, "cat %s" % m.LOG, timeout=120) or ""
    path = os.path.join(S, "tr_%s.log" % tag)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    keep = [l for l in body.splitlines() if any(k in l for k in INTERESTING)]
    print("-- %s: %d lines, %d interesting" % (tag, len(body.splitlines()), len(keep)))
    for l in keep[:14]:
        print("   ", l[:120])
    return body


def alive(host, tries=8):
    m.osd_enable(host)
    for _ in range(tries):
        m.key(host, "F12")
        time.sleep(2)
        rows = m.osd_rows(host)
        if rows:
            m.key(host, "F12")
            time.sleep(1)
            return rows
        time.sleep(3)
    return []


print("== flashing (log NOT cleared at any point)", flush=True)
if not m.flash(H, RBF, 28):
    sys.exit("flash failed")

rows = alive(H)
if not rows:
    sys.exit("no OSD after the flash")
print("== core alive, %d rows; CD row reads: %r"
      % (len(rows), next((t for _, t, _ in rows if t.startswith("CD")), "?")), flush=True)

dump("after_flash")

print("== picking the disc from the OSD", flush=True)
m.key(H, "F12"); time.sleep(1.0)
m.key(H, "DOWN"); m.key(H, "UP")
m.key(H, "ENTER"); time.sleep(1.4)
m.key(H, "ENTER"); time.sleep(2.0)
m.key(H, "F12"); time.sleep(2.0)

dump("after_pick")

print("== waiting out the boot window", flush=True)
time.sleep(60)
body = dump("after_boot")

bios = "start_lba=14 " in body
loader = "start_lba=12 " in body
print()
print("BIOS read (lba 14): %s   loader read (lba 12): %s" % (bios, loader))
print("VERDICT: %s" % ("booted" if (bios and loader) else "did NOT boot"))
