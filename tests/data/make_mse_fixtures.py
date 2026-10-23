#!/usr/bin/env python3
"""Regenerates the Media Source Extensions fixtures that tests/MseTest.cpp and tests/PageTest.cpp append to SourceBuffers.

mse_muxed.mp4   fragmented MP4, H.264 (Main, two B-frames) + AAC-LC, 96x64, 25 fps, 2 s, a fragment per key frame (1 s)
mse_video.mp4   the same video alone
mse_audio.mp4   the same audio alone
mse_audio.mp3   MPEG-1 Layer III, 44.1 kHz, 1 s
mse_audio.aac   AAC-LC in ADTS, 44.1 kHz, 1 s
Needs: ffmpeg with libx264, aac and libmp3lame. Usage: tests/data/make_mse_fixtures.py
"""
import os, subprocess

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)

def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

video = ["-f", "lavfi", "-i", "testsrc2=size=96x64:rate=25"]
audio = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100"]
v264 = ["-c:v", "libx264", "-profile:v", "main", "-bf", "2", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0", "-pix_fmt", "yuv420p"]
aac = ["-c:a", "aac", "-b:a", "64k"]
frag = ["-movflags", "+frag_keyframe+empty_moov+default_base_moof"]

run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, *audio, "-t", "2", *v264, *aac, *frag, "mse_muxed.mp4")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, "-t", "2", *v264, *frag, "mse_video.mp4")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *audio, "-t", "2", *aac, *frag, "mse_audio.mp4")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *audio, "-t", "1", "-c:a", "libmp3lame", "-b:a", "64k", "-write_xing", "0", "-id3v2_version", "0", "mse_audio.mp3")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *audio, "-t", "1", *aac, "-f", "adts", "mse_audio.aac")
