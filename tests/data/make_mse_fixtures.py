#!/usr/bin/env python3
"""Regenerates the Media Source Extensions fixtures that tests/MseTest.cpp and tests/PageTest.cpp append to SourceBuffers.

mse_muxed.mp4   fragmented MP4, H.264 (Main, two B-frames) + AAC-LC, 96x64, 25 fps, 2 s, a fragment per key frame (1 s)
mse_video.mp4   the same video alone
mse_audio.mp4   the same audio alone
mse_audio.mp3   MPEG-1 Layer III, 44.1 kHz, 1 s
mse_audio.aac   AAC-LC in ADTS, 44.1 kHz, 1 s
mse_webm.webm   WebM, VP9 + Opus, 96x64, 25 fps, 2 s, a cluster per second (a key frame each)
mse_vp9.webm    the same video alone
mse_opus.webm   the same Opus sound alone; mse_vorbis.webm the same with Vorbis
mse_live.webm   the muxed file written the way live streams are: the segment and the clusters have no size
mse_vp9_0.bgra, mse_vp9_30.bgra   what libvpx decodes of pictures 0 and 30 of the video, 96x64 BGRA (the player's pictures are compared with them)
Needs: ffmpeg with libx264, aac, libmp3lame, libvpx-vp9, libopus and libvorbis. Usage: tests/data/make_mse_fixtures.py
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

vp9 = ["-c:v", "libvpx-vp9", "-b:v", "300k", "-g", "25", "-keyint_min", "25", "-lag-in-frames", "0", "-deadline", "good", "-cpu-used", "5", "-pix_fmt", "yuv420p"]
opus = ["-c:a", "libopus", "-b:a", "48k"]
cluster = ["-cluster_time_limit", "1000", "-cluster_size_limit", "1000000"]
run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, *audio, "-t", "2", *vp9, *opus, *cluster, "mse_webm.webm")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, "-t", "2", *vp9, *cluster, "mse_vp9.webm")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *audio, "-t", "2", *opus, *cluster, "mse_opus.webm")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *audio, "-t", "2", "-c:a", "libvorbis", "-b:a", "64k", *cluster, "mse_vorbis.webm")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, *audio, "-t", "2", *vp9, *opus, "-f", "webm", "-live", "1", "mse_live.webm")
for n in (0, 30):
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-i", "mse_vp9.webm", "-vf", "select=eq(n\\,%d)" % n, "-frames:v", "1", "-pix_fmt", "bgra", "-f", "rawvideo", "mse_vp9_%d.bgra" % n)
