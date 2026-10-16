# Releasing MiSTer_AmigaCD

A release is a GitHub release with the binary attached and a changelog. It is
made **by hand, on demand**. No push, pull request or ordinary build ever
publishes one; `.github/workflows/release.yml` has `workflow_dispatch` as its
only trigger.

## Making one

Actions -> **release** -> **Run workflow**, on `master`:

| Input | Meaning |
|---|---|
| `tag` | e.g. `v20261016`. Empty means `v<today, UTC>`. Must not exist yet; a second release on the same day takes a suffix, `v20261016.1` |
| `prerelease` | Tick for a build meant for testing rather than for everyone |
| `notes` | Optional text shown above the generated changelog, e.g. what to test first |

Or from a shell: `gh workflow run release.yml -f tag=v20261016 -f prerelease=true`.

The run then:

1. **guard** refuses any branch but `master`, and any tag that already exists.
   A release is never replaced: someone may already have installed it.
2. **build** is `build.yml` itself, called, so all of its jobs run: the
   syntax check, the ARM cross-build, the RTL benches and the host tests. If
   any of them fails, nothing is published.
3. **publish** writes the changelog, creates the tag on the built commit, and
   publishes the release with these files attached:
   - `MiSTer_AmigaCD` (stripped)
   - `MiSTer_AmigaCD.elf` (the same build with symbols, for reading crash logs)
   - `SHA256SUMS`

## The changelog

`scripts/release-notes.py <previous tag> <commit>` covers everything that
landed on `master` since the newest earlier `v*` tag, one line per merged PR:

- Each line is titled from the PR title, which GitHub writes into the merge
  commit, and grouped by its conventional prefix: Features (`feat:`), Fixes
  (`fix:`), Documentation, Build and CI, and so on.
- A title with no conventional prefix goes under "Other changes" as it is.
  That includes the `akiko:`-style area prefixes, which are not
  conventional-commit types.
- The commits inside a PR are not listed.
- The first release has no earlier tag, so it lists the 50 most recent
  changes and says so.

So the changelog is only as good as the PR titles. A title like
`feat: mount .dms floppy archives` reads well; a branch name does not. A PR
merged with no title falls back to its branch name.

To preview the notes without releasing anything:

```sh
python3 scripts/release-notes.py "$(git describe --tags --abbrev=0 --match 'v[0-9]*')" HEAD
```

`tests/test_release_notes.sh` checks the script against a scratch repository
shaped like this one's history, and runs in `make -C tests`.
