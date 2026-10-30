#!/usr/bin/env python3
# Differential tester for the AArch64 emulator. Usage: gen.py <int|fp|neon|mem> <seed> <workdir>  (see ../aarch64_diff.sh)
# Generates an AArch64 test program: for each case, load random state, run one instruction, dump state.
import random, subprocess, sys, struct
WORK = sys.argv[3]
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
mode = sys.argv[1]

SPECIAL64 = [0, 1, 2, 3, 0xFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0x0123456789ABCDEF, 0xFEDCBA9876543210]
SF64 = [0x0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x4000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000,
        0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF, 0x4059000000000000, 0xC059000000000000, 0x3FE0000000000000, 0x43E0000000000000, 0x41DFFFFFFFC00000]
SF32 = [0x0, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FA00000, 1, 0x007FFFFF, 0x7F7FFFFF, 0x42C80000, 0xC2C80000, 0x3F000000, 0x4F000000]

def r64():
    k = random.random()
    if k < 0.25: return random.choice(SPECIAL64)
    if k < 0.4: return random.getrandbits(8)
    if k < 0.5: return random.getrandbits(32)
    return random.getrandbits(64)
def v64(fp):
    if fp == 'h': return vh()
    if fp == 'mix':
        k = random.random()
        if k < 0.25: return vh()
        if k < 0.55: return v64(True)
        return r64()
    k = random.random()
    if fp and k < 0.45:
        if random.random() < 0.5: return random.choice(SF64)
        return random.choice(SF32) | (random.choice(SF32) << 32)
    if fp and k < 0.7:  # near-ordinary doubles/floats
        import struct as s
        if random.random() < 0.5:
            return struct.unpack('<Q', struct.pack('<d', random.uniform(-1000, 1000)))[0]
        a = struct.unpack('<I', struct.pack('<f', random.uniform(-1000, 1000)))[0]
        b = struct.unpack('<I', struct.pack('<f', random.uniform(-1000, 1000)))[0]
        return a | (b << 32)
    return r64()

SF16 = [0x0, 0x8000, 0x3C00, 0xBC00, 0x4000, 0x7C00, 0xFC00, 0x7E00, 0x7D00, 0x0001, 0x03FF, 0x7BFF, 0x5640, 0xD640, 0x3800, 0x5BE0, 0x0400, 0x8001, 0xFBFF, 0x3555]
def vh():
    # four packed half-precision values, mostly special or ordinary
    out = 0
    for i in range(4):
        k = random.random()
        h = random.choice(SF16) if k < 0.4 else (struct.unpack('<H', struct.pack('<e', random.uniform(-100, 100)))[0] if k < 0.8 else random.getrandbits(16))
        out |= h << (16 * i)
    return out

def pred16():
    # a predicate for a 128-bit vector: random, all true, element-aligned patterns for each element size, or sparse
    k = random.random()
    if k < 0.3: return random.getrandbits(16)
    if k < 0.4: return 0xFFFF
    if k < 0.45: return 0
    if k < 0.85:
        esz = random.randrange(4); out = 0
        for i in range(16 >> esz):
            if random.random() < 0.6: out |= 1 << (i << esz)
        return out
    return 1 << random.randrange(16)

def regs3(): return [random.randrange(0, 31) for _ in range(3)]
def fp(): return random.randrange(0, 32)

cases = []
def add(text): cases.append(text)

def X(n, sf=True): return ('x%d' if sf else 'w%d') % n

if mode == 'int':
    for _ in range(6000):
        sf = random.random() < 0.6
        d, n, m = [random.randrange(0, 30) for _ in range(3)]
        R = lambda k: X(k, sf)
        c = random.choice(['eq','ne','cs','cc','mi','pl','vs','vc','hi','ls','ge','lt','gt','le'])
        sh = random.choice(['lsl','lsr','asr','ror']); amt = random.randrange(0, 64 if sf else 32)
        ext = random.choice(['uxtb','uxth','uxtw','uxtx','sxtb','sxth','sxtw','sxtx']); eamt = random.randrange(0, 5)
        imm12 = random.randrange(0, 4096); lg = random.choice(['0','12'])
        bm = random.choice(['0xff',('0xf0f0f0f0f0f0f0f0' if sf else '0xf0f0f0f0'),'0x5555555555555555' if sf else '0x55555555','0x1','0x7fffffff',('0xffff0000ffff0000' if sf else '0xffff0000'),('0x3ffffffe3ffffffe' if sf else '0x3ffffffe'), '0xff00ff00ff00ff00' if sf else '0xff00ff00'])
        lsb = random.randrange(0, 32 if not sf else 64); wid = random.randrange(1, (32 if not sf else 64) - lsb + 1)
        op = random.randrange(0, 40)
        t = [
         lambda: f'add {R(d)}, {R(n)}, {R(m)}, {sh if sh!="ror" else "lsl"} #{amt}',
         lambda: f'sub {R(d)}, {R(n)}, {R(m)}, {sh if sh!="ror" else "lsr"} #{amt}',
         lambda: f'adds {R(d)}, {R(n)}, {R(m)}',
         lambda: f'subs {R(d)}, {R(n)}, {R(m)}, {sh if sh!="ror" else "asr"} #{amt}',
         lambda: f'add {X(d)}, {X(n)}, {X(m, ext[-1]=="x")}, {ext} #{eamt}',
         lambda: f'subs {X(d)}, {X(n)}, {X(m, ext[-1]=="x")}, {ext} #{eamt}',
         lambda: f'add {R(d)}, {R(n)}, #{imm12}' + ('' if lg=='0' else ', lsl #12'),
         lambda: f'subs {R(d)}, {R(n)}, #{imm12}',
         lambda: f'adc {R(d)}, {R(n)}, {R(m)}',
         lambda: f'sbcs {R(d)}, {R(n)}, {R(m)}',
         lambda: f'and {R(d)}, {R(n)}, {R(m)}, {sh} #{amt}',
         lambda: f'bics {R(d)}, {R(n)}, {R(m)}, {sh} #{amt}',
         lambda: f'orn {R(d)}, {R(n)}, {R(m)}, {sh} #{amt}',
         lambda: f'eon {R(d)}, {R(n)}, {R(m)}',
         lambda: f'and {R(d)}, {R(n)}, #{bm}',
         lambda: f'eor {R(d)}, {R(n)}, #{bm}',
         lambda: f'ands {R(d)}, {R(n)}, #{bm}',
         lambda: f'orr {R(d)}, {R(n)}, #{bm}',
         lambda: f'csel {R(d)}, {R(n)}, {R(m)}, {c}',
         lambda: f'csinc {R(d)}, {R(n)}, {R(m)}, {c}',
         lambda: f'csinv {R(d)}, {R(n)}, {R(m)}, {c}',
         lambda: f'csneg {R(d)}, {R(n)}, {R(m)}, {c}',
         lambda: f'ccmp {R(n)}, {R(m)}, #{random.randrange(16)}, {c}',
         lambda: f'ccmn {R(n)}, #{random.randrange(32)}, #{random.randrange(16)}, {c}',
         lambda: f'ubfx {R(d)}, {R(n)}, #{lsb}, #{wid}',
         lambda: f'sbfx {R(d)}, {R(n)}, #{lsb}, #{wid}',
         lambda: f'bfi {R(d)}, {R(n)}, #{lsb}, #{wid}',
         lambda: f'bfxil {R(d)}, {R(n)}, #{lsb}, #{wid}',
         lambda: f'extr {R(d)}, {R(n)}, {R(m)}, #{random.randrange(32 if not sf else 64)}',
         lambda: f'madd {R(d)}, {R(n)}, {R(m)}, {R(random.randrange(30))}',
         lambda: f'msub {R(d)}, {R(n)}, {R(m)}, {R(random.randrange(30))}',
         lambda: f'smaddl {X(d)}, {X(n,False)}, {X(m,False)}, {X(random.randrange(30))}',
         lambda: f'umsubl {X(d)}, {X(n,False)}, {X(m,False)}, {X(random.randrange(30))}',
         lambda: f'smulh {X(d)}, {X(n)}, {X(m)}',
         lambda: f'umulh {X(d)}, {X(n)}, {X(m)}',
         lambda: f'udiv {R(d)}, {R(n)}, {R(m)}',
         lambda: f'sdiv {R(d)}, {R(n)}, {R(m)}',
         lambda: f'{random.choice(["lsl","lsr","asr","ror"])} {R(d)}, {R(n)}, {R(m)}',
         lambda: f'{random.choice(["rbit","rev","rev16","clz","cls"])} {R(d)}, {R(n)}',
         lambda: f'rev32 {X(d)}, {X(n)}',
         lambda: f'movk {R(d)}, #{random.randrange(65536)}, lsl #{random.choice([0,16] + ([32,48] if sf else []))}',
        ][op]
        add(t())
    # moves / adr are position dependent; skip
elif mode == 'fp':
    for _ in range(5000):
        d, n, m, a = fp(), fp(), fp(), fp()
        dbl = random.random() < 0.5
        P = 'd' if dbl else 's'
        S = lambda k: f'{P}{k}'
        op = random.randrange(0, 30)
        c = random.choice(['eq','ne','cs','cc','mi','pl','vs','vc','hi','ls','ge','lt','gt','le'])
        sf = random.random() < 0.5
        t = [
         lambda: f'fadd {S(d)}, {S(n)}, {S(m)}', lambda: f'fsub {S(d)}, {S(n)}, {S(m)}', lambda: f'fmul {S(d)}, {S(n)}, {S(m)}', lambda: f'fdiv {S(d)}, {S(n)}, {S(m)}',
         lambda: f'fnmul {S(d)}, {S(n)}, {S(m)}', lambda: f'fmax {S(d)}, {S(n)}, {S(m)}', lambda: f'fmin {S(d)}, {S(n)}, {S(m)}', lambda: f'fmaxnm {S(d)}, {S(n)}, {S(m)}', lambda: f'fminnm {S(d)}, {S(n)}, {S(m)}',
         lambda: f'fmadd {S(d)}, {S(n)}, {S(m)}, {S(a)}', lambda: f'fmsub {S(d)}, {S(n)}, {S(m)}, {S(a)}', lambda: f'fnmadd {S(d)}, {S(n)}, {S(m)}, {S(a)}', lambda: f'fnmsub {S(d)}, {S(n)}, {S(m)}, {S(a)}',
         lambda: f'fsqrt {S(d)}, {S(n)}', lambda: f'fabs {S(d)}, {S(n)}', lambda: f'fneg {S(d)}, {S(n)}',
         lambda: f'{random.choice(["frintn","frintp","frintm","frintz","frinta","frintx","frinti"])} {S(d)}, {S(n)}',
         lambda: f'fcmp {S(n)}, {S(m)}', lambda: f'fcmpe {S(n)}, #0.0', lambda: f'fccmp {S(n)}, {S(m)}, #{random.randrange(16)}, {c}', lambda: f'fcsel {S(d)}, {S(n)}, {S(m)}, {c}',
         lambda: f'fcvt {"s" if dbl else "d"}{d}, {S(n)}',
         lambda: f'{random.choice(["fcvtzs","fcvtzu","fcvtns","fcvtnu","fcvtms","fcvtmu","fcvtps","fcvtpu","fcvtas","fcvtau"])} {X(d % 31, sf)}, {S(n)}',
         lambda: f'{random.choice(["scvtf","ucvtf"])} {S(d)}, {X(n % 31, sf)}',
         lambda: f'{random.choice(["scvtf","ucvtf","fcvtzs","fcvtzu"])} ' + (f'{S(d)}, {X(n % 31, sf)}, #{random.randrange(1, 32)}' if False else f'{S(d)}, {X(n % 31, sf)}, #{random.randrange(1, 32)}') if False else f'scvtf {S(d)}, {X(n % 31, sf)}, #{random.randrange(1, 32)}',
         lambda: f'fcvtzs {X(d % 31, sf)}, {S(n)}, #{random.randrange(1, 32)}',
         lambda: f'fmov {S(d)}, #{random.choice(["1.0","-2.5","0.125","31.0","-0.5","10.0"])}',
         lambda: f'fmov {X(d % 31, dbl)}, {S(n)}',
         lambda: f'fmov {S(d)}, {X(n % 31, dbl)}',
         lambda: f'fmov x{d%31}, v{n}.d[1]',
        ][op]
        add(t())
elif mode == 'mem':
    for _ in range(7000):
        t = random.randrange(0, 9)
        rt = random.randrange(0, 26); rt2 = random.randrange(0, 26)
        while rt2 == rt: rt2 = random.randrange(0, 26)
        sz = random.choice(['b','h','w','x']); W = lambda k, z: ('x%d' if z == 'x' else 'w%d') % k
        if t == 0:   # integer loads and stores, unsigned/unscaled/pre/post
            m = random.choice(['ldr','str','ldrb','strb','ldrh','strh','ldrsb','ldrsh','ldrsw','ldur','stur','ldurb','sturh','ldursw','ldrsb_w','ldrsh_w'])
            z = 'x' if m in ('ldrsw','ldursw') else random.choice(['w','x'])
            nm = m.replace('_w', '')
            if m.endswith('_w'): z = 'w'
            if m in ('ldr','str','ldur','stur'): reg = f'{z}{rt}'; scale = 3 if z == 'x' else 2
            elif m in ('ldrb','strb','ldurb'): reg = f'w{rt}'; scale = 0
            elif m in ('ldrh','strh','sturh'): reg = f'w{rt}'; scale = 1
            elif m in ('ldrsb','ldrsb_w'): reg = f'{z}{rt}'; scale = 0
            elif m in ('ldrsh','ldrsh_w'): reg = f'{z}{rt}'; scale = 1
            else: reg = f'x{rt}'; scale = 2
            form = random.choice(['u','pre','post']) if not nm.startswith(('ldur','stur')) else 'un'
            if form == 'u': txt = f'{nm} {reg}, [x28, #{random.randrange(0, 20) << scale}]'
            elif form == 'un': txt = f'{nm} {reg}, [x28, #{random.randrange(-60, 60)}]'
            elif form == 'pre': txt = f'{nm} {reg}, [x28, #{random.randrange(-40, 40)}]!'
            else: txt = f'{nm} {reg}, [x28], #{random.randrange(-40, 40)}'
            add(txt)
        elif t == 1:   # register offset
            m = random.choice(['ldr','str','ldrb','strh','ldrsw','ldrsb','ldrh'])
            ext = random.choice(['lsl','uxtw','sxtw','sxtx']); xr = 'w27' if ext in ('uxtw','sxtw') else 'x27'
            if ext == 'sxtx': ext = 'lsl'; xr = 'x27'
            z = 'x' if m == 'ldrsw' else random.choice(['w','x'])
            reg = f'{z}{rt}' if m in ('ldr','str','ldrsw','ldrsb') else f'w{rt}'
            sc = {'ldr': 3 if z=='x' else 2, 'str': 3 if z=='x' else 2, 'ldrb': 0, 'strh': 1, 'ldrsw': 2, 'ldrsb': 0, 'ldrh': 1}[m]
            amt = random.choice([0, sc]) if ext != 'lsl' or True else 0
            amt_txt = f' #{amt}' if (amt or ext != 'lsl') else ''
            ext_txt = f', {ext}{amt_txt}' if amt_txt or ext != 'lsl' else ''
            val = random.choice([0, 1, 2, 3, 5, 8] if ext in ('uxtw',) or xr == 'x27' and ext == 'lsl' and False else [0, 1, 2, 3, 5, 8, -1, -4, -8])
            if ext == 'uxtw': val = random.choice([0, 1, 2, 3, 5, 8])
            add(f'mov x27, #{val}\n  {m} {reg}, [x28, {xr}{ext_txt}]')
        elif t == 2:   # pairs
            m = random.choice(['ldp','stp','ldpsw','ldp','stp']); z = 'x' if m == 'ldpsw' else random.choice(['w','x']); sc = 3 if z == 'x' else 2
            if m == 'ldpsw': sc = 2
            form = random.choice(['u','pre','post']); imm = random.randrange(-16, 16) << sc
            if form == 'u': add(f'{m} {z}{rt}, {z}{rt2}, [x28, #{imm}]')
            elif form == 'pre': add(f'{m} {z}{rt}, {z}{rt2}, [x28, #{imm}]!')
            else: add(f'{m} {z}{rt}, {z}{rt2}, [x28], #{imm}')
        elif t == 3:   # SIMD&FP scalar loads and stores and pairs
            p = random.choice(['b','h','s','d','q']); sc = {'b':0,'h':1,'s':2,'d':3,'q':4}[p]
            m = random.choice(['ldr','str'])
            form = random.choice(['u','un','pre','post','reg'])
            if form == 'u': add(f'{m} {p}{rt}, [x28, #{random.randrange(0, 12) << sc}]')
            elif form == 'un': add(f'{"ldur" if m=="ldr" else "stur"} {p}{rt}, [x28, #{random.randrange(-60, 60)}]')
            elif form == 'pre': add(f'{m} {p}{rt}, [x28, #{random.randrange(-40, 40)}]!')
            elif form == 'post': add(f'{m} {p}{rt}, [x28], #{random.randrange(-40, 40)}')
            else: add(f'mov x27, #{random.choice([0,1,2,4])}\n  {m} {p}{rt}, [x28, x27, lsl #{sc}]')
            if p in ('s','d','q') and random.random() < 0.5:
                mp = random.choice(['ldp','stp']); form = random.choice(['u','pre','post']); imm = random.randrange(-8, 8) << sc
                cases.pop()
                if form == 'u': add(f'{mp} {p}{rt}, {p}{rt2}, [x28, #{imm}]')
                elif form == 'pre': add(f'{mp} {p}{rt}, {p}{rt2}, [x28, #{imm}]!')
                else: add(f'{mp} {p}{rt}, {p}{rt2}, [x28], #{imm}')
        elif t == 4:   # structure loads/stores, multiple
            nreg = random.choice([1,2,3,4]); arr = random.choice(['8b','16b','4h','8h','2s','4s','2d'])
            if arr == '1d': pass
            m = random.choice(['ld','st']) + random.choice(['1','1','2','3','4'])
            if m[2] != '1': nreg = int(m[2])
            if m[2] != '1' and arr == '2d' and False: arr = '4s'
            regs = ', '.join(f'v{(rt + i) % 32}.{arr}' for i in range(nreg))
            post = random.choice(['', 'imm', 'reg'])
            if post == '': add(f'{m} {{{regs}}}, [x28]')
            elif post == 'imm':
                bytes_ = nreg * (16 if arr[-1] in 'b' and arr.startswith('16') or arr in ('8h','4s','2d') else 8)
                bytes_ = nreg * (16 if arr in ('16b','8h','4s','2d') else 8)
                add(f'{m} {{{regs}}}, [x28], #{bytes_}')
            else: add(f'mov x27, #{random.choice([8,16,24,32])}\n  {m} {{{regs}}}, [x28], x27')
        elif t == 5:   # single structure: lane, replicate
            e = random.choice('bhsd'); n_ = random.choice([1,2,3,4]); w_ = {'b':8,'h':16,'s':32,'d':64}[e]
            rep = random.random() < 0.3
            if rep:
                arr = random.choice(['8b','16b','4h','8h','2s','4s','1d','2d']); regs = ', '.join(f'v{(rt + i) % 32}.{arr}' for i in range(n_))
                add(f'ld{n_}r {{{regs}}}, [x28]' if random.random() < 0.5 else f'ld{n_}r {{{regs}}}, [x28], #{n_ * {"b":1,"h":2,"s":4,"d":8}[arr[-1]]}')
            else:
                regs = ', '.join(f'v{(rt + i) % 32}.{e}' for i in range(n_)); idx = random.randrange(128 // w_)
                m = random.choice(['ld','st']) + str(n_)
                if random.random() < 0.5: add(f'{m} {{{regs}}}[{idx}], [x28]')
                else: add(f'{m} {{{regs}}}[{idx}], [x28], #{n_ * w_ // 8}')
        elif t == 6:   # LSE atomics
            op = random.choice(['ldadd','ldclr','ldeor','ldset','ldsmax','ldsmin','ldumax','ldumin','swp'])
            sfx = random.choice(['','a','l','al']); sz = random.choice(['','b','h']); z = 'x' if sz == '' and random.random() < 0.5 else 'w'
            add(f'{op}{sfx}{sz} {z}{rt}, {z}{rt2}, [x28]')
        elif t == 7:   # compare-and-swap, exclusives
            kind = random.choice(['cas','casa','casl','casal','casb','cash','ldxr_stxr','ldar_stlr'])
            z = random.choice(['w','x'])
            if kind.startswith('cas'):
                zz = 'w' if kind in ('casb','cash') else z
                add(f'ldr x{rt2}, [x28]\n  mov x{rt}, x{rt2}\n  {kind} {zz}{rt}, {zz}{rt2 if random.random()<0.5 else (rt+1)%26}, [x28]' if random.random() < 0.6 else f'{kind} {zz}{rt}, {zz}{rt2}, [x28]')
            elif kind == 'ldxr_stxr':
                add(f'ldxr {z}{rt}, [x28]\n  stxr w{rt2}, {z}{rt}, [x28]')
            else:
                add(f'ldar {z}{rt}, [x28]\n  stlr {z}{rt2}, [x28, #8]' if False else f'ldar {z}{rt}, [x28]\n  stlr {z}{rt2}, [x28]')
        else:   # literal-free address generation and prefetch
            add(random.choice([f'prfm pldl1keep, [x28]', f'ldrsw x{rt}, [x28, #4]', f'ldr w{rt}, [x28, #-4]']))
elif mode == 'neon':
    arrs = ['8b','16b','4h','8h','2s','4s','2d']
    for _ in range(9000):
        d, n, m = random.randrange(32), random.randrange(32), random.randrange(32)
        arr = random.choice(arrs); a2 = random.choice(['8b','4h','2s','16b','8h','4s'])
        q = arr.startswith(('16','8h','4s','2d')) and arr not in ('4h','2s')
        bigarr = random.choice(['16b','8h','4s','2d']); smallarr = random.choice(['8b','4h','2s'])
        fa = random.choice(['2s','4s','2d'])
        el = {'8b':'b','16b':'b','4h':'h','8h':'h','2s':'s','4s':'s','2d':'d'}
        op = random.choice([
         'add','sub','mul','mla','mls','and','orr','eor','bic','orn','bsl','bit','bif','cmeq','cmgt','cmge','cmhi','cmhs','cmtst','smax','umax','smin','umin','sabd','uabd','saba','uaba','shadd','uhadd','srhadd','urhadd','shsub','uhsub','sqadd','uqadd','sqsub','uqsub','sshl','ushl','addp','smaxp','umaxp','sminp','uminp',
         'not','neg','abs','cnt','clz','cls','rev64','rev32','rev16','rbit','cmeq0','cmgt0','cmge0','cmle0','cmlt0','xtn','xtn2','uaddlp','saddlp','uadalp','sadalp','shll',
         'addv','uaddlv','saddlv','umaxv','uminv','smaxv','sminv','uzp1','uzp2','zip1','zip2','trn1','trn2','ext','dup_e','dup_g','ins_g','ins_e','umov','smov',
         'movi','mvni','orri','bici','shl','sshr','ushr','ssra','usra','sli','sri','shrn','shrn2','ushll','sshll','ushll2','sshll2','uaddl','saddl','uaddw','saddw','usubl','ssubl','umull','smull','umlal','smlal','umull2','smull2','uabdl','mul_e','mla_e','umull_e',
         'fadd','fsub','fmul','fdiv','fmax','fmin','fmaxnm','fminnm','fmla','fmls','fabd','faddp','fcmeq','fcmge','fcmgt','facge','fabs','fneg','fsqrt','scvtf','ucvtf','fcvtzs','fcvtzu','fcvtns','fcvtms','fcvtps','fcvtas','frintn','frintz','frintm','frintp','frinta','fcmeq0','fcmgt0','fcmlt0','fmla_e','fmul_e','fcvtl','fcvtn','tbl','faddp_s','addp_s'])
        sa = f'v{d}.{arr}, v{n}.{arr}, v{m}.{arr}'
        two = f'v{d}.{arr}, v{n}.{arr}'
        w = {'8b':8,'16b':8,'4h':16,'8h':16,'2s':32,'4s':32,'2d':64}[arr]
        def sh_right(): return random.randrange(1, w + 1)
        def sh_left(): return random.randrange(0, w)
        if op in ('and','orr','eor','bic','orn','bsl','bit','bif','not','cnt','rbit'):
            arr = random.choice(['8b','16b']); sa = f'v{d}.{arr}, v{n}.{arr}, v{m}.{arr}'; two = f'v{d}.{arr}, v{n}.{arr}'
        if op in ('mul','mla','mls','mul_e','mla_e') and arr in ('2d',): arr = '4s'; sa = f'v{d}.{arr}, v{n}.{arr}, v{m}.{arr}'
        intarr = lambda: random.choice(['8b','16b','4h','8h','2s','4s'])
        if op in ('mul','mla','mls','sabd','uabd','saba','uaba','smax','umax','smin','umin','shadd','uhadd','srhadd','urhadd','shsub','uhsub','smaxp','umaxp','sminp','uminp','cls','clz','rev32','rev16','rev64','sqadd','uqadd','sqsub','uqsub'):
            if arr == '2d': arr = '4s'
            sa = f'v{d}.{arr}, v{n}.{arr}, v{m}.{arr}'; two = f'v{d}.{arr}, v{n}.{arr}'
        if op in ('rev32',) and arr in ('4s','2s'): arr = '8h'; two = f'v{d}.{arr}, v{n}.{arr}'
        if op in ('rev16',): arr = random.choice(['8b','16b']); two = f'v{d}.{arr}, v{n}.{arr}'
        txt = None
        if op in ('add','sub','and','orr','eor','bic','orn','bsl','bit','bif','cmeq','cmgt','cmge','cmhi','cmhs','cmtst','smax','umax','smin','umin','sabd','uabd','saba','uaba','shadd','uhadd','srhadd','urhadd','shsub','uhsub','sqadd','uqadd','sqsub','uqsub','sshl','ushl','mul','mla','mls','addp','smaxp','umaxp','sminp','uminp'):
            txt = f'{op} {sa}'
        elif op in ('not','neg','abs','cnt','clz','cls','rev64','rev32','rev16','rbit','uaddlp','saddlp','uadalp','sadalp'):
            if op in ('uaddlp','saddlp','uadalp','sadalp') and arr == '2d': arr='4s'
            txt = f'{op} v{d}.{arr}, v{n}.{arr}' if op not in ('uaddlp','saddlp','uadalp','sadalp') else None
            if txt is None:
                dst = {'8b':'4h','16b':'8h','4h':'2s','8h':'4s','2s':'1d','4s':'2d'}[arr if arr!='2d' else '4s']
                txt = f'{op} v{d}.{dst}, v{n}.{arr if arr!="2d" else "4s"}'
        elif op in ('cmeq0','cmgt0','cmge0','cmle0','cmlt0'):
            txt = f'{op[:-1]} v{d}.{arr}, v{n}.{arr}, #0'
        elif op in ('xtn','xtn2'):
            src = {'8b':'8h','16b':'8h','4h':'4s','8h':'4s','2s':'2d','4s':'2d','2d':'2d'}[arr]; dst = {'8h':'8b','4s':'4h','2d':'2s'}[src]
            txt = f'xtn v{d}.{dst}, v{n}.{src}' if op=='xtn' else f'xtn2 v{d}.{ {"8h":"16b","4s":"8h","2d":"4s"}[src] }, v{n}.{src}'
        elif op == 'shll':
            src = random.choice(['8b','4h','2s']); dst = {'8b':'8h','4h':'4s','2s':'2d'}[src]; sh = {'8b':8,'4h':16,'2s':32}[src]
            txt = f'shll v{d}.{dst}, v{n}.{src}, #{sh}'
        elif op in ('addv','umaxv','uminv','smaxv','sminv'):
            arr = random.choice(['8b','16b','4h','8h','4s']); txt = f'{op} {el[arr]}{d}, v{n}.{arr}'
        elif op in ('uaddlv','saddlv'):
            arr = random.choice(['8b','16b','4h','8h','4s']); dstp = {'b':'h','h':'s','s':'d'}[el[arr]]; txt = f'{op} {dstp}{d}, v{n}.{arr}'
        elif op in ('uzp1','uzp2','zip1','zip2','trn1','trn2'):
            txt = f'{op} {sa}'
        elif op == 'ext':
            arr = random.choice(['8b','16b']); mx = 7 if arr=='8b' else 15
            txt = f'ext v{d}.{arr}, v{n}.{arr}, v{m}.{arr}, #{random.randrange(mx+1)}'
        elif op == 'dup_e':
            e = el[arr]; w_ = {'b':8,'h':16,'s':32,'d':64}[e]; idx = random.randrange(128 // w_)
            txt = f'dup v{d}.{arr}, v{n}.{e}[{idx}]'
        elif op == 'dup_g':
            txt = f'dup v{d}.{arr}, {"x" if arr=="2d" else "w"}{n % 31}'
        elif op == 'ins_g':
            e = random.choice('bhsd'); w_ = {'b':8,'h':16,'s':32,'d':64}[e]; txt = f'mov v{d}.{e}[{random.randrange(128//w_)}], {"x" if e=="d" else "w"}{n % 31}'
        elif op == 'ins_e':
            e = random.choice('bhsd'); w_ = {'b':8,'h':16,'s':32,'d':64}[e]; txt = f'mov v{d}.{e}[{random.randrange(128//w_)}], v{n}.{e}[{random.randrange(128//w_)}]'
        elif op in ('umov','smov'):
            e = random.choice('bhsd') if op=='umov' else random.choice('bhs'); w_ = {'b':8,'h':16,'s':32,'d':64}[e]
            if op == 'umov': txt = f'umov {"x" if e=="d" else "w"}{d % 31}, v{n}.{e}[{random.randrange(128//w_)}]'
            else: txt = f'smov {random.choice(["x","w"]) if e!="s" else "x"}{d % 31}, v{n}.{e}[{random.randrange(128//w_)}]'
        elif op in ('movi','mvni','orri','bici'):
            imm = random.randrange(256)
            if op in ('movi','mvni'):
                form = random.choice(['8b','16b','4h','8h','2s','4s','2s_msl','4s_msl','2d','d'])
                if form in ('8b','16b'): txt = f'movi v{d}.{form}, #{imm}' if op=='movi' else None
                elif form in ('4h','8h'): txt = f'{op} v{d}.{form}, #{imm}, lsl #{random.choice([0,8])}'
                elif form in ('2s','4s'): txt = f'{op} v{d}.{form}, #{imm}, lsl #{random.choice([0,8,16,24])}'
                elif form.endswith('msl'): txt = f'{op} v{d}.{form[:2]}, #{imm}, msl #{random.choice([8,16])}'
                elif form == '2d': txt = f'movi v{d}.2d, #0x{"".join(random.choice(["00","ff"]) for _ in range(8))}' if op=='movi' else None
                else: txt = f'movi d{d}, #0x{"".join(random.choice(["00","ff"]) for _ in range(8))}' if op=='movi' else None
            else:
                form = random.choice(['4h','8h','2s','4s'])
                txt = f'{op[:-1]} v{d}.{form}, #{imm}, lsl #{random.choice([0,8] if "h" in form else [0,8,16,24])}'
            if txt is None: txt = f'movi v{d}.16b, #{imm}'
        elif op in ('shl','sli'):
            txt = f'{op} v{d}.{arr}, v{n}.{arr}, #{sh_left()}'
        elif op in ('sshr','ushr','ssra','usra','sri'):
            txt = f'{op} v{d}.{arr}, v{n}.{arr}, #{sh_right()}'
        elif op in ('shrn','shrn2'):
            src = random.choice(['8h','4s','2d']); dst = {'8h':'8b','4s':'4h','2d':'2s'}[src]; dst2 = {'8h':'16b','4s':'8h','2d':'4s'}[src]; w_ = {'8h':16,'4s':32,'2d':64}[src]
            txt = f'shrn v{d}.{dst}, v{n}.{src}, #{random.randrange(1, w_//2+1)}' if op=='shrn' else f'shrn2 v{d}.{dst2}, v{n}.{src}, #{random.randrange(1, w_//2+1)}'
        elif op in ('ushll','sshll','ushll2','sshll2'):
            src = random.choice(['8b','4h','2s']); dst = {'8b':'8h','4h':'4s','2s':'2d'}[src]; w_ = {'8b':8,'4h':16,'2s':32}[src]
            if op.endswith('2'): src = {'8b':'16b','4h':'8h','2s':'4s'}[src]
            txt = f'{op} v{d}.{dst}, v{n}.{src}, #{random.randrange(0, w_)}'
        elif op in ('uaddl','saddl','uaddw','saddw','usubl','ssubl','umull','smull','umlal','smlal','umull2','smull2','uabdl'):
            src = random.choice(['8b','4h','2s']); dst = {'8b':'8h','4h':'4s','2s':'2d'}[src]
            if op.endswith('2'): src = {'8b':'16b','4h':'8h','2s':'4s'}[src]
            if op in ('uaddw','saddw'): txt = f'{op} v{d}.{dst}, v{n}.{dst}, v{m}.{src}'
            else: txt = f'{op} v{d}.{dst}, v{n}.{src}, v{m}.{src}'
        elif op in ('mul_e','mla_e'):
            arr = random.choice(['4h','8h','2s','4s']); e = el[arr]
            txt = f'{op[:3]} v{d}.{arr}, v{n}.{arr}, v{m % 16 if e=="h" else m}.{e}[{random.randrange(8 if e=="h" else 4)}]'
        elif op == 'umull_e':
            src = random.choice(['4h','2s']); dst = {'4h':'4s','2s':'2d'}[src]; e = el[src]
            txt = f'umull v{d}.{dst}, v{n}.{src}, v{m % 16 if e=="h" else m}.{e}[{random.randrange(8 if e=="h" else 4)}]'
        elif op in ('fadd','fsub','fmul','fdiv','fmax','fmin','fmaxnm','fminnm','fmla','fmls','fabd','faddp','fcmeq','fcmge','fcmgt','facge'):
            txt = f'{op} v{d}.{fa}, v{n}.{fa}, v{m}.{fa}'
        elif op in ('fabs','fneg','fsqrt','scvtf','ucvtf','fcvtzs','fcvtzu','fcvtns','fcvtms','fcvtps','fcvtas','frintn','frintz','frintm','frintp','frinta'):
            txt = f'{op} v{d}.{fa}, v{n}.{fa}'
        elif op in ('fcmeq0','fcmgt0','fcmlt0'):
            txt = f'{op[:-1]} v{d}.{fa}, v{n}.{fa}, #0.0'
        elif op == 'fcvtl': txt = f'fcvtl v{d}.2d, v{n}.2s'
        elif op == 'fcvtn': txt = f'fcvtn v{d}.2s, v{n}.2d'
        elif op in ('fmla_e','fmul_e'):
            f = random.choice(['2s','4s','2d']); e = 'd' if f=='2d' else 's'
            txt = f'{op[:-2]} v{d}.{f}, v{n}.{f}, v{m}.{e}[{random.randrange(2 if e=="d" else 4)}]'
        elif op == 'tbl':
            ln = random.randrange(1, 5); regs = ', '.join(f'v{(n+i)%32}.16b' for i in range(ln))
            txt = f'{random.choice(["tbl","tbx"])} v{d}.{random.choice(["8b","16b"])}, {{{regs}}}, v{m}.{random.choice(["8b","16b"])}'
            # keep arrangements consistent
            arrx = random.choice(['8b','16b']); txt = f'{random.choice(["tbl","tbx"])} v{d}.{arrx}, {{{regs}}}, v{m}.{arrx}'
        elif op == 'faddp_s':
            txt = random.choice([f'faddp d{d}, v{n}.2d', f'faddp s{d}, v{n}.2s'])
        elif op == 'addp_s':
            txt = f'addp d{d}, v{n}.2d'
        if txt: add(txt)

elif mode == 'fp16':
    for _ in range(5000):
        d, n, m, a = fp(), fp(), fp(), fp()
        S = lambda k: f'h{k}'
        c = random.choice(['eq','ne','cs','cc','mi','pl','vs','vc','hi','ls','ge','lt','gt','le'])
        sf = random.random() < 0.5
        t = [
         lambda: f'fadd {S(d)}, {S(n)}, {S(m)}', lambda: f'fsub {S(d)}, {S(n)}, {S(m)}', lambda: f'fmul {S(d)}, {S(n)}, {S(m)}', lambda: f'fdiv {S(d)}, {S(n)}, {S(m)}',
         lambda: f'fnmul {S(d)}, {S(n)}, {S(m)}', lambda: f'fmax {S(d)}, {S(n)}, {S(m)}', lambda: f'fmin {S(d)}, {S(n)}, {S(m)}', lambda: f'fmaxnm {S(d)}, {S(n)}, {S(m)}', lambda: f'fminnm {S(d)}, {S(n)}, {S(m)}',
         lambda: f'fmadd {S(d)}, {S(n)}, {S(m)}, {S(a)}', lambda: f'fmsub {S(d)}, {S(n)}, {S(m)}, {S(a)}', lambda: f'fnmadd {S(d)}, {S(n)}, {S(m)}, {S(a)}', lambda: f'fnmsub {S(d)}, {S(n)}, {S(m)}, {S(a)}',
         lambda: f'fsqrt {S(d)}, {S(n)}', lambda: f'fabs {S(d)}, {S(n)}', lambda: f'fneg {S(d)}, {S(n)}', lambda: f'fmov {S(d)}, {S(n)}',
         lambda: f'{random.choice(["frintn","frintp","frintm","frintz","frinta","frintx","frinti"])} {S(d)}, {S(n)}',
         lambda: f'fcmp {S(n)}, {S(m)}', lambda: f'fcmpe {S(n)}, #0.0', lambda: f'fccmp {S(n)}, {S(m)}, #{random.randrange(16)}, {c}', lambda: f'fcsel {S(d)}, {S(n)}, {S(m)}, {c}',
         lambda: f'fcvt s{d}, {S(n)}', lambda: f'fcvt d{d}, {S(n)}', lambda: f'fcvt {S(d)}, s{n}', lambda: f'fcvt {S(d)}, d{n}',
         lambda: f'{random.choice(["fcvtzs","fcvtzu","fcvtns","fcvtnu","fcvtms","fcvtmu","fcvtps","fcvtpu","fcvtas","fcvtau"])} {X(d % 31, sf)}, {S(n)}',
         lambda: f'{random.choice(["scvtf","ucvtf"])} {S(d)}, {X(n % 31, sf)}',
         lambda: f'scvtf {S(d)}, {X(n % 31, sf)}, #{random.randrange(1, 32)}', lambda: f'fcvtzu {X(d % 31, sf)}, {S(n)}, #{random.randrange(1, 32)}',
         lambda: f'fmov {S(d)}, #{random.choice(["1.0","-2.5","0.125","31.0","-0.5","10.0"])}',
         lambda: f'fmov {X(d % 31, sf)}, {S(n)}', lambda: f'fmov {S(d)}, {X(n % 31, sf)}',
         lambda: f'bfcvt {S(d)}, s{n}',
         lambda: f'fjcvtzs w{d % 31}, d{n}',
         lambda: f'{random.choice(["frint32x","frint64x"])} {random.choice("sd")}{d}, ' + random.choice("sd") + f'{n}' if False else f'{random.choice(["frint32x","frint64x"])} s{d}, s{n}',
         lambda: f'{random.choice(["frint32x","frint64x"])} d{d}, d{n}',
        ]
        add(random.choice(t)())
elif mode == 'words':
    # instruction words chosen by words.py (WORK/sample.txt: "<hex> <disassembly>"), with random state: ints, packed halves, singles and doubles in every vector lane
    for line in open(WORK + '/sample.txt'):
        w, text = line.rstrip('\n').split(' ', 1)
        add(f'.inst 0x{w} // {text}')
elif mode in ('svemem', 'sme'):
    # SVE loads and stores chosen by words.py (SVEMEM=1): the registers that address memory are rewritten so that every access lands in the scratch buffer.
    # x28 is the buffer base, x27 a small element offset, z31 a vector of small offsets (or, as a vector base, of addresses inside the buffer).
    import re as _re
    for line in open(WORK + '/sample.txt'):
        w, text = line.rstrip('\n').split(' ', 1)
        pre = ''
        m = _re.search(r'\[(x\d+|sp)(?:, (x\d+|z\d+\.[sd]))?', text)
        t2 = _re.sub(r'\[x\d+', '[x28', text)
        if _re.search(r'\[z\d+\.([sd])', t2):      # vector base (plus immediate or scalar)
            t2 = _re.sub(r'\[z\d+\.([sd])', r'[z31.\1', t2)
            T = _re.search(r'\[z31\.([sd])', t2).group(1)
            pre = f'index z31.{T}, ' + ('w28' if T == 's' else 'x28') + f', #{random.choice([1, 2, 4, 8])}'
        elif _re.search(r', z\d+\.([sd])(, |\])', t2):      # vector of offsets
            t2 = _re.sub(r', z\d+\.([sd])(, |\])', r', z31.\1\2', t2, count=1)
            T = _re.search(r', z31\.([sd])', t2).group(1)
            neg = 'sxtw' in t2 and random.random() < 0.3
            pre = f'index z31.{T}, #{random.randrange(-8, 0) if neg else random.randrange(0, 4)}, #{random.choice([1, 2, 3, 4])}'
        t2 = _re.sub(r', x\d+(\]|, lsl)', r', x27\1', t2)
        if _re.search(r'\bz31\b', _re.sub(r'\[[^]]*\]', '', t2)): continue      # the destination or source list uses z31: skip
        if mode == 'sme' and _re.match(r'ld1[bhwdq] \{za\d+v', t2):      # (qemu leaves the inactive elements of a vertical slice load alone; the architecture clears them: use an all-true predicate)
            t2 = _re.sub(r'p\d/z', 'p7/z', t2); pre = 'ptrue p7.b'
        if pre: add(pre + '\n  ' + t2)
        else: add(t2)
elif mode == 'int2':
    for _ in range(6000):
        sf = random.random() < 0.6
        d, n, m = [random.randrange(0, 28) for _ in range(3)]
        R = lambda k: X(k, sf)
        op = random.randrange(0, 17)
        if 'OP' in __import__('os').environ: op = int(__import__('os').environ['OP'])
        t = [
         lambda: f'{random.choice(["smax","smin","umax","umin"])} {R(d)}, {R(n)}, {R(m)}',
         lambda: f'{random.choice(["smax","smin"])} {R(d)}, {R(n)}, #{random.randrange(-128, 128)}',
         lambda: f'{random.choice(["umax","umin"])} {R(d)}, {R(n)}, #{random.randrange(0, 256)}',
         lambda: f'{random.choice(["abs","cnt","ctz"])} {R(d)}, {R(n)}',
         lambda: f'{random.choice(["crc32b","crc32h","crc32w","crc32cb","crc32ch","crc32cw"])} w{d}, w{n}, w{m}',
         lambda: f'{random.choice(["crc32x","crc32cx"])} w{d}, w{n}, x{m}',
         lambda: f'smulh x{d}, x{n}, x{m}', lambda: f'umulh x{d}, x{n}, x{m}',
         lambda: f'stxp w{m}, {R(d)}, {R(n)}, [x28]' if len({d, n, m}) == 3 else f'stlxp w{m}, {R(d)}, {R(n)}, [x28]' if len({d, n, m}) == 3 else f'ldxp {R(d)}, {R(n)}, [x28]',
         lambda: f'ldxp {R(d)}, {R(n)}, [x28]' if d != n else f'ldxp {R(d)}, {R((n + 1) % 28)}, [x28]',
         lambda: f'ldxp {R(d)}, {R(n)}, [x28] ; {random.choice(["stxp","stlxp"])} w{m}, {R(d)}, {R(n)}, [x28]' if len({d, n, m}) == 3 else f'ldaxp {R(d)}, {R((d + 1) % 28)}, [x28]',
         lambda: f'casp {X(2*(d//2), sf)}, {X(2*(d//2)+1, sf)}, {X(2*(n//2), sf)}, {X(2*(n//2)+1, sf)}, [x28]',
         lambda: f'caspal {X(2*(d//2), sf)}, {X(2*(d//2)+1, sf)}, {X(2*(n//2), sf)}, {X(2*(n//2)+1, sf)}, [x28]',
         lambda: f'{random.choice(["ldapur","stlur"])} {R(d)}, [x28, #{8 * random.randrange(-12, 12)}]',
         lambda: f'{random.choice(["ldapurb","stlurb","ldapurh","stlurh"])} w{d}, [x28, #{8 * random.randrange(-12, 12)}]',
         lambda: f'{random.choice(["ldapursb","ldapursh"])} {"w" if random.random()<0.5 else "x"}{d}, [x28, #{8 * random.randrange(-12, 12)}]',
         lambda: f'ldapursw x{d}, [x28, #{8 * random.randrange(-12, 12)}]',
        ]
        add(t[op]())

# --- emit asm
NZCV_OFF = 0; X_OFF = 8; V_OFF = 8 + 31*8 + 8  # keep 16-aligned: x0..x30 then pad
V_OFF = (8 + 31*8 + 15) // 16 * 16
INIT_ZA = V_OFF + 32*16 + 48
INIT_SIZE = INIT_ZA + 256
RES_OFF = (INIT_SIZE + 15) // 16 * 16
RES_X = RES_OFF; RES_V = RES_OFF + 31*8 + 24  # nzcv,fpsr stored first? layout below
# result layout: x0..x30 (248), nzcv(8), fpsr(8), pad to 16, v0..v31
RES_NZCV = RES_OFF + 31*8; RES_FPSR = RES_NZCV + 8
RES_V = (RES_FPSR + 8 + 15) // 16 * 16
RES_P = RES_V + 32*16       # p0..p15 then FFR, 2 bytes each (vector length 128 bits)
RES_ZA = (RES_P + 17*2 + 15) // 16 * 16      # the ZA array: 16 rows of 16 bytes
BUF_OFF = RES_ZA + 256
BUF_SIZE = 1024
INIT_P = V_OFF + 32*16      # initial predicates, same layout (inside the init area, which is RES_OFF-aligned, so reserve the room)
BLK = BUF_OFF + BUF_SIZE

out = ['.text', '.globl _start', '_start:'] + (['  smstart'] if mode == 'sme' else [])
data = ['.data', '.balign 16']
for i, text in enumerate(cases):
    out.append(f'  adrp x0, blk{i}\n  add x0, x0, :lo12:blk{i}\n  mov sp, x0')
    out.append(f'  ldr x0, [sp, #{NZCV_OFF}]\n  msr nzcv, x0\n  msr fpsr, xzr')
    if mode == 'sme':      # streaming mode with ZA enabled: state is loaded with the SVE and ZA loads, which are legal there
        out.append(f'  add x1, sp, #{INIT_ZA}\n  mov w12, #0')
        for k in range(16): out.append(f'  ldr za[w12, #{k}], [x1, #{k}, mul vl]')
        out.append(f'  add x1, sp, #{V_OFF}')
        for k in range(32): out.append(f'  ldr z{k}, [x1, #{k}, mul vl]')
        out.append(f'  add x1, sp, #{INIT_P}')
        for k in range(16): out.append(f'  ldr p{k}, [x1, #{k}, mul vl]')
    else:
        for k in range(0, 32, 2): out.append(f'  ldp q{k}, q{k+1}, [sp, #{V_OFF + k*16}]')
        out.append(f'  add x1, sp, #{INIT_P}\n  ldr p0, [x1, #16, mul vl]\n  wrffr p0.b')
        for k in range(16): out.append(f'  ldr p{k}, [x1, #{k}, mul vl]')
    for k in range(0, 30, 2): out.append(f'  ldp x{k}, x{k+1}, [sp, #{X_OFF + k*8}]')
    out.append(f'  ldr x30, [sp, #{X_OFF + 30*8}]')
    out.append(f'  add x28, sp, #{BUF_OFF + 384}')
    out.append(f'  mov x27, #{random.randrange(0, 6)}')
    out.append(f'  {text}')
    out.append(f'  add sp, sp, #{RES_OFF}')
    for k in range(0, 30, 2): out.append(f'  stp x{k}, x{k+1}, [sp, #{RES_X + k*8 - RES_OFF}]')
    out.append(f'  str x30, [sp, #{RES_X + 30*8 - RES_OFF}]')
    out.append(f'  mrs x0, nzcv\n  str x0, [sp, #{RES_NZCV - RES_OFF}]\n  mrs x0, fpsr\n  str x0, [sp, #{RES_FPSR - RES_OFF}]')
    if mode == 'sme':
        out.append(f'  add x1, sp, #{RES_V - RES_OFF}')
        for k in range(32): out.append(f'  str z{k}, [x1, #{k}, mul vl]')
        out.append(f'  add x1, sp, #{RES_P - RES_OFF}')
        for k in range(16): out.append(f'  str p{k}, [x1, #{k}, mul vl]')
        out.append(f'  add x1, sp, #{RES_ZA - RES_OFF}\n  mov w12, #0')
        for k in range(16): out.append(f'  str za[w12, #{k}], [x1, #{k}, mul vl]')
    else:
        for k in range(0, 32, 2): out.append(f'  stp q{k}, q{k+1}, [sp, #{RES_V + k*16 - RES_OFF}]')
        out.append(f'  add x1, sp, #{RES_P - RES_OFF}')
        for k in range(16): out.append(f'  str p{k}, [x1, #{k}, mul vl]')
        out.append(f'  rdffr p0.b\n  str p0, [x1, #16, mul vl]')
    data.append(f'blk{i}:')
    words = [random.getrandbits(32) & 0xF0000000]  # nzcv
    words = [struct.pack('<Q', random.getrandbits(4) << 28)]
    xs = [struct.pack('<Q', r64()) for _ in range(31)]
    pad = [struct.pack('<Q', 0)] * ((V_OFF - 8 - 31*8) // 8)
    fpmode = 'h' if mode in ('fp16', 'neon16') else 'mix' if mode in ('words', 'sme', 'svemem') else mode in ('fp', 'neon', 'fpx', 'neonx')
    vs = []
    for _ in range(32):
        vs.append(struct.pack('<QQ', v64(fpmode), v64(fpmode)))
    ps = b''.join(struct.pack('<H', pred16()) for _ in range(17))
    blob = b''.join(words + xs + pad + vs) + ps
    blob += bytes(INIT_ZA - len(blob)) + bytes(random.getrandbits(8) for _ in range(256))
    blob += bytes(BUF_OFF - len(blob)) + bytes(random.getrandbits(8) for _ in range(BUF_SIZE))
    for off in range(0, len(blob), 16):
        data.append('  .quad 0x%x, 0x%x' % struct.unpack('<QQ', blob[off:off+16]))
out.append(f'  mov x0, #1\n  adr x1, blk0\n  ldr x2, =total\n  mov x8, #64\n  svc #0')
out.append('  mov x0, #0\n  mov x8, #94\n  svc #0')
out.append('.balign 8\n.pool')
data.append(f'.set total, {BLK * len(cases)}')
open(WORK + '/t.s', 'w').write('\n'.join(out + data) + '\n')
open(WORK + '/cases.txt', 'w').write('\n'.join(c.replace('\n', ' ; ') for c in cases) + '\n')
open(WORK + '/layout.txt', 'w').write(f'{BLK} {X_OFF} {V_OFF} {RES_X} {RES_NZCV} {RES_FPSR} {RES_V} {BUF_OFF} {RES_P} {RES_ZA}\n')
