# `snac_state` word order

The one thing two repositories have to agree about.

`menu.sv` packs the SNAC reader's outputs into a 112-bit vector, the framework
streams it to the host 16 bits at a time, and `snac_psx_poll()` in
`FringeCoder/AmigaCD` (`support/snac/snac_psx.cpp`) unpacks it. Nothing connects
the two but this ordering. Get it wrong and every button is a different button —
obvious the instant anyone tries a pad, invisible until then, and it looks like a
broken reader rather than a field order.

This file is the declared order. Both repositories check their own side against
it in CI, which is what makes the contract enforceable without either repository
needing access to the other.

## The order

`menu.sv`:

```verilog
assign snac_state = { snac_axes1, snac_pad1, snac_axes0, snac_pad0,
                      snac_id1, snac_id0 };
```

Verilog concatenation puts the **first operand in the most significant bits**,
and the stream sends the **least significant word first**. So the wire order is
the reverse of the way the line reads:

| word | bits | field | host reads into |
|---|---|---|---|
| 0 | `[15:0]` | `{snac_id1, snac_id0}` | `status` — `id[0] = status & 0xFF`, `id[1] = status >> 8` |
| 1 | `[31:16]` | `snac_pad0` | `pad[0]` |
| 2 | `[47:32]` | `snac_axes0[15:0]` | `axes0_lo` |
| 3 | `[63:48]` | `snac_axes0[31:16]` | `axes0_hi` |
| 4 | `[79:64]` | `snac_pad1` | `pad[1]` |
| 5 | `[95:80]` | `snac_axes1[15:0]` | `axes1_lo` |
| 6 | `[111:96]` | `snac_axes1[31:16]` | `axes1_hi` |

Seven words, 112 bits. All seven are read every poll even when a field has no
consumer: the core streams a fixed-length reply and a short read leaves the next
transaction misaligned.

Within a 32-bit axes field the low word arrives first, so
`axes[p] = (hi << 16) | lo`. Within `axes`, `rtl/snac_psx.v` packs
`{rx_byte[8], rx_byte[7], rx_byte[6], rx_byte[5]}` = `{LY, LX, RY, RX}`, one byte
each, MSB first — so `axes[31:24]` is LY and `axes[7:0]` is RX. The mouse code
depends on that and says so at the site.

## How each side is checked

- **Menu core** — `snac_state_check.sh`, run in CI. It extracts the
  `assign snac_state = {...}` operand list from `menu.sv` and compares it to the
  order above. A reordered pack fails the build here.
- **AmigaCD** — the same script lives there as `snac_state_check.sh` and reads
  the sequence of assignments in `snac_psx_poll()` instead. A reordered unpack
  fails the build there.

Neither check can see the other repository. That is the point: each side is
pinned to this table, so a change on either side that is not also a change to
this table is caught where it was made, at the commit that made it.

**If one of those steps fails, do not delete it as stale.** It means the packing
or the unpacking has moved. Either put it back, or change both sides and this
table in the same breath — and then the two copies of this file have to be made
to match again, which is deliberate friction for a change that breaks every
button on every pad.

## If it ever does drift

The symptom is not subtle once you know to look for it: buttons map to the wrong
buttons, or the sticks read as a device ID. The fastest confirmation is the id
byte — `id[0]` is the low byte of word 0 and reads `0x41` for a digital pad,
`0x73` analog, `0x63` GunCon, `0x00` for an empty port. If `id[0]` is a plausible
device ID and the buttons are still wrong, the ordering is intact and the fault
is in `decode()` or the mapping above it. If `id[0]` is `0x00` with a pad plugged
in, or something like `0x80`, suspect this table first.
