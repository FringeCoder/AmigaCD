# Userspace branch move: `amiga-console` → `amigacd-userspace`

**Date:** 2026-08-07
**Status:** assessment. Not executed.
**Repo:** `kblood/Main_MiSTer-cd32-cdtv`

The same move we made on the core side (`amiga-console` → `amigacd-core`, 2026-08-01) is
available on the userspace side and has not been made. This is the gained/lost analysis,
done by content rather than by commit identity — the two lines were rebased, so no commit
hash survives the crossing.

## Verdict

**Move.** `amigacd-userspace` is a content superset of our pin, and it lands us on the same
upstream base as the physical-disc fork we have to keep merging from.

## The two lines

Fork point `fd1aabe` (2026-06-09, "cdtv: retry-until-acked play-end STCH").

| | `amiga-console` (our pin) | `amigacd-userspace` |
|---|---|---|
| Tip | `52625a4`, 2026-06-14 | `2aa07c5`, 2026-07-29 |
| Commits past fork | 6 | 67 |
| Upstream Main_MiSTer | **Release 20260325** | **Release 20260707** + through 2026-07-29 |

## Gained

**All five AmigaCD commits are present, rebased.** Same subjects, same order:

| Ours | Theirs |
|---|---|
| `52625a4` AmigaCD: add "Save settings" to the console OSD | `2aa07c5` |
| `a568034` AmigaCD: add D-Cache toggle to Settings, default OFF | `63ee4cf` |
| `377e877` AmigaCD: browse the "Amiga" games folder | `c3e44d3` |
| `d3dad15` AmigaCD: keep stock MiSTer common menus reachable | `e13255e` |
| `b90f151` AmigaCD: simplified console OSD + per-system profiles | `30379f9` |

**Four months of upstream Main_MiSTer**, including `7317947` "ide: assert DSC on ATAPI SEEK
so VIDE-CDD.SYS stops stalling", `df0538a` A2065 Ethernet host support (the userspace half
of what `amigacd-core` gained on the RTL side), `2025dc7` CD-i track boundaries, `36e752a`
EDID read after HPD/cold boot, `14052d2` Bluetooth pairing menu lockups, N64 64DD, SNES SA1.

**`ed3ebf4`** — clears the A2065 DDR3 control words even when the card is off. Pairs with
the RTL we already took.

**Cleanups** — `63d848d` removes debug instrumentation and fork-local build files;
`449449e` / `a5aa683` de-narrate the CD32/CDTV comments, matching what `amigacd-core` did to
the RTL.

## Lost

**One commit, deliberately.** `0e0562e` "akiko: Deep Core garble-localization debug
instrumentation (read-only)" is not on the new line — because `63d848d` on that line removes
debug instrumentation as a matter of policy. It is read-only diagnostics, not behaviour.

Nothing else is unique to `amiga-console`. Unlike the core move, there is no equivalent of
the `chipRD_dma` question — no functional change is dropped.

## The reason this matters more than it looks

`docs/upstream-pins.md` records the physical-disc fork's base as upstream Main_MiSTer
`7317947` (2026-07-13).

- `amigacd-userspace` **contains** `7317947`.
- `amiga-console` **does not** — our line predates it by four months.

Every merge from `Anime0t4ku/Main_MiSTer_Physical_Disc` currently has to cross that four-month
gap. The three commits that landed 2026-08-07 alone touch 37 files, +3341/−2985, concentrated
in `user_io.cpp`, `support/psx/psx.cpp`, `support/saturn/saturncdd.cpp` and
`support/pcecd/pcecdd.cpp` — files we carry our own versions of. Much of that diff is not
physical-disc's work at all; it is upstream drift we have not taken.

Moving first, then merging physical-disc, should shrink that conflict surface substantially.

## Sequencing

Do **not** fold this into the PSX SNAC branch. That work is mid-flight with an unresolved
core timing failure, and mixing a 67-commit rebase into it would make both unreviewable.

Suggested order:

1. Finish or park PSX SNAC Plan 2.
2. Move to `amigacd-userspace`, on its own branch, and re-verify the line numbers in
   `docs/integration-plan.md` — `docs/upstream-pins.md` warns about exactly this.
3. Then merge the physical-disc changes, against the aligned base.

## Also found

The local `upstream/main-cd32-cdtv` clone is checked out on **`master`** (`22a7759`,
2026-05-23). `docs/upstream-pins.md` warns that `master` on kblood's forks "tracks plain
upstream Main_MiSTer and contains none of the CD32 work". Anything read out of that working
tree recently was read against the wrong branch. `scripts/fetch-upstream.sh` checks out
`amiga-console`, so this was changed by hand at some point.
