#!/usr/bin/env bash
#
# What scripts/release-notes.py puts in a release's changelog.
#
# It reads the first-parent history the way GitHub writes it: a merged PR is a
# "Merge pull request #N from ..." commit whose body's first line is the PR
# title. This builds exactly that in a scratch repository and checks the notes:
# one line per PR, titled from the PR not the branch, grouped by prefix, the
# commits inside a PR not listed again, the previous release excluded, and a
# direct commit on the branch still counted.
#
# FALSIFIED. Each mutation was applied to release-notes.py and the named check
# failed:
#   --first-parent dropped        "commits inside a PR are not listed"
#   body title ignored            "a PR is titled from its title"
#   empty-body fallback dropped   "a PR with no title falls back to its branch"
#   feat/fix mapping swapped      "feat goes under Features", "fix ... Fixes"
#   range ignored (from unused)   "the previous release is not listed again"
#
# Run: bash tests/test_release_notes.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
script="$here/../scripts/release-notes.py"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
git init -q -b master .
git config user.email t@example.com
git config user.name test
git config commit.gpgsign false

commit() { echo "$1" >> f; git add f; git commit -q -m "$1"; }
merge_pr() {   # merge_pr <num> <branch> <title>
	git merge -q --no-ff "$2" -m "Merge pull request #$1 from Owner/$2" -m "$3"
}

commit "initial"
git tag v20260101

git checkout -q -b feat-x
commit "wip: first go"
commit "address review nit"
git checkout -q master
merge_pr 10 feat-x "feat: mount .dms floppy archives"

git checkout -q -b fix-y
commit "fix the thing"
git checkout -q master
merge_pr 11 fix-y "fix(akiko): the seek was never spent"

git checkout -q -b no-title
commit "something"
git checkout -q master
git merge -q --no-ff no-title -m "Merge pull request #12 from Owner/no-title"

commit "docs: direct commit on master"

notes="$(python3 "$script" v20260101 master --repo Owner/Repo)"
echo "$notes" | sed 's/^/    | /'

check "the heading names the previous release" '[[ "$notes" == *"## Changes since v20260101"* ]]'
check "a PR is titled from its title" '[[ "$notes" == *"- mount .dms floppy archives ([#10](https://github.com/Owner/Repo/pull/10))"* ]]'
check "feat goes under Features" '[[ "$(echo "$notes" | sed -n "/### Features/,/###/p")" == *"mount .dms"* ]]'
check "fix goes under Fixes, with its scope" '[[ "$(echo "$notes" | sed -n "/### Fixes/,/###/p")" == *"akiko: the seek was never spent"* ]]'
check "a PR with no title falls back to its branch" '[[ "$notes" == *"Owner/no-title"* ]]'
check "a direct commit is listed" '[[ "$notes" == *"direct commit on master"* ]]'
check "commits inside a PR are not listed" '[[ "$notes" != *"address review nit"* && "$notes" != *"wip: first go"* ]]'
check "the previous release is not listed again" '[[ "$notes" != *"initial"* ]]'
check "the compare link is there" '[[ "$notes" == *"compare/v20260101...master"* ]]'

first="$(python3 "$script" "" master --limit 2)"
check "a first release says it is truncated" '[[ "$first" == *"the 2 most recent changes"* ]]'
check "and lists only that many" '[ "$(echo "$first" | grep -c "^- ")" = 2 ]'

none="$(python3 "$script" master master)"
check "nothing since the last release says so" '[[ "$none" == *"No changes."* ]]'

exit $fail
