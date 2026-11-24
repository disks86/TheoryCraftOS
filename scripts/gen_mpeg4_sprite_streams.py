#!/usr/bin/env python3
# Makes the static sprite streams of the MPEG-4 Part 2 decoder's tests (tests/data/m4vdec_sprite_*.m4v) and the pictures the standard says they decode to
# (*.yuv, I420, the S planes in order). No encoder at hand writes static sprites (sprite_enable = "static", ISO/IEC 14496-2 7.8, a Main profile tool) and FFmpeg's
# decoder does not decode them, so this is an encoder with its own model of the standard:
#
#   - the sprite is the layer's first plane, an I plane of sprite_width x sprite_height (data partitioning with reversible VLCs and random coefficients, as
#     scripts/gen_mpeg4_rvlc_stream.py writes them). That part is checked against FFmpeg: the same I plane under a plain layer of the sprite's size
#     (sprite_enable 0) is decoded by FFmpeg, and its picture is the sprite the model warps;
#   - the S planes carry only their sprite trajectory (0 .. 4 warping points: stationary, translation, isotropic and affine transforms, perspective) and the
#     brightness change factor; the model warps the sprite with the equations of 7.8.4 / 7.8.5 written in their division form (the decoder uses the shift form of
#     the same equations), bilinear sample reconstruction with rounding_control 0, and the sprite padded (its edge samples repeated) outside its area;
#   - not coded S planes (vop_coded 0) give no picture.
#
# Where the published text leaves room the model chooses as the decoder does (and the decoder's comments say so):
#   - a single warping point's chrominance offset is (i0' >> 1) | (i0' & 1), as FFmpeg's GMC (which shares the equations) has it;
#   - the brightness change (7.8.5: Y * (brightness_change_factor * 0.01 + 1), luminance only, clipped to 0 .. 255) rounds to the nearest integer, halves up:
#     the standard does not say how the product becomes an integer (an M4IF technical note of 2002 points this out).
#
# Also m4vdec_sprite_lowlatency.m4v: a layer with low_latency_sprite_enable 1, which the decoder refuses.
# Usage: scripts/gen_mpeg4_sprite_streams.py   (needs ffmpeg for the sprites; run from the repository root)
import os
import random
import re
import subprocess
import tempfile

INC = 'src/Hardware/VideoCodec/Software/Mpeg4Tables.inc'
text = open(INC).read()


def table(name):
    m = re.search(r'constexpr \w+ ' + name + r'\s*=\s*\{(.*?)\};', text, re.S)
    groups = re.findall(r'\{([^{}]*)\}', m.group(1))
    return [[int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', g)] for g in groups]


def array(name):
    m = re.search(name + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]


RV_INTRA = table('kIntraReversible')
INTRA_MCBPC = list(zip(array('kIntraMcbpcCode'), array('kIntraMcbpcBits')))
CBPY = list(zip(array('kCbpyCode'), array('kCbpyBits')))
DC_LUMA = list(zip(array('kDcLumaCode'), array('kDcLumaBits')))
DC_CHROMA = list(zip(array('kDcChromaCode'), array('kDcChromaBits')))


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def code(self, pair):
        self.put(pair[0], pair[1])

    def stuff(self):   # next_start_code(): a zero, then ones to the byte boundary
        self.bits.append(0)
        while len(self.bits) % 8:
            self.bits.append(1)

    def start(self, code):
        assert len(self.bits) % 8 == 0
        self.put(0x000001, 24)
        self.put(code, 8)

    def bytes(self):
        assert len(self.bits) % 8 == 0
        return bytes(int(''.join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


# ---- the sprite: an I plane, data partitioned, reversible VLCs -------------------------------------------------------------------------------------------

def rvlc_events(rng):
    events, pos = [], 1
    while True:
        run = rng.choice([0, 0, 0, 1, 2, 5]) if rng.random() < 0.9 else rng.randint(6, 20)
        level = rng.choice([1, 1, 1, 2, 3]) if rng.random() < 0.85 else rng.randint(4, 30)
        if rng.random() < 0.5:
            level = -level
        if pos + run > 63:
            run = 63 - pos
        pos += run + 1
        last = pos >= 64 or rng.random() < 0.3
        events.append((1 if last else 0, run, level))
        if last:
            return events


def put_rvlc(b, events):
    codes, bits, runs, levels = RV_INTRA
    for last, run, level in events:
        mag = abs(level)
        index = None
        for i in range(169):
            if runs[i] == run and levels[i] == mag and (i >= 103) == bool(last):
                index = i
                break
        if index is None:   # the escape: ESC, 1, LAST, RUN, 1, LEVEL (11 bits), 1, ESC, then the sign
            b.put(codes[169], bits[169])
            b.put(1, 1)
            b.put(last, 1)
            b.put(run, 6)
            b.put(1, 1)
            b.put(mag, 11)
            b.put(1, 1)
            b.put(codes[169], bits[169])
        else:
            b.put(codes[index], bits[index])
        b.put(1 if level < 0 else 0, 1)


def put_dc(b, n, value):
    size = 0 if value == 0 else abs(value).bit_length()
    b.code((DC_LUMA if n < 4 else DC_CHROMA)[size])
    if size:
        b.put(value if value > 0 else value + (1 << size) - 1, size)
        if size > 8:
            b.put(1, 1)


def intra_vop(b, rng, mbs, time_bits, quant):
    """The sprite's I plane (one video packet: partitions of DC, modes and texture)."""
    b.start(0xB6)
    b.put(0, 2)        # vop_coding_type I
    b.put(0, 1)        # modulo_time_base
    b.put(1, 1)
    b.put(0, time_bits)
    b.put(1, 1)
    b.put(1, 1)        # vop_coded
    b.put(0, 3)        # intra_dc_vlc_thr: DC always on its own
    b.put(quant, 5)
    modes = [(rng.randint(0, 63), rng.random() < 0.4) for _ in range(mbs)]
    for cbp, _ in modes:
        b.code(INTRA_MCBPC[cbp & 3])
        for n in range(6):
            put_dc(b, n, rng.randint(-9, 9))
    b.put(0x6B001, 19)   # dc_marker
    for cbp, ac_pred in modes:
        b.put(1 if ac_pred else 0, 1)
        b.code(CBPY[cbp >> 2])
    for cbp, _ in modes:
        for n in range(6):
            if cbp & (32 >> n):
                put_rvlc(b, rvlc_events(rng))
    b.stuff()


def layer(b, c, width, height, sprite):
    """A visual object sequence, visual object and video object layer header (Main profile); sprite False: the plain layer FFmpeg decodes."""
    b.start(0xB0)
    b.put(0x32, 8)     # profile_and_level_indication: Main profile, level 2
    b.start(0xB5)      # visual_object_start_code
    b.put(0, 1)        # is_visual_object_identifier
    b.put(1, 4)        # visual_object_type: video
    b.put(0, 1)        # video_signal_type
    b.stuff()
    b.start(0x00)      # video_object_start_code
    b.start(0x20)      # video_object_layer_start_code
    b.put(0, 1)        # random_accessible_vol
    b.put(3, 8)        # video_object_type_indication: Main
    verid = c['verid']
    if verid != 1:
        b.put(1, 1)    # is_object_layer_identifier
        b.put(verid, 4)
        b.put(1, 3)    # video_object_layer_priority
    else:
        b.put(0, 1)
    b.put(1, 4)        # aspect_ratio_info: square
    b.put(0, 1)        # vol_control_parameters
    b.put(0, 2)        # rectangular
    b.put(1, 1)
    b.put(25, 16)      # vop_time_increment_resolution
    b.put(1, 1)
    b.put(0, 1)        # fixed_vop_rate
    b.put(1, 1)
    b.put(width, 13)
    b.put(1, 1)
    b.put(height, 13)
    b.put(1, 1)
    b.put(0, 1)        # interlaced
    b.put(1, 1)        # obmc_disable
    if not sprite:
        b.put(0, 1 if verid == 1 else 2)
    else:
        b.put(1, 1 if verid == 1 else 2)   # sprite_enable: static
        b.put(c['sw'], 13)
        b.put(1, 1)
        b.put(c['sh'], 13)
        b.put(1, 1)
        b.put(c['left'] & 0x1FFF, 13)
        b.put(1, 1)
        b.put(c['top'] & 0x1FFF, 13)
        b.put(1, 1)
        b.put(c['points'], 6)
        b.put(c['accuracy'], 2)
        b.put(1 if c['brightness'] else 0, 1)
        b.put(1 if c.get('low_latency') else 0, 1)
    b.put(0, 1)        # not_8_bit
    b.put(0, 1)        # quant_type: H.263
    if verid != 1:
        b.put(0, 1)    # quarter_sample
    b.put(1, 1)        # complexity_estimation_disable
    b.put(0, 1)        # resync_marker_disable
    b.put(1, 1)        # data_partitioned
    b.put(1, 1)        # reversible_vlc
    if verid != 1:
        b.put(0, 1)    # newpred_enable
        b.put(0, 1)    # reduced_resolution_vop_enable
    b.put(0, 1)        # scalability
    b.stuff()


# Table B-33 (dmv_length) and B-34 (brightness_change_factor_length).
DMV_LENGTH = ['00', '010', '011', '100', '101', '110', '1110', '11110', '111110', '1111110', '11111110', '111111110', '1111111110', '11111111110',
              '111111111110']


def put_dmv(b, d):
    n = 0 if d == 0 else abs(d).bit_length()
    for ch in DMV_LENGTH[n]:
        b.put(int(ch), 1)
    if n:
        b.put(d if d > 0 else d + (1 << n) - 1, n)
    b.put(1, 1)    # marker_bit


def put_brightness(b, f):
    assert -112 <= f <= 1648 and f != 0
    if -16 <= f <= 16:
        b.put(0, 1)
        b.put(f + 16 if f < 0 else f + 15, 5)
    elif -48 <= f <= 48:
        b.put(2, 2)
        b.put(f + 48 if f < 0 else f + 15, 6)
    elif -112 <= f <= 112:
        b.put(6, 3)
        b.put(f + 112 if f < 0 else f + 15, 7)
    elif f <= 624:
        b.put(14, 4)
        b.put(f - 113, 9)
    else:
        b.put(15, 4)
        b.put(f - 625, 10)


def sprite_vop(b, c, time, s):
    b.start(0xB6)
    b.put(3, 2)        # vop_coding_type S
    b.put(0, 1)        # modulo_time_base
    b.put(1, 1)
    b.put(time, 5)
    b.put(1, 1)
    b.put(1 if s.get('coded', True) else 0, 1)
    if s.get('coded', True):
        b.put(s.get('dc_thr', 0), 3)   # intra_dc_vlc_thr (sent, unused)
        for du, dv in s['traj'][:c['points']]:
            put_dmv(b, du)
            put_dmv(b, dv)
        if c['brightness']:
            put_brightness(b, s['bright'])
    b.stuff()


# ---- the model of the warp (7.8.4, 7.8.5) ---------------------------------------------------------------------------------------------------------------

def rdiv2(n, d):    # "//": to the nearest, halves away from zero
    q, rem = divmod(abs(n), d)
    if 2 * rem >= d:
        q += 1
    return q if n >= 0 else -q


def rdiv3(n, d):    # "///": to the nearest, halves (whatever the sign) toward plus infinity
    if d < 0:
        n, d = -n, -d
    return (2 * n + d) // (2 * d)


def trunc_div2(v):  # "/" by 2: toward zero
    return -((-v) // 2) if v < 0 else v // 2


def warp(c, sprite, traj, bright, W, H):
    acc, points = c['accuracy'], c['points']
    s = 2 << acc
    r = 16 // s
    du = [traj[i][0] if i < points else 0 for i in range(4)]
    dv = [traj[i][1] if i < points else 0 for i in range(4)]
    vop = [(0, 0), (W, 0), (0, H), (W, H)]
    sp = [(s // 2 * (2 * vop[0][0] + du[0]), s // 2 * (2 * vop[0][1] + dv[0])),
          (s // 2 * (2 * vop[1][0] + du[1] + du[0]), s // 2 * (2 * vop[1][1] + dv[1] + dv[0])),
          (s // 2 * (2 * vop[2][0] + du[2] + du[0]), s // 2 * (2 * vop[2][1] + dv[2] + dv[0])),
          (s // 2 * (2 * vop[3][0] + du[3] + du[2] + du[1] + du[0]), s // 2 * (2 * vop[3][1] + dv[3] + dv[2] + dv[1] + dv[0]))]
    (i0, j0), (i1, j1), (i2, j2), (i3, j3) = sp
    alpha = 1
    while (1 << alpha) < W:
        alpha += 1
    beta = 1
    while (1 << beta) < H:
        beta += 1
    Wp, Hp = 1 << alpha, 1 << beta
    i1v = 16 * Wp + rdiv2((W - Wp) * (r * i0) + Wp * (r * i1 - 16 * W), W)
    j1v = rdiv2((W - Wp) * (r * j0) + Wp * (r * j1), W)
    i2v = rdiv2((H - Hp) * (r * i0) + Hp * (r * i2), H)
    j2v = 16 * Hp + rdiv2((H - Hp) * (r * j0) + Hp * (r * j2 - 16 * H), H)
    if points == 4:
        g = ((i0 - i1 - i2 + i3) * (j2 - j3) - (i2 - i3) * (j0 - j1 - j2 + j3)) * H
        h = ((i1 - i3) * (j0 - j1 - j2 + j3) - (i0 - i1 - i2 + i3) * (j1 - j3)) * W
        D = (i1 - i3) * (j2 - j3) - (i2 - i3) * (j1 - j3)
        a = D * (i1 - i0) * H + g * i1
        bb = D * (i2 - i0) * W + h * i2
        cc = D * i0 * W * H
        d = D * (j1 - j0) * H + g * j1
        e = D * (j2 - j0) * W + h * j2
        f = D * j0 * W * H

    def luma(i, j):
        if points == 0:
            return s * i, s * j
        if points == 1:
            return i0 + s * i, j0 + s * j
        if points == 2:
            return (i0 + rdiv3((-r * i0 + i1v) * i + (r * j0 - j1v) * j, Wp * r), j0 + rdiv3((-r * j0 + j1v) * i + (-r * i0 + i1v) * j, Wp * r))
        if points == 3:
            return (i0 + rdiv3((-r * i0 + i1v) * Hp * i + (-r * i0 + i2v) * Wp * j, Wp * Hp * r),
                    j0 + rdiv3((-r * j0 + j1v) * Hp * i + (-r * j0 + j2v) * Wp * j, Wp * Hp * r))
        den = g * i + h * j + D * W * H
        return rdiv3(a * i + bb * j + cc, den), rdiv3(d * i + e * j + f, den)

    def chroma(ic, jc):
        Ic, Jc = 4 * ic + 1, 4 * jc + 1
        if points == 0:
            return s * ic, s * jc
        if points == 1:
            return s * ic + ((i0 >> 1) | (i0 & 1)), s * jc + ((j0 >> 1) | (j0 & 1))
        if points == 2:
            return (rdiv3((-r * i0 + i1v) * Ic + (r * j0 - j1v) * Jc + 2 * Wp * r * i0 - 16 * Wp, 4 * Wp * r),
                    rdiv3((-r * j0 + j1v) * Ic + (-r * i0 + i1v) * Jc + 2 * Wp * r * j0 - 16 * Wp, 4 * Wp * r))
        if points == 3:
            return (rdiv3((-r * i0 + i1v) * Hp * Ic + (-r * i0 + i2v) * Wp * Jc + 2 * Wp * Hp * r * i0 - 16 * Wp * Hp, 4 * Wp * Hp * r),
                    rdiv3((-r * j0 + j1v) * Hp * Ic + (-r * j0 + j2v) * Wp * Jc + 2 * Wp * Hp * r * j0 - 16 * Wp * Hp, 4 * Wp * Hp * r))
        den = 4 * g * Ic + 4 * h * Jc + 8 * D * W * H
        return (rdiv3(2 * a * Ic + 2 * bb * Jc + 4 * cc - (g * Ic + h * Jc + 2 * D * W * H) * s, den),
                rdiv3(2 * d * Ic + 2 * e * Jc + 4 * f - (g * Ic + h * Jc + 2 * D * W * H) * s, den))

    def sample(plane, pw, ph, x0, y0, F, G):
        x, y = F // s, G // s
        ri, rj = F - x * s, G - y * s

        def at(xx, yy):
            xx = min(max(xx - x0, 0), pw - 1)
            yy = min(max(yy - y0, 0), ph - 1)
            return plane[yy][xx]
        return ((s - rj) * ((s - ri) * at(x, y) + ri * at(x + 1, y)) + rj * ((s - ri) * at(x, y + 1) + ri * at(x + 1, y + 1)) + s * s // 2) // (s * s)

    Y, U, V = sprite
    left, top, sw, sh = c['left'], c['top'], c['sw'], c['sh']
    cl, ct = trunc_div2(left), trunc_div2(top)
    cw, ch = trunc_div2(left + sw) - cl, trunc_div2(top + sh) - ct
    out = bytearray()
    for j in range(H):
        for i in range(W):
            F, G = luma(i, j)
            v = sample(Y, sw, sh, left, top, F, G)
            if c['brightness']:
                p = v * (100 + bright)
                v = 0 if p <= 0 else min(255, (p + 50) // 100)
            out.append(v)
    for plane in (U, V):
        for jc in range((H + 1) // 2):
            for ic in range((W + 1) // 2):
                F, G = chroma(ic, jc)
                out.append(sample(plane, cw, ch, cl, ct, F, G))
    return bytes(out)


# ---- the streams ----------------------------------------------------------------------------------------------------------------------------------------

def ffmpeg_sprite(c, ivop):
    """FFmpeg's decode of the sprite's I plane under a plain layer of the sprite's size: the sprite (Y, U, V rows)."""
    b = Bits()
    layer(b, c, c['sw'], c['sh'], False)
    data = b.bytes() + ivop
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, 'sprite.m4v'), os.path.join(d, 'sprite.yuv')
        open(src, 'wb').write(data)
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y', '-threads', '1', '-cpuflags', '0', '-flags', 'bitexact', '-idct', 'simple',
                        '-i', src, '-f', 'rawvideo', '-pix_fmt', 'yuv420p', dst], check=True)
        raw = open(dst, 'rb').read()
    sw, sh = c['sw'], c['sh']
    cw, ch = (sw + 1) // 2, (sh + 1) // 2
    assert len(raw) == sw * sh + 2 * cw * ch, 'FFmpeg did not decode the sprite'
    Y = [list(raw[y * sw:(y + 1) * sw]) for y in range(sh)]
    off = sw * sh
    U = [list(raw[off + y * cw:off + (y + 1) * cw]) for y in range(ch)]
    off += cw * ch
    V = [list(raw[off + y * cw:off + (y + 1) * cw]) for y in range(ch)]
    return Y, U, V


CASES = [
    # stationary (0 points): the layer's area of the sprite, which starts left of and above it; a plane that is not coded
    dict(name='p0', verid=1, W=48, H=32, sw=80, sh=64, left=-16, top=-16, points=0, accuracy=0, brightness=False, seed=1,
         s=[dict(traj=[]), dict(coded=False), dict(traj=[], dc_thr=5)]),
    # translation in 1/16 samples, partly off the sprite (padding), with the brightness change (every length of its code); a size that is not whole macroblocks
    dict(name='p1', verid=2, W=40, H=24, sw=96, sh=64, left=-24, top=-20, points=1, accuracy=3, brightness=True, seed=2,
         s=[dict(traj=[(37, -21)], bright=-12), dict(traj=[(-411, 133)], bright=30), dict(traj=[(1203, 700)], bright=-80),
            dict(traj=[(5, 9)], bright=200), dict(traj=[(-3, -1)], bright=1000)]),
    # two points: rotation and zoom in half samples
    dict(name='p2', verid=1, W=48, H=32, sw=96, sh=80, left=-32, top=-24, points=2, accuracy=1, brightness=False, seed=3,
         s=[dict(traj=[(3, -5), (9, 7)]), dict(traj=[(-20, 11), (-31, -17)]), dict(traj=[(0, 0), (0, 0)])]),
    # three points: an affine warp in quarter samples
    dict(name='p3', verid=2, W=48, H=32, sw=96, sh=80, left=-24, top=-32, points=3, accuracy=2, brightness=False, seed=4,
         s=[dict(traj=[(5, 3), (14, -9), (-7, 12)]), dict(traj=[(-30, 17), (40, 22), (11, -35)]), dict(traj=[(100, -60), (-3, 8), (6, 5)])]),
    # four points: perspective, in 1/16 samples, with brightness change
    dict(name='p4', verid=1, W=48, H=32, sw=96, sh=80, left=-24, top=-24, points=4, accuracy=3, brightness=True, seed=5,
         s=[dict(traj=[(21, -13), (30, 16), (-12, 24), (9, -7)], bright=7), dict(traj=[(-60, 41), (-25, -30), (44, 18), (-17, 33)], bright=-45),
            dict(traj=[(4, 2), (0, 0), (0, 0), (0, 0)], bright=113)]),
]


def main():
    for c in CASES:
        rng = random.Random(c['seed'])
        mbs = ((c['sw'] + 15) // 16) * ((c['sh'] + 15) // 16)
        ib = Bits()
        intra_vop(ib, rng, mbs, 5, rng.randint(4, 9))
        ivop = ib.bytes()
        sprite = ffmpeg_sprite(c, ivop)
        b = Bits()
        layer(b, c, c['W'], c['H'], True)
        data = b.bytes() + ivop
        pictures = b''
        for t, s in enumerate(c['s']):
            sb = Bits()
            sprite_vop(sb, c, (t + 1) % 25, s)
            data += sb.bytes()
            if s.get('coded', True):
                pictures += warp(c, sprite, s['traj'], s.get('bright', 0), c['W'], c['H'])
        open('tests/data/m4vdec_sprite_%s.m4v' % c['name'], 'wb').write(data)
        open('tests/data/m4vdec_sprite_%s.yuv' % c['name'], 'wb').write(pictures)
        print('m4vdec_sprite_%s: %d bytes, %d pictures' % (c['name'], len(data), sum(1 for s in c['s'] if s.get('coded', True))))
    # low_latency_sprite_enable 1: refused
    c = dict(CASES[0], low_latency=True)
    rng = random.Random(9)
    b = Bits()
    layer(b, c, c['W'], c['H'], True)
    intra_vop(b, rng, ((c['sw'] + 15) // 16) * ((c['sh'] + 15) // 16), 5, 6)
    open('tests/data/m4vdec_sprite_lowlatency.m4v', 'wb').write(b.bytes())


if __name__ == '__main__':
    main()
