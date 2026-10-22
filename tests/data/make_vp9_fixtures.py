#!/usr/bin/env python3
"""Regenerates the VP9 fixtures (IVF files) and manifest_vp9.txt that tests/VideoCodecTest.cpp checks the decoder against.

The streams come from libvpx (through ffmpeg's libvpx-vp9 and vpxenc). The expected pictures come from libvpx's own decoder (vpxdec) for
8-bit 4:2:0 streams, and from ffmpeg's decoder - converted to 8-bit 4:2:0 the way Vp9Decoder does it - for the other profiles; never from
this OS's code.
Needs: ffmpeg with libvpx-vp9, vpxenc and vpxdec. Usage: tests/data/make_vp9_fixtures.py
Manifest lines:  vp9 <file> <width> <height> <frames> <fnv64 of all decoded 8-bit I420 pictures, cropped, in output order>
"""
import os, subprocess, sys
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

def ffenc(name, source, enc, extra=()):
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", source, *extra, "-c:v", "libvpx-vp9", *enc, name)

def i420_from_native(raw, w, h, ssx, ssy, bd, nframes):
    """What Vp9Decoder::convert does: 8-bit I420; chroma that is not 4:2:0 is box-averaged, deeper samples are rounded down."""
    bps = 2 if bd > 8 else 1
    cw, ch = (w + ssx) >> ssx, (h + ssy) >> ssy
    fsz = (w * h + 2 * cw * ch) * bps
    ow, oh = (w + 1) // 2, (h + 1) // 2
    def to8(v):
        if bd == 8: return v
        return min(255, (v + (1 << (bd - 9))) >> (bd - 8))
    out = bytearray()
    for f in range(nframes):
        base = f * fsz
        def sample(off, stride, x, y):
            i = base + off + (y * stride + x) * bps
            return raw[i] if bps == 1 else raw[i] | raw[i + 1] << 8
        for y in range(h):
            for x in range(w): out.append(to8(sample(0, w, x, y)))
        fx, fy = (1 if ssx else 2), (1 if ssy else 2)
        for p in (1, 2):
            po = (w * h + (p - 1) * cw * ch) * bps
            for y in range(oh):
                for x in range(ow):
                    s = 0
                    for j in range(fy):
                        for i in range(fx):
                            s += sample(po, cw, min(x * fx + i, cw - 1), min(y * fy + j, ch - 1))
                    n = fx * fy
                    out.append(to8((s + n // 2) // n))
    return bytes(out)

def oracle_vpxdec(name, w, h):
    raw = subprocess.run(["vpxdec", "--i420", "-o", "-", name], check=True, capture_output=True).stdout
    return raw

def add(name, w, h, raw, frames=None):
    manifest.append(f"vp9 {name} {w} {h} {frames if frames is not None else len(raw) // (w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2))} {fnv(raw):016x}")

def native(name, pix_fmt, w, h, ssx, ssy, bd):
    raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", name, "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"], check=True, capture_output=True).stdout
    n = len(raw) // (((w * h + 2 * ((w + ssx) >> ssx) * ((h + ssy) >> ssy))) * (2 if bd > 8 else 1))
    add(name, w, h, i420_from_native(raw, w, h, ssx, ssy, bd, n), n)

src = lambda w, h, r=10: f"testsrc2=size={w}x{h}:rate={r}"
# key frames only
ffenc("vp9_intra.ivf", src(64, 48), ["-g", "1", "-b:v", "0", "-crf", "30", "-cpu-used", "4"], ["-frames:v", "3", "-pix_fmt", "yuv420p"])
add("vp9_intra.ivf", 64, 48, oracle_vpxdec("vp9_intra.ivf", 64, 48))
# inter frames with a hidden alternate reference (superframes) and compound prediction, an odd size
ffenc("vp9_inter.ivf", f"{src(100, 60)},scale=95:63", ["-b:v", "0", "-crf", "28", "-cpu-used", "1", "-auto-alt-ref", "1", "-lag-in-frames", "8"],
      ["-frames:v", "14", "-pix_fmt", "yuv420p"])
add("vp9_inter.ivf", 95, 63, oracle_vpxdec("vp9_inter.ivf", 95, 63))
# tile columns and rows
ffenc("vp9_tiles.ivf", src(520, 72), ["-b:v", "0", "-crf", "34", "-cpu-used", "5", "-tile-columns", "1", "-tile-rows", "1"], ["-frames:v", "4", "-pix_fmt", "yuv420p"])
add("vp9_tiles.ivf", 520, 72, oracle_vpxdec("vp9_tiles.ivf", 520, 72))
# lossless (Walsh-Hadamard)
ffenc("vp9_lossless.ivf", src(48, 32), ["-lossless", "1", "-cpu-used", "4"], ["-frames:v", "4", "-pix_fmt", "yuv420p"])
add("vp9_lossless.ivf", 48, 32, oracle_vpxdec("vp9_lossless.ivf", 48, 32))
# segmentation (cyclic refresh), error resilient mode
ffenc("vp9_segments.ivf", f"{src(128, 80)}", ["-b:v", "120k", "-aq-mode", "3", "-cpu-used", "4", "-deadline", "realtime", "-error-resilient", "1"],
      ["-frames:v", "12", "-pix_fmt", "yuv420p"])
add("vp9_segments.ivf", 128, 80, oracle_vpxdec("vp9_segments.ivf", 128, 80))
# profiles 1 to 3
for name, pf, ssx, ssy, bd in (("vp9_444.ivf", "yuv444p", 0, 0, 8), ("vp9_422.ivf", "yuv422p", 1, 0, 8), ("vp9_440.ivf", "yuv440p", 0, 1, 8),
                               ("vp9_10bit.ivf", "yuv420p10le", 1, 1, 10), ("vp9_12bit_444.ivf", "yuv444p12le", 0, 0, 12)):
    w, h = 90, 54
    ffenc(name, f"{src(w, h)},format=yuv444p", ["-pix_fmt", pf, "-b:v", "0", "-crf", "30", "-cpu-used", "3"], ["-frames:v", "6"])
    native(name, pf, w, h, ssx, ssy, bd)
# references of another size (dynamic resizing): the pictures change size in the stream (640x360, then 480x270 from picture 120 on)
run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", src(640, 360, 30), "-frames:v", "180", "-pix_fmt", "yuv420p", "scaled_src.y4m")
subprocess.run(["vpxenc", "scaled_src.y4m", "--codec=vp9", "--ivf", "-o", "vp9_scaled.ivf", "--end-usage=cbr", "--target-bitrate=60", "--resize-allowed=1",
                "--resize-up=60", "--resize-down=30", "--cpu-used=5", "--lag-in-frames=0", "--rt", "--min-q=20", "--max-q=63", "--buf-sz=1000",
                "--buf-initial-sz=500", "--buf-optimal-sz=600"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
os.remove("scaled_src.y4m")
raw = oracle_vpxdec("vp9_scaled.ivf", 640, 360)
add("vp9_scaled.ivf", 640, 360, raw, 180)
open("manifest_vp9.txt", "w").write("\n".join(manifest) + "\n")
print("\n".join(manifest))
