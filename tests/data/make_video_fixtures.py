#!/usr/bin/env python3
"""Regenerates the H.264 fixtures and manifest_video.txt that tests/MediaTest.cpp checks the decoder against.

The streams come from x264 (through ffmpeg) and, for long-term references and reference list modification, which x264 never
uses, from Cisco's OpenH264 encoder (set OPENH264_ENC to its h264enc binary to rebuild that one; the committed file is used
otherwise). The expected pictures come from ffmpeg's H.264 decoder, not from this OS's code.
Needs: ffmpeg with libx264. Usage: tests/data/make_video_fixtures.py
Manifest lines:  video <file> <width> <height> <frames> <fnv64 of all decoded I420 frames, cropped>
"""
import os, subprocess
import make_pcm_fixtures

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

def oracle(name, w, h):
    raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", name, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"],
                         check=True, capture_output=True).stdout
    frames = len(raw) // (w * h * 3 // 2)
    assert frames * (w * h * 3 // 2) == len(raw)
    manifest.append(f"video {name} {w} {h} {frames} {fnv(raw):016x}")

def x264(name, source, x264_opts, frames, w, h, extra=(), container="h264"):
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", source, "-frames:v", str(frames), "-pix_fmt", "yuv420p",
        "-c:v", "libx264", *x264_opts, *extra, "-f", container, name)
    oracle(name, w, h)

# Baseline (CAVLC): several reference frames, two slices, a cropped size
x264("h264_cavlc.h264", "testsrc2=size=90x50:rate=25", ["-profile:v", "baseline", "-x264-params", "cabac=0:ref=3:slices=2"], 10, 90, 50)
# Intra only, CABAC with the 8x8 transform
x264("h264_intra.h264", "mandelbrot=size=64x48:rate=25", ["-profile:v", "high", "-x264-params", "keyint=1:8x8dct=1"], 3, 64, 48)
# Main profile with B frames and weighted prediction, spatial direct; quantisation matrices, 8x8 transform
x264("h264_bframes.h264", "life=size=80x48:rate=25:mold=10:ratio=0.1", ["-profile:v", "high", "-x264-params",
     "bframes=3:ref=3:weightp=2:weightb=1:direct=spatial:8x8dct=1:b-pyramid=normal"], 14, 80, 48)
x264("h264_cqm.h264", "mandelbrot=size=96x64:rate=25", ["-profile:v", "high", "-x264-params", "cqm=jvt:bframes=2:direct=temporal"], 9, 96, 64)
x264("h264_noise.h264", "color=c=black:size=64x48:rate=25,geq=lum='random(1)*255':cb=128:cr=128,format=yuv420p",
     ["-profile:v", "high", "-x264-params", "qp=8:bframes=2"], 5, 64, 48)
# MP4 with B frames (length-prefixed NAL units from an avcC record, an edit list) and an AAC soundtrack
run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=96x64:rate=25", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.6:sample_rate=22050",
    "-frames:v", "15", "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "high", "-x264-params", "bframes=3:ref=2:keyint=10:scenecut=0",
    "-c:a", "aac", "-b:a", "32k", "-ac", "1", "-shortest", "-movflags", "+faststart", "h264_bframes.mp4")
oracle("h264_bframes.mp4", 96, 64)
# The same video in an AVI (Annex B frames, no reordering)
x264("h264.avi", "testsrc2=size=64x48:rate=25", ["-profile:v", "main", "-x264-params", "bframes=0:keyint=6"], 8, 64, 48, container="avi")
# I_PCM macroblocks (hand-built streams)
make_pcm_fixtures.build_all()
oracle("h264_pcm_cavlc.h264", 48, 48)
oracle("h264_pcm_cabac.h264", 48, 48)
# OpenH264: long-term references and reference list modification
enc = os.environ.get("OPENH264_ENC")
if enc:
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i", "mandelbrot=size=96x64:rate=25", "-frames:v", "24", "-pix_fmt", "yuv420p", "-f", "rawvideo", "ltr.yuv")
    open("ltr.cfg", "w").write("FrameWidth 96\nFrameHeight 64\nFrameRateOut 12\nReconFile /dev/null\nProfileIdc 66\nInitialQP 24\n"
                               "SpatialBitrate 600\nMaxSpatialBitrate 0\nSliceMode 0\nSliceSize 1500\nSliceNum 1\n")
    run(enc, "-org", "ltr.yuv", "-sw", "96", "-sh", "64", "-bf", "h264_ltr.h264", "-rc", "-1", "-numl", "1", "-lconfig", "0", "ltr.cfg",
        "-ltr", "1", "-ltrnum", "2", "-ltrper", "3")
    os.remove("ltr.yuv"); os.remove("ltr.cfg")
oracle("h264_ltr.h264", 96, 64)

open("manifest_video.txt", "w").write("\n".join(manifest) + "\n")
print(len(manifest), "video fixtures")
