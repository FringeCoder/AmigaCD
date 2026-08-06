# RTL

`snac_psx.v` is the canonical copy of the PSX SNAC reader. It lives here, in the
userspace repo, for two reasons:

- It is shared by two core repos (the Menu core fork and the AmigaCD core), and a
  module with two consumers needs one home rather than two drifting copies.
- This is the repo with CI, so this is where its testbench can run on every push.

Core repos take a **copy**. When this file changes, both copies must be refreshed —
`upstream/core-menu/rtl/snac_psx.v` and, from Plan 2 onward,
`upstream/core-cd32-cdtv/rtl/snac_psx.v`.

Simulation uses Icarus Verilog, not ModelSim, which is not installed on the dev
machine. See the `rtl-sim` job in `.github/workflows/build.yml`.
