#!/usr/bin/env python3
# Writes tests/data/hevcrexttool_*.h265: small H.265 streams that use the coding tools of the range extensions, which no encoder at hand writes (x265 makes the
# 4:0:0 / 4:2:2 / 4:4:4 and 12-bit streams of scripts/gen_hevc_rext_test_streams.sh, with no SPS or PPS range extension):
#   transform_skip_rotation, transform_skip_context, implicit RDPCM (intra blocks of the horizontal and vertical modes without a transform), explicit RDPCM (inter
#   blocks without a transform), intra_smoothing_disabled, high_precision_offsets (weighted prediction), persistent_rice_adaptation (with wavefront rows, whose
#   StatCoeff is synchronised with the contexts), transform skip of blocks up to 32x32 (log2_max_transform_skip_block_size), cross-component prediction (4:4:4),
#   the chroma QP offset lists, log2_sao_offset_scale, with lossless coding units, scaling lists, SAO, cu_qp_delta, sign data hiding, NxN intra partitions, merge,
#   skip and AMVP inter coding units in P pictures.
#
# It is a tiny H.265 encoder in the manner of scripts/gen_hevc10_pcm_stream.py: the parameter sets and slice data are written with a CABAC encoder (the context
# initialisation from src/Hardware/VideoCodec/Software/H265CabacInit.inc, the contexts numbered as H265Cabac.hpp numbers them); every decision (splits, modes,
# partitions, motion vector differences, coefficient levels, flags) is taken at random with a fixed seed, not by searching. The stream is legal; the pictures need
# not look like anything. Nothing here knows what the pictures decode to: FFmpeg decodes each stream, libde265 (dec265) must agree with it, and the MD5 of every
# plane of FFmpeg's pictures is then written into the stream as a decoded picture hash SEI after each picture, which a decoder checks over its own planes.
#
# Not here: extended_precision_processing_flag and cabac_bypass_alignment_enabled_flag (neither FFmpeg nor libde265 decodes them, so there is no reference).
# Usage: python3 -I scripts/gen_hevc_rext_tool_streams.py [out-dir]   (needs ffmpeg and dec265)
import hashlib
import os
import random
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CTX = dict(sao_merge=0, sao_type=1, split_cu=2, bypass=5, skip=6, cu_qp_delta=9, pred_mode=11, part_mode=12, prev_intra=16, chroma_pred=17, merge_flag=18,
           merge_idx=19, mvp=27, rqt_root_cbf=28, split_tf=29, cbf_luma=32, cbf_chroma=34, mvd_g0=38, mvd_g1=39, tskip=40, last_x=42, last_y=60, csbf=78,
           sig=82, gt1=124, gt2=148, cbf_chroma4=154, sig_skip=155, rdpcm=157, rdpcm_dir=159, res_scale=161, res_sign=169, cqp_flag=171, cqp_idx=172)
with open(os.path.join(ROOT, "src/Hardware/VideoCodec/Software/H265CabacInit.inc")) as f:
    INIT = [[int(v) for v in row.split(",")] for row in re.findall(r"\{([0-9, ]+)\}", f.read())]

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
    def __init__(self, bw, init_type, slice_qp):
        self.bw = bw
        self.init(init_type, slice_qp)
        self.start()

    def init(self, init_type, slice_qp):
        qp = max(0, min(51, slice_qp))
        self.state = []
        for v in INIT[init_type]:
            m, n = (v >> 4) * 5 - 45, ((v & 15) << 3) - 16
            pre = max(1, min(126, ((m * qp) >> 4) + n))
            self.state.append([pre - 64, 1] if pre > 63 else [63 - pre, 0])

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
        b = int(b)
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
        return b

    def bypass(self, b):
        b = int(b)
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
        return b

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

    def save(self):
        return [list(s) for s in self.state]

    def restore(self, saved):
        self.state = [list(s) for s in saved]


def escape(payload):
    out, zeros = bytearray(), 0
    for b in payload:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def nal(type_, payload):
    return bytes([0, 0, 0, 1, type_ << 1, 1]) + escape(payload)


def profile_tier_level(bw, p):
    bw.u(0, 2); bw.u(0, 1); bw.u(4, 5)    # profile space, tier, general_profile_idc 4: format range extensions
    bw.u(1 << (31 - 4), 32)                 # general_profile_compatibility_flag[4]
    bw.u(1, 1); bw.u(0, 1); bw.u(0, 1); bw.u(1, 1)   # progressive, not interlaced, not non-packed, frame only
    deep = max(p["bd_y"], p["bd_c"])
    bw.u(int(deep <= 12), 1); bw.u(int(deep <= 10), 1); bw.u(int(deep <= 8), 1)   # max_12bit, max_10bit, max_8bit
    bw.u(int(p["cf"] <= 2), 1); bw.u(int(p["cf"] <= 1), 1); bw.u(int(p["cf"] == 0), 1)   # max_422chroma, max_420chroma, max_monochrome
    bw.u(0, 1); bw.u(0, 1); bw.u(1, 1)    # intra, one_picture_only, lower_bit_rate
    bw.u(0, 34); bw.u(0, 1)
    bw.u(93, 8)                             # level 3.1


def vps(p):
    bw = BitWriter()
    bw.u(0, 4); bw.u(1, 1); bw.u(1, 1); bw.u(0, 6); bw.u(0, 3); bw.u(1, 1); bw.u(0xFFFF, 16)
    profile_tier_level(bw, p)
    bw.u(1, 1); bw.ue(1); bw.ue(0); bw.ue(0)
    bw.u(0, 6); bw.ue(0); bw.u(0, 1); bw.u(0, 1)
    bw.trailing()
    return nal(32, bw.bytes())


def sps(p):
    bw = BitWriter()
    bw.u(0, 4); bw.u(0, 3); bw.u(1, 1)
    profile_tier_level(bw, p)
    bw.ue(0); bw.ue(p["cf"])
    if p["cf"] == 3:
        bw.u(0, 1)                          # separate_colour_plane_flag
    bw.ue(p["w"]); bw.ue(p["h"]); bw.u(0, 1)
    bw.ue(p["bd_y"] - 8); bw.ue(p["bd_c"] - 8); bw.ue(4)   # log2_max_pic_order_cnt_lsb 8
    bw.u(1, 1); bw.ue(1); bw.ue(0); bw.ue(0)
    bw.ue(0); bw.ue(2); bw.ue(0); bw.ue(3)  # coding blocks 8x8 .. 32x32, transform blocks 4x4 .. 32x32
    bw.ue(2); bw.ue(3)                      # max_transform_hierarchy_depth inter 2, intra 3
    bw.u(p["scaling"], 1)
    if p["scaling"]:
        bw.u(0, 1)                          # the default lists
    bw.u(0, 1); bw.u(p["sao"], 1); bw.u(0, 1)   # no AMP, SAO, no PCM
    bw.ue(1); bw.ue(1); bw.ue(0); bw.ue(0); bw.u(1, 1)    # one short-term set: the picture before
    bw.u(0, 1); bw.u(0, 1); bw.u(1, 1)    # no long-term pictures, no temporal MVP, strong intra smoothing
    bw.u(0, 1)                              # no VUI
    bw.u(1, 1)                              # sps_extension_present_flag
    bw.u(1, 1); bw.u(0, 1); bw.u(0, 1); bw.u(0, 1); bw.u(0, 4)
    for name in ("rotation", "ts_context", "implicit_rdpcm", "explicit_rdpcm", "extended_precision", "smoothing_disabled", "high_precision", "persistent_rice",
                 "bypass_alignment"):
        bw.u(p[name], 1)
    bw.trailing()
    return nal(33, bw.bytes())


def pps(p):
    bw = BitWriter()
    bw.ue(0); bw.ue(0); bw.u(0, 1); bw.u(0, 1); bw.u(0, 3); bw.u(p["sign_hiding"], 1); bw.u(0, 1)
    bw.ue(0); bw.ue(0); bw.se(p["init_qp"] - 26)
    bw.u(0, 1); bw.u(1, 1); bw.u(1, 1); bw.ue(1)    # transform skip; cu_qp_delta with quantization groups of 16x16
    bw.se(p["cb_offset"]); bw.se(p["cr_offset"]); bw.u(0, 1)
    bw.u(1, 1); bw.u(0, 1)                # weighted prediction (P), not bi-prediction
    bw.u(1, 1)                            # transquant bypass
    bw.u(0, 1); bw.u(p["wpp"], 1)         # no tiles, entropy_coding_sync
    bw.u(0, 1)                            # loop filter across slices
    bw.u(1, 1); bw.u(0, 1); bw.u(0, 1); bw.se(p["beta"]); bw.se(p["tc"])
    bw.u(0, 1); bw.u(0, 1); bw.ue(0); bw.u(0, 1)
    bw.u(1, 1)                            # pps_extension_present_flag
    bw.u(1, 1); bw.u(0, 1); bw.u(0, 1); bw.u(0, 1); bw.u(0, 4)
    bw.ue(p["log2_max_ts"] - 2)
    bw.u(p["cross"], 1)
    bw.u(int(bool(p["cqp_list"])), 1)
    if p["cqp_list"]:
        bw.ue(p["cqp_depth"]); bw.ue(len(p["cqp_list"]) - 1)
        for cb, cr in p["cqp_list"]:
            bw.se(cb); bw.se(cr)
    bw.ue(p["sao_scale"][0]); bw.ue(p["sao_scale"][1])
    bw.trailing()
    return nal(34, bw.bytes())


def sei_hash(planes):
    # suffix SEI: decoded picture hash (payload 132), MD5 of each plane
    payload = bytes([0]) + b"".join(hashlib.md5(pl).digest() for pl in planes)
    bw = BitWriter()
    bw.u(132, 8); bw.u(len(payload), 8)
    for b in payload:
        bw.u(b, 8)
    bw.trailing()
    return nal(40, bw.bytes())


def scan_orders():
    out = {}
    for log2 in range(4):
        n = 1 << log2
        diag, x, y = [], 0, 0
        while len(diag) < n * n:
            while y >= 0:
                if x < n and y < n:
                    diag.append((x, y))
                y -= 1
                x += 1
            y, x = x, 0
        out[(0, log2)] = diag
        out[(1, log2)] = [(i % n, i // n) for i in range(n * n)]
        out[(2, log2)] = [(i // n, i % n) for i in range(n * n)]
    return out


SCAN = scan_orders()
CTX_IDX_MAP = [0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8]
CHROMA_MODES = [0, 26, 10, 1]
MODE422 = [0, 1, 2, 2, 2, 2, 3, 5, 7, 8, 10, 12, 13, 15, 17, 18, 19, 20, 21, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 28, 29, 29, 30, 31]
CTB_LOG2, MIN_CB_LOG2, MIN_TB_LOG2, MAX_TB_LOG2 = 5, 3, 2, 5


class Picture:
    def __init__(self, p, rng, poc, intra):
        self.p, self.r, self.poc, self.is_intra_pic = p, rng, poc, intra
        self.cat = p["cf"]
        self.sw = 1 if self.cat in (1, 2) else 0
        self.sh = 1 if self.cat == 1 else 0
        self.depth = {}      # per 8x8 block: the coding quadtree depth
        self.mode = {}       # per 4x4 block: the luma intra mode (DC for inter blocks)
        self.skip = {}       # per 4x4 block: cu_skip_flag
        self.qp_bd = 6 * (p["bd_y"] - 8)

    # ---- slice header and data ----
    def encode(self):
        p, r = self.p, self.r
        bw = BitWriter()
        bw.u(1, 1)
        if self.is_intra_pic:
            bw.u(0, 1)
        bw.ue(0); bw.ue(2 if self.is_intra_pic else 1)
        if not self.is_intra_pic:
            bw.u(self.poc & 255, 8); bw.u(1, 1)   # short_term_ref_pic_set_sps_flag (the one set)
        if p["sao"]:
            bw.u(1, 1)
            if self.cat:
                bw.u(1, 1)
        if not self.is_intra_pic:
            bw.u(0, 1)                             # num_ref_idx_active_override_flag
            self.weights(bw)
            self.max_merge = r.randint(1, 5)
            bw.ue(5 - self.max_merge)
        self.slice_qp = p["slice_qp"] + r.randint(-3, 3)
        bw.se(self.slice_qp - p["init_qp"])
        self.cqp_enabled = bool(p["cqp_list"]) and r.random() < 0.85
        if p["cqp_list"]:
            bw.u(int(self.cqp_enabled), 1)
        ctb_w, ctb_h = (p["w"] + 31) >> 5, (p["h"] + 31) >> 5
        rows = []
        init_type = 0 if self.is_intra_pic else 1
        # the substreams: one for the slice, or one per CTB row with wavefront rows
        data = BitWriter()
        self.cabac = CabacEncoder(data, init_type, self.slice_qp)
        self.stat = [0, 0, 0, 0]
        saved = None
        for ry in range(ctb_h):
            if p["wpp"] and ry > 0:
                data = BitWriter()
                self.cabac = CabacEncoder(data, init_type, self.slice_qp)
                if saved is not None:
                    self.cabac.restore(saved[0])
                    self.stat = list(saved[1])
                else:
                    self.stat = [0, 0, 0, 0]
            for rx in range(ctb_w):
                if p["sao"]:
                    self.sao(rx, ry)
                self.quadtree(rx << 5, ry << 5, CTB_LOG2, 0)
                last = rx == ctb_w - 1 and ry == ctb_h - 1
                self.cabac.terminate(int(last))
                if p["wpp"] and rx == 1:
                    saved = (self.cabac.save(), list(self.stat))   # (after the second CTB of the row: what the next row starts from)
                if p["wpp"] and rx == ctb_w - 1 and not last:
                    self.cabac.terminate(1)        # end_of_subset_one_bit
                    data.align_zero()
                    rows.append(data)
        data.align_zero()
        rows.append(data)
        substreams = [d.bytes() for d in rows]
        if p["wpp"]:
            # entry points: the sizes of the substreams with the emulation prevention bytes the NAL unit will have
            whole = b"".join(substreams)
            escaped_sizes, at = [], 0
            for s in substreams:
                start, end = at, at + len(s)
                at = end
                escaped_sizes.append(len(escape(whole[:end])) - len(escape(whole[:start])))
            bw.ue(len(substreams) - 1)
            if len(substreams) > 1:
                bw.ue(31)
                for size in escaped_sizes[:-1]:
                    bw.u(size - 1, 32)
        bw.u(1, 1); bw.align_zero()   # byte_alignment()
        payload = bw.bytes() + b"".join(substreams)
        return nal(19 if self.is_intra_pic else 1, payload)

    def weights(self, bw):
        p, r = self.p, self.r
        half_y = 1 << (p["bd_y"] - 1 if p["high_precision"] else 7)
        half_c = 1 << (p["bd_c"] - 1 if p["high_precision"] else 7)
        denom = r.randint(0, 7)
        bw.ue(denom)
        cdenom = denom
        if self.cat:
            cdenom = r.randint(0, 7)
            bw.se(cdenom - denom)
        lw = r.random() < 0.8
        cwf = self.cat != 0 and r.random() < 0.8
        bw.u(int(lw), 1)
        if self.cat:
            bw.u(int(cwf), 1)
        if lw:
            bw.se(r.randint(-40, 40)); bw.se(r.randint(-half_y, half_y - 1))
        if cwf:
            for _ in range(2):
                bw.se(r.randint(-40, 40)); bw.se(r.randint(-4 * half_c, 4 * half_c - 1))

    def sao(self, rx, ry):
        c, r, p = self.cabac, self.r, self.p
        if rx > 0:
            merge = r.random() < 0.2
            c.decision(CTX["sao_merge"], merge)
            if merge:
                return
        if ry > 0:
            merge = r.random() < 0.2
            c.decision(CTX["sao_merge"], merge)
            if merge:
                return
        last_kind = 0
        for comp in range(3 if self.cat else 1):
            bd = p["bd_y"] if comp == 0 else p["bd_c"]
            cmax = (1 << (min(bd, 10) - 5)) - 1
            if comp < 2:
                kind = r.choice([0, 1, 2, 2])
                c.decision(CTX["sao_type"], kind != 0)
                if kind:
                    c.bypass(kind == 2)
                last_kind = kind
            kind = last_kind
            if not kind:
                continue
            offsets = [r.randint(0, cmax) for _ in range(4)]
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
        p, c, r = self.p, self.cabac, self.r
        if x0 >= p["w"] or y0 >= p["h"]:
            return
        size = 1 << log2
        if log2 >= CTB_LOG2 - 1:
            self.qp_delta_coded = False       # quantization groups of 16x16 (diff_cu_qp_delta_depth 1)
        if self.cqp_enabled and log2 >= CTB_LOG2 - p["cqp_depth"]:
            self.cqp_coded = False
        if x0 + size <= p["w"] and y0 + size <= p["h"] and log2 > MIN_CB_LOG2:
            split = r.random() < 0.55
            inc = 0
            if x0 > 0 and self.depth[((x0 - 1) >> 3, y0 >> 3)] > depth:
                inc += 1
            if y0 > 0 and self.depth[(x0 >> 3, (y0 - 1) >> 3)] > depth:
                inc += 1
            c.decision(CTX["split_cu"] + inc, split)
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

    def fill(self, table, x, y, size, v):
        for yy in range(y >> 2, (y + size) >> 2):
            for xx in range(x >> 2, (x + size) >> 2):
                table[(xx, yy)] = v

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

    def coding_unit(self, x0, y0, log2):
        p, c, r = self.p, self.cabac, self.r
        size = 1 << log2
        self.nxn = False
        self.bypass = r.random() < p["bypass_p"]
        c.decision(CTX["bypass"], self.bypass)
        if not self.is_intra_pic:
            inc = 0
            if x0 > 0 and self.skip.get(((x0 - 1) >> 2, y0 >> 2)):
                inc += 1
            if y0 > 0 and self.skip.get((x0 >> 2, (y0 - 1) >> 2)):
                inc += 1
            skip = r.random() < 0.25
            c.decision(CTX["skip"] + inc, skip)
            self.fill(self.skip, x0, y0, size, skip)
            if skip:
                self.fill(self.mode, x0, y0, size, 1)
                self.merge_idx()
                return
            self.intra = r.random() < 0.3
            c.decision(CTX["pred_mode"], self.intra)
        else:
            self.intra = True
        if not self.intra:
            self.fill(self.mode, x0, y0, size, 1)
            c.decision(CTX["part_mode"], 1)             # 2Nx2N
            merge = r.random() < 0.5
            c.decision(CTX["merge_flag"], merge)
            if merge:
                self.merge_idx()
            else:
                mvd = [r.choice([0, 0, 1, -1, 2, -3, 5, -8, 13, -21, 34]) for _ in range(2)]
                g0 = [int(v != 0) for v in mvd]
                c.decision(CTX["mvd_g0"], g0[0]); c.decision(CTX["mvd_g0"], g0[1])
                g1 = [int(abs(v) > 1) for v in mvd]
                for k in range(2):
                    if g0[k]:
                        c.decision(CTX["mvd_g1"], g1[k])
                for k in range(2):
                    if g0[k]:
                        if g1[k]:
                            c.eg(abs(mvd[k]) - 2, 1)
                        c.bypass(mvd[k] < 0)
                c.decision(CTX["mvp"], r.randrange(2))
                root = r.random() < 0.8
                c.decision(CTX["rqt_root_cbf"], root)
                if not root:
                    return
            self.chroma_dm = [False] * 4
            self.luma_modes = [1] * 4
            self.chroma_modes = [0] * 4
            self.transform_tree(x0, y0, x0, y0, log2, 0, 0, [0, 0], [0, 0], False, x0, y0, log2)
            return
        nxn = False
        if log2 == MIN_CB_LOG2:
            nxn = r.random() < 0.45
            c.decision(CTX["part_mode"], not nxn)
        self.nxn = nxn
        parts = 4 if nxn else 1
        pb = size >> 1 if nxn else size
        pus = [(x0 + (i & 1) * pb, y0 + (i >> 1) * pb) for i in range(parts)]
        coded = []
        modes = []
        for (x, y) in pus:
            want = r.randrange(35)
            if r.random() < 0.35:
                want = r.choice([10, 26])          # (implicit RDPCM applies to these)
            cand = self.candidates(x, y)
            if r.random() < 0.3:
                want = cand[r.randrange(3)]
            self.fill(self.mode, x, y, pb, want)
            coded.append((want in cand, cand, want))
            modes.append(want)
        for in_list, _, _ in coded:
            c.decision(CTX["prev_intra"], in_list)
        for in_list, cand, want in coded:
            if in_list:
                idx = cand.index(want)
                c.bypass(idx > 0)
                if idx > 0:
                    c.bypass(idx > 1)
            else:
                c.bypass_bits(want - sum(1 for v in cand if v < want), 5)
        if parts == 1:
            modes = modes * 4
        self.luma_modes = modes
        chroma_parts = parts if self.cat == 3 else (1 if self.cat else 0)
        self.chroma_dm = [False] * 4
        self.chroma_modes = [0] * 4
        for i in range(chroma_parts):
            syntax = r.randrange(5)
            if r.random() < 0.3:
                syntax = 4
            if syntax == 4:
                c.decision(CTX["chroma_pred"], 0)
                m = modes[i]
            else:
                c.decision(CTX["chroma_pred"], 1)
                c.bypass_bits(syntax, 2)
                m = CHROMA_MODES[syntax]
                if m == modes[i]:
                    m = 34
            if self.cat == 2:
                m = MODE422[m]
            self.chroma_dm[i] = syntax == 4
            self.chroma_modes[i] = m
        if chroma_parts == 1:
            self.chroma_dm = [self.chroma_dm[0]] * 4
            self.chroma_modes = [self.chroma_modes[0]] * 4
        self.transform_tree(x0, y0, x0, y0, log2, 0, 0, [0, 0], [0, 0], nxn, x0, y0, log2)

    def merge_idx(self):
        c, r = self.cabac, self.r
        if self.max_merge > 1:
            idx = r.randrange(self.max_merge)
            c.decision(CTX["merge_idx"], idx > 0)
            for k in range(1, self.max_merge - 1):
                if idx < k:
                    break
                c.bypass(idx > k)

    def transform_tree(self, x0, y0, xb, yb, log2, depth, blk, parent_cb, parent_cr, nxn, xcu, ycu, log2cu):
        p, c, r = self.p, self.cabac, self.r
        cat = self.cat
        max_depth = (3 + (1 if nxn else 0)) if self.intra else 2
        if log2 <= MAX_TB_LOG2 and log2 > MIN_TB_LOG2 and depth < max_depth and not (nxn and depth == 0):
            split = r.random() < 0.45
            c.decision(CTX["split_tf"] + 5 - log2, split)
        else:
            split = log2 > MAX_TB_LOG2 or (nxn and depth == 0)
        cbf_cb, cbf_cr = [0, 0], [0, 0]
        if (log2 > 2 and cat != 0) or cat == 3:
            ctx = CTX["cbf_chroma"] + depth if depth < 4 else CTX["cbf_chroma4"]
            two = cat == 2 and (not split or log2 == 3)
            for cbf, parent in ((cbf_cb, parent_cb), (cbf_cr, parent_cr)):
                if depth == 0 or parent[0]:
                    cbf[0] = c.decision(ctx, r.random() < 0.6)
                    if two:
                        cbf[1] = c.decision(ctx, r.random() < 0.6)
        elif depth > 0 and cat != 0:
            cbf_cb, cbf_cr = list(parent_cb), list(parent_cr)
        if split:
            half = 1 << (log2 - 1)
            for i, (dx, dy) in enumerate([(0, 0), (half, 0), (0, half), (half, half)]):
                self.transform_tree(x0 + dx, y0 + dy, x0, y0, log2 - 1, depth + 1, i, cbf_cb, cbf_cr, nxn, xcu, ycu, log2cu)
            return
        cbf_luma = 1
        if self.intra or depth != 0 or any(cbf_cb) or any(cbf_cr):
            cbf_luma = c.decision(CTX["cbf_luma"] + (1 if depth == 0 else 0), r.random() < 0.7)
        self.transform_unit(x0, y0, xb, yb, log2, blk, cbf_luma, cbf_cb, cbf_cr, xcu, ycu, log2cu)

    def transform_unit(self, x0, y0, xb, yb, log2, blk, cbf_luma, cbf_cb, cbf_cr, xcu, ycu, log2cu):
        p, c, r = self.p, self.cabac, self.r
        cat = self.cat
        log2c = max(2, log2 - (0 if cat == 3 else 1))
        chroma_here = cat != 0 and (log2 > 2 or cat == 3)
        chroma_last = cat != 0 and not chroma_here and blk == 3
        cbf_chroma = cat != 0 and (any(cbf_cb) or any(cbf_cr))
        if (cbf_luma or cbf_chroma) and not self.qp_delta_coded:
            self.qp_delta_coded = True
            delta = r.choice([0, 0, 0, 1, -1, 3, -4, 6, -7])
            v = abs(delta)
            prefix = min(v, 5)
            for i in range(prefix):
                c.decision(CTX["cu_qp_delta"] + (0 if i == 0 else 1), 1)
            if prefix < 5:
                c.decision(CTX["cu_qp_delta"] + (0 if prefix == 0 else 1), 0)
            else:
                c.eg(v - 5, 0)
            if v:
                c.bypass(delta < 0)
        if cbf_chroma and not self.bypass and self.cqp_enabled and not self.cqp_coded:
            flag = r.random() < 0.7
            c.decision(CTX["cqp_flag"], flag)
            n = len(p["cqp_list"])
            if flag and n > 1:
                idx = r.randrange(n)
                for k in range(n - 1):
                    c.decision(CTX["cqp_idx"], idx > k)
                    if idx <= k:
                        break
            self.cqp_coded = True
        half = (1 << log2cu) >> 1
        part = ((2 if y0 - ycu >= half else 0) + (1 if x0 - xcu >= half else 0)) if self.intra and self.nxn else 0
        luma_mode = self.luma_modes[part] if self.intra else -1
        if cbf_luma:
            self.residual(log2, 0, luma_mode)
        if not chroma_here and not chroma_last:
            return
        lc = log2c if chroma_here else 2
        chroma_mode = self.chroma_modes[part] if self.intra else -1
        cross = p["cross"] and cbf_luma and (not self.intra or self.chroma_dm[part])
        for comp, cbf in ((1, cbf_cb), (2, cbf_cr)):
            if cross:
                v = r.choice([0, 0, 1, 2, 3, 4])
                for k in range(4):
                    c.decision(CTX["res_scale"] + 4 * (comp - 1) + k, v > k)
                    if v <= k:
                        break
                if v:
                    c.decision(CTX["res_sign"] + comp - 1, r.randrange(2))
            for t in range(2 if cat == 2 else 1):
                if cbf[t]:
                    self.residual(lc, comp, chroma_mode)

    def residual(self, log2, comp, mode):
        p, c, r = self.p, self.cabac, self.r
        n = 1 << log2
        ts = False
        if not self.bypass and log2 <= p["log2_max_ts"]:
            ts = c.decision(CTX["tskip"] + (1 if comp else 0), r.random() < 0.4)
        rdpcm = False
        if not self.intra and p["explicit_rdpcm"] and (ts or self.bypass):
            rdpcm = c.decision(CTX["rdpcm"] + (1 if comp else 0), r.random() < 0.6)
            if rdpcm:
                c.decision(CTX["rdpcm_dir"] + (1 if comp else 0), r.randrange(2))
        no_transform = ts or self.bypass
        implicit = self.intra and p["implicit_rdpcm"] and ts and mode in (10, 26)
        scan_idx = 0
        if self.intra and (log2 == 2 or (log2 == 3 and (comp == 0 or self.cat == 3))):
            scan_idx = 2 if 6 <= mode <= 14 else (1 if 22 <= mode <= 30 else 0)
        sb_log2 = log2 - 2
        sbs = SCAN[(scan_idx, sb_log2)]
        pos4 = SCAN[(scan_idx, 2)]
        nsb = 1 << (2 * sb_log2)
        # the coefficients: a few sub-blocks, a few positions each
        levels = {}
        count = r.choice([1, 1, 2, 3, 5, 9, 16, 30]) if n > 4 else r.choice([1, 1, 2, 3, 5, 9, 16])
        for _ in range(count):
            k = r.randrange(nsb) if r.random() < 0.6 else r.randrange(min(nsb, 2))
            q = r.randrange(16)
            mag = min(p["max_level"], r.choice([1, 1, 1, 1, 2, 2, 3, 4, 5, 7, 11, 15, 40, 120, 700, 3000]))
            levels[(k, q)] = mag if r.random() < 0.5 else -mag
        last_k = max(k for k, _ in levels)
        last_q = max(q for k, q in levels if k == last_k)
        lx = (sbs[last_k][0] << 2) + pos4[last_q][0]
        ly = (sbs[last_k][1] << 2) + pos4[last_q][1]
        if scan_idx == 2:
            lx, ly = ly, lx
        if comp == 0:
            off, shift = 3 * (log2 - 2) + ((log2 - 1) >> 2), (log2 + 1) >> 2
        else:
            off, shift = 15, log2 - 2
        max_prefix = (log2 << 1) - 1
        for which, v in (("last_x", lx), ("last_y", ly)):
            # the prefix and suffix of last_sig_coeff_x / y (9.3.3.? / Table 9-?)
            if v < 4:
                pre, suf, sbits = v, 0, 0
            else:
                pre = 0
                while True:
                    base = (1 << ((pre >> 1) - 1)) * (2 + (pre & 1)) if pre > 3 else pre
                    nb = (pre >> 1) - 1 if pre > 3 else 0
                    if pre > 3 and base <= v < base + (1 << nb):
                        break
                    pre += 1
                suf, sbits = v - base, nb
            for i in range(pre):
                c.decision(CTX[which] + off + (i >> shift), 1)
            if pre < max_prefix:
                c.decision(CTX[which] + off + (pre >> shift), 0)
            if which == "last_x":
                x_suf = (suf, sbits)
            else:
                y_suf = (suf, sbits)
        if x_suf[1]:
            c.bypass_bits(*x_suf)
        if y_suf[1]:
            c.bypass_bits(*y_suf)
        sb_w = 1 << sb_log2
        csbf = {}
        c1 = 1
        sb_type = (2 if comp == 0 else 0) + (1 if no_transform else 0)
        sign_hiding_ok = p["sign_hiding"] and not self.bypass and not implicit and not rdpcm
        for i in range(last_k, -1, -1):
            xs, ys = sbs[i]
            right = csbf.get((xs + 1, ys), 0) if xs < sb_w - 1 else 0
            below = csbf.get((xs, ys + 1), 0) if ys < sb_w - 1 else 0
            sig_here = [q for (k, q) in levels if k == i]
            coded = True
            infer_dc = False
            if 0 < i < last_k:
                coded = bool(sig_here)
                c.decision(CTX["csbf"] + (2 if comp else 0) + (right | below), coded)
                infer_dc = True
            csbf[(xs, ys)] = int(coded)
            if not coded:
                continue
            prev = right | (below << 1)
            start = last_q - 1 if i == last_k else 15
            sig = {}
            if i == last_k:
                sig[last_q] = 1
            any_sig = i == last_k
            for q in range(start, -1, -1):
                want = 1 if q in sig_here else 0
                if q == 0 and infer_dc and not any_sig:
                    sig[0] = 1                     # inferred
                    if (i, 0) not in levels:
                        levels[(i, 0)] = 1
                    continue
                xp, yp = pos4[q]
                xc, yc = (xs << 2) + xp, (ys << 2) + yp
                if p["ts_context"] and no_transform:
                    ctx = CTX["sig_skip"] + (1 if comp else 0)
                else:
                    if log2 == 2:
                        s = CTX_IDX_MAP[(yc << 2) + xc]
                    elif xc + yc == 0:
                        s = 0
                    else:
                        if prev == 0:
                            s = 2 if xp + yp == 0 else (1 if xp + yp < 3 else 0)
                        elif prev == 1:
                            s = 2 if yp == 0 else (1 if yp == 1 else 0)
                        elif prev == 2:
                            s = 2 if xp == 0 else (1 if xp == 1 else 0)
                        else:
                            s = 2
                        if comp == 0:
                            if xs > 0 or ys > 0:
                                s += 3
                            s += (9 if scan_idx == 0 else 15) if log2 == 3 else 21
                        else:
                            s += 9 if log2 == 3 else 12
                    ctx = CTX["sig"] + s + (27 if comp else 0)
                c.decision(ctx, want)
                sig[q] = want
                if want:
                    any_sig = True
            order = [q for q in range(15, -1, -1) if sig.get(q)]
            if not order:
                continue   # (a DC sub-block without coefficients)
            mags = {q: abs(levels.get((i, q), 1)) for q in order}
            signs = {q: int(levels.get((i, q), 1) < 0) for q in order}
            ctx_set = 0 if (i == 0 or comp > 0) else 2
            if c1 == 0:
                ctx_set += 1
            c1 = 1
            g1, g2 = {}, {}
            first_g1 = -1
            for q in order[:8]:
                flag = int(mags[q] > 1)
                c.decision(CTX["gt1"] + ctx_set * 4 + c1 + (16 if comp else 0), flag)
                g1[q] = flag
                if flag:
                    c1 = 0
                    if first_g1 < 0:
                        first_g1 = q
                elif 0 < c1 < 3:
                    c1 += 1
            if first_g1 >= 0:
                g2[first_g1] = c.decision(CTX["gt2"] + ctx_set + (4 if comp else 0), mags[first_g1] > 2)
            first_sig, last_sig = order[-1], order[0]
            hidden = sign_hiding_ok and last_sig - first_sig > 3
            for q in order:
                if not (hidden and q == first_sig):
                    c.bypass(signs[q])
            rice = self.stat[sb_type] // 4 if p["persistent_rice"] else 0
            first_rem = True
            for k, q in enumerate(order):
                base = 1 + g1.get(q, 0) + g2.get(q, 0)
                if base == ((3 if q == first_g1 else 2) if k < 8 else 1):
                    rem = max(0, mags[q] - base)
                    if (rem >> rice) < 4:
                        for _ in range(rem >> rice):
                            c.bypass(1)
                        c.bypass(0)
                        c.bypass_bits(rem & ((1 << rice) - 1), rice)
                    else:
                        for _ in range(4):
                            c.bypass(1)
                        c.eg(rem - (4 << rice), rice + 1)
                    if p["persistent_rice"] and first_rem:
                        init = self.stat[sb_type] // 4
                        if rem >= (3 << init):
                            self.stat[sb_type] += 1
                        elif 2 * rem < (1 << init) and self.stat[sb_type] > 0:
                            self.stat[sb_type] -= 1
                    first_rem = False
                    if base + rem > (3 << rice):
                        rice = rice + 1 if p["persistent_rice"] else min(rice + 1, 4)


def make_stream(p, seed, pictures):
    rng = random.Random(seed)
    out = [vps(p), sps(p), pps(p)]
    pics = []
    for i in range(pictures):
        pic = Picture(p, rng, i, i == 0)
        pics.append(pic.encode())
    return out, pics


def decode(path, pf, w, h, cf, deep, tmp):
    # FFmpeg's and libde265's pictures (each a list of planes), in output order
    raw = os.path.join(tmp, "ffmpeg.yuv")
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "quiet", "-y", "-i", path, "-f", "rawvideo", "-pix_fmt", pf, raw], check=True)
    de = os.path.join(tmp, "de265.yuv")
    subprocess.run(["dec265", "-q", "-o", de, path], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    bps = 2 if deep else 1
    cw, ch = (w if cf == 3 else w // 2), (h if cf >= 2 else h // 2)
    planes = [w * h] + ([cw * ch] * 2 if cf else [])
    size = sum(planes) * bps
    out = []
    for data in (open(raw, "rb").read(), open(de, "rb").read()):
        frames = []
        for at in range(0, len(data), size):
            f, off = [], at
            for n in planes:
                f.append(data[off:off + n * bps])
                off += n * bps
            frames.append(f)
        out.append(frames)
    return out


# The streams: (name, the tools (everything else off: DEFAULTS), seed, pictures, reference). The reference is "both" - FFmpeg's pictures, which libde265's must be
# bit for bit -, "ffmpeg" where libde265 (1.1.3) is known to decode a tool otherwise (below), or "libde265" where FFmpeg (n9.0.2) is known to decode the tool otherwise
# than the standard says, which libde265 and the kernel's decoder agree on:
#   - high_precision_offsets_enabled_flag: FFmpeg keeps the offsets in units of 8-bit samples (WpOffsetBdShift BitDepth - 8, the offset range of 8 bits)
#   - transform_skip_rotation and implicit RDPCM of lossless (cu_transquant_bypass) blocks: FFmpeg rotates no lossless block, and keeps the edge filters of
#     the horizontal and vertical intra modes on (disableIntraBoundaryFilter, 8.4.4.2.6)
#   - RDPCM, transform skip of blocks larger than 4x4 and cross-component prediction at 12 bits: FFmpeg holds the residuals in 16 bits, which they overflow
#   - SAO of 4:2:2 pictures with lossless blocks: FFmpeg restores the lossless blocks' chroma (which SAO must leave alone) at the wrong rows
#   - cu_chroma_qp_offset_idx: FFmpeg reads it with cMax Max(5, chroma_qp_offset_list_len_minus1) instead of chroma_qp_offset_list_len_minus1, which is the same
#     for lists of six entries
# libde265 in turn decodes the chroma QP offset lists of more than two entries otherwise than FFmpeg and the kernel's decoder (the streams with six: "ffmpeg"), and
# rotates some transform-skipped chroma blocks of P pictures in 4:2:0 and 4:2:2 that the standard and FFmpeg do not (the 4:2:0 and 4:2:2 streams have no rotation),
# applies the scaling lists to transform-skipped blocks larger than 4x4 (which take the flat factor 16, 8.6.4.1) in some cases, and decodes some 32x32 inter
# coding units of 4:2:2 with high precision offsets otherwise than FFmpeg and the kernel's decoder (the 4:2:2 streams have no high precision offsets).
SIX = [(3, -2), (-6, 5), (12, -12), (-1, 0), (7, 4), (-12, 11)]
STREAMS = [
    ("444_8", dict(cf=3, bd_y=8, bd_c=8, rotation=1, ts_context=1, implicit_rdpcm=1, explicit_rdpcm=1, persistent_rice=1, high_precision=1, cross=1,
                   cqp_list=SIX, cqp_depth=1, log2_max_ts=4, sign_hiding=1, bypass_p=0), 11, 3, "ffmpeg"),
    ("444_8_lossless", dict(cf=3, bd_y=8, bd_c=8, ts_context=1, explicit_rdpcm=1, persistent_rice=1, cross=1, cqp_list=SIX, cqp_depth=2, log2_max_ts=5,
                            bypass_p=0.35, sao=1), 16, 3, "ffmpeg"),
    ("444_12", dict(cf=3, bd_y=12, bd_c=12, rotation=1, ts_context=1, persistent_rice=1, cross=1, smoothing_disabled=1, cqp_list=SIX, cqp_depth=0, log2_max_ts=2,
                    sao=1, sao_scale=(2, 1), wpp=1, scaling=1, bypass_p=0, max_level=40), 12, 3, "ffmpeg"),
    ("422_10", dict(cf=2, bd_y=10, bd_c=10, ts_context=1, explicit_rdpcm=1, persistent_rice=1, smoothing_disabled=1, cqp_list=SIX, cqp_depth=2, log2_max_ts=3,
                    sign_hiding=1, bypass_p=0.2), 13, 3, "ffmpeg"),
    ("420_8", dict(cf=1, bd_y=8, bd_c=8, ts_context=1, implicit_rdpcm=1, explicit_rdpcm=1, persistent_rice=1, log2_max_ts=5, wpp=1, sign_hiding=1, bypass_p=0),
     14, 3, "both"),
    ("400_10", dict(cf=0, bd_y=10, bd_c=10, rotation=1, implicit_rdpcm=1, explicit_rdpcm=1, persistent_rice=1, smoothing_disabled=1, log2_max_ts=4, sao=1,
                    bypass_p=0), 15, 3, "both"),
    # what FFmpeg decodes otherwise (above): libde265's pictures
    ("444_10_lossless", dict(cf=3, bd_y=10, bd_c=10, rotation=1, ts_context=1, implicit_rdpcm=1, explicit_rdpcm=1, persistent_rice=1, cross=1, log2_max_ts=3,
                             bypass_p=0.4), 17, 3, "libde265"),
    ("444_12_hpo", dict(cf=3, bd_y=12, bd_c=12, implicit_rdpcm=1, explicit_rdpcm=1, persistent_rice=1, high_precision=1, cross=1, cqp_list=[(4, -4), (-9, 6)],
                        cqp_depth=1, log2_max_ts=5, bypass_p=0.15), 18, 3, "libde265"),
    ("422_10_sao_lossless", dict(cf=2, bd_y=10, bd_c=10, ts_context=1, explicit_rdpcm=1, persistent_rice=1, log2_max_ts=3, sao=1, bypass_p=0.3), 20, 3,
     "libde265"),
    ("400_12_hpo", dict(cf=0, bd_y=12, bd_c=12, implicit_rdpcm=1, explicit_rdpcm=1, persistent_rice=1, high_precision=1, log2_max_ts=4, bypass_p=0.1),
     19, 3, "libde265"),
]
DEFAULTS = dict(w=96, h=64, rotation=0, ts_context=0, implicit_rdpcm=0, explicit_rdpcm=0, extended_precision=0, smoothing_disabled=0, high_precision=0,
                persistent_rice=0, bypass_alignment=0, cross=0, cqp_list=[], cqp_depth=0, log2_max_ts=2, sao=0, sao_scale=(0, 0), wpp=0, scaling=0, sign_hiding=0,
                init_qp=30, slice_qp=30, cb_offset=2, cr_offset=-3, beta=1, tc=-1, bypass_p=0.2, max_level=3000)


def pix_fmt(p):
    base = {0: "gray", 1: "yuv420p", 2: "yuv422p", 3: "yuv444p"}[p["cf"]]
    bd = max(p["bd_y"], p["bd_c"])
    return base if bd == 8 else f"{base}{bd}le" if p["cf"] else f"gray{bd}le"


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tests/data")
    for name, params, seed, count, reference in STREAMS:
        p = dict(DEFAULTS)
        p.update(params)
        headers, pics = make_stream(p, seed, count)
        path = os.path.join(out_dir, f"hevcrexttool_{name}.h265")
        with tempfile.TemporaryDirectory() as tmp:
            plain = os.path.join(tmp, "plain.h265")
            with open(plain, "wb") as f:
                f.write(b"".join(headers) + b"".join(pics))
            ffmpeg, de265 = decode(plain, pix_fmt(p), p["w"], p["h"], p["cf"], max(p["bd_y"], p["bd_c"]) > 8, tmp)
            if len(de265) != count or len(ffmpeg) != count:
                raise SystemExit(f"{name}: libde265 / FFmpeg made {len(de265)} / {len(ffmpeg)} pictures of {count}")
            if reference == "both" and ffmpeg != de265:
                raise SystemExit(f"{name}: libde265 and FFmpeg disagree")
            frames = de265 if reference == "libde265" else ffmpeg
            stream = b"".join(headers) + b"".join(pic + sei_hash(frames[i]) for i, pic in enumerate(pics))
            with open(path, "wb") as f:
                f.write(stream)
        note = "FFmpeg and libde265 agree" if ffmpeg == de265 else f"{reference}'s pictures (the other differs, as expected)"
        print(f"{path}: {len(stream)} bytes, {count} pictures; {note}")


if __name__ == "__main__":
    main()
