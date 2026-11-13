#!/usr/bin/env python3
# Makes H.263 streams with B pictures for tests/VideoCodecTest.cpp, which no encoder at hand writes (FFmpeg skips the B part of PB-frames and does not decode
# Annex O's B pictures): PB-frames (Annex G), Improved PB-frames (Annex M) and B pictures (Annex O). This script is an encoder with its own model of the
# decoding process, written from the text of ITU-T H.263 (01/2005) - 6.1 and Annexes G, M and O - and not from the decoder: it chooses random macroblock
# types, vectors and coefficients, writes them, and reconstructs every picture as the Recommendation says, so its pictures (*.yuv, display order) are what a
# decoder must make. 128x96 (sub-QCIF), 8 x 6 macroblocks. The VLC tables come from src/Hardware/VideoCodec/Software/Mpeg4Tables.inc.
# Usage: scripts/gen_h263_bframe_streams.py   (writes tests/data/h263dec_pb.*, h263dec_ipb.*, h263dec_b.*)
import random
import re

INC = 'src/Hardware/VideoCodec/Software/Mpeg4Tables.inc'
text = open(INC).read()


def array(name):
    m = re.search(name + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]


def table(name):
    m = re.search(r'constexpr \w+ ' + name + r'\s*=\s*\{(.*?)\};', text, re.S)
    groups = re.findall(r'\{([^{}]*)\}', m.group(1))
    return [[int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', g)] for g in groups]


INTRA_MCBPC = list(zip(array('kIntraMcbpcCode'), array('kIntraMcbpcBits')))
INTER_MCBPC = list(zip(array('kInterMcbpcCode'), array('kInterMcbpcBits')))
CBPY = list(zip(array('kCbpyCode'), array('kCbpyBits')))
MV = list(zip(array('kMvCode'), array('kMvBits')))
COEF = table('kInterCoefficients')   # code, bits, run, level (last for index >= 58)
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50,
          43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]
W, H, MBW, MBH = 128, 96, 8, 6


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def code(self, pair):
        self.put(pair[0], pair[1])

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def bytes(self):
        self.align()
        return bytes(int(''.join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def cdiv(a, b):   # C division: truncation toward zero
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


# ---- the decoding process (the Recommendation's, written here independently) ----
W1, W2, W3, W4, W5, W6, W7 = 22725, 21407, 19266, 16383, 12873, 8867, 4520


def idct(block):
    """The 'simple' integer IDCT (as the decoder and FFmpeg use: the Recommendation allows any IEEE 1180 conforming one)."""
    t = [0] * 64
    for r in range(8):
        x = [max(-2048, min(2047, block[r * 8 + c])) for c in range(8)]
        if not any(x[1:]):
            for c in range(8):
                t[r * 8 + c] = x[0] * 8
            continue
        a0 = W4 * x[0] + (1 << 10)
        a1 = a0 + W6 * x[2] - W4 * x[4] - W2 * x[6]
        a2 = a0 - W6 * x[2] - W4 * x[4] + W2 * x[6]
        a3 = a0 - W2 * x[2] + W4 * x[4] - W6 * x[6]
        a0 = a0 + W2 * x[2] + W4 * x[4] + W6 * x[6]
        b0 = W1 * x[1] + W3 * x[3] + W5 * x[5] + W7 * x[7]
        b1 = W3 * x[1] - W7 * x[3] - W1 * x[5] - W5 * x[7]
        b2 = W5 * x[1] - W1 * x[3] + W7 * x[5] + W3 * x[7]
        b3 = W7 * x[1] - W5 * x[3] + W3 * x[5] - W1 * x[7]
        row = [(a0 + b0) >> 11, (a1 + b1) >> 11, (a2 + b2) >> 11, (a3 + b3) >> 11, (a3 - b3) >> 11, (a2 - b2) >> 11, (a1 - b1) >> 11, (a0 - b0) >> 11]
        t[r * 8:r * 8 + 8] = row
    out = [0] * 64
    for c in range(8):
        col = [t[r * 8 + c] for r in range(8)]
        a0 = W4 * (col[0] + 32)
        a1 = a0 + W6 * col[2] - W4 * col[4] - W2 * col[6]
        a2 = a0 - W6 * col[2] - W4 * col[4] + W2 * col[6]
        a3 = a0 - W2 * col[2] + W4 * col[4] - W6 * col[6]
        a0 = a0 + W2 * col[2] + W4 * col[4] + W6 * col[6]
        b0 = W1 * col[1] + W3 * col[3] + W5 * col[5] + W7 * col[7]
        b1 = W3 * col[1] - W7 * col[3] - W1 * col[5] - W5 * col[7]
        b2 = W5 * col[1] - W1 * col[3] + W7 * col[5] + W3 * col[7]
        b3 = W7 * col[1] - W5 * col[3] + W3 * col[5] - W1 * col[7]
        vals = [(a0 + b0) >> 20, (a1 + b1) >> 20, (a2 + b2) >> 20, (a3 + b3) >> 20, (a3 - b3) >> 20, (a2 - b2) >> 20, (a1 - b1) >> 20, (a0 - b0) >> 20]
        for r in range(8):
            out[r * 8 + c] = vals[r]
    return out


def clip(v):
    return 0 if v < 0 else 255 if v > 255 else v


class Picture:
    def __init__(self):
        self.p = [[0] * (W * H), [0] * (W * H // 4), [0] * (W * H // 4)]

    def size(self, c):
        return (W, H) if c == 0 else (W // 2, H // 2)

    def get(self, c, x, y):
        w, h = self.size(c)
        return self.p[c][min(max(y, 0), h - 1) * w + min(max(x, 0), w - 1)]

    def half(self, c, x2, y2):   # 6.1.2, Figure 13, RCONTROL = 0
        x, y, fx, fy = x2 >> 1, y2 >> 1, x2 & 1, y2 & 1
        a = self.get(c, x, y)
        if not fx and not fy:
            return a
        if fx and not fy:
            return (a + self.get(c, x + 1, y) + 1) // 2
        if not fx:
            return (a + self.get(c, x, y + 1) + 1) // 2
        return (a + self.get(c, x + 1, y) + self.get(c, x, y + 1) + self.get(c, x + 1, y + 1) + 2) // 4

    def set(self, c, x, y, v):
        w, _ = self.size(c)
        self.p[c][y * w + x] = v

    def copy(self):
        q = Picture()
        q.p = [list(a) for a in self.p]
        return q

    def bytes(self):
        return bytes(self.p[0] + self.p[1] + self.p[2])


ROUND_CHROMA = [0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2]   # Table F.1 / 16, as the sum of four vectors' sixteenth position


def chroma_of_sum(s):
    """Table F.1: the sum of four luma vectors / 8, to the nearest half sample (towards the half position)."""
    sign = -1 if s < 0 else 1
    a = abs(s)
    return sign * ((a // 16) * 2 + ROUND_CHROMA[a % 16])


def chroma_of_one(v):
    """Table 16 (5.3.7 / 6.1.1): one luma vector / 2, quarter positions to the half position."""
    sign = -1 if v < 0 else 1
    a = abs(v)
    return sign * ((a // 4) * 2 + (0 if a % 4 == 0 else 1))


def predict_block(ref, c, x0, y0, size, vx, vy):
    """The size x size prediction at (x0, y0) of component c with a half-sample vector."""
    return [[ref.half(c, (x0 + i) * 2 + vx, (y0 + j) * 2 + vy) for i in range(size)] for j in range(size)]


def predict_mb(ref, mx, my, x, y):
    """A macroblock predicted with one vector: luma 16 x 16, chroma 8 x 8 with the chroma vector of Table 16."""
    cx, cy = chroma_of_one(mx), chroma_of_one(my)
    return [predict_block(ref, 0, x * 16, y * 16, 16, mx, my), predict_block(ref, 1, x * 8, y * 8, 8, cx, cy), predict_block(ref, 2, x * 8, y * 8, 8, cx, cy)]


def average(a, b):   # O.4 / G.5: (forward + backward) / 2, truncated
    return [[(a[j][i] + b[j][i]) // 2 for i in range(len(a[0]))] for j in range(len(a))]


def put_mb(pic, x, y, pred, residual):
    """Writes a macroblock: prediction (None: intra) plus the blocks' residual."""
    for n in range(6):
        c = 0 if n < 4 else n - 3
        bx = x * 16 + (n & 1) * 8 if n < 4 else x * 8
        by = y * 16 + (n >> 1) * 8 if n < 4 else y * 8
        res = idct(residual[n]) if residual[n] is not None else [0] * 64
        for j in range(8):
            for i in range(8):
                if pred is None:
                    v = res[j * 8 + i]
                else:
                    px = (n & 1) * 8 + i if n < 4 else i
                    py = (n >> 1) * 8 + j if n < 4 else j
                    v = pred[c][py][px] + res[j * 8 + i]
                pic.set(c, bx + i, by + j, clip(v))


# ---- syntax ----
rng = random.Random(263)


def put_mvd(b, d):
    """Table 14: a vector difference in half samples, sent modulo 64 (the decoder wraps the vector into [-16, 15.5])."""
    while d > 31:
        d -= 64
    while d < -32:
        d += 64
    b.code(MV[abs(d)])
    if d:
        b.put(1 if d < 0 else 0, 1)


def put_coefficients(b, events, q, intra):
    """TCOEF (Table 16, escape: LAST, RUN 6, LEVEL 8) and the dequantised block (raster order)."""
    block = [0] * 64
    pos = 1 if intra else 0
    for last, run, level in events:
        mag = abs(level)
        index = None
        for k in range(102):
            if COEF[2][k] == run and COEF[3][k] == mag and (k >= 58) == bool(last):
                index = k
                break
        if index is None:
            b.code((COEF[0][102], COEF[1][102]))
            b.put(last, 1)
            b.put(run, 6)
            b.put(level & 0xFF, 8)
        else:
            b.code((COEF[0][index], COEF[1][index]))
            b.put(1 if level < 0 else 0, 1)
        pos += run
        value = 2 * q * mag + ((q - 1) | 1)   # 6.2.1: |REC| = QUANT * (2 * |LEVEL| + 1) (- 1 when QUANT is even)
        block[ZIGZAG[pos]] = value if level > 0 else -value
        pos += 1
    return block


def random_events(intra):
    events, pos = [], 1 if intra else 0
    while True:
        run = rng.choice([0, 0, 1, 2, 4])
        if pos + run > 63:
            run = 63 - pos
        pos += run + 1
        level = rng.choice([1, 1, 2, 3, -1, -2, 5, -9])
        last = pos >= 64 or rng.random() < 0.35
        events.append((1 if last else 0, run, level))
        if last:
            return events


def picture_header(b, tr, ptype_inter, quant, pb=False, trb=0, dbquant=0, plus=None):
    b.align()
    b.put(0x20, 22)   # PSC
    b.put(tr & 0xFF, 8)
    b.put(1, 1)
    b.put(0, 1)
    b.put(0, 3)
    if plus is None:
        b.put(1, 3)   # sub-QCIF
        b.put(1 if ptype_inter else 0, 1)
        b.put(0, 1)   # UMV
        b.put(0, 1)   # SAC
        b.put(0, 1)   # AP
        b.put(1 if pb else 0, 1)
        b.put(quant, 5)
        b.put(0, 1)   # CPM
    else:
        b.put(7, 3)   # PLUSPTYPE
        b.put(1, 3)   # UFEP: the full OPPTYPE
        b.put(1, 3)   # sub-QCIF
        b.put(0, 11)  # custom PCF, UMV, SAC, AP, AIC, DF, SS, RPS, ISD, AIV, MQ
        b.put(1, 1)
        b.put(0, 3)
        b.put(plus, 3)   # MPPTYPE: 0 I, 1 P, 2 Improved PB, 3 B
        b.put(0, 3)   # RPR, RRU, RTYPE
        b.put(0, 2)
        b.put(1, 1)
        b.put(0, 1)   # CPM
        if plus == 3:
            b.put(1, 4)   # ELNUM
            b.put(1, 4)   # RLNUM
        b.put(quant, 5)
    if pb:
        b.put(trb, 3)
        b.put(dbquant, 2)
    b.put(0, 1)   # PEI


def predictor(mvs, x, y):
    """6.1.1 for a picture without GOB headers: candidates left, above, above right; outside the picture: rules 2-4."""
    def at(xx, yy):
        return mvs.get((xx, yy), (0, 0))
    if y == 0:
        mv1 = at(x - 1, y) if x > 0 else (0, 0)
        return mv1
    mv1 = at(x - 1, y) if x > 0 else (0, 0)
    mv2 = at(x, y - 1)
    mv3 = at(x + 1, y - 1) if x + 1 < MBW else (0, 0)
    return tuple(sorted([mv1[k], mv2[k], mv3[k]])[1] for k in range(2))


def write_intra_blocks(b, q, cbp):
    """INTRADC then TCOEF for each block of an intra macroblock; returns the dequantised blocks."""
    dcs = [rng.choice([rng.randint(1, 127), rng.randint(129, 254), 255]) for _ in range(6)]
    blocks = []
    for n in range(6):
        b.put(dcs[n], 8)
        block = put_coefficients(b, random_events(True), q, True) if cbp & (32 >> n) else [0] * 64
        block[0] = (128 if dcs[n] == 255 else dcs[n]) * 8
        blocks.append(block)
    return blocks


def write_inter_blocks(b, q, cbp):
    return [put_coefficients(b, random_events(False), q, False) if cbp & (32 >> n) else None for n in range(6)]


def choose_vector(pred):
    while True:
        v = (max(-30, min(30, pred[0] + rng.randint(-6, 6))), max(-30, min(30, pred[1] + rng.randint(-6, 6))))
        if all(abs(v[k] - pred[k]) <= 15 for k in range(2)):
            return v


def i_picture(b, tr, plus):
    q = 6
    picture_header(b, tr, False, q, plus=0 if plus else None)
    pic = Picture()
    for y in range(MBH):
        for x in range(MBW):
            cbpc, cbpy = rng.randint(0, 3), rng.randint(0, 15)
            b.code(INTRA_MCBPC[cbpc])
            b.code(CBPY[cbpy])
            put_mb(pic, x, y, None, write_intra_blocks(b, q, cbpc | (cbpy << 2)))
    return pic


def p_picture(b, prev, tr, plus, pb=0, trb=0, trd=2, dbquant=0):
    """A P picture, or with pb = 1 / 2 a PB-frame (Annex G) / Improved PB-frame (Annex M): returns (P, B part)."""
    q = 5
    if pb == 2:
        picture_header(b, tr, True, q, pb=True, trb=trb, dbquant=dbquant, plus=2)
    else:
        picture_header(b, tr, True, q, pb=pb == 1, trb=trb, dbquant=dbquant, plus=1 if plus else None)
    bq = min(31, (5 + dbquant) * q // 4)
    pic, bpic = Picture(), Picture()
    mvs, forward_left = {}, {}
    for y in range(MBH):
        for x in range(MBW):
            kind = rng.choice(['skip', 'inter', 'inter', 'intra'])
            pred = predictor(mvs, x, y)
            if kind == 'skip':
                b.put(1, 1)
                mvs[(x, y)] = (0, 0)
                mb_mv, residual, p_pred = (0, 0), [None] * 6, predict_mb(prev, 0, 0, x, y)
                put_mb(pic, x, y, p_pred, residual)
                b_mode, mvd, cbpb, b_fw = 'bi', (0, 0), 0, None
            else:
                cbpc, cbpy = rng.randint(0, 3), rng.randint(0, 15)
                b.put(0, 1)
                b.code(INTER_MCBPC[(4 if kind == 'intra' else 0) + cbpc])
                # MODB (Table 11 / Table M.1), CBPB
                b_mode, has_mvdb, cbpb = 'bi', False, 0
                if pb == 1:
                    choice = rng.randint(0, 2)
                    if choice == 0:
                        b.put(0, 1)
                    else:
                        b.put(2 if choice == 1 else 3, 2)
                        has_mvdb = True
                        if choice == 2:
                            cbpb = rng.randint(0, 63)
                            b.put(cbpb, 6)
                elif pb == 2:
                    index = rng.randint(0, 5)
                    b_mode = ['bi', 'bi', 'fw', 'fw', 'bw', 'bw'][index]
                    b.put(((1 << index) - 1) << 1 if index < 5 else 31, index + 1 if index < 5 else 5)
                    has_mvdb = index in (2, 3)
                    if index in (1, 3, 5):
                        cbpb = rng.randint(0, 63)
                        b.put(cbpb, 6)
                b.code(CBPY[cbpy if kind == 'intra' else cbpy ^ 15])
                need_mv = kind == 'inter' or (pb == 1) or (pb == 2 and b_mode == 'bi')
                mb_mv = (0, 0)
                if need_mv:
                    mb_mv = choose_vector(pred)
                    put_mvd(b, mb_mv[0] - pred[0])
                    put_mvd(b, mb_mv[1] - pred[1])
                mvs[(x, y)] = mb_mv if (kind == 'inter' or pb) else (0, 0)
                mvd, b_fw = (0, 0), None
                if has_mvdb and pb == 1:
                    pc = [cdiv(trb * mb_mv[k], trd) for k in range(2)]
                    mvd = (rng.randint(-3, 3), rng.randint(-3, 3))
                    put_mvd(b, mvd[0])
                    put_mvd(b, mvd[1])
                elif has_mvdb and pb == 2:
                    fp = forward_left.get((x - 1, y), (0, 0)) if x > 0 else (0, 0)
                    b_fw = (max(-30, min(30, fp[0] + rng.randint(-6, 6))), max(-30, min(30, fp[1] + rng.randint(-6, 6))))
                    put_mvd(b, b_fw[0] - fp[0])
                    put_mvd(b, b_fw[1] - fp[1])
                if kind == 'intra':
                    blocks = write_intra_blocks(b, q, cbpc | (cbpy << 2))
                    put_mb(pic, x, y, None, blocks)
                else:
                    residual = write_inter_blocks(b, q, cbpc | (cbpy << 2))
                    put_mb(pic, x, y, predict_mb(prev, mb_mv[0], mb_mv[1], x, y), residual)
                b_residual = write_inter_blocks(b, bq, cbpb) if pb else None
            if b_fw is not None:
                forward_left[(x, y)] = b_fw
            if not pb:
                continue
            if kind == 'skip':
                b_residual = [None] * 6
            # The B part (G.4, G.5, M.2), predicted from `prev` and PREC (this macroblock of `pic`, reconstructed).
            if b_mode == 'bw':
                bpred = [[[pic.get(c, (x * 16 if c == 0 else x * 8) + i, (y * 16 if c == 0 else y * 8) + j) for i in range(16 if c == 0 else 8)]
                          for j in range(16 if c == 0 else 8)] for c in range(3)]
            elif b_mode == 'fw':
                bpred = predict_mb(prev, b_fw[0], b_fw[1], x, y)
            else:
                mv = mb_mv if kind != 'skip' else (0, 0)
                d = mvd if pb == 1 else (0, 0)
                mvf = [cdiv(trb * mv[k], trd) + d[k] for k in range(2)]
                mvb = [cdiv((trb - trd) * mv[k], trd) if d[k] == 0 else mvf[k] - mv[k] for k in range(2)]
                luma = predict_block(prev, 0, x * 16, y * 16, 16, mvf[0], mvf[1])
                for nh in range(2):
                    for nv in range(2):
                        mh, mvv = mvb
                        for i in range(nh * 8 + max(0, cdiv(-mh + 1, 2) - nh * 8), nh * 8 + min(7, 15 - cdiv(mh + 1, 2) - nh * 8) + 1):
                            for j in range(nv * 8 + max(0, cdiv(-mvv + 1, 2) - nv * 8), nv * 8 + min(7, 15 - cdiv(mvv + 1, 2) - nv * 8) + 1):
                                back = pic.half(0, (x * 16 + i) * 2 + mh, (y * 16 + j) * 2 + mvv)
                                luma[j][i] = (luma[j][i] + back) // 2
                mfc = [chroma_of_sum(4 * mvf[k]) for k in range(2)]
                mbc = [chroma_of_sum(4 * mvb[k]) for k in range(2)]
                bpred = [luma]
                for c in (1, 2):
                    ch = predict_block(prev, c, x * 8, y * 8, 8, mfc[0], mfc[1])
                    for i in range(max(0, cdiv(-mbc[0] + 1, 2)), min(7, 7 - cdiv(mbc[0] + 1, 2)) + 1):
                        for j in range(max(0, cdiv(-mbc[1] + 1, 2)), min(7, 7 - cdiv(mbc[1] + 1, 2)) + 1):
                            back = pic.half(c, (x * 8 + i) * 2 + mbc[0], (y * 8 + j) * 2 + mbc[1])
                            ch[j][i] = (ch[j][i] + back) // 2
                    bpred.append(ch)
            put_mb(bpic, x, y, bpred, b_residual)
    return pic, bpic, mvs


def b_picture(b, past, future, future_mvs, tr, trb, trd):
    """Annex O: a B picture between `past` and `future` (whose macroblock vectors are `future_mvs`, None for intra)."""
    q = 7
    picture_header(b, tr, True, q, plus=3)
    pic = Picture()
    fw_mvs, bw_mvs = {}, {}
    # Table O.1: index -> (code, bits)
    codes = {0: (3, 2), 1: (1, 4), 2: (4, 3), 3: (5, 3), 4: (6, 5), 5: (2, 3), 6: (3, 3), 7: (7, 5), 8: (4, 5), 9: (5, 5), 10: (1, 5), 11: (1, 6),
             12: (1, 7)}
    for y in range(MBH):
        for x in range(MBW):
            kind = rng.choice(['skip', 0, 2, 3, 5, 6, 8, 9, 11, 9, 3])
            col = future_mvs.get((x, y))
            colv = col if col is not None else (0, 0)
            direct_f = [cdiv(trb * colv[k], trd) for k in range(2)]
            direct_b = [cdiv((trb - trd) * colv[k], trd) for k in range(2)]
            if kind == 'skip':
                b.put(1, 1)
                put_mb(pic, x, y, [average(f, bb) for f, bb in zip(predict_mb(past, *direct_f, x, y), predict_mb(future, *direct_b, x, y))], [None] * 6)
                continue
            b.put(0, 1)
            b.code(codes[kind])
            coded = kind in (0, 3, 6, 9, 11)
            cbp = 0
            if coded:
                cbpc, cbpy = rng.randint(0, 3), rng.randint(0, 15)
                b.code([(0, 1), (2, 2), (7, 3), (6, 3)][cbpc])   # Table O.4
                b.code(CBPY[cbpy if kind == 11 else cbpy ^ 15])
                cbp = cbpc | (cbpy << 2)
            fwd = bwd = None
            if kind in (2, 3, 8, 9):
                p = predictor(fw_mvs, x, y)
                fwd = choose_vector(p)
                put_mvd(b, fwd[0] - p[0])
                put_mvd(b, fwd[1] - p[1])
                fw_mvs[(x, y)] = fwd
            if kind in (5, 6, 8, 9):
                p = predictor(bw_mvs, x, y)
                bwd = choose_vector(p)
                put_mvd(b, bwd[0] - p[0])
                put_mvd(b, bwd[1] - p[1])
                bw_mvs[(x, y)] = bwd
            if kind == 11:
                put_mb(pic, x, y, None, write_intra_blocks(b, q, cbp))
                continue
            residual = write_inter_blocks(b, q, cbp)
            if kind == 0:
                pred = [average(f, bb) for f, bb in zip(predict_mb(past, *direct_f, x, y), predict_mb(future, *direct_b, x, y))]
            elif fwd and bwd:
                pred = [average(f, bb) for f, bb in zip(predict_mb(past, *fwd, x, y), predict_mb(future, *bwd, x, y))]
            elif fwd:
                pred = predict_mb(past, *fwd, x, y)
            else:
                pred = predict_mb(future, *bwd, x, y)
            put_mb(pic, x, y, pred, residual)
    return pic


def write(name, data, pictures):
    open('tests/data/%s.h263' % name, 'wb').write(data)
    open('tests/data/%s.yuv' % name, 'wb').write(b''.join(p.bytes() for p in pictures))


# Annex G: I, then four PB-frames (TR + 2, TRB 1)
b = Bits()
shown = []
prev = i_picture(b, 0, False)
shown.append(prev)
for k in range(1, 5):
    p, bp, _ = p_picture(b, prev, 2 * k, False, pb=1, trb=1, trd=2, dbquant=k % 4)
    shown += [bp, p]
    prev = p
write('h263dec_pb', b.bytes(), shown)

# Annex M: I, then four Improved PB-frames
b = Bits()
shown = []
prev = i_picture(b, 0, True)
shown.append(prev)
for k in range(1, 5):
    p, bp, _ = p_picture(b, prev, 2 * k, True, pb=2, trb=1, trd=2, dbquant=(k + 1) % 4)
    shown += [bp, p]
    prev = p
write('h263dec_ipb', b.bytes(), shown)

# Annex O: I0 P2 B1 P4 B3 (P pictures record their vectors for direct mode)
b = Bits()
i0 = i_picture(b, 0, True)
p2, _, v2 = p_picture(b, i0, 2, True)
b1 = b_picture(b, i0, p2, v2, 1, 1, 2)
p4, _, v4 = p_picture(b, p2, 4, True)
b3 = b_picture(b, p2, p4, v4, 3, 1, 2)
write('h263dec_b', b.bytes(), [i0, b1, p2, b3, p4])
print('wrote tests/data/h263dec_pb, h263dec_ipb, h263dec_b (.h263, .yuv)')
