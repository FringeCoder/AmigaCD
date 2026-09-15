# RTL

This is the userspace repository, and it has RTL in it. Two different reasons,
pointing in opposite directions, so it is worth knowing which file is which
before editing anything here.

## `snac_psx.v` is canonical here

The PSX SNAC reader lives here and both core repositories take a **copy**:

| repository | role |
|---|---|
| `FringeCoder/AmigaCD` (this one) | canonical |
| `FringeCoder/AmigaCD_MiSTer` | copy, stamped in `rtl/snac_psx.vendor` |
| `FringeCoder/Menu_MiSTer` | copy, stamped in `rtl/snac_psx.vendor` |

It is here because it is shared by two cores and belongs to neither: it is a
device protocol, not an Amiga thing. A module with two consumers needs one home
rather than two drifting copies, and putting that home inside either core would
make the other one's copy look second class.

Fix it here. Both cores then run `./snac_vendor_check.sh --stamp <this repo>`.

`snac_cd32.v` looks like its sibling and is not: it reshapes a PSX button vector
into **Minimig's** joystick order, so it is Amiga-specific and belongs to the
core. It is in the vendored list below.

## Everything else here is a copy of the core's

The save state modules — `ss_*` — are built in `FringeCoder/AmigaCD_MiSTer` and
nowhere else. They reference `AmigaCD.sv`'s wiring, the SDRAM CPU port and the
DDR3 controller; they are the core's, and every change to them has originated
there.

They are copied into this repository for one reason: **their testbenches are
here**, in `rtl/tb/`, and Icarus needs the sources beside them. That is worth
keeping — it is a second instrument on the same modules, and the core's own
source comments cite these benches — but it means the copy has to be refreshed
whenever the core moves.

`rtl/core-rtl.vendor` lists the files with their hashes and the core commit each
came from. `./core_rtl_vendor_check.sh` verifies them and runs first in CI.

## Why the stamp exists

It had already gone wrong. On 2026-09-15, `rtl/ss_ctrl.v` here was two commits
behind the core's: the core had registered its DDR3 read return — a timing fix
that took eleven of the design's worst twelve paths — and dropped the `rom_scan`
term from the SDRAM port mux. Neither had been copied over.

CI was green on both sides the whole time. `rtl/tb/ss_ctrl_tb.v`, which the
core's source comments cite as the proof of that wiring, had been running
against a module the core no longer built. Nothing anywhere recorded that these
files were copies at all, so there was nothing to notice.

This file used to say the RTL was here "because this is the repo with CI". That
stopped being true when the core grew a bench suite of its own; it now runs
twenty-odd benches per push. The reason is the testbenches, not the CI.

## Working on any of it

```sh
./core_rtl_vendor_check.sh                    # what CI runs
./core_rtl_vendor_check.sh /path/to/core      # also diff the canonical copies
./core_rtl_vendor_check.sh --stamp /path      # re-vendor and restamp
./snac_state_check.sh                         # the snac_state word order
```

After a re-vendor, run the benches — the stamp proves the bytes match, not that
they still pass. **Never restamp to clear a failing check:** restamping records
whatever is in the tree as correct, which is the one thing the check doubts.

Simulation is Icarus Verilog, not ModelSim, which is not installed on the dev
machine. See the `rtl-sim` job in `.github/workflows/build.yml`.
