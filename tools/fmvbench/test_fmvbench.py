#!/usr/bin/env python3
"""Host test for fmvbench (tools/fmvbench, docs/fmv-plan.md phase 1).

fmvbench's numbers are only worth having if it decodes what a real decoder
decodes. This checks that against ffmpeg on streams ffmpeg makes to the Video
CD specification (-target pal-vcd / ntsc-vcd, with B-pictures as on real
discs):

  1. demux: the video and audio elementary streams fmvbench extracts are
     byte-identical to ffmpeg's -c copy of the same streams;
  2. the same holds when the stream is wrapped as a Video CD .dat (RIFF/CDXA)
     and as a raw 2352-byte Mode 2 Form 2 track with empty sectors in it --
     the forms Cannon Fodder's MPEG track and a ripped VCD arrive in;
  3. decoded pictures match ffmpeg's decode (PSNR per plane), at an I, a P
     and a B picture -- in display order, so reordering is checked too;
  4. the RGB565 conversion matches a float BT.601 limited-range reference
     within one LSB per channel;
  5. a full run counts every picture and audio frame, finds the frame rate,
     and prints a gate verdict; the paced mode runs.

Needs ffmpeg on PATH. Run from anywhere:  python3 tools/fmvbench/test_fmvbench.py

The ARM binary the MiSTer runs can be checked the same way, under user-mode
qemu, which also exercises libmpeg2's ARM assembler (-a arm):
  FMVBENCH_BIN=tools/fmvbench/fmvbench FMVBENCH_RUNNER=qemu-arm-static \
  FMVBENCH_ACCELS="c arm" python3 tools/fmvbench/test_fmvbench.py
Timings under qemu mean nothing; the decode checks are what count.
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("FMVBENCH_BIN") or os.path.join(HERE, "fmvbench-host")
RUNNER = os.environ.get("FMVBENCH_RUNNER", "").split()
ACCELS = os.environ.get("FMVBENCH_ACCELS", "c").split()

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def run(args, **kw):
    return subprocess.run(args, check=True, capture_output=True, **kw)


def make_stream(path, target, size, rate, secs):
    # testsrc2 plus temporal noise: moving detail in every macroblock, so the
    # decoder does real work and the encoder fills the VCD bit budget.
    src = f"testsrc2=size={size}:rate={rate},noise=alls=24:allf=t"
    run(["ffmpeg", "-v", "error", "-y",
         "-f", "lavfi", "-i", src,
         "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
         "-t", str(secs), "-target", target, "-bf", "2", path])


def ffmpeg_es(path, tmp):
    v, a = os.path.join(tmp, "ref.m1v"), os.path.join(tmp, "ref.mp2")
    run(["ffmpeg", "-v", "error", "-y", "-i", path, "-map", "0:v", "-c", "copy",
         "-f", "mpeg1video", v, "-map", "0:a", "-c", "copy", "-f", "mp2", a])
    return open(v, "rb").read(), open(a, "rb").read()


def ffmpeg_i420(path):
    return run(["ffmpeg", "-v", "error", "-i", path, "-map", "0:v",
                "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"]).stdout


def ffmpeg_types(path):
    """Picture types in display order, from ffprobe."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v",
               "-show_entries", "frame=pict_type", "-of", "csv=p=0", path]).stdout
    return [t.strip(",") for t in out.decode().split() if t.strip(",")]


def psnr(a, b):
    se = sum((x - y) ** 2 for x, y in zip(a, b))
    if se == 0:
        return float("inf")
    return 10 * math.log10(255 * 255 * len(a) / se)


def sectors(ps, form2=True):
    """Wrap a program stream in raw 2352-byte Mode 2 sectors. Form 2: one
    2324-byte VCD pack per sector, with an empty (zero payload) sector every
    7th, as a VCD track has them. Form 1: 2048-byte pieces, no empty sectors --
    packs straddle sectors there, and an empty sector inside a packet would
    corrupt it, which no real disc does."""
    out = bytearray()
    sync = b"\x00" + b"\xff" * 10 + b"\x00"
    pay = 2324 if form2 else 2048
    lba = 0

    def one(data, submode):
        nonlocal lba
        m, s, f = (lba + 150) // 4500, ((lba + 150) // 75) % 60, (lba + 150) % 75
        bcd = lambda x: (x // 10) << 4 | x % 10
        sub = bytes([1, 1, submode, 0]) * 2
        body = data.ljust(pay, b"\x00")
        tail = b"\x00" * (2352 - 24 - pay)
        out.extend(sync + bytes([bcd(m), bcd(s), bcd(f), 2]) + sub + body + tail)
        lba += 1

    for i in range(0, len(ps), pay):
        if form2 and lba % 7 == 3:
            one(b"", 0x20 if form2 else 0)   # empty sector: zeroes, no start code
        # submode: 0x62 = video, Form 2, real-time (a VCD MPEG sector);
        # 0x08 = data, Form 1
        one(ps[i:i + pay], 0x62 if form2 else 0x08)
    return bytes(out)


def handmux(video, audio):
    """An MPEG-1 program stream built by hand, using every packet-header form
    ISO 11172-1 allows -- stuffing bytes, the STD buffer field, PTS, PTS+DTS
    and none -- plus system-header and padding packets. ffmpeg's own VCD
    muxer writes no STD buffer fields, so without this the demuxer's handling
    of them is never exercised. No field here contains 00 00 01."""
    pack = b"\x00\x00\x01\xba" + bytes([0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x1b, 0x91])
    sysh = b"\x00\x00\x01\xbb" + struct.pack(">H", 12) + bytes([0x80, 0x1b, 0x91, 0x04, 0xe1, 0xff,
                                                                  0xe0, 0xe0, 0x2e, 0xc0, 0xc0, 0x20])
    forms = [
        b"",                                                  # 0x0f only
        b"\xff\xff",                                          # stuffing
        b"\x60\x2e",                                          # STD buffer
        b"\xff\x40\x20",                                      # stuffing + STD
    ]
    tails = [b"\x0f", b"\x21\x00\x01\x00\x01", b"\x31\x00\x01\x00\x01\x11\x00\x01\x00\x01"]
    out = bytearray(pack + sysh)
    vi = ai = k = 0
    while vi < len(video) or ai < len(audio):
        for sid, es, pos, size in ((0xe0, video, vi, 1800), (0xc0, audio, ai, 500)):
            if pos >= len(es):
                continue
            chunk = es[pos:pos + size - 7 * (k % 3)]
            hdr = forms[k % len(forms)] + tails[k % len(tails)]
            # the STD field must precede the PTS, and 0x0f stands alone
            if hdr.endswith(b"\x0f") and len(hdr) > 1 and k % 2:
                hdr = hdr[:-1] + b"\x21\x00\x01\x00\x01"
            body = hdr + chunk
            out += b"\x00\x00\x01" + bytes([sid]) + struct.pack(">H", len(body)) + body
            if sid == 0xe0:
                vi += len(chunk)
            else:
                ai += len(chunk)
            k += 1
        if k % 5 == 0:
            out += b"\x00\x00\x01\xbe" + struct.pack(">H", 40) + b"\xff" * 40   # padding
            out += pack
    out += b"\x00\x00\x01\xb9"
    return bytes(out)


def cdxa(raw):
    fmt = b"fmt " + struct.pack("<I", 16) + b"\x00" * 16
    data = b"data" + struct.pack("<I", len(raw)) + raw
    body = b"CDXA" + fmt + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def demux(path, prefix):
    out = run(RUNNER + [BIN, "-x", prefix, path]).stdout.decode()
    return (open(prefix + ".m1v", "rb").read(), open(prefix + ".mp2", "rb").read(), out)


def kv(line, key):
    m = re.search(r"\b%s=([^\s]+)" % key, line)
    return m.group(1) if m else None


def main():
    if not shutil.which("ffmpeg"):
        print("ffmpeg not found")
        return 2
    if not os.path.exists(BIN):
        if "FMVBENCH_BIN" in os.environ:
            print("no binary at " + BIN)
            return 2
        run(["make", "-C", HERE, "host"])

    tmp = tempfile.mkdtemp(prefix="fmvbench-")
    try:
        pal = os.path.join(tmp, "pal.mpg")
        make_stream(pal, "pal-vcd", "352x288", 25, 4)
        ps = open(pal, "rb").read()
        rv, ra = ffmpeg_es(pal, tmp)

        # 1, 2: demux, three ways in
        v, a, out = demux(pal, os.path.join(tmp, "ps"))
        check(v == rv and len(v) > 0, f"program stream: video ES identical to ffmpeg's ({len(v)} bytes)")
        check(a == ra and len(a) > 0, f"program stream: audio ES identical to ffmpeg's ({len(a)} bytes)")

        hm = os.path.join(tmp, "hand.mpg")
        open(hm, "wb").write(handmux(rv, ra))
        v5, a5, _ = demux(hm, os.path.join(tmp, "hand"))
        check(v5 == rv and a5 == ra,
              "hand-muxed MPEG-1 PS (stuffing, STD buffer, PTS, PTS+DTS, padding, system header): same ES")

        vob = os.path.join(tmp, "mpeg2ps.vob")
        run(["ffmpeg", "-v", "error", "-y", "-i", pal, "-c", "copy", "-f", "vob", vob])
        v6, a6, out6 = demux(vob, os.path.join(tmp, "vob"))
        check("mpeg2_ps=1" in out6 and v6 == rv and a6 == ra, "MPEG-2 program stream (PES headers): same ES")

        raw_path = os.path.join(tmp, "track.bin")
        open(raw_path, "wb").write(sectors(ps))
        v2, a2, out2 = demux(raw_path, os.path.join(tmp, "raw"))
        check("kind=raw-2352" in out2, "raw track detected as raw-2352")
        check(v2 == rv and a2 == ra, "raw Mode 2 Form 2 track, empty sectors in it: same ES")

        dat_path = os.path.join(tmp, "AVSEQ01.DAT")
        open(dat_path, "wb").write(cdxa(sectors(ps)))
        v3, a3, out3 = demux(dat_path, os.path.join(tmp, "dat"))
        check("kind=vcd-dat" in out3, "RIFF/CDXA .dat detected")
        check(v3 == rv and a3 == ra, "VCD .dat: same ES")

        f1_path = os.path.join(tmp, "form1.bin")
        open(f1_path, "wb").write(sectors(ps, form2=False))
        v4, a4, _ = demux(f1_path, os.path.join(tmp, "f1"))
        check(v4 == rv and a4 == ra, "raw Mode 2 Form 1 track: same ES")

        # 3: pictures against ffmpeg, in display order
        w, h = 352, 288
        fsz = w * h * 3 // 2
        ref = ffmpeg_i420(pal)
        types = ffmpeg_types(pal)
        check("B" in types, f"test stream has B-pictures ({''.join(types[:16])}...)")
        picks = []
        for t in "IPB":
            idx = [i for i, x in enumerate(types) if x == t and i > 0]
            if idx:
                picks.append((t, idx[len(idx) // 2]))
        for (t, n), acc in [(pk, a) for a in ACCELS for pk in picks]:
            got = run(RUNNER + [BIN, "-a", acc, "-y", str(n), pal]).stdout
            want = ref[n * fsz:(n + 1) * fsz]
            ok = len(got) == fsz
            planes = [(0, w * h), (w * h, w * h // 4), (w * h * 5 // 4, w * h // 4)]
            vals = [psnr(got[o:o + l], want[o:o + l]) for o, l in planes] if ok else [0, 0, 0]
            check(ok and min(vals) >= 40,
                  f"[-a {acc}] picture {n} ({t}) matches ffmpeg: PSNR Y/U/V {', '.join('%.1f' % x for x in vals)} dB (>= 40)")

        # 4: RGB565 against a float BT.601 limited-range reference
        n = picks[0][1]
        yuv = run(RUNNER + [BIN, "-y", str(n), pal]).stdout
        rgb = run(RUNNER + [BIN, "-Y", str(n), pal]).stdout
        Y, U, V = yuv[:w * h], yuv[w * h:w * h * 5 // 4], yuv[w * h * 5 // 4:]
        worst = 0
        for yy in range(0, h):
            for xx in range(0, w):
                c = Y[yy * w + xx] - 16
                d = U[(yy // 2) * (w // 2) + xx // 2] - 128
                e = V[(yy // 2) * (w // 2) + xx // 2] - 128
                r = min(255, max(0, round(1.164383 * c + 1.596027 * e)))
                g = min(255, max(0, round(1.164383 * c - 0.391762 * d - 0.812968 * e)))
                b = min(255, max(0, round(1.164383 * c + 2.017232 * d)))
                px = rgb[2 * (yy * w + xx)] | rgb[2 * (yy * w + xx) + 1] << 8
                gr, gg, gb = px >> 11, (px >> 5) & 63, px & 31
                worst = max(worst, abs(gr - (r >> 3)), abs(gg - (g >> 2)), abs(gb - (b >> 3)))
        check(len(rgb) == 2 * w * h and worst <= 1,
              f"RGB565 within 1 LSB of BT.601 limited range (worst {worst})")

        # 5: full runs
        res = subprocess.run(RUNNER + [BIN, "-c", "-1", pal], capture_output=True)
        out = res.stdout.decode()
        vline = next((l for l in out.splitlines() if l.startswith("FMVBENCH VIDEO")), "")
        aline = next((l for l in out.splitlines() if l.startswith("FMVBENCH AUDIO")), "")
        gline = next((l for l in out.splitlines() if l.startswith("FMVBENCH GATE")), "")
        check(kv(vline, "pictures") == str(len(types)) and kv(vline, "displayed") == str(len(types)),
              f"PAL: every picture decoded and displayed ({kv(vline, 'displayed')} of {len(types)})")
        check(kv(vline, "fps") == "25.000", "PAL: 25 fps from the sequence header")
        check(kv(aline, "rate") == "44100" and int(kv(aline, "frames") or 0) > 0,
              f"PAL: audio decoded at 44100 Hz ({kv(aline, 'frames')} frames)")
        check(gline.startswith("FMVBENCH GATE: PASS") == (res.returncode == 0)
              and ("PASS" in gline or "FAIL" in gline),
              "gate line printed and the exit status agrees with it")

        ntsc = os.path.join(tmp, "ntsc.mpg")
        make_stream(ntsc, "ntsc-vcd", "352x240", "30000/1001", 2)
        res = subprocess.run(RUNNER + [BIN, "-c", "-1", "-q", ntsc], capture_output=True)
        out = res.stdout.decode()
        vline = next((l for l in out.splitlines() if l.startswith("FMVBENCH VIDEO")), "")
        check(kv(vline, "fps") == "29.970", f"NTSC: 29.97 fps ({kv(vline, 'fps')})")

        res = subprocess.run(RUNNER + [BIN, "-c", "-1", "-p", "-q", ntsc], capture_output=True)
        lline = next((l for l in res.stdout.decode().splitlines() if l.startswith("FMVBENCH LOAD")), "")
        check(kv(lline, "paced") == "1" and
              float(kv(lline, "wall_s") or 0) >= 0.9 * float(kv(lline, "content_s") or 1),
              f"paced run plays in real time (wall {kv(lline, 'wall_s')} s for "
              f"{kv(lline, 'content_s')} s of content)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("RUN: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
