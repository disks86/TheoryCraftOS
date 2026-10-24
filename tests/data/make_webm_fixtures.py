#!/usr/bin/env python3
"""Regenerates the WebM fixtures of tests/WebmTest.cpp and the vectors it checks them against (tests/WebmVectors.inc).

ffmpeg writes the two base files; the other files are the same frames written again by the small EBML writer below, to exercise what a demuxer has
to cope with and ffmpeg's muxer never writes:

webm_vp9_opus.webm     VP9 + Opus (constant bit rate), 64x48, 10 fps, 1.6 s, a cluster per 500 ms (ffmpeg's own file)
webm_vp8_vorbis.webm   VP8 + Vorbis, the same picture, with libvpx's hidden alt-ref frames (ffmpeg's own file)
webm_live.webm         the first file with the segment and every cluster of unknown size (what a live stream looks like)
webm_groups.webm       every block in a BlockGroup with a BlockDuration; the sound laced four frames to a block (fixed-size lacing)
webm_xiph.webm         the second file with the Vorbis sound laced (Xiph lacing), four packets to a block
webm_ebml.webm         the first file with the Opus sound laced (EBML lacing, uneven sizes after a re-encode with VBR)
webm_zlib.webm         the first file with the VP9 frames deflated (zlib content compression)
webm_enc.webm          the first file encrypted the way WebM says (AES-CTR, key tests/data/make_eme_fixtures.py KID1/KEY1): the VP9 frames are partitioned (clear
                       and encrypted runs), the Opus packets are encrypted whole except a few left in the clear
webm_vp8_arf.webm      the VP8 frames of vp8_arf.ivf (libvpx with hidden alt-ref frames, each a block of its own), written by the writer below
webm_opus.weba         the sound of the first file alone
webm_vorbis.weba       the sound of the second file alone
webm_ref_opus.ogg, webm_ref_vorbis.ogg   the same sound as Ogg files (ffmpeg, stream copy): the decoded sound of the WebM files must equal what these decode to
Needs: ffmpeg with libvpx, libvpx-vp9, libopus and libvorbis; python 3. Usage: tests/data/make_webm_fixtures.py
"""
import os, subprocess, struct, zlib

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)

def run(*args, capture=False):
    r = subprocess.run(args, check=True, stdout=subprocess.PIPE if capture else subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return r.stdout

video = ["-f", "lavfi", "-i", "testsrc2=size=64x48:rate=10"]
audio = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
cluster = ["-cluster_time_limit", "500", "-cluster_size_limit", "1000000"]
vp9 = ["-c:v", "libvpx-vp9", "-b:v", "200k", "-g", "8", "-keyint_min", "8", "-lag-in-frames", "0", "-deadline", "good", "-cpu-used", "5", "-pix_fmt", "yuv420p"]
vp8 = ["-c:v", "libvpx", "-b:v", "200k", "-g", "8", "-auto-alt-ref", "1", "-lag-in-frames", "6", "-deadline", "good", "-cpu-used", "5", "-pix_fmt", "yuv420p"]
opus = ["-c:a", "libopus", "-b:a", "32k", "-vbr", "off", "-application", "audio"]
vorbis = ["-c:a", "libvorbis", "-b:a", "48k"]
run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, *audio, "-t", "1.6", *vp9, *opus, *cluster, "webm_vp9_opus.webm")
run("ffmpeg", "-nostdin", "-v", "error", "-y", *video, *audio, "-t", "1.6", *vp8, *vorbis, *cluster, "webm_vp8_vorbis.webm")
# a second Opus encoding with a variable bit rate: packets of different sizes, for the EBML lacing
run("ffmpeg", "-nostdin", "-v", "error", "-y", *audio, "-t", "1.6", "-c:a", "libopus", "-b:a", "32k", *cluster, "vbr_opus.tmp.webm")

# AES-CTR from the EME fixtures' pure Python implementation (checked against openssl there)
_eme = open(os.path.join(here, "make_eme_fixtures.py")).read()
exec(_eme[_eme.index("SBOX = [0] * 256"):_eme.index("def self_check")])
KID1 = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
KEY1 = bytes.fromhex("f0e1d2c3b4a5968778695a4b3c2d1e0f")

# ---- EBML ----------------------------------------------------------------------------------------------------------------------------------

def idbytes(i):
    n = (i.bit_length() + 7) // 8
    return i.to_bytes(n, 'big')

def sizebytes(n, unknown=False):
    if unknown: return b'\x01\xff\xff\xff\xff\xff\xff\xff'
    for length in range(1, 9):
        if n < (1 << (7 * length)) - 1:
            return (n | (1 << (7 * length))).to_bytes(length, 'big')
    raise ValueError

def el(i, payload, unknown=False):
    return idbytes(i) + sizebytes(len(payload), unknown) + payload

def uint(i, v):
    n = max(1, (v.bit_length() + 7) // 8)
    return el(i, v.to_bytes(n, 'big'))

def sint(i, v):
    n = 1
    while not (-(1 << (8 * n - 1)) <= v < (1 << (8 * n - 1))): n += 1
    return el(i, v.to_bytes(n, 'big', signed=True))

def read_vint(b, p, keep_marker):
    first = b[p]
    n = 1
    while n <= 8 and not (first & (0x80 >> (n - 1))): n += 1
    v = first if keep_marker else first & (0xff >> n)
    allones = (first & (0xff >> n)) == (0xff >> n)
    for k in range(1, n):
        v = (v << 8) | b[p + k]
        allones = allones and b[p + k] == 0xff
    return v, n, allones

def children(b, start, end):
    p = start
    while p < end:
        i, n1, _ = read_vint(b, p, True)
        s, n2, unknown = read_vint(b, p + n1, False)
        payload = p + n1 + n2
        stop = end if unknown else payload + s
        yield i, payload, stop
        p = stop

def read_uint(b, s, e): return int.from_bytes(b[s:e], 'big')

class Track:
    pass

class Block:
    pass

def parse(path):
    b = open(path, 'rb').read()
    tracks, blocks, info = [], [], {'scale': 1000000, 'duration': 0.0}
    for i, s, e in children(b, 0, len(b)):
        if i != 0x18538067: continue
        for ci, cs, ce in children(b, s, e):
            if ci == 0x1549A966:
                for ii, isx, ie in children(b, cs, ce):
                    if ii == 0x2AD7B1: info['scale'] = read_uint(b, isx, ie)
                    elif ii == 0x4489: info['duration'] = struct.unpack('>f' if ie - isx == 4 else '>d', b[isx:ie])[0]
            elif ci == 0x1654AE6B:
                for ti, ts, te in children(b, cs, ce):
                    t = Track()
                    t.entry = list(children(b, ts, te))
                    t.children = [(x, b[xs:xe]) for x, xs, xe in t.entry]
                    for x, p in t.children:
                        if x == 0xD7: t.number = int.from_bytes(p, 'big')
                        if x == 0x83: t.kind = int.from_bytes(p, 'big')
                    tracks.append(t)
            elif ci == 0x1F43B675:
                tc = 0
                for ki, ks, ke in children(b, cs, ce):
                    if ki == 0xE7: tc = read_uint(b, ks, ke)
                    elif ki == 0xA3 or ki == 0xA0:
                        blk = Block()
                        blk.duration = None; blk.discard = None; blk.key = True
                        if ki == 0xA3:
                            raw = b[ks:ke]
                            blk.key = bool(raw[2 + 1 + 0 - 0] & 0x80) if False else None
                        else:
                            raw = None
                            for gi, gs, ge in children(b, ks, ke):
                                if gi == 0xA1: raw = b[gs:ge]
                                elif gi == 0x9B: blk.duration = read_uint(b, gs, ge)
                                elif gi == 0x75A2: blk.discard = int.from_bytes(b[gs:ge], 'big', signed=True)
                                elif gi == 0xFB: blk.key = False
                        # track number (1 byte vint here), relative time, flags
                        number, n, _ = read_vint(raw, 0, False)
                        rel = struct.unpack('>h', raw[n:n + 2])[0]
                        flags = raw[n + 2]
                        if ki == 0xA3: blk.key = bool(flags & 0x80)
                        assert (flags & 6) == 0, 'the base files are not laced'
                        blk.track, blk.pts, blk.data = number, tc + rel, raw[n + 3:]
                        blocks.append(blk)
    return b, tracks, blocks, info

# ---- writing -------------------------------------------------------------------------------------------------------------------------------

def vint(v):
    for n in range(1, 9):
        if v < (1 << (7 * n)) - 1: return (v | (1 << (7 * n))).to_bytes(n, 'big')
    raise ValueError

def lace(frames, mode):
    out = bytes([len(frames) - 1])
    if mode == 'xiph':
        for f in frames[:-1]:
            n = len(f)
            out += b'\xff' * (n // 255) + bytes([n % 255])
    elif mode == 'ebml':
        out += vint(len(frames[0]))
        for k in range(1, len(frames) - 1):
            d = len(frames[k]) - len(frames[k - 1])
            for n in range(1, 9):
                bias = (1 << (7 * n - 1)) - 1
                if -bias <= d <= bias:
                    out += (d + bias + (1 << (7 * n))).to_bytes(n, 'big')
                    break
    elif mode == 'fixed':
        assert len(set(len(f) for f in frames)) == 1
    return out + b''.join(frames)

def mux(path, info, tracks, blocks, unknown=False, groups=False, laces={}, strip={}, deflate=set(), per_cluster=500, encrypt=False):
    """tracks: Track objects; blocks: (Block...) in file order; laces: {track number: (mode, frames per block)}; strip: {track number: prefix};
    deflate: track numbers whose frames are zlib compressed."""
    ebml_header = el(0x1A45DFA3, uint(0x4286, 1) + uint(0x42F7, 1) + uint(0x42F2, 4) + uint(0x42F3, 8) + el(0x4282, b'webm') + uint(0x4287, 4) + uint(0x4285, 2))
    scale = info['scale']
    info_el = el(0x1549A966, uint(0x2AD7B1, scale) + el(0x4489, struct.pack('>f', info['duration'])) + el(0x4D80, b'fixture') + el(0x5741, b'fixture'))
    entries = b''
    for t in tracks:
        kids = b''
        for x, p in t.children:
            if x == 0x6D80: continue
            kids += el(x, p)
        if encrypt:
            enc = uint(0x47E1, 5) + el(0x47E2, KID1) + el(0x47E7, uint(0x47E8, 1))
            kids += el(0x6D80, el(0x6240, uint(0x5031, 0) + uint(0x5032, 1) + uint(0x5033, 1) + el(0x5035, enc)))
        if t.number in strip or t.number in deflate:
            comp = uint(0x4254, 3) + el(0x4255, strip[t.number]) if t.number in strip else uint(0x4254, 0)
            kids += el(0x6D80, el(0x6240, uint(0x5031, 0) + uint(0x5032, 1) + uint(0x5033, 0) + el(0x5034, comp)))
        entries += el(0xAE, kids)
    tracks_el = el(0x1654AE6B, entries)
    # the stored form of one frame
    counter = [0]
    def stored(number, data):
        if encrypt:
            counter[0] += 1
            n = counter[0]
            if number != video_number(tracks) and n % 7 == 0:
                return b'\x00' + data   # in the clear
            iv = n.to_bytes(8, 'big')
            if number == video_number(tracks) and len(data) > 40:
                offsets = [5] if n % 2 else [5, 20, 25]
                bounds = [0] + offsets + [len(data)]
                parts = [data[bounds[k]:bounds[k + 1]] for k in range(len(bounds) - 1)]
                encrypted_bytes = b''.join(parts[1::2])
                stream = ctr_bytes(iv, len(encrypted_bytes))
                cipher = bytes(x ^ y for x, y in zip(encrypted_bytes, stream))
                out, taken = b'', 0
                for k, part in enumerate(parts):
                    if k % 2: out += cipher[taken:taken + len(part)]; taken += len(part)
                    else: out += part
                return b'\x03' + iv + bytes([len(offsets)]) + b''.join(o.to_bytes(4, 'big') for o in offsets) + out
            stream = ctr_bytes(iv, len(data))
            return b'\x01' + iv + bytes(x ^ y for x, y in zip(data, stream))
        if number in strip:
            assert data.startswith(strip[number])
            data = data[len(strip[number]):]
        if number in deflate: data = zlib.compress(data)
        return data
    # blocks, with lacing: consecutive blocks of a laced track are put together
    items = []   # (pts, key, track, frames, duration_ticks or None, discard)
    pending = {}
    for blk in blocks:
        mode = laces.get(blk.track)
        if not mode:
            items.append((blk.pts, blk.key, blk.track, [stored(blk.track, blk.data)], blk.duration, blk.discard))
            continue
        pending.setdefault(blk.track, []).append(blk)
        if len(pending[blk.track]) == mode[1]:
            group = pending.pop(blk.track)
            items.append((group[0].pts, True, blk.track, [stored(blk.track, g.data) for g in group], None, group[-1].discard))
    for number, group in pending.items():
        for blk in group: items.append((blk.pts, blk.key, number, [stored(number, blk.data)], blk.duration, blk.discard))
    items.sort(key=lambda x: x[0])
    clusters = b''
    start = None
    body = b''
    cluster_list = []
    for it in items:
        if start is None or it[0] - start >= per_cluster * 1000000 // scale:
            if start is not None: cluster_list.append((start, body))
            start, body = it[0], b''
        rel = it[0] - start
        number = it[2]
        mode = laces.get(number)
        if len(it[3]) > 1:
            flags = {'xiph': 2, 'fixed': 4, 'ebml': 6}[mode[0]]
            payload = vint(number) + struct.pack('>h', rel) + bytes([flags]) + lace(it[3], mode[0])
        else:
            payload = vint(number) + struct.pack('>h', rel) + bytes([0x80 if it[1] else 0]) + it[3][0]
        if groups or len(it[3]) > 1 or it[5] is not None:
            g = el(0xA1, payload)
            dur = it[4]
            if dur is None and len(it[3]) > 1: dur = len(it[3]) * 20   # (20 ms Opus frames; the Vorbis lacing below overrides)
            if len(it[3]) > 1 and mode[0] == 'xiph': dur = None
            if dur is not None and len(it[3]) > 1 and mode[0] != 'xiph': pass
            if dur is not None: g += uint(0x9B, dur)
            if not it[1]: g += sint(0xFB, -1)
            if it[5] is not None: g += sint(0x75A2, it[5])
            body += el(0xA0, g)
        else:
            body += el(0xA3, payload)
    cluster_list.append((start, body))
    for start, body in cluster_list:
        clusters += el(0x1F43B675, uint(0xE7, start) + body, unknown)
    segment = el(0x18538067, info_el + tracks_el + clusters, unknown)
    open(path, 'wb').write(ebml_header + segment)

def ctr_bytes(iv8, n):
    """The AES-CTR key stream for n bytes: the counter block is the 8 byte IV and a 64 bit block counter from 0."""
    out = b''
    rks = expand_key(KEY1)
    for blk in range((n + 15) // 16):
        out += bytes(encrypt_block(rks, iv8 + blk.to_bytes(8, 'big')))
    return out[:n]

def blocks_of(blocks, number): return [x for x in blocks if x.track == number]

# ---- the files ------------------------------------------------------------------------------------------------------------------------------

bufs = {}
for name in ("webm_vp9_opus.webm", "webm_vp8_vorbis.webm", "vbr_opus.tmp.webm"):
    bufs[name] = parse(name)

def audio_number(tracks): return [t.number for t in tracks if t.kind == 2][0]
def video_number(tracks): return [t.number for t in tracks if t.kind == 1][0]

b, tracks, blocks, info = bufs["webm_vp9_opus.webm"]
a, v = audio_number(tracks), video_number(tracks)
mux("webm_live.webm", info, tracks, blocks, unknown=True)
mux("webm_groups.webm", info, tracks, blocks, groups=True, laces={a: ('fixed', 4)})
mux("webm_zlib.webm", info, tracks, blocks, deflate={v})
mux("webm_enc.webm", info, tracks, blocks, encrypt=True)
# the VBR Opus sound with the EBML lacing, in the video's file
_, vtracks, vblocks, vinfo = bufs["vbr_opus.tmp.webm"]
va = audio_number(vtracks)
mixed_tracks = [t for t in tracks if t.kind == 1] + [t for t in vtracks if t.kind == 2]
for t in mixed_tracks[1:]:
    t.number = 2
    t.children = [(x, bytes([2]) if x == 0xD7 else q) for x, q in t.children]
for x in vblocks: x.track = 2
mixed = blocks_of(blocks, v) + vblocks
mux("webm_ebml.webm", info, mixed_tracks, mixed, laces={2: ('ebml', 4)})
b2, tracks2, blocks2, info2 = bufs["webm_vp8_vorbis.webm"]
a2 = audio_number(tracks2)
mux("webm_xiph.webm", info2, tracks2, blocks2, laces={a2: ('xiph', 4)})

# VP8 with hidden frames: the frames of an IVF file, one block each
def ivf_blocks(path):
    d = open(path, 'rb').read()
    w, h, den, num = struct.unpack('<HHII', d[12:24])
    p, out = 32, []
    while p < len(d):
        size, pts = struct.unpack('<IQ', d[p:p + 12])
        blk = Block()
        blk.data, blk.pts, blk.duration, blk.discard, blk.track = d[p + 12:p + 12 + size], pts * 1000 * num // den, None, None, 1
        blk.key = not (blk.data[0] & 1)
        out.append(blk)
        p += 12 + size
    return w, h, out
w, h, arf_blocks = ivf_blocks("vp8_arf.ivf")
arf_track = Track()
arf_track.number, arf_track.kind = 1, 1
arf_track.children = [(0xD7, b'\x01'), (0x73C5, b'\x01'), (0x83, b'\x01'), (0x9C, b'\x00'), (0x86, b'V_VP8'), (0xE0, uint(0xB0, w) + uint(0xBA, h))]
mux("webm_vp8_arf.webm", {'scale': 1000000, 'duration': float(arf_blocks[-1].pts + 40)}, [arf_track], arf_blocks)

# the sound alone
def sound_only(src, dst):
    run("ffmpeg", "-nostdin", "-v", "error", "-y", "-i", src, "-vn", "-c:a", "copy", "-f", "webm", dst)
sound_only("webm_vp9_opus.webm", "webm_opus.weba")
sound_only("webm_vp8_vorbis.webm", "webm_vorbis.weba")
run("ffmpeg", "-nostdin", "-v", "error", "-y", "-i", "webm_vp9_opus.webm", "-vn", "-c:a", "copy", "-f", "ogg", "webm_ref_opus.ogg")
run("ffmpeg", "-nostdin", "-v", "error", "-y", "-i", "webm_vp8_vorbis.webm", "-vn", "-c:a", "copy", "-f", "ogg", "webm_ref_vorbis.ogg")
os.remove("vbr_opus.tmp.webm")

# ---- the vectors ----------------------------------------------------------------------------------------------------------------------------

def fnv(data, h=0xcbf29ce484222325):
    for c in data: h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def planes_hashes(path, w=64, h=48):
    raw = run("ffmpeg", "-nostdin", "-v", "error", "-i", path, "-an", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-", capture=True)
    size = w * h * 3 // 2
    return [fnv(raw[i:i + size]) for i in range(0, len(raw), size)]

out = ["// Generated by tests/data/make_webm_fixtures.py: what tests/WebmTest.cpp expects of the fixtures. Do not edit.\n"]
def emit_file(tag, path, blocks_, tracks_):
    for t in tracks_:
        mine = blocks_of(blocks_, t.number)
        kind = 'video' if t.kind == 1 else 'audio'
        out.append("static const WebmFrameVector k_%s_%s[] = {\n" % (tag, kind))
        for x in mine:
            out.append("    {%d, %d, %s, 0x%016xull},\n" % (x.pts * info['scale'] // 1000, len(x.data), 'true' if (x.key or t.kind == 2) else 'false', fnv(x.data)))
        out.append("};\n")
emit_file("vp9_opus", "webm_vp9_opus.webm", blocks, tracks)
emit_file("vp8_vorbis", "webm_vp8_vorbis.webm", blocks2, tracks2)
emit_file("vp8_arf", "webm_vp8_arf.webm", arf_blocks, [arf_track])
out.append("static const uint64_t k_vp8_arf_pictures[] = {%s};\n" % ", ".join("0x%016xull" % x for x in planes_hashes("webm_vp8_arf.webm", 64, 48)))
out.append("static const uint64_t k_vp9_opus_pictures[] = {%s};\n" % ", ".join("0x%016xull" % x for x in planes_hashes("webm_vp9_opus.webm")))
out.append("static const uint64_t k_vp8_vorbis_pictures[] = {%s};\n" % ", ".join("0x%016xull" % x for x in planes_hashes("webm_vp8_vorbis.webm")))
open("../WebmVectors.inc", "w").write("".join(out))
print("ok")
