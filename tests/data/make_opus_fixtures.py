#!/usr/bin/env python3
"""Makes the Opus fixtures of tests/MediaTest.cpp and adds them to manifest.txt (as `audiotol` lines: the reference is libopus' own decode of
the file, made by ffmpeg, compared with a tolerance). Covers SILK (narrow and wide band, mono and stereo, 10 and 60 ms frames), hybrid and
CELT frames. Needs ffmpeg with libopus. Usage: tests/data/make_opus_fixtures.py
"""
import os, subprocess

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)

f0 = "(120+10*sin(2*PI*3*t))*t"
voice = "+".join("%s*sin(2*PI*%d*%s)" % (a, k, f0) for a, k in [(1, 1), (0.5, 2), (0.33, 3), (0.25, 4), (0.2, 5), (0.12, 8), (0.08, 14)])
expr = "(%s)*0.2*(0.6+0.4*sin(2*PI*4*t))" % voice

def run(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, **kw)

# 0.4 s of a voice-like tone (harmonics with vibrato and an envelope), plus a noise burst so that CELT has something to code
run("ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "aevalsrc='%s':s=48000:d=0.4" % expr, "-f", "lavfi", "-i", "anoisesrc=d=0.4:c=pink:a=0.03:r=48000",
    "-filter_complex", "amix=inputs=2:normalize=0,pan=stereo|c0=c0|c1=0.6*c0", "-ar", "48000", "src_opus.wav")

fixtures = [  # name, channels, application, bitrate, frame ms, cutoff Hz
    ("opus_silk_nb_mono.opus", 1, "voip", "12k", 10, 4000),
    ("opus_silk_wb_mono.opus", 1, "voip", "24k", 20, 8000),
    ("opus_silk_wb_stereo_60.opus", 2, "voip", "32k", 60, 8000),
    ("opus_hybrid_stereo.opus", 2, "audio", "32k", 20, 20000),
    ("opus_celt_mono.opus", 1, "lowdelay", "64k", 20, 20000),
    ("opus_celt_stereo_10.opus", 2, "lowdelay", "96k", 10, 20000),
]
lines = []
for name, ch, app, br, fd, cut in fixtures:
    run("ffmpeg", "-v", "error", "-y", "-i", "src_opus.wav", "-ac", str(ch), "-c:a", "libopus", "-application", app, "-b:a", br, "-frame_duration", str(fd),
        "-cutoff", str(cut), name)
    raw = run("ffmpeg", "-v", "error", "-c:a", "libopus", "-i", name, "-f", "s16le", "-acodec", "pcm_s16le", "-").stdout
    open(name + ".ref", "wb").write(raw)
    lines.append("audiotol %s 48000 %d %d %s.ref" % (name, ch, len(raw) // 2 // ch, name))
os.remove("src_opus.wav")

old = [l for l in open("manifest.txt").read().splitlines() if not l.startswith("audiotol opus_")]
open("manifest.txt", "w").write("\n".join(old + lines) + "\n")
print(len(lines), "opus fixtures")
