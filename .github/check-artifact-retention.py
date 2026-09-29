#!/usr/bin/env python3
"""Refuse an upload-artifact step that does not set retention-days.

Without retention-days an artifact keeps the repository default and never
expires. 493 of them accumulated in this repository between 2026-07-30 and
2026-09-28, 0.83 GB against the 500 MB cap for a private repo.

The failure mode is what makes this worth a CI gate rather than a convention:
once the quota is full the upload step fails, so EVERY build fails -- including
on a pull request that touches nothing but documentation. That looks exactly
like a broken build and sends whoever hits it looking in the wrong place. It
happened on FringeCoder/AmigaCD#80.

Run from the repository root; exits non-zero and names the offending steps.
"""
import glob
import os
import sys

try:
    import yaml
except ImportError:
    sys.exit("PyYAML is required: pip install pyyaml")


def offenders(path):
    """Steps in one workflow file that upload artifacts without a retention."""
    with open(path, encoding='utf-8') as fh:
        doc = yaml.safe_load(fh) or {}
    out = []
    for job_name, job in (doc.get('jobs') or {}).items():
        if not isinstance(job, dict):
            continue
        for step in (job.get('steps') or []):
            if not isinstance(step, dict):
                continue
            uses = step.get('uses') or ''
            # Covers actions/upload-artifact and the pages variant.
            if 'upload-artifact' not in uses:
                continue
            with_ = step.get('with') or {}
            if 'retention-days' not in with_:
                out.append('%s / %s' % (job_name, step.get('name') or uses))
    return out


def main():
    files = sorted(glob.glob(os.path.join('.github', 'workflows', '*.yml')) +
                   glob.glob(os.path.join('.github', 'workflows', '*.yaml')))
    if not files:
        print('no workflow files found -- run this from the repository root')
        return 1

    bad = False
    for path in files:
        missing = offenders(path)
        if missing:
            bad = True
            print('%s: upload-artifact without retention-days' % path)
            for m in missing:
                print('    - %s' % m)

    if bad:
        print()
        print('Add `retention-days:` to the steps above. Artifacts that never')
        print('expire fill the storage quota, and a full quota fails every build.')
        return 1

    print('checked %d workflow file(s); every upload-artifact step sets '
          'retention-days' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
