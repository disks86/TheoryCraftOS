#!/usr/bin/env python3
# Compares the state dumps of a test program run under qemu-riscv64 and under the emulator. Usage: compare.py <workdir>
import sys
w = sys.argv[1]
q = open(w + '/q.bin', 'rb').read()
raw = open(w + '/e.raw', 'rb').read()
i = raw.rfind(b'[exit=')
e = raw[:i]
lay = list(map(int, open(w + '/layout.txt').read().split()))
blk, rx, rf, rc, bo = lay[:5]
rv, rvc, bufsize = (lay[5:8] + [0, 0, 256])[:3] if len(lay) >= 8 else (0, 0, 256)
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
    if a[bo:] != b[bo:]:
        bad += 1
        if bad <= 12:
            diffs = []
            for k in range(30):
                x = int.from_bytes(a[rx + k * 8:rx + k * 8 + 8], 'little')
                y = int.from_bytes(b[rx + k * 8:rx + k * 8 + 8], 'little')
                if x != y:
                    diffs.append('x%d q=%x e=%x' % (k + 1, x, y))
            for k in range(32):
                x = int.from_bytes(a[rf + k * 8:rf + k * 8 + 8], 'little')
                y = int.from_bytes(b[rf + k * 8:rf + k * 8 + 8], 'little')
                if x != y:
                    diffs.append('f%d q=%x e=%x' % (k, x, y))
            x = int.from_bytes(a[rc:rc + 8], 'little')
            y = int.from_bytes(b[rc:rc + 8], 'little')
            if x != y:
                diffs.append('fcsr q=%x e=%x' % (x, y))
            if rv:
                for k in range(32):
                    x = a[rv + k * 16:rv + k * 16 + 16]
                    y = b[rv + k * 16:rv + k * 16 + 16]
                    if x != y:
                        diffs.append('v%d q=%s e=%s' % (k, x[::-1].hex(), y[::-1].hex()))
                for k, name in enumerate(['vl', 'vtype', 'vxsat', 'vxrm']):
                    x = int.from_bytes(a[rvc + k * 8:rvc + k * 8 + 8], 'little')
                    y = int.from_bytes(b[rvc + k * 8:rvc + k * 8 + 8], 'little')
                    if x != y:
                        diffs.append('%s q=%x e=%x' % (name, x, y))
            if a[bo:bo + bufsize] != b[bo:bo + bufsize]:
                diffs.append('MEMORY differs')
            print('MISMATCH #%d: %s' % (ci, text))
            print('   ', '; '.join(diffs[:8]))
print('cases', len([c for c in cases if c]), 'mismatches', bad)
sys.exit(1 if bad else 0)
