# AmigaCD userspace: state and open items

Written 2026-09-18, by going back over a long working session and collecting
what was **identified and then not done**. Until now this repository had no
ranked list of open items at all — `docs/hardware-test-plan.md` covers hardware
verification, and everything else lived in commit messages and PR bodies.

That is the same failure the sibling repos were carrying and had fixed: an item
recorded only where it was decided is an item nobody finds. `FringeCoder/Menu_MiSTer`
has `docs/state-todo.md` and the core has `docs/core-accuracy-todo.md`; this is
the userspace third.

Every item below cites a file and line. Nothing here is inferred from behaviour
alone.

## How we verify

- **HOST** — `./syntax_check.sh` (94 files parse, 4 excluded with reasons) and
  `make -C tests` (compiled tests plus shell tests). Both run in CI.
- **SIM** — an Icarus bench. The RTL ones live in the core repo; this repository
  has `rtl/tb/` for the save state modules whose sources it vendors.
- **TITLE** — observation against real hardware. This is the bottleneck:
  `docs/hardware-test-plan.md` stands at **32 unverified changes**, and
  everything in it needs a MiSTer.

An item marked **[no code]** is a decision or a record, not work.

---

## U1 — The Minimig System page's row count has no compile-time guard  [HOST]  — **DONE 2026-09-21**

`menu.cpp:7322`. Every AmigaCD page ties its row count to its `menumask` with a
`static_assert` against the length of its hints table, so adding a row without a
hint fails the build. `MENU_MINIMIG_CHIPSET1` has no hints table, so it has
nothing to compare against, and its row count is stated only in a prose comment.

**That comment was wrong, and it cost real time.** Until 2026-09-17 it read "15
selectable rows: 0..10 from this fork", which missed that FDD Turbo was an
eleventh — so two rows drew at `menusub == 11`, both highlighted together, and
FDD Turbo could never be toggled because `CHIPSET2` read 11 as the save-slot
stepper. Fixed, but the class of error is still unguarded there.

Low priority for one reason: `menu.cpp:6809` redirects `MENU_MINIMIG_MAIN1` to
the AmigaCD page, so this page is reachable only when this binary runs a
non-AmigaCD Minimig core. It is the one page in the tree where a row miscount is
still silent.

Doing it means either giving that page a hints table it does not otherwise need,
or a different shape of guard. Neither is obvious, which is why it was left.

**Done, and as a different shape of guard — for every page, not that one.** The
compile-time framing was the wrong one and is worth recording as such: the row
literals are spread over sixty lines of drawing code, and only the invert *flag*
reaches `OsdWrite()`, never the `k` it was computed from. There is nothing for a
`static_assert` to count. A hints table for that page would have tied its length
to its mask and left every other page's literals unchecked.

What is checkable is the consequence. With the cursor on a selectable row,
exactly one row comes out inverted; two rows sharing a `k` give two, and a mask
bit no row draws gives none. `osd.cpp` counts rows and inverted rows per paint,
`osd_rowguard.cpp` decides, and `menu.cpp`'s `HandleUI()` reads the previous
paint's totals and warns once per menu state. It covers all ~40 pages including
the AmigaCD ones, not just `MENU_MINIMIG_CHIPSET1`.

Split so the decision is testable: `tests/test_osd_rowguard.cpp`, 16 checks,
falsified against seven mutations. The tally itself is not testable here — it
counts real `OsdWriteOffset()` calls — so it is in
`docs/hardware-test-plan.md` §1e3, with the one reproduction that can falsify it
(break a row literal on purpose and confirm the check speaks). **Run that one
first**: a diagnostic that cannot speak reads exactly like a clean bill of
health.

Two things it found:

- **One page inverts two rows on purpose.** `Configs -> Load config` renders the
  selected slot's second info line inverted so it reads as one highlighted
  block (`menu.cpp:7121`). It gets `OsdRowTallyExempt()` rather than a changed
  rendering. It is the only page in the tree that needs it; the warning names
  the state, so a future one costs one console line and two lines of fix.
- **A redundant guard, removed.** The verdict started with an explicit
  `if (!menumask) return 0;`. Dropping it changed nothing any check could see —
  a zero mask has no bit for the cursor to be on, so the cursor test already
  covers it. Confirmed in both directions (removed: suite green; re-added: suite
  green) and taken out, because a line that reads as load-bearing and is not is
  worse than no line.

## U2 — The save state preview does not use the scaler's output rect  [TITLE]

`video.cpp:3951`. The preview letterboxes the thumbnail against the whole
framebuffer, which assumes the core's picture fills the video mode.

`theshaneobrien/mister-disc-drive-support` does it properly: its
`overlay_dest_rect()` maps into the live-picture rect, taking
`ms->output_width/height` from the same DDR3 scaler header the capture reads and
scaling against `v_cur.item[1]` / `item[5]`. The overlay then lands exactly where
the game's picture is.

Ours is wrong whenever the picture does not fill the mode — the thumbnail is a
different size and position than the thing it is a picture of. Not taken with
the other two overlay fixes (2026-09-18) because it is more invasive than they
were and wanted its own round.

Needs a display to judge, which is why it is TITLE rather than HOST.

## U3 — A video mode cannot be calibrated before something displays it  [TITLE]

`support/minimig/minimig_config.cpp:1697`. Screen position is stored per video
mode, keyed `hsize | vsize<<12 | mode<<24`. The row now says so, and the test
pattern added on 2026-09-18 gives the adjustment something to align to — but
neither addresses the underlying gap: you cannot populate a preset for a
resolution no software has selected yet.

So calibrating on a CD32 boot screen calibrates that mode, and a game in hires,
interlace, NTSC or superhires still starts from a zeroed position.

The obvious fix — copy one mode's position to others — is **not obviously
correct**: `hbl_l`/`hbl_r` are pixel counts within a line, and a 320-wide and a
640-wide mode do not mean the same thing by the same number. Anyone doing this
needs to establish the scaling relationship first, on hardware, or it will
confidently store wrong positions.

## U4 — `minimig_vadjust_info()` cannot be unit tested  [HOST]  — **DONE 2026-09-21**

`support/minimig/minimig_config.cpp:1697`, reading statics fed by
`UIO_GET_VMODE` (`:1495`, `:1538`) in a file the host compiler cannot build. So
the one thing a test could pin — that the three states render the right strings
— is checked only by reading.

The fix is the one that worked for `cd_savename.cpp` and `rom_crypt.cpp`: move
the pure part into a MiSTer-header-free module and pass the mode in. Small, and
the precedent is established.

**Done.** `support/minimig/vadjust_info.{h,cpp}` now holds `vadjust_info_text()`
and `vadjust_mode_key()`, and `tests/test_vadjust_info.cpp` covers them — 26
checks, falsified against nine mutations, with the one mutation it cannot catch
recorded in its header. `minimig_vadjust_info()` is a four-line wrapper that does
the only untestable part, reading the statics the SPI side fills.

Two things came out of it that reading had not:

- **`!known && found` is a reachable state, and nothing said the code handled
  it.** `vadj_found` is a static that survives a mode change, and `store_vsize()`
  sets it without consulting `vadj_known`. The old code did return 0 on that
  path, correctly — but only by the order of its two branches, which no check
  pinned. It is now a named check. The return value is also normalised to 0/1;
  it was a raw `char` before, and callers compare it against 1.
- **The mode key was undefined behaviour at one value.** `(res & 0xFF) << 24`
  shifted an `int`; at `res & 0xFF == 255` that is `0xFF000000`, which does not
  fit in a signed `int`. Reachable only for video mode 255, but free to fix: the
  new function is unsigned throughout.

Related and already paid for: a live readout of the blanking edges was written
and **reverted**, because selecting the row sets `menustate = MENU_NONE1` — the
OSD closes before adjust mode begins, which is exactly why the arrows reach the
core. There is no menu surface to draw a readout on. Do not re-attempt it; the
values are in the `Stored` toast instead.

## U5 — `tests/test_mgl_rbf.cpp` cannot catch a `memmove` -> `memcpy` change  [HOST]

`tests/test_mgl_rbf.cpp:9` records it: the old and new `<rbf>` names overlap
whenever they differ in length, which makes `memcpy` undefined behaviour — but
glibc on this host produces the right answer anyway, so the check passes. A test
cannot depend on undefined behaviour misbehaving.

The `memmove` is correct by the standard and unverified by the suite. The ARM
build is where it would differ, if anywhere. Recorded rather than solved because
the honest options are a sanitiser run or nothing.

## U6 — Main_MiSTer's floppy config struct diverges from ours, permanently  [no code]

`docs/upstream-pins.md` has the detail. Upstream `3d7a844` puts the external
drives in a separate `mm_floppyexTYPE externalfloppy { exDrives[4] }`; ours are
`extDrives[4]` inside `mm_floppyTYPE`. Different `sizeof(mm_configTYPE)` and
different offsets for every field after `floppy`, which on this fork means
`cd_speed`, the SNAC modes and the hardfiles.

Two standing costs, not one-offs:

1. **Every future Main_MiSTer sync conflicts there.** The 2026-09-18 sync hit
   nine conflicts and was completed as a curated extraction instead of a merge.
2. **`support/minimig/scpfile.cpp:309` carries a patch that must be re-applied**
   whenever a sync takes that file. Upstream has `std::max(1LL, m_lastTime)`;
   `int64_t` is `long` on an LP64 host and `long long` on 32-bit ARM, so
   upstream's form resolves only on the target and makes the file unparseable by
   `syntax_check.sh`.

The recommendation is to stay diverged — ours works, it is migrated, and it is
the same feature. This item exists so that the next person to hit those nine
conflicts knows it is expected and why, rather than resolving their way into a
layout that misreads every config already on a card.

## U7 — A raphnet-adapted Wii arcade stick has no quirk entry  [TITLE]

`input.cpp:4173` carries `make_unique(0x289B, 0x0057, -1)` for one raphnet
adapter, because those present as multi-port devices. Nothing else in the tree
knows about them.

Raised 2026-09-18: a Tatsunoko vs Capcom stick through a raphnet Wii-to-USB
adapter does not respond to all its buttons on a SuperStation One. That device is
not MiSTer and nothing here runs on it — but the MiSTer is the instrument that
can answer it, because `input.cpp:5908` logs every `EV_KEY` and `:5955` every
`EV_ABS` with VID:PID, code and value.

Blocked on that log. If buttons arrive as axes, or some produce nothing, the
finding may deserve a quirk entry here. Until someone presses each button with
the log open there is nothing to write.

---

## Order

**Nothing HOST-only is left.** U1 and U4 were the two, and both are done, which
means every remaining item needs either a display or a log from the machine.

1. **U2, U3** — both need a display. U2 is a defined change waiting for a way to
   judge it; U3 needs a measurement before it can even be specified.
2. **U7** — blocked on one log from the person holding the hardware.
3. **U5, U6** — records. Neither is work. U6 is the one to reread before the
   next upstream sync.

The done sections are kept rather than deleted, because what each found on the
way is the kind of thing the next reader of those files should not have to
rediscover: for U4 a reachable `!known && found` state and one undefined shift,
for U1 that the compile-time framing was impossible and why.

## What is not on this list

**Hardware verification.** `docs/hardware-test-plan.md` is that list and it is
much longer than this one: 32 changes across PRs #52–#64, none of them seen on a
machine. Nothing above competes with it for priority. The ordering there —
§0 a Kickstart that boots, then §4 deployment, because that is what makes every
other result trustworthy — still holds.

**The video test pattern's Quartus fit.** It is RTL, so it belongs to the core
repo's backlog; see T27 there.
