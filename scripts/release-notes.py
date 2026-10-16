#!/usr/bin/env python3
"""Changelog for a release: what landed on the release branch since the last one.

    scripts/release-notes.py <from> <to> [--limit N] [--repo owner/name]

<from> is the previous release tag, or "" for the first release (then only the
most recent --limit entries are listed, and the notes say so -- the history
behind this repository's first release is the whole of Main_MiSTer).

Walks the FIRST-PARENT history of <to>, because that is what the branch
actually received: one entry per merged pull request, titled from the PR
title GitHub puts in the merge commit's body, plus any commit made on the
branch directly. The commits inside a PR are its own business; a changelog of
them repeats every "fix review nit" twice over.

Entries are grouped by their conventional-commit prefix (feat, fix, ...), which
is what this repository's commit and PR titles use. A title without one goes
under "Other changes" rather than being guessed at.

Prints Markdown on stdout. Read-only: it never creates tags or releases.
"""
import argparse
import re
import subprocess
import sys

GROUPS = [
    ('feat', 'Features'),
    ('fix', 'Fixes'),
    ('perf', 'Performance'),
    ('docs', 'Documentation'),
    ('ci', 'Build and CI'),
    ('build', 'Build and CI'),
    ('test', 'Tests'),
    ('refactor', 'Refactoring'),
    ('chore', 'Maintenance'),
]
OTHER = 'Other changes'

PREFIX = re.compile(r'^(?P<type>[a-z]+)(?:\((?P<scope>[^)]*)\))?(?P<bang>!)?:\s*(?P<rest>.+)$')
PR_MERGE = re.compile(r'^Merge pull request #(?P<num>\d+) from (?P<branch>\S+)')


def git(*args):
    return subprocess.run(['git', *args], check=True, capture_output=True, text=True).stdout


def entries(rev_range, limit):
    # NUL-free separators: unit (0x1f) between fields, record (0x1e) between commits.
    out = git('log', '--first-parent', '--format=%H%x1f%s%x1f%b%x1e', rev_range)
    for rec in out.split('\x1e'):
        rec = rec.strip('\n')
        if not rec:
            continue
        sha, subject, body = (rec.split('\x1f') + ['', ''])[:3]
        m = PR_MERGE.match(subject)
        if m:
            # GitHub writes the PR title as the first line of the body. A PR
            # merged with an empty description still has it; one merged some
            # other way may not, and then the branch name is all there is.
            title = next((l.strip() for l in body.splitlines() if l.strip()), '') or m.group('branch')
            yield {'sha': sha, 'title': title, 'pr': int(m.group('num'))}
        else:
            yield {'sha': sha, 'title': subject.strip(), 'pr': None}
        limit -= 1
        if limit == 0:
            return


def group_of(title):
    m = PREFIX.match(title)
    if not m:
        return OTHER, title, False
    t = m.group('type')
    name = next((g for k, g in GROUPS if k == t), OTHER)
    rest = m.group('rest')
    if m.group('scope'):
        rest = '%s: %s' % (m.group('scope'), rest)
    # Conventional "!" marks a breaking change; it is kept visible.
    return name, (rest if name != OTHER else title), bool(m.group('bang'))


def render(items, frm, to, repo, truncated_at):
    lines = []
    if frm:
        lines.append('## Changes since %s' % frm)
    else:
        lines.append('## Changes')
        lines.append('')
        lines.append('_First release: the %d most recent changes are listed._' % truncated_at)
    if not items:
        lines.append('')
        lines.append('No changes.')
        return '\n'.join(lines) + '\n'

    order = []
    for _, g in GROUPS:
        if g not in order:
            order.append(g)
    order.append(OTHER)

    grouped = {g: [] for g in order}
    for it in items:
        g, text, breaking = group_of(it['title'])
        ref = ('#%d' % it['pr']) if it['pr'] else it['sha'][:7]
        if repo and it['pr']:
            ref = '[#%d](https://github.com/%s/pull/%d)' % (it['pr'], repo, it['pr'])
        grouped[g].append('- %s%s (%s)' % ('**BREAKING** ' if breaking else '', text, ref))

    for g in order:
        if grouped[g]:
            lines.append('')
            lines.append('### ' + g)
            lines.append('')
            lines.extend(grouped[g])
    if repo and frm:
        lines.append('')
        lines.append('Full diff: https://github.com/%s/compare/%s...%s' % (repo, frm, to))
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frm')
    ap.add_argument('to')
    ap.add_argument('--limit', type=int, default=50)
    ap.add_argument('--repo', default='')
    a = ap.parse_args()

    rev_range = ('%s..%s' % (a.frm, a.to)) if a.frm else a.to
    limit = 0 if a.frm else a.limit          # 0: no limit
    items = list(entries(rev_range, limit))
    sys.stdout.write(render(items, a.frm, a.to, a.repo, a.limit))


if __name__ == '__main__':
    main()
