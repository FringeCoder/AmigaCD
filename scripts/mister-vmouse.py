#!/usr/bin/env python3
# A virtual USB mouse on the MiSTer through /dev/uinput, for clicking guest
# buttons when nobody is at the machine. Steps, in order:
#   m DX DY   move by DX, DY counts (split into small reports)
#   c         left click
#   s SEC     sleep
# Runs ON the MiSTer (python3 is there): copy it over and run it with ssh.
# One count moves the Amiga pointer about 0.9 screenshot px across and 0.5 down.
# e.g. vmouse.py "m -2000 -2000" "m 300 200" c
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y, BTN_LEFT, BTN_RIGHT = 0, 1, 0x110, 0x111

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT)
fcntl.ioctl(fd, UI_SET_RELBIT, REL_X)
fcntl.ioctl(fd, UI_SET_RELBIT, REL_Y)
name = b"claude-vmouse".ljust(80, b"\0")
os.write(fd, name + struct.pack("<HHHHI", 3, 0x1234, 0x5678, 1, 0) + b"\0" * (4 * 64 * 4))
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(3)                      # let Main find the new device

def ev(t, c, v):
    os.write(fd, struct.pack("<llHHi", 0, 0, t, c, v))

def syn():
    ev(EV_SYN, 0, 0)

for step in sys.argv[1:]:
    a = step.split()
    if a[0] == "m":
        dx, dy = int(a[1]), int(a[2])
        while dx or dy:
            sx = max(-10, min(10, dx)); sy = max(-10, min(10, dy))
            ev(EV_REL, REL_X, sx); ev(EV_REL, REL_Y, sy); syn()
            dx -= sx; dy -= sy
            time.sleep(0.01)
    elif a[0] == "c":
        ev(EV_KEY, BTN_LEFT, 1); syn(); time.sleep(0.15)
        ev(EV_KEY, BTN_LEFT, 0); syn()
    elif a[0] == "s":
        time.sleep(float(a[1]))
time.sleep(0.5)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
