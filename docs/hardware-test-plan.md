# Hardware test plan

Everything in PRs #52–#57, plus the menu fixes added since, was written without
a machine. Twenty-three changes, none verified. This is the list, ordered so
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

### 1c. The two new rows

| Check | Pass |
|---|---|
| `Advanced -> FDD Turbo` | Reads `On`/`Off` and toggles. It has never been reachable from this OSD, so **there is no prior behaviour to compare against** |
| With it On, load a multi-disk ADF game | Faster than with it Off. No difference means the setting is not reaching the core |
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

## 2. With a disc

| Check | Pass | Watch for |
|---|---|---|
| Disc detail under the CD row | Volume label and `N trk  M CDDA` | An empty label is **normal** — Simon the Sorcerer ships as `CD32_TEST` |
| `Write .mgl for this disc` | Toast says `Wrote <name>.mgl`, file appears beside the disc | |
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
