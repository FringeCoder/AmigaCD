#!/usr/bin/env python3
# A virtual USB keyboard on the MiSTer through /dev/uinput, for typing into the
# guest when nobody is at the machine (the Companion daemon's keyboard reaches
# the OSD but not the core). Steps, in order:
#   k NAME [N]   press and release a key N times (Linux key names without KEY_)
#   t TEXT       type lower-case letters, digits and spaces
#   s SEC        sleep
# e.g. vkey.py "k DOWN 10" "k ENTER" "t frontier"
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
EV_SYN, EV_KEY = 0, 1
KEYS = {"ESC": 1, "BACKSPACE": 14, "TAB": 15, "ENTER": 28, "SPACE": 57, "LEFTSHIFT": 42,
        "F1": 59, "F2": 60, "F10": 68, "UP": 103, "LEFT": 105, "RIGHT": 106, "DOWN": 108,
        "PAGEUP": 104, "PAGEDOWN": 109, "HOME": 102, "END": 107, "LEFTALT": 56, "LEFTCTRL": 29}
for i, c in enumerate("1234567890"): KEYS[c] = 2 + i
for row, start in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for i, c in enumerate(row): KEYS[c] = start + i

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
for code in set(KEYS.values()):
    fcntl.ioctl(fd, UI_SET_KEYBIT, code)
name = b"claude-vkey".ljust(80, b"\0")
os.write(fd, name + struct.pack("<HHHHI", 3, 0x1234, 0x5679, 1, 0) + b"\0" * (4 * 64 * 4))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(3)                      # let Main find the new device

def ev(t, c, v):
    os.write(fd, struct.pack("<llHHi", 0, 0, t, c, v))

def tap(code):
    ev(EV_KEY, code, 1); ev(EV_SYN, 0, 0); time.sleep(0.12)
    ev(EV_KEY, code, 0); ev(EV_SYN, 0, 0); time.sleep(0.25)

for step in sys.argv[1:]:
    a = step.split(None, 1)
    if a[0] == "k":
        p = a[1].split()
        for _ in range(int(p[1]) if len(p) > 1 else 1):
            tap(KEYS[p[0]])
    elif a[0] == "t":
        for ch in a[1]:
            tap(KEYS["SPACE" if ch == " " else ch])
    elif a[0] == "s":
        time.sleep(float(a[1]))
time.sleep(0.5)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
