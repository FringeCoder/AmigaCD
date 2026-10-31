# CRT look over HDMI: scanlines and the CRT_Beam filters

Written 2026-10-31 for a Philips 32PW9551: a 16:9 CRT with a vertical-stripe
(aperture grille) tube, an HDMI input, Pixel Plus and 100 Hz Digital Scan. Most
of it holds for any display. **Nothing here has been checked on a screen yet**;
it is worked out from the code, and each claim cites where.

## Scanlines: already in the core

`Video & Audio -> Scandoubler FX -> CRT 25% / 50% / 75%` (`menu.cpp`,
`config_scanlines_msg`). It is not analog-only: the scaler's input is taken
*after* the framework's scanline stage (`sys/sys_top.v`, `VGA_scanlines` and
`hr_out = vga_data_sl`), so HDMI gets them too.

- The core line-doubles (PAL 288 -> 576) and darkens every second line. At
  `video_mode_pal=3` (720x576@50) with `vscale_mode=1`, the scaler then runs at
  exactly 1x vertically and each dark line lands on one output line.
- Turning it on also makes the pixel clock adaptive (`AmigaCD.sv`, `ce_out`:
  28 MHz is forced only when neither `status[42]` nor the scandoubler is on).
- None in interlaced screens (`scandoubler = ... & ~lace`) or RTG
  (`sl_r <= FB_EN ? 0 : scanlines`). Both are by design.
- Cost: the line doubler holds one line, ~64 us. No frame of lag.

## The CRT_Beam filters

An aperture-grille tube does not blur adjacent pixels along a line, and fed a
clean digital signal it shows pixel edges harder than a TV of the Amiga's day
did. That softness came from the beam spot. These filters put it back, in the
scaler's horizontal polyphase filter, which works per line and adds no lag.

| File | Use | What it is |
|---|---|---|
| `CRT_Beam/Beam_Light.txt` | horizontal | Gaussian spot, sigma 0.35 source pixels |
| `CRT_Beam/Beam_Medium.txt` | horizontal | sigma 0.45 |
| `CRT_Beam/Beam_Soft.txt` | horizontal | sigma 0.55 |
| `CRT_Beam/Vertical_Sharp.txt` | vertical | nearest neighbour |

Copy `filters/CRT_Beam/` to `/media/fat/Filters/CRT_Beam/`
(`scripts/stage-output.sh` stages it), then in `Video processing`:

- Horz filter: From file -> `CRT_Beam/Beam_Medium.txt`
- **Vert filter: From file -> `CRT_Beam/Vertical_Sharp.txt`.** The default,
  "Same as Horz", would run the beam vertically too. A beam blends its
  neighbours even at phase 0, so it would smear the scanlines.
- Scan filter: leave on "Same as Vert".

As defaults:

```ini
vfilter_default=CRT_Beam/Beam_Medium.txt
vfilter_vertical_default=CRT_Beam/Vertical_Sharp.txt
```

Where it acts: only where the scaler resamples horizontally. Lores (320 wide)
is widened to the 4:3 area and gets it. Hires (640) is narrowed to fit a
720-wide mode with a 4:3 picture, which runs ascal's downscale path; whether
the output filter still shapes that path has not been checked.

The files are generated: `tools/crt-beam-filters.py` writes them and
`--check` (in CI) fails on a hand edit or on a file `read_video_filter()` would
silently replace with nearest neighbour. To change a strength, change the
script and rerun it.

## The rest of the setup, for that TV

- `video_mode_pal=3`, `video_mode_ntsc=2`, `vsync_adjust=2`, `vscale_mode=1`.
  50 Hz at 50 Hz, so the low-latency mode only nudges the clock ~0.2%.
- `custom_aspect_ratio_1=15:16`, selected with the core's Aspect Ratio. Main
  always signals 16:9 (`video.cpp`, ADV7513 register 0x17) and 576p goes out
  as VIC 18, while the scaler sizes the picture as if pixels were square:
  540x576 square-pixel units is a 4:3 picture on a 16:9 tube. If the TV ignores
  the flag and shows 4:3, plain 4:3 is right instead. Check with the core's
  calibration pattern (`status[55]`): circles should be round.
- On the TV: Pixel Plus off and Sharpness down. Pixel Plus is edge enhancement,
  which is the opposite of what the beam filter does, and it is processing lag.
- No shadow mask filter: the tube has a real one.
