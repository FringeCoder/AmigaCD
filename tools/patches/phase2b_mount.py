#!/usr/bin/env python3
"""Phase 2b: let ide_open/ide_img_set carry a file-less CD drive.

Phase 2a makes the bridges accept drive->phys, but the drive never gets that
far. Two independent blockers in ide.cpp:

  1. The entire CD mount path in ide_open sits behind
         if (filename[0] && FileOpenEx(&hdd_file[unit], filename, ...))
     The sentinel is not a path and will never open, so cdrom_parse is never
     even reached. The physical branch has to run *before* that gate.

  2. ide_img_set derives presence purely from the fileTYPE pointer. A physical
     drive has media but no file, so it would land with present=0 / cd=0 and
     cd_find_drive would still refuse it despite drive->phys.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _patchlib import PatchSet  # noqa: E402

ps = PatchSet(Path(__file__).resolve().parents[2])

ps.add(
    "ide.cpp",
    '#include "support/minimig/cdtv_cd.h"\n',
    '#include "support/minimig/cdtv_cd.h"\n'
    '#include "support/physical_disc/physical_disc.h"\n',
    "physical_disc.h include",
)

ps.add(
    "ide.cpp",
    "\tdrive->present = f ? 1 : 0;",
    "\t// A physical CD-ROM drive has media but no backing file, so presence\n"
    "\t// cannot come from the fileTYPE pointer alone.\n"
    "\tdrive->present = (f || drive->phys) ? 1 : 0;",
    "ide_img_set: presence from phys as well as f",
)

ps.add(
    "ide.cpp",
    "\telse\n\t{\n\t\tdrive->total_sectors = 0;\n\t}",
    "\telse if (!drive->phys)\n"
    "\t{\n"
    "\t\t// Physical drives keep the total_sectors that load_physical_disc\n"
    "\t\t// derived from the drive's own TOC; there is no file size to fall\n"
    "\t\t// back on.\n"
    "\t\tdrive->total_sectors = 0;\n"
    "\t}",
    "ide_img_set: keep physical geometry",
)

# Ahead of the FileOpenEx gate. On failure we deliberately do NOT return:
# FileOpenEx will reject the sentinel, the mount block is skipped, and control
# reaches the normal "not present" cleanup, which closes any stale CHD and
# clears the akiko/cdtv save paths.
ps.add(
    "ide.cpp",
    '\t\tprintf("\\nChecking HDD %d\\n", unit);\n'
    "\t\tif (filename[0] && FileOpenEx(&hdd_file[unit], filename,",
    '\t\tprintf("\\nChecking HDD %d\\n", unit);\n'
    "\n"
    "\t\t// Physical CD-ROM drive. Must be handled before the FileOpenEx gate\n"
    "\t\t// below: the sentinel is not a path, so that gate would reject it and\n"
    "\t\t// cdrom_parse would never run. There is also nothing for\n"
    "\t\t// ide_img_mount to open, hence the file-less ide_img_set. Geometry is\n"
    "\t\t// zeroed because ide_img_set only applies cylinders/heads/spt to\n"
    "\t\t// non-CD drives.\n"
    "\t\tif (filename[0] && !strcmp(filename, PHYSICAL_DISC_SENTINEL))\n"
    "\t\t{\n"
    "\t\t\tif (cdrom_parse(unit, filename))\n"
    "\t\t\t{\n"
    "\t\t\t\tide_img_set(unit, 0, 1, 0, 0, 0);\n"
    '\t\t\t\tprintf("HDD %d: physical CD-ROM drive\\n", unit);\n'
    "\t\t\t\treturn 1;\n"
    "\t\t\t}\n"
    '\t\t\tprintf("HDD %d: no readable physical disc\\n", unit);\n'
    "\t\t\t// fall through to the not-present cleanup below\n"
    "\t\t}\n"
    "\n"
    "\t\tif (filename[0] && FileOpenEx(&hdd_file[unit], filename,",
    "ide_open: file-less mount path ahead of the FileOpenEx gate",
)

if __name__ == "__main__":
    sys.exit(ps.apply())
