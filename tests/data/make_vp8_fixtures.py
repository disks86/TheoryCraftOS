#!/usr/bin/env python3
"""Regenerates the VP8 fixtures (IVF files) and manifest_vp8.txt that tests/VideoCodecTest.cpp checks Vp8Decoder against.

The streams are made by libvpx's vpxenc from ffmpeg-generated sources; the expected pictures are what libvpx's own decoder (vpxdec --i420)
writes for them, never anything from this OS's code. Every stream covers a feature of the format: key frames only, inter frames, odd sizes,
several token partitions, the loop filter's sharpness, the simple filter and bilinear prediction (profiles 1-3, full-pixel chroma in 3),
hidden alternate reference frames (golden / altref buffers), segmentation (cyclic refresh), error resilient mode (probabilities not kept),
several key frames and a change of the picture size in mid stream.
Needs: ffmpeg, vpxenc, vpxdec. Usage: tests/data/make_vp8_fixtures.py
Manifest lines:  vp8 <file> <width> <height> <frames> <fnv64 of all decoded 8-bit I420 pictures, cropped, in output order>
"""
import os, struct, subprocess
here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)
manifest = []

def fnv(data):
    h = 0xcbf29ce484222325
    for b in bytes(data):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def source(name, lavfi, frames):
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", lavfi, "-frames:v", str(frames), "-pix_fmt", "yuv420p", name)

def encode(name, src, *opts):
    run("vpxenc", "--codec=vp8", "--ivf", "-o", name, "--fps=15/1", *opts, src)

def odd_source(name, w, h, frames):
    """A raw I420 file of odd dimensions (ffmpeg's lavfi sources round them up): a moving gradient with a bright box."""
    cw, ch = (w + 1) // 2, (h + 1) // 2
    out = bytearray()
    for f in range(frames):
        for y in range(h):
            for x in range(w):
                v = (x * 5 + y * 3 + f * 7) & 255
                if 8 + f <= x < 20 + f and 4 <= y < 12: v = 235 - (x ^ y) * 3 % 40
                out.append(v)
        for p in range(2):
            for y in range(ch):
                for x in range(cw):
                    out.append((128 + (x * 9 - y * 7 + f * 3) * (1 if p == 0 else -1)) & 255 if (x + y) % 3 else 100 + p * 50)
    open(name, "wb").write(out)

def add(name, w, h, frames):
    raw = subprocess.run(["vpxdec", "--i420", "-o", "-", name], check=True, capture_output=True).stdout
    manifest.append(f"vp8 {name} {w} {h} {frames} {fnv(raw):016x}")

def packets(name):
    d = open(name, "rb").read()
    pos, out = 32, []
    while pos + 12 <= len(d):
        n = struct.unpack("<I", d[pos:pos + 4])[0]
        out.append(d[pos + 12:pos + 12 + n])
        pos += 12 + n
    return d[:32], out

def case(name, w, h, frames, lavfi, *opts):
    source("src.y4m", lavfi, frames)
    encode(name, "src.y4m", f"--limit={frames}", *opts)
    add(name, w, h, frames)

mandel = lambda w, h: f"mandelbrot=size={w}x{h}:rate=15"
test = lambda w, h: f"testsrc2=size={w}x{h}:rate=15"
q = lambda lo, hi: [f"--min-q={lo}", f"--max-q={hi}"]

case("vp8_intra.ivf", 64, 48, 4, test(64, 48), "--kf-max-dist=0", "--kf-min-dist=0", *q(10, 40))
case("vp8_inter.ivf", 80, 64, 12, mandel(80, 64), *q(20, 50))
odd_source("odd.i420", 33, 17, 10)
encode("vp8_odd.ivf", "odd.i420", "--i420", "--width=33", "--height=17", "--limit=10", *q(10, 40))
add("vp8_odd.ivf", 33, 17, 10)
os.remove("odd.i420")
case("vp8_parts.ivf", 48, 144, 6, mandel(48, 144), "--token-parts=3", *q(20, 50))
case("vp8_parts2.ivf", 48, 80, 6, mandel(48, 80), "--token-parts=1", *q(20, 50))
case("vp8_sharp.ivf", 64, 48, 8, mandel(64, 48), "--sharpness=6", *q(40, 60))
case("vp8_lowq.ivf", 48, 32, 6, mandel(48, 32), *q(0, 8))
case("vp8_highq.ivf", 64, 48, 8, mandel(64, 48), *q(55, 63))
for p in (1, 2, 3):
    case(f"vp8_profile{p}.ivf", 80, 64, 10, mandel(80, 64), f"--profile={p}", *q(30, 50))
case("vp8_profile3_rt.ivf", 96, 80, 10, mandel(96, 80), "--profile=3", "--rt", "--cpu-used=-6", "--end-usage=cbr", "--target-bitrate=100")
case("vp8_profile1_rt.ivf", 96, 80, 10, mandel(96, 80), "--profile=1", "--rt", "--cpu-used=-6", "--end-usage=cbr", "--target-bitrate=100")
case("vp8_arf.ivf", 64, 48, 24, mandel(64, 48), "--auto-alt-ref=1", "--lag-in-frames=10", "--passes=2", *q(20, 50))
case("vp8_segments.ivf", 96, 80, 10, mandel(96, 80), "--rt", "--cpu-used=-6", "--end-usage=cbr", "--target-bitrate=150")
case("vp8_errres.ivf", 64, 48, 10, mandel(64, 48), "--error-resilient=1", *q(20, 50))
case("vp8_keys.ivf", 64, 48, 12, mandel(64, 48), "--kf-max-dist=4", "--kf-min-dist=0", *q(20, 50))
case("vp8_noise.ivf", 64, 48, 8, mandel(64, 48), "--noise-sensitivity=3", "--static-thresh=1000", "--cpu-used=0", "--good", *q(30, 50))

# the picture size changes at a key frame
head, a = packets("vp8_odd.ivf")
_, b = packets("vp8_inter.ivf")
with open("vp8_resize.ivf", "wb") as f:
    f.write(head)
    for i, p in enumerate(a + b):
        f.write(struct.pack("<IQ", len(p), i) + p)
add("vp8_resize.ivf", 33, 17, len(a) + len(b))

os.remove("src.y4m")
open("manifest_vp8.txt", "w").write("\n".join(manifest) + "\n")
print("\n".join(manifest))
