#!/usr/bin/env python3
"""Writes VC-1 test streams. There is no VC-1 encoder to be had, so this one makes streams out of random syntax elements: random macroblock types, vectors, coded block
patterns, quantizer changes and coefficients (all small, so that nothing saturates), in every combination the decoder supports. What the pictures look like does not matter;
what matters is that a reference decoder (ffmpeg's) and this OS's decoder read the same streams and make the same pictures. The expected pictures come from ffmpeg.

  gen_vc1_synthetic.py out_dir      writes out_dir/vc1_<name>.rcv (Simple and Main profile, "RCV" files) and out_dir/vc1_<name>.vc1 (Advanced profile elementary streams)
  gen_vc1_synthetic.py --selftest out.inc
                                    writes the streams of the VCN VC-1 self-test (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc) as C++ arrays

The variable length codes are read from src/Hardware/VideoCodec/Software/Vc1Tables.inc, the decoder's own tables: a reference decoder that reads the streams the way the
standard says proves them right. The generator mirrors the parts of the decoder state that decide which bits are present (coded block flag prediction, the hybrid motion
vector prediction bit, the escape code lengths, the transform type state).
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
                 postproc=0, broadcast=0, tfcntr=0, hrd=0, resync=0, no_ic=0)
        f.update(flags)
        self.f = f
        self.rnd = 0
        self.res_x8 = 0

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
        b.put(f['broadcast'], 1); b.put(0, 1); b.put(f['tfcntr'], 1); b.put(f['finterp'], 1); b.put(0, 1); b.put(0, 1)
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
        b.put(0, 1); b.put(1, 1); b.put(0, 1); b.put(0, 1)
        b.put(f['loop_filter'], 1); b.put(f['fastuvmc'], 1); b.put(f['extended_mv'], 1); b.put(f['dquant'], 2); b.put(f['vstransform'], 1); b.put(f['overlap'], 1)
        b.put(f['quantizer'], 2)
        if f['hrd']:
            for _ in range(2):
                b.put(255, 8)
        b.put(1 if size_override else 0, 1)
        if size_override:
            b.put((self.w >> 1) - 1, 12); b.put((self.h >> 1) - 1, 12)
        if f['extended_mv']:
            b.put(0, 1)
        b.put(0, 1); b.put(0, 1)
        return b.data(stop_bit=True)

    # ---- bit planes (8.7)
    def put_plane(self, b, plane, mode, invert, w=None, h=None):
        """Writes `plane` (rows of 0/1, mbh rows of mbw) with IMODE `mode` (0 raw ... 6 column skip) and the invert bit."""
        w = self.mbw if w is None else w
        h = self.mbh if h is None else h
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
        return [[1 if self.rng.random() < density else 0 for _ in range(self.mbw)] for _ in range(self.mbh)]

    def plane_with_random_mode(self, b, density):
        """Writes a random plane in a random mode; returns (plane, raw)."""
        plane = self.random_plane(density)
        mode = self.rng.choice([0, 1, 2, 3, 4, 5, 6])
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
                b.put(0, 1)
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
            pl['direct'] = self.plane_with_random_mode(b, float(os.environ.get('VC1_BDIRECT', 0.3)))
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
        if (edges & 8) and mby == self.mbh - 1:
            mquant = -p['altpq']
        if mquant == 0 or mquant > 31 or mquant < -31:
            mquant = 1
        return mquant

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
        for mby in range(mbh):
            for mbx in range(mbw):
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
        first = mby == 0
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
        for mby in range(mbh):
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
                a_av = self.intra_flag(mbx, mby, bx, by - 1) if (i in (2, 3) or mby > 0) else 0
                c_av = self.intra_flag(mbx, mby, bx - 1, by) if (i in (1, 3) or mbx > 0) else 0
            else:
                a_av = ((self.intra_mask[(mbx, mby - 1)] >> i) & 1) if mby > 0 else 0
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
        if os.environ.get('VC1_DEBUG'):
            sys.stderr.write('picture %s\n' % {k: v for k, v in p.items() if k not in ('planes',)})
        if p['skipped']:
            pass
        elif p['type'] in ('I', 'BI'):
            self.body_intra_picture(b, p)
        else:
            self.body_inter_picture(b, p)
        if b.tell() % 8 == 0:   # (ffmpeg wants a spare bit after the last macroblock when a bit plane is raw)
            b.put(0, 1)
        return b.data()


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
    return enc_m.struct_c(), [d for d, _ in frames_m], units


def write_selftest(path):
    config, main, adv = selftest_streams()

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
    open(path, 'w').write(text)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--selftest':
        write_selftest(sys.argv[2])
        return
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, (seed, profile, w, h, n, flags) in STREAMS.items():
        enc, frames = BUILDERS[name](seed, profile, w, h, n, **flags)
        if profile == 3:
            write_adv(os.path.join(out, 'vc1_%s.vc1' % name), enc, frames)
        else:
            write_rcv(os.path.join(out, 'vc1_%s.rcv' % name), enc, frames)


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
}
BUILDERS = {name: (stream_pattern if 'pattern' in STREAMS[name][5] else stream_i_only) for name in STREAMS}

if __name__ == '__main__':
    main()
