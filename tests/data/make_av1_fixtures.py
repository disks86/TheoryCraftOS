#!/usr/bin/env python3
"""Regenerates the AV1 fixtures (IVF files) and manifest_av1.txt that tests/VideoCodecTest.cpp checks the decoder against.

The streams come from libaom (aomenc), SVT-AV1 and rav1e, fed with ffmpeg's synthetic sources, and cover the coding tools of the specification.
The expected pictures come from dav1d (the reference decoder), converted to 8-bit 4:2:0 the way Av1Decoder does it - deeper samples are rounded down,
chroma that is not 4:2:0 is box-averaged, monochrome pictures get neutral chroma; never from this OS's code.
Needs: ffmpeg, aomenc, SvtAv1EncApp, rav1e, dav1d. Usage: tests/data/make_av1_fixtures.py
Manifest lines:  av1 <file> <width> <height> <frames> <fnv64 of all decoded 8-bit I420 pictures, cropped, in output order>
"""
import os, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)
manifest = []

def fnv(data):
    h = 0xcbf29ce484222325
    for b in bytes(data):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def run(*args, **kw):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)

def y4m(source, pix_fmt, frames, extra=()):
    path = tempfile.NamedTemporaryFile(suffix=".y4m", delete=False).name
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", source, *extra, "-frames:v", str(frames), "-pix_fmt", pix_fmt, "-strict", "-1", path)
    return path

def aom(name, source, pix_fmt, frames, flags=(), extra=()):
    src = y4m(source, pix_fmt, frames, extra)
    run("aomenc", "--ivf", "--passes=1", "--cpu-used=4", "--end-usage=q", "--cq-level=30", f"--limit={frames}", *flags, "-o", name, src)
    os.unlink(src)

def svt(name, source, pix_fmt, frames, flags=()):
    src = y4m(source, pix_fmt, frames)
    run("SvtAv1EncApp", "-i", src, "-b", name, "--preset", "10", "--crf", "35", "-n", str(frames), *flags)
    os.unlink(src)

def rav1e(name, source, pix_fmt, frames, flags=()):
    src = y4m(source, pix_fmt, frames)
    run("rav1e", src, "-o", name, "--speed", "8", "--quantizer", "100", "--limit", str(frames), *flags)
    os.unlink(src)

def frame_sizes(name):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "frame=width,height", "-of", "csv=p=0", name], check=True, capture_output=True).stdout.decode()
    return [tuple(int(v) for v in line.split(",")[:2]) for line in out.split() if line]

def reference(name, w, h, ssx, ssy, mono, bd):
    """The pictures dav1d makes, as 8-bit I420 the way Av1Decoder::convert does it (each picture at its own size)."""
    out = tempfile.NamedTemporaryFile(suffix=".yuv", delete=False).name
    run("dav1d", "-q", "-i", name, "-o", out)
    raw = open(out, "rb").read()
    os.unlink(out)
    bps = 2 if bd > 8 else 1
    sizes = frame_sizes(name)
    def to8(v):
        if bd == 8: return v
        return min(255, (v + (1 << (bd - 9))) >> (bd - 8))
    res = bytearray()
    base = 0
    for (w, h) in sizes:
        cw, ch = (w + ssx) >> ssx, (h + ssy) >> ssy
        fsz = (w * h + (0 if mono else 2 * cw * ch)) * bps
        ow, oh = (w + 1) // 2, (h + 1) // 2
        def sample(off, stride, x, y):
            i = base + off + (y * stride + x) * bps
            return raw[i] if bps == 1 else raw[i] | raw[i + 1] << 8
        for y in range(h):
            for x in range(w): res.append(to8(sample(0, w, x, y)))
        if mono:
            res += bytes([128]) * (2 * ow * oh)
        else:
            fx, fy = (1 if ssx else 2), (1 if ssy else 2)
            for p in (1, 2):
                po = (w * h + (p - 1) * cw * ch) * bps
                for y in range(oh):
                    for x in range(ow):
                        s = 0
                        for j in range(fy):
                            for i in range(fx):
                                s += sample(po, cw, min(x * fx + i, cw - 1), min(y * fy + j, ch - 1))
                        k = fx * fy
                        res.append(to8((s + k // 2) // k))
        base += fsz
    assert base == len(raw), (name, base, len(raw))
    return bytes(res), len(sizes)

def add(name, w, h, ssx=1, ssy=1, mono=False, bd=8):
    raw, n = reference(name, w, h, ssx, ssy, mono, bd)
    manifest.append(f"av1 {name} {w} {h} {n} {fnv(raw):016x}")

src = lambda w, h, r=10: f"testsrc2=size={w}x{h}:rate={r}"
noisy = lambda w, h: f"testsrc2=size={w}x{h}:rate=10,noise=alls=12:allf=t"
# key frames only
aom("av1_intra.ivf", src(64, 48), "yuv420p", 3, ["--kf-max-dist=0"])
add("av1_intra.ivf", 64, 48)
# inter frames with hidden alternate references (show_existing_frame), compound prediction, global and warped motion, OBMC, an odd size
aom("av1_inter.ivf", f"{src(100, 60)},scale=95:63", "yuv420p", 14, ["--lag-in-frames=8", "--auto-alt-ref=1", "--cpu-used=1", "--enable-global-motion=1", "--enable-warped-motion=1", "--enable-obmc=1"])
add("av1_inter.ivf", 95, 63)
# tiles
aom("av1_tiles.ivf", src(520, 72), "yuv420p", 4, ["--tile-columns=2", "--tile-rows=1"])
add("av1_tiles.ivf", 520, 72)
# 128x128 superblocks, loop restoration, CDEF, all the intra tools
aom("av1_sb128.ivf", noisy(200, 136), "yuv420p", 4, ["--sb-size=128", "--enable-restoration=1", "--cpu-used=1", "--enable-filter-intra=1", "--enable-cfl-intra=1", "--enable-smooth-intra=1", "--enable-paeth-intra=1"])
add("av1_sb128.ivf", 200, 136)
# lossless
aom("av1_lossless.ivf", src(48, 32), "yuv420p", 4, ["--lossless=1"])
add("av1_lossless.ivf", 48, 32)
# quantiser matrices, delta q
aom("av1_qm.ivf", noisy(96, 64), "yuv420p", 4, ["--enable-qm=1", "--qm-min=2", "--qm-max=10", "--deltaq-mode=1", "--aq-mode=1"])
add("av1_qm.ivf", 96, 64)
# segmentation through aq-mode
aom("av1_segments.ivf", noisy(128, 80), "yuv420p", 8, ["--aq-mode=2", "--cpu-used=2"])
add("av1_segments.ivf", 128, 80)
# screen content: palette, intra block copy
aom("av1_screen.ivf", f"mandelbrot=size=128x96:rate=10,format=yuv420p", "yuv420p", 4, ["--tune-content=screen", "--enable-intrabc=1", "--enable-palette=1"])
add("av1_screen.ivf", 128, 96)
# super-resolution
aom("av1_superres.ivf", src(160, 90), "yuv420p", 4, ["--superres-mode=1", "--superres-denominator=12"])
add("av1_superres.ivf", 160, 90)
# reference scaling (spatial resize mode)
aom("av1_resize.ivf", src(160, 90), "yuv420p", 8, ["--resize-mode=1", "--resize-denominator=12"])
add("av1_resize.ivf", 160, 90)
# film grain
aom("av1_grain.ivf", src(96, 64), "yuv420p", 4, ["--film-grain-test=4"])
add("av1_grain.ivf", 96, 64)
# profiles and bit depths
aom("av1_10bit.ivf", src(90, 54), "yuv420p10le", 6, ["--bit-depth=10", "--input-bit-depth=10"])
add("av1_10bit.ivf", 90, 54, bd=10)
aom("av1_12bit.ivf", src(90, 54), "yuv420p12le", 4, ["--bit-depth=12", "--input-bit-depth=12", "--profile=2"])
add("av1_12bit.ivf", 90, 54, bd=12)
aom("av1_444.ivf", src(90, 54), "yuv444p", 6, ["--profile=1"])
add("av1_444.ivf", 90, 54, 0, 0)
aom("av1_422.ivf", src(90, 54), "yuv422p", 6, ["--profile=2"])
add("av1_422.ivf", 90, 54, 1, 0)
aom("av1_mono.ivf", src(90, 54), "yuv420p", 4, ["--monochrome"])
add("av1_mono.ivf", 90, 54, mono=True)
# other encoders
svt("av1_svt.ivf", src(176, 96), "yuv420p", 12)
add("av1_svt.ivf", 176, 96)
rav1e("av1_rav1e.ivf", src(128, 72), "yuv420p", 8)
add("av1_rav1e.ivf", 128, 72)
open("manifest_av1.txt", "w").write("\n".join(manifest) + "\n")
