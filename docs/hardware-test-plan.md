# Hardware test plan

Everything in PRs #52–#56 was written without a machine. Sixteen changes, none
verified. This is the list, ordered so that the things other things depend on
come first.

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
