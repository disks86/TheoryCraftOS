"""Builds tiny H.264 streams of I_PCM macroblocks mixed with Intra16x16 ones, in CAVLC and CABAC (the CABAC arithmetic encoder
below is written from the standard's 9.3.4). x264 never emits I_PCM, so this is the only way to exercise that path; ffmpeg
decodes them for the expected pictures. Used by make_video_fixtures.py."""
import os, random, re
here = os.path.dirname(os.path.abspath(__file__))
t = open(os.path.join(here, '..', '..', 'src', 'Hardware', 'VideoCodec', 'Software', 'H264Tables.inc')).read()
def arr(name):
    m = re.search(name + r'\[\d+\] = \{(.*?)\};', t, re.S)
    return [int(x) for x in m.group(1).replace('\n', '').split(',')]
LPS = arr('kRangeLps'); NEXT = arr('kStateNext'); ctx = arr('kCabacInit')   # [460][4 models][m, n]

class BW:
    def __init__(self): self.bits = []
    def u(self, v, n):
        for i in range(n - 1, -1, -1): self.bits.append((v >> i) & 1)
    def ue(self, v):
        v += 1; n = v.bit_length(); self.u(0, n - 1); self.u(v, n)
    def se(self, v): self.ue(2 * v - 1 if v > 0 else -2 * v)
    def align0(self):
        while len(self.bits) % 8: self.bits.append(0)
    def trailing(self):
        self.bits.append(1); self.align0()
    def bytes(self):
        assert len(self.bits) % 8 == 0
        return bytes(int(''.join(map(str, self.bits[i:i+8])), 2) for i in range(0, len(self.bits), 8))

def nal(t, ref, payload):
    out = bytearray(); z = 0
    for b in payload:
        if z >= 2 and b <= 3: out.append(3); z = 0
        out.append(b); z = z + 1 if b == 0 else 0
    return b'\x00\x00\x00\x01' + bytes([(ref << 5) | t]) + bytes(out)

class Cabac:
    def __init__(self, bw, qp):
        self.bw = bw
        self.state = [0] * 460; self.mps = [0] * 460
        flat = ctx
        for i in range(460):
            m = flat[(i * 4 + 0) * 2]; n = flat[(i * 4 + 0) * 2 + 1]
            pre = min(max(((m * min(max(qp, 0), 51)) >> 4) + n, 1), 126)
            if pre <= 63: self.state[i] = 63 - pre; self.mps[i] = 0
            else: self.state[i] = pre - 64; self.mps[i] = 1
        self.start()
    def start(self):
        self.low = 0; self.range = 510; self.first = True; self.out = 0
    def put(self, b):
        if self.first: self.first = False
        else: self.bw.bits.append(b)
        while self.out > 0: self.bw.bits.append(1 - b); self.out -= 1
    def renorm(self):
        while self.range < 256:
            if self.low < 256: self.put(0)
            elif self.low >= 512: self.low -= 512; self.put(1)
            else: self.low -= 256; self.out += 1
            self.range <<= 1; self.low <<= 1
    def decision(self, c, b):
        s = self.state[c]; q = (self.range >> 6) & 3; lps = LPS[s * 4 + q]
        self.range -= lps
        if b != self.mps[c]:
            self.low += self.range; self.range = lps
            if s == 0: self.mps[c] = 1 - self.mps[c]
            self.state[c] = NEXT[s * 2]
        else:
            self.state[c] = NEXT[s * 2 + 1]
        self.renorm()
    def terminate(self, b):
        self.range -= 2
        if b:
            self.low += self.range
            self.range = 2; self.renorm()
            self.put((self.low >> 9) & 1)
            self.bw.u(((self.low >> 7) & 3) | 1, 2)
        else:
            self.renorm()

def make(cabac, seed, w=2, h=2, name='pcm'):
    rnd = random.Random(seed)
    sps = BW(); sps.u(77, 8); sps.u(0, 8); sps.u(30, 8); sps.ue(0); sps.ue(0); sps.ue(2); sps.ue(1); sps.u(0, 1)
    sps.ue(w - 1); sps.ue(h - 1); sps.u(1, 1); sps.u(1, 1); sps.u(0, 1); sps.u(0, 1); sps.trailing()
    pps = BW(); pps.ue(0); pps.ue(0); pps.u(1 if cabac else 0, 1); pps.u(0, 1); pps.ue(0); pps.ue(0); pps.ue(0); pps.u(0, 1); pps.u(0, 2)
    pps.se(0); pps.se(0); pps.se(0); pps.u(1, 1); pps.u(0, 1); pps.u(0, 1); pps.trailing()
    sl = BW(); sl.ue(0); sl.ue(7); sl.ue(0); sl.u(0, 4); sl.ue(0); sl.u(0, 1); sl.u(0, 1)
    qp = 26 + 6
    sl.se(6)   # slice_qp_delta
    sl.ue(0); sl.se(0); sl.se(0)   # deblocking enabled, offsets 0
    n = w * h
    kinds = ['I16' if (i % 3 == 0) else 'PCM' for i in range(n)]
    def pcm_bytes():
        d = bytearray(rnd.randrange(256) for _ in range(384))
        for k in range(0, 380, 37): d[k] = 0; d[k + 1] = 0; d[k + 2] = rnd.randrange(4)   # emulation prevention food
        return bytes(d)
    if not cabac:
        for i, k in enumerate(kinds):
            mx, my = i % w, i // w
            if k == 'PCM':
                sl.ue(25); sl.align0()
                for b in pcm_bytes(): sl.u(b, 8)
            else:
                sl.ue(3); sl.ue(0); sl.se(0)
                # coeff_token (0 coeffs) of Intra16x16DCLevel: nC from the neighbours' block 0 counts (PCM = 16)
                left = kinds[i - 1] if mx > 0 else None; top = kinds[i - w] if my > 0 else None
                na = (16 if left == 'PCM' else 0) if left else None
                nb = (16 if top == 'PCM' else 0) if top else None
                nc = (na + nb + 1) >> 1 if (na is not None and nb is not None) else (na if na is not None else (nb if nb is not None else 0))
                if nc < 2: sl.u(1, 1)
                elif nc < 4: sl.u(3, 2)
                elif nc < 8: sl.u(15, 4)
                else: sl.u(3, 6)
        sl.trailing()
    else:
        while len(sl.bits) % 8: sl.bits.append(1)   # cabac_alignment_one_bit
        c = Cabac(sl, qp)
        for i, k in enumerate(kinds):
            mx, my = i % w, i // w
            left = kinds[i - 1] if mx > 0 else None; top = kinds[i - w] if my > 0 else None
            inc = (1 if left else 0) + (1 if top else 0)    # neighbours are I16 or PCM: never I_NxN
            c.decision(3 + inc, 1)
            if k == 'PCM':
                c.terminate(1)
                sl.align0()
                for b in pcm_bytes(): sl.u(b, 8)
                c.start()
            else:
                c.terminate(0)
                c.decision(3 + 3, 0)         # luma cbp 0
                c.decision(3 + 4, 0)         # chroma cbp 0
                c.decision(3 + 6, 1); c.decision(3 + 7, 0)   # prediction mode 2 (DC)
                c.decision(64 + 0, 0)        # intra chroma pred mode 0
                c.decision(60 + 0, 0)        # mb_qp_delta 0
                ca = 1 if left is None else (1 if left == 'PCM' else 0)
                cb = 1 if top is None else (1 if top == 'PCM' else 0)
                c.decision(85 + ca + 2 * cb, 0)   # coded_block_flag of the DC block: not coded
            c.terminate(1 if i == n - 1 else 0)
        sl.align0()      # the last '1' bit written by the flush is the stop bit
        # (the flush wrote the stop bit; pad with zeros to a byte)
    data = nal(7, 3, sps.bytes()) + nal(8, 3, pps.bytes()) + nal(5, 3, sl.bytes())
    open(name + '.h264', 'wb').write(data)

def build_all():
    make(False, 1, 3, 3, 'h264_pcm_cavlc')
    make(True, 4, 3, 3, 'h264_pcm_cabac')
