# MiSTer Floppy — hardware notes

Bring-up notes for the Mr Floppy SNAC board, written after getting two drives
working. Everything here is about the physical side; the software side is in
[the integration plan](superpowers/plans/2026-08-02-misterfloppy-integration.md).

## The ribbon cable decides which drive is which

This cost the most time during bring-up, so it goes first.

A PC floppy ribbon cable has a **twist** in wires 10–16, which swaps DRVSA/DRVSB
and MOTEA/MOTEB. That twist is the entire reason two identical drives can share
one cable: 3.5" PC drives are usually hardwired DS1 with no jumper at all, and
the twist is what makes one of them answer as A instead.

| Position | Drive | AmigaCD OSD label |
|---|---|---|
| After the twist (end connector) | A | `External Drive 0/A` |
| Before the twist (middle connector) | B | `External Drive 1/B` |

**The twist must fall between the two drives.** Symptom when it does not: one
drive works perfectly, two do not. Both drives answer as the same drive and
conflict. Observed 2026-08-03 with the cable connected the wrong way round, so
the twist sat outside the pair rather than between them — reversing it fixed it
with no software change.

A "universal" 3.5/5.25 cable (e.g. CablesOnline FF-001) carries four connectors:
two 5.25" edge and two 3.5" header, normally one of each on either side of the
twist. Using the two 3.5" headers works **only** if they straddle the twist.

## Drive type detection

The Advanced page reports what the board senses:

```
   = Detected (IBM/PC)      cable straight
   X Detected (Shugart)     cable crossed
     Not Detected
```

Read from `mister_floppy_status` in `Minimig.sv` — bit 6 detected, bit 7 IBM/PC
vs Shugart, bit 8 cable orientation. This is the core's own answer, not
something userspace infers, so trust it over assumptions about the drive.

`MiSTerFloppyRAWIO.v` senses the type over I2C on USER_IO pins 0 and 3.
Detection therefore keeps working even when the data pins are broken, which is
worth knowing: **"Detected" does not mean "readable"**.

IBM/PC mode limits the board to two drives, which the OSD enforces
(`driveMask &= 0x03`) rather than offering drives that cannot answer.

## Pin map

From `MiSTerFloppyRAWIO.v`. Useful when something is half-working.

| USER_IO | Signal |
|---|---|
| 0 | I2C SDA |
| 1 | Write data |
| 2 | PIN5 (index, crossed cable) |
| 3 | I2C SCL |
| 4 | PIN9 (read data, straight cable) |
| 5 | PIN8 (index, straight cable) |
| 6 | PIN6 (read data, crossed cable) |

## SW[1] on the DE10-Nano must be off

`sys_top.v` gives USER_IO pins **2, 4 and 5** to HDMI I2S audio when the board's
`SW[1]` slide switch is on, and forces `user_in[2/4/5]` high. Those carry read
data and index, so the board would still be detected over I2C while never
reading a disk.

This is the physical switch on pin W24, **not** an ini setting — `hdmi_audio_96k`
is unrelated (it selects the 96 kHz sample rate). If a dock takes I2S audio off
the user port, that is mutually exclusive with Mr Floppy; they are the same pins.

## Motor handling with two drives

`MiSTerFloppySHUGART` is instantiated as `#(28687500, 1)` — the `1` is
`AmigaMode`, which latches the motor **per drive** on select
(`nDriveLatchedA` on DriveSelect0, `nDriveLatchedB` on DriveSelect1). The Amiga
shares one motor line and expects the drive to latch it at select time, so this
is required for two drives; without it both motors follow the same signal.

All four PC control lines are driven: DRVSA (pin 14), DRVSB (pin 12),
MOTEA (pin 10), MOTEB (pin 16).

## Write support

The board carries a **write jumper**. With it disabled no write can reach the
media regardless of what the software does, which is the recommended state until
you have a reason otherwise.

On the software side, ADF images write back through `WriteTrack()` gated on
`DSK_WRITABLE`. Flux images (SCP/IPF) are read-only by construction:
`InsertFloppy` deliberately omits `DSK_WRITABLE` for them, so the padlock always
shows on a flux mount and no write reaches the file. That is correct behaviour,
not a bug to chase.
