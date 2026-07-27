#!/usr/bin/env python3
"""Phase 1 of the physical-disc adapter: wire support/physical_disc into the
ide_cdrom drive_t layer that the Akiko/CDTV bridges already consume.

Every edit is a literal, asserted replacement -- if the upstream text has
moved, the script fails loudly instead of silently patching the wrong place.
Idempotent: re-running is a no-op.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
edits = []


def patch(relpath, old, new, note):
    edits.append((relpath, old, new, note))


# ---------------------------------------------------------------- cd.h
patch(
    "cd.h",
    "\tint sectorSize;\n\tchd_file *chd_f;",
    "\tint sectorSize;\n"
    "\t// Media is a physical CD-ROM drive (support/physical_disc), not a file.\n"
    "\t// When set, chd_f and every tracks[].f are NULL/closed and all reads go\n"
    "\t// through physical_disc_read_*() instead of seeking a file.\n"
    "\tint phys;\n"
    "\tchd_file *chd_f;",
    "toc_t.phys flag",
)

# ---------------------------------------------------------------- ide.h
patch(
    "ide.h",
    "\tuint8_t  cd;\n\tuint8_t  load_state;",
    "\tuint8_t  cd;\n"
    "\t// Media is a physical CD-ROM drive rather than a CHD/CUE/ISO. Mirrors\n"
    "\t// toc_t.phys in cd.h. chd_f and track[].f are all NULL/closed; the\n"
    "\t// track table is still fully populated from the drive's real TOC.\n"
    "\tuint8_t  phys;\n"
    "\tuint8_t  load_state;",
    "drive_t.phys flag",
)

# ------------------------------------------------------ ide_cdrom.cpp: include
patch(
    "ide_cdrom.cpp",
    '#include "support/minimig/cdtv_cd.h"\n',
    '#include "support/minimig/cdtv_cd.h"\n'
    '#include "support/physical_disc/physical_disc.h"\n',
    "physical_disc.h include",
)

# ------------------------------------------- ide_cdrom.cpp: load_physical_disc
patch(
    "ide_cdrom.cpp",
    "\treturn chdfile;\n}\n\n\nstatic const char* load_cue_file(drive_t *drv, const char *cuefile)",
    "\treturn chdfile;\n}\n"
    "\n"
    "\n"
    "// Mount a physical CD-ROM drive as this drive's media.\n"
    "//\n"
    "// physical_disc_load_toc fills a cd.h toc_t, so this is the same\n"
    "// toc_t -> drive_t/track_t translation load_chd_file does above, minus\n"
    "// everything CHD-specific. Physical tracks are always raw 2352 with\n"
    "// chd_offset 0, which is why cdrom_read_raw_sector's phys branch can read\n"
    "// straight through with no per-track arithmetic.\n"
    "//\n"
    "// Safe to call on an already-mounted physical drive: physical_disc_open\n"
    "// returns early when the same device is open, so this re-reads the TOC\n"
    "// without re-spinning the drive.\n"
    "//\n"
    "// Returns the sentinel on success (callers only test for non-NULL and pass\n"
    "// the value on as a display name), 0 on failure.\n"
    "static const char* load_physical_disc(drive_t *drv)\n"
    "{\n"
    "\ttoc_t tmpTOC = { };\n"
    "\n"
    "\tif (physical_disc_open(NULL)) return 0;\n"
    "\tif (physical_disc_load_toc(&tmpTOC))\n"
    "\t{\n"
    "\t\tphysical_disc_close();\n"
    '\t\tprintf("\\x1b[32mIDE: no readable physical disc\\n\\x1b[0m");\n'
    "\t\treturn 0;\n"
    "\t}\n"
    "\n"
    "\tif (tmpTOC.last < 1)\n"
    "\t{\n"
    "\t\tphysical_disc_close();\n"
    "\t\treturn 0;\n"
    "\t}\n"
    "\n"
    "\tmemset(drv->track, 0, sizeof(drv->track));\n"
    "\tdrv->track_cnt = 0;\n"
    "\tdrv->data_num = 0;\n"
    "\tuint32_t total_sector_size = 0;\n"
    "\n"
    "\tfor (int i = 0; i < tmpTOC.last; i++)\n"
    "\t{\n"
    "\t\tcd_track_t *src = &tmpTOC.tracks[i];\n"
    "\t\ttrack_t *trk = &drv->track[i];\n"
    "\t\ttrk->number = i + 1;\n"
    "\t\ttrk->sectorSize = src->sector_size;              // always 2352\n"
    "\t\t// TT_CDDA == 0 -> audio, anything else -> data. The Akiko and CDTV\n"
    "\t\t// bridges classify tracks purely by (attr & 0x40), same as the CHD path.\n"
    "\t\tif (src->type) trk->attr = 0x40;\n"
    "\t\ttrk->mode2 = 0;                                  // moot: reads are raw 2352\n"
    "\t\ttrk->chd_offset = 0;\n"
    "\t\ttrk->start = src->start;\n"
    "\t\ttrk->length = src->end - src->start;\n"
    "\t\tdrv->track_cnt++;\n"
    "\t\ttotal_sector_size += trk->length * trk->sectorSize;\n"
    "\t}\n"
    "\n"
    "\t// Lead-out sentinel. akiko_build_toc, the OOB guard in\n"
    "\t// akiko_handle_sec_req and cdtv cmd_play all treat track[track_cnt - 1]\n"
    "\t// as lead-out and take the disc end from its start.\n"
    "\ttrack_t *lead_out = &drv->track[drv->track_cnt];\n"
    "\tlead_out->number = drv->track_cnt + 1;\n"
    "\tlead_out->attr = 0;\n"
    "\tlead_out->start = tmpTOC.tracks[tmpTOC.last - 1].end;\n"
    "\tlead_out->length = 0;\n"
    "\tdrv->track_cnt++;\n"
    "\n"
    "\tdrv->total_sectors = total_sector_size / 512;\n"
    "\tdrv->chd_total_size = total_sector_size;\n"
    "\n"
    "\t// First data track, matching load_chd_file / load_cue_file.\n"
    "\tfor (uint8_t i = 0; i < drv->track_cnt; i++)\n"
    "\t{\n"
    "\t\tif (drv->track[i].attr == 0x40)\n"
    "\t\t{\n"
    "\t\t\tdrv->data_num = i;\n"
    "\t\t\tbreak;\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
    "\tdrv->phys = 1;\n"
    "\tdrv->chd_f = NULL;\n"
    "\n"
    '\tprintf("\\x1b[32mIDE: physical disc mounted, %u track(s), lead-out lba=%u\\n\\x1b[0m",\n'
    "\t\tdrv->track_cnt - 1, lead_out->start);\n"
    "\n"
    "\treturn PHYSICAL_DISC_SENTINEL;\n"
    "}\n"
    "\n"
    "\n"
    "static const char* load_cue_file(drive_t *drv, const char *cuefile)",
    "load_physical_disc()",
)

# --------------------------------------- ide_cdrom.cpp: cdrom_read_raw_sector
patch(
    "ide_cdrom.cpp",
    "\tif (!drive || !buf) return -1;\n"
    "\n"
    "\tbool is_index0 = false;",
    "\tif (!drive || !buf) return -1;\n"
    "\n"
    "\t// Physical drive: READ CD already hands back the exact 2352-byte raw\n"
    "\t// frame (flags 0xF8 for data, 0x10 for audio), so there is no per-track\n"
    "\t// sizing and no sync-header synthesis to do. On failure physical_disc\n"
    "\t// has already zero-filled buf; we still report -1 so the caller's\n"
    "\t// retry/hold logic runs.\n"
    "\tif (drive->phys)\n"
    "\t{\n"
    "\t\treturn physical_disc_read_sector((int)lba, buf, NULL) ? -1 : 0;\n"
    "\t}\n"
    "\n"
    "\tbool is_index0 = false;",
    "cdrom_read_raw_sector phys branch",
)

# ------------------------------------------- ide_cdrom.cpp: cdrom_close_chd
patch(
    "ide_cdrom.cpp",
    "\tif (drv->chd_f)\n"
    "\t{\n"
    "\t\tchd_close(drv->chd_f);\n"
    "\t\tdrv->chd_f = NULL;\n"
    "\t}",
    "\tif (drv->phys)\n"
    "\t{\n"
    "\t\tphysical_disc_close();\n"
    "\t\tdrv->phys = 0;\n"
    "\t}\n"
    "\n"
    "\tif (drv->chd_f)\n"
    "\t{\n"
    "\t\tchd_close(drv->chd_f);\n"
    "\t\tdrv->chd_f = NULL;\n"
    "\t}",
    "cdrom_close_chd releases the physical drive",
)

# ------------------------------------------------ ide_cdrom.cpp: cdrom_parse
patch(
    "ide_cdrom.cpp",
    "\tstatic char last_path[2][2][1024] = {};\n"
    "\tconst char *cmp_filename = filename ? filename : \"\";",
    "\tstatic char last_path[2][2][1024] = {};\n"
    "\n"
    "\t// Physical CD-ROM drive. Handled ahead of the normal path bookkeeping\n"
    "\t// because there is no file to getFullPath, and because a re-mount must\n"
    "\t// not tear the drive down and re-spin it. load_physical_disc re-reads\n"
    "\t// the TOC on every call, so a disc swap followed by a re-mount picks up\n"
    "\t// the new geometry for free.\n"
    "\tif (filename && !strcmp(filename, PHYSICAL_DISC_SENTINEL))\n"
    "\t{\n"
    "\t\tdrive_t *d = &ide_inst[num].drive[drv];\n"
    "\n"
    "\t\tif (!d->phys)\n"
    "\t\t{\n"
    "\t\t\t// Coming from file-backed media (or cold): drop it first.\n"
    "\t\t\tcdrom_close_chd(d);\n"
    "\t\t\tfor (uint8_t i = 0; i < sizeof(d->track) / sizeof(track_t); i++)\n"
    "\t\t\t{\n"
    "\t\t\t\tif (d->track[i].f.opened()) FileClose(&d->track[i].f);\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\n"
    "\t\td->mcr_flag = true;\n"
    "\t\td->playing = 0;\n"
    "\t\td->paused = 0;\n"
    "\t\td->play_start_lba = 0;\n"
    "\t\td->play_end_lba = 0;\n"
    "\n"
    "\t\tres = load_physical_disc(d);\n"
    "\t\tif (!res)\n"
    "\t\t{\n"
    "\t\t\tlast_path[num][drv][0] = '\\0';\n"
    "\t\t\takiko_cd32_set_cd_path(\"\");\n"
    "\t\t\tcdtv_cd_set_cd_path(\"\");\n"
    "\t\t\treturn 0;\n"
    "\t\t}\n"
    "\n"
    "\t\tstrncpy(last_path[num][drv], PHYSICAL_DISC_SENTINEL,\n"
    "\t\t        sizeof(last_path[0][0]) - 1);\n"
    "\t\tlast_path[num][drv][sizeof(last_path[0][0]) - 1] = '\\0';\n"
    "\n"
    "\t\t// TODO(phase 4): per-game NVRAM is keyed off the image path, so every\n"
    "\t\t// physical disc currently shares one .nvr. Replace the sentinel here\n"
    "\t\t// with physical_disc_disc_label()/_serial() once that lands.\n"
    "\t\takiko_cd32_set_cd_path(PHYSICAL_DISC_SENTINEL);\n"
    "\t\tcdtv_cd_set_cd_path(PHYSICAL_DISC_SENTINEL);\n"
    "\t\treturn res;\n"
    "\t}\n"
    "\n"
    "\tconst char *cmp_filename = filename ? filename : \"\";",
    "cdrom_parse sentinel dispatch",
)


def main():
    applied, skipped, failed = [], [], []

    for relpath, old, new, note in edits:
        path = ROOT / relpath
        raw = path.read_text(encoding="utf-8", newline="")
        # Tree checks out CRLF on Windows; anchors above are written LF.
        # Normalise for matching, restore the file's own convention on write.
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        if new in text:
            skipped.append(note)
            continue
        count = text.count(old)
        if count != 1:
            failed.append(f"{relpath}: {note}: expected 1 anchor match, found {count}")
            continue
        out = text.replace(old, new, 1)
        if crlf:
            out = out.replace("\n", "\r\n")
        path.write_text(out, encoding="utf-8", newline="")
        applied.append(f"{relpath}: {note}")

    for line in applied:
        print("  applied  ", line)
    for line in skipped:
        print("  already  ", line)
    for line in failed:
        print("  FAILED   ", line)

    if failed:
        print(f"\n{len(failed)} edit(s) failed -- nothing else was rolled back, "
              f"inspect with git diff", file=sys.stderr)
        return 1
    print(f"\n{len(applied)} applied, {len(skipped)} already present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
