# Main_MiSTer — CD32 / CDTV native-bridge fork

This is a fork of [MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer)
that adds **native CD32 (Akiko) and CDTV (CR-511) CD support** to the Minimig
userspace driver, so CHD images mount directly without IDEFix97, the
CD32-Emulator HDF launcher, or any other workaround layer.

The matching RTL fork is at
[kblood/Minimig-AGA-cd32-cdtv-native-akiko](https://github.com/kblood/Minimig-AGA-cd32-cdtv-native-akiko)
— both are needed; the userspace driver here talks to the FPGA bridges over the
custom UIO channels added in that core.

**Status:**
- **CD32 (Akiko):** native bridge boots a wide compatibility fleet —
  Cannon Fodder, Banshee, Beneath a Steel Sky, Microcosm, Sim City CD32 and
  many others, including titles that require fast-RAM authenticity tweaks.
- **CDTV (CR-511):** M1 splash + M2 Phase-1b complete — Dune intro animation
  runs end-to-end from a native CHD.

Work happens on the `cd32-native-mvp` branch. `master` tracks upstream
MiSTer-devel for ease of merging upstream changes.

Build with the cross-compile recipe documented at
[mister-devel.github.io](https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/).

---

# Main_MiSTer Main Binary and Wiki Repo

This repo serves as the home for the MiSTer Main binaries and the Wiki.

For the purposes of getting google to crawl the wiki, here's a link to the (not for humans) [crawlable wiki](https://github-wiki-see.page/m/MiSTer-devel/Wiki_MiSTer/wiki)

If you're a human looking for the wiki, that's [here](https://github.com/MiSTer-devel/Wiki_MiSTer/wiki)

To compile this application, read more about that [here](https://mister-devel.github.io/MkDocs_MiSTer/developer/mistercompile/#general-prerequisites-for-arm-cross-compiling)
