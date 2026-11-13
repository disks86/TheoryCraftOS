#!/usr/bin/env python3
# Makes tests/data/m4vdec_rvlc.m4v: an MPEG-4 Simple profile stream with data partitioning and reversible VLCs (reversible_vlc = 1), which no encoder at hand
# writes. 48x32 pictures: an I plane and three P planes with random coefficients (some through the RVLC escape), video packets, and P macroblocks that are
# not coded, intra, with one vector or with four. The tables come from src/Hardware/VideoCodec/Software/Mpeg4Tables.inc; the reference pictures
# (m4vdec_rvlc.yuv) are FFmpeg's decode of it (its decoder reads RVLC texture). Usage: scripts/gen_mpeg4_rvlc_stream.py   (then the ffmpeg line it prints)
import random
import re

INC = 'src/Hardware/VideoCodec/Software/Mpeg4Tables.inc'
text = open(INC).read()


def table(name):
    m = re.search(r'constexpr \w+ ' + name + r'\s*=\s*\{(.*?)\};', text, re.S)
    groups = re.findall(r'\{([^{}]*)\}', m.group(1))
    return [[int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', g)] for g in groups]


def array(name):
    m = re.search(name + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]


RV = {'intra': table('kIntraReversible'), 'inter': table('kInterReversible')}
INTRA_MCBPC = list(zip(array('kIntraMcbpcCode'), array('kIntraMcbpcBits')))
INTER_MCBPC = list(zip(array('kInterMcbpcCode'), array('kInterMcbpcBits')))
CBPY = list(zip(array('kCbpyCode'), array('kCbpyBits')))
DC_LUMA = list(zip(array('kDcLumaCode'), array('kDcLumaBits')))
DC_CHROMA = list(zip(array('kDcChromaCode'), array('kDcChromaBits')))
MV = list(zip(array('kMvCode'), array('kMvBits')))
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50,
          43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]


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


W, H, MBW, MBH = 48, 32, 3, 2
MB_BITS = 3   # bits of macroblock_number for 6 macroblocks
rng = random.Random(4263)


def rvlc_events(kind):
    """Random (last, run, level) events for a block's texture, ending with last = 1."""
    events, pos = [], 1 if kind == 'intra' else 0
    while True:
        run = rng.choice([0, 0, 0, 1, 2, 5]) if rng.random() < 0.9 else rng.randint(6, 20)
        level = rng.choice([1, 1, 1, 2, 3]) if rng.random() < 0.85 else rng.randint(4, 60)
        if rng.random() < 0.5:
            level = -level
        if pos + run > 63:
            run = 63 - pos
        pos += run + 1
        last = pos >= 64 or rng.random() < 0.25
        events.append((1 if last else 0, run, level))
        if last:
            return events


def put_rvlc(b, kind, events):
    codes, bits, runs, levels = RV[kind]
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


def put_mvd(b, d):   # f_code 1
    b.code(MV[abs(d)])
    if d:
        b.put(1 if d < 0 else 0, 1)


def vop(b, kind, time, quant):
    b.start(0xB6)
    b.put(0 if kind == 'I' else 1, 2)
    b.put(0, 1)   # modulo_time_base
    b.put(1, 1)
    b.put(time, 5)
    b.put(1, 1)
    b.put(1, 1)   # vop_coded
    if kind == 'P':
        b.put(0, 1)   # vop_rounding_type
    b.put(0, 3)   # intra_dc_vlc_thr: DC always on its own
    b.put(quant, 5)
    if kind == 'P':
        b.put(1, 3)   # vop_fcode_forward
    # Two video packets: macroblocks 0..2 and 3..5.
    for packet, mbs in enumerate([(0, 1, 2), (3, 4, 5)]):
        if packet:
            b.stuff()
            b.put(0, 16)   # resync_marker: 16 zeros (I; P: 15 + f_code), then a one
            b.put(1, 1)
            b.put(mbs[0], MB_BITS)
            b.put(quant, 5)
            b.put(0, 1)   # header_extension_code
        modes = []
        for mb in mbs:
            mode = 'intra' if kind == 'I' else rng.choice(['skip', 'inter', 'inter', 'four', 'intra'])
            cbp = rng.randint(0, 63) if mode != 'skip' else 0
            modes.append((mode, cbp))
        # Partition 1.
        for mode, cbp in modes:
            if kind == 'I':
                b.code(INTRA_MCBPC[cbp & 3])
                for n in range(6):
                    put_dc(b, n, rng.randint(-6, 6))
            else:
                if mode == 'skip':
                    b.put(1, 1)
                    continue
                b.put(0, 1)
                kindex = {'inter': 0, 'intra': 1, 'four': 4}[mode]
                b.code(INTER_MCBPC[kindex * 4 + (cbp & 3)])
                if mode in ('inter', 'four'):
                    for _ in range(4 if mode == 'four' else 1):
                        put_mvd(b, rng.randint(-4, 4))
                        put_mvd(b, rng.randint(-4, 4))
        if kind == 'I':
            b.put(0x6B001, 19)   # dc_marker
        else:
            b.put(0x1F001, 17)   # motion_marker
        # Partition 2.
        for mode, cbp in modes:
            if mode == 'skip':
                continue
            if mode == 'intra':
                b.put(0, 1)   # ac_pred_flag
                b.code(CBPY[cbp >> 2])
                if kind == 'P':
                    for n in range(6):
                        put_dc(b, n, rng.randint(-6, 6))
            else:
                b.code(CBPY[(cbp >> 2) ^ 15])
        # Partition 3: the texture.
        for mode, cbp in modes:
            if mode == 'skip':
                continue
            for n in range(6):
                if cbp & (32 >> n):
                    put_rvlc(b, 'intra' if mode == 'intra' else 'inter', rvlc_events('intra' if mode == 'intra' else 'inter'))
    b.stuff()


b = Bits()
b.start(0x00)   # video_object_start_code
b.start(0x20)   # video_object_layer_start_code
b.put(0, 1)     # random_accessible_vol
b.put(1, 8)     # video_object_type_indication: Simple
b.put(0, 1)     # is_object_layer_identifier
b.put(1, 4)     # aspect_ratio_info: square
b.put(0, 1)     # vol_control_parameters
b.put(0, 2)     # rectangular
b.put(1, 1)
b.put(25, 16)   # vop_time_increment_resolution
b.put(1, 1)
b.put(0, 1)     # fixed_vop_rate
b.put(1, 1)
b.put(W, 13)
b.put(1, 1)
b.put(H, 13)
b.put(1, 1)
b.put(0, 1)     # interlaced
b.put(1, 1)     # obmc_disable
b.put(0, 1)     # sprite_enable
b.put(0, 1)     # not_8_bit
b.put(0, 1)     # quant_type: H.263
b.put(1, 1)     # complexity_estimation_disable
b.put(0, 1)     # resync_marker_disable
b.put(1, 1)     # data_partitioned
b.put(1, 1)     # reversible_vlc
b.put(0, 1)     # scalability
b.stuff()
for i, (kind, quant) in enumerate([('I', 6), ('P', 5), ('P', 9), ('P', 4)]):
    vop(b, kind, i, quant)
open('tests/data/m4vdec_rvlc.m4v', 'wb').write(b.bytes())
print('ffmpeg -nostdin -y -threads 1 -cpuflags 0 -flags bitexact -idct simple -i tests/data/m4vdec_rvlc.m4v -f rawvideo -pix_fmt yuv420p tests/data/m4vdec_rvlc.yuv')
