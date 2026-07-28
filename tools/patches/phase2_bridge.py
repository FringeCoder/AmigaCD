#!/usr/bin/env python3
"""Phase 2: teach the Akiko and CDTV bridges to accept a physical CD drive.

Phase 1 mounts the drive into drive_t, but both bridges decide "is there
media?" by testing for a file handle (chd_f || f). A physical drive has
neither, so without this the BIOS sits on the no-disc splash forever.

Four classes of change:
  1. media detection  - cd_is_mounted, cd_find_drive, cdtv_find_drive
  2. CDTV READ DATA   - a fourth chd_f guard, on the *data* path
  3. CDDA read        - both audio readers hard-require chd_f
  4. CDDA endianness  - READ CD returns little-endian, CHD is big-endian
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _patchlib import PatchSet  # noqa: E402

AKIKO = "support/minimig/akiko_cd32.cpp"
CDTV = "support/minimig/cdtv_cd.cpp"

ps = PatchSet(Path(__file__).resolve().parents[2])

# ------------------------------------------------------------------ includes
ps.add(
    AKIKO,
    '#include "../chd/mister_chd.h" // mister_chd_read_sector for CDDA pump\n',
    '#include "../chd/mister_chd.h" // mister_chd_read_sector for CDDA pump\n'
    '#include "../physical_disc/physical_disc.h" // physical CD-ROM drive media\n',
    "physical_disc.h include",
)

ps.add(
    CDTV,
    '#include "../chd/mister_chd.h"    // mister_chd_read_sector for CDDA pump\n',
    '#include "../chd/mister_chd.h"    // mister_chd_read_sector for CDDA pump\n'
    '#include "../physical_disc/physical_disc.h" // physical CD-ROM drive media\n',
    "physical_disc.h include",
)

# ---------------------------------------------------------- media detection
# The two akiko sites are textually identical up to the return statement, so
# the anchors carry one extra line to stay unique.
ps.add(
    AKIKO,
    "\t\t\tif (drv->cd && (drv->chd_f || drv->f)) {\n\t\t\t\treturn true;",
    "\t\t\tif (drv->cd && (drv->chd_f || drv->f || drv->phys)) {\n\t\t\t\treturn true;",
    "cd_is_mounted accepts a physical drive",
)

ps.add(
    AKIKO,
    "\t\t\tif (drv->cd && (drv->chd_f || drv->f)) {\n\t\t\t\treturn drv;",
    "\t\t\tif (drv->cd && (drv->chd_f || drv->f || drv->phys)) {\n\t\t\t\treturn drv;",
    "cd_find_drive accepts a physical drive",
)

ps.add(
    CDTV,
    "\t\t\tif (drv->cd && (drv->chd_f || drv->f)) {\n\t\t\t\treturn drv;",
    "\t\t\tif (drv->cd && (drv->chd_f || drv->f || drv->phys)) {\n\t\t\t\treturn drv;",
    "cdtv_find_drive accepts a physical drive",
)

# ------------------------------------------------------- CDTV READ DATA path
ps.add(
    CDTV,
    "\t\t\tdrive_t *drv = cdtv_find_drive();\n"
    "\t\t\tif (!drv || !drv->chd_f) {\n"
    '\t\t\t\tcdtv_dbg("READ DATA lba=%u nsec=%u — no drive/chd", lba, nsec);',
    "\t\t\tdrive_t *drv = cdtv_find_drive();\n"
    "\t\t\t// Data path, not audio. Easy to miss when grepping for the CDDA\n"
    "\t\t\t// chd_f checks, but CDTV data reads fail without it even once\n"
    "\t\t\t// cdtv_find_drive accepts a physical drive.\n"
    "\t\t\tif (!drv || (!drv->chd_f && !drv->phys)) {\n"
    '\t\t\t\tcdtv_dbg("READ DATA lba=%u nsec=%u — no drive/media", lba, nsec);',
    "READ DATA accepts a physical drive",
)

# ------------------------------------------------------------ CDDA readers
# Guard: keep the null checks, but allow either media kind.
for _f in (AKIKO, CDTV):
    ps.add(
        _f,
        "\tif (!drv || !drv->chd_f || !buf2352) return false;",
        "\tif (!drv || !buf2352) return false;\n"
        "\tif (!drv->chd_f && !drv->phys) return false;",
        "audio reader accepts a physical drive",
    )

# Read: branch before the CHD fetch, after the track/attr/sectorSize checks so
# a data track is still refused as audio.
ps.add(
    AKIKO,
    "\tuint32_t chd_lba = lba + track->chd_offset;\n"
    "\tif (mister_chd_read_sector(drv->chd_f, chd_lba, 0, 0,\n"
    "\t                           AKIKO_CDDA_BYTES, buf2352,",
    "\tif (drv->phys) {\n"
    "\t\tif (physical_disc_read_sector((int)lba, buf2352, NULL)) return false;\n"
    "\t\t// READ CD (flags 0x10) returns audio in disc order, i.e.\n"
    "\t\t// little-endian per 16-bit sample. CHD stores it big-endian, and\n"
    "\t\t// akiko_push_audio_sector byte-swaps unconditionally on the way to\n"
    "\t\t// the FIFO. Normalise to CHD order here so the push path stays\n"
    "\t\t// source-agnostic. Getting this wrong is immediately audible:\n"
    "\t\t// white noise, not silence.\n"
    "\t\tuint16_t *s = (uint16_t *)buf2352;\n"
    "\t\tfor (int i = 0; i < AKIKO_CDDA_BYTES / 2; i++) s[i] = bswap_16(s[i]);\n"
    "\t\treturn true;\n"
    "\t}\n"
    "\n"
    "\tuint32_t chd_lba = lba + track->chd_offset;\n"
    "\tif (mister_chd_read_sector(drv->chd_f, chd_lba, 0, 0,\n"
    "\t                           AKIKO_CDDA_BYTES, buf2352,",
    "CDDA read via physical drive",
)

ps.add(
    CDTV,
    "\tuint32_t chd_lba = lba + track->chd_offset;\n"
    "\tif (mister_chd_read_sector(drv->chd_f, chd_lba, 0, 0,\n"
    "\t                           CDTV_CDDA_BYTES, buf2352,",
    "\tif (drv->phys) {\n"
    "\t\tif (physical_disc_read_sector((int)lba, buf2352, NULL)) return false;\n"
    "\t\t// See the CD32 path: disc order is little-endian, CHD order is\n"
    "\t\t// big-endian, and cdtv_push_audio_sector swaps unconditionally.\n"
    "\t\tuint16_t *s = (uint16_t *)buf2352;\n"
    "\t\tfor (int i = 0; i < CDTV_CDDA_BYTES / 2; i++) s[i] = bswap_16(s[i]);\n"
    "\t\treturn true;\n"
    "\t}\n"
    "\n"
    "\tuint32_t chd_lba = lba + track->chd_offset;\n"
    "\tif (mister_chd_read_sector(drv->chd_f, chd_lba, 0, 0,\n"
    "\t                           CDTV_CDDA_BYTES, buf2352,",
    "CDDA read via physical drive",
)

if __name__ == "__main__":
    sys.exit(ps.apply())
