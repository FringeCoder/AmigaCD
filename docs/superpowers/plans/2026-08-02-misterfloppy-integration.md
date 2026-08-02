# MiSTer Floppy Integration Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring RobSmithDev's MiSTer Floppy work — flux-accurate SCP/IPF disk images and
real-drive support over the SNAC port — into the AmigaCD core, so real Amiga floppies
(dumped with a Greaseweazle) and protected originals both work, across **two** drives in the
console OSD, with ADF write-back and real-drive writes included.

Two drives now because that is what the Mr Floppy board presents; `extDrives[]` is sized 4 so
growing to the Amiga maximum later is a menu change, not a data-structure change.

**Architecture:** Rob's fork splits cleanly. Seven new `MiSTerFloppy*` RTL modules plus a
rewritten `paula_floppy.v` are drop-in files; only `minimig.v`, `userio.v` and `Minimig.sv`
need merging against kblood's work. Userspace gains a `FluxFile` abstraction with `SCPFile`
and `IPFFile` implementations, driven from a reworked `minimig_fdd.cpp`. The real-drive
path (`MiSTerFloppySHUGART`) is instantiated inside `paula_floppy.v` and cannot be excluded
without forking that file, so the RTL is taken whole and the hardware path is gated in
userspace and the OSD instead.

**Tech Stack:** Verilog/SystemVerilog (Quartus Prime Lite 17.0.2), C++14 (arm-none-linux-
gnueabihf 10.2), GitHub Actions cross-build, MiSTer DE10-Nano.

## Global Constraints

- **PRs target `develop`.** Always `gh pr create --base develop`; the repo default is `master`.
- **Core branch is `amigacd-core`** (`a27f4e8`), hardware-verified 2026-08-01. Do not move off it.
- **CONF_STR must stay `"AmigaCD;..."`** — `is_amigacd()` prefix-matches it, and the console
  OSD, per-game NVRAM and physical-disc paths all depend on that resolving.
- **No unit-test framework exists in this tree.** There is no pytest/gtest equivalent and none
  is being added. Every task's verification is (a) CI cross-build green with no new warnings,
  and (b) a named hardware check. Where a step below says "verify", it means run that exact
  check and read the output — not assume.
- **Upstream sources are read-only references.** Remotes `robsmith-main` (userspace) and
  `robsmith-core` (RTL) are fetched for extraction only; never push to them.
- **Every RTL change costs a ~40 minute Quartus run.** Batch RTL edits; do not iterate on
  hardware one line at a time.
- **Timing baseline: `amigacd-core` passes with +0.017 ns worst-case slack and no failing
  setup paths** (measured 2026-08-02). Any RTL task that produces a failing path has regressed
  and must stop, not be tuned around.

## Reference pins

| Repo | Ref | Commit |
|---|---|---|
| `RobSmithDev/Main_MiSTer` | `master` | `353089b` (2026-06-15) |
| `RobSmithDev/Minimig-AGA_MiSTer` | `MiSTer` | `3b2c3dd` (2026-06-14) |
| Rob's userspace fork point | `MiSTer-devel/Main_MiSTer` | `f8a198e` |
| Rob's RTL fork point | `MiSTer-devel/Minimig-AGA_MiSTer` | `eb7a26e` |

Rob's RTL forks *before* the A2065 commit (`809b955`, 2026-07-26), so his floppy work merges
onto our newer base, never the reverse.

## File Structure

**RTL — drop-in (copy verbatim from `robsmith-core/MiSTer`):**

| File | Responsibility |
|---|---|
| `rtl/MiSTerFloppyVirtualFluxDrive.v` | Serves flux from userspace-supplied buffers (the image path) |
| `rtl/MiSTerFloppyPLL.v` | Data separator / flux timing recovery |
| `rtl/MiSTerFloppyFifo.v` | Flux word FIFO between userspace and Paula |
| `rtl/MiSTerFloppySHUGART.v` | Real Shugart/PC drive over SNAC (instantiates RAWIO) |
| `rtl/MiSTerFloppyRAWIO.v` | Raw pin-level drive I/O (instantiates i2c) |
| `rtl/MiSTerFloppyi2c.v` | Mr Floppy board configuration over i2c |
| `rtl/MiSTerFloppyIBM.v` | IBM/PC format decode — **not instantiated**, carried for completeness |
| `rtl/misterfloppy.qip` | Quartus file list for the above |
| `rtl/paula_floppy.v` | Rewritten (+426): instantiates VirtualFluxDrive, PLL, SHUGART, Fifo |
| `rtl/paula.v` | +23, wires the new floppy ports through |

**RTL — merge required (kblood also modified these):**

| File | Rob | kblood | Collision |
|---|---|---|---|
| `rtl/minimig.v` | +26/−4 | +229/−9 | Different regions (Rob: instantiation ports; kblood: memory/DMA) |
| `rtl/userio.v` | +19/−10 | +7/−7 | **Same command-decode block** — Rob's hunk at ~464 sits on kblood's `cd32pad` line at 466 |
| `Minimig.sv` | +25/−3 real (ignore line-ending churn) | ours | USER_OUT mux, `mister_floppy_status`, CONF_STR |

**Userspace — new files (copy from `robsmith-main/master`):**

- `support/minimig/scpfile.{cpp,h}` — SuperCard Pro flux image reader
- `support/minimig/ipffile.{cpp,h}` — IPF via CAPS, `dlopen`s `/media/fat/linux/libcapsimg.so`
- `support/minimig/capsapi/{CapsAPI.h,Comtype.h,LICENSE.txt}` — SPS headers

**Userspace — modify:**

- `support/minimig/minimig_fdd.{cpp,h}` — `FluxFile` base class, `adfTYPE` gains `lastBit` and
  `fluxFile`, flux serving path. **Our tree has zero commits here**, so this is a clean take.
- `user_io.h` — `UIO_MM2_FLPEX01 0xFA`, `UIO_MM2_FLPEX23 0xFB`, `UIO_MM2_USRPORT 0xFC`.
  Verified free: our tree uses `0xF0`–`0xF9` only.
- `Makefile` — add `-ldl` to `IMLIB2_LIB` (needed by `ipffile.cpp`).
- `menu.cpp` — **do not take Rob's 216 lines.** They target the full Minimig OSD; AmigaCD
  jumps to `MENU_AMIGACD_MAIN1` at `menu.cpp:5828` and never renders that page. Write our own
  rows against the console menu instead (Task 4), mirroring his semantics.
- `support/minimig/minimig_config.{cpp,h}` — `mm_floppyTYPE` gains `extDrives[4]`,
  `mm_userportMode` enum (`mmup_mp32pi=0`, `mmup_misterfloppy=1`), `minimig_ConfigUserPort()`.
  Note `mm_floppyTYPEOld` exists alongside for config-version migration — take it, or saved
  configs written before this change will not load.

---

### Task 1: RTL merge, floppy dormant

Get a core that contains all the floppy RTL, builds, fits, and behaves exactly as today.
No userspace change, so nothing can drive the new logic yet. This isolates the two risks
that would sink the whole effort — Quartus area and timing — before any C++ is written.

**Files:**
- Create (copy verbatim from `robsmith-core/MiSTer`): the 8 drop-in RTL files listed above
- Modify: `rtl/paula.v`, `rtl/paula_floppy.v` (replace with Rob's), `rtl/minimig.v`,
  `rtl/userio.v`, `Minimig.sv`, `files.qip`
- Work in: `upstream/core-cd32-cdtv`, new branch off `amigacd-core`

- [ ] **Step 1: Branch and extract the drop-in files**

```bash
cd upstream/core-cd32-cdtv
git checkout -b feature/misterfloppy amigacd-core
for f in MiSTerFloppyVirtualFluxDrive MiSTerFloppyPLL MiSTerFloppyFifo \
         MiSTerFloppySHUGART MiSTerFloppyRAWIO MiSTerFloppyi2c MiSTerFloppyIBM; do
    git show robsmith-core/MiSTer:rtl/$f.v > rtl/$f.v
done
git show robsmith-core/MiSTer:rtl/misterfloppy.qip > rtl/misterfloppy.qip
git show robsmith-core/MiSTer:rtl/paula_floppy.v  > rtl/paula_floppy.v
git show robsmith-core/MiSTer:rtl/paula.v         > rtl/paula.v
```

- [ ] **Step 2: Confirm paula.v took cleanly**

kblood did not modify `paula.v` or `paula_floppy.v`, so overwriting is safe — but prove it
rather than assume:

```bash
git diff --stat amigacd-core -- rtl/paula.v rtl/paula_floppy.v
```

Expected: changes confined to those two files. If `git log amigacd-core --oneline -- rtl/paula.v`
shows any kblood commit, **stop** — the overwrite would discard core work and this task needs
a real merge instead.

- [ ] **Step 3: Add the qip to the build**

Append to `files.qip`:

```tcl
set_global_assignment -name QIP_FILE [file join $::quartus(qip_path) misterfloppy.qip]
```

- [ ] **Step 4: Merge `rtl/minimig.v`**

Rob adds a port block and instantiation wiring; kblood's changes are in the memory/DMA
regions. Apply Rob's hunks by hand:

```bash
git diff eb7a26e..robsmith-core/MiSTer -- rtl/minimig.v
```

The five hunks add: `USER_IN`/`USER_OUT`/`user_port_mode`/`mister_floppy_status` to the module
port list, and pass them down to `paula` and `userio`. Nothing kblood touched is in those
regions — verify by checking each hunk's context line still exists unmodified before applying.

- [ ] **Step 5: Merge `rtl/userio.v` — the one real collision**

Rob's hunk near line 464 lands in the same `IO_STROBE` command-decode block as kblood's
joystick line. Ours currently reads:

```verilog
if (joystick_cfg_sel) {joy_swap, cd32pad, joy_ana_en} <= {IO_DIN[3], ~IO_DIN[1] & IO_DIN[2], IO_DIN[1]};
```

That line **must survive** — it is the CD32 pad fix from PR #7, verified on hardware. Add
Rob's `user_port_mode` decode alongside it, do not replace the block wholesale.

- [ ] **Step 6: Merge `Minimig.sv`**

Diff with whitespace ignored (his fork has different line endings; the raw diff shows 2180
bogus lines, the real change is 25):

```bash
git diff --ignore-all-space eb7a26e..robsmith-core/MiSTer -- Minimig.sv
```

Apply: `reg [2:0] mister_floppy_status`, the `status_menumask` addition, the
`USER_OUT = user_port_mode ? IndirectUserOutFlop : IndirectUserOutmt32` mux, the
`minimig` instantiation ports, and the `userport_change_reset` logic.

**Keep our CONF_STR line `"AmigaCD;UART115200:230400,MIDI;"` unchanged.** Rob's says
`"Minimig;"` — taking it would break `is_amigacd()` and with it the console OSD, per-game
NVRAM and the physical-disc path.

- [ ] **Step 7: Build in Quartus and read the fitter report**

This is the gate for the entire plan. Record from the compilation report:

- Total logic utilisation (ALMs) before vs after
- TimeQuest worst-case slack and TNS

**Baseline, measured on the `amigacd-core` build 2026-08-02:**

```
This design does not contain any failing setup paths.
The worst-case slack is 0.017 ns.
```

So the gate here is **binary, not comparative**: today the design meets timing, and the only
acceptable result is that it still does. Do not reason about TNS deltas — if any setup path
fails, that is the stop condition.

Note the earlier ~−1.9 to −2.1 ns TNS figures in this project's history came from
`amiga-console` builds and do **not** describe the current core. `amigacd-core` is the better
design; it actually closes.

But 17 ps is not headroom — it is a design sitting exactly on the line, so expect this to need
attention rather than to sail through.

**Device utilisation, same build (`5CSEBA6U23I7`, Quartus 17.0.2):**

| Resource | Used | Free |
|---|---|---|
| ALMs | 22,619 / 41,910 (54%) | **19,291** |
| Block memory | 1,977,756 / 5,662,720 (35%) | 65% |
| DSP blocks | 67 / 112 (60%) | 45 |
| PLLs | 3 / 6 (50%) | 3 |
| Pins | 145 / 314 (46%) | 169 |

Three things make that less alarming than it sounds:

- `MiSTerFloppyPLL.v` is a **digital data separator**, not a hardware PLL. It has zero Altera
  megafunction references and runs on the existing `clk`/`clk7_en` 28 MHz domain. It consumes
  no PLL resource and introduces **no new clock domain**, so there are no new cross-domain
  paths to close.
- The floppy logic all sits on that same slow 28 MHz domain, well away from the SDRAM/CPU
  paths where the critical path actually lives.
- **The device is only 54% full.** ~2,100 lines of control-path Verilog with no DSP or memory
  demands should land in the low thousands of ALMs — call it 2–4 points of utilisation against
  19,291 free. Routing congestion is not a plausible limiter at this fill level, and the
  fitter has ample room to place the new logic away from the critical path. The SNAC pins are
  already declared in `sys_top` as `USER_IN`/`USER_OUT`, so no new pins are needed either.

So the honest read is that Task 1 is **likely to be routine**. The residual risk is not area
but placement luck: 17 ps is thin enough that a fitter reshuffle alone could tip an unrelated
path negative, and a re-run with a different seed may be all that is needed. Treat a single
marginal failure as noise to re-run, not as proof the port cannot work.

If it genuinely will not close, the fallback is to fork `paula_floppy.v` and remove the
`MiSTerFloppySHUGART` instantiation, dropping the real-drive path to buy back area for the
image path. On these numbers that should not be necessary.

- [ ] **Step 8: Hardware regression — nothing should have changed yet**

Flash the RBF, deploy the current `develop` binary unchanged, then:

- A CD32 game boots from CHD and runs
- The physical CD drive still mounts (`Real CD` row)
- The fire button still works in Lotus Trilogy's in-game menu (guards the `userio.v` merge)
- An ADF still mounts and boots in `df0` (guards `paula_floppy.v`)

Any failure here is a merge error in Steps 4–6, not a floppy bug.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "minimig: add MiSTer Floppy RTL (flux drive, PLL, SHUGART), dormant

Ports RobSmithDev's floppy RTL from Minimig-AGA_MiSTer @ 3b2c3dd onto
amigacd-core. Seven new MiSTerFloppy* modules plus his paula_floppy.v and
paula.v are taken verbatim; minimig.v, userio.v and Minimig.sv are merged by
hand against kblood's work.

userio.v was the only real collision: Rob's user_port_mode decode lands in the
same IO_STROBE block as the cd32pad joystick line, which is kept intact.

CONF_STR stays AmigaCD. No userspace change, so the new logic is unreachable --
this commit exists to prove the core still fits and still meets timing."
```

---

### Task 2: Userspace flux plumbing + SCP

Make `.scp` images mountable in `df0`. This is the payoff for the Greaseweazle: dump on the
PC with `gw read --format amiga.amigados out.scp`, copy over, mount.

**Files:**
- Create: `support/minimig/scpfile.{cpp,h}`
- Modify: `support/minimig/minimig_fdd.{cpp,h}`, `user_io.h`, `Makefile`
- Work in: main repo, new branch off `develop`

**Interfaces:**
- Produces: `class FluxFile` (abstract) in `minimig_fdd.h` with `openFile`, `closeFile`,
  `fluxReady`, `fluxMode`, `selectTrack`, `fluxRead`, `unFluxRead`, `fluxDummyRead`;
  `class SCPFile : public FluxFile`; `adfTYPE.fluxFile` and `adfTYPE.lastBit`;
  `DSK_FLUXMODE 0x100`, `FLUXMODE_RAWFLUX 1`, `FLUXMODE_DENSITY 2`
- Consumes: Task 1's RTL (`UIO_MM2_FLPEX01`/`02`/`USRPORT` decode)

- [ ] **Step 1: Take the floppy core files**

Our tree has **zero** commits to these, so this is extraction, not merging:

```bash
git checkout -b feature/flux-images develop
git show robsmith-main/master:support/minimig/minimig_fdd.h   > support/minimig/minimig_fdd.h
git show robsmith-main/master:support/minimig/minimig_fdd.cpp > support/minimig/minimig_fdd.cpp
git show robsmith-main/master:support/minimig/scpfile.h       > support/minimig/scpfile.h
git show robsmith-main/master:support/minimig/scpfile.cpp     > support/minimig/scpfile.cpp
```

- [ ] **Step 2: Prove the "zero commits" claim before trusting Step 1**

```bash
git log --oneline 0f3a6ad..develop -- support/minimig/minimig_fdd.cpp support/minimig/minimig_fdd.h
```

Expected: empty. If not empty, **stop** — the overwrite discards our work and these files
need merging instead.

- [ ] **Step 3: Add the UIO opcodes**

In `user_io.h`, after `#define UIO_MM2_JOY 0xF9`:

```c
#define UIO_MM2_FLPEX01 0xFA
#define UIO_MM2_FLPEX23 0xFB
#define UIO_MM2_USRPORT 0xFC
```

Verified free — our tree uses `0xF0`–`0xF9` only.

- [ ] **Step 4: Add `-ldl` to the Makefile**

`ipffile.cpp` (Task 3) needs it, and adding it now keeps the two tasks independent:

```make
IMLIB2_LIB  = -Llib/imlib2 -lfreetype -lbz2 -lpng16 -lz -lImlib2 -ldl
```

- [ ] **Step 5: Exclude `ipffile.cpp` for now**

Do **not** copy `ipffile.cpp`/`.h` or `capsapi/` in this task. The Makefile globs `*.cpp`
under `support/`, so an unbuilt IPF file would break the build. IPF is Task 3.

- [ ] **Step 6: Build via CI**

```bash
git push -u origin feature/flux-images
gh run list --repo FringeCoder/AmigaCD --branch feature/flux-images --limit 1
```

Expected: green, no new warnings. Likely first-failure mode is a missing `file_io.h` helper
Rob added — check `git diff f8a198e..robsmith-main/master -- file_io.h` and take what is needed.

- [ ] **Step 7: Hardware check — ADF must not regress**

`minimig_fdd.cpp` was replaced wholesale, so the existing ADF path is the thing most at risk.
Deploy, then mount a known-good ADF in `df0` and boot it. If ADF broke, the flux work is
irrelevant until it is fixed.

- [ ] **Step 8: Hardware check — SCP mounts**

Dump a disk on the PC:

```bash
gw read --format amiga.amigados disk.scp
```

Copy to the MiSTer, mount in `df0` via the existing Floppy row, boot it. Success is the disk
booting; the OSD will not yet *say* anything about flux mode — that is Task 5.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "minimig: flux-capable floppy path + SCP image support

Takes RobSmithDev's minimig_fdd rework and SCPFile from Main_MiSTer @ 353089b.
Our tree had zero commits to minimig_fdd.{cpp,h}, so this is an extraction
rather than a merge.

Adds the FluxFile abstraction (adfTYPE gains fluxFile and lastBit), the
DSK_FLUXMODE status bit, and UIO opcodes 0xFA/0xFB/0xFC -- verified free, our
tree used 0xF0-0xF9 only.

IPF is deliberately excluded here; it needs libcapsimg and lands separately."
```

---

### Task 3: IPF support

**Files:**
- Create: `support/minimig/ipffile.{cpp,h}`, `support/minimig/capsapi/{CapsAPI.h,Comtype.h,LICENSE.txt}`

- [ ] **Step 1: Extract**

```bash
git checkout -b feature/ipf-images develop
mkdir -p support/minimig/capsapi
for f in CapsAPI.h Comtype.h LICENSE.txt; do
    git show robsmith-main/master:support/minimig/capsapi/$f > support/minimig/capsapi/$f
done
git show robsmith-main/master:support/minimig/ipffile.h   > support/minimig/ipffile.h
git show robsmith-main/master:support/minimig/ipffile.cpp > support/minimig/ipffile.cpp
```

- [ ] **Step 2: Note the runtime dependency in the file header**

`ipffile.cpp:85` does `dlopen("/media/fat/linux/libcapsimg.so", RTLD_NOW)`. That library is
**not in this repo and is not open source** — it is the SPS/CAPS decoder, distributed
separately. Add a comment at the top of `ipffile.cpp` recording that IPF silently degrades to
"unsupported" when the `.so` is absent, so nobody debugs a missing feature that is really a
missing file.

- [ ] **Step 3: Build via CI**

Expected: green. `-ldl` was added in Task 2.

- [ ] **Step 4: Hardware check**

Install `libcapsimg.so` to `/media/fat/linux/`, mount an `.ipf`, boot it. Then **rename the
`.so` and mount the same IPF again** — it must fail cleanly with a log line, not crash. That
second half is the check that matters, because most users will not have the library.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "minimig: IPF image support via libcapsimg

Adds RobSmithDev's IPFFile and the SPS CAPS headers. The decoder itself is
dlopen'd from /media/fat/linux/libcapsimg.so at runtime and is not vendored --
it is not open source. IPF degrades to unsupported when it is absent."
```

---

### Task 4: AmigaCD OSD — two drives, external-drive selection, detection line

Rob's 216 lines of `menu.cpp` target the full Minimig OSD, which AmigaCD never renders
(`menu.cpp:5828` jumps to `MENU_AMIGACD_MAIN1`). Write our own rows, borrowing his semantics
so the two behave identically where they overlap.

Three deliverables: a second floppy drive on the main menu, per-drive selection between an
image and a real external drive, and the board-detection line.

**Files:**
- Modify: `menu.cpp` (AmigaCD console menu only), `support/minimig/minimig_config.cpp`
  (`amigacd_apply_system` floppy drive count)

**Interfaces consumed (from Rob, via Tasks 2-3):**
- `minimig_config.floppy.extDrives[4]` — per-drive: `0` = use the mounted image,
  `1` = External Drive 0/A, `2` = External Drive 1/B, `3`/`4` = drives 2/3
- `minimig_config.userport` of type `mm_userportMode`: `mmup_mp32pi=0`, `mmup_misterfloppy=1`
- `minimig_ConfigUserPort(mm_userportMode)` and `minimig_ConfigFloppy(drives, speed)`
- `osdMask` bit 6 (`64`) = board detected, bit 7 (`128`) = IBM/PC vs Shugart,
  bit 8 (`256`) = cable straight (`=`) vs crossed (`X`). These are `mister_floppy_status`
  from `Minimig.sv`, landing above `mt32_cfg`/`mt32_available` in `status_menumask`.
- `DSK_WRITABLE` — absent means write-protected, rendered by Rob as the padlock glyph `\x17`

- [ ] **Step 1: Raise the AmigaCD default drive count to 2**

`amigacd_apply_system()` currently pins a single drive:

```c
if (minimig_config.floppy.drives < 1) minimig_config.floppy.drives = 1; // single df0
```

**Determine the encoding before changing anything.** Rob's OSD loops `i <= minimig_config.floppy.drives`,
which implies `drives` is a count-minus-one (`0` = df0 only, `1` = df0+df1). If that holds,
the existing line already yields two drives and this step is a no-op. Confirm by reading
`minimig_ConfigFloppy` and the RTL `floppy_config` decode:

```bash
grep -n "minimig_ConfigFloppy" support/minimig/minimig_config.cpp
grep -n "floppy_config" upstream/core-cd32-cdtv/rtl/*.v
```

Write down which it is in the commit message. If it is a plain count, change the `1` to `2`
and re-check that `amigacd_apply_system` is not clamping it back down elsewhere.

- [ ] **Step 2: Add the DF1 row to the main menu**

The main menu is currently 9 rows (`menumask = 0x1FF`, PR #8). Adding DF1 makes it 10:

| menusub | Row |
|---|---|
| 0 | CD |
| 1 | Real CD |
| 2 | **Floppy DF0** |
| 3 | **Floppy DF1** (new) |
| 4 | System |
| 5 | Settings |
| 6 | Advanced |
| 7 | Save settings |
| 8 | Reset |
| 9 | Exit |

Set `menumask = 0x3FF` and renumber every handler case in `MENU_AMIGACD_MAIN2`. **This file's
row numbering has bitten us before** — PR #8 renumbered nine rows and every handler had to
move with them. Enumerate all `menusub ==` occurrences in the handler and check for gaps and
duplicates before building:

```bash
awk 'NR>=6580 && NR<=6800 && /menusub ==/ {print NR": "$0}' menu.cpp
```

Also update the two submenu return points, which currently set `menusub = 4` (Settings) and
`menusub = 5` (Advanced) on Back — they become `5` and `6`.

- [ ] **Step 3: Render both floppy rows with image, flux, write-protect and external drive**

Factor the row into a helper so DF0 and DF1 cannot drift apart:

```c
// One floppy row. `unit` is 0 or 1; `sel` is the menusub this row owns.
static void amigacd_floppy_row(int m, int unit, int sel)
{
    char s[64];
    snprintf(s, sizeof(s), " Floppy %d : ", unit);

    if (df[unit].status & DSK_INSERTED)
    {
        char *p = strrchr(df[unit].name, '/');
        strncat(s, p ? p + 1 : df[unit].name, 18);
        if (df[unit].status & DSK_FLUXMODE)     strcat(s, " [flux]");
        if (!(df[unit].status & DSK_WRITABLE))  strcat(s, " \x17"); // padlock
    }
    else
    {
        // No image: show the external drive assignment if Mr Floppy owns the
        // user port. Letters vs digits track the detected drive type, matching
        // Rob's menu.cpp:5831 -- A/B for IBM/PC, 0/1 for Shugart.
        int ext = (minimig_config.userport == mmup_misterfloppy)
                ? minimig_config.floppy.extDrives[unit] : 0;
        switch (ext)
        {
            case 1:
                strcat(s, "External Drive ");
                strcat(s, (osdMask & 64) ? ((osdMask & 128) ? "A" : "0") : "0/A");
                break;
            case 2:
                strcat(s, "External Drive ");
                strcat(s, (osdMask & 64) ? ((osdMask & 128) ? "B" : "1") : "1/B");
                break;
            default:
                strcat(s, "* no disk *");
                break;
        }
    }
    OsdWrite(m, s, menusub == sel, 0);
}
```

Call it twice from `MENU_AMIGACD_MAIN1`:

```c
amigacd_floppy_row(m++, 0, 2);
amigacd_floppy_row(m++, 1, 3);
```

- [ ] **Step 4: Handle selection on both rows**

`select` opens the file browser; `minus`/`plus` cycle the external-drive assignment when Mr
Floppy owns the user port, so a real drive can be attached without a disk image:

```c
else if (menusub == 2 || menusub == 3) // Floppy DF0 / DF1
{
    int unit = menusub - 2;
    if (minus || plus)
    {
        if (minimig_config.userport == mmup_misterfloppy)
        {
            // 0 = image, 1 = external A/0, 2 = external B/1. Two drives today;
            // extDrives[] is sized 4 for the eventual Amiga maximum.
            int e = minimig_config.floppy.extDrives[unit];
            e = plus ? (e + 1) % 3 : (e + 2) % 3;
            minimig_config.floppy.extDrives[unit] = e;
            if (e) { df[unit].status = 0; FileClose(&df[unit].file); }
            menustate = MENU_AMIGACD_MAIN1;
        }
    }
    else if (select || recent)
    {
        minimig_config.floppy.extDrives[unit] = 0;   // an image overrides the external drive
        df[unit].status = 0;
        fs_Options = SCANO_DIR;
        fs_MenuSelect = MENU_AMIGACD_ADFFILE_SELECTED;
        fs_MenuCancel = MENU_AMIGACD_MAIN1;
        strcpy(fs_pFileExt, "ADFSCPIPF");
        if (select) SelectFile(Selected_F[unit], "ADFSCPIPF", fs_Options, fs_MenuSelect, fs_MenuCancel);
        else if (recent_init(unit)) menustate = MENU_RECENT1;
    }
}
```

`MENU_AMIGACD_ADFFILE_SELECTED` currently hardcodes `df[0]`. It must insert into the unit the
row owns. Add a file-scope `static int amigacd_fdd_unit = 0;`, set it immediately before
`SelectFile`, and use it in the selection handler in place of the literal `0`.

- [ ] **Step 5: Backspace ejects the row's own drive**

The existing handler is `else if (c == KEY_BACKSPACE && menusub == 2)`. Extend to both:

```c
else if (c == KEY_BACKSPACE && (menusub == 2 || menusub == 3))
{
    int unit = menusub - 2;
    df[unit].status = 0;
    FileClose(&df[unit].file);
    menustate = MENU_AMIGACD_MAIN1;
}
```

- [ ] **Step 6: Extend the file filter to flux formats**

Change `ADF` to `ADFSCPIPF` (or `ADFSCP` if Task 3 was skipped — offering a filter for a
format that cannot open produces a confusing failure at mount time).

Verified this works: `file_io.cpp:1455` and `:1752` walk the filter in 3-char groups
(`ext += 3`), and `fs_pFileExt` is `char[13]`, so three extensions fit with room to spare.
`file_io.cpp:1437` returns early for filters longer than 3 chars, but that is the
*display-name* path deciding whether to strip the extension — with a multi-extension filter
the extension stays visible in the listing, which is what we want since `.adf` and `.scp`
otherwise look identical.

- [ ] **Step 7: Add the User Port row and detection line to the Advanced page**

`MENU_AMIGACD_ADVANCED1` (added in PR #8) currently holds Ethernet and Back, `menumask = 0x3`.
Add User Port above Ethernet, making it 3 selectable rows (`menumask = 0x7`), with the
detection line rendered beneath as **non-selectable** (`OsdWrite(..., 0, 1)`):

```c
strcpy(s, " User Port: ");
strcat(s, (minimig_config.userport == mmup_misterfloppy) ? "MiSTer Floppy" : "MT32-Pi");
OsdWrite(m++, s, menusub == 0, 0);

// Detection line. Bits are mister_floppy_status from Minimig.sv:
//   64 = board detected, 128 = IBM/PC (else Shugart), 256 = straight cable (else crossed).
if (minimig_config.userport == mmup_misterfloppy)
{
    if (osdMask & 64)
    {
        strcpy(s, "   ");
        strcat(s, (osdMask & 256) ? "=" : "X");           // cable: straight / crossed
        strcat(s, (osdMask & 128) ? " Detected (IBM/PC)"  // drive type
                                  : " Detected (Shugart)");
    }
    else strcpy(s, "   Not Detected");
    OsdWrite(m++, s, 0, 1);
}
```

Cycling the row calls `minimig_ConfigUserPort()`. **The SNAC port is shared with MT32-pi** —
`Minimig.sv` muxes `USER_OUT` on `user_port_mode` and resets `mt32pi` when it changes
(`userport_change_reset`), so the two are mutually exclusive by construction. Renumber
Ethernet to `menusub == 1` and Back to `menusub == 2`, and update both Back return points.

- [ ] **Step 8: Build via CI**

Expected green. Watch for `Selected_F[]` bounds — it is indexed per file-selector slot, and
DF1 needs its own.

- [ ] **Step 9: Hardware check**

Without the Mr Floppy board attached, everything except detection is still testable:

- DF0 and DF1 both mount ADFs independently, and both boot
- A write-protected ADF (`chmod a-w`) shows the padlock; a writable one does not
- An SCP shows `[flux]`
- **Write-back works**: boot Workbench in DF0, save a file to a writable ADF, reset, confirm
  it persists. This is the first exercise of `WriteTrack()` in this project.
- Advanced shows `User Port: MT32-Pi`; switching to `MiSTer Floppy` shows `Not Detected`
- Settings, Advanced, Save, Reset and Exit all still work — the renumbering in Step 2 is the
  most likely thing to have broken

With the board attached, the detection line should read `Detected (Shugart)` or
`Detected (IBM/PC)` with the cable glyph, and cycling a floppy row with `-`/`+` should offer
the external drives.

- [ ] **Step 10: Commit**

```bash
git add menu.cpp support/minimig/minimig_config.cpp
git commit -m "AmigaCD: two floppy drives, external-drive selection, board detection"
```

Full message should record: the row renumbering (9 -> 10, `menumask` `0x1FF` -> `0x3FF`), the
`floppy.drives` encoding determined in Step 1, and that `extDrives[]` is sized 4 for the Amiga
maximum with two exposed today.

---

### Task 5: SNAC real-drive support

**Blocked on hardware.** Needs the Mr Floppy board on the SuperDock's SNAC port. Do not start
until Tasks 1–4 are verified, because a failure here is otherwise indistinguishable from a
failure there.

- [ ] **Step 1: Add the user-port mode row to the Advanced page**

The SNAC port is **shared with MT32-pi** — `Minimig.sv` muxes `USER_OUT` on `user_port_mode`.
Both cannot be active. Add a row to `MENU_AMIGACD_ADVANCED1` (added in PR #8) selecting
MIDI/MT32-pi vs Floppy, sent with `UIO_MM2_USRPORT`.

- [ ] **Step 2: Hardware check**

Real disk in the drive, read a directory, boot it. Then switch the row back to MT32-pi and
confirm MIDI still works — the mux resets `mt32pi` on change (`userport_change_reset`), so
this is the regression most likely to be missed.

---

### Task 6: Greaseweazle on the MiSTer (optional, independent)

Everything above needs no Greaseweazle integration — dumping on a PC and copying the `.scp`
over is the supported workflow, and Task 2 makes it useful.

This task is only worth doing to skip the PC round-trip. It has **no upstream to mine** and
is genuinely novel work.

- [ ] **Step 1: Establish whether the kernel can even see the device**

Before designing anything, plug the Greaseweazle into the MiSTer and run:

```bash
lsusb; ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null; zcat /proc/config.gz 2>/dev/null | grep -i cdc_acm
```

If there is no `cdc_acm` and no `/dev/ttyACM0`, this task requires a kernel module the MiSTer
does not ship, and it should be abandoned rather than worked around.

- [ ] **Step 2: Only if Step 1 succeeds — scope the protocol work**

The Greaseweazle host protocol is a documented serial command set. A minimal read path is:
open port, `GetInfo`, `Seek`, `ReadFlux` per track, decode to SCP, write to `/tmp`, hand the
path to the existing mount code from Task 2. Budget a full read at 30–60 s per disk; this is
a "press a row, wait, then it mounts" flow, never real-time.

**Do not attempt real-time drive emulation over USB.** Paula reads flux transitions with
microsecond timing; USB latency is milliseconds. That gap is why the SNAC path (Task 5)
exists and cannot be substituted in software.

---

## Risks

| Risk | Signal | Response |
|---|---|---|
| Quartus timing | Task 1 Step 7 reports **any** failing setup path (baseline: passes, +0.017 ns) | Re-run first â 17 ps is thin enough that fitter placement alone can tip it. Only if it reproduces: fork `paula_floppy.v`, drop `MiSTerFloppySHUGART` |
| `userio.v` merge breaks CD32 pad | Fire button dead in Lotus in-game menu | Re-check the `cd32pad` line survived Step 5 verbatim |
| `minimig_fdd.cpp` overwrite breaks ADF | Task 2 Step 7 | Diff our file against `0f3a6ad` to find what was assumed absent |
| IPF unusable | `libcapsimg.so` missing | Expected for most users; must degrade cleanly, not crash |
| SNAC contention with MT32-pi | MIDI dies after enabling floppy | Task 5 Step 2 explicitly tests the reverse direction |

## Out of scope

- **Nothing about write is excluded** — but note what "write support" actually means here,
  because the three cases differ and only two exist:

  | Path | Write? | Where |
  |---|---|---|
  | ADF images | **yes** | `WriteTrack()` in `minimig_fdd.cpp:618`, sector write-back via `FileWriteSec`, gated on `DSK_WRITABLE` (set from the file's read-only attribute at `:767`) |
  | Real drive over SNAC | **yes** | RTL (`MiSTerFloppySHUGART`), and interlocked by the board's write jumper |
  | SCP / IPF flux images | **no such thing** | `FluxFile` declares no write method at all — `fluxRead`, `unFluxRead`, `fluxDummyRead`, `selectTrack` and nothing else. Flux images are read-only by Rob's design, not by a decision in this plan. |

  So ADF write-back comes with Task 2 for free, real-drive write comes with Task 5, and flux
  write cannot be "added" without designing an encoder Rob never wrote.
- `MiSTerFloppyIBM.v` is carried but never instantiated — PC-format decode is not wired up.
- Rob's `menu.cpp` changes, his `minimig_config.cpp` changes (+105) beyond what Tasks 2–4
  require, and his N64/Apple IIgs commits, which are unrelated upstream merges.
