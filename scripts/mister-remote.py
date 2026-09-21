#!/usr/bin/env python3
"""Drive the MiSTer's OSD and read its screen, through MiSTer Companion's remote.

This is the display-side counterpart to amiga-peek.py. That one reads the
guest's memory through ss_ctrl; this one presses keys and takes screenshots
through the Companion daemon on the MiSTer, so a menu page or a test program's
output can be checked from here rather than photographed off a CRT. On
2026-09-21 two OSD bugs were found by hand that this would have shown in one
screenshot each, and on 2026-09-03 a hex value misread from a phone photo cost
a working hypothesis.

    ./scripts/mister-remote.py ping
    ./scripts/mister-remote.py shot out.png            screenshot to a file
    ./scripts/mister-remote.py key F12                  tap one key
    ./scripts/mister-remote.py keys F12 DOWN DOWN ENTER sequence, 250 ms apart
    ./scripts/mister-remote.py pad a                    controller button
    ./scripts/mister-remote.py osd "F12 DOWN*7 ENTER" out.png
                                                        keys, settle, screenshot
    ./scripts/mister-remote.py text                     the OSD as text, via ssh
    ./scripts/mister-remote.py menu "F12 DOWN*7 ENTER"  keys, then the OSD as text

THE OSD IS NOT IN A SCREENSHOT
------------------------------
MiSTer's screenshot is the scaler framebuffer, and the OSD is composited in
the FPGA after it, so no capture ever contains the menu -- `shot` with the
menu open returns the game underneath. For menus use `text` / `menu`: the
binary mirrors every OSD paint to /tmp/osd.txt while /tmp/osd_mirror exists
(osd.cpp, osd_mirror_flush), with the selected row marked `>` and a greyed
one `~`. Exact text and exact cursor, which is what checking a page needs.
`text` creates the marker if it is missing; the first paint after that fills
the file, so press a key if it comes back empty.

Key names are Linux input names without the KEY_ prefix: F12, UP, DOWN, LEFT,
RIGHT, ENTER, ESC, BACKSPACE, KPMINUS, KPPLUS, MINUS, EQUAL, F1..F3, LEFTCTRL,
LEFTALT, RIGHTALT. Pad buttons: a b x y l r lb rb select start mode home, and
up/down/left/right for the d-pad. `KEY*n` repeats a key n times.

HOW IT WORKS
------------
The daemon is /media/fat/Scripts/.config/companion_remote/companion_remote_daemon,
a self-contained Python service Companion installs. It creates a uinput
keyboard and pad, so a key from here is indistinguishable from one pressed at
the machine -- it goes through the same input path as a real keyboard,
including MiSTer's own OSD handling. The screenshot is MiSTer's: the daemon
injects Alt+ScrollLock, the running core binary writes the PNG under
/media/fat/screenshots, and the daemon serves the newest one. So a screenshot
here exercises OUR binary's screenshot path, not the daemon's.

The websocket is the daemon's own minimal RFC 6455; this client matches it
with the standard library only, so there is nothing to pip install.

TWO THINGS THAT WILL MISLEAD YOU
--------------------------------
1. A screenshot is of the FRAMEBUFFER the core scales out, without the OSD. It is
   not the chipset's idea of the frame -- for that, see the copper and
   beamcounter benches. A row that looks wrong here may be the scaler.

2. The daemon only reports a NEW screenshot. If the core does not write one
   (the binary predates screenshot support, or the shortcut is bound to
   something else), `shot` times out rather than returning a stale image.
   That is deliberate on the daemon's side and the right behaviour.
"""
import argparse
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.request

DEFAULT_PORT = 9191
WS_PATH = "/remote/v1"


def default_host():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(os.path.dirname(here), "host")
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return "192.168.2.103"


class Remote:
    """One websocket session to the daemon."""

    def __init__(self, host, port=DEFAULT_PORT, timeout=10.0):
        self.host, self.port = host, port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (WS_PATH, host, port, key))
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("daemon closed the connection during the handshake")
            head += chunk
        status = head.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 101 " not in status:
            raise RuntimeError("websocket upgrade refused: %s" % status)
        # The daemon greets every new connection with a status frame. Taken
        # as the reply to the first command, it made a screenshot fetch race
        # the PNG still being written -- the daemon's "still being written"
        # 500. Consume it here so replies line up with commands.
        self.sock.settimeout(2.0)
        try:
            self._recv()
        except (socket.timeout, RuntimeError):
            pass
        self.sock.settimeout(timeout)

    def _send(self, payload):
        data = json.dumps(payload).encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(hdr + mask + masked)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("daemon closed the connection")
            buf += chunk
        return buf

    def _recv(self):
        while True:
            b0, b1 = self._recv_exact(2)
            op = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._recv_exact(8))[0]
            mask = self._recv_exact(4) if b1 & 0x80 else None
            data = self._recv_exact(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if op == 0x8:
                raise RuntimeError("daemon closed the websocket")
            if op == 0x9:                       # ping -> pong
                self.sock.sendall(struct.pack("!BB", 0x8A, 0x80) + b"\0\0\0\0")
                continue
            if op in (0x1, 0x2):
                return json.loads(data.decode())

    def command(self, **payload):
        self._send(payload)
        # The daemon may push status frames; take the first result-shaped one.
        for _ in range(8):
            r = self._recv()
            if isinstance(r, dict) and "ok" in r:
                if not r.get("ok", False):
                    raise RuntimeError(r.get("message") or "command refused")
                return r
        raise RuntimeError("no reply to %r" % (payload,))

    def key(self, name, action="tap"):
        return self.command(type="keyboard", key="KEY_" + name.upper(), action=action)

    def pad(self, name, action="tap"):
        return self.command(type="controller", name=name.lower(), action=action)

    def screenshot(self, out_path):
        # fresh=1 makes the daemon trigger the capture and return the new
        # PNG in one synchronous request, so there is no window in which the
        # file exists but is not finished.
        url = "http://%s:%d/api/screenshot?fresh=1" % (self.host, self.port)
        with urllib.request.urlopen(url, timeout=20) as r:
            png = r.read()
        if not png.startswith(b"\x89PNG"):
            raise RuntimeError("daemon returned %d bytes that are not a PNG" % len(png))
        with open(out_path, "wb") as f:
            f.write(png)
        return len(png)

    def close(self):
        try:
            self.sock.sendall(struct.pack("!BB", 0x88, 0x80) + b"\0\0\0\0")
        except OSError:
            pass
        self.sock.close()


def osd_text(host):
    """The mirrored OSD, or '' if the binary has not painted since the marker
    was created. ssh as root, the same convention as amiga-peek.py."""
    r = subprocess.run(["ssh", "-o", "ConnectTimeout=10", "-o", "BatchMode=yes",
                        "root@" + host,
                        "touch /tmp/osd_mirror; cat /tmp/osd.txt 2>/dev/null"],
                       capture_output=True)
    # The OSD font has glyphs above 0x7F -- the stock menu's box drawing,
    # the submenu arrow -- and they are not UTF-8. Decode as Latin-1 so
    # nothing ever raises; the shapes come out as odd characters, which is
    # fine, the text and the cursor are what this is for.
    return r.stdout.decode("latin-1")


def expand_keys(spec):
    """'F12 DOWN*3 ENTER' -> ['F12', 'DOWN', 'DOWN', 'DOWN', 'ENTER']"""
    out = []
    for tok in spec.replace(",", " ").split():
        if "*" in tok:
            k, n = tok.split("*", 1)
            out += [k] * int(n)
        else:
            out.append(tok)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--gap", type=float, default=0.25,
                    help="seconds between keys in a sequence (default 0.25)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    p = sub.add_parser("shot"); p.add_argument("out")
    p = sub.add_parser("key"); p.add_argument("name"); p.add_argument("action", nargs="?", default="tap")
    p = sub.add_parser("keys"); p.add_argument("names", nargs="+")
    p = sub.add_parser("pad"); p.add_argument("name"); p.add_argument("action", nargs="?", default="tap")
    p = sub.add_parser("osd", help="key sequence, then a screenshot")
    p.add_argument("spec"); p.add_argument("out")
    p.add_argument("--settle", type=float, default=0.6,
                   help="seconds to wait after the last key before the shot")
    sub.add_parser("text", help="the OSD as text (needs the mirror, see above)")
    p = sub.add_parser("menu", help="key sequence, then the OSD as text")
    p.add_argument("spec")
    p.add_argument("--settle", type=float, default=0.4)
    args = ap.parse_args()

    host = args.host or default_host()
    if args.cmd == "text":
        sys.stdout.write(osd_text(host) or "(no OSD paint recorded yet -- press a key)\n")
        return 0

    r = Remote(host, args.port)
    try:
        if args.cmd == "ping":
            print(r.command(type="ping").get("message"))
        elif args.cmd == "shot":
            print("%d bytes -> %s" % (r.screenshot(args.out), args.out))
        elif args.cmd == "key":
            r.key(args.name, args.action); print("ok")
        elif args.cmd == "pad":
            r.pad(args.name, args.action); print("ok")
        elif args.cmd == "keys":
            for k in expand_keys(" ".join(args.names)):
                r.key(k); time.sleep(args.gap)
            print("ok")
        elif args.cmd == "osd":
            for k in expand_keys(args.spec):
                r.key(k); time.sleep(args.gap)
            time.sleep(args.settle)
            print("%d bytes -> %s" % (r.screenshot(args.out), args.out))
        elif args.cmd == "menu":
            osd_text(host)                       # make sure the marker exists first
            for k in expand_keys(args.spec):
                r.key(k); time.sleep(args.gap)
            time.sleep(args.settle)
            sys.stdout.write(osd_text(host) or "(no OSD paint recorded)\n")
    finally:
        r.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as e:
        sys.exit("error: %s" % e)
