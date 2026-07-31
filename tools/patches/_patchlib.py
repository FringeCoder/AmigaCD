"""Shared helper for the vendored-file patch scripts.

Each patch is a literal, asserted replacement against an anchor that must
occur exactly once. If upstream text moves, the patch fails loudly instead of
silently landing in the wrong place. Applying is idempotent: a patch whose
replacement text is already present is reported as "already" and skipped.

The tree checks out CRLF on Windows while these scripts are written with LF
anchors, so matching is done on a normalised copy and the file's own line
ending convention is restored on write.
"""
import sys
from pathlib import Path


class PatchSet:
    def __init__(self, root):
        self.root = Path(root)
        self.edits = []

    def add(self, relpath, old, new, note):
        self.edits.append((relpath, old, new, note))

    def apply(self):
        applied, skipped, failed = [], [], []

        for relpath, old, new, note in self.edits:
            path = self.root / relpath
            if not path.exists():
                failed.append(f"{relpath}: {note}: file not found")
                continue
            raw = path.read_text(encoding="utf-8", newline="")
            crlf = "\r\n" in raw
            text = raw.replace("\r\n", "\n")
            if new in text:
                skipped.append(f"{relpath}: {note}")
                continue
            count = text.count(old)
            if count != 1:
                failed.append(
                    f"{relpath}: {note}: expected 1 anchor match, found {count}")
                continue
            out = text.replace(old, new, 1)
            if crlf:
                out = out.replace("\n", "\r\n")
            path.write_text(out, encoding="utf-8", newline="")
            applied.append(f"{relpath}: {note}")

        for line in applied:
            print("  applied  ", line)
        for line in skipped:
            print("  already  ", line)
        for line in failed:
            print("  FAILED   ", line)

        if failed:
            print(f"\n{len(failed)} edit(s) failed; earlier edits in this run were "
                  f"NOT rolled back. Inspect with git diff.", file=sys.stderr)
            return 1
        print(f"\n{len(applied)} applied, {len(skipped)} already present")
        return 0
