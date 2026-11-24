#!/usr/bin/env python3
# Writes tests/data/hevc10_pcm.h265: a small Main 10 stream with the tools x265 cannot make - PCM coding units (9-bit luma, 7-bit chroma PCM samples in a 10-bit
# picture, with the loop filters over them in the first picture and pcm_loop_filter_disabled_flag in the second) - next to intra coding units with 4x4 residuals at
# quantisers below zero (QpY down to -QpBdOffsetY = -12) and up to 51 (cu_qp_delta across the whole range, wrapping), transform skip, cu_transquant_bypass, NxN
# partitions, every intra mode, chroma residuals with chroma QP offsets, sample adaptive offset with the 10-bit offset range (0..31) and the deblocking filter.
#
# It is a tiny H.265 encoder: it writes the parameter sets and slice data with a CABAC encoder (the context initialisation from src/Hardware/VideoCodec/Software/
# H265CabacInit.inc). The coefficient levels and modes are chosen at random (fixed seed), not by searching: the stream is legal, the pictures need not look like
# anything. FFmpeg's decode of it is the reference (scripts/gen_hevc10_test_streams.sh writes tests/data/hevc10_pcm.md5).
# Usage: python3 -I scripts/gen_hevc10_pcm_stream.py [out.h265]
import os
import random
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 128, 64
CTB_LOG2, MIN_CB_LOG2, MAX_TB_LOG2 = 4, 3, 4
BIT_DEPTH = 10
PCM_LUMA, PCM_CHROMA = 9, 7

# context variables (the Ctx enumeration of H265Cabac.hpp)
CTX = dict(sao_merge=0, sao_type=1, split_cu=2, bypass=5, cu_qp_delta=9, part_mode=12, prev_intra=16, chroma_pred=17, split_tf=29, cbf_luma=32, cbf_chroma=34,
           tskip=40, last_x=42, last_y=60, sig=82, gt1=124, gt2=148)
with open(os.path.join(ROOT, "src/Hardware/VideoCodec/Software/H265CabacInit.inc")) as f:
    rows = re.findall(r"\{([0-9, ]+)\}", f.read())
INIT_I = [int(v) for v in rows[0].split(",")]

RANGE_LPS = [[128,176,208,240],[128,167,197,227],[128,158,187,216],[123,150,178,205],[116,142,169,195],[111,135,160,185],[105,128,152,175],[100,122,144,166],
             [95,116,137,158],[90,110,130,150],[85,104,123,142],[81,99,117,135],[77,94,111,128],[73,89,105,122],[69,85,100,116],[66,80,95,110],[62,76,90,104],[59,72,86,99],
             [56,69,81,94],[53,65,77,89],[51,62,73,85],[48,59,69,80],[46,56,66,76],[43,53,63,72],[41,50,59,69],[39,48,56,65],[37,45,54,62],[35,43,51,59],[33,41,48,56],
             [32,39,46,53],[30,37,43,50],[29,35,41,48],[27,33,39,45],[26,31,37,43],[24,30,35,41],[23,28,33,39],[22,27,32,37],[21,26,30,35],[20,24,29,33],[19,23,27,31],
             [18,22,26,30],[17,21,25,28],[16,20,23,27],[15,19,22,25],[14,18,21,24],[14,17,20,23],[13,16,19,22],[12,15,18,21],[12,14,17,20],[11,14,16,19],[11,13,15,18],
             [10,12,15,17],[10,12,14,16],[9,11,13,15],[9,11,12,14],[8,10,12,14],[8,9,11,13],[7,9,11,12],[7,9,10,12],[7,8,10,11],[6,8,9,11],[6,7,9,10],[6,7,8,9],[2,2,2,2]]
TRANS_LPS = [0,0,1,2,2,4,4,5,6,7,8,9,9,11,11,12,13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,33,33,34,34,35,35,
             35,36,36,36,37,37,37,38,38,63]
TRANS_MPS = [min(i + 1, 62) for i in range(63)] + [63]


class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(0, n - 1)
        self.u(v, n)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def align_zero(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def trailing(self):
        self.bits.append(1)
        self.align_zero()

    def bytes(self):
        assert len(self.bits) % 8 == 0
        return bytes(int("".join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


class CabacEncoder:
    def __init__(self, bw, slice_qp):
        self.bw = bw
        qp = max(0, min(51, slice_qp))
        self.state = []
        for v in INIT_I:
            m, n = (v >> 4) * 5 - 45, ((v & 15) << 3) - 16
            pre = max(1, min(126, ((m * qp) >> 4) + n))
            self.state.append([pre - 64, 1] if pre > 63 else [63 - pre, 0])
        self.start()

    def start(self):
        self.low, self.range, self.first, self.outstanding = 0, 510, True, 0

    def put(self, b):
        if self.first:
            self.first = False
        else:
            self.bw.bits.append(b)
        while self.outstanding:
            self.bw.bits.append(1 - b)
            self.outstanding -= 1

    def renorm(self):
        while self.range < 256:
            if self.low < 256:
                self.put(0)
            elif self.low >= 512:
                self.low -= 512
                self.put(1)
            else:
                self.low -= 256
                self.outstanding += 1
            self.range <<= 1
            self.low <<= 1

    def decision(self, ctx, b):
        st = self.state[ctx]
        lps = RANGE_LPS[st[0]][(self.range >> 6) & 3]
        self.range -= lps
        if b != st[1]:
            self.low += self.range
            self.range = lps
            if st[0] == 0:
                st[1] = 1 - st[1]
            st[0] = TRANS_LPS[st[0]]
        else:
            st[0] = TRANS_MPS[st[0]]
        self.renorm()

    def bypass(self, b):
        self.low <<= 1
        if b:
            self.low += self.range
        if self.low >= 1024:
            self.put(1)
            self.low -= 1024
        elif self.low < 512:
            self.put(0)
        else:
            self.low -= 512
            self.outstanding += 1

    def bypass_bits(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bypass((v >> i) & 1)

    def terminate(self, b):
        self.range -= 2
        if b:
            self.low += self.range
            self.range = 2
            self.renorm()
            self.put((self.low >> 9) & 1)
            self.bw.u(((self.low >> 7) & 3) | 1, 2)
        else:
            self.renorm()

    def eg(self, v, k):
        while v >= (1 << k):
            self.bypass(1)
            v -= 1 << k
            k += 1
        self.bypass(0)
        self.bypass_bits(v, k)


def nal(type_, payload):
    out = bytearray([0, 0, 0, 1, type_ << 1, 1])
    zeros = 0
    for b in payload:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def profile_tier_level(bw):
    bw.u(0, 2); bw.u(0, 1); bw.u(2, 5)    # profile space, tier, general_profile_idc 2: Main 10
    bw.u(0x20000000, 32)                    # general_profile_compatibility_flag[2]
    bw.u(1, 1); bw.u(0, 1); bw.u(0, 1); bw.u(1, 1)   # progressive, not interlaced, not non-packed, frame only
    bw.u(0, 43); bw.u(0, 1)
    bw.u(60, 8)                             # level 2


def vps():
    bw = BitWriter()
    bw.u(0, 4); bw.u(1, 1); bw.u(1, 1); bw.u(0, 6); bw.u(0, 3); bw.u(1, 1); bw.u(0xFFFF, 16)
    profile_tier_level(bw)
    bw.u(1, 1); bw.ue(0); bw.ue(0); bw.ue(0)
    bw.u(0, 6); bw.ue(0); bw.u(0, 1); bw.u(0, 1)
    bw.trailing()
    return nal(32, bw.bytes())


def sps(sps_id, pcm_loop_filter_disabled):
    bw = BitWriter()
    bw.u(0, 4); bw.u(0, 3); bw.u(1, 1)
    profile_tier_level(bw)
    bw.ue(sps_id); bw.ue(1); bw.ue(W); bw.ue(H); bw.u(0, 1)
    bw.ue(BIT_DEPTH - 8); bw.ue(BIT_DEPTH - 8); bw.ue(4)
    bw.u(1, 1); bw.ue(0); bw.ue(0); bw.ue(0)
    bw.ue(MIN_CB_LOG2 - 3); bw.ue(CTB_LOG2 - MIN_CB_LOG2); bw.ue(0); bw.ue(MAX_TB_LOG2 - 2)
    bw.ue(0); bw.ue(1)                      # max_transform_hierarchy_depth inter 0, intra 1
    bw.u(0, 1); bw.u(0, 1); bw.u(1, 1)      # no scaling lists, no AMP, SAO
    bw.u(1, 1); bw.u(PCM_LUMA - 1, 4); bw.u(PCM_CHROMA - 1, 4); bw.ue(0); bw.ue(1); bw.u(pcm_loop_filter_disabled, 1)   # PCM 8x8 .. 16x16
    bw.ue(0); bw.u(0, 1); bw.u(0, 1); bw.u(1, 1)    # no RPS, no long-term pictures, no temporal MVP, strong intra smoothing
    bw.u(0, 1); bw.u(0, 1)
    bw.trailing()
    return nal(33, bw.bytes())


def pps(pps_id, sps_id, init_qp, cb, cr, beta, tc):
    bw = BitWriter()
    bw.ue(pps_id); bw.ue(sps_id); bw.u(0, 1); bw.u(0, 1); bw.u(0, 3); bw.u(0, 1); bw.u(0, 1)
    bw.ue(0); bw.ue(0); bw.se(init_qp - 26)
    bw.u(0, 1); bw.u(1, 1); bw.u(1, 1); bw.ue(1)     # transform skip; cu_qp_delta with quantization groups of 8x8
    bw.se(cb); bw.se(cr); bw.u(0, 1); bw.u(0, 1); bw.u(0, 1)
    bw.u(1, 1)                                         # transquant bypass
    bw.u(0, 1); bw.u(0, 1); bw.u(0, 1)
    bw.u(1, 1); bw.u(0, 1); bw.u(0, 1); bw.se(beta); bw.se(tc)
    bw.u(0, 1); bw.u(0, 1); bw.ue(0); bw.u(0, 1); bw.u(0, 1)
    bw.trailing()
    return nal(34, bw.bytes())


def diag_scan4():
    out, x, y = [], 0, 0
    while len(out) < 16:
        while y >= 0:
            if x < 4 and y < 4:
                out.append((x, y))
            y -= 1
            x += 1
        y, x = x, 0
    return out


SCANS = [diag_scan4(), [(x, y) for y in range(4) for x in range(4)], [(x, y) for x in range(4) for y in range(4)]]
SIG_MAP = [0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8]


class Picture:
    def __init__(self, rng, slice_qp, pcm_lf_disabled):
        self.rng = rng
        self.slice_qp = slice_qp
        self.pcm_lf_disabled = pcm_lf_disabled
        self.w4, self.h4 = W // 4, H // 4
        self.depth = {}      # per 8x8 (min CB) block: the coding quadtree depth
        self.mode = {}       # per 4x4 block: the luma intra mode (DC for PCM)
        self.qp_bd = 6 * (BIT_DEPTH - 8)

    def encode(self, pps_id):
        bw = BitWriter()
        bw.u(1, 1); bw.u(0, 1); bw.ue(pps_id); bw.ue(2)    # first slice segment, no_output_of_prior_pics, pps, I slice
        bw.u(1, 1); bw.u(1, 1)                              # SAO luma and chroma
        bw.se(self.slice_qp - self.init_qp)
        bw.u(1, 1); bw.align_zero()
        self.cabac = CabacEncoder(bw, self.slice_qp)
        ctb_w, ctb_h = (W + 15) >> 4, (H + 15) >> 4
        self.sao_params = {}
        for ry in range(ctb_h):
            for rx in range(ctb_w):
                self.sao(rx, ry)
                self.quadtree(rx << 4, ry << 4, CTB_LOG2, 0)
                self.cabac.terminate(1 if (rx, ry) == (ctb_w - 1, ctb_h - 1) else 0)
        bw.align_zero()
        return nal(20, bw.bytes())

    def sao(self, rx, ry):
        c, r = self.cabac, self.rng
        if rx > 0:
            merge = r.random() < 0.2
            c.decision(CTX["sao_merge"], int(merge))
            if merge:
                return
        if ry > 0:
            merge = r.random() < 0.2
            c.decision(CTX["sao_merge"], int(merge))
            if merge:
                return
        cmax = (1 << (min(BIT_DEPTH, 10) - 5)) - 1
        for comp in range(3):
            if comp < 2:
                kind = r.choice([0, 1, 2, 2])
                c.decision(CTX["sao_type"], int(kind != 0))
                if kind:
                    c.bypass(int(kind == 2))
                last_kind = kind
            kind = last_kind
            if not kind:
                continue
            offsets = [r.choice([0, 1, 3, 7, 12, 20, 31]) for _ in range(4)]
            for v in offsets:
                for _ in range(v):
                    c.bypass(1)
                if v < cmax:
                    c.bypass(0)
            if kind == 1:
                for v in offsets:
                    if v:
                        c.bypass(r.randrange(2))
                c.bypass_bits(r.randrange(32), 5)
            elif comp < 2:
                c.bypass_bits(r.randrange(4), 2)

    def quadtree(self, x0, y0, log2, depth):
        if x0 >= W or y0 >= H:
            return
        size = 1 << log2
        if log2 >= 3:
            self.qp_delta_coded = False     # a quantization group starts (Log2MinCuQpDeltaSize 3)
        if x0 + size <= W and y0 + size <= H and log2 > MIN_CB_LOG2:
            split = self.rng.random() < 0.5
            inc = 0
            if x0 > 0 and self.depth[((x0 - 1) >> 3, y0 >> 3)] > depth:
                inc += 1
            if y0 > 0 and self.depth[(x0 >> 3, (y0 - 1) >> 3)] > depth:
                inc += 1
            self.cabac.decision(CTX["split_cu"] + inc, int(split))
        else:
            split = log2 > MIN_CB_LOG2
        if split:
            half = size >> 1
            for dy in (0, half):
                for dx in (0, half):
                    self.quadtree(x0 + dx, y0 + dy, log2 - 1, depth + 1)
            return
        for by in range(y0 >> 3, (y0 + size) >> 3):
            for bx in range(x0 >> 3, (x0 + size) >> 3):
                self.depth[(bx, by)] = depth
        self.coding_unit(x0, y0, log2)

    def candidates(self, x, y):
        def neighbour(xn, yn, above):
            if xn < 0 or yn < 0:
                return 1
            if above and yn < ((y >> CTB_LOG2) << CTB_LOG2):
                return 1
            return self.mode[(xn >> 2, yn >> 2)]
        a, b = neighbour(x - 1, y, False), neighbour(x, y - 1, True)
        if a == b:
            return [0, 1, 26] if a < 2 else [a, 2 + ((a + 29) % 32), 2 + ((a - 2 + 1) % 32)]
        c = 0 if 0 not in (a, b) else (1 if 1 not in (a, b) else 26)
        return [a, b, c]

    def set_mode(self, x, y, size, m):
        for yy in range(y >> 2, (y + size) >> 2):
            for xx in range(x >> 2, (x + size) >> 2):
                self.mode[(xx, yy)] = m

    def coding_unit(self, x0, y0, log2):
        c, r = self.cabac, self.rng
        size = 1 << log2
        self.bypass = r.random() < 0.15
        c.decision(CTX["bypass"], int(self.bypass))
        nxn = False
        if log2 == MIN_CB_LOG2:
            nxn = r.random() < 0.4
            c.decision(CTX["part_mode"], int(not nxn))
        if not nxn and log2 <= 4 and r.random() < 0.35:
            c.terminate(1)
            self.pcm(x0, y0, size)
            self.set_mode(x0, y0, size, 1)
            return
        if not nxn:
            c.terminate(0)
        # luma modes: every PU's flag first, then its index or remaining mode (each PU's candidates come from the PUs before it)
        pus = [(x0, y0), (x0 + 4, y0), (x0, y0 + 4), (x0 + 4, y0 + 4)] if nxn else [(x0, y0)]
        pu_size = 4 if nxn else size
        coded = []
        for (x, y) in pus:
            want = r.randrange(35)
            cand = self.candidates(x, y)
            if r.random() < 0.4:
                want = cand[r.randrange(3)]
            self.set_mode(x, y, pu_size, want)
            coded.append((want in cand, cand, want))
        for in_list, _, _ in coded:
            c.decision(CTX["prev_intra"], int(in_list))
        for in_list, cand, want in coded:
            if in_list:
                idx = cand.index(want)
                c.bypass(int(idx > 0))
                if idx > 0:
                    c.bypass(int(idx > 1))
            else:
                c.bypass_bits(want - sum(1 for v in cand if v < want), 5)
        chroma = r.randrange(5)
        if chroma == 4:
            c.decision(CTX["chroma_pred"], 0)
        else:
            c.decision(CTX["chroma_pred"], 1)
            c.bypass_bits(chroma, 2)
        luma0 = self.mode[(x0 >> 2, y0 >> 2)]
        if chroma == 4:
            self.chroma_mode = luma0
        else:
            self.chroma_mode = [0, 26, 10, 1][chroma]
            if self.chroma_mode == luma0:
                self.chroma_mode = 34
        self.transform_tree(x0, y0, x0, y0, log2, 0, 0, [1, 1], nxn)

    def pcm(self, x0, y0, size):
        bw, r = self.cabac.bw, self.rng
        bw.align_zero()
        for y in range(size):
            for x in range(size):
                base = ((x0 + x) * 5 + (y0 + y) * 3) % (1 << PCM_LUMA)
                bw.u(max(0, min((1 << PCM_LUMA) - 1, base + r.randrange(-20, 21))), PCM_LUMA)
        for comp in range(2):
            for y in range(size // 2):
                for x in range(size // 2):
                    bw.u(r.randrange(1 << PCM_CHROMA) if r.random() < 0.3 else (x * 9 + y * 4 + comp * 40) % (1 << PCM_CHROMA), PCM_CHROMA)
        self.cabac.start()

    def transform_tree(self, x0, y0, xb, yb, log2, depth, blk, parent_cbf, nxn):
        c, r = self.cabac, self.rng
        max_depth = 1 + (1 if nxn else 0)
        if log2 <= MAX_TB_LOG2 and log2 > 2 and depth < max_depth and not (nxn and depth == 0):
            split = r.random() < 0.6
            c.decision(CTX["split_tf"] + 5 - log2, int(split))
        else:
            split = nxn and depth == 0
        cbf = [0, 0]
        if log2 > 2:
            for k in range(2):
                if depth == 0 or parent_cbf[k]:
                    # residuals only where the chroma blocks are 4x4: below an 8x8 node, or on it when it is not split further
                    cbf[k] = int(log2 == 3 and r.random() < 0.6) if not (split and log2 == 4) else int(r.random() < 0.7)
                    c.decision(CTX["cbf_chroma"] + depth, cbf[k])
        else:
            cbf = parent_cbf
        if split:
            half = 1 << (log2 - 1)
            for i, (dx, dy) in enumerate([(0, 0), (half, 0), (0, half), (half, half)]):
                self.transform_tree(x0 + dx, y0 + dy, x0, y0, log2 - 1, depth + 1, i, cbf, nxn)
            return
        cbf_luma = int(log2 == 2 and r.random() < 0.7)
        c.decision(CTX["cbf_luma"] + (1 if depth == 0 else 0), cbf_luma)
        chroma_here = log2 > 2 or blk == 3
        cbf_chroma = (cbf[0] or cbf[1]) and chroma_here
        if (cbf_luma or (cbf[0] or cbf[1])) and not self.qp_delta_coded:
            self.qp_delta_coded = True
            delta = r.choice([0, 0, 1, -1, 3, -4, 9, -13, 31, -32, 20, -25])
            v = abs(delta)
            prefix = min(v, 5)
            for i in range(prefix):
                c.decision(CTX["cu_qp_delta"] + (0 if i == 0 else 1), 1)
            if prefix < 5:
                c.decision(CTX["cu_qp_delta"] + (0 if prefix == 0 else 1), 0)
            else:
                c.eg(v - 5, 0)
            if v:
                c.bypass(int(delta < 0))
        if cbf_luma:
            self.residual(log2, 0, self.mode[(x0 >> 2, y0 >> 2)])
        if cbf_chroma:
            for k in range(2):
                if cbf[k]:
                    self.residual(2, 1 + k, self.chroma_mode)

    def residual(self, log2, comp, mode):
        assert log2 == 2
        c, r = self.cabac, self.rng
        if not self.bypass:
            c.decision(CTX["tskip"] + (1 if comp else 0), int(r.random() < 0.25))
        scan_idx = 2 if 6 <= mode <= 14 else (1 if 22 <= mode <= 30 else 0)
        scan = SCANS[scan_idx]
        count = r.choice([1, 1, 2, 3, 5, 9, 16])
        positions = sorted(r.sample(range(16), count))
        levels = {}
        for n in positions:
            mag = r.choice([1, 1, 1, 2, 2, 3, 4, 7, 15, 40, 300])
            levels[n] = mag if r.random() < 0.5 else -mag
        last = positions[-1]
        lx, ly = scan[last]
        if scan_idx == 2:
            lx, ly = ly, lx
        for which, v in (("last_x", lx), ("last_y", ly)):
            base = CTX[which] + (15 if comp else 0)
            for i in range(v):
                c.decision(base + i, 1)
            if v < 3:
                c.decision(base + v, 0)
        for n in range(last - 1, -1, -1):
            x, y = scan[n]
            c.decision(CTX["sig"] + (27 if comp else 0) + SIG_MAP[(y << 2) + x], int(n in levels))
        order = [n for n in range(15, -1, -1) if n in levels]
        g1, g2 = {}, {}
        g1ctx = 1
        first_g1 = -1
        for n in order[:8]:
            flag = int(abs(levels[n]) > 1)
            c.decision(CTX["gt1"] + (16 if comp else 0) + min(3, g1ctx), flag)
            g1[n] = flag
            if g1ctx > 0:
                g1ctx = 0 if flag else g1ctx + 1
            if flag and first_g1 < 0:
                first_g1 = n
        if first_g1 >= 0:
            flag = int(abs(levels[first_g1]) > 2)
            c.decision(CTX["gt2"] + (4 if comp else 0), flag)
            g2[first_g1] = flag
        for n in order:
            c.bypass(int(levels[n] < 0))
        rice, num_sig = 0, 0
        for n in order:
            base = 1 + g1.get(n, 0) + g2.get(n, 0)
            if base == ((3 if n == first_g1 else 2) if num_sig < 8 else 1):
                rem = abs(levels[n]) - base
                if (rem >> rice) < 4:
                    for _ in range(rem >> rice):
                        c.bypass(1)
                    c.bypass(0)
                    c.bypass_bits(rem & ((1 << rice) - 1), rice)
                else:
                    for _ in range(4):
                        c.bypass(1)
                    c.eg(rem - (4 << rice), rice + 1)
                if base + rem > 3 * (1 << rice):
                    rice = min(rice + 1, 4)
            num_sig += 1


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests/data/hevc10_pcm.h265")
    rng = random.Random(20261124)
    stream = vps() + sps(0, 0) + sps(1, 1) + pps(0, 0, -4, -4, 3, 2, 3) + pps(1, 1, 30, 5, -6, -1, 2)
    for pps_id, init_qp, slice_qp, lf in ((0, -4, -10, 0), (1, 30, 33, 1)):
        p = Picture(rng, slice_qp, lf)
        p.init_qp = init_qp
        stream += p.encode(pps_id)
    with open(out, "wb") as f:
        f.write(stream)
    print(f"{out}: {len(stream)} bytes")


if __name__ == "__main__":
    main()
