#!/usr/bin/env python3
"""Walk every AmigaCD OSD page on the real machine and report what the cursor does.

The static checks in tests/test_osd_rowguard.cpp and the row-budget counts in
menu.cpp catch a page that disagrees with itself in source. This is the other
half: what the binary actually paints, read back through the OSD text mirror
after every keypress. It found nothing the static checks had not, on the day
it was written -- but the two bugs that prompted it (a row whose handler was
unreachable, a row drawn on the line Back overwrites) were both invisible in
source and obvious in one look at the mirror.

    ./scripts/osd-audit.py                 walk every page, print the report
    ./scripts/osd-audit.py --page Settings walk one page

For each page: press DOWN until the cursor returns to where it started,
recording every stop. A stop is FLAGGED when

  - nothing is highlighted (a menusub the mask allows but no row draws), or
  - the highlighted row is blank (a row drawn past the last line and
    overwritten, or a spacer that took the cursor), or
  - the same row is highlighted twice in one lap without the cursor having
    moved (a DOWN that did nothing).

Then, for every row whose text ends in the submenu arrow, ENTER, walk that
page the same way, and LEFT back -- checking that LEFT lands on the row it
came from.

Needs the mirror (osd.cpp, /tmp/osd_mirror) and the Companion remote; see
scripts/mister-remote.py for both. Leaves the OSD closed when it finishes.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
mr = __import__("mister-remote")

ARROW = "\x16"          # the submenu marker menu.cpp appends to a label
GAP = 0.35              # seconds between a key and the read


class Page:
    def __init__(self, text):
        self.title = "?"
        self.rows = {}      # index -> (flag, text)
        for line in text.splitlines():
            m = re.match(r"\[(.*)\]$", line)
            if m:
                self.title = m.group(1); continue
            m = re.match(r"\s*(\d+) (.) (.*)$", line)
            if m:
                self.rows[int(m.group(1))] = (m.group(2), m.group(3).rstrip())

    @property
    def cursor(self):
        for i, (f, t) in sorted(self.rows.items()):
            if f == ">":
                return i, t
        return None, None

    def closed(self):
        return self.title == "closed"


class Walker:
    def __init__(self, host):
        self.host = host
        self.r = mr.Remote(host)
        self.findings = []
        mr.osd_text(host)                     # ensure the marker exists

    def read(self):
        return Page(mr.osd_text(self.host))

    def key(self, k, gap=GAP):
        self.r.key(k)
        time.sleep(gap)
        return self.read()

    def flag(self, page, msg):
        self.findings.append((page, msg))
        print("  !! " + msg)

    def walk(self, depth=0):
        """Walk the current page. Returns the list of (index, text) stops."""
        p = self.read()
        ind = "  " * depth
        print(f"{ind}## [{p.title}]")
        for i, (f, t) in sorted(p.rows.items()):
            print(f"{ind}  {i:2d} {f} {t}")
        start = p.cursor
        if start[0] is None:
            self.flag(p.title, "page opened with nothing highlighted")
            return []
        stops = [start]
        prev = start
        # A row under the cursor shows '>' whatever else it is, so its own
        # greyed flag is only visible on reads where the cursor is elsewhere.
        # Collect those across the lap; walk_tree needs them.
        self.flags = {i: f for i, (f, t) in p.rows.items() if f != ">"}
        for _ in range(40):
            q = self.key("DOWN")
            for i, (f, t) in q.rows.items():
                if f != ">":
                    self.flags[i] = f
            if q.title != p.title:
                self.flag(p.title, f"DOWN changed the page to [{q.title}]")
                return stops
            cur = q.cursor
            if cur[0] is None:
                self.flag(p.title, f"hidden stop after row {prev[0]} ({prev[1].strip()!r}): nothing highlighted")
                cur = (None, "")
            elif not cur[1].strip():
                self.flag(p.title, f"cursor on a blank line {cur[0]} after row {prev[0]}")
            elif cur == prev:
                self.flag(p.title, f"DOWN did not move from row {cur[0]} ({cur[1].strip()!r})")
            if cur == start:
                break
            stops.append(cur)
            prev = cur
        else:
            self.flag(p.title, "cursor never returned to its start in 40 DOWNs")
        print(f"{ind}  lap: " + " -> ".join(str(i) for i, _ in stops) + f" -> {start[0]}")
        return stops

    def goto_row(self, index):
        """Move the cursor to a row index by stepping DOWN (wrapping)."""
        for _ in range(40):
            p = self.read()
            if p.cursor[0] == index:
                return p
            self.key("DOWN")
        raise RuntimeError(f"could not reach row {index}")

    def walk_tree(self, depth=0, only=None):
        stops = self.walk(depth)
        flags = dict(self.flags)
        subs = [(i, t) for i, t in stops if i is not None and t.endswith(ARROW)]
        for i, t in subs:
            name = t.rstrip(ARROW).strip()
            if only and name != only:
                continue
            greyed = flags.get(i) == "~"
            here = self.goto_row(i)
            q = self.key("ENTER", gap=0.6)
            if q.title == here.title:
                if greyed:
                    # A greyed submenu row refusing ENTER is the design: the
                    # handler re-checks rather than trusting the greying, so a
                    # page for a device that is not the current tenant cannot
                    # be opened. MT32-pi settings with SNAC on the user port.
                    print(f"{'  ' * depth}  (greyed {name!r} refused ENTER, as designed)")
                else:
                    self.flag(here.title, f"ENTER on {name!r} (row {i}) did not open a page")
                continue
            self.walk_tree(depth + 1)
            back = self.key("LEFT", gap=0.6)
            if back.title != here.title:
                self.flag(q.title, f"LEFT from [{q.title}] landed on [{back.title}], not [{here.title}]")
                # try to recover: LEFT again
                back = self.key("LEFT", gap=0.6)
            elif back.cursor[0] != i:
                self.flag(q.title, f"LEFT from [{q.title}] returned to row {back.cursor[0]}, not {i} ({name!r})")

    def close(self):
        p = self.read()
        for _ in range(4):
            if p.closed():
                break
            p = self.key("ESC", gap=0.6)
        if not p.closed():
            self.key("F12", gap=0.6)
        self.r.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=None)
    ap.add_argument("--page", default=None, help="walk only this submenu of the main page")
    args = ap.parse_args()
    host = args.host or mr.default_host()

    w = Walker(host)
    try:
        p = w.read()
        if p.closed() or p.title in ("?", "closed"):
            p = w.key("F12", gap=0.8)
        if p.title != "AmigaCD":
            # come back to the main page from wherever the cursor was left
            for _ in range(4):
                if p.title == "AmigaCD":
                    break
                p = w.key("LEFT", gap=0.6)
        if p.title != "AmigaCD":
            sys.exit(f"error: could not reach the main page (on [{p.title}])")
        w.walk_tree(only=args.page)
    finally:
        w.close()

    print()
    if w.findings:
        print(f"{len(w.findings)} FINDINGS")
        for page, msg in w.findings:
            print(f"  [{page}] {msg}")
        return 1
    print("no findings: every stop on every page is a drawn, non-blank row, and every LEFT lands where it came from")
    return 0


if __name__ == "__main__":
    sys.exit(main())
