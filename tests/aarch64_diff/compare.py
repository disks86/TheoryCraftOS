#!/usr/bin/env python3
# Compares the state dumps of a test program run under qemu-aarch64 and under the emulator. Usage: compare.py <workdir>
import sys
w = sys.argv[1]
q = open(w + '/q.bin', 'rb').read()
raw = open(w + '/e.raw', 'rb').read()
i = raw.rfind(b'[exit=')
e = raw[:i]
blk, xo, vo, rx, rn, rf, rv, bo, rp, rza, rzt = map(int, open(w + '/layout.txt').read().split())
cases = open(w + '/cases.txt').read().split('\n')
print(raw[i:].decode().strip(), 'sizes', len(q), len(e))
bad = 0
names = {}
for ci, text in enumerate(cases):
    if not text:
        continue
    a = q[ci * blk:(ci + 1) * blk]
    b = e[ci * blk:(ci + 1) * blk]
    if len(b) < blk:
        print('emulator stopped before case', ci, text)
        bad += 1
        break
    if a[rx:] != b[rx:] or False:
        bad += 1
        names.setdefault(text.split('//')[-1].split()[0] if '//' in text else text.split()[0], []).append(text)
        if bad <= 12:
            diffs = []
            for k in range(31):
                x = int.from_bytes(a[rx + k * 8:rx + k * 8 + 8], 'little')
                y = int.from_bytes(b[rx + k * 8:rx + k * 8 + 8], 'little')
                if x != y:
                    diffs.append('x%d q=%x e=%x' % (k, x, y))
            for nm, off in (('nzcv', rn), ('fpsr', rf)):
                x = int.from_bytes(a[off:off + 8], 'little')
                y = int.from_bytes(b[off:off + 8], 'little')
                if x != y:
                    diffs.append('%s q=%x e=%x' % (nm, x, y))
            for k in range(32):
                x = a[rv + k * 16:rv + k * 16 + 16]
                y = b[rv + k * 16:rv + k * 16 + 16]
                if x != y:
                    diffs.append('v%d q=%s e=%s' % (k, x[::-1].hex(), y[::-1].hex()))
            if a[rp:rp + 34] != b[rp:rp + 34]:
                pq = [int.from_bytes(a[rp + 2 * k:rp + 2 * k + 2], 'little') for k in range(17)]
                pe = [int.from_bytes(b[rp + 2 * k:rp + 2 * k + 2], 'little') for k in range(17)]
                diffs.append(' '.join('%s q=%04x e=%04x' % ('p%d' % k if k < 16 else 'ffr', pq[k], pe[k]) for k in range(17) if pq[k] != pe[k]))
            if a[rza:rza + 256] != b[rza:rza + 256]:
                diffs.append('ZA ' + ' '.join('row%d q=%s e=%s' % (r, a[rza + 16 * r:rza + 16 * r + 16][::-1].hex(), b[rza + 16 * r:rza + 16 * r + 16][::-1].hex()) for r in range(16) if a[rza + 16 * r:rza + 16 * r + 16] != b[rza + 16 * r:rza + 16 * r + 16])[:600])
            if a[rzt:rzt + 64] != b[rzt:rzt + 64]:
                diffs.append('ZT0 q=%s e=%s' % (a[rzt:rzt + 64][::-1].hex(), b[rzt:rzt + 64][::-1].hex()))
            if a[bo:] != b[bo:]:
                diffs.append('MEMORY q=%s e=%s' % (a[bo:bo + 160].hex(), b[bo:bo + 160].hex()))
            print('MISMATCH #%d: %s' % (ci, text))
            print('   ', '; '.join(diffs[:6]))
if bad > 12: print('mismatching mnemonics:', ' '.join('%s:%d' % (k, len(v)) for k, v in sorted(names.items(), key=lambda kv: -len(kv[1]))))
print('cases', len([c for c in cases if c]), 'mismatches', bad)
sys.exit(1 if bad else 0)
