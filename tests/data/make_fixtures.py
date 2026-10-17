#!/usr/bin/env python3
"""Regenerates the media fixtures and manifest.txt that tests/MediaTest.cpp checks the decoders against.

The expected results come from independent decoders - Pillow, ffmpeg - not from this OS's code (IMA ADPCM, which
ffmpeg decodes with a different rounding than the standard algorithm, uses the reference algorithm written below).
Needs: python3 with numpy and Pillow, ffmpeg, ImageMagick (convert). Usage: tests/data/make_fixtures.py
Manifest lines:  image <file> <width> <height> <frames> <fnv64 of RGBA frames, colour zeroed where alpha is 0>
                 audio <file> <rate> <channels> <frames> <fnv64 of the s16le samples>
"""
import os, struct, subprocess, sys, tempfile
import numpy as np
from PIL import Image

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

def picture(w=40, h=30, shift=0):
    y, x = np.mgrid[0:h, 0:w]
    r = (x * 255 // (w - 1) + shift * 40) % 256
    g = (y * 255 // (h - 1))
    b = ((x + y) * 4 + shift * 70) % 256
    img = np.stack([r, g, b], -1).astype(np.uint8)
    d = (x - w // 2 - shift * 3) ** 2 + (y - h // 2) ** 2
    img[d < 100] = (240, 230, 20)
    img[(x // 5 + y // 5) % 7 == 0] //= 2
    return img

def with_alpha(rgb):
    h, w, _ = rgb.shape
    y, x = np.mgrid[0:h, 0:w]
    a = np.clip(255 - ((x - w // 2) ** 2 + (y - h // 2) ** 2) // 2, 0, 255).astype(np.uint8)
    a[:4, :] = 0
    return np.dstack([rgb, a])

def add_image(path, frames_rgba):
    """frames_rgba: list of HxWx4 uint8 arrays that a correct decoder must produce."""
    out = bytearray()
    for f in frames_rgba:
        f = f.copy()
        f[f[..., 3] == 0, 0:3] = 0
        out += f.tobytes()
    h, w, _ = frames_rgba[0].shape
    manifest.append(f"image {path} {w} {h} {len(frames_rgba)} {fnv(out):016x}")

def pil_oracle(path):
    im = Image.open(path)
    frames = []
    n = getattr(im, "n_frames", 1)
    for i in range(n):
        im.seek(i)
        frames.append(np.asarray(im.convert("RGBA")))
    return frames

def ffmpeg_oracle(path, w, h):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgba", "-"], check=True, capture_output=True).stdout
    n = len(raw) // (w * h * 4)
    return [np.frombuffer(raw, np.uint8)[i * w * h * 4:(i + 1) * w * h * 4].reshape(h, w, 4) for i in range(n)]

base = picture()
rgba = with_alpha(base)
Image.fromarray(base).save("rgb.png")
Image.fromarray(rgba).save("rgba.png")
Image.fromarray(base).convert("L").save("gray.png")
Image.fromarray(base).convert("P", palette=Image.ADAPTIVE, colors=32).save("pal.png")
run("convert", "rgb.png", "-interlace", "PNG", "rgb_interlaced.png")
run("convert", "rgb.png", "-colorspace", "gray", "-depth", "16", "gray16.png")
run("convert", "rgba.png", "-depth", "16", "rgba16.png")
Image.fromarray(base).save("rgb.bmp")
Image.fromarray(base).convert("P", palette=Image.ADAPTIVE, colors=64).save("pal8.bmp")
run("convert", "rgb.png", "-colors", "50", "-compress", "RLE", "bmp3:rle8.bmp")
run("convert", "rgba.png", "-define", "bmp:format=bmp4", "bmp:rgba32.bmp")
Image.fromarray(base).save("rgb.jpg", quality=85, subsampling=2)
Image.fromarray(base).save("rgb444.jpg", quality=90, subsampling=0)
Image.fromarray(base).save("progressive.jpg", quality=80, progressive=True, subsampling=2)
Image.fromarray(base).convert("L").save("gray.jpg", quality=85)
Image.fromarray(base).save("rgb.ppm")
Image.fromarray(base).convert("L").save("gray.pgm")
Image.fromarray(base).convert("L").convert("1").save("bw.pbm")
run("convert", "rgba.png", "pam:rgba.pam")
Image.fromarray(base).save("rgb.tga", compression="tga_rle")
Image.fromarray(rgba).save("rgba.tga")
run("convert", "rgb.png", "-compress", "LZW", "-define", "tiff:predictor=2", "rgb_lzw.tif")
run("convert", "rgba.png", "-compress", "Zip", "rgba_zip.tif")
run("convert", "rgb.png", "-define", "tiff:tile-geometry=16x16", "-compress", "LZW", "tiled.tif")
run("convert", "rgb.png", "-colors", "32", "-type", "palette", "pal.tif")
run("convert", "rgb.png", "-colorspace", "gray", "-depth", "4", "gray4.tif")
run("convert", "rgb.png", "-define", "tiff:endian=msb", "-compress", "RLE", "rgb_be.tif")
run("convert", "rgb.png", "-interlace", "plane", "-define", "tiff:rows-per-strip=7", "planar.tif")
big = Image.fromarray(base).resize((160, 120), Image.BICUBIC)
big.save("big.webp", lossless=True, method=6, quality=100)               # predictor, cross-colour, meta prefix codes
Image.fromarray(rgba).save("rgba.webp", lossless=True, method=4, quality=50, exact=True)
Image.fromarray(base).convert("P", palette=Image.ADAPTIVE, colors=12).convert("RGBA").save("pal.webp", lossless=True, method=4, quality=0)  # colour indexing
Image.fromarray(base).resize((160, 120), Image.BICUBIC).save("lossy.webp", quality=70, method=6)   # VP8
Image.fromarray(rgba).resize((96, 72), Image.BICUBIC).save("lossy_alpha.webp", quality=60, alpha_quality=70, exact=True)  # VP8 + ALPH
Image.fromarray(base).resize((130, 99), Image.BICUBIC).save("lossy_odd.webp", quality=40, method=2)
wframes = [picture(w=48, h=36, shift=s) for s in range(5)]
wframes = [Image.fromarray(with_alpha(f) if i % 2 else f).convert("RGBA") for i, f in enumerate(wframes)]
wframes[0].save("anim.webp", save_all=True, append_images=wframes[1:], duration=[40, 90, 60, 120, 30], loop=3, lossless=True)
wframes[0].save("anim_lossy.webp", save_all=True, append_images=wframes[1:], duration=50, loop=0, quality=60)
run("ffmpeg", "-v", "error", "-y", "-i", "rgba.png", "rgba.qoi")
run("ffmpeg", "-v", "error", "-y", "-i", "rgb.png", "rgb.qoi")

frames = [picture(shift=s) for s in range(4)]
pil_frames = [Image.fromarray(f).convert("P", palette=Image.ADAPTIVE, colors=64) for f in frames]
pil_frames[0].save("static.gif")
pil_frames[0].save("anim.gif", save_all=True, append_images=pil_frames[1:], duration=[50, 80, 120, 200], loop=3, disposal=1)
run("convert", "rgb.png", "-interlace", "GIF", "interlaced.gif")
with tempfile.TemporaryDirectory() as t:
    for i, f in enumerate(frames):
        Image.fromarray(with_alpha(f)).save(f"{t}/f{i}.png")
    run("ffmpeg", "-v", "error", "-y", "-framerate", "10", "-i", f"{t}/f%d.png", "-plays", "0", "-f", "apng", "anim.apng")

for p in ["rgb.png", "rgba.png", "gray.png", "pal.png", "rgb_interlaced.png", "rgb.bmp", "pal8.bmp", "rgb.jpg", "rgb444.jpg", "progressive.jpg", "gray.jpg",
          "rgb.ppm", "gray.pgm", "bw.pbm", "rgb.tga", "rgba.tga", "big.webp", "rgba.webp", "pal.webp", "lossy.webp", "lossy_alpha.webp", "lossy_odd.webp", "rgb_lzw.tif", "rgba_zip.tif", "tiled.tif", "pal.tif", "gray4.tif", "rgb_be.tif", "planar.tif", "static.gif", "anim.gif", "interlaced.gif", "anim.webp", "anim_lossy.webp"]:
    add_image(p, pil_oracle(p))
# 16-bit grey: the decoders keep the high byte (ffmpeg rounds to nearest instead, so it is not the oracle here)
g16 = (np.asarray(Image.open("gray16.png")).astype(np.uint32) >> 8).astype(np.uint8)
add_image("gray16.png", [np.dstack([g16, g16, g16, np.full_like(g16, 255)])])
for p in ["rgba16.png", "rle8.bmp", "rgba32.bmp", "rgba.pam", "rgba.qoi", "rgb.qoi", "anim.apng"]:
    add_image(p, ffmpeg_oracle(p, 40, 30))

# ---- audio ----
t = np.arange(2205)
left = (np.sin(t * 0.07) * 14000 + np.sin(t * 0.31) * 6000).astype(np.int16)
right = (np.sin(t * 0.045) * 12000 + np.random.default_rng(3).integers(-3000, 3000, len(t))).astype(np.int16)
pcm = np.stack([left, right], -1)
open("src.raw", "wb").write(pcm.astype("<i2").tobytes())

def ff_audio(out, *args, mono=False):
    ch = "1" if mono else "2"
    run("ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", "11025", "-ac", "2", "-i", "src.raw", "-ac", ch, *args, out)

def ff_oracle(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "s16le", "-acodec", "pcm_s16le", "-"], check=True, capture_output=True).stdout
    return raw

def add_audio(path, raw, rate, channels):
    manifest.append(f"audio {path} {rate} {channels} {len(raw) // 2 // channels} {fnv(raw):016x}")

def ima_reference(path, channels):
    d = open(path, "rb").read()
    i = 12
    while d[i:i + 4] != b"data":
        sz = struct.unpack("<I", d[i + 4:i + 8])[0]
        if d[i:i + 4] == b"fmt ":
            ba = struct.unpack("<H", d[i + 8 + 12:i + 8 + 14])[0]
        i += 8 + sz + (sz & 1)
    data = d[i + 8:]
    S = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
    IX = [-1, -1, -1, -1, 2, 4, 6, 8]
    out = []
    for b in range(len(data) // ba):
        blk = data[b * ba:(b + 1) * ba]
        st = []
        chans = [[] for _ in range(channels)]
        for c in range(channels):
            p, idx = struct.unpack("<hB", blk[4 * c:4 * c + 3])
            st.append([p, idx])
            chans[c].append(p)
        pos = 4 * channels
        while pos < ba:
            for c in range(channels):
                for byte in blk[pos:pos + 4]:
                    for n in (byte & 15, byte >> 4):
                        p, idx = st[c]
                        step = S[idx]
                        df = step >> 3
                        if n & 4: df += step
                        if n & 2: df += step >> 1
                        if n & 1: df += step >> 2
                        p += -df if n & 8 else df
                        p = max(-32768, min(32767, p))
                        idx = max(0, min(88, idx + IX[n & 7]))
                        st[c] = [p, idx]
                        chans[c].append(p)
                pos += 4
        for k in range(len(chans[0])):
            for c in range(channels):
                out.append(chans[c][k])
    return np.array(out, "<i2").tobytes()

wavs = {"pcm8": "pcm_u8", "pcm16": "pcm_s16le", "pcm24": "pcm_s24le", "pcm32": "pcm_s32le", "f32": "pcm_f32le", "f64": "pcm_f64le", "alaw": "pcm_alaw", "mulaw": "pcm_mulaw", "adpcm_ms": "adpcm_ms"}
for name, codec in wavs.items():
    ff_audio(f"{name}.wav", "-acodec", codec)
    add_audio(f"{name}.wav", ff_oracle(f"{name}.wav"), 11025, 2)
ff_audio("pcm16_mono.wav", "-acodec", "pcm_s16le", mono=True)
add_audio("pcm16_mono.wav", ff_oracle("pcm16_mono.wav"), 11025, 1)
ff_audio("adpcm_ms_mono.wav", "-acodec", "adpcm_ms", mono=True)
add_audio("adpcm_ms_mono.wav", ff_oracle("adpcm_ms_mono.wav"), 11025, 1)
for ch in (1, 2):
    name = "ima_mono.wav" if ch == 1 else "ima.wav"
    ff_audio(name, "-acodec", "adpcm_ima_wav", mono=ch == 1)
    add_audio(name, ima_reference(name, ch), 11025, ch)
for name, args in {"s16.aiff": ["-acodec", "pcm_s16be"], "s24.aiff": ["-acodec", "pcm_s24be"], "ulaw.aifc": ["-acodec", "pcm_mulaw", "-f", "aiff"],
                   "sowt.aifc": ["-acodec", "pcm_s16le", "-f", "aiff"], "u8.aifc": ["-acodec", "pcm_u8", "-f", "aiff"], "s16.au": ["-acodec", "pcm_s16be"],
                   "mulaw.au": ["-acodec", "pcm_mulaw"], "f32.au": ["-acodec", "pcm_f32be"], "fast.flac": ["-compression_level", "0"],
                   "best.flac": ["-compression_level", "8"], "s32.flac": ["-sample_fmt", "s32"]}.items():
    ff_audio(name, *args)
    add_audio(name, ff_oracle(name), 11025, 2)
# Lossy codecs: each decoder rounds a little differently, so the reference is ffmpeg's decode, compared with a tolerance.
def add_tolerance_fixture(name, rate, ch, true_frames=None):
    raw = ff_oracle(name)
    if true_frames is not None:
        raw = raw[:true_frames * ch * 2]   # the stream's true length (ffmpeg adds the padding of its last block)
    open(name + ".ref", "wb").write(raw)
    manifest.append(f"audiotol {name} {rate} {ch} {len(raw) // 2 // ch} {name}.ref")

for name, args, mono in [("stereo.ogg", ["-acodec", "libvorbis", "-q:a", "4"], False), ("mono.ogg", ["-acodec", "libvorbis", "-q:a", "2"], True),
                         ("stereo.mp3", ["-acodec", "libmp3lame", "-b:a", "96k"], False), ("mono.mp3", ["-acodec", "libmp3lame", "-b:a", "32k", "-ar", "11025"], True)]:
    ff_audio(name, *args, mono=mono)
    add_tolerance_fixture(name, 11025, 1 if mono else 2, len(t))
ff_audio("lsf.mp2", "-acodec", "mp2", "-b:a", "64k", "-ar", "22050")   # MPEG-2 (low sampling frequency) Layer II
add_tolerance_fixture("lsf.mp2", 22050, 2)
# MPEG-1 Layer I has no encoder to hand: a stream is built directly (random allocations, scale factors and samples), which is a
# legitimate bitstream, and ffmpeg's decoder is the reference.
def layer1_stream(frames, bitrate_idx=12, sr_idx=1):
    import random
    rnd = random.Random(7)
    kbps = [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448][bitrate_idx]
    size = 12 * kbps * 1000 // [44100, 48000, 32000][sr_idx] * 4
    out = bytearray()
    for _ in range(frames):
        allocs = [[rnd.choice([0, 1, 2, 3, 5]) if sb < 26 else 0 for sb in range(32)] for _ in range(2)]
        fields = [(allocs[c][sb], 4) for sb in range(32) for c in range(2)]
        fields += [(rnd.randrange(0, 63), 6) for sb in range(32) for c in range(2) if allocs[c][sb]]
        for _s in range(12):
            for sb in range(32):
                for c in range(2):
                    if allocs[c][sb]:
                        n = allocs[c][sb] + 1
                        fields.append((rnd.randrange(0, (1 << n) - 1), n))
        acc = 0
        nbits = 0
        for v, n in fields:
            acc = (acc << n) | v
            nbits += n
        pad = (-nbits) % 8
        body = (acc << pad).to_bytes((nbits + pad) // 8, "big")
        out += bytes([0xFF, 0xFF, (bitrate_idx << 4) | (sr_idx << 2), 0]) + body + bytes(size - 4 - len(body))
    return bytes(out)
open("layer1.mp1", "wb").write(layer1_stream(20))
add_tolerance_fixture("layer1.mp1", 48000, 2)
ff_audio("opus.ogg", "-acodec", "libopus", "-b:a", "48k")   # a codec that is not here: must be refused, not misread
os.remove("src.raw")

open("manifest.txt", "w").write("\n".join(manifest) + "\n")
print(len(manifest), "fixtures")
