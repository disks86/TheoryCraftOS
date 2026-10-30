#!/usr/bin/env python3
# Builds the instruction sample for the "words" differential mode: random 32-bit words are decoded with llvm-mc (all features), the ones that are valid, canonical (no should-be-zero /
# should-be-one fields set the other way) and safe to run in a straight-line test are kept, stratified by mnemonic and operand shape so that every instruction form is exercised.
# Usage: words.py <seed> <workdir> [filter-regex] [per-key] [candidates]       writes <workdir>/sample.txt: "<hex word> <disassembly>"
import random, re, struct, subprocess, sys, collections
seed, work = int(sys.argv[1]), sys.argv[2]
flt = re.compile(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
per_key = int(sys.argv[4]) if len(sys.argv) > 4 else 4
candidates = int(sys.argv[5]) if len(sys.argv) > 5 else 600000
rng = random.Random(seed)
import os
SME = bool(os.environ.get('SME'))
SVEMEM = bool(os.environ.get('SVEMEM'))
SVE = bool(os.environ.get('SVE')) or SVEMEM
words = []
while len(words) < candidates:
    w = rng.getrandbits(32)
    if (w >> 26) & 7 == 5: continue                 # branches
    if (w >> 24) in (0xD4, 0xD5, 0xD6, 0xD7): continue    # exceptions, system, branches to register
    if SVE: w = (w & ~(0xF << 25)) | (2 << 25)            # the SVE encoding space only
    if SME: w = (w & ~(0xF << 25)) | (1 << 31)            # the SME encoding space (bit 31 set, bits 28:25 clear)
    words.append(w)
MATTR = '--mattr=+all'
valid, texts = [], []
CH = 100000
for base in range(0, len(words), CH):
    chunk = words[base:base + CH]
    lines = ''.join('0x%02x,0x%02x,0x%02x,0x%02x\n' % (w & 255, (w >> 8) & 255, (w >> 16) & 255, w >> 24) for w in chunk)
    p = subprocess.run(['llvm-mc', '--disassemble', '--triple=aarch64', MATTR], input=lines.encode(), capture_output=True)
    bad = set(int(m.group(1)) - 1 for m in re.finditer(rb'<stdin>:(\d+):\d+: warning: invalid instruction encoding', p.stderr))
    dis = [l.strip() for l in p.stdout.decode().split('\n') if l.strip()]
    it = iter(dis)
    ok = [(chunk[i], ' '.join(next(it).split())) for i in range(len(chunk)) if i not in bad]
    q = subprocess.run(['llvm-mc', '--triple=aarch64', MATTR, '--show-encoding'], input=('\n'.join(t for _, t in ok) + '\n').encode(), capture_output=True)
    encs = re.findall(rb'encoding: \[(0x[0-9a-f]{2}),(0x[0-9a-f]{2}),(0x[0-9a-f]{2}),(0x[0-9a-f]{2})\]', q.stdout)
    failed = set(int(m.group(1)) - 1 for m in re.finditer(rb'<stdin>:(\d+):\d+: error', q.stderr))
    ei = iter(encs)
    for k, (w, t) in enumerate(ok):
        if k in failed: continue
        e = next(ei, None)
        if e is None: break
        if int(e[0], 16) | int(e[1], 16) << 8 | int(e[2], 16) << 16 | int(e[3], 16) << 24 == w:
            valid.append(w); texts.append(t)
SKIP_SVE = re.compile(r'^(ld|st|prf|ldr|str)')       # (loads and stores need valid addresses: the "sve_mem" mode)
SKIP = re.compile(r'^(ld|st|prf|cas|swp|ldr|str|udf|brk|hlt|svc|hvc|smc|dcps|eret|drps|msr|mrs|sys|at |dc |ic |tlbi|wf|sev|hint|yield|nop|bti|paci|auti|xpac|ret|br|bl|b\.|cb|tb|isb|dsb|dmb|clrex|ssbb|pssbb|sb|esb|psb|tsb|csdb|cpy|set|rcw|gcs|chk|irg|gmi|addg|subg|stg|stz|ldg|zero|sm(start|stop)|rdffr|setffr|wrffr|rdvl|addvl|addpl|rdsvl|addsvl|addspl|ld[0-9]|st[0-9])')
def key(t):
    m = t.split()[0]
    shape = re.sub(r'[0-9]+', '#', t.split(None, 1)[1]) if ' ' in t else ''
    shape = re.sub(r'\bx#|\bw#|\bxzr|\bwzr|\bsp\b', 'R', shape)
    shape = re.sub(r'#-?[0-9a-fx.]+', 'I', shape)
    return m + ' ' + shape
groups = collections.defaultdict(list)
for w, t in zip(valid, texts):
    if SME:
        if re.search(r'\bsp\b|\bx27\b|\bx28\b|\bz31\b', t): continue
    elif SVEMEM:
        # memory forms only; the ones that need the stack pointer or a 128-bit element (SVE2.1) are skipped
        # (qemu does not load a first-fault or non-fault element that is the first active one but sits in the upper half of the register, which the architecture does not allow: those are
        # covered by the unit tests instead)
        if not re.match(r'^(ld[1-4]|ldnt1|st[1-4]|stnt1|prf)', t) or re.search(r'\bza|\bpn[0-9]|\bzt0|\bsp\b|\bz31\b|\bx27\b|\bx28\b', t): continue
    elif SVE:
        if SKIP_SVE.match(t) or re.search(r'\bza|\bzt0|\[x[0-9]|\[sp|\bsm(start|stop)', t): continue
    else:
        if SKIP.match(t): continue
        if re.search(r'\bz#|\bz[0-9]|\bp[0-9]+[/,.]|\bpn|\bza|\[x[0-9]|\[sp', t): continue
    if re.match(r'^\S+\s+w?sp\b', t): continue        # writes the stack pointer, which the test program needs
    if flt and not flt.search(t): continue
    groups[key(t)].append((w, t))
out = []
for k, lst in groups.items():
    rng.shuffle(lst)
    out.extend(lst[:per_key])
rng.shuffle(out)
with open(work + '/sample.txt', 'w') as f:
    for w, t in out: f.write('%08x %s\n' % (w, t))
print('valid canonical words', len(valid), 'groups', len(groups), 'sampled', len(out))

# keep only what the emulator implements (tests/aarch64_diff/probe built by aarch64_diff.sh), and report the rest
import os
probe = os.environ.get('A64_PROBE')
if probe:
    rows = [l.rstrip('\n').split(' ', 1) for l in open(work + '/sample.txt')]
    data = b''.join(struct.pack('<I', int(w, 16)) for w, _ in rows)
    res = subprocess.run([probe], input=data, capture_output=True).stdout
    kept = [r for r, st in zip(rows, res) if st != 1]
    # drop what qemu does not implement either (SIGILL), so the comparison only covers instructions both can run
    qp = os.environ.get('A64_QPROBE')
    if qp and kept:
        data = b''.join(struct.pack('<I', int(w, 16)) for w, _ in kept)
        def qrun(blob, limit):
            return subprocess.run(['qemu-aarch64', '-cpu', 'max,sve-max-vq=1,sme128=on', qp], input=blob, capture_output=True, timeout=limit).stdout
        qres = b''
        for base in range(0, len(kept), 200):      # (a word can hang qemu, e.g. a faulting store in streaming mode: such a chunk is redone word by word and the hanging word counts as rejected)
            chunk = data[4 * base:4 * (base + 200)]
            try:
                qres += qrun(chunk, 30)
            except subprocess.TimeoutExpired:
                for k in range(len(chunk) // 4):
                    try: qres += qrun(chunk[4 * k:4 * k + 4], 3)
                    except subprocess.TimeoutExpired: qres += bytes([1])
        if len(qres) == len(kept):
            lost = collections.Counter(t.split()[0] for (w, t), st in zip(kept, qres) if st == 1)
            kept = [r for r, st in zip(kept, qres) if st != 1]
            if lost: print('qemu rejects:', ' '.join('%s:%d' % kv for kv in lost.most_common(30)))
        else:
            print('qprobe failed (%d of %d results); keeping all' % (len(qres), len(kept)))
    missing = collections.Counter(t.split()[0] for (w, t), st in zip(rows, res) if st == 1)
    with open(work + '/missing.txt', 'w') as f:
        for (w, t), st in zip(rows, res):
            if st == 1: f.write('%s %s\n' % (w, t))
    with open(work + '/sample.txt', 'w') as f:
        for w, t in kept: f.write('%s %s\n' % (w, t))
    print('emulator lacks %d of %d sampled forms; kept %d' % (sum(missing.values()), len(rows), len(kept)))
    if missing: print('  missing mnemonics:', ' '.join('%s:%d' % kv for kv in missing.most_common(40)))
