#!/usr/bin/env python3
"""Writes MPEG-2 test streams that use the prediction modes ffmpeg's encoder never produces: field pictures (as pairs, the second predicted from the first), 16x8
prediction, dual prime, skipped macroblocks and bidirectional prediction in field pictures. The pictures hold no residual except the intra ones, whose blocks have a DC
coefficient only, so what a decoder does with the motion vectors shows directly. The expected pictures come from ffmpeg's decoder (see scripts/gen_mpeg2_test_streams.sh);
mpeg2dec is the second opinion.

  gen_mpeg2_synthetic.py out_prefix        writes out_prefix_frames.m2v, out_prefix_fields.m2v and out_prefix_fields_bff.m2v

The variable length codes are read from src/Hardware/VideoCodec/Software/Mpeg2Tables.inc (the decoder's own tables: a decoder that reads the streams the way the
standard says proves them right).
"""
import random
import re
import sys

W, H = 96, 96   # 6 x 6 macroblocks; a frame of an interlaced sequence is a whole number of macroblock pairs


def load_tables():
    text = open('src/Hardware/VideoCodec/Software/Mpeg2Tables.inc').read()
    out = {}
    for name, body in re.findall(r'constexpr \w+ (\w+)\[\d+\] = \{(.*?)\};', text, re.S):
        out[name] = [int(v) for v in re.findall(r'-?\d+', body)]
    for name in ('kAddressEscapeCode', 'kAddressEscapeBits'):
        out[name] = int(re.search(name + r' = (\d+);', text).group(1))
    return out


T = load_tables()


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def start_code(self, code):
        self.align()
        self.put(0x000001, 24)
        self.put(code, 8)

    def data(self):
        self.align()
        return bytes(int(''.join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


I, P, B = 1, 2, 3
TOP, BOTTOM, FRAME = 1, 2, 3


class Encoder:
    def __init__(self, seed):
        self.bw = BitWriter()
        self.rng = random.Random(seed)
        self.tr = 0

    # -- headers
    def sequence(self):
        bw = self.bw
        bw.start_code(0xB3)
        bw.put(W, 12); bw.put(H, 12); bw.put(1, 4); bw.put(5, 4); bw.put(0x3FFFF, 18); bw.put(1, 1); bw.put(112, 10); bw.put(0, 1); bw.put(0, 1); bw.put(0, 1)
        bw.start_code(0xB5)
        bw.put(1, 4); bw.put(0x48, 8); bw.put(0, 1); bw.put(1, 2); bw.put(0, 2); bw.put(0, 2); bw.put(0, 12); bw.put(1, 1); bw.put(0, 8); bw.put(0, 1); bw.put(0, 2); bw.put(0, 5)
        bw.start_code(0xB8)
        bw.put(0, 25); bw.put(1, 1); bw.put(0, 1)

    def picture(self, ptype, structure, top_first, f_codes, frame_dct_pred=False, dc_precision=0):
        bw = self.bw
        bw.start_code(0x00)
        bw.put(self.tr & 0x3FF, 10)
        bw.put(ptype, 3); bw.put(0xFFFF, 16)
        if ptype in (P, B):
            bw.put(0, 1); bw.put(7, 3)
        if ptype == B:
            bw.put(0, 1); bw.put(7, 3)
        bw.put(0, 1)   # extra_bit_picture
        bw.start_code(0xB5)
        bw.put(8, 4)
        for fc in (f_codes[0][0], f_codes[0][1], f_codes[1][0], f_codes[1][1]):
            bw.put(fc, 4)
        bw.put(dc_precision, 2); bw.put(structure, 2); bw.put(1 if top_first else 0, 1); bw.put(1 if frame_dct_pred else 0, 1)
        bw.put(0, 1); bw.put(0, 1); bw.put(0, 1); bw.put(0, 1); bw.put(0, 1); bw.put(0, 1); bw.put(0 if structure != FRAME else 0, 1); bw.put(0, 1)
        # (concealment 0, q_scale_type 0, intra_vlc_format 0, alternate_scan 0, repeat_first_field 0, chroma_420_type 0, progressive_frame 0, composite_display 0)
        self.tr += 1

    # -- macroblock layer helpers
    def vlc(self, code_table, bits_table, index):
        self.bw.put(T[code_table][index], T[bits_table][index])

    def address_increment(self, n):
        while n > 33:
            self.bw.put(T['kAddressEscapeCode'], T['kAddressEscapeBits'])
            n -= 33
        self.vlc('kAddressIncrementCode', 'kAddressIncrementBits', n - 1)

    def motion_component(self, f_code, value, pred):
        f = 1 << (f_code - 1)
        low, high, rng = -16 * f, 16 * f - 1, 32 * f
        delta = value - pred
        if delta < low:
            delta += rng
        if delta > high:
            delta -= rng
        if delta == 0:
            self.vlc('kMotionCodeCode', 'kMotionCodeBits', 0)
            return
        a = abs(delta)
        if f_code == 1:
            code, residual = a, 0
        else:
            code, residual = ((a - 1) >> (f_code - 1)) + 1, (a - 1) & (f - 1)
        self.vlc('kMotionCodeCode', 'kMotionCodeBits', code)
        self.bw.put(1 if delta < 0 else 0, 1)
        if f_code != 1:
            self.bw.put(residual, f_code - 1)

    def dmv(self, v):
        if v == 0:
            self.bw.put(0, 1)
        else:
            self.bw.put(1, 1)
            self.bw.put(1 if v < 0 else 0, 1)

    def intra_block_dc_only(self, comp, level, dc_pred):
        # dct_dc_size and the differential, then the end of block (B.14: "10").
        diff = level - dc_pred[comp]
        dc_pred[comp] = level
        size = 0 if diff == 0 else abs(diff).bit_length()
        if comp == 0:
            self.vlc('kDcSizeLumaCode', 'kDcSizeLumaBits', size)
        else:
            self.vlc('kDcSizeChromaCode', 'kDcSizeChromaBits', size)
        if size:
            self.bw.put(diff if diff > 0 else diff + (1 << size) - 1, size)
        self.bw.put(2, 2)   # EOB



def fits(bx, by, bw, bh, vx, vy, pw, ph):
    # The block the half-sample vector (vx, vy) points at, with the extra sample of the interpolation and a margin of one, lies inside the plane.
    x0, y0 = bx + (vx >> 1) - 1, by + (vy >> 1) - 1
    return x0 >= 0 and y0 >= 0 and x0 + bw + 3 <= pw and y0 + bh + 3 <= ph


LOG = []   # (temporal reference, picture type, row, column, what) per macroblock, for debugging


class PictureWriter:
    """Writes the slices of one picture. `kind` is 'frame' or 'field' (a field picture)."""

    def __init__(self, enc, ptype, structure, top_first, first_field_only=False):
        self.first_field_only = first_field_only   # the second field of the first picture: the only reference is the first field
        self.e, self.ptype, self.structure, self.top_first = enc, ptype, structure, top_first
        self.frame = structure == FRAME
        self.rows = 6 if self.frame else 3
        self.cols = 6
        self.rng = enc.rng

    # -- vectors
    def field_select(self):
        if self.first_field_only:
            return 0 if self.top_first else 1
        return self.rng.randint(0, 1)

    def pick(self, mx, my, part, count, field_in_frame):
        """A vector for macroblock (mx, my); `part` 0/1 for the half of a 16x8 or the field of a frame macroblock."""
        for _ in range(12):
            vx, vy = self.rng.randint(-6, 6), self.rng.randint(-6, 6)
            if self.frame and not field_in_frame:
                ok = fits(mx * 16, my * 16, 16, 16, vx, vy, W, H)
            elif self.frame:
                ok = fits(mx * 16, my * 8, 16, 8, vx, vy, W, H // 2)
            elif count == 2:
                ok = fits(mx * 16, my * 16 + 8 * part, 16, 8, vx, vy, W, H // 2)
            else:
                ok = fits(mx * 16, my * 16, 16, 16, vx, vy, W, H // 2)
            if ok:
                return vx, vy
        return 0, 0

    def write_vectors(self, mx, my, motion, dirs):
        e = self.e
        for d in dirs:
            fc = self.fcode
            pmv = self.pmv[d]
            if motion == 'frame':
                vx, vy = self.pick(mx, my, 0, 1, False)
                e.motion_component(fc, vx, pmv[0][0]); e.motion_component(fc, vy, pmv[0][1])
                pmv[0] = [vx, vy]; pmv[1] = [vx, vy]
            elif motion == 'field' and self.frame:
                for j in range(2):
                    e.bw.put(self.field_select(), 1)
                    vx, vy = self.pick(mx, my, j, 2, True)
                    e.motion_component(fc, vx, pmv[j][0]); e.motion_component(fc, vy, pmv[j][1] >> 1)
                    pmv[j] = [vx, 2 * vy]
            elif motion == 'field':
                e.bw.put(self.field_select(), 1)
                vx, vy = self.pick(mx, my, 0, 1, False)
                e.motion_component(fc, vx, pmv[0][0]); e.motion_component(fc, vy, pmv[0][1])
                pmv[0] = [vx, vy]; pmv[1] = [vx, vy]
            elif motion == '16x8':
                for j in range(2):
                    e.bw.put(self.field_select(), 1)
                    vx, vy = self.pick(mx, my, j, 2, False)
                    e.motion_component(fc, vx, pmv[j][0]); e.motion_component(fc, vy, pmv[j][1])
                    pmv[j] = [vx, vy]
            else:   # dual prime
                shift = 1 if self.frame else 0
                vx, vy = self.rng.randint(-4, 4), self.rng.randint(-3, 3)
                e.motion_component(fc, vx, pmv[0][0]); e.dmv(self.rng.randint(-1, 1))
                e.motion_component(fc, vy, pmv[0][1] >> shift); e.dmv(self.rng.randint(-1, 1))
                pmv[0] = [vx, vy << shift]; pmv[1] = [vx, vy << shift]

    # -- macroblocks
    def intra(self, comp_levels):
        e = self.e
        bits = {I: (1, 1), P: (3, 5), B: (3, 5)}[self.ptype]
        e.bw.put(*bits)
        if self.frame:
            e.bw.put(self.rng.randint(0, 1), 1)   # field_dct
        for b in range(6):
            comp = 0 if b < 4 else b - 3
            e.intra_block_dc_only(comp, self.rng.randint(70, 190), self.dc_pred)
        self.pmv = [[[0, 0], [0, 0]] for _ in range(2)]

    def inter(self, mx, my, motion, dirs):
        e = self.e
        if self.ptype == P:
            e.bw.put(1, 3)
        else:
            code = {(0, 1): (2, 2), (1,): (2, 3), (0,): (2, 4)}[tuple(dirs) if tuple(dirs) != (0, 1) else (0, 1)]
            e.bw.put(*code)
        mt = {'frame': 2, 'field': 1, 'dmv': 3, '16x8': 2}[motion]
        e.bw.put(mt, 2)
        self.write_vectors(mx, my, motion, dirs)
        self.dc_pred = [128, 128, 128]

    def slice(self, my):
        e = self.e
        e.bw.start_code(1 + my)
        e.bw.put(self.rng.randint(1, 31), 5)
        e.bw.put(0, 1)
        self.dc_pred = [128, 128, 128]
        self.pmv = [[[0, 0], [0, 0]] for _ in range(2)]
        skipped = 0
        prev_inter = False
        for mx in range(self.cols):
            last = mx == self.cols - 1
            r = self.rng.random()
            interior = 1 <= mx <= self.cols - 2 and 1 <= my <= self.rows - 2 and not self.first_field_only
            if self.ptype == I:
                kind = 'intra'
            elif r < 0.12:
                kind = 'intra'
            elif r < 0.30 and mx > 0 and not last and not self.first_field_only and (self.ptype == P or prev_inter):
                kind = 'skip'
            else:
                kind = 'inter'
            if kind == 'skip':
                LOG.append((self.e.tr, self.ptype, my, mx, 'skip'))
                skipped += 1
                if self.ptype == P:
                    self.pmv = [[[0, 0], [0, 0]] for _ in range(2)]
                self.dc_pred = [128, 128, 128]
                continue
            e.address_increment(1 + skipped)
            skipped = 0
            if kind == 'intra':
                self.intra(None)
                prev_inter = False
                continue
            if self.frame:
                choices = ['frame', 'field'] + (['dmv'] if self.ptype == P and interior else [])
            else:
                choices = ['field', '16x8'] + (['dmv'] if self.ptype == P and interior else [])
            motion = self.rng.choice(choices)
            if self.ptype == P:
                dirs = (0,)
            else:
                dirs = self.rng.choice([(0,), (1,), (0, 1)])
            LOG.append((self.e.tr, self.ptype, my, mx, motion, dirs))
            self.inter(mx, my, motion, dirs)
            prev_inter = True

    def write(self, fcodes):
        e = self.e
        e.picture(self.ptype, self.structure, self.top_first, fcodes)
        self.fcode = 2
        for my in range(self.rows):
            self.slice(my)


def build_frames(seed):
    e = Encoder(seed)
    e.sequence()
    for ptype, tr in ((I, 0), (P, 3), (B, 1), (B, 2), (P, 6), (B, 4), (B, 5)):
        e.tr = tr
        PictureWriter(e, ptype, FRAME, True).write(fcodes_for(ptype))
    e.bw.start_code(0xB7)
    return e.bw.data()


def fcodes_for(ptype):
    return [[15, 15], [15, 15]] if ptype == I else [[2, 2], [15, 15]] if ptype == P else [[2, 2], [2, 2]]


def build_fields(seed, top_first):
    e = Encoder(seed)
    e.sequence()
    first = TOP if top_first else BOTTOM
    second = BOTTOM if top_first else TOP
    for (t1, t2), tr in (((I, P), 0), ((P, P), 3), ((B, B), 1), ((B, B), 2), ((P, P), 6), ((B, B), 4), ((B, B), 5)):
        e.tr = tr
        PictureWriter(e, t1, first, top_first).write(fcodes_for(t1))
        e.tr = tr
        PictureWriter(e, t2, second, top_first, first_field_only=(t1 == I)).write(fcodes_for(t2))
    e.bw.start_code(0xB7)
    return e.bw.data()


if __name__ == '__main__':
    prefix = sys.argv[1]
    open(prefix + '_frames.m2v', 'wb').write(build_frames(1))
    open(prefix + '_fields.m2v', 'wb').write(build_fields(2, True))
    open(prefix + '_fields_bff.m2v', 'wb').write(build_fields(3, False))
