# Vector-extension cases for gen.py (modes vint, vmem, vfp). Every case starts with a vsetvli of a random legal configuration, then one or two vector instructions whose
# register groups are legal (aligned, non-overlapping unless the overlap is allowed). VLEN is 128 (qemu: -cpu max,vlen=128).
import random

BUF_OFF = 1040          # the buffer in a vector block (see gen.py)
BIAS = 1536             # (x31 points this far into the block)
BUF_BASE = 512          # the offset in the buffer that memory accesses are based at

def vconfigs():
    out = []
    for sew in (8, 16, 32, 64):
        for lm in ('mf8', 'mf4', 'mf2', 'm1', 'm2', 'm4', 'm8'):
            num, den = {'mf8': (1, 8), 'mf4': (1, 4), 'mf2': (1, 2), 'm1': (1, 1), 'm2': (2, 1), 'm4': (4, 1), 'm8': (8, 1)}[lm]
            if sew * den <= 64 * num: out.append((sew, lm, num, den))
    return out
CONFIGS = vconfigs()

HALF = False      # the half precision mode (Zvfh): floating-point cases may use SEW 16, and 8-bit integers convert to and from half

class Cfg:
    def __init__(self, sew=None, fp=False, max_lmul=8, min_sew=8):
        while True:
            self.sew, self.lm, self.num, self.den = random.choice(CONFIGS)
            if sew and self.sew != sew: continue
            if fp and self.sew not in ((16, 16, 32, 64) if HALF else (32, 64)): continue
            if self.num > max_lmul or self.sew < min_sew: continue
            break
        if sew: pass
        self.ta = random.choice(['ta', 'tu'])
        self.ma = random.choice(['ma', 'mu'])
        self.vlmax = 128 // self.sew * self.num // self.den
        self.regs = max(1, self.num // self.den)       # registers of a group
    def vtype(self): return f'e{self.sew}, {self.lm}, {self.ta}, {self.ma}'
    def set(self, avl=None):
        if avl is None:
            avl = random.choice([0, 1, self.vlmax, self.vlmax, self.vlmax + 5, random.randrange(0, self.vlmax + 1), max(0, self.vlmax - 1)])
        return f'li x29, {avl}\n vsetvli x28, x29, {self.vtype()}'

def groups(count, size_list):
    """picks `count` non-overlapping register groups with the given sizes (registers) from v8..v31"""
    blocks = [8, 16, 24]
    random.shuffle(blocks)
    res = []
    for i in range(count):
        size = size_list[i]
        base = blocks[i] + random.randrange(0, 8 // size) * size
        res.append(base)
    return res

def mask_suffix(allow=True):
    return ', v0.t' if allow and random.random() < 0.5 else ''

def imm5(signed=True): return random.choice([0, 1, 2, 7, 15, -1, -16, -2]) if signed else random.choice([0, 1, 2, 5, 7, 15, 16, 31])
def xr(): return 'x%d' % random.choice(list(range(1, 28)) + [30])

INT_BIN = ['add', 'sub', 'rsub', 'min', 'minu', 'max', 'maxu', 'and', 'or', 'xor', 'sll', 'srl', 'sra', 'saddu', 'sadd', 'ssubu', 'ssub', 'aaddu', 'aadd', 'asubu', 'asub',
           'smul', 'ssrl', 'ssra', 'mul', 'mulh', 'mulhu', 'mulhsu', 'div', 'divu', 'rem', 'remu']
FORMS = {   # which of vv, vx, vi exist
    'add': 'vv vx vi', 'sub': 'vv vx', 'rsub': 'vx vi', 'min': 'vv vx', 'minu': 'vv vx', 'max': 'vv vx', 'maxu': 'vv vx', 'and': 'vv vx vi', 'or': 'vv vx vi', 'xor': 'vv vx vi',
    'sll': 'vv vx vi', 'srl': 'vv vx vi', 'sra': 'vv vx vi', 'saddu': 'vv vx vi', 'sadd': 'vv vx vi', 'ssubu': 'vv vx', 'ssub': 'vv vx', 'aaddu': 'vv vx', 'aadd': 'vv vx',
    'asubu': 'vv vx', 'asub': 'vv vx', 'smul': 'vv vx', 'ssrl': 'vv vx vi', 'ssra': 'vv vx vi', 'mul': 'vv vx', 'mulh': 'vv vx', 'mulhu': 'vv vx', 'mulhsu': 'vv vx',
    'div': 'vv vx', 'divu': 'vv vx', 'rem': 'vv vx', 'remu': 'vv vx'}
UIMM = {'sll', 'srl', 'sra', 'ssrl', 'ssra'}
INT_CMP = ['mseq', 'msne', 'msltu', 'mslt', 'msleu', 'msle', 'msgtu', 'msgt']
CMP_FORMS = {m: ('vv vx vi' if m in ('mseq', 'msne', 'msleu', 'msle') else 'vx vi' if m in ('msgtu', 'msgt') else 'vv vx') for m in INT_CMP}
MACC = ['macc', 'nmsac', 'madd', 'nmsub']
WIDE = ['waddu', 'wadd', 'wsubu', 'wsub', 'wmul', 'wmulu', 'wmulsu', 'wmaccu', 'wmacc', 'wmaccsu', 'wmaccus']
WIDEW = ['waddu.w', 'wadd.w', 'wsubu.w', 'wsub.w']
NARROW = ['nsrl', 'nsra', 'nclipu', 'nclip']
RED = ['redsum', 'redand', 'redor', 'redxor', 'redmin', 'redminu', 'redmax', 'redmaxu']
MASKLOG = ['mand', 'mnand', 'mandn', 'mxor', 'mor', 'mnor', 'morn', 'mxnor']

def operand(form, size, taken, cfg, mn=None, sew_imm_unsigned=False):
    """the vs1/rs1/imm operand text for a form"""
    if form == 'vv': return None
    if form == 'vx': return xr()
    return str(imm5(signed=not sew_imm_unsigned))

def one_int_case(cfg):
    k = random.randrange(0, 100)
    m = mask_suffix()
    r = cfg.regs
    if k < 40:
        mn = random.choice(INT_BIN + MACC)
        forms = (FORMS[mn] if mn in FORMS else 'vv vx').split()
        form = random.choice(forms)
        vd, vs2, vs1 = groups(3, [r, r, r])
        if random.random() < 0.2: vd = vs2
        if random.random() < 0.1 and form == 'vv': vd = vs1
        if mn in MACC: op = f'v{mn}.{form}'; srcs = (vs1, vs2)
        else: op = f'v{mn}.{form}'
        if form == 'vv': return f'{op} v{vd}, v{vs1}, v{vs2}{m}' if mn in MACC else f'{op} v{vd}, v{vs2}, v{vs1}{m}'
        o = operand(form, r, None, cfg, mn, mn in UIMM)
        return f'{op} v{vd}, {o}, v{vs2}{m}' if mn in MACC else f'{op} v{vd}, v{vs2}, {o}{m}'
    if k < 50:
        mn = random.choice(INT_CMP); form = random.choice(CMP_FORMS[mn].split())
        vd, vs2, vs1 = groups(3, [1, r, r])
        if form == 'vv': return f'v{mn}.vv v{vd}, v{vs2}, v{vs1}{m}'
        return f'v{mn}.{form} v{vd}, v{vs2}, {operand(form, r, None, cfg)}{m}'
    if k < 56:   # carries
        mn = random.choice(['adc', 'sbc', 'madc', 'msbc']); form = random.choice(['vv', 'vx'] + (['vi'] if mn in ('adc', 'madc') else []))
        mask_dest = mn.startswith('m')
        vd, vs2, vs1 = groups(3, [1 if mask_dest else r, r, r])
        carry = ', v0' if (mn in ('adc', 'sbc') or random.random() < 0.6) else ''
        if mask_dest and carry == '': pass
        sfx = {'vv': 'vvm', 'vx': 'vxm', 'vi': 'vim'}[form] if carry else {'vv': 'vv', 'vx': 'vx', 'vi': 'vi'}[form]
        src = f'v{vs1}' if form == 'vv' else operand(form, r, None, cfg)
        return f'v{mn}.{sfx} v{vd}, v{vs2}, {src}{carry}'
    if k < 60:   # merge / move
        vd, vs2, vs1 = groups(3, [r, r, r])
        c = random.randrange(0, 4)
        if c == 0: return f'vmerge.vvm v{vd}, v{vs2}, v{vs1}, v0'
        if c == 1: return f'vmerge.vxm v{vd}, v{vs2}, {xr()}, v0'
        if c == 2: return f'vmerge.vim v{vd}, v{vs2}, {imm5()}, v0'
        return random.choice([f'vmv.v.v v{vd}, v{vs1}', f'vmv.v.x v{vd}, {xr()}', f'vmv.v.i v{vd}, {imm5()}'])
    if k < 66 and cfg.sew <= 32 and cfg.num <= 4:   # widening
        vd, vs2, vs1 = groups(3, [2 * r, r, r])
        mn = random.choice(WIDE + WIDEW)
        if mn.endswith('.w'):
            vs2 = groups(3, [2 * r, 2 * r, r])[1]
            while vs2 // 8 == vd // 8: vs2 = groups(3, [2 * r, 2 * r, r])[1]
            vd, vs2, vs1 = groups(3, [2 * r, 2 * r, r])
        form = random.choice(['vv', 'vx']) if mn != 'wmaccus' else 'vx'
        macc = mn.startswith('wmacc')
        if mn.endswith('.w'):
            mn = mn[:-2]
            return f'v{mn}.wv v{vd}, v{vs2}, v{vs1}{m}' if form == 'vv' else f'v{mn}.wx v{vd}, v{vs2}, {xr()}{m}'
        if form == 'vv': return f'v{mn}.vv v{vd}, v{vs1}, v{vs2}{m}' if macc else f'v{mn}.vv v{vd}, v{vs2}, v{vs1}{m}'
        return f'v{mn}.vx v{vd}, {xr()}, v{vs2}{m}' if macc else f'v{mn}.vx v{vd}, v{vs2}, {xr()}{m}'
    if k < 71 and cfg.sew <= 32 and cfg.num <= 4:   # narrowing
        vd, vs2, vs1 = groups(3, [r, 2 * r, r])
        mn = random.choice(NARROW); form = random.choice(['wv', 'wx', 'wi'])
        src = f'v{vs1}' if form == 'wv' else xr() if form == 'wx' else str(imm5(False))
        return f'v{mn}.{form} v{vd}, v{vs2}, {src}{m}'
    if k < 76:   # reductions
        mn = random.choice(RED + (['wredsum', 'wredsumu'] if cfg.sew <= 32 and cfg.num <= 4 else []))
        vd, vs2, vs1 = groups(3, [1, r, 1])
        if random.random() < 0.3: vd = vs1
        return f'v{mn}.vs v{vd}, v{vs2}, v{vs1}{m}'
    if k < 80:   # mask logic
        vd, vs2, vs1 = (random.randrange(8, 32) for _ in range(3))
        return f'v{random.choice(MASKLOG)}.mm v{vd}, v{vs2}, v{vs1}'
    if k < 85:   # mask unary
        vd, vs2 = groups(2, [1, 1]); c = random.randrange(0, 6)
        if c < 3: return f'{random.choice(["vmsbf", "vmsof", "vmsif"])}.m v{vd}, v{vs2}{m}'
        if c == 3:
            d2, s2 = groups(2, [r, 1])
            return f'viota.m v{d2}, v{s2}{m}'
        if c == 4: return random.choice([f'vcpop.m x{random.randrange(1, 28)}, v{vs2}{m}', f'vfirst.m x{random.randrange(1, 28)}, v{vs2}{m}'])
        return f'vid.v v{groups(1, [r])[0]}{m}'
    if k < 91:   # slides, gather, compress
        vd, vs2, vs1 = groups(3, [r, r, r])
        c = random.randrange(0, 9)
        if c == 0: return f'vslideup.vx v{vd}, v{vs2}, {xr()}{m}'
        if c == 1: return f'vslideup.vi v{vd}, v{vs2}, {imm5(False)}{m}'
        if c == 2: return f'vslidedown.vx v{vd}, v{vs2}, {xr()}{m}'
        if c == 3: return f'vslidedown.vi v{vd}, v{vs2}, {imm5(False)}{m}'
        if c == 4: return random.choice([f'vslide1up.vx v{vd}, v{vs2}, {xr()}{m}', f'vslide1down.vx v{vd}, v{vs2}, {xr()}{m}'])
        if c == 5: return f'vrgather.vv v{vd}, v{vs2}, v{vs1}{m}'
        if c == 6: return random.choice([f'vrgather.vx v{vd}, v{vs2}, {xr()}{m}', f'vrgather.vi v{vd}, v{vs2}, {imm5(False)}{m}'])
        if c == 7:
            vd, vs2, mk = groups(3, [r, r, 1])
            return f'vcompress.vm v{vd}, v{vs2}, v{mk}'
        er = max(1, (16 * cfg.num // cfg.den) // cfg.sew)  # EMUL of the 16-bit index group
        n = max(1, 16 * cfg.num // cfg.den // cfg.sew) if False else None
        emul_num = 16 * cfg.num; emul_den = cfg.sew * cfg.den
        regs16 = max(1, emul_num // emul_den)
        if emul_num * 8 < emul_den or emul_num > 8 * emul_den: return 'nop'
        vd, vs2, vs1 = groups(3, [r, r, regs16])
        return f'vrgatherei16.vv v{vd}, v{vs2}, v{vs1}{m}'
    if k < 94:   # scalar moves
        c = random.randrange(0, 3)
        if c == 0: return f'vmv.x.s {xr()}, v{random.randrange(8, 32)}'
        if c == 1: return f'vmv.s.x v{random.randrange(8, 32)}, {xr()}'
        return random.choice([f'vmv1r.v v{random.randrange(8, 32)}, v{random.randrange(8, 32)}', f'vmv2r.v v{random.choice(range(8, 32, 2))}, v{random.choice(range(8, 32, 2))}',
                              f'vmv4r.v v{random.choice(range(8, 32, 4))}, v{random.choice(range(8, 32, 4))}', f'vmv8r.v v{random.choice(range(8, 32, 8))}, v{random.choice(range(8, 32, 8))}'])
    # extensions
    for f in (2, 4, 8):
        pass
    f = random.choice([2, 4, 8])
    if cfg.sew // f < 8: return 'nop'
    emul_src_den = cfg.den * f
    if emul_src_den // cfg.num > 8: return 'nop'
    rsrc = max(1, cfg.num // emul_src_den)
    vd, vs2 = groups(2, [r, rsrc])
    return f'{random.choice(["vzext", "vsext"])}.vf{f} v{vd}, v{vs2}{m}'

def int_case():
    cfg = Cfg()
    pre = cfg.set()
    if random.random() < 0.3: pre += f'\n csrwi vxrm, {random.randrange(0, 4)}'
    t = one_int_case(cfg)
    return pre + '\n ' + t.replace('\n', '\n ')

# --- memory ---------------------------------------------------------------------------------------------------------------------------------------
EEWS = {8: 'e8', 16: 'e16', 32: 'e32', 64: 'e64'}

def mem_case():
    cfg = Cfg()
    pre = cfg.set()
    m = mask_suffix()
    kind = random.randrange(0, 10)
    base = f'addi x27, x31, {BUF_OFF + BUF_BASE - BIAS}'
    eew = random.choice([8, 16, 32, 64])
    # EMUL of the data
    import math
    e_log = int(math.log2(eew // 8)) - int(math.log2(cfg.sew // 8)) + (int(math.log2(cfg.num)) - int(math.log2(cfg.den)))
    if e_log < -3 or e_log > 3: return None
    regs = max(1, 1 << e_log) if e_log > 0 else 1
    nf = 1
    if kind < 4 and random.random() < 0.4 and regs <= 4:
        nf = random.choice([n for n in range(2, 9) if n * regs <= 8])
    vd = random.randrange(8, 32 - nf * regs + 1) // regs * regs
    while vd < 8 or vd + nf * regs > 32: vd = random.randrange(8, 32) // regs * regs
    seg = f'seg{nf}' if nf > 1 else ''
    mns = {0: 'vle', 1: 'vse', 2: 'vlse', 3: 'vsse', 4: 'vluxei', 5: 'vsuxei', 6: 'vloxei', 7: 'vsoxei'}
    if kind == 8:   # whole registers, mask
        n = random.choice([1, 2, 4, 8])
        d = random.randrange(8, 32) // n * n
        if d < 8: d = 8 // n * n
        return pre + f'\n {base}\n ' + random.choice([f'vl{n}re{eew}.v v{d}, (x27)', f'vs{n}r.v v{d}, (x27)', 'vlm.v v%d, (x27)' % random.randrange(8, 32), 'vsm.v v%d, (x27)' % random.randrange(8, 32)])
    if kind == 9:   # fault-only-first
        return pre + f'\n {base}\n vle{eew}ff.v v{vd}, (x27){m}'
    mn = mns[kind]
    if kind in (0, 1):
        op = f'v{"l" if kind == 0 else "s"}{"seg" + str(nf) if nf > 1 else ""}e{eew}.v'.replace('vlseg', 'vlseg').replace('vsseg', 'vsseg')
        return pre + f'\n {base}\n {op} v{vd}, (x27){m}'
    if kind in (2, 3):
        # |stride| * vl must stay within the buffer
        stride = random.choice([0, 1, -1, 2, 3, -4, 5, 8, 16, -16, eew // 8, -(eew // 8) * nf, nf * eew // 8 + 3])
        if abs(stride) * cfg.vlmax > 440: return None
        op = f'v{"l" if kind == 2 else "s"}{"sseg" + str(nf) if nf > 1 else "se"}{"" if nf > 1 else ""}'
        op = f'vls{"seg" + str(nf) if nf > 1 else ""}e{eew}.v' if kind == 2 else f'vss{"seg" + str(nf) if nf > 1 else ""}e{eew}.v'
        return pre + f'\n {base}\n li x26, {stride}\n {op} v{vd}, (x27), x26{m}'
    # indexed: the data uses SEW, the index EEW is `eew`; build the index vector first
    d_regs = cfg.regs
    nfi = 1
    if random.random() < 0.3 and d_regs <= 4:
        nfi = random.choice([n for n in range(2, 9) if n * d_regs <= 8])
    vdi = random.randrange(8, 32 - nfi * d_regs + 1) // d_regs * d_regs
    while vdi < 8 or vdi + nfi * d_regs > 32: vdi = random.randrange(8, 32) // d_regs * d_regs
    i_log = int(math.log2(eew // 8)) - int(math.log2(cfg.sew // 8)) + (int(math.log2(cfg.num)) - int(math.log2(cfg.den)))
    if i_log < -3 or i_log > 3: return None
    iregs = max(1, 1 << i_log) if i_log > 0 else 1
    # the index group must not overlap the data group
    vi = random.randrange(8, 32) // iregs * iregs
    tries = 0
    while (vi < 8 or not (vi + iregs <= vdi or vi >= vdi + nfi * d_regs)) and tries < 100:
        vi = random.randrange(8, 32) // iregs * iregs; tries += 1
    if tries >= 100 or vi < 8: return None
    scale = random.choice([1, 2, 4, 8]) * nfi * cfg.sew // 8
    if scale * cfg.vlmax > 480: return None
    if eew == 8 and scale * cfg.vlmax > 255: return None
    ieew = f'e{eew}, m{max(1, 1 << i_log) if i_log >= 0 else "f" + str(1 << -i_log)}, ta, ma'
    # (EMUL / EEW of the index equals LMUL / SEW, so the same AVL gives the same vl)
    setup = f'\n {base}\n vsetvli x0, x29, {ieew}\n vid.v v{vi}\n li x26, {scale}\n vmul.vx v{vi}, v{vi}, x26\n vsetvli x0, x29, {cfg.vtype()}'
    seg = f'seg{nfi}' if nfi > 1 else ''
    op = {4: f'vluxei{eew}.v', 5: f'vsuxei{eew}.v', 6: f'vloxei{eew}.v', 7: f'vsoxei{eew}.v'}[kind]
    if nfi > 1: op = op.replace('vl', f'vl').replace('ei', f'seg{nfi}ei', 1)
    return pre + setup + f'\n {op} v{vdi}, (x27), v{vi}{m}'

# --- floating point ---------------------------------------------------------------------------------------------------------------------------
def fr(): return 'f%d' % random.randrange(0, 32)
FBIN = ['fadd', 'fsub', 'fmul', 'fdiv', 'fmin', 'fmax', 'fsgnj', 'fsgnjn', 'fsgnjx']
FFMA = ['fmacc', 'fnmacc', 'fmsac', 'fnmsac', 'fmadd', 'fnmadd', 'fmsub', 'fnmsub']
FCMP = ['mfeq', 'mfne', 'mflt', 'mfle']

def fp_case():
    if HALF and random.random() < 0.12:   # Zvfbfmin and Zvfbfwma
        cfg = Cfg(sew=16, max_lmul=4)
        pre = cfg.set(); m = mask_suffix(); r = cfg.regs
        c = random.randrange(0, 3)
        if c == 0:
            vd, vs2 = groups(2, [2 * r, r]); return pre + f'\n vfwcvtbf16.f.f.v v{vd}, v{vs2}{m}'
        if c == 1:
            vd, vs2 = groups(2, [r, 2 * r]); return pre + f'\n vfncvtbf16.f.f.w v{vd}, v{vs2}{m}'
        vd, vs2, vs1 = groups(3, [2 * r, r, r])
        if random.random() < 0.5: return pre + f'\n vfwmaccbf16.vv v{vd}, v{vs1}, v{vs2}{m}'
        return pre + f'\n vfwmaccbf16.vf v{vd}, {fr()}, v{vs2}{m}'
    if HALF and random.random() < 0.1:   # 8-bit integers <-> half precision
        cfg = Cfg(sew=8, max_lmul=4)
        pre = cfg.set(); m = mask_suffix(); r = cfg.regs
        if random.random() < 0.5:
            vd, vs2 = groups(2, [2 * r, r])
            return pre + f'\n {random.choice(["vfwcvt.f.xu.v", "vfwcvt.f.x.v"])} v{vd}, v{vs2}{m}'
        vd, vs2 = groups(2, [r, 2 * r])
        return pre + f'\n {random.choice(["vfncvt.xu.f.w", "vfncvt.x.f.w", "vfncvt.rtz.xu.f.w", "vfncvt.rtz.x.f.w"])} v{vd}, v{vs2}{m}'
    if random.random() < 0.08:   # 16-bit integers <-> single precision
        cfg = Cfg(sew=16, max_lmul=4)
        pre = cfg.set(); m = mask_suffix(); r = cfg.regs
        if random.random() < 0.5:
            vd, vs2 = groups(2, [2 * r, r])
            return pre + f'\n {random.choice(["vfwcvt.f.xu.v", "vfwcvt.f.x.v"])} v{vd}, v{vs2}{m}'
        vd, vs2 = groups(2, [r, 2 * r])
        return pre + f'\n {random.choice(["vfncvt.xu.f.w", "vfncvt.x.f.w", "vfncvt.rtz.xu.f.w", "vfncvt.rtz.x.f.w"])} v{vd}, v{vs2}{m}'
    k = random.randrange(0, 100)
    # widening and narrowing forms use SEW 32 (and 16 for integer <-> float); the rest 32 or 64
    cfg = Cfg(fp=True)
    pre = cfg.set()
    m = mask_suffix()
    r = cfg.regs
    if k < 35:
        mn = random.choice(FBIN + FFMA)
        form = random.choice(['vv', 'vf'])
        vd, vs2, vs1 = groups(3, [r, r, r])
        if random.random() < 0.2: vd = vs2
        src = f'v{vs1}' if form == 'vv' else fr()
        t = f'v{mn}.{form} v{vd}, {src}, v{vs2}{m}' if mn in FFMA else f'v{mn}.{form} v{vd}, v{vs2}, {src}{m}'
    elif k < 40:
        mn = random.choice(['frsub', 'frdiv']); vd, vs2 = groups(2, [r, r])
        t = f'v{mn}.vf v{vd}, v{vs2}, {fr()}{m}'
    elif k < 50:
        mn = random.choice(FCMP + ['mfgt', 'mfge'])
        form = 'vf' if mn in ('mfgt', 'mfge') else random.choice(['vv', 'vf'])
        vd, vs2, vs1 = groups(3, [1, r, r])
        t = f'v{mn}.{form} v{vd}, v{vs2}, ' + (f'v{vs1}' if form == 'vv' else fr()) + m
    elif k < 55:
        vd, vs2 = groups(2, [r, r]); t = f'{random.choice(["vfsqrt.v", "vfclass.v"])} v{vd}, v{vs2}{m}'
        if random.random() < 0.5:   # the 7-bit estimates, with operands at the interesting exponents
            fb, eb = (10, 5) if cfg.sew == 16 else (23, 8) if cfg.sew == 32 else (52, 11)
            e = random.choice([0, 0, 0, 1, 2, 3, (1 << eb) - 2, (1 << eb) - 3, (1 << eb) - 4, (1 << eb) - 1, random.randrange(0, 1 << eb), (1 << (eb - 1)) - 1, 1 << (eb - 1)])
            fr_ = random.choice([0, 1, 1 << (fb - 1), 1 << (fb - 2), (1 << (fb - 2)) - 1, (1 << fb) - 1, random.getrandbits(fb), random.getrandbits(fb) >> random.randrange(0, fb)])
            if random.random() < 0.3: e = 0; fr_ = random.choice([1 << (fb - 1), 1 << (fb - 2), (1 << (fb - 2)) - 1, (1 << (fb - 3)) + 5, (1 << (fb - 2)) + 7])
            pattern = (random.getrandbits(1) << (fb + eb)) | (e << fb) | fr_
            t = f'li x26, {pattern}\n vmv.v.x v{vs2}, x26\n {random.choice(["vfrec7.v", "vfrsqrt7.v"])} v{vd}, v{vs2}{m}'
    elif k < 62:
        vd, vs2 = groups(2, [r, r]); c = random.randrange(0, 5)
        if c == 0: t = f'vfmerge.vfm v{vd}, v{vs2}, {fr()}, v0'
        elif c == 1: t = f'vfmv.v.f v{vd}, {fr()}'
        elif c == 2: t = f'vfmv.f.s {fr()}, v{vs2}'
        elif c == 3: t = f'vfmv.s.f v{vd}, {fr()}'
        else: t = f'{random.choice(["vfslide1up", "vfslide1down"])}.vf v{vd}, v{vs2}, {fr()}{m}'
    elif k < 70:
        mn = random.choice(['fredusum', 'fredosum', 'fredmin', 'fredmax'])
        vd, vs2, vs1 = groups(3, [1, r, 1])
        t = f'v{mn}.vs v{vd}, v{vs2}, v{vs1}{m}'
    elif k < 80:   # same-width conversions
        vd, vs2 = groups(2, [r, r])
        t = f'{random.choice(["vfcvt.xu.f.v", "vfcvt.x.f.v", "vfcvt.rtz.xu.f.v", "vfcvt.rtz.x.f.v", "vfcvt.f.xu.v", "vfcvt.f.x.v"])} v{vd}, v{vs2}{m}'
    elif k < 90:   # widening
        if cfg.sew not in ((16, 32) if HALF else (32,)) or cfg.num > 4: return pre + '\n nop'
        vd, vs2, vs1 = groups(3, [2 * r, r, r]); c = random.randrange(0, 3)
        if c == 0:
            t = f'{random.choice(["vfwcvt.xu.f.v", "vfwcvt.x.f.v", "vfwcvt.rtz.xu.f.v", "vfwcvt.rtz.x.f.v", "vfwcvt.f.f.v"])} v{vd}, v{vs2}{m}'
        elif c == 1:
            mn = random.choice(['fwadd', 'fwsub', 'fwmul', 'fwmacc', 'fwnmacc', 'fwmsac', 'fwnmsac']); form = random.choice(['vv', 'vf'])
            src = f'v{vs1}' if form == 'vv' else fr()
            t = f'v{mn}.{form} v{vd}, {src}, v{vs2}{m}' if mn.endswith(('macc', 'msac')) else f'v{mn}.{form} v{vd}, v{vs2}, {src}{m}'
        else:
            mn = random.choice(['fwadd', 'fwsub']); form = random.choice(['wv', 'wf'])
            vd, vs2, vs1 = groups(3, [2 * r, 2 * r, r])
            t = f'v{mn}.{form} v{vd}, v{vs2}, ' + (f'v{vs1}' if form == 'wv' else fr()) + m
    else:
        if cfg.sew not in ((16, 32) if HALF else (32,)) or cfg.num > 4: return pre + '\n nop'
        vd, vs2, vs1 = groups(3, [r, 2 * r, r]); c = random.randrange(0, 3)
        if c == 0:
            t = f'{random.choice(["vfncvt.xu.f.w", "vfncvt.x.f.w", "vfncvt.rtz.xu.f.w", "vfncvt.rtz.x.f.w", "vfncvt.f.xu.w", "vfncvt.f.x.w", "vfncvt.f.f.w", "vfncvt.rod.f.f.w"])} v{vd}, v{vs2}{m}'
        elif c == 1:
            vd, vs2, vs1 = groups(3, [1, r, 1]); t = f'{random.choice(["vfwredusum", "vfwredosum"])}.vs v{vd}, v{vs2}, v{vs1}{m}'
        else:
            t = f'{random.choice(["vfwcvt.f.xu.v", "vfwcvt.f.x.v"])} v{groups(2, [2 * r, r])[0]}, v{groups(2, [2 * r, r])[1]}{m}'
            vd, vs2 = groups(2, [2 * r, r]); t = f'{random.choice(["vfwcvt.f.xu.v", "vfwcvt.f.x.v"])} v{vd}, v{vs2}{m}'
    return pre + '\n ' + t


def bit_case():
    """Zvbb, Zvkb and Zvbc: vandn, vror, vrol, vwsll, the unary bit instructions, vclmul and vclmulh"""
    k = random.randrange(0, 100)
    if k < 12:      # vclmul needs SEW = 64
        cfg = Cfg(sew=64)
    else:
        cfg = Cfg()
    r = cfg.regs
    m = mask_suffix()
    pre = cfg.set()
    if k < 12:
        vd, vs2, vs1 = groups(3, [r, r, r])
        mn = random.choice(['vclmul', 'vclmulh'])
        if random.random() < 0.5: return f'{pre}\n {mn}.vv v{vd}, v{vs2}, v{vs1}{m}'
        return f'{pre}\n {mn}.vx v{vd}, v{vs2}, {xr()}{m}'
    if k < 40:
        vd, vs2, vs1 = groups(3, [r, r, r])
        if random.random() < 0.2: vd = vs2
        mn = random.choice(['vandn', 'vror', 'vrol'])
        forms = ['vv', 'vx'] + (['vi'] if mn == 'vror' else [])
        form = random.choice(forms)
        if form == 'vv': return f'{pre}\n {mn}.vv v{vd}, v{vs2}, v{vs1}{m}'
        if form == 'vx': return f'{pre}\n {mn}.vx v{vd}, v{vs2}, {xr()}{m}'
        return f'{pre}\n vror.vi v{vd}, v{vs2}, {random.choice([0, 1, 7, 31, 32, 33, 63, random.randrange(0, 64)])}{m}'
    if k < 60 and cfg.sew <= 32 and cfg.num <= 4:
        vd, vs2, vs1 = groups(3, [2 * r, r, r])
        form = random.choice(['vv', 'vx', 'vi'])
        if form == 'vv': return f'{pre}\n vwsll.vv v{vd}, v{vs2}, v{vs1}{m}'
        if form == 'vx': return f'{pre}\n vwsll.vx v{vd}, v{vs2}, {xr()}{m}'
        return f'{pre}\n vwsll.vi v{vd}, v{vs2}, {random.choice([0, 1, 7, 15, 31, random.randrange(0, 32)])}{m}'
    vd, vs2, _ = groups(3, [r, r, r])
    if random.random() < 0.2: vd = vs2
    return f'{pre}\n {random.choice(["vbrev8", "vrev8", "vbrev", "vclz", "vctz", "vcpop"])}.v v{vd}, v{vs2}{m}'
