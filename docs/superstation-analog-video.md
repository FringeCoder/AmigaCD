# SuperStation: PAL composite and RGB at the same time

Confirmed working on hardware, 2026-10-18.

The SuperStation has an RGB-to-composite encoder on board. Main_MiSTer's
`vga_mode=subcarrier` is the mode for exactly that kind of board: RGB stays on
the analog port, composite sync goes out on HSYNC and the colour subcarrier on
VSYNC, and the encoder builds composite from them. Both outputs carry the same
picture at once. `vga_mode=cvbs` and `svideo` are the wrong modes here — they
put the encoded signal on the RGB pins instead of RGB.

## Setting

In `/media/fat/MiSTer.ini`:

```ini
[AmigaCD]
vga_mode=subcarrier   ; also forces composite_sync=1 and forced_scandoubler=0
vga_scaler=0
```

The section name is the first field of the core's `CONF_STR`.

PAL needs nothing extra: `video.cpp` `set_yc_mode()` treats a refresh below
55 Hz as PAL and sends a 4.43361875 MHz subcarrier. `ntsc_mode=1` gives PAL-60
colour on 60 Hz screens; without it they get NTSC colour.

## Board switches

`sys/sys_top.v:177`: `SW[3]` selects the digital I/O board mode, which
tri-states the analog video and audio pins (`av_dis`). It must be off for any
analog output. On boards where the MCP23009 I/O expander is detected
(`mcp_en`), the switch is ignored and the expander's mode bit decides instead.
On the SuperStation the switches are on the right side of the case. The
working setup was confirmed with all four down. Whether down means off there,
and whether those switches are wired to `SW[3:0]` at all, has not been checked:
flipping SW3 and watching whether analog goes dark would settle both.

## When composite loses colour

`sys/sys_top.v:1466` gates the subcarrier off unless the analog output is the
core's own 15 kHz video. In this core that happens with:

- the OSD scandoubler effect (hq2x or scanlines), which makes the output 31 kHz;
- RTG/Picasso96 screens, which route analog through the scaler
  (`AmigaCD.sv` `assign VGA_SCALER = FB_EN`);
- `vga_scaler=1` or `vga_mode=ypbpr`.

Native chipset screens are unaffected.

## Checking it from the log

On every video mode change:

```
Calculated YC parameters for 'AmigaCD_50.0': PAL PHASE_INC=...
Sending YC config to FPGA: 0x04 (pal_en=1, cvbs=0, yc_en=0)
Sending subcarrier enable to FPGA: 1
```

A per-mode `PHASE_INC` override goes in `yc.txt`, keyed by the name in quotes
on the first line (core name and measured refresh, so it may read `49.9`).
