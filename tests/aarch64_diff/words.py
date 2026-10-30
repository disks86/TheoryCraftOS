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
words = []
while len(words) < candidates:
    w = rng.getrandbits(32)
    if (w >> 26) & 7 == 5: continue                 # branches
    if (w >> 24) in (0xD4, 0xD5, 0xD6, 0xD7): continue    # exceptions, system, branches to register
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
SKIP = re.compile(r'^(ld|st|prf|cas|swp|ldr|str|udf|brk|hlt|svc|hvc|smc|dcps|eret|drps|msr|mrs|sys|at |dc |ic |tlbi|wf|sev|hint|yield|nop|bti|paci|auti|xpac|ret|br|bl|b\.|cb|tb|isb|dsb|dmb|clrex|ssbb|pssbb|sb|esb|psb|tsb|csdb|cpy|set|rcw|gcs|chk|irg|gmi|addg|subg|stg|stz|ldg|zero|sm(start|stop)|rdffr|setffr|wrffr|rdvl|addvl|addpl|rdsvl|addsvl|addspl|ld[0-9]|st[0-9])')
def key(t):
    m = t.split()[0]
    shape = re.sub(r'[0-9]+', '#', t.split(None, 1)[1]) if ' ' in t else ''
    shape = re.sub(r'\bx#|\bw#|\bxzr|\bwzr|\bsp\b', 'R', shape)
    shape = re.sub(r'#-?[0-9a-fx.]+', 'I', shape)
    return m + ' ' + shape
groups = collections.defaultdict(list)
for w, t in zip(valid, texts):
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
    missing = collections.Counter(t.split()[0] for (w, t), st in zip(rows, res) if st == 1)
    with open(work + '/sample.txt', 'w') as f:
        for w, t in kept: f.write('%s %s\n' % (w, t))
    print('emulator lacks %d of %d sampled forms; kept %d' % (sum(missing.values()), len(rows), len(kept)))
    if missing: print('  missing mnemonics:', ' '.join('%s:%d' % kv for kv in missing.most_common(40)))
