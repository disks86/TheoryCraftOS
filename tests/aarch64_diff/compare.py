#!/usr/bin/env python3
# Compares the state dumps of a test program run under qemu-aarch64 and under the emulator. Usage: compare.py <workdir>
import sys
w = sys.argv[1]
q = open(w + '/q.bin', 'rb').read()
raw = open(w + '/e.raw', 'rb').read()
i = raw.rfind(b'[exit=')
e = raw[:i]
blk, xo, vo, rx, rn, rf, rv, bo, rp = map(int, open(w + '/layout.txt').read().split())
cases = open(w + '/cases.txt').read().split('\n')
print(raw[i:].decode().strip(), 'sizes', len(q), len(e))
bad = 0
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
            if a[bo:] != b[bo:]:
                diffs.append('MEMORY q=%s e=%s' % (a[bo:bo + 160].hex(), b[bo:bo + 160].hex()))
            print('MISMATCH #%d: %s' % (ci, text))
            print('   ', '; '.join(diffs[:5]))
print('cases', len([c for c in cases if c]), 'mismatches', bad)
sys.exit(1 if bad else 0)
