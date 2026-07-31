#!/usr/bin/env python3
"""Phase 4a: OSD trigger for the physical CD drive.

Everything from phases 1-2 is dormant because nothing passes
PHYSICAL_DISC_SENTINEL to cdrom_parse. This adds a "Real CD" row to the
AmigaCD console OSD that does exactly what the image-mount path does --
park a filename in hardfile[0], set cfg=2, call ide_open -- except the
filename is the sentinel.

Selecting the row when it is already mounted re-runs load_physical_disc,
which re-reads the TOC. That is the manual disc-swap path until phase 4's
media-change wiring lands.

Adding a row shifts every menusub below it in MENU_AMIGACD_MAIN1/MAIN2, plus
the two return-to-Settings-row assignments in MENU_AMIGACD_SETTINGS2. Each
shift is anchored on its distinguishing comment or surrounding line so the
edits cannot cascade into each other.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _patchlib import PatchSet  # noqa: E402

M = "menu.cpp"
ps = PatchSet(Path(__file__).resolve().parents[2])

ps.add(
    M,
    '#include "ide.h"\n',
    '#include "ide.h"\n'
    '#include "support/physical_disc/physical_disc.h"\n',
    "physical_disc.h include",
)

# ------------------------------------------------ MENU_AMIGACD_MAIN1 layout
ps.add(
    M,
    "\t\tmenumask = 0x7F;            // CD, Floppy, System, Settings, Save, Reset, Exit",
    "\t\tmenumask = 0xFF;            // CD, Real CD, Floppy, System, Settings, Save, Reset, Exit",
    "MAIN1 menumask",
)

ps.add(
    M,
    '\t\tstrcpy(s, " CD       : ");\n'
    "\t\tif (minimig_config.hardfile[0].filename[0])\n"
    "\t\t{\n"
    "\t\t\tchar *name = minimig_config.hardfile[0].filename;\n"
    "\t\t\tchar *p = strrchr(name, '/');\n"
    "\t\t\tstrncat(s, p ? p + 1 : name, 24);\n"
    "\t\t}\n"
    '\t\telse strcat(s, "* no disc *");\n'
    "\t\tOsdWrite(m++, s, menusub == 0, 0);\n"
    "\n",
    '\t\tstrcpy(s, " CD       : ");\n'
    "\t\tif (!strcmp(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL))\n"
    "\t\t{\n"
    '\t\t\tstrcat(s, "* physical drive *");\n'
    "\t\t}\n"
    "\t\telse if (minimig_config.hardfile[0].filename[0])\n"
    "\t\t{\n"
    "\t\t\tchar *name = minimig_config.hardfile[0].filename;\n"
    "\t\t\tchar *p = strrchr(name, '/');\n"
    "\t\t\tstrncat(s, p ? p + 1 : name, 24);\n"
    "\t\t}\n"
    '\t\telse strcat(s, "* no disc *");\n'
    "\t\tOsdWrite(m++, s, menusub == 0, 0);\n"
    "\n"
    "\t\t// Physical CD-ROM drive attached to the MiSTer. Mounting parks the\n"
    "\t\t// sentinel in hardfile[0].filename, which is what ide_open matches on.\n"
    "\t\t// Re-selecting while mounted re-reads the TOC, i.e. disc swap.\n"
    "\t\t{\n"
    "\t\t\tbool phys_cd = !strcmp(minimig_config.hardfile[0].filename,\n"
    "\t\t\t                       PHYSICAL_DISC_SENTINEL);\n"
    '\t\t\tstrcpy(s, " Real CD  : ");\n'
    '\t\t\tstrcat(s, phys_cd ? "re-read TOC" : "mount drive");\n'
    "\t\t\tOsdWrite(m++, s, menusub == 1, 0);\n"
    "\t\t}\n"
    "\n",
    "MAIN1 CD row + new Real CD row",
)

ps.add(
    M,
    '\t\telse strcat(s, "* no disk *");\n\t\tOsdWrite(m++, s, menusub == 1, 0);',
    '\t\telse strcat(s, "* no disk *");\n\t\tOsdWrite(m++, s, menusub == 2, 0);',
    "MAIN1 floppy row -> 2",
)

ps.add(
    M,
    '\t\tstrcat(s, cdtv ? "CDTV" : "CD32");\n\t\tOsdWrite(m++, s, menusub == 2, 0);',
    '\t\tstrcat(s, cdtv ? "CDTV" : "CD32");\n\t\tOsdWrite(m++, s, menusub == 3, 0);',
    "MAIN1 system row -> 3",
)

ps.add(
    M,
    '\t\tOsdWrite(m++, " Settings                 \\x16", menusub == 3, 0);\n'
    '\t\tOsdWrite(m++, " Save settings", menusub == 4, 0);',
    '\t\tOsdWrite(m++, " Settings                 \\x16", menusub == 4, 0);\n'
    '\t\tOsdWrite(m++, " Save settings", menusub == 5, 0);',
    "MAIN1 settings/save rows -> 4/5",
)

ps.add(
    M,
    '\t\tOsdWrite(OsdGetSize() - 2, " Reset", menusub == 5, 0);\n'
    "\t\tOsdWrite(OsdGetSize() - 1, STD_EXIT, menusub == 6, 0);",
    '\t\tOsdWrite(OsdGetSize() - 2, " Reset", menusub == 6, 0);\n'
    "\t\tOsdWrite(OsdGetSize() - 1, STD_EXIT, menusub == 7, 0);",
    "MAIN1 reset/exit rows -> 6/7",
)

# ---------------------------------------------- MENU_AMIGACD_MAIN2 handlers
ps.add(
    M,
    "\t\t\telse if (menusub == 1) // Floppy df0",
    "\t\t\telse if (menusub == 1) // Real CD: mount the physical drive\n"
    "\t\t\t{\n"
    "\t\t\t\tif (select)\n"
    "\t\t\t\t{\n"
    "\t\t\t\t\tstrcpy(minimig_config.hardfile[0].filename, PHYSICAL_DISC_SENTINEL);\n"
    "\t\t\t\t\tminimig_config.hardfile[0].cfg = 2;\n"
    "\t\t\t\t\tif (ide_open(0, minimig_config.hardfile[0].filename))\n"
    "\t\t\t\t\t{\n"
    "\t\t\t\t\t\t// No minimig_reset() here: the akiko bridge raises its own\n"
    "\t\t\t\t\t\t// mediachange once cd_is_mounted() flips, same as for an\n"
    "\t\t\t\t\t\t// image mount.\n"
    '\t\t\t\t\t\tInfo("Physical CD mounted", 1500);\n'
    "\t\t\t\t\t}\n"
    "\t\t\t\t\telse\n"
    "\t\t\t\t\t{\n"
    "\t\t\t\t\t\tminimig_config.hardfile[0].filename[0] = 0;\n"
    '\t\t\t\t\t\tInfo("No readable disc", 2000);\n'
    "\t\t\t\t\t}\n"
    "\t\t\t\t\tmenustate = MENU_AMIGACD_MAIN1;\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t\telse if (menusub == 2) // Floppy df0",
    "MAIN2 Real CD handler + floppy -> 2",
)

ps.add(
    M,
    "\t\t\telse if (menusub == 2) // System: toggle CD32 <-> CDTV",
    "\t\t\telse if (menusub == 3) // System: toggle CD32 <-> CDTV",
    "MAIN2 system -> 3",
)

ps.add(
    M,
    "\t\t\telse if (menusub == 3 && select) // Settings submenu",
    "\t\t\telse if (menusub == 4 && select) // Settings submenu",
    "MAIN2 settings -> 4",
)

ps.add(
    M,
    "\t\t\telse if (menusub == 4 && select) // Save settings -> default config slot 0",
    "\t\t\telse if (menusub == 5 && select) // Save settings -> default config slot 0",
    "MAIN2 save -> 5",
)

ps.add(
    M,
    "\t\t\telse if (menusub == 5 && select) // Reset",
    "\t\t\telse if (menusub == 6 && select) // Reset",
    "MAIN2 reset -> 6",
)

ps.add(
    M,
    "\t\t\telse if (menusub == 6 && select) // Exit",
    "\t\t\telse if (menusub == 7 && select) // Exit",
    "MAIN2 exit -> 7",
)

ps.add(
    M,
    "\t\telse if (c == KEY_BACKSPACE && menusub == 1) // eject floppy",
    "\t\telse if (c == KEY_BACKSPACE && menusub == 2) // eject floppy",
    "MAIN2 floppy eject key -> 2",
)

# --------------------------------------- MENU_AMIGACD_SETTINGS2 return rows
ps.add(
    M,
    "\t\telse if (back || left)\n"
    "\t\t{\n"
    "\t\t\tmenustate = MENU_AMIGACD_MAIN1;\n"
    "\t\t\tmenusub = 3;\n"
    "\t\t}",
    "\t\telse if (back || left)\n"
    "\t\t{\n"
    "\t\t\tmenustate = MENU_AMIGACD_MAIN1;\n"
    "\t\t\tmenusub = 4;\n"
    "\t\t}",
    "SETTINGS2 back returns to Settings row 4",
)

ps.add(
    M,
    "\t\t\telse if (menusub == 4 && select) // Back\n"
    "\t\t\t{\n"
    "\t\t\t\tmenustate = MENU_AMIGACD_MAIN1;\n"
    "\t\t\t\tmenusub = 3;\n"
    "\t\t\t}",
    "\t\t\telse if (menusub == 4 && select) // Back\n"
    "\t\t\t{\n"
    "\t\t\t\tmenustate = MENU_AMIGACD_MAIN1;\n"
    "\t\t\t\tmenusub = 4;\n"
    "\t\t\t}",
    "SETTINGS2 Back item returns to Settings row 4",
)

if __name__ == "__main__":
    sys.exit(ps.apply())
