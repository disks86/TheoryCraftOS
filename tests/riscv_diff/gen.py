#!/usr/bin/env python3
# Differential tester for the RISC-V emulator. Usage: gen.py <int|mem|fp|comp> <seed> <workdir>  (see ../riscv_diff.sh)
# Generates an RV64GC test program: for each case, load random state (x1..x30, f0..f31, fcsr), run a few instructions, dump the state. x31 holds the case's block and is
# never an operand of the cases.
import random, struct, sys
WORK = sys.argv[3]
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
mode = sys.argv[1]

SPECIAL64 = [0, 1, 2, 3, 0xFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0x0123456789ABCDEF, 0xFEDCBA9876543210,
             0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFE, 0x100000000]
SF64 = [0x0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x4000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000,
        0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x7FEFFFFFFFFFFFFF, 0x4059000000000000, 0xC059000000000000, 0x3FE0000000000000, 0x43E0000000000000, 0x41DFFFFFFFC00000,
        0x7FF8000000000123, 0xFFF8000000000000, 0x3FF8000000000000, 0x400C000000000000, 0xC3E0000000000000, 0x43F0000000000000, 0x0010000000000000, 0x000FFFFFFFFFFFFE]
SF32 = [0x0, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FA00000, 1, 0x007FFFFF, 0x7F7FFFFF, 0x42C80000, 0xC2C80000, 0x3F000000, 0x4F000000,
        0x7FC00123, 0xFFC00000, 0x3FC00000, 0x40600000, 0xCF000000, 0x4F800000, 0x00800000, 0x007FFFFE, 0x5F000000, 0xDF000000]

def r64():
    k = random.random()
    if k < 0.3: return random.choice(SPECIAL64)
    if k < 0.45: return random.getrandbits(8)
    if k < 0.55: return random.getrandbits(32)
    if k < 0.6: return random.getrandbits(5)
    return random.getrandbits(64)
def f64():
    k = random.random()
    if k < 0.35:   # a NaN-boxed single
        if random.random() < 0.7: s = random.choice(SF32)
        else: s = struct.unpack('<I', struct.pack('<f', random.uniform(-1000, 1000)))[0]
        return 0xFFFFFFFF00000000 | s
    if k < 0.65: return random.choice(SF64)
    if k < 0.8: return struct.unpack('<Q', struct.pack('<d', random.uniform(-1000, 1000)))[0]
    if k < 0.9: return random.choice(SF32)       # not boxed
    return random.getrandbits(64)

def R(): return 'x%d' % random.choice(list(range(1, 28)) + [30])      # (x28 and x29 are scratch registers of the cases)
def Rc(): return 'x%d' % random.randrange(8, 16)
def F(): return 'f%d' % random.randrange(0, 32)
def Fc(): return 'f%d' % random.randrange(8, 16)

cases = []   # (text, kind)  kind: None, or ('mem', register) -> the register is pointed into the buffer first
def add(text): cases.append(text)

RM = ['rne', 'rtz', 'rdn', 'rup', 'rmm', 'dyn']
def rm(): return random.choice(RM)

if mode == 'int':
    for _ in range(7000):
        d, n, m = R(), R(), R()
        imm12 = random.choice([random.randrange(-2048, 2048), 0, -1, 2047, -2048, 1])
        sh = random.choice([0, 1, 7, 31, 32, 63, random.randrange(0, 64)])
        sw = random.choice([0, 1, 7, 15, 31, random.randrange(0, 32)])
        u20 = random.choice([0, 1, 0xFFFFF, 0x80000, 0x7FFFF, random.randrange(0, 1 << 20)])
        op = random.randrange(0, 12)
        if op == 0: add(f'{random.choice(["add","sub","sll","slt","sltu","xor","srl","sra","or","and"])} {d}, {n}, {m}')
        elif op == 1: add(f'{random.choice(["addw","subw","sllw","srlw","sraw"])} {d}, {n}, {m}')
        elif op == 2: add(f'{random.choice(["mul","mulh","mulhsu","mulhu","div","divu","rem","remu"])} {d}, {n}, {m}')
        elif op == 3: add(f'{random.choice(["mulw","divw","divuw","remw","remuw"])} {d}, {n}, {m}')
        elif op == 4: add(f'{random.choice(["addi","slti","sltiu","xori","ori","andi"])} {d}, {n}, {imm12}')
        elif op == 5: add(f'{random.choice(["slli","srli","srai"])} {d}, {n}, {sh}')
        elif op == 6: add(f'addiw {d}, {n}, {imm12}')
        elif op == 7: add(f'{random.choice(["slliw","srliw","sraiw"])} {d}, {n}, {sw}')
        elif op == 8: add(f'{random.choice(["lui","auipc"])} {d}, {u20}')
        elif op == 9: add(f'{random.choice(["beq","bne","blt","bge","bltu","bgeu"])} {n}, {m}, 1f\n li x29, 77\n1: li x28, 5')
        elif op == 10: add(f'jal {d}, 1f\n li x29, 77\n1: nop')
        else: add(f'auipc x29, 0\n jalr {d}, 12(x29)\n li x28, 77\n nop')
    for _ in range(300):   # d == n, n == m, d == x0 forms
        a = R(); add(f'{random.choice(["add","sub","xor","mul","div","rem","sltu"])} {a}, {a}, {a}')
        add(f'{random.choice(["add","sub","and","mulh","divu"])} x0, {R()}, {R()}')
        add(f'jalr x0, 0({a})' if False else f'addi x0, {a}, 5')

elif mode == 'mem':
    # the base register is pointed into the 256-byte buffer first; offsets keep every access inside it
    for _ in range(4000):
        b, d, s = R(), R(), R()
        while d == b: d = R()
        while s == b: s = R()
        k = random.randrange(0, 10)
        off = random.randrange(0, 120)
        base = f'addi {b}, x31, 528+64'   # BUF at 528 in the block, +64 so negative offsets work too
        if k == 0: add(f'{base}\n {random.choice(["lb","lh","lw","ld","lbu","lhu","lwu"])} {d}, {off - 40}({b})')
        elif k == 1: add(f'{base}\n {random.choice(["sb","sh","sw","sd"])} {s}, {off - 40}({b})')
        elif k == 2: add(f'{base}\n {random.choice(["flw","fld"])} {F()}, {off - 40}({b})')
        elif k == 3: add(f'{base}\n {random.choice(["fsw","fsd"])} {F()}, {off - 40}({b})')
        elif k == 4:
            w = random.choice(['w', 'd']); sz = 4 if w == 'w' else 8
            add(f'{base}\n andi {b}, {b}, -{sz}\n lr.{w} {d}, ({b})\n sc.{w} {s}, {s}, ({b})')   # (sc result replaces the data register)
        elif k == 5:
            w = random.choice(['w', 'd']); sz = 4 if w == 'w' else 8
            op = random.choice(['amoswap', 'amoadd', 'amoxor', 'amoand', 'amoor', 'amomin', 'amomax', 'amominu', 'amomaxu'])
            add(f'{base}\n andi {b}, {b}, -{sz}\n {op}.{w} {d}, {s}, ({b})')
        elif k == 6:   # sc without a reservation, and sc to another address
            w = random.choice(['w', 'd']); sz = 4 if w == 'w' else 8
            add(f'{base}\n andi {b}, {b}, -{sz}\n sc.{w} {d}, {s}, ({b})')
        elif k == 7:
            w = random.choice(['w', 'd']); sz = 4 if w == 'w' else 8
            add(f'{base}\n andi {b}, {b}, -16\n lr.{w} {d}, ({b})\n addi x29, {b}, 8\n sc.{w} {s}, {s}, (x29)')
        elif k == 8: add(f'{base}\n sd {s}, 0({b})\n ld {d}, 0({b})\n lbu x29, 3({b})\n lh x28, 6({b})')
        else: add(f'{base}\n fsd {F()}, 8({b})\n fld {F()}, 8({b})\n fsw {F()}, 20({b})\n flw {F()}, 20({b})')

elif mode == 'fp':
    for _ in range(9000):
        fd, f1, f2, f3, x = F(), F(), F(), F(), R()
        w = random.choice(['s', 'd'])
        k = random.randrange(0, 17)
        if k <= 3: add(f'{random.choice(["fadd","fsub","fmul","fdiv"])}.{w} {fd}, {f1}, {f2}, {rm()}')
        elif k == 4: add(f'fsqrt.{w} {fd}, {f1}, {rm()}')
        elif k == 5: add(f'{random.choice(["fmadd","fmsub","fnmsub","fnmadd"])}.{w} {fd}, {f1}, {f2}, {f3}, {rm()}')
        elif k == 6: add(f'{random.choice(["fsgnj","fsgnjn","fsgnjx","fmin","fmax"])}.{w} {fd}, {f1}, {f2}')
        elif k == 7: add(f'{random.choice(["feq","flt","fle"])}.{w} {x}, {f1}, {f2}')
        elif k == 8: add(f'fclass.{w} {x}, {f1}')
        elif k == 9: add(f'fcvt.{random.choice(["w","wu","l","lu"])}.{w} {x}, {f1}, {rm()}')
        elif k == 10: add(f'fcvt.{w}.{random.choice(["w","wu","l","lu"])} {fd}, {x}, {rm()}')
        elif k == 11: add('fcvt.s.d %s, %s, %s' % (fd, f1, rm()) if random.random() < 0.5 else 'fcvt.d.s %s, %s' % (fd, f1))
        elif k == 12: add(f'fmv.x.{"w" if w == "s" else "d"} {x}, {f1}')
        elif k == 13: add(f'fmv.{"w" if w == "s" else "d"}.x {fd}, {x}')
        elif k == 14: add(f'frflags {x}\n frrm x29\n frcsr x28')
        elif k == 15: add(f'li x29, {random.randrange(0, 5)}\n fsrm {x}, x29\n li x29, {random.randrange(0, 32)}\n fsflags x28, x29\n li x29, {(random.randrange(0, 5) << 5) | random.randrange(0, 32)}\n fscsr x28, x29\n {random.choice(["fadd","fmul"])}.{w} {fd}, {f1}, {f2}')
        else: add(f'csrrwi {x}, fflags, {random.randrange(0,32)}\n csrrsi x29, frm, {random.randrange(0, 5)}\n csrrci x28, fcsr, {random.randrange(0,32)}')

    # targeted: products just below the smallest normal number that round up to it (tininess after rounding differs from before), and min/max of zeros
    from fractions import Fraction
    def tiny_pair(double):
        mb = 52 if double else 23
        for _ in range(1000):
            m1 = 1 + Fraction(random.getrandbits(mb), 1 << mb)
            target = 2 - Fraction(random.random()) * Fraction(1, 1 << (mb + 1)) * random.choice([1, 2, 3])
            m2n = round((target / m1) * (1 << mb))
            m2 = Fraction(m2n, 1 << mb)
            if 1 <= m2 < 2 and 2 - Fraction(1, 1 << (mb + 1)) <= m1 * m2 < 2 + 0: return m1, m2
        return None
    def bits_of(m, e, double):
        mb = 52 if double else 23
        frac = int((m - 1) * (1 << mb))
        be = e + (1023 if double else 127)
        return (be << mb) | frac
    for _ in range(700):
        double = random.random() < 0.5
        pr = tiny_pair(double)
        if not pr: continue
        m1, m2 = pr
        bias_min = -1022 if double else -126
        ea = random.randrange(bias_min // 2 - 3, bias_min // 2 + 3)
        eb = bias_min - 1 - ea            # the product lies in the binade just below the smallest normal number
        if random.random() < 0.5: m1, m2, ea, eb = m1, m2, ea, eb
        w = 'd' if double else 's'
        a, b = bits_of(m1, ea, double), bits_of(m2, eb, double)
        neg = random.random() < 0.3
        if neg: a |= (1 << (63 if double else 31))
        mv = 'fmv.d.x' if double else 'fmv.w.x'
        op = random.choice(['fmul', 'fmul', 'fmul', 'fmadd'])
        extra = f', f3' if op == 'fmadd' else ''
        add(f'li x29, {a}\n {mv} f1, x29\n li x29, {b}\n {mv} f2, x29\n fmv.d.x f3, x0\n {op}.{w} f4, f1, f2{extra}, {rm()}')
    for w, mv, sgn in (('s', 'fmv.w.x', 31), ('d', 'fmv.d.x', 63)):
        for a in (0, 1 << sgn):
            for b in (0, 1 << sgn):
                for op in ('fmin', 'fmax', 'feq', 'flt', 'fle', 'fsgnj', 'fsgnjn', 'fsgnjx'):
                    tgt = 'x5' if op in ('feq', 'flt', 'fle') else 'f4'
                    add(f'li x29, {a}\n {mv} f1, x29\n li x29, {b}\n {mv} f2, x29\n {op}.{w} {tgt}, f1, f2')

elif mode == 'bit':
    # Zba, Zbb, Zbs, Zbc, Zicond (assembled with -march=rv64gc_zba_zbb_zbs_zbc_zicond; see ../riscv_diff.sh)
    RR = ["sh1add", "sh2add", "sh3add", "add.uw", "sh1add.uw", "sh2add.uw", "sh3add.uw", "andn", "orn", "xnor", "min", "minu", "max", "maxu", "rol", "ror", "rolw", "rorw",
          "clmul", "clmulr", "clmulh", "bclr", "bext", "binv", "bset", "czero.eqz", "czero.nez"]
    UN = ["clz", "ctz", "cpop", "clzw", "ctzw", "cpopw", "sext.b", "sext.h", "zext.h", "rev8", "orc.b"]
    for _ in range(7000):
        d, n, m = R(), R(), R()
        k = random.randrange(0, 4)
        sh = random.choice([0, 1, 7, 31, 32, 63, random.randrange(0, 64)])
        if k == 0 or k == 1: add(f'{random.choice(RR)} {d}, {n}, {m}')
        elif k == 2: add(f'{random.choice(UN)} {d}, {n}')
        else:
            op = random.choice(["rori", "bseti", "bclri", "binvi", "bexti", "slli.uw", "roriw"])
            add(f'{op} {d}, {n}, {sh & 31 if op == "roriw" else sh}')
    for _ in range(300):
        a = R(); add(f'{random.choice(RR)} {a}, {a}, {a}')
        add(f'{random.choice(RR)} x0, {R()}, {R()}')
        add(f'{random.choice(UN)} {a}, {a}')
elif mode == 'comp':
    for _ in range(5000):
        k = random.randrange(0, 30)
        d, s = R(), R()
        imm6 = random.choice([random.randrange(-32, 32), 1, -1])
        while imm6 == 0 and k in (3, 4, 5): imm6 = random.randrange(-32, 32)
        sh = random.randrange(1, 64)
        if k == 0: add(f'c.addi {d}, {random.choice([i for i in range(-32, 32) if i != 0])}')
        elif k == 1: add(f'c.addiw {d}, {imm6}')
        elif k == 2: add(f'c.li {d}, {imm6}')
        elif k == 3: add(f'c.lui {d if d != "x2" else "x3"}, {random.choice([i for i in range(1, 32)] + [0xFFFE0 + j for j in range(0, 32)])}')
        elif k == 4: add(f'c.addi16sp x2, {random.choice([16, -16, 32, 496, -512, 64])}\n mv x29, x2')
        elif k == 5: add(f'c.addi4spn {Rc()}, x2, {random.choice([4, 8, 1020, 100, 256])}')
        elif k == 6: add(f'{random.choice(["c.srli","c.srai"])} {Rc()}, {sh}')
        elif k == 7: add(f'c.andi {Rc()}, {imm6}')
        elif k == 8: add(f'{random.choice(["c.sub","c.xor","c.or","c.and","c.subw","c.addw"])} {Rc()}, {Rc()}')
        elif k == 9: add(f'c.slli {d}, {sh}')
        elif k == 10: add(f'c.mv {d}, {s}')
        elif k == 11: add(f'c.add {d}, {s}')
        elif k == 12: add(f'{random.choice(["c.beqz","c.bnez"])} {Rc()}, 1f\n li x29, 77\n1: li x28, 5')
        elif k == 13: add('c.j 1f\n li x29, 77\n1: li x28, 5')
        elif k == 14: add(f'auipc x29, 0\n addi x29, x29, 12\n c.jr x29\n li x28, 77\n nop')
        elif k == 15: add(f'auipc x29, 0\n addi x29, x29, 12\n c.jalr x29\n li x28, 77\n nop\n mv {d}, x1')
        elif k == 16:
            b = Rc(); d2 = Rc()
            add(f'addi {b}, x31, 528+64\n c.lw {d2}, {random.choice(range(0, 128, 4))}({b})')
        elif k == 17:
            b = Rc(); d2 = Rc()
            add(f'addi {b}, x31, 528+64\n c.ld {d2}, {random.choice(range(0, 256, 8))}({b})')
        elif k == 18:
            b = Rc(); d2 = Rc()
            add(f'addi {b}, x31, 528+64\n c.sw {d2}, {random.choice(range(0, 128, 4))}({b})')
        elif k == 19:
            b = Rc(); d2 = Rc()
            add(f'addi {b}, x31, 528+64\n c.sd {d2}, {random.choice(range(0, 256, 8))}({b})')
        elif k == 20:
            add(f'addi x2, x31, 528+96\n c.lwsp {d}, {random.choice(range(0, 64, 4))}(x2)' if d != 'x2' else 'nop')
        elif k == 21:
            add(f'addi x2, x31, 528+96\n c.ldsp {d if d != "x2" else "x3"}, {random.choice(range(0, 128, 8))}(x2)')
        elif k == 22:
            add(f'addi x2, x31, 528+96\n c.swsp {d}, {random.choice(range(0, 64, 4))}(x2)' if d != 'x2' else 'nop')
        elif k == 23:
            add(f'addi x2, x31, 528+96\n c.sdsp {d}, {random.choice(range(0, 128, 8))}(x2)' if d != 'x2' else 'nop')
        elif k == 24:
            add(f'addi {Rc()}, x31, 528+64\n c.fld f8, 8(x8)\n' if False else f'addi x9, x31, 528+64\n c.fld {Fc()}, {random.choice(range(0, 256, 8))}(x9)')
        elif k == 25:
            add(f'addi x9, x31, 528+64\n c.fsd {Fc()}, {random.choice(range(0, 256, 8))}(x9)')
        elif k == 26:
            add(f'addi x2, x31, 528+96\n c.fldsp {F()}, {random.choice(range(0, 128, 8))}(x2)')
        elif k == 27:
            add(f'addi x2, x31, 528+96\n c.fsdsp {F()}, {random.choice(range(0, 128, 8))}(x2)')
        elif k == 28: add('c.nop\n c.nop')
        else: add(f'c.mv {d}, {s}\n c.add {d}, {d}')

elif mode in ('vint', 'vmem', 'vfp'):
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import genv
    gen = {'vint': genv.int_case, 'vmem': genv.mem_case, 'vfp': genv.fp_case}[mode]
    while len(cases) < (4000 if mode != 'vfp' else 5000):
        c = gen()
        if c: add(c)

# --- emit the assembly
VEC = mode.startswith('v')
if VEC:      # the vector block: x, f and fcsr as before, then the vector registers, a 1024-byte buffer, and the results
    BLK = 3112
    INIT_F, INIT_FCSR, INIT_V, BUF, RES_X, RES_F, RES_FCSR, RES_V, RES_VCSR = 256, 512, 528, 1040, 2064, 2304, 2560, 2568, 3080
    BIAS = 1536      # x31 points BIAS bytes into the block so that every offset fits a 12-bit immediate
else:
    BIAS = 0
    BLK = 1296
    INIT_F, INIT_FCSR, BUF, RES_X, RES_F, RES_FCSR = 256, 512, 528, 784, 1024, 1280
out = ['.option norelax', '.option rvc', '.text', '.globl _start', '_start:']
data = ['.data', '.balign 16']
total = len(cases) * BLK
for ci, text in enumerate(cases):
    blk = bytearray(BLK)
    for r in range(1, 31): blk[(r - 1) * 8:(r - 1) * 8 + 8] = struct.pack('<Q', r64())
    for r in range(32): blk[INIT_F + r * 8:INIT_F + r * 8 + 8] = struct.pack('<Q', f64())
    fcsr = (random.randrange(0, 5) << 5) | (random.getrandbits(5) if random.random() < 0.3 else 0)
    blk[INIT_FCSR:INIT_FCSR + 8] = struct.pack('<Q', fcsr)
    for i in range(1024 if VEC else 256): blk[BUF + i] = random.getrandbits(8)
    if VEC:
        for r in range(32):
            k = random.random()
            if k < 0.4: lane = bytes(random.getrandbits(8) for _ in range(16))
            elif mode == 'vfp' and k < 0.85:
                width = random.choice([4, 8])
                if width == 4: lane = b''.join(struct.pack('<I', random.choice(SF32) if random.random() < 0.5 else struct.unpack('<I', struct.pack('<f', random.uniform(-1000, 1000)))[0]) for _ in range(4))
                else: lane = b''.join(struct.pack('<Q', random.choice(SF64) if random.random() < 0.5 else struct.unpack('<Q', struct.pack('<d', random.uniform(-1000, 1000)))[0]) for _ in range(2))
            else:
                v = r64(); lane = struct.pack('<Q', v) * 2 if random.random() < 0.5 else struct.pack('<QQ', v, r64())
            blk[INIT_V + r * 16:INIT_V + r * 16 + 16] = lane
    data.append(f'blk{ci}:')
    data.append('  .byte ' + ','.join(str(b) for b in blk))
    out.append(f'  lla x31, blk{ci}')
    if BIAS: out.append(f'  addi x31, x31, {BIAS}')
    if VEC:
        out.append(f'  addi x28, x31, {INIT_V - BIAS}')
        for g in range(4):
            out.append(f'  vl8re8.v v{g * 8}, (x28)')
            if g < 3: out.append('  addi x28, x28, 128')
        out.append('  csrwi vxrm, 0\n  csrwi vxsat, 0')
    out.append(f'  ld x1, {INIT_FCSR - BIAS}(x31)\n  csrw fcsr, x1')
    for r in range(32): out.append(f'  fld f{r}, {INIT_F + r * 8 - BIAS}(x31)')
    for r in range(1, 31): out.append(f'  ld x{r}, {(r - 1) * 8 - BIAS}(x31)')
    out.append(' ' + text.replace('\n', '\n '))
    for r in range(1, 31): out.append(f'  sd x{r}, {RES_X + (r - 1) * 8 - BIAS}(x31)')
    for r in range(32): out.append(f'  fsd f{r}, {RES_F + r * 8 - BIAS}(x31)')
    out.append(f'  frcsr x1\n  sd x1, {RES_FCSR - BIAS}(x31)')
    if VEC:
        out.append(f'  addi x28, x31, {RES_V - BIAS}')
        for g in range(4):
            out.append(f'  vs8r.v v{g * 8}, (x28)')
            if g < 3: out.append('  addi x28, x28, 128')
        for n, csr in enumerate(['vl', 'vtype', 'vxsat', 'vxrm']):
            out.append(f'  csrr x1, {csr}\n  sd x1, {RES_VCSR + n * 8 - BIAS}(x31)')
out.append(f'  li a0, 1\n  lla a1, blk0\n  li a2, {total}\n  li a7, 64\n  ecall')
out.append('  li a0, 0\n  li a7, 94\n  ecall')
open(WORK + '/t.s', 'w').write('\n'.join(out + data) + '\n')
open(WORK + '/cases.txt', 'w').write('\n'.join(c.replace('\n', ' ; ') for c in cases) + '\n')
open(WORK + '/layout.txt', 'w').write(f'{BLK} {RES_X} {RES_F} {RES_FCSR} {BUF}' + (f' {RES_V} {RES_VCSR} 1024' if VEC else '') + '\n')
