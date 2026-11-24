#!/usr/bin/env python3
"""Writes VC-1 test streams. There is no VC-1 encoder to be had, so this one makes streams out of random syntax elements: random macroblock types, vectors, coded block
patterns, quantizer changes and coefficients (all small, so that nothing saturates), in every combination the decoder supports. What the pictures look like does not matter;
what matters is that a reference decoder (ffmpeg's) and this OS's decoder read the same streams and make the same pictures. The expected pictures come from ffmpeg.

  gen_vc1_synthetic.py out_dir      writes out_dir/vc1_<name>.rcv (Simple and Main profile, "RCV" files) and out_dir/vc1_<name>.vc1 (Advanced profile elementary streams)
  gen_vc1_synthetic.py --expect bits.json out_dir
                                    the same, and writes where the header and the macroblock data of every start code unit end (bit positions in the unescaped
                                    payload, per stream and frame), to check a decoder's bit consumption against
  gen_vc1_synthetic.py --rangemap name pictures.yuv
                                    applies the range mapping of stream `name` (RANGE_MAPY / RANGE_MAPUV) to FFmpeg's pictures of it, which FFmpeg does not do
  gen_vc1_synthetic.py --selftest out.inc
                                    writes the streams of the VCN VC-1 self-test (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc) as C++ arrays

The variable length codes are read from src/Hardware/VideoCodec/Software/Vc1Tables.inc, the decoder's own tables, and for the interlaced syntax from
scripts/vc1_interlaced_tables.py (taken from FFmpeg): a reference decoder that reads the streams the way the standard says proves them right. The generator mirrors the
parts of the decoder state that decide which bits are present (coded block flag prediction, the hybrid motion vector prediction bit of progressive and of P field
pictures (with the field predictor scaling), the escape code lengths, the transform type state, slice boundaries).

Interlaced streams (IEnc, stream_interlaced) mix field pictures (both fields, the second behind a field start code), interlaced frame pictures and progressive pictures,
optionally cut into slices; FFmpeg is the reference for the syntax (vc1.c ff_vc1_parse_frame_header_adv, vc1_block.c, vc1_pred.c), see the notes at IEnc.
"""
import os
import random
import re
import struct
import sys


def load_tables():
    text = open('src/Hardware/VideoCodec/Software/Vc1Tables.inc').read()
    out = {}
    for name, body in re.findall(r'constexpr \w+ (\w+)\[\d+\] = \{(.*?)\};', text, re.S):
        out[name] = [int(v) for v in re.findall(r'-?\d+', body)]
    return out


T = load_tables()


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def code(self, codes, bits, index):
        self.put(codes[index], bits[index])

    def tell(self):
        return len(self.bits)

    def data(self, stop_bit=False):
        b = list(self.bits)
        if stop_bit:
            b.append(1)
        while len(b) % 8:
            b.append(0)
        return bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def escape_bytes(data):
    """Emulation prevention (Advanced profile): 00 00 followed by a byte below 4 gets a 03 in between."""
    out = bytearray()
    for i, b in enumerate(data):
        if len(out) >= 2 and out[-1] == 0 and out[-2] == 0 and b < 4:
            out.append(3)
        out.append(b)
    return bytes(out)


def table(name, index, width):
    return T[name][index * width:(index + 1) * width]


# -- the AC coefficient tables ------------------------------------------------------------------------------------------------------
AC_SIZE = T['kAcSize']
AC_LAST = T['kAcLastIndex']


def ac_entries(cs):
    """(run, level, last) of each non-escape code of coefficient table `cs` with its index."""
    base = cs * 185
    out = []
    for idx in range(AC_SIZE[cs] - 1):
        out.append((T['kAcRun'][base + idx], T['kAcLevel'][base + idx], idx >= AC_LAST[cs], idx))
    return out


AC_BY_KEY = [{(r, l, last): idx for (r, l, last, idx) in ac_entries(cs)} for cs in range(8)]
AC_SMALL = [[e for e in ac_entries(cs) if e[1] <= 3 and e[0] <= 6] for cs in range(8)]


class Enc:
    """One stream: a profile, its flags, and the pictures."""

    def __init__(self, seed, profile, width, height, **flags):
        self.rng = random.Random(seed)
        self.profile = profile            # 0 simple, 1 main, 3 advanced
        self.w, self.h = width, height
        self.mbw, self.mbh = (width + 15) // 16, (height + 15) // 16
        f = dict(loop_filter=0, multires=0, fastuvmc=1, extended_mv=0, dquant=0, vstransform=0, overlap=0, rangered=0, max_b_frames=0, quantizer=0, finterp=0,
                 postproc=0, broadcast=0, tfcntr=0, hrd=0, resync=0, no_ic=0, interlace=0, psf=0, extended_dmv=0, refdist=0, range_mapy=None,
                 range_mapuv=None)
        f.update(flags)
        self.f = f
        self.rnd = 0
        self.res_x8 = 0
        # Picture geometry and slices (the interlaced streams change these per picture; a progressive picture without slices keeps the defaults).
        self.cur_mbh = self.mbh     # macroblock rows of the picture being written (of one field in a field picture)
        self.slice_rows = ()        # rows (of the picture) at which a slice starts
        self.slice_top = 0          # first row of the current slice
        self.addr_base = 0          # added to a row to make SLICE_ADDR (the field's row count for the second field)
        self.units = None           # the start code units of the frame being written (interlaced streams), see Unit
        self.expect = []            # per frame: [(unit, header end, macroblock data end)] (bit positions), see --expect

    # ---- sequence level
    def struct_c(self):
        f = self.f
        b = Bits()
        b.put(self.profile, 2); b.put(0, 1); b.put(0, 1)
        b.put(self.rng.randint(0, 7), 3); b.put(self.rng.randint(0, 31), 5)
        b.put(f['loop_filter'], 1); b.put(0, 1); b.put(f['multires'], 1); b.put(1, 1)
        b.put(f['fastuvmc'], 1); b.put(f['extended_mv'], 1); b.put(f['dquant'], 2); b.put(f['vstransform'], 1); b.put(0, 1)
        b.put(f['overlap'], 1); b.put(f['resync'], 1); b.put(f['rangered'], 1); b.put(f['max_b_frames'], 3); b.put(f['quantizer'], 2); b.put(f['finterp'], 1)
        b.put(1, 1)
        return b.data()

    def adv_sequence(self):
        f = self.f
        b = Bits()
        b.put(3, 2); b.put(self.rng.randint(0, 4), 3); b.put(1, 2)
        b.put(self.rng.randint(0, 7), 3); b.put(self.rng.randint(0, 31), 5); b.put(f['postproc'], 1)
        b.put((self.w >> 1) - 1, 12); b.put((self.h >> 1) - 1, 12)
        b.put(f['broadcast'], 1); b.put(f['interlace'], 1); b.put(f['tfcntr'], 1); b.put(f['finterp'], 1); b.put(0, 1); b.put(f['psf'], 1)
        b.put(0, 1)   # display extended info
        b.put(f['hrd'], 1)
        if f['hrd']:
            b.put(2, 5); b.put(0, 4); b.put(0, 4)
            for _ in range(2):
                b.put(100, 16); b.put(200, 16)
        return b.data(stop_bit=True)

    def adv_entry_point(self, size_override=False):
        f = self.f
        b = Bits()
        b.put(0, 1); b.put(1, 1); b.put(0, 1); b.put(f['refdist'], 1)   # BROKEN_LINK, CLOSED_ENTRY, PANSCAN_FLAG, REFDIST_FLAG
        b.put(f['loop_filter'], 1); b.put(f['fastuvmc'], 1); b.put(f['extended_mv'], 1); b.put(f['dquant'], 2); b.put(f['vstransform'], 1); b.put(f['overlap'], 1)
        b.put(f['quantizer'], 2)
        if f['hrd']:
            for _ in range(2):
                b.put(255, 8)
        b.put(1 if size_override else 0, 1)
        if size_override:
            b.put((self.w >> 1) - 1, 12); b.put((self.h >> 1) - 1, 12)
        if f['extended_mv']:
            b.put(f['extended_dmv'], 1)
        for m in (f['range_mapy'], f['range_mapuv']):   # RANGE_MAPY_FLAG, RANGE_MAPY, RANGE_MAPUV_FLAG, RANGE_MAPUV
            b.put(0 if m is None else 1, 1)
            if m is not None:
                b.put(m, 3)
        return b.data(stop_bit=True)

    # ---- bit planes (8.7)
    def put_plane(self, b, plane, mode, invert, w=None, h=None):
        """Writes `plane` (rows of 0/1, mbh rows of mbw) with IMODE `mode` (0 raw ... 6 column skip) and the invert bit."""
        w = self.mbw if w is None else w
        h = self.cur_mbh if h is None else h
        b.put(invert, 1)
        b.code(T['kImodeCode'], T['kImodeBits'], mode)
        if mode == 0:
            return
        P = [row[:] for row in plane]
        if mode in (2, 4):   # differential: the coded bits are the differences
            D = [[0] * w for _ in range(h)]
            for y in range(h):
                for x in range(w):
                    if y == 0 and x == 0:
                        D[y][x] = P[y][x] ^ invert
                    elif y == 0:
                        D[y][x] = P[y][x] ^ P[y][x - 1]
                    elif x == 0:
                        D[y][x] = P[y][x] ^ P[y - 1][x]
                    elif P[y][x - 1] != P[y - 1][x]:
                        D[y][x] = P[y][x] ^ invert
                    else:
                        D[y][x] = P[y][x] ^ P[y][x - 1]
            P = D
        elif invert:
            P = [[v ^ 1 for v in row] for row in P]

        def rowskip(x0, ww, y0, hh):
            for y in range(y0, y0 + hh):
                row = P[y][x0:x0 + ww]
                if not any(row) and self.rng.random() < 0.8:
                    b.put(0, 1)
                else:
                    b.put(1, 1)
                    for v in row:
                        b.put(v, 1)

        def colskip(x0, ww, y0, hh):
            for x in range(x0, x0 + ww):
                col = [P[y][x] for y in range(y0, y0 + hh)]
                if not any(col) and self.rng.random() < 0.8:
                    b.put(0, 1)
                else:
                    b.put(1, 1)
                    for v in col:
                        b.put(v, 1)

        if mode in (1, 2):
            flat = [P[y][x] for y in range(h) for x in range(w)]
            i = 0
            if len(flat) & 1:
                b.put(flat[0], 1)
                i = 1
            while i < len(flat):
                sym = flat[i] | (flat[i + 1] << 1)
                b.code(T['kNorm2Code'], T['kNorm2Bits'], sym)
                i += 2
        elif mode in (3, 4):
            if h % 3 == 0 and w % 3 != 0:
                for y in range(0, h, 3):
                    for x in range(w & 1, w, 2):
                        sym = P[y][x] | (P[y][x + 1] << 1) | (P[y + 1][x] << 2) | (P[y + 1][x + 1] << 3) | (P[y + 2][x] << 4) | (P[y + 2][x + 1] << 5)
                        b.code(T['kNorm6Code'], T['kNorm6Bits'], sym)
                if w & 1:
                    colskip(0, 1, 0, h)
            else:
                for y in range(h & 1, h, 2):
                    for x in range(w % 3, w, 3):
                        sym = P[y][x] | (P[y][x + 1] << 1) | (P[y][x + 2] << 2) | (P[y + 1][x] << 3) | (P[y + 1][x + 1] << 4) | (P[y + 1][x + 2] << 5)
                        b.code(T['kNorm6Code'], T['kNorm6Bits'], sym)
                x0 = w % 3
                if x0:
                    colskip(0, x0, 0, h)
                if h & 1:
                    rowskip(x0, w - x0, 0, 1)
        elif mode == 5:
            rowskip(0, w, 0, h)
        elif mode == 6:
            colskip(0, w, 0, h)

    def random_plane(self, density):
        return [[1 if self.rng.random() < density else 0 for _ in range(self.mbw)] for _ in range(self.cur_mbh)]

    def plane_with_random_mode(self, b, density):
        """Writes a random plane in a random mode; returns (plane, raw)."""
        plane = self.random_plane(density)
        mode = self.rng.choice([0, 1, 2, 3, 4, 5, 6])
        self.count('imode%d' % mode)
        self.put_plane(b, plane, mode, self.rng.randint(0, 1))
        return plane, mode == 0

    # ---- picture headers
    PQUANT = [T['kPquantTable'][i * 32:(i + 1) * 32] for i in range(3)]

    def pick_quant(self, pq_choice=None):
        f = self.f
        qm = f['quantizer']
        pqindex = pq_choice if pq_choice is not None else self.rng.choice([1, 2, 3, 5, 8, 9, 10, 12, 14, 16, 20, 25])
        pq = self.PQUANT[0 if qm == 0 else 1][pqindex]
        halfpq = self.rng.randint(0, 1) if pqindex < 9 else 0
        if qm == 0:
            pquantizer = 1 if pqindex < 9 else 0
        elif qm == 2:
            pquantizer = 0
        elif qm == 1:
            pquantizer = self.rng.randint(0, 1)
        else:
            pquantizer = 1
        return pqindex, pq, halfpq, pquantizer

    def new_picture(self, ptype, **kw):
        """The parameters of a picture: everything the header says."""
        r = self.rng
        f = self.f
        adv = self.profile == 3
        p = dict(type=ptype, skipped=False)
        p['pqindex'], p['pq'], p['halfpq'], p['pquantizer'] = self.pick_quant(kw.get('pqindex'))
        p['mvrange'] = r.randint(0, 3) if f['extended_mv'] else 0
        p['k_x'] = p['mvrange'] + 9 + (p['mvrange'] >> 1)
        p['k_y'] = p['mvrange'] + 8
        p['range_x'] = 1 << (p['k_x'] - 1)
        p['range_y'] = 1 << (p['k_y'] - 1)
        p['tt_index'] = (p['pq'] > 4) + (p['pq'] > 12)
        p['interpfrm'] = r.randint(0, 1) if f['finterp'] else 0
        p['bfraction_index'] = kw.get('bfraction_index', r.choice([0, 1, 2, 3, 4, 5, 8, 10, 15, 20]))
        p['bfraction'] = T['kBfraction'][p['bfraction_index']]
        p['rnd'] = None
        # dquant
        p['dquantfrm'] = 0
        p['dqprofile'] = 0
        p['dqsbedge'] = 0
        p['dqbilevel'] = 0
        p['altpq'] = 0
        if f['dquant'] == 1:
            p['dquantfrm'] = r.randint(0, 1)
            if p['dquantfrm']:
                p['dqprofile'] = r.randint(0, 3)
                p['dqsbedge'] = r.randint(0, 3)
                p['dqbilevel'] = r.randint(0, 1)
                pqdiff = r.randint(0, 7)
                p['pqdiff'] = pqdiff
                p['altpq_explicit'] = r.randint(1, 31)
                p['altpq'] = p['altpq_explicit'] if pqdiff == 7 else p['pq'] + pqdiff + 1
                if p['altpq'] > 31:
                    p['pqdiff'] = 7
                    p['altpq_explicit'] = r.randint(1, 31)
                    p['altpq'] = p['altpq_explicit']
        if not adv and ptype in ('I', 'BI'):
            p['dquantfrm'] = 0   # (an I picture of Simple and Main profile has no DQUANT syntax)
        p['ttmbf'] = r.randint(0, 1) if f['vstransform'] else 1
        p['ttfrm_code'] = r.randint(0, 3)
        p['ttfrm'] = T['kTtfrmToTt'][p['ttfrm_code']] if (f['vstransform'] and p['ttmbf']) else (0 if f['vstransform'] else 0)
        p['ttmbf_eff'] = p['ttmbf']
        p['c_ac'] = r.randint(0, 2)
        p['y_ac'] = r.randint(0, 2)
        p['dc_table'] = r.randint(0, 1)
        p['mv_table'] = r.randint(0, 3)
        p['cbptab'] = r.randint(0, 3)
        p['condover'] = 0
        p.update(kw)
        return p

    def vop_dquant(self, b, p):
        f = self.f
        if f['dquant'] != 1:
            return
        self.count('dquantfrm%d dqprofile%d dqbilevel%d' % (p['dquantfrm'], p['dqprofile'], p['dqbilevel']) if p['dquantfrm'] else 'dquantfrm0')
        b.put(p['dquantfrm'], 1)
        if not p['dquantfrm']:
            return
        b.put(p['dqprofile'], 2)
        if p['dqprofile'] in (1, 2):
            b.put(p['dqsbedge'], 2)
        elif p['dqprofile'] == 3:
            b.put(p['dqbilevel'], 1)
            if not p['dqbilevel']:
                return
        b.put(p['pqdiff'], 3)
        if p['pqdiff'] == 7:
            b.put(p['altpq_explicit'], 5)

    def header(self, b, p):
        """Writes the picture header (not the macroblock data) and fills in the bit planes the picture has (p['planes'])."""
        f = self.f
        r = self.rng
        adv = self.profile == 3
        t = p['type']   # 'I' 'P' 'B' 'BI'
        mbw, mbh = self.mbw, self.mbh
        p['planes'] = {}
        if not adv:
            if f['finterp']:
                b.put(p['interpfrm'], 1)
            b.put(r.randint(0, 3), 2)
            if f['rangered']:
                p['rangeredfrm'] = p.get('rangeredfrm', r.randint(0, 1))   # RANGEREDFRM (no stream without range reduction gets here)
                b.put(p['rangeredfrm'], 1)
            if t == 'P':
                b.put(1, 1)
            else:
                b.put(0, 1)
                if f['max_b_frames']:
                    b.put(0 if t in ('B', 'BI') else 1, 1)
            if t in ('B', 'BI'):
                idx = 22 if t == 'BI' else (p['bfraction_index'] if p['bfraction_index'] not in (21, 22) else 1)
                p['bfraction_index'] = idx
                p['bfraction'] = T['kBfraction'][idx]
                if idx < 7:
                    b.put(idx, 3)
                else:
                    b.put(7, 3); b.put(idx - 7, 4)
            if t in ('I', 'BI'):
                b.put(r.randint(0, 127), 7)
                self.rnd = 1
            elif t == 'P':
                self.rnd ^= 1
            p['rnd'] = self.rnd
        else:
            u = {'P': 0, 'B': 1, 'I': 2, 'BI': 3}[t]
            if p['skipped']:
                u = 4
            if f['interlace']:
                b.put(0, 1)   # FCM: a progressive frame in an interlaced sequence
            # unary with stop bit 0, length at most 4
            for _ in range(u):
                b.put(1, 1)
            if u < 4:
                b.put(0, 1)
            if f['tfcntr']:
                b.put(r.randint(0, 255), 8)
            if f['broadcast']:
                b.put(r.randint(0, 3), 2)
            if p['skipped']:
                return
            p['rnd'] = r.randint(0, 1)
            b.put(p['rnd'], 1)
            if f['interlace']:
                b.put(r.randint(0, 1), 1)   # UVSAMP
            if f['finterp']:
                b.put(p['interpfrm'], 1)
            if t == 'BI':   # (the picture type already says it; no BFRACTION)
                p['bfraction_index'], p['bfraction'] = 22, 0
            if t == 'B':
                idx = 22 if t == 'BI' else (p['bfraction_index'] if p['bfraction_index'] not in (21, 22) else 1)
                p['bfraction_index'] = idx
                p['bfraction'] = T['kBfraction'][idx]
                if idx < 7:
                    b.put(idx, 3)
                else:
                    b.put(7, 3); b.put(idx - 7, 4)
        b.put(p['pqindex'], 5)
        if p['pqindex'] < 9:
            b.put(p['halfpq'], 1)
        if f['quantizer'] == 1:
            b.put(p['pquantizer'], 1)
        if adv:
            if f['postproc']:
                b.put(r.randint(0, 3), 2)
        else:
            if f['extended_mv']:
                for _ in range(p['mvrange']):
                    b.put(1, 1)
                if p['mvrange'] < 3:
                    b.put(0, 1)
            if f['multires'] and t != 'B':
                b.put(0, 2)
            if self.res_x8 and t in ('I', 'BI'):
                b.put(0, 1)
        pl = p['planes']
        if t == 'P':
            if adv:
                if f['extended_mv']:
                    for _ in range(p['mvrange']):
                        b.put(1, 1)
                    if p['mvrange'] < 3:
                        b.put(0, 1)
                if f['interlace'] and f['extended_dmv']:   # DMVRANGE (unused by progressive frames, but there)
                    dmv = r.randint(0, 3)
                    for _ in range(dmv):
                        b.put(1, 1)
                    if dmv < 3:
                        b.put(0, 1)
            lowquant = 0 if p['pq'] > 12 else 1
            mode_idx = p['mvmode_idx'] = r.randint(0, 3 if f.get('no_ic') else 4)
            mvmode = T['kMvPmodeTable'][lowquant * 5 + mode_idx]
            # unary with stop bit 1, at most 4 zeros
            for _ in range(mode_idx):
                b.put(0, 1)
            if mode_idx < 4:
                b.put(1, 1)
            p['mv_mode'] = mvmode
            p['use_ic'] = 0
            mv2 = None
            if mvmode == 4:
                m2 = p['mvmode2_idx'] = r.randint(0, 3)
                mv2 = T['kMvPmodeTable2'][lowquant * 4 + m2]
                for _ in range(m2):
                    b.put(0, 1)
                if m2 < 3:
                    b.put(1, 1)
                b.put(r.randint(0, 63), 6); b.put(r.randint(0, 63), 6)
                p['use_ic'] = 1
            eff = mv2 if mvmode == 4 else mvmode
            p['mv_mode_eff'] = eff
            p['quarter'] = eff not in (2, 0)
            p['mixed'] = eff == 3
            if p['mixed']:
                pl['mvtype'] = self.plane_with_random_mode(b, 0.4)
            else:
                pl['mvtype'] = ([[0] * mbw for _ in range(mbh)], False)
            pl['skip'] = self.plane_with_random_mode(b, float(os.environ.get('VC1_PSKIP', 0.3)))
            b.put(p['mv_table'], 2)
            b.put(p['cbptab'], 2)
            self.vop_dquant(b, p)
            self.transform_info(b, p)
        elif t == 'B':
            if adv:
                if f['extended_mv']:
                    for _ in range(p['mvrange']):
                        b.put(1, 1)
                    if p['mvrange'] < 3:
                        b.put(0, 1)
            p['mv_mode_eff'] = 1
            if r.randint(0, 1):
                b.put(1, 1)
                p['quarter'] = True
            else:
                b.put(0, 1)
                p['quarter'] = False
            pl['direct'] = self.plane_with_random_mode(b, p.get('direct_density', float(os.environ.get('VC1_BDIRECT', 0.3))))
            pl['skip'] = self.plane_with_random_mode(b, 0.3)
            b.put(p['mv_table'], 2)
            b.put(p['cbptab'], 2)
            self.vop_dquant(b, p)
            self.transform_info(b, p)
        elif adv:   # I and BI
            pl['acpred'] = self.plane_with_random_mode(b, 0.5)
            p['condover'] = 0
            if f['overlap'] and p['pq'] <= 8:
                p['condover'] = r.randint(0, 2)
                b.put(0, 1) if p['condover'] == 0 else (b.put(1, 1), b.put(p['condover'] - 1, 1))
                if p['condover'] == 2:
                    pl['overflags'] = self.plane_with_random_mode(b, 0.5)
        # AC syntax
        self.put012(b, p['c_ac'])
        if t in ('I', 'BI'):
            self.put012(b, p['y_ac'])
        b.put(p['dc_table'], 1)
        if adv and t in ('I', 'BI'):
            self.vop_dquant(b, p)

    @staticmethod
    def put012(b, v):
        if v == 0:
            b.put(0, 1)
        else:
            b.put(1, 1); b.put(v - 1, 1)

    def transform_info(self, b, p):
        f = self.f
        if f['vstransform']:
            self.count('ttmbf%d' % p['ttmbf'])
            b.put(p['ttmbf'], 1)
            if p['ttmbf']:
                b.put(p['ttfrm_code'], 2)
        # (without vstransform: ttmbf = 1, ttfrm = 8x8)
        if not f['vstransform']:
            p['ttmbf'] = 1
            p['ttfrm'] = 0

    # ---- coefficients
    def coding_sets(self, p):
        intra_picture = p['type'] in ('I', 'BI')
        ac = p['y_ac'] if intra_picture else p['c_ac']
        cs_intra = {0: 6 if p['pqindex'] <= 8 else 2, 1: 0, 2: 4}[ac]
        cs_inter = {0: 7 if p['pqindex'] <= 8 else 3, 1: 1, 2: 5}[p['c_ac']]
        return cs_intra, cs_inter

    def write_dc(self, b, p, n, quant):
        r = self.rng
        m = 3 - quant if quant in (1, 2) else 0
        base = (p['dc_table'] * 2 + (1 if n >= 4 else 0)) * 120
        codes, bits = T['kDcCode'], T['kDcBits']
        roll = r.random()
        if roll < 0.25:
            sym = 0
        elif roll < 0.95:
            sym = r.randint(1, 6)
        else:
            sym = 119
        b.put(codes[base + sym], bits[base + sym])
        if sym:
            if sym == 119:
                b.put(r.randint(0, 40), 8 + m)
            elif m:
                b.put(r.randint(0, (1 << m) - 1), m)
            b.put(r.randint(0, 1), 1)

    def write_pair(self, b, p, cs, run, level, sign, last, st):
        r = self.rng
        base = cs * 185
        code_base = cs * 186
        codes, bits = T['kAcCode'], T['kAcBits']
        key = (run, level, bool(last))
        options = []
        idx = AC_BY_KEY[cs].get(key)
        if idx is not None:
            options += ['direct'] * 6
        dl = (T['kAcLastDeltaLevel'][cs * 44 + run] if last else T['kAcDeltaLevel'][cs * 31 + run]) if run < (44 if last else 31) else 255
        if dl < 128:
            idx1 = AC_BY_KEY[cs].get((run, level - dl, bool(last)))
            if idx1 is not None and level - dl >= 1:
                options.append('esc1')
        if level < (10 if last else 57):
            dr = (T['kAcLastDeltaRun'][cs * 10 + level] if last else T['kAcDeltaRun'][cs * 57 + level])
            if dr < 128:
                idx2 = AC_BY_KEY[cs].get((run - dr - 1, level, bool(last)))
                if idx2 is not None and run - dr - 1 >= 0:
                    options.append('esc2')
        options.append('esc3')
        choice = r.choice(options)
        escape_index = AC_SIZE[cs] - 1
        if choice == 'direct':
            b.put(codes[code_base + idx], bits[code_base + idx])
            b.put(sign, 1)
            return
        b.put(codes[code_base + escape_index], bits[code_base + escape_index])
        if choice == 'esc1':
            b.put(1, 1)
            b.put(codes[code_base + idx1], bits[code_base + idx1])
            b.put(sign, 1)
        elif choice == 'esc2':
            b.put(0, 1); b.put(1, 1)
            b.put(codes[code_base + idx2], bits[code_base + idx2])
            b.put(sign, 1)
        else:
            b.put(0, 1); b.put(0, 1)
            b.put(1 if last else 0, 1)
            if st['esc3'] is None:
                if p['pq'] < 8 or p['dquantfrm']:
                    L = r.randint(8, 11)
                    b.put(0, 3); b.put(L - 8, 2)
                else:
                    L = 8
                    b.put(0, 6)   # six zeros: unary length 6, no stop bit
                rl_code = 3
                b.put(rl_code, 2)
                st['esc3'] = (L, 3 + rl_code)
            L, RL = st['esc3']
            b.put(run, RL)
            b.put(sign, 1)
            b.put(level, L)

    def write_block_pairs(self, b, p, cs, first_pos, max_pos, st):
        """The run/level pairs of one block (or sub-block): positions first_pos .. max_pos."""
        r = self.rng
        n = r.randint(1, 4)
        pos = first_pos
        pairs = []
        for k in range(n):
            if pos > max_pos:
                break
            run = r.randint(0, min(4, max_pos - pos))
            level = r.choice([1, 1, 1, 2, 2, 3, 4])
            pairs.append((run, level, r.randint(0, 1)))
            pos += run + 1
        if not pairs:
            pairs = [(0, 1, 0)]
        for k, (run, level, sign) in enumerate(pairs):
            self.write_pair(b, p, cs, run, level, sign, k == len(pairs) - 1, st)

    def write_intra_block(self, b, p, n, coded, cs, quant, st):
        self.write_dc(b, p, n, quant)
        if coded:
            self.write_block_pairs(b, p, cs, 1, 63, st)

    def mquant_bits(self, b, p, mbx, mby):
        """GET_MQUANT: writes the bits and returns the macroblock's quantizer (negative: not the picture's)."""
        r = self.rng
        mquant = p['pq']
        if not p['dquantfrm']:
            return mquant
        edges = 0
        if p['dqprofile'] == 3:
            if p['dqbilevel']:
                v = r.randint(0, 1)
                b.put(v, 1)
                mquant = -p['altpq'] if v else p['pq']
            else:
                mqdiff = r.choice([0, 1, 2, 3, 7])
                b.put(mqdiff, 3)
                if mqdiff != 7:
                    mquant = -p['pq'] - mqdiff
                else:
                    v = r.randint(1, 31)
                    b.put(v, 5)
                    mquant = -v
        if p['dqprofile'] == 2:
            edges = 1 << p['dqsbedge']
        elif p['dqprofile'] == 1:
            edges = (3 << p['dqsbedge']) % 15
        elif p['dqprofile'] == 0:
            edges = 15
        if (edges & 1) and mbx == 0:
            mquant = -p['altpq']
        if (edges & 2) and mby == 0:
            mquant = -p['altpq']
        if (edges & 4) and mbx == self.mbw - 1:
            mquant = -p['altpq']
        if (edges & 8) and mby == self.cur_mbh - 1:
            mquant = -p['altpq']
        if mquant == 0 or mquant > 31 or mquant < -31:
            mquant = 1
        return mquant

    def count(self, key):
        """Counts a syntax element value for a coverage summary (IEnc.stats; no effect on the stream)."""
        stats = getattr(self, 'stats', None)
        if stats is not None:
            stats[key] = stats.get(key, 0) + 1

    # ---- slices
    def slice_hook(self, b, mby, st, coded=None):
        """Called at the start of every macroblock row: at the first row of a slice (other than the picture's first) ends the current start code unit
        and starts a slice unit (SLICE_ADDR, PIC_HEADER_FLAG 0). A slice starts the escape mode 3 code lengths again, and its first row has no
        neighbours above (the decoder's first_slice_line: motion vector, coded block and AC prediction)."""
        if mby == 0 or mby not in self.slice_rows:
            if self.units is not None:
                self.units[-1].rows += 1
            return b
        self.close_unit()
        u = Unit(0x0B)
        u.b.put(self.addr_base + mby, 9)   # SLICE_ADDR
        u.b.put(0, 1)                      # PIC_HEADER_FLAG
        u.hdr_end = u.b.tell()
        u.rows = 1
        self.units.append(u)
        st['esc3'] = None
        self.slice_top = mby
        self.count('slice')
        if coded is not None:   # (the decoder clears the coded block flags of the row above)
            for x in range(len(coded[0])):
                coded[2 * mby - 1][x] = 0
        return u.b

    def close_unit(self):
        u = self.units[-1]
        if u.mb_end is None:
            u.mb_end = u.b.tell()

    # ---- picture bodies
    def body_intra_picture(self, b, p):
        """The macroblock layer of an I or BI picture."""
        r = self.rng
        adv = self.profile == 3
        mbw, mbh = self.mbw, self.mbh
        st = {'esc3': None}
        cs_intra, cs_inter = self.coding_sets(p)
        coded = [[0] * (2 * mbw) for _ in range(2 * mbh)]
        pl = p['planes']
        fieldtx_plane, fieldtx_raw = pl.get('fieldtx', (None, False))
        for mby in range(self.cur_mbh):
            b = self.slice_hook(b, mby, st, coded)
            for mbx in range(mbw):
                if fieldtx_raw:   # FIELDTX (interlaced frame I pictures)
                    b.put(fieldtx_plane[mby][mbx], 1)
                flags = [r.randint(0, 1) for _ in range(6)]
                diffs = []
                for k in range(4):
                    x, y = 2 * mbx + (k & 1), 2 * mby + (k >> 1)
                    a = coded[y][x - 1] if x > 0 else 0
                    bb = coded[y - 1][x - 1] if x > 0 and y > 0 else 0
                    c = coded[y - 1][x] if y > 0 else 0
                    pred = a if bb == c else c
                    diffs.append(flags[k] ^ pred)
                    coded[y][x] = flags[k]
                diffs += flags[4:]
                sym = sum(d << (5 - k) for k, d in enumerate(diffs))
                b.code(T['kIntraCbpcyCode'], T['kIntraCbpcyBits'], sym)
                if not adv:
                    b.put(r.randint(0, 1), 1)
                    quant = p['pq']
                else:
                    plane, raw = pl['acpred']
                    if raw:
                        b.put(plane[mby][mbx], 1)
                    if p['condover'] == 2:
                        oplane, oraw = pl['overflags']
                        if oraw:
                            b.put(oplane[mby][mbx], 1)
                    mq = self.mquant_bits(b, p, mbx, mby)
                    quant = abs(mq)
                for k in range(6):
                    self.write_intra_block(b, p, k, flags[k], cs_intra if k < 4 else cs_inter, quant, st)

    # ---- motion vector data
    OFFSET = [0, 1, 3, 7, 15, 31, 63, 127, 255]
    SIZE = [0, 2, 3, 4, 5, 8]

    def write_mvdata(self, b, p, intra_ok, coeffs):
        """Writes MVDATA; returns (dmv_x, dmv_y, has_coeffs, intra)."""
        r = self.rng
        qs = 1 if p['quarter'] else 0
        roll = r.random()
        if intra_ok and roll < 0.12:
            base = 36
        elif roll < 0.22:
            base = 35
        else:
            base = r.randint(0, 2) * 6 + r.randint(0, 2)
            if base == 0 and not coeffs:
                base = 1
        sym = (36 + base) if coeffs else (base - 1)
        b.put(T['kMvDiffCode'][p['mv_table'] * 73 + sym], T['kMvDiffBits'][p['mv_table'] * 73 + sym])
        dx = dy = 0
        intra = False
        if base == 0:
            pass
        elif base == 35:
            nx, ny = p['k_x'] - 1 + qs, p['k_y'] - 1 + qs
            dx, dy = r.randint(0, 15), r.randint(0, 15)
            b.put(dx, nx); b.put(dy, ny)
        elif base == 36:
            intra = True
        else:
            vals = []
            for index1 in (base % 6, base // 6):
                off = self.OFFSET[index1]
                size = self.SIZE[index1] - (1 if (not qs and index1 == 5) else 0)
                d = off
                if size > 0:
                    raw = r.randint(0, (1 << size) - 1)
                    b.put(raw, size)
                    sign = -(raw & 1)
                    d = (sign ^ ((raw >> 1) + off)) - sign
                vals.append(d)
            dx, dy = vals
        return dx, dy, bool(coeffs), intra

    def mid3(self, a, b, c):
        return sorted((a, b, c))[1]

    def pred_mv(self, b, p, mbx, mby, n, dmv_x, dmv_y, mv1, intra):
        """Mirrors the decoder's motion vector prediction; writes the hybrid prediction bit when there is one."""
        mbw, mbh = self.mbw, self.mbh
        if not p['quarter']:
            dmv_x *= 2
            dmv_y *= 2
        bx, by = 2 * mbx + (n & 1), 2 * mby + (n >> 1)
        mv = self.mvs
        if intra:
            mv[(bx, by)] = (0, 0)
            if mv1:
                for k in range(1, 4):
                    mv[(2 * mbx + (k & 1), 2 * mby + (k >> 1))] = (0, 0)
            return
        first = mby == self.slice_top
        a_valid = (not first) or n in (2, 3)
        b_valid = a_valid
        c_valid = mbx > 0 or n in (1, 3)
        if mv1:
            off = -1 if mbx == mbw - 1 else 2
            b_valid = b_valid and mbw > 1
        else:
            off = {0: (-1 if mbx else 1), 1: (-1 if mbx == mbw - 1 else 1), 2: 1, 3: -1}[n]
        A = mv.get((bx, by - 1), (0, 0)) if a_valid else (0, 0)
        B = mv.get((bx + off, by - 1), (0, 0)) if b_valid else (0, 0)
        C = mv.get((bx - 1, by), (0, 0)) if c_valid else (0, 0)
        valid = int(a_valid) + int(b_valid) + int(c_valid)
        if a_valid:
            px, py = A
        elif c_valid:
            px, py = C
        elif b_valid:
            px, py = B
        else:
            px = py = 0
        if valid > 1:
            px, py = self.mid3(A[0], B[0], C[0]), self.mid3(A[1], B[1], C[1])
        lim = -60 if mv1 else -28
        qx = (mbx << 6) + (32 if n in (1, 3) else 0)
        qy = (mby << 6) + (32 if n in (2, 3) else 0)
        X, Y = (mbw << 6) - 4, (mbh << 6) - 4
        if qx + px < lim: px = lim - qx
        if qy + py < lim: py = lim - qy
        if qx + px > X: px = X - qx
        if qy + py > Y: py = Y - qy

        def intra_at(x, y):
            return (self.intra_mask[(x >> 1, y >> 1)] >> (((y & 1) << 1) | (x & 1))) & 1

        if a_valid and c_valid:
            def hybrid():
                bit = self.rng.randint(0, 1)
                b.put(bit, 1)
                return A if bit else C
            s1 = abs(px) + abs(py) if intra_at(bx, by - 1) else abs(px - A[0]) + abs(py - A[1])
            if s1 > 32:
                px, py = hybrid()
            else:
                s2 = abs(px) + abs(py) if intra_at(bx - 1, by) else abs(px - C[0]) + abs(py - C[1])
                if s2 > 32:
                    px, py = hybrid()
        rx, ry = p['range_x'], p['range_y']
        mx = ((px + dmv_x + rx) & ((rx << 1) - 1)) - rx
        my = ((py + dmv_y + ry) & ((ry << 1) - 1)) - ry
        mv[(bx, by)] = (mx, my)
        if mv1:
            for k in range(1, 4):
                mv[(2 * mbx + (k & 1), 2 * mby + (k >> 1))] = (mx, my)

    # ---- inter blocks
    def write_inter_block(self, b, p, ttmb, first_block, cs, quant, st):
        """The residual of an inter block; returns (new ttmb, first_block)."""
        r = self.rng
        f = self.f
        ttblk = ttmb & 7 if ttmb != -1 else 0
        subblkpat = 0
        if ttmb == -1:
            sym = r.randint(0, 7)
            b.code(T['kTtblkCode'][p['tt_index'] * 8:], T['kTtblkBits'][p['tt_index'] * 8:], sym)
            ttblk = T['kTtblkToTt'][p['tt_index'] * 8 + sym]
        if ttblk == 7:
            q = r.randint(0, 14)
            b.code(T['kSubblkpatCode'][p['tt_index'] * 15:], T['kSubblkpatBits'][p['tt_index'] * 15:], q)
            subblkpat = ~(q + 1)
        if ttblk not in (0, 7) and ((p['ttmbf'] or (ttmb != -1 and (ttmb & 8) and not first_block))):
            v = r.randint(0, 2)
            self.put012(b, v)
            subblkpat = (v ^ 3) if v else 0
            if ttblk in (1, 2):
                ttblk = 3
            if ttblk in (4, 5):
                ttblk = 6
        if ttblk in (1, 2):
            subblkpat = 2 - (1 if ttblk == 2 else 0)
            ttblk = 3
        if ttblk in (4, 5):
            subblkpat = 2 - (1 if ttblk == 5 else 0)
            ttblk = 6
        if ttblk == 0:
            self.write_block_pairs(b, p, cs, 0, 63, st)
        elif ttblk == 7:
            for j in range(4):
                if not (subblkpat & (1 << (3 - j))):
                    self.write_block_pairs(b, p, cs, 0, 15, st)
        elif ttblk == 3:
            for j in range(2):
                if not (subblkpat & (1 << (1 - j))):
                    self.write_block_pairs(b, p, cs, 0, 31, st)
        else:
            for j in range(2):
                if not (subblkpat & (1 << (1 - j))):
                    self.write_block_pairs(b, p, cs, 0, 31, st)
        if not p['ttmbf'] and ttmb < 8:
            ttmb = -1
        return ttmb, False

    def write_ttmb(self, b, p):
        sym = self.rng.randint(0, 15)
        b.put(T['kTtmbCode'][p['tt_index'] * 16 + sym], T['kTtmbBits'][p['tt_index'] * 16 + sym])
        return sym

    # ---- P and B pictures
    def body_inter_picture(self, b, p):
        r = self.rng
        mbw, mbh = self.mbw, self.mbh
        st = {'esc3': None}
        cs_intra, cs_inter = self.coding_sets(p)
        self.mvs = {}
        self.intra_mask = {(x, y): 0 for x in range(mbw) for y in range(mbh)}
        pl = p['planes']
        skip_plane, skip_raw = pl['skip']
        for mby in range(self.cur_mbh):
            b = self.slice_hook(b, mby, st)
            for mbx in range(mbw):
                if p['type'] == 'P':
                    self.p_macroblock(b, p, mbx, mby, pl, skip_plane[mby][mbx], skip_raw, st, cs_intra, cs_inter)
                else:
                    self.b_macroblock(b, p, mbx, mby, pl, skip_plane[mby][mbx], skip_raw, st, cs_intra, cs_inter)

    def intra_flag(self, mbx, mby, bx, by):
        """Whether the luma block at block position (bx, by) is intra, 0 outside the picture."""
        if bx < 0 or by < 0 or (bx >> 1) >= self.mbw or (by >> 1) >= self.mbh:
            return 0
        return (self.intra_mask[(bx >> 1, by >> 1)] >> (((by & 1) << 1) | (bx & 1))) & 1

    def p_macroblock(self, b, p, mbx, mby, pl, skipped, skip_raw, st, cs_intra, cs_inter):
        r = self.rng
        mvtype_plane, mvtype_raw = pl['mvtype']
        fourmv = mvtype_plane[mby][mbx]
        if mvtype_raw:
            b.put(fourmv, 1)
        if skip_raw:
            b.put(skipped, 1)
        ttmb = p['ttfrm'] if p['ttmbf'] else 0
        ttmb = p['ttfrm']
        first_block = True
        self.intra_mask[(mbx, mby)] = 0
        if not fourmv:
            if skipped:
                self.pred_mv(b, p, mbx, mby, 0, 0, 0, True, False)
                return
            coeffs = r.randint(0, 1)
            dx, dy, has_coeffs, intra = self.write_mvdata(b, p, True, coeffs)
            self.pred_mv(b, p, mbx, mby, 0, dx, dy, True, intra)
            quant = p['pq']
            cbp = 0
            if intra and not has_coeffs:
                mq = self.mquant_bits(b, p, mbx, mby)
                quant = abs(mq)
                b.put(r.randint(0, 1), 1)
            elif has_coeffs:
                if intra:
                    b.put(r.randint(0, 1), 1)
                cbp = r.randint(0, 63)
                b.put(T['kCbpcyCode'][p['cbptab'] * 64 + cbp], T['kCbpcyBits'][p['cbptab'] * 64 + cbp])
                mq = self.mquant_bits(b, p, mbx, mby)
                quant = abs(mq)
            if not p['ttmbf'] and not intra and has_coeffs:
                ttmb = self.write_ttmb(b, p)
            if intra:
                self.intra_mask[(mbx, mby)] = 0x3F
            for i in range(6):
                val = (cbp >> (5 - i)) & 1
                if intra:
                    self.write_intra_block(b, p, i, val, cs_intra if i < 4 else cs_inter, quant, st)
                elif val:
                    ttmb, first_block = self.write_inter_block(b, p, ttmb, first_block, cs_inter, quant, st)
            return
        # 4MV
        if skipped:
            for i in range(4):
                self.pred_mv(b, p, mbx, mby, i, 0, 0, False, False)
            return
        cbp = r.randint(0, 63)
        b.put(T['kCbpcyCode'][p['cbptab'] * 64 + cbp], T['kCbpcyBits'][p['cbptab'] * 64 + cbp])
        is_intra = [0] * 6
        is_coded = [0] * 6
        intra_count = 0
        coded_inter = False
        for i in range(6):
            val = (cbp >> (5 - i)) & 1
            if i < 4:
                intra = False
                coded = 0
                dx = dy = 0
                if val:
                    dx, dy, coded, intra = self.write_mvdata(b, p, True, r.randint(0, 1))
                self.pred_mv(b, p, mbx, mby, i, dx, dy, False, intra)
                intra_count += 1 if intra else 0
                is_intra[i] = 1 if intra else 0
                is_coded[i] = 1 if coded else 0
            else:
                is_intra[i] = 1 if intra_count >= 3 else 0
                is_coded[i] = val
            if is_intra[i]:
                self.intra_mask[(mbx, mby)] |= 1 << i
            if not coded_inter:
                coded_inter = (not is_intra[i]) and bool(is_coded[i])
        if not intra_count and not coded_inter:
            return
        mq = self.mquant_bits(b, p, mbx, mby)
        quant = abs(mq)
        intrapred = False
        for i in range(6):
            if not is_intra[i]:
                continue
            if i < 4:
                bx, by = 2 * mbx + (i & 1), 2 * mby + (i >> 1)
                a_av = self.intra_flag(mbx, mby, bx, by - 1) if (i in (2, 3) or mby > self.slice_top) else 0
                c_av = self.intra_flag(mbx, mby, bx - 1, by) if (i in (1, 3) or mbx > 0) else 0
            else:
                a_av = ((self.intra_mask[(mbx, mby - 1)] >> i) & 1) if mby > self.slice_top else 0
                c_av = ((self.intra_mask[(mbx - 1, mby)] >> i) & 1) if mbx > 0 else 0
            if a_av or c_av:
                intrapred = True
                break
        if intrapred:
            b.put(r.randint(0, 1), 1)
        if not p['ttmbf'] and coded_inter:
            ttmb = self.write_ttmb(b, p)
        for i in range(6):
            if is_intra[i]:
                self.write_intra_block(b, p, i, is_coded[i], cs_intra if i < 4 else cs_inter, quant, st)
            elif is_coded[i]:
                ttmb, first_block = self.write_inter_block(b, p, ttmb, first_block, cs_inter, quant, st)

    def b_macroblock(self, b, p, mbx, mby, pl, skipped, skip_raw, st, cs_intra, cs_inter):
        r = self.rng
        direct_plane, direct_raw = pl['direct']
        direct = direct_plane[mby][mbx]
        if direct_raw:
            b.put(direct, 1)
        if skip_raw:
            b.put(skipped, 1)
        ttmb = p['ttfrm']
        first_block = True
        intra = False
        has_coeffs = False
        bmvtype = 0
        if not direct:
            if not skipped:
                coeffs = r.randint(0, 1)
                dx, dy, has_coeffs, intra = self.write_mvdata(b, p, True, coeffs)
            if skipped or not intra:
                bmvtype = int(os.environ['VC1_BTYPE']) if 'VC1_BTYPE' in os.environ else r.randint(0, 2)
                self.put012(b, bmvtype)
        if skipped:
            return
        cbp = 0
        quant = p['pq']
        if direct:
            cbp = r.randint(0, 63)
            b.put(T['kCbpcyCode'][p['cbptab'] * 64 + cbp], T['kCbpcyBits'][p['cbptab'] * 64 + cbp])
            quant = abs(self.mquant_bits(b, p, mbx, mby))
            if not p['ttmbf']:
                ttmb = self.write_ttmb(b, p)
        else:
            if not has_coeffs and not intra:
                return
            if intra and not has_coeffs:
                quant = abs(self.mquant_bits(b, p, mbx, mby))
                b.put(r.randint(0, 1), 1)
            else:
                if bmvtype == 2:
                    dx, dy, has2, _ = self.write_mvdata(b, p, False, r.randint(0, 1))
                    if not has2:
                        return
                    has_coeffs = True
                if intra:
                    b.put(r.randint(0, 1), 1)
                cbp = r.randint(0, 63)
                b.put(T['kCbpcyCode'][p['cbptab'] * 64 + cbp], T['kCbpcyBits'][p['cbptab'] * 64 + cbp])
                quant = abs(self.mquant_bits(b, p, mbx, mby))
                if not p['ttmbf'] and not intra and has_coeffs:
                    ttmb = self.write_ttmb(b, p)
        for i in range(6):
            val = (cbp >> (5 - i)) & 1
            if intra:
                self.write_intra_block(b, p, i, val, cs_intra if i < 4 else cs_inter, quant, st)
            elif val:
                ttmb, first_block = self.write_inter_block(b, p, ttmb, first_block, cs_inter, quant, st)

    # ---- streams
    def frame_bytes(self, p):
        b = Bits()
        self.header(b, p)
        hdr_end = b.tell()
        if os.environ.get('VC1_DEBUG'):
            sys.stderr.write('picture %s\n' % {k: v for k, v in p.items() if k not in ('planes',)})
        if p['skipped']:
            pass
        elif p['type'] in ('I', 'BI'):
            self.body_intra_picture(b, p)
        else:
            self.body_inter_picture(b, p)
        self.expect.append([('frame', hdr_end, b.tell())])
        if b.tell() % 8 == 0:   # (ffmpeg wants a spare bit after the last macroblock when a bit plane is raw)
            b.put(0, 1)
        return b.data()


# ======================================================================================================================================
# Interlaced pictures (Advanced profile): field pictures (FCM 11), frame pictures (FCM 10) and progressive pictures (FCM 0) in one interlaced
# sequence, with slices. The syntax is the one FFmpeg's decoder reads (libavcodec/vc1.c ff_vc1_parse_frame_header_adv, vc1_block.c
# vc1_decode_p_mb_intfr / _intfi, vc1_decode_b_mb_intfi / _intfr, vc1_pred.c ff_vc1_pred_mv).

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc1_interlaced_tables as IT   # noqa: E402

OFFSET_TABLE = [[0, 1, 2, 4, 8, 16, 32, 64, 128], [0, 1, 3, 7, 15, 31, 63, 127, 255]]   # (index 1: DMVRANGE extends the component)


def check_vlc(name, codes, bits):
    """Every code fits its length and none is the prefix of another."""
    words = sorted(format(c, '0%db' % n) for c, n in zip(codes, bits))
    assert all(c < (1 << n) for c, n in zip(codes, bits)), name
    for a, b in zip(words, words[1:]):
        assert not b.startswith(a), (name, a, b)


for _name in ('FOURMVBP', 'TWOMVBP', 'INTFR_4MV_MBMODE', 'INTFR_NON4MV_MBMODE', 'IF_MMV_MBMODE', 'IF_1MV_MBMODE', 'MVDATA_1REF', 'MVDATA_2REF', 'ICBPCY'):
    for _k, (_c, _b) in enumerate(zip(getattr(IT, _name + '_CODES'), getattr(IT, _name + '_BITS'))):
        check_vlc('%s[%d]' % (_name, _k), _c, _b)


class Unit:
    """One start code unit of a frame: the frame (0x0D), the second field (0x0C) or a slice (0x0B); where its header and its macroblock data end
    (bit positions in the unescaped payload) and how many macroblock rows it has."""
    NAMES = {0x0D: 'frame', 0x0C: 'field', 0x0B: 'slice'}

    def __init__(self, code):
        self.code = code
        self.b = Bits()
        self.hdr_end = None
        self.mb_end = None
        self.rows = 0


def clip(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


class IEnc(Enc):
    """An Advanced profile stream in an interlaced sequence. Besides what Enc mirrors it mirrors the motion vector prediction of P field pictures
    (which decides whether a hybrid prediction bit is there: the predictors of the same and the opposite field, scaled by the reference distance),
    the slice boundaries (the first row of a slice has no neighbours above) and the frame coding mode of the last anchor (direct mode needs an
    anchor of the same kind in FFmpeg).

    What FFmpeg does, which the streams follow (and a decoder that matches FFmpeg must do too):
    - the second field's header (behind the field start code 0x0C) starts at PQINDEX; FPTYPE, TFF, RNDCTRL, REFDIST and BFRACTION are the frame's.
    - REFDIST is read only in I/I I/P P/I P/P frames (and is 0 without REFDIST_FLAG); B fields use the value of the last field pair that had it.
    - BFRACTION of an interlaced frame B picture comes after PQINDEX .. POSTPROC; its value must not be 0 (index 21 and 22 are refused).
    - SLICE_ADDR counts the rows of the second field on from the first field's: a slice of the second field at its row r has the address
      (field rows) + r. A field has (height / 2 + 15) / 16 macroblock rows (FFmpeg: FFALIGN(frame rows, 2) / 2).
    - motion vector prediction of field pictures: no pull back; predictors of intra blocks are not available; for a 1MV macroblock of a mixed-MV
      field picture predictor B is 2 blocks right (left in the last column) of the block above (1 left in other field pictures); hybrid prediction
      only in P fields, compared after the same/opposite field scaling; no hybrid prediction in interlaced frame pictures.
    - direct mode in a B field needs a field anchor ("Mixed field/frame direct mode not supported"); an interlaced frame B picture with a field
      anchor warns; so the generator uses direct mode only when the backward anchor is of the picture's own kind (also for progressive B).
    - every macroblock needs at least 2 bits left in the unit when it starts, else FFmpeg stops the slice without a message: each unit has at
      least one bit after its last macroblock."""

    FPTYPE = {('I', 'I'): 0, ('I', 'P'): 1, ('P', 'I'): 2, ('P', 'P'): 3, ('B', 'B'): 4, ('B', 'BI'): 5, ('BI', 'B'): 6, ('BI', 'BI'): 7}

    def __init__(self, seed, width, height, **flags):
        flags.setdefault('interlace', 1)
        self.slices = flags.pop('slices', 0)          # probability that a picture is cut into slices
        self.ic_prob = flags.pop('ic_prob', 0)        # probability that a P field picture uses intensity compensation (else 1 in 5)
        self.garbage = None
        super().__init__(seed, 3, width, height, **flags)
        if os.environ.get('VC1_GARBAGE'):            # a variant whose bits after the macroblock data are random (the pictures must not change)
            self.garbage = random.Random(seed * 7919 + 1)
        self.refdist = 0
        self.anchor_fcm = None   # frame coding mode of the last anchor: 'p' progressive, 'r' interlaced frame, 'f' field pair
        self.anchors = 0
        self.expect = []         # per frame: [(unit, header end, macroblock data end)], for the check against an instrumented decoder
        self.stats = {}          # how often each syntax element value was written (coverage)

    # ---- units, slices, small syntax helpers
    def begin_frame(self):
        self.units = [Unit(0x0D)]
        self.slice_top = 0
        self.slice_rows = ()
        self.addr_base = 0
        return self.units[0].b

    def mark_header(self, b):
        self.units[-1].hdr_end = b.tell()

    def pick_slices(self, rows):
        if not self.slices or rows < 2 or self.rng.random() >= self.slices:
            return ()
        k = self.rng.randint(1, min(3, rows - 1))
        return tuple(sorted(self.rng.sample(range(1, rows), k)))

    def finish_frame(self, rows_expected):
        self.close_unit()
        g = self.garbage
        assert sum(u.rows for u in self.units) == rows_expected, ([u.rows for u in self.units], rows_expected)
        out, exp = [], []
        for u in self.units:
            if u.code == 0x0B:
                assert u.rows >= 1
            bits = list(u.b.bits)
            bits.append(g.randint(0, 1) if g else 0)   # (at least one bit after the last macroblock: FFmpeg stops at a macroblock with <= 1 bit left)
            while len(bits) % 8:
                bits.append(g.randint(0, 1) if g else 0)
            data = bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))
            if g:
                data += bytes(g.randint(1, 255) for _ in range(g.randint(1, 3)))
            out.append((u.code, data))
            exp.append((Unit.NAMES[u.code], u.hdr_end, u.mb_end))
        self.expect.append(exp)
        self.units = None
        return out

    @staticmethod
    def put_unary_ones(b, v, limit):
        """get_unary(gb, 0, limit): v ones, then a zero unless v == limit."""
        for _ in range(v):
            b.put(1, 1)
        if v < limit:
            b.put(0, 1)

    @staticmethod
    def put_unary_zeros(b, v, limit):
        """get_unary(gb, 1, limit): v zeros, then a one unless v == limit."""
        for _ in range(v):
            b.put(0, 1)
        if v < limit:
            b.put(1, 1)

    def put_bfraction(self, b, idx):
        if idx < 7:
            b.put(idx, 3)
        else:
            b.put(7, 3); b.put(idx - 7, 4)

    def common_info(self, b, p):
        """PQINDEX HALFQP PQUANTIZER POSTPROC: the part every picture and field header has."""
        b.put(p['pqindex'], 5)
        if p['pqindex'] < 9:
            b.put(p['halfpq'], 1)
        if self.f['quantizer'] == 1:
            b.put(p['pquantizer'], 1)
        if self.f['postproc']:
            b.put(self.rng.randint(0, 3), 2)

    def mv_ranges(self, b, p):
        """MVRANGE and DMVRANGE."""
        if self.f['extended_mv']:
            self.put_unary_ones(b, p['mvrange'], 3)
        p['dmvrange'] = 0
        if self.f['extended_dmv']:
            p['dmvrange'] = self.rng.randint(0, 3)
            self.put_unary_ones(b, p['dmvrange'], 3)

    def lum(self, b):
        """LUMSCALE and LUMSHIFT: mild, so that a chain of intensity compensated pictures does not end up flat."""
        b.put(self.rng.randint(26, 38), 6)
        b.put(self.rng.choice([0, 1, 2, 3, 61, 62, 63]), 6)

    def tail(self, b, p, intra):
        """TRANSACFRM (TRANSACFRM2) TRANSDCTAB, and VOPDQUANT of an intra picture."""
        self.put012(b, p['c_ac'])
        if intra:
            self.put012(b, p['y_ac'])
        b.put(p['dc_table'], 1)
        if intra:
            self.vop_dquant(b, p)

    def write_icbp(self, b, p):
        """ICBPCY (interlaced pictures code 1 .. 63)."""
        cbp = self.rng.randint(1, 63)
        b.put(IT.ICBPCY_CODES[p['icbptab']][cbp - 1], IT.ICBPCY_BITS[p['icbptab']][cbp - 1])
        return cbp

    def residual(self, b, p, mbx, mby, cbp, st, cs_inter):
        """MQUANT and TTMB when there is a coded block, then the coded inter blocks."""
        quant = p['pq']
        ttmb = p['ttfrm']
        first = True
        if cbp:
            quant = abs(self.mquant_bits(b, p, mbx, mby))
            if not p['ttmbf']:
                ttmb = self.write_ttmb(b, p)
        for i in range(6):
            if (cbp >> (5 - i)) & 1:
                ttmb, first = self.write_inter_block(b, p, ttmb, first, cs_inter, quant, st)

    def write_imvdata(self, b, p):
        """MVDATA of an interlaced picture (get_mvdata_interlaced); returns (dmv_x, dmv_y, pred_flag) as the decoder makes them."""
        r = self.rng
        numref = p['numref']
        if numref:
            codes, bits, esc, ymax = IT.MVDATA_2REF_CODES[p['imvtab']], IT.MVDATA_2REF_BITS[p['imvtab']], 125, 13
        else:
            codes, bits, esc, ymax = IT.MVDATA_1REF_CODES[p['imvtab']], IT.MVDATA_1REF_BITS[p['imvtab']], 71, 7
        ex, ey = p['dmvrange'] & 1, (p['dmvrange'] >> 1) & 1
        self.count('mvdata%dref' % (numref + 1))
        if r.random() < 0.05:   # escape: the differentials as k_x and k_y bit numbers (for two references the low bit of y is the predictor flag)
            b.put(codes[esc], bits[esc])
            self.count('mvescape')
            dx, dy = r.randint(0, 15), r.randint(0, 15)
            b.put(dx, p['k_x']); b.put(dy, p['k_y'])
            pf = 0
            if numref:
                pf = dy & 1
                dy = (dy + (dy & 1)) >> 1
            return dx, dy, pf
        while True:
            xi = r.choice([0, 0, 1, 1, 2, 2, 3, 4, 5])
            yi = r.choice([0, 0, 1, 1, 2, 2, 3, 4, 5]) if not numref else r.choice([0, 1, 0, 1, 2, 3, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])
            if (xi or yi) and yi <= ymax:
                break
        index = xi + 9 * yi - 1
        b.put(codes[index], bits[index])
        dx = dy = 0
        if xi:
            n = xi + ex
            val = r.randint(0, (1 << n) - 1)
            b.put(val, n)
            sign = -(val & 1)
            dx = (sign ^ ((val >> 1) + OFFSET_TABLE[ex][xi])) - sign
        pf = (yi & 1) if numref else 0
        if yi > numref:
            ys = yi >> numref
            n = ys + ey
            val = r.randint(0, (1 << n) - 1)
            b.put(val, n)
            sign = -(val & 1)
            dy = (sign ^ ((val >> 1) + OFFSET_TABLE[ey][ys])) - sign
        return dx, dy, pf

    # ---- motion vector prediction of P field pictures (ff_vc1_pred_mv, field branch)
    def scale_same(self, p, v, dim):
        hpel = 0 if p['quarter'] else 1
        n = v >> hpel
        S = IT.FIELD_MVPRED_SCALES[p['second']]
        rd = min(self.refdist, 3)
        if dim == 0:
            if abs(n) > 255:
                s = n
            elif abs(n) < S[3][rd]:
                s = (n * S[1][rd]) >> 8
            else:
                s = ((n * S[2][rd]) >> 8) + (-S[5][rd] if n < 0 else S[5][rd])
            s = clip(s, -p['range_x'], p['range_x'] - 1)
        else:
            if abs(n) > 63:
                s = n
            elif abs(n) < S[4][rd]:
                s = (n * S[1][rd]) >> 8
            else:
                s = ((n * S[2][rd]) >> 8) + (-S[6][rd] if n < 0 else S[6][rd])
            half = p['range_y'] // 2
            if p['cft'] and not p['ref_ft']:
                s = clip(s, -half + 1, half)
            else:
                s = clip(s, -half, half - 1)
        return s * (1 << hpel)

    def scale_opp(self, p, v, dim):
        hpel = 0 if p['quarter'] else 1
        n = v >> hpel
        rd = min(self.refdist, 3)
        return ((n * IT.FIELD_MVPRED_SCALES[p['second']][0][rd]) >> 8) * (1 << hpel)

    def pred_mv_field(self, b, p, mbx, mby, n, dmv_x, dmv_y, mv1, pred_flag):
        if not p['quarter']:
            dmv_x *= 2
            dmv_y *= 2
        mbw = self.mbw
        bx, by = 2 * mbx + (n & 1), 2 * mby + (n >> 1)
        a_valid = mby != self.slice_top or n in (2, 3)
        b_valid = a_valid
        c_valid = mbx > 0 or n in (1, 3)
        if mv1:
            if p['mixed']:
                off = -2 if mbx == mbw - 1 else 2
            else:
                off = -1 if mbx == mbw - 1 else 2
            b_valid = b_valid and mbw > 1
        else:
            off = {0: (-1 if mbx else 1), 1: (-1 if mbx == mbw - 1 else 1), 2: 1, 3: -1}[n]
            if mbw == 1:
                b_valid = b_valid and c_valid
        fi = self.fintra
        pos = {'A': (bx, by - 1), 'B': (bx + off, by - 1), 'C': (bx - 1, by)}
        valid = {'A': a_valid and not fi.get(pos['A'], 0), 'B': b_valid and not fi.get(pos['B'], 0), 'C': c_valid and not fi.get(pos['C'], 0)}
        num_opp = num_same = 0
        pr = {}
        for k in 'ABC':
            if valid[k]:
                x, y = self.fmv[pos[k]]
                fl = self.fmvf[pos[k]]
                num_opp += fl
                num_same += 1 - fl
            else:
                x = y = fl = 0
            pr[k] = (x, y, fl)
        if p['numref'] == 0:
            opposite = 1 - p['reffield']
        elif num_same <= num_opp:
            opposite = 1 - pred_flag
        else:
            opposite = pred_flag
        p['ref_ft'] = (1 - p['cft']) if opposite else p['cft']
        for k in 'ABC':
            x, y, fl = pr[k]
            if valid[k] and fl != opposite:
                if opposite:
                    x, y = self.scale_opp(p, x, 0), self.scale_opp(p, y, 1)
                else:
                    x, y = self.scale_same(p, x, 0), self.scale_same(p, y, 1)
            pr[k] = (x, y)
        A, B, C = pr['A'], pr['B'], pr['C']
        if valid['A']:
            px, py = A
        elif valid['C']:
            px, py = C
        elif valid['B']:
            px, py = B
        else:
            px = py = 0
        if num_same + num_opp > 1:
            px, py = self.mid3(A[0], B[0], C[0]), self.mid3(A[1], B[1], C[1])
        if valid['A'] and valid['C']:   # hybrid prediction (P field pictures; never in B field pictures)
            if abs(px - A[0]) + abs(py - A[1]) > 32 or abs(px - C[0]) + abs(py - C[1]) > 32:
                bit = self.rng.randint(0, 1)
                b.put(bit, 1)
                px, py = A if bit else C
                self.stats['hybrid'] = self.stats.get('hybrid', 0) + 1
        rx = p['range_x']
        ry = p['range_y'] >> 1 if p['numref'] else p['range_y']
        yb = 1 if (p['cft'] and p['ref_ft'] == 0) else 0
        mx = ((px + dmv_x + rx) & ((rx << 1) - 1)) - rx
        my = ((py + dmv_y + ry - yb) & ((ry << 1) - 1)) - ry + yb
        blocks = [(2 * mbx + (k & 1), 2 * mby + (k >> 1)) for k in range(4)] if mv1 else [(bx, by)]
        for q in blocks:
            self.fmv[q] = (mx, my)
            self.fmvf[q] = opposite
        self.stats['opp' if opposite else 'same'] = self.stats.get('opp' if opposite else 'same', 0) + 1

    # ---- macroblocks shared by field and frame pictures
    def intra_mb_blocks(self, b, p, cbp, quant, st, cs_intra, cs_inter):
        for i in range(6):
            self.write_intra_block(b, p, i, (cbp >> (5 - i)) & 1, cs_intra if i < 4 else cs_inter, quant, st)

    def field_intra_mb(self, b, p, mbx, mby, has_cbp, st, cs_intra, cs_inter):
        """An intra macroblock of a P or B field: MQUANT, ACPRED, ICBPCY (MBMODE says whether)."""
        quant = abs(self.mquant_bits(b, p, mbx, mby))
        b.put(self.rng.randint(0, 1), 1)
        cbp = self.write_icbp(b, p) if has_cbp else 0
        self.intra_mb_blocks(b, p, cbp, quant, st, cs_intra, cs_inter)
        for k in range(4):
            self.fintra[(2 * mbx + (k & 1), 2 * mby + (k >> 1))] = 1
        self.stats['intra'] = self.stats.get('intra', 0) + 1

    def frame_intra_mb(self, b, p, mbx, mby, st, cs_intra, cs_inter):
        """An intra macroblock of an interlaced frame P or B picture: FIELDTX, CBPPRESENT, ICBPCY, ACPRED, MQUANT."""
        r = self.rng
        b.put(r.randint(0, 1), 1)
        has = r.randint(0, 1)
        b.put(has, 1)
        cbp = self.write_icbp(b, p) if has else 0
        b.put(r.randint(0, 1), 1)
        quant = abs(self.mquant_bits(b, p, mbx, mby))
        self.intra_mb_blocks(b, p, cbp, quant, st, cs_intra, cs_inter)
        self.stats['intra'] = self.stats.get('intra', 0) + 1

    def pick(self, weights):
        return self.rng.choices(range(len(weights)), weights)[0]

    # ---- picture headers
    def intra_header(self, b, p, fieldtx):
        """The rest of an I or BI picture header: FIELDTX (interlaced frame), ACPRED, CONDOVER, OVERFLAGS, then the AC and DC tables and VOPDQUANT."""
        pl = p['planes'] = {}
        if fieldtx:
            pl['fieldtx'] = self.plane_with_random_mode(b, 0.5)
        pl['acpred'] = self.plane_with_random_mode(b, 0.5)
        p['condover'] = 0
        if self.f['overlap'] and p['pq'] <= 8:
            p['condover'] = self.rng.randint(0, 2)
            self.count('condover%d' % p['condover'])
            self.put012(b, p['condover'])
            if p['condover'] == 2:
                pl['overflags'] = self.plane_with_random_mode(b, 0.5)
        self.tail(b, p, True)

    def p_field_header(self, b, p):
        r = self.rng
        f = self.f
        p['planes'] = {}
        first_frame = not self.anchors
        p['numref'] = 0 if first_frame else r.randint(0, 1)    # NUMREF (in the first frame the only field before is the first field)
        b.put(p['numref'], 1)
        p['reffield'] = 0
        if not p['numref']:
            p['reffield'] = 0 if first_frame else r.randint(0, 1)   # REFFIELD
            b.put(p['reffield'], 1)
        self.mv_ranges(b, p)
        lowquant = 0 if p['pq'] > 12 else 1
        if self.ic_prob and r.random() < self.ic_prob:
            m = 3   # (MVMODE index 3 is intensity compensation in both tables)
        else:
            m = r.randint(0, 3 if f['no_ic'] else 4)
        self.put_unary_zeros(b, m, 4)
        mvmode = T['kMvPmodeTable'][lowquant * 5 + m]
        eff = mvmode
        p['intcompfield'] = 0
        if mvmode == 4:   # intensity compensation: MVMODE2, INTCOMPFIELD, LUMSCALE/LUMSHIFT of the top and/or the bottom reference field
            m2 = r.randint(0, 3)
            self.put_unary_zeros(b, m2, 3)
            eff = T['kMvPmodeTable2'][lowquant * 4 + m2]
            icf = p['intcompfield'] = r.choice([1, 2, 3])
            if icf == 3:
                b.put(1, 1)
            else:
                b.put(0, 1); b.put(1 if icf == 2 else 0, 1)
            if icf & 1:
                self.lum(b)
            if icf & 2:
                self.lum(b)
        p['mv_mode'], p['mv_mode_eff'] = mvmode, eff
        p['quarter'] = eff not in (0, 2)
        p['mixed'] = eff == 3
        self.count('Pfield numref%d reffield%d mvmode%d/%d icf%d mvrange%d dmvrange%d' % (p['numref'], p['reffield'], mvmode, eff, p['intcompfield'], p['mvrange'], p['dmvrange']))
        p['mbmodetab'] = r.randint(0, 7); b.put(p['mbmodetab'], 3)
        p['imvtab'] = r.randint(0, 7 if p['numref'] else 3); b.put(p['imvtab'], 2 + p['numref'])
        p['icbptab'] = r.randint(0, 7); b.put(p['icbptab'], 3)
        if p['mixed']:
            p['fourmvbptab'] = r.randint(0, 3); b.put(p['fourmvbptab'], 2)
        self.vop_dquant(b, p)
        self.transform_info(b, p)
        self.tail(b, p, False)
        if not p['quarter']:
            p['range_x'] <<= 1
            p['range_y'] <<= 1

    def b_field_header(self, b, p):
        r = self.rng
        pl = p['planes'] = {}
        self.mv_ranges(b, p)
        lowquant = 0 if p['pq'] > 12 else 1
        m = r.randint(0, 3)
        self.put_unary_zeros(b, m, 3)
        p['mv_mode'] = p['mv_mode_eff'] = mv = T['kMvPmodeTable2'][lowquant * 4 + m]
        p['quarter'] = mv in (1, 3)
        p['mixed'] = mv == 3
        self.count('Bfield mvmode%d' % mv)
        pl['forward'] = self.plane_with_random_mode(b, 0.3)
        p['mbmodetab'] = r.randint(0, 7); b.put(p['mbmodetab'], 3)
        p['imvtab'] = r.randint(0, 7); b.put(p['imvtab'], 3)
        p['icbptab'] = r.randint(0, 7); b.put(p['icbptab'], 3)
        if p['mixed']:
            p['fourmvbptab'] = r.randint(0, 3); b.put(p['fourmvbptab'], 2)
        self.vop_dquant(b, p)
        self.transform_info(b, p)
        self.tail(b, p, False)
        p['numref'] = 1   # (B fields always have two references)

    def p_frame_header(self, b, p):
        r = self.rng
        pl = p['planes'] = {}
        p['numref'] = 0
        self.mv_ranges(b, p)
        p['fourmvswitch'] = r.randint(0, 1); b.put(p['fourmvswitch'], 1)
        ic = 0 if self.f['no_ic'] else r.randint(0, 1)
        self.count('Pframe 4mvswitch%d intcomp%d' % (p['fourmvswitch'], ic))
        b.put(ic, 1)   # INTCOMP
        if ic:
            self.lum(b)
        pl['skip'] = self.plane_with_random_mode(b, 0.25)
        p['mbmodetab'] = r.randint(0, 3); b.put(p['mbmodetab'], 2)
        p['imvtab'] = r.randint(0, 3); b.put(p['imvtab'], 2)
        p['icbptab'] = r.randint(0, 7); b.put(p['icbptab'], 3)
        p['twomvbptab'] = r.randint(0, 3); b.put(p['twomvbptab'], 2)
        if p['fourmvswitch']:
            p['fourmvbptab'] = r.randint(0, 3); b.put(p['fourmvbptab'], 2)
        self.vop_dquant(b, p)
        self.transform_info(b, p)
        self.tail(b, p, False)
        p['quarter'] = True

    def b_frame_header(self, b, p, allow_direct):
        r = self.rng
        pl = p['planes'] = {}
        p['numref'] = 0
        idx = p['bfraction_index'] if p['bfraction_index'] <= 20 else 1
        self.put_bfraction(b, idx)
        self.mv_ranges(b, p)
        b.put(0, 1)   # INTCOMP (always 0 in B pictures)
        pl['direct'] = self.plane_with_random_mode(b, 0.3 if allow_direct else 0.0)
        pl['skip'] = self.plane_with_random_mode(b, 0.25)
        p['mbmodetab'] = r.randint(0, 3); b.put(p['mbmodetab'], 2)
        p['imvtab'] = r.randint(0, 3); b.put(p['imvtab'], 2)
        p['icbptab'] = r.randint(0, 7); b.put(p['icbptab'], 3)
        p['twomvbptab'] = r.randint(0, 3); b.put(p['twomvbptab'], 2)
        p['fourmvbptab'] = r.randint(0, 3); b.put(p['fourmvbptab'], 2)
        self.vop_dquant(b, p)
        self.transform_info(b, p)
        self.tail(b, p, False)
        p['quarter'] = True

    # ---- picture bodies
    def body_p_field(self, b, p):
        r = self.rng
        st = {'esc3': None}
        cs_intra, cs_inter = self.coding_sets(p)
        self.fmv, self.fmvf, self.fintra = {}, {}, {}
        tab = p['mbmodetab']
        codes, bits = (IT.IF_MMV_MBMODE_CODES[tab], IT.IF_MMV_MBMODE_BITS[tab]) if p['mixed'] else (IT.IF_1MV_MBMODE_CODES[tab], IT.IF_1MV_MBMODE_BITS[tab])
        weights = [1, 1, 3, 3, 3, 3] + ([3, 3] if p['mixed'] else [])
        for mby in range(self.cur_mbh):
            b = self.slice_hook(b, mby, st)
            for mbx in range(self.mbw):
                idx = self.pick(weights)
                b.put(codes[idx], bits[idx])
                if idx <= 1:
                    self.field_intra_mb(b, p, mbx, mby, idx & 1, st, cs_intra, cs_inter)
                    continue
                for k in range(4):
                    self.fintra[(2 * mbx + (k & 1), 2 * mby + (k >> 1))] = 0
                if idx <= 5:   # 1MV: MVDATA if idx & 1, coded blocks unless idx & 2
                    dx = dy = pf = 0
                    if idx & 1:
                        dx, dy, pf = self.write_imvdata(b, p)
                    self.pred_mv_field(b, p, mbx, mby, 0, dx, dy, True, pf)
                    coeffs = not (idx & 2)
                else:          # 4MV: 4MVBP, the vectors it says, coded blocks if idx & 1
                    bp = r.randint(0, 15)
                    b.put(IT.FOURMVBP_CODES[p['fourmvbptab']][bp], IT.FOURMVBP_BITS[p['fourmvbptab']][bp])
                    for n in range(4):
                        dx = dy = pf = 0
                        if bp & (8 >> n):
                            dx, dy, pf = self.write_imvdata(b, p)
                        self.pred_mv_field(b, p, mbx, mby, n, dx, dy, False, pf)
                    coeffs = idx & 1
                    self.stats['4mv'] = self.stats.get('4mv', 0) + 1
                cbp = self.write_icbp(b, p) if coeffs else 0
                self.residual(b, p, mbx, mby, cbp, st, cs_inter)

    def body_b_field(self, b, p, allow_direct):
        r = self.rng
        st = {'esc3': None}
        cs_intra, cs_inter = self.coding_sets(p)
        self.fintra = {}
        tab = p['mbmodetab']
        codes, bits = (IT.IF_MMV_MBMODE_CODES[tab], IT.IF_MMV_MBMODE_BITS[tab]) if p['mixed'] else (IT.IF_1MV_MBMODE_CODES[tab], IT.IF_1MV_MBMODE_BITS[tab])
        weights = [1, 1, 3, 3, 3, 3] + ([3, 3] if p['mixed'] else [])
        fplane, fraw = p['planes']['forward']
        for mby in range(self.cur_mbh):
            b = self.slice_hook(b, mby, st)
            for mbx in range(self.mbw):
                idx = self.pick(weights)
                b.put(codes[idx], bits[idx])
                if idx <= 1:
                    self.field_intra_mb(b, p, mbx, mby, idx & 1, st, cs_intra, cs_inter)
                    continue
                fwd = fplane[mby][mbx]
                if fraw:
                    b.put(fwd, 1)   # FORWARDBIT
                if idx <= 5:
                    t = 'F'
                    if not fwd:    # BMVTYPE: 0 backward, 10 direct, 11 interpolated (then INTERPMVP)
                        t = r.choice(['B', 'I'] + (['D'] if allow_direct else []))
                        self.put012(b, {'B': 0, 'D': 1, 'I': 2}[t])
                    interpmvp = 0
                    if t == 'I':
                        interpmvp = r.randint(0, 1)
                        b.put(interpmvp, 1)
                    if t != 'D' and idx & 1:
                        self.write_imvdata(b, p)
                    if interpmvp:
                        self.write_imvdata(b, p)
                    self.stats['b' + t] = self.stats.get('b' + t, 0) + 1
                    coeffs = not (idx & 2)
                else:
                    bp = r.randint(0, 15)
                    b.put(IT.FOURMVBP_CODES[p['fourmvbptab']][bp], IT.FOURMVBP_BITS[p['fourmvbptab']][bp])
                    for n in range(4):
                        if bp & (8 >> n):
                            self.write_imvdata(b, p)
                    coeffs = idx & 1
                    self.stats['4mv'] = self.stats.get('4mv', 0) + 1
                cbp = self.write_icbp(b, p) if coeffs else 0
                self.residual(b, p, mbx, mby, cbp, st, cs_inter)

    def body_p_frame(self, b, p):
        r = self.rng
        st = {'esc3': None}
        cs_intra, cs_inter = self.coding_sets(p)
        sw = p['fourmvswitch']
        table = IT.INTFR_MBMODE[sw]
        tab = p['mbmodetab']
        codes, bits = (IT.INTFR_4MV_MBMODE_CODES[tab], IT.INTFR_4MV_MBMODE_BITS[tab]) if sw else (IT.INTFR_NON4MV_MBMODE_CODES[tab], IT.INTFR_NON4MV_MBMODE_BITS[tab])
        weights = [3] * (len(table) - 1) + [2]
        skip_plane, skip_raw = p['planes']['skip']
        for mby in range(self.cur_mbh):
            b = self.slice_hook(b, mby, st)
            for mbx in range(self.mbw):
                skipped = skip_plane[mby][mbx]
                if skip_raw:
                    b.put(skipped, 1)
                if skipped:
                    continue
                idx = self.pick(weights)
                b.put(codes[idx], bits[idx])
                typ, _fieldtx, mvp, cbpp = table[idx]
                self.stats['P' + typ] = self.stats.get('P' + typ, 0) + 1
                if typ == 'INTRA':
                    self.frame_intra_mb(b, p, mbx, mby, st, cs_intra, cs_inter)
                    continue
                cbp = self.write_icbp(b, p) if cbpp else 0
                if typ == '2MV_FIELD':
                    bp = r.randint(0, 3)
                    b.put(IT.TWOMVBP_CODES[p['twomvbptab']][bp], IT.TWOMVBP_BITS[p['twomvbptab']][bp])
                    for bit in (2, 1):
                        if bp & bit:
                            self.write_imvdata(b, p)
                elif typ in ('4MV', '4MV_FIELD'):
                    bp = r.randint(0, 15)
                    b.put(IT.FOURMVBP_CODES[p['fourmvbptab']][bp], IT.FOURMVBP_BITS[p['fourmvbptab']][bp])
                    for n in range(4):
                        if bp & (8 >> n):
                            self.write_imvdata(b, p)
                elif mvp:
                    self.write_imvdata(b, p)
                self.residual(b, p, mbx, mby, cbp, st, cs_inter)

    def body_b_frame(self, b, p):
        r = self.rng
        st = {'esc3': None}
        cs_intra, cs_inter = self.coding_sets(p)
        table = IT.INTFR_MBMODE[0]
        tab = p['mbmodetab']
        codes, bits = IT.INTFR_NON4MV_MBMODE_CODES[tab], IT.INTFR_NON4MV_MBMODE_BITS[tab]
        weights = [3] * 8 + [2]
        skip_plane, skip_raw = p['planes']['skip']
        dplane, draw = p['planes']['direct']
        for mby in range(self.cur_mbh):
            b = self.slice_hook(b, mby, st)
            for mbx in range(self.mbw):
                skipped = skip_plane[mby][mbx]
                if skip_raw:
                    b.put(skipped, 1)
                typ, mvp, cbpp = '1MV', 0, 0
                if not skipped:
                    idx = self.pick(weights)
                    b.put(codes[idx], bits[idx])
                    typ, _fieldtx, mvp, cbpp = table[idx]
                    if typ == 'INTRA':
                        self.frame_intra_mb(b, p, mbx, mby, st, cs_intra, cs_inter)
                        continue
                twomv = typ == '2MV_FIELD'
                direct = dplane[mby][mbx]
                if draw:
                    b.put(direct, 1)   # DIRECTBBIT
                bt = None
                if not direct:
                    bt = r.randint(0, 2)   # BMVTYPE: 0 and 1 forward / backward (which depends on BFRACTION), 2 interpolated
                    self.put012(b, bt)
                    if twomv and bt != 2:
                        b.put(r.randint(0, 1), 1)   # MVSW
                self.stats['B%s%s' % (typ, 'D' if direct else bt)] = self.stats.get('B%s%s' % (typ, 'D' if direct else bt), 0) + 1
                if skipped:
                    continue
                cbp = self.write_icbp(b, p) if cbpp else 0
                if not direct:
                    if bt == 2 and twomv:
                        bp = r.randint(0, 15)
                        b.put(IT.FOURMVBP_CODES[p['fourmvbptab']][bp], IT.FOURMVBP_BITS[p['fourmvbptab']][bp])
                        for i in range(4):
                            if (bp >> (3 - i)) & 1:
                                self.write_imvdata(b, p)
                    elif bt == 2 or twomv:
                        bp = r.randint(0, 3)
                        b.put(IT.TWOMVBP_CODES[p['twomvbptab']][bp], IT.TWOMVBP_BITS[p['twomvbptab']][bp])
                        for bit in (2, 1):
                            if bp & bit:
                                self.write_imvdata(b, p)
                    elif mvp:
                        self.write_imvdata(b, p)
                self.residual(b, p, mbx, mby, cbp, st, cs_inter)

    # ---- frames
    def anchor(self, fcm):
        self.anchor_fcm = fcm
        self.anchors += 1

    def field_frame(self, types):
        """A field pair: FCM 11, FPTYPE, TFCNTR, TFF/RFF, RNDCTRL, UVSAMP, REFDIST or BFRACTION, the first field, a field start code, the second
        field; slices in either field."""
        r = self.rng
        f = self.f
        b = self.begin_frame()
        b.put(3, 2)
        b.put(self.FPTYPE[types], 3)
        if f['tfcntr']:
            b.put(r.randint(0, 255), 8)
        tff = 1
        if f['broadcast']:
            tff = r.randint(0, 1)
            b.put(tff, 1); b.put(r.randint(0, 1), 1)
        b.put(r.randint(0, 1), 1)   # RNDCTRL
        b.put(r.randint(0, 1), 1)   # UVSAMP
        is_b = types[0] in ('B', 'BI')
        self.count('fptype %s/%s tff%d' % (types[0], types[1], tff))
        if not f['refdist']:
            self.refdist = 0
        elif not is_b:   # REFDIST: 2 bits, 3 and up continue in unary
            rd = r.choice([0, 1, 2, 3, 4, 6])
            b.put(min(rd, 3), 2)
            if rd >= 3:
                self.put_unary_ones(b, rd - 3, 14)
            self.refdist = rd
            self.count('refdist%d' % rd)
        bfi = None
        if is_b:
            bfi = r.choice([0, 1, 2, 3, 4, 5, 6, 8, 10, 15, 20])
            self.put_bfraction(b, bfi)
        allow_direct = self.anchor_fcm == 'f'
        self.cur_mbh = (self.mbh + 1) // 2   # (a field has (height / 2 + 15) / 16 rows)
        for k, t in enumerate(types):
            if k:
                self.close_unit()
                self.units.append(Unit(0x0C))
                b = self.units[-1].b
            p = self.new_picture(t)
            p.update(second=k, cft=0 if (tff ^ k) else 1, field=True)
            if bfi is not None:
                p['bfraction_index'] = bfi
            self.slice_top = 0
            self.slice_rows = self.pick_slices(self.cur_mbh)
            self.addr_base = k * self.cur_mbh
            self.common_info(b, p)
            if t in ('I', 'BI'):
                self.intra_header(b, p, False)
                self.mark_header(b)
                self.body_intra_picture(b, p)
            elif t == 'P':
                self.p_field_header(b, p)
                self.mark_header(b)
                self.body_p_field(b, p)
            else:
                self.b_field_header(b, p)
                self.mark_header(b)
                self.body_b_field(b, p, allow_direct)
        rows = 2 * self.cur_mbh
        self.cur_mbh = self.mbh
        if not is_b:
            self.anchor('f')
        return self.finish_frame(rows)

    def frame_interlaced(self, t):
        """An interlaced frame picture: FCM 10, PTYPE, TFCNTR, TFF/RFF, RNDCTRL, UVSAMP, the picture header, the macroblocks (and slices)."""
        r = self.rng
        f = self.f
        b = self.begin_frame()
        b.put(2, 2)
        u = {'P': 0, 'B': 1, 'I': 2, 'BI': 3, 'S': 4}[t]
        self.put_unary_ones(b, u, 4)
        if f['tfcntr']:
            b.put(r.randint(0, 255), 8)
        if f['broadcast']:
            b.put(r.randint(0, 1), 1); b.put(r.randint(0, 1), 1)
        if t == 'S':
            self.mark_header(b)
            self.units[0].rows = self.mbh
            self.anchor('r')
            return self.finish_frame(self.mbh)
        b.put(r.randint(0, 1), 1)   # RNDCTRL
        b.put(r.randint(0, 1), 1)   # UVSAMP
        p = self.new_picture(t)
        allow_direct = self.anchor_fcm == 'r'
        self.slice_rows = self.pick_slices(self.mbh)
        self.common_info(b, p)
        if t in ('I', 'BI'):
            self.intra_header(b, p, True)
            self.mark_header(b)
            self.body_intra_picture(b, p)
        elif t == 'P':
            self.p_frame_header(b, p)
            self.mark_header(b)
            self.body_p_frame(b, p)
        else:
            self.b_frame_header(b, p, allow_direct)
            self.mark_header(b)
            self.body_b_frame(b, p)
        if t in ('I', 'P'):
            self.anchor('r')
        return self.finish_frame(self.mbh)

    def frame_progressive(self, t):
        """A progressive picture in the interlaced sequence (FCM 0), possibly in slices."""
        b = self.begin_frame()
        if t == 'S':
            p = self.new_picture('P', skipped=True)
            self.header(b, p)
            self.mark_header(b)
            self.units[0].rows = self.mbh
            self.anchor('p')
            return self.finish_frame(self.mbh)
        p = self.new_picture(t)
        if t == 'B' and self.anchor_fcm != 'p':
            p['direct_density'] = 0.0
        self.slice_rows = self.pick_slices(self.mbh)
        self.header(b, p)
        self.mark_header(b)
        if t in ('I', 'BI'):
            self.body_intra_picture(b, p)
        else:
            self.body_inter_picture(b, p)
        if t in ('I', 'P'):
            self.anchor('p')
        return self.finish_frame(self.mbh)

    def frame(self, token):
        """token: 'f' + the field types ('fIP', 'fBX': X is BI), 'r' + type (interlaced frame), 'p' + type (progressive); types I P B X(BI) S(skipped)."""
        kind, rest = token[0], token[1:].replace('X', 'Y')
        types = [{'Y': 'BI'}.get(c, c) for c in rest]
        if kind == 'f':
            return self.field_frame(tuple(types))
        if kind == 'r':
            return self.frame_interlaced(types[0])
        return self.frame_progressive(types[0])


def stream_interlaced(seed, profile, w, h, n, frames=(), **flags):
    enc = IEnc(seed, w, h, **flags)
    out = [enc.frame(t) for t in frames]
    assert len(out) == n, (len(out), n)
    return enc, out


def write_units(path, enc, frames):
    """An Advanced profile elementary stream of frames made of start code units."""
    out = bytearray()
    out += b'\x00\x00\x01\x0f' + escape_bytes(enc.adv_sequence())
    out += b'\x00\x00\x01\x0e' + escape_bytes(enc.adv_entry_point())
    for units in frames:
        for code, data in units:
            out += bytes([0, 0, 1, code]) + escape_bytes(data)
    open(path, 'wb').write(bytes(out))


def write_rcv(path, enc, frames):
    """frames: list of (bytes, key)."""
    out = bytearray()
    out += struct.pack('<I', len(frames) | 0xC5000000)
    out += struct.pack('<I', 4)
    out += enc.struct_c()
    out += struct.pack('<III', enc.h, enc.w, 0xC)
    out += b'\x00' * 8
    out += struct.pack('<I', 0xFFFFFFFF)
    for i, (data, key) in enumerate(frames):
        out += struct.pack('<I', len(data))[:3]
        out += bytes([0x80 if key else 0])
        out += struct.pack('<I', i * 40)
        out += data
    open(path, 'wb').write(bytes(out))


def write_adv(path, enc, frames):
    out = bytearray()
    out += b'\x00\x00\x01\x0f' + escape_bytes(enc.adv_sequence())
    out += b'\x00\x00\x01\x0e' + escape_bytes(enc.adv_entry_point())
    for data, key in frames:
        out += b'\x00\x00\x01\x0d' + escape_bytes(data)
    open(path, 'wb').write(bytes(out))


def stream_i_only(seed, profile, w, h, n, **flags):
    enc = Enc(seed, profile, w, h, **flags)
    frames = []
    for i in range(n):
        p = enc.new_picture('I')
        frames.append((enc.frame_bytes(p), True))
    return enc, frames


def stream_pattern(seed, profile, w, h, n, pattern='I', **flags):
    """Pictures in decoding order: `pattern` (letters I P B X (BI)) repeated to n pictures; only the first is forced to be an I picture."""
    enc = Enc(seed, profile, w, h, **flags)
    frames = []
    for i in range(n):
        t = 'I' if i == 0 else {'X': 'BI', 'S': 'P'}.get(pattern[i % len(pattern)], pattern[i % len(pattern)])
        p = enc.new_picture(t, skipped=True) if (i and pattern[i % len(pattern)] == 'S') else enc.new_picture(t)
        frames.append((enc.frame_bytes(p), t == 'I'))
    return enc, frames


def selftest_streams():
    """The two streams of the engine self-test: Main profile (the frames as a container stores them, with the sequence header apart) and Advanced profile (start codes)."""
    flags = dict(pattern='IPBBPBBP', max_b_frames=2, loop_filter=1, overlap=1, vstransform=1, no_ic=1)
    enc_m, frames_m = stream_pattern(41, 1, 64, 64, 8, **flags)
    enc_a, frames_a = stream_pattern(42, 3, 64, 64, 8, **dict(pattern='IPBBPBBP', loop_filter=1, overlap=1, vstransform=1, no_ic=1))
    # The Advanced stream: the sequence header and entry point in front of the first frame, then every frame behind its start code.
    units = []
    for i, (data, key) in enumerate(frames_a):
        u = b''
        if i == 0:
            u += b'\x00\x00\x01\x0f' + escape_bytes(enc_a.adv_sequence()) + b'\x00\x00\x01\x0e' + escape_bytes(enc_a.adv_entry_point())
        units.append(u + b'\x00\x00\x01\x0d' + escape_bytes(data))
    # The third stream: Advanced profile in an interlaced sequence (the frames are progressive; FFmpeg has no interlaced VC-1 encoder or reference for a stream made here).
    enc_i, frames_i = stream_pattern(43, 3, 64, 64, 8, **dict(pattern='IPBBPBBP', interlace=1, broadcast=1, loop_filter=1, overlap=1, vstransform=1, no_ic=1))
    units_i = []
    for i, (data, key) in enumerate(frames_i):
        u = b''
        if i == 0:
            u += b'\x00\x00\x01\x0f' + escape_bytes(enc_i.adv_sequence()) + b'\x00\x00\x01\x0e' + escape_bytes(enc_i.adv_entry_point())
        units_i.append(u + b'\x00\x00\x01\x0d' + escape_bytes(data))
    return enc_m.struct_c(), [d for d, _ in frames_m], units, units_i


def write_selftest(path):
    config, main, adv, interlaced = selftest_streams()

    def array(name, data, ctype='uint8_t'):
        rows = ['    ' + ', '.join('0x%02X' % b for b in data[j:j + 24]) + ',' for j in range(0, len(data), 24)]
        return 'constexpr %s %s[%d] = {\n%s\n};\n' % (ctype, name, len(data), '\n'.join(rows))

    def ends(name, parts):
        total, out = 0, []
        for p in parts:
            total += len(p)
            out.append(total)
        return 'constexpr uint64_t %s[%d] = {%s};\n' % (name, len(out), ', '.join(str(e) for e in out))

    text = ('// Generated by scripts/gen_vc1_synthetic.py --selftest (see scripts/gen_vc1_selftest.sh): two 64 x 64 streams of %d pictures, I P B B P B B P in coding order, one packet per\n'
            '// picture: Main profile (the sequence header apart, the frames as a container stores them) and Advanced profile (with start codes, the first packet with the\n'
            '// sequence header and entry point). The streams the VCN VC-1 self-test has the engine decode. Do not edit by hand.\n'
            'constexpr uint32_t kVc1SelfTestPictures = %d;\n'
            'constexpr uint32_t kVc1SelfTestSize = 64;\n') % (len(main), len(main))
    text += array('kVc1SelfTestMainConfig', config)
    text += ends('kVc1SelfTestMainEnds', main)
    text += array('kVc1SelfTestMainStream', b''.join(main))
    text += ends('kVc1SelfTestAdvancedEnds', adv)
    text += array('kVc1SelfTestAdvancedStream', b''.join(adv))
    text += '// The same pattern in an interlaced sequence (progressive frames).\n'
    text += ends('kVc1SelfTestInterlacedEnds', interlaced)
    text += array('kVc1SelfTestInterlacedStream', b''.join(interlaced))
    open(path, 'w').write(text)


def range_map(name, yuv):
    """Applies the range mapping of stream `name` (RANGE_MAPY / RANGE_MAPUV of its entry point, SMPTE 421M 6.2.15 and 6.2.16) to the decoded
    pictures in `yuv`: FFmpeg parses the fields but does not map ("Luma scaling is not supported"), so the expected pictures are FFmpeg's
    (which are right before the mapping: the mapping only changes the output, not the reference pictures) mapped here."""
    seed, profile, w, h, n, flags = STREAMS[name]
    data = bytearray(open(yuv, 'rb').read())
    ysize, csize = w * h, (w // 2) * (h // 2)
    assert len(data) == n * (ysize + 2 * csize)

    def lut(m):
        return bytes(range(256)) if m is None else bytes(clip((((v - 128) * (m + 9) + 4) >> 3) + 128, 0, 255) for v in range(256))
    ly, luv = lut(flags.get('range_mapy')), lut(flags.get('range_mapuv'))
    for i in range(n):
        o = i * (ysize + 2 * csize)
        data[o:o + ysize] = data[o:o + ysize].translate(ly)
        data[o + ysize:o + ysize + 2 * csize] = data[o + ysize:o + ysize + 2 * csize].translate(luv)
    open(yuv, 'wb').write(bytes(data))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--selftest':
        write_selftest(sys.argv[2])
        return
    if len(sys.argv) == 4 and sys.argv[1] == '--rangemap':
        range_map(sys.argv[2], sys.argv[3])
        return
    expect = None
    if len(sys.argv) == 4 and sys.argv[1] == '--expect':
        expect = sys.argv[2]
        del sys.argv[1:3]
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    expected = {}
    for name, (seed, profile, w, h, n, flags) in STREAMS.items():
        enc, frames = BUILDERS[name](seed, profile, w, h, n, **flags)
        if 'frames' in flags:
            write_units(os.path.join(out, 'vc1_%s.vc1' % name), enc, frames)
        elif profile == 3:
            write_adv(os.path.join(out, 'vc1_%s.vc1' % name), enc, frames)
        else:
            write_rcv(os.path.join(out, 'vc1_%s.rcv' % name), enc, frames)
        expected[name] = enc.expect
    if expect:
        import json
        json.dump(expected, open(expect, 'w'))


STREAMS = {
    # intra only
    'simple_intra': (1, 0, 96, 64, 3, dict()),
    'sm_intra': (2, 1, 96, 64, 3, {}),
    'sm_intra_lf': (3, 1, 112, 80, 5, dict(loop_filter=1, overlap=1)),
    'sm_intra_q': (4, 1, 64, 48, 6, dict(quantizer=1, dquant=1, vstransform=1)),
    'sm_intra_nu': (5, 1, 80, 64, 4, dict(quantizer=2, overlap=1, loop_filter=1)),
    'adv_intra': (6, 3, 96, 64, 4, {}),
    'adv_intra_lf': (7, 3, 112, 80, 6, dict(loop_filter=1, overlap=1, dquant=1)),
    'adv_intra_odd': (8, 3, 90, 54, 4, dict(loop_filter=1, overlap=1, quantizer=1, postproc=1, broadcast=1)),
    # P pictures (1MV, 4MV, half sample, intensity compensation, skipped macroblocks)
    'sp_ipp': (11, 0, 96, 64, 8, dict(pattern='IPPP')),
    'sm_ipp': (13, 1, 96, 64, 8, dict(pattern='IPPP', extended_mv=1, vstransform=1)),
    'sm_ipp_q': (14, 1, 80, 64, 10, dict(pattern='IPPP', quantizer=1, dquant=1, loop_filter=1, overlap=1, vstransform=1, extended_mv=1)),
    'sm_ipp_nu': (21, 1, 64, 48, 8, dict(pattern='IPPP', quantizer=2, overlap=1, loop_filter=1)),
    # B pictures (direct, forward, backward, interpolated), BI pictures, reordering
    'sm_ipb': (15, 1, 96, 64, 9, dict(pattern='IPBB', max_b_frames=2)),
    'sm_ipb_f': (16, 1, 96, 64, 12, dict(pattern='IPBBPBBX', max_b_frames=3, finterp=1, loop_filter=1, overlap=1, vstransform=1, dquant=1)),
    # Advanced profile
    'adv_ipp': (17, 3, 96, 64, 8, dict(pattern='IPPP')),
    'adv_ipb': (18, 3, 112, 80, 10, dict(pattern='IPBBPBXB', loop_filter=1, overlap=1, dquant=1, extended_mv=1, vstransform=1, finterp=1)),
    'adv_ipp_odd': (19, 3, 90, 54, 8, dict(pattern='IPPBPP', loop_filter=1, overlap=1, quantizer=1, postproc=1, broadcast=1, tfcntr=1)),
    'adv_skip': (22, 3, 80, 48, 10, dict(pattern='IPSPPSSP', extended_mv=1, loop_filter=1, overlap=1, broadcast=1)),
    # Advanced profile in an interlaced sequence, every frame progressive (the frame coding mode is there, and the pulldown bits are TFF and RFF; FFmpeg has no progressive segmented frame mode, so psf stays 0)
    'adv_il_ipb': (31, 3, 96, 64, 10, dict(pattern='IPBBPBXB', interlace=1, loop_filter=1, overlap=1, dquant=1, extended_mv=1, vstransform=1, finterp=1, broadcast=1, tfcntr=1)),
    'adv_il_ipp': (32, 3, 90, 54, 8, dict(pattern='IPPBPP', interlace=1, loop_filter=1, overlap=1, quantizer=1, postproc=1, broadcast=1, extended_mv=1, extended_dmv=1)),
    'adv_il_skip': (33, 3, 80, 48, 10, dict(pattern='IPSPPSSP', interlace=1, extended_mv=1, loop_filter=1, overlap=1)),
    # Interlaced pictures (stream_interlaced; `frames` in decoding order: 'f' + the two field types (X = BI), 'r' + the type of an interlaced
    # frame picture, 'p' + the type of a progressive picture, S = skipped; `slices` the probability that a picture is cut into slices).
    # Field pictures: FPTYPE I/I I/P P/I P/P, NUMREF 0/1, REFFIELD, MVMODE incl. intensity compensation (INTCOMPFIELD), 1MV and mixed-MV fields
    'adv_fi_ipp': (201, 3, 96, 64, 8, dict(frames=['fII', 'fIP', 'fPP', 'fPI', 'fPP', 'fPP', 'fIP', 'fPP'], loop_filter=1, overlap=1, ic_prob=0.5)),
    # ... with MVRANGE and DMVRANGE, REFDIST, DQUANT, VSTRANSFORM, overlap, TFF/RFF, TFCNTR, explicit quantizer; 3 macroblock rows per field
    'adv_fi_mv': (202, 3, 112, 96, 10, dict(frames=['fII', 'fPP', 'fPP', 'fPI', 'fPP', 'fIP', 'fPP', 'fPP', 'fPP', 'fPP'], extended_mv=1, extended_dmv=1,
                                            refdist=1, dquant=1, vstransform=1, overlap=1, loop_filter=1, broadcast=1, tfcntr=1, quantizer=1,
                                            ic_prob=0.3)),
    # B fields: FPTYPE B/B B/BI BI/B BI/BI, BFRACTION, FORWARDMB, BMVTYPE (forward, backward, direct, interpolated), 4MV B fields
    'adv_fi_b': (203, 3, 96, 64, 12, dict(frames=['fII', 'fPP', 'fBB', 'fPP', 'fBX', 'fXB', 'fPI', 'fXX', 'fBB', 'fPP', 'fBB', 'fBB'], refdist=1,
                                          broadcast=1, extended_mv=1, dquant=1, vstransform=1, loop_filter=1)),
    # Slices in both fields (SLICE_ADDR of the second field counts on from the first field's rows), odd size, 3 rows per field
    'adv_fi_slices': (204, 3, 122, 90, 8, dict(frames=['fII', 'fPP', 'fIP', 'fBB', 'fPP', 'fBX', 'fPI', 'fBB'], slices=0.9, dquant=1, overlap=1,
                                               loop_filter=1, extended_mv=1, extended_dmv=1)),
    # Interlaced frame pictures: I with FIELDTX, ACPRED, CONDOVER/OVERFLAGS; BI
    'adv_fr_i': (206, 3, 96, 64, 6, dict(frames=['rI', 'rI', 'rP', 'rX', 'rI', 'rX'], overlap=1, dquant=1, loop_filter=1)),
    # P: every MBMODE (1MV, 2MV field, 4MV, 4MV field, intra, skipped), 4MVSWITCH, INTCOMP, 2MVBP/4MVBP, MVRANGE/DMVRANGE, a skipped frame; 5 rows
    'adv_fr_p': (205, 3, 112, 80, 10, dict(frames=['rI', 'rP', 'rP', 'rP', 'rS', 'rP', 'rP', 'rI', 'rP', 'rP'], extended_mv=1, extended_dmv=1,
                                           vstransform=1, dquant=1, loop_filter=1, quantizer=2)),
    # B: direct (DIRECTMB), forward/backward/interpolated, 2MV field B with MVSW, skipped
    'adv_fr_b': (207, 3, 96, 64, 10, dict(frames=['rI', 'rP', 'rB', 'rB', 'rP', 'rB', 'rX', 'rP', 'rB', 'rB'], dquant=1, vstransform=1, extended_mv=1,
                                          loop_filter=1, broadcast=1, overlap=1)),
    # Slices in interlaced frame pictures, odd size
    'adv_fr_slices': (208, 3, 106, 70, 8, dict(frames=['rI', 'rP', 'rB', 'rP', 'rB', 'rI', 'rP', 'rB'], slices=0.9, extended_mv=1, extended_dmv=1,
                                               dquant=1, loop_filter=1)),
    # Progressive, interlaced frame and field pictures in one sequence; B pictures of each kind between anchors of other kinds; slices
    'adv_mix_a': (209, 3, 112, 96, 12, dict(frames=['pI', 'fPP', 'rP', 'fBB', 'pP', 'rB', 'fPI', 'pB', 'rP', 'fBX', 'pS', 'fPP'], slices=0.5,
                                            extended_mv=1, dquant=1, loop_filter=1, overlap=1, vstransform=1)),
    'adv_mix_b': (210, 3, 106, 90, 12, dict(frames=['fII', 'rP', 'pP', 'fBB', 'pB', 'rB', 'fPP', 'rS', 'pS', 'fXB', 'rX', 'pX'], slices=0.6,
                                            refdist=1, broadcast=1, tfcntr=1, quantizer=1, postproc=1, extended_mv=1, extended_dmv=1, loop_filter=1)),
    # An odd number of macroblock rows in the frame (5: each field has 3, the last one half outside the field), 5 columns, all three kinds, slices
    'adv_mix_odd': (215, 3, 74, 80, 10, dict(frames=['fII', 'pP', 'rP', 'fBB', 'fPP', 'rB', 'pB', 'fIP', 'fXB', 'rP'], slices=0.6, dquant=1,
                                              extended_mv=1, vstransform=1, loop_filter=1, overlap=1)),
    # Range mapping (entry point RANGE_MAPY / RANGE_MAPUV; the expected pictures are mapped by range_map, see there)
    'adv_rmap_p': (211, 3, 96, 64, 8, dict(pattern='IPBBPBXB', range_mapy=3, range_mapuv=6, loop_filter=1, vstransform=1)),
    'adv_rmap_il': (212, 3, 96, 64, 8, dict(frames=['fII', 'rP', 'fBB', 'pP', 'rB', 'fPP', 'pB', 'rP'], range_mapy=7, loop_filter=1)),
    # Main profile range reduction (RANGERED, RANGEREDFRM random per picture, so P and B pictures follow pictures with the other value)
    'main_rr_ipb': (213, 1, 96, 64, 10, dict(pattern='IPBBPBBX', max_b_frames=2, rangered=1, loop_filter=1, overlap=1, vstransform=1, dquant=1)),
    'main_rr_ipp': (214, 1, 80, 64, 10, dict(pattern='IPPP', rangered=1, extended_mv=1, quantizer=1, loop_filter=1)),
}
BUILDERS = {name: (stream_interlaced if 'frames' in STREAMS[name][5] else stream_pattern if 'pattern' in STREAMS[name][5] else stream_i_only)
            for name in STREAMS}

if __name__ == '__main__':
    main()
