# Assessment: merging `MiSTer-devel/Main_MiSTer` @ `d8c88df`

Written 2026-08-04, before touching any code.

**Conclusion up front: this is far smaller than it looks.** The headline —
kblood's CD32/CDTV work landing upstream in `7d88019` — brings us almost
nothing, because we already have it and more. The whole merge is worth **one
8-line guard** plus four unrelated upstream commits.

## What moved

`7db21a8` → `d8c88df`, ten commits:

| Commit | | Value to us |
|---|---|---|
| `7d88019` | minimig: native CD32 (Akiko) and CDTV host support (#1258) | **~nil** — see below |
| `fe7b489` | ide_cdrom: pick the first data track when loading a CHD | already have |
| `8ac5829` | ide_cdrom: accept unquoted FILE lines in CUE sheets | already have |
| `3442c20` | ide_cdrom: read with the geometry of the track that owns the LBA | already have |
| `690121a` | ide_cdrom: use the audio track's CHD offset for CDDA reads | already have |
| `03252f5` | ide_cdrom: CHD/CUE track-geometry and CDDA-offset fixes | already have |
| `035b86f` | Mac: BlueSCSI Toolbox, CD changer, CD-ROM images (#1255) | unrelated, cheap |
| `1c01394` | OSD: lookahead for expanded menu entries (#1256) | unrelated, cheap |
| `663104d` | Detect PCXT-EGA variant in `is_pcxt` (#1259) | unrelated, cheap |
| `d8c88df` | Improve automatic YC PHASE_INC accuracy (#1260) | unrelated, cheap |

`7d88019` is authored by **Kasper Olesen** — it is kblood's own fork work
submitted upstream, which is why it overlaps ours so completely.

## Why `7d88019` gives us nothing

Upstream had **no** `akiko_cd32.cpp` before this commit, so from the merge base
`7db21a8` both sides *add* the file. Git will report add/add conflicts across
every CD32 file. That looks alarming and is not.

Both versions descend from the same kblood work — ours via his fork branches,
upstream's via his PR. Comparing his fork against what he upstreamed, with
comments and whitespace stripped:

| File | kblood fork | upstream | **functional delta** |
|---|---|---|---|
| `akiko_cd32.cpp` | 1452 | 1459 | **9 lines** |
| `cdtv_cd.cpp` | 783 | 783 | **0 lines** |

The raw diff is ~1,145 lines. Essentially all of it is comment de-narration and
the debug/trace removal — and we already took the trace removal ourselves in
PR #10.

Our versions are supersets: 1572 and 796 code lines respectively, carrying the
physical-disc path, per-game NVRAM, disc-name overrides and autoboot that
upstream has no equivalent of.

### The one thing worth taking

Of those 9 lines, one is a real fix:

```c
static inline bool cd32_active(void)
{
    return (minimig_config.cpu & 0x03) == 3
        && ((minimig_config.chipset >> 2) & 7) == 6
        && minimig_config.hardfile[0].cfg == 2;
}
```

applied at the top of `akiko_cd32_poll()`:

```c
if (!cd32_active()) return;
```

**We have no equivalent.** Our Akiko poll runs unconditionally on every Minimig
poll, including when the core is configured as a CDTV, where it can do nothing
useful. The AmigaCD CD32 profile sets `cpu = 0x03`, `chipset = 0x18` (AGA) and
`hardfile[0].cfg = 2`, so the guard is true for CD32 and false for CDTV —
exactly the intent.

The other two lines are `user_io_file_mount(path, 0)` gaining two explicit
default arguments. Cosmetic.

## Contested files

Sixteen of the 22 files upstream touched are ones we have modified:

| File | our commits | resolution |
|---|---|---|
| `support/minimig/akiko_cd32.cpp` | 51 | **ours** (superset) |
| `menu.cpp` | 35 | **ours** |
| `ide_cdrom.cpp` | 18 | **ours** (we have all 5 fixes) |
| `user_io.cpp` | 15 | ours + their 13-line Mac hook |
| `video.cpp` | 14 | **theirs** (YC accuracy; our changes are elsewhere) |
| `support/minimig/minimig_config.cpp` | 11 | **ours** |
| `ide.cpp`, `cdtv_cd.cpp` | 8, 8 | **ours** |
| the rest | 1–4 | case by case |

## Recommended approach

**Do not cherry-pick `7d88019`.** It would replace our supersets with subsets.

Two options, in order of preference:

**1. Real merge, ours-wins rule for CD32.** `git merge mister-upstream/master`,
then resolve every conflict in `support/minimig/`, `ide_cdrom.*` and `ide.cpp`
by taking ours, and take theirs for `video.*`, `support/mac/*` and the OSD
lookahead in `file_io.cpp`. Verbose but mechanical, and it establishes a merge
base so the *next* upstream sync is cheap — which matters now that our
foundation is mainline and will keep moving.

**2. Cherry-pick only what is new.** The `cd32_active()` guard, plus the four
unrelated commits. Smaller and safer, but leaves no merge base, so every future
sync re-does this analysis.

Option 1 is the better investment provided the resolution rule is applied
mechanically rather than by reading 1,400 lines of conflict.

Either way the `cd32_active()` guard should land, and it is independently
useful — worth doing first, on its own, so it can be tested in isolation.

## What this changes strategically

Our foundation is now mainline MiSTer. That should make this fork *smaller*
over time rather than larger: the Akiko and CDTV host code no longer needs
carrying as a fork-only asset, and future upstream fixes to it arrive for free.

What stays ours: the physical-disc path, per-game NVRAM and disc-name
overrides, autoboot, the AmigaCD console OSD, and the MiSTer Floppy port. None
of that exists upstream and none of it is on a collision course with it.

**Verification requirement:** every option here touches the CD32 boot path, so
nothing merges without the standing hardware checks — CD32 from CHD, physical
CD mount, Lotus fire button, ADF in df0, and saved config surviving a reload.
