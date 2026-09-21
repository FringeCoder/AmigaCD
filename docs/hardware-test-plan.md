# Hardware test plan

Everything in PRs #52–#57, plus the menu fixes added since, was written without
a machine. Thirty-three changes, none verified. This is the list, ordered so
that the things other things depend on come first.

The value here is not the steps — those are mostly obvious — it is the **failure
modes**. Several of these fail silently or misleadingly, and knowing which
before you start is the difference between a finding and an afternoon.

Mark each row as you go. A finding is worth more than a tick.

---

## 0. Before anything: a Kickstart

Nothing below runs without one, and the ROM path is the most-changed code in
this batch.

| | |
|---|---|
| **Do** | Boot the core with your Amiga Forever ROMs and `ROM.KEY` in place. CD32 profile. |
| **Pass** | The machine boots to the CD32 splash. |
| **Watch for** | **A grey or flashing screen with no message.** The encryption has no integrity check of any kind, so a wrong decryption produces a ROM-sized block of plausible nonsense and the only symptom is a machine that does not boot. It will not tell you the ROM is wrong, because nothing in the file says so. |

### 0a. The case that was actually broken

| | |
|---|---|
| **Do** | Same, with an **extended** ROM configured as well (`Settings → Ext ROM`). This is the normal CD32 setup. |
| **Pass** | Boots, and a disc boots from it. |
| **Why** | Before this batch, `LoadRomSlot` refused every encrypted ROM — to the log, not the screen — and `ApplyConfiguration` fell back to uploading the main ROM alone. A CD32 with encrypted ROMs came up with **no extended ROM**, which does not boot a disc. If this works, the fix is real. |

### 0b. The message

| | |
|---|---|
| **Do** | Move `ROM.KEY` aside. Boot. |
| **Pass** | The boot screen says `Encrypted ROM but no ROM.KEY found`, not `Unsupported ROM size`. |
| **Why** | Three failures used to arrive as one line about size, and this is the likeliest one — an encrypted ROM has exactly the right size and fails a different guard. Put `ROM.KEY` back afterwards. |

---

## 1. Navigation — needs nothing but the OSD

No disc, no game. Worth doing in one pass because four of these are new routes
into pages this core could never open, and each has a way back that was wrong in
a different way.

| Check | Pass |
|---|---|
| `Settings → Video & Audio` opens | The Amiga's own page: TV Standard, Stereo mix, Audio Filter, Paula Output, Adjust screen position |
| Back from it | Returns to **Settings**, on the Video & Audio row — not the main page, and not Save settings |
| `Advanced → MT32-pi settings` opens (with MT32-Pi as the User Port tenant) | Synth, Munt ROM, soundfont |
| Back from it | Returns to **Advanced**, on the MT32-pi row |
| MT32-pi row with another tenant selected | Greyed, and SELECT does nothing |
| `Configs → Load a config` and `→ Save to a config slot` | Both open; back from each returns to **Configs**, on the row it came from |
| SNAC `Port 1` / `Port 2` still cycle their modes | They were renumbered twice in this batch |
| `Advanced → Fast-IDE` | Reads `On`/`Off` on a CD32; `N/A (CDTV)`, `N/A (no IDE)` or `N/A (68000)` elsewhere, greyed |
| Build date, bottom line of the main page | Matches the core you just copied. **This is the one that makes every other test trustworthy** — see §4 |
| `Real CD` row with no USB drive attached | `* no drive found *`, greyed; SELECT says `No CD-ROM drive found` |

**Fast-IDE's sense is inverted** in the config (`ide_cfg & 0x20` *clear* means
on). The row was copied from the Drives page rather than reasoned about, so if
it reads backwards, that is the thing to check.

---

### 1a. Row hints

The bottom line now says what the selected row does, instead of one message per
page. Park on a row for about a second and a half and it appears.

| Check | Pass |
|---|---|
| Arrow down the main page, pausing on each row | The bottom line changes with the row |
| The pause | Long enough that scrolling through the page does not flash the line in and out, short enough that stopping on a row answers you. **This is the one that needs a person** — 1500 ms was chosen by argument, not by feel |
| Every hint | Starts with the core name and build date, then ` - `, then the row's text. That prefix is how §4 stays checkable |
| `Configs`, `Settings`, `Advanced` | Same behaviour on each; the last row of each page (`Back`) must have its own text |
| `Advanced` with SNAC selected as the tenant, then without | The hints must follow the rows that shift, not stay where they were. `Ethernet`, `Fast-IDE` and `Back` are the ones that move |
| A row whose hint names a key — `CD`, `Reset`, `exit`, `Save state` | The key named is the one that works |

**A missing hint is silent.** A row with no entry falls back to the core name
alone, which is exactly what the line said before this change — so a hint that
was never written looks like a page that has none. If a row shows only the name
and build date, that is the finding.

The row count is guarded at compile time (one hint per selectable row, checked
against each page's `menumask`), so the failure that reaches hardware is a hint
that is *wrong*, not one that is missing.

---

### 1b. Swapping a floppy

The floppy rows used to eject on SELECT, which meant the recent-disc key ejected
too. Both now open a chooser with a disk already in.

| Check | Pass |
|---|---|
| SELECT on `Floppy 0` with a disk in | The file browser opens. It does **not** eject |
| The ` key on the same row | The recent list opens, showing disks used before |
| Pick a different disk | It swaps; the Amiga sees a disk change, as on a first insert |
| **Cancel out of the browser instead** | The original disk is **still mounted**. This is the regression to watch: `df[fu].status` is no longer cleared when the browser opens, and if it were, a cancel would silently eject |
| Swap four or five times over | No slowdown, no failure to open. `InsertFloppy` does not close the outgoing file itself, so a leaked descriptor per swap would show up here first |
| Backspace and A+B on the row | Still eject, as the hint says |
| Swap in df1, then in df0 | Each row swaps its own drive |
| Assign `External Drive 0` with LEFT/RIGHT, then SELECT and **cancel** | The assignment is **still there**. It used to be cleared before the browser opened, so a cancelled press dropped it |
| Assign an external drive, then actually pick an image | The image wins and the assignment goes |

### 1c. The two new rows

| Check | Pass |
|---|---|
| `Advanced -> Drives` | Opens the new page. FDD Turbo moved here from Advanced |
| `Drives -> FDD Turbo` | Reads `On`/`Off` and toggles. It has never been reachable from this OSD before #59, so **there is no prior behaviour to compare against** |
| With it On, load a multi-disk ADF game | Faster than with it Off. No difference means the setting is not reaching the core |
| `Drives -> df0 drive` / `df1 drive` | Cycles off -> the drives the board actually carries. **This is the same walk as LEFT/RIGHT on the main page's Floppy rows, through one shared helper** -- if the two disagree, the extraction is wrong |
| A drive already taken by the other unit | Skipped. An IBM/PC cable carries two, so 3 and 4 are skipped on one |
| With the User Port set to anything but MiSTer Floppy | Both drive rows greyed, and two lines say to change the User Port. SELECT on a greyed row does nothing |
| The `df0`/`df1` status lines below | Show the mounted image, a padlock when not writable, and the flux mark on SCP/IPF. Informational only -- **mounting stays on the main page** |
| LEFT/RIGHT on the main page's Floppy rows | **Still works.** The shortcut was kept deliberately; removing it to reward the new page would be a regression |
| Back from Drives | Returns to **Advanced**, on the Drives row -- which is row 6 with SNAC as the user port tenant and row 4 without |
| `Configs -> Reset <machine> to defaults` | Names the machine you are on. Asks. `No` returns to Configs |
| `Yes` | RAM, ROMs, CPU and IDE go back to the profile, and the machine restarts |
| Discs and hard disks after it | **Still mounted.** The page promises this |
| Re-open the OSD after it | Settings read as defaults, and nothing was written to disk unless you press Save settings |
| On a machine whose default ROM is missing | Toasts which ROM, and still restarts |

### 1d. Reset now asks

| Check | Pass |
|---|---|
| `Reset` on the main page | A confirm page. `No` returns to the Reset row, not to exit |
| `Yes` | Restarts |
| The wording on a CD32 | Names the last save state |
| The wording on an A500 | Says the machine cannot take a save state -- it is a 68000, and `AmigaCD.sv` refuses them |
| **Ctrl+LAlt+RAlt** | Resets **straight away, with no confirmation.** Deliberate: it is the binding for when the Amiga is wedged and the OSD will not open. If this starts asking, that is the bug |

---

### 1e. Two that only show up in the log or the corner

| Check | Pass |
|---|---|
| Mount **only** `Hard Disk 1`, no CD and no HD0, on a CD32 or A1200 | `Save state` works and names the file after that HDF. It used to refuse: only HD0 was consulted |
| Then mount HD0 as well | The name comes from HD0, as the row order implies |
| `dmesg`/the MiSTer log after browsing every page of this OSD | **No** `OSD: line N is past the ... lines` messages. The diagnostic is new; any line it prints names a page whose rows do not fit and a row nobody can see |

**The OSD line diagnostic has no test and cannot have one here.** It fires from
a draw that only runs on real hardware, and the state it reports requires a page
edited to overflow. It is in the plan rather than the test suite for that
reason: the check is that it stays quiet, and if it ever speaks, believe it --
the Minimig System page hid its Ethernet row this way for several commits.

---

### 1e2. The save state preview draw path

Rewritten to build in a cached shadow and copy once, and to decache the image.
Both came from reviewing theshaneobrien's overlay PoC; neither cost is measurable
from here.

| Check | Pass |
|---|---|
| Arrow between save slots on `Settings` | The preview changes **without a visible stall**. It used to clear the bank with a per-pixel loop and blend into uncached DDR -- two full passes, ~197 ms each at 1080p on Shane's measurement. If it still hitches, the shadow is not helping and the cost is elsewhere |
| At 1080p versus 720p | Both feel the same. A cost that scales hard with resolution means the DDR pass still dominates |
| **Save a slot, view it, re-save the same slot, view it again** | The **second** view shows the **new** capture. This is the repeat-image bug: imlib keys its cache on the path and a thumbnail is overwritten in place, so a stale decode shows the previous capture |
| Move off the slot and back repeatedly | No leak and no slowdown over a few dozen passes -- the shadow is malloc'd per draw now |
| A slot with no thumbnail, then one with | Falls back to the core's own video, then draws; no black frame held |

### 1e3. The cursor/menumask guard

New, from `docs/state-todo.md` U1. Every page draws its cursor by comparing
`menusub` against a literal at each row, and declares the row set separately in
`menumask`. When the two disagree the symptom is invisible: the System page had
two rows at `menusub == 11` for as long as FDD Turbo existed, and the row below
could never be toggled. There is now a runtime check that with the cursor on a
selectable row exactly one row comes out inverted.

The verdict is tested — `tests/test_osd_rowguard.cpp`, falsified against seven
mutations. The **tally** is not, and cannot be here: it counts real
`OsdWriteOffset()` calls.

| Check | Pass |
|---|---|
| Walk the cursor over **every row of every page** of this OSD with the log open | **No** `OSD: menu state ... expected exactly one` lines. The check is that it stays quiet |
| `Configs -> Load config`, cursor on a slot that has a second info line | Still quiet. That page inverts two rows deliberately and calls `OsdRowTallyExempt()`; if it speaks, the exemption is not being reached |
| Deliberately break one page and confirm the check speaks | Change one row literal on the System page to duplicate its neighbour (e.g. the `menusub == 12` on `Save slot` to `11`), rebuild, put the cursor on row 11: **one** line naming the state, `2 of them selected`. This is the only falsification available for the tally half |

If the third check does not print, the guard is not wired up and the first two
checks mean nothing — a diagnostic that cannot speak reads exactly like a clean
bill of health. Do it before trusting the other two.

---

### 1f. Screen position, and that it is per video mode

`Settings -> Video & Audio` now reports the current mode above the adjust row.
The point of the line is that the stored position is keyed by video mode, which
nothing said before.

| Check | Pass |
|---|---|
| Open the page with no disc, on a CD32 | The line above `Adjust screen position` reads the current mode, e.g. `320x256: not adjusted yet` |
| `Adjust screen position`, move with the arrows, Enter | Toasts `Stored` **and the four edge values**, e.g. `[123-456, 12-34]`, on two lines. The mode line then **immediately** reads `position saved` -- not after the next mode change |
| Those four numbers against the `VMODE: store position:` log line | The same four, in the same order. That is the point of matching the format |
| Esc instead of Enter | `Canceled`, the picture reverts, and the line still says what it said before |
| Reload the core, reopen the page | Still `position saved`. It lives in `<core>_vadjust.dat`, not in the config slots |
| `Save settings`, then `Configs -> Load a config` | The screen position is untouched either way. It is deliberately not part of a config |
| **Boot a game that runs at another resolution** | The line reads that resolution and `not adjusted yet`, and the picture is at the default position. **This is the behaviour the line exists to explain, not a bug** |
| Adjust in that mode too, then switch back and forth | Each mode keeps its own position |
| Before the Kickstart has produced video | `(no video mode reported yet)` rather than a stale or invented mode |

**Why the values are a toast and not a live readout.** Selecting the row sets
`menustate = MENU_NONE1` -- the OSD CLOSES before adjust mode starts, and that is
exactly why the arrows reach the core instead of the menu. So while adjusting
there is no menu surface to draw on, and the toast at the end is the one moment a
value can be shown. A live readout was written and reverted for this reason.

**There is now a test pattern, and it is the least verified thing in this
document.** `Adjust screen position (grid)` switches the Amiga's picture for a
border, a centre crosshair and a grid, generated in the core from `hde`/`vde` so
the border lands on the first and last active pixel and line. Adjust until all
four edges are visible and the picture is both fully on screen and centred.

| Check | Pass |
|---|---|
| Select the row | The picture is replaced by a grid on black with a white border |
| The border | **All four edges present.** A missing edge is the bug -- the right and bottom are drawn from the PREVIOUS line and frame, so they are the ones that can be wrong |
| Move with the arrows | The whole pattern moves, border included |
| The first frame after enabling | May lack the right and bottom edge for one frame. By design; invisible at 50 Hz. If it persists, the size measurement is broken |
| Enter | Pattern goes, picture returns, `Stored` toast with the values |
| Esc | Pattern goes, picture returns, position reverts |
| Grid spacing | Every 32 pixels across, 16 lines down -- close to square on PAL lores. If it looks nothing like square, the aspect is not what it should be |
| Centre crosshair | Actually at the centre of the border, not offset |
| With a scandoubler or hq2x on, and with gamma | The pattern scales with them. It is muxed into the mixer INPUT precisely so that it does |
| A CD32, a CDTV, an A500 and an A1200 | Works on each; the pattern knows nothing about the machine |
| **Timing after a Quartus fit** | **Unverified. This is the one to check first.** It adds a 24-bit 2:1 mux on the mixer's input path in a design with 0.117 ns of setup margin. If the fit regresses, that mux is the first suspect |

**Everything about its appearance is unverified**, including whether the grid is
legible, whether white-on-black is right for a CRT, and whether 2-pixel borders
are visible after scaling. The geometry is benched
(`rtl/sim/testpattern/tb_video_testpattern.sv`, thirteen checks, five mutations);
nothing in simulation can say what it looks like.

**The two cheaper routes were checked and are dead**, recorded so nobody spends
time on them again:

- **The OSD or info window.** `sys/osd.v` caps `infow`/`infoh` at 504 pixels, but
  the real objection is that the info window is positioned INSIDE the active area
  and therefore moves with the adjustment being made. It cannot show where the
  screen edges are.
- **The HPS framebuffer** (what the save state thumbnails use). It replaces core
  video and is not subject to the core's blanking, so a pattern drawn there says
  nothing about the window being adjusted.

What is still missing even with the pattern: a mode cannot be calibrated before
something displays it. The pattern shows the mode the core is in, so it does not
help populate a preset for a resolution no software has selected yet.

---

## 2. With a disc

| Check | Pass | Watch for |
|---|---|---|
| Disc detail under the CD row | Volume label and `N trk  M CDDA` | An empty label is **normal** — Simon the Sorcerer ships as `CD32_TEST` |
| `Write .mgl for this disc` (Advanced page) | Toast says `Wrote <name>.mgl`, file appears beside the disc | |
| Press it again | `Already current: <name>.mgl` | Not `Wrote` — that would mean the no-change path is broken |
| Hand-edit the `.mgl` (add a `<reset/>`), deploy a new core, press again | `Updated <name>.mgl`, and **your `<reset/>` is still there** | If the edit is gone, the in-place rewrite is not working and it fails silently |
| Launch that `.mgl` from the MiSTer menu | Core loads with the disc mounted | Also exercises the `index 4` path from #52 |
| `CD32 (2x)` on a title that streams from CD | Loads are ~4× slower and the game still works | |
| `Clear NVRAM` on a disc with a real save | Confirm page names the file; afterwards the row reads `(new)` | **Reset before checking in-game** — the machine holds the old NVRAM until then |
| NVRAM row generally | Names a `cd32-<hash>.nvr` file | `NOT SAVING` means a load failed its verify and writes are being refused — that state was invisible before |

---

## 3. Save states

| Check | Pass |
|---|---|
| Slot row with nothing mounted | `(no media)` |
| Slot row, empty slot | `(empty)` |
| After a save | `16 Sep 21:35` — the date it was written |
| `Ctrl+Alt+F1` mid-game | Saves; `Ctrl+Alt+F2` restores |
| **The Amiga must see neither key** | Nothing happens in the game itself. The combo is consumed, press and release |
| `Ctrl+Alt+F3` | Steps the slot, toast says which |
| On a 68000 or with Z3 fast RAM | The hotkeys toast a refusal instead of acting |

---

## 4. Deployment

Do this **first** in practice, since it is how everything else gets onto the
machine — it is last here only because it is about the tooling rather than the
core.

| Check | Pass |
|---|---|
| `./scripts/deploy-core.sh --dry-run` | Names the core and what it would prune |
| `./scripts/deploy-core.sh` | Copies, and the core browser shows the date in its own column |
| Two dated cores on the card afterwards | `--keep 2` leaves the new one and the previous one |
| `./scripts/deploy-menu.sh` | Sends `menu.rbf` and reports the spare refreshed |
| `./scripts/deploy-menu.sh --check` | `menu.rbf matches the spare` |
| Run `update_all`, then `--check` again | If it took the Menu core, this says so — and `--restore` puts it back |

---

## Still open, not from this batch

These predate #52 and are unrelated to it: `timing-probe.sh diagnose`,
`vamigats.sh kickstart`, a Quartus fit on `AmigaCD.qpf`, Z2 8 MB cold boot with
a CD, the save-state restore round trip, Hybris sprites, preset debounce by
feel, `collect-diag.sh`, Menu core M3 (the SNAC reader has no hardware
verification on that core at all), vAmigaTS with Kickstart 1.3, and the CD32
probe burn.

`docs/vamigats.md` and `docs/timing-probe.md` cover those.
