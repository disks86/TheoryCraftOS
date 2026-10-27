#!/usr/bin/env python3
"""Checks that the SDMA engines (src/Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.cpp) touches registers in the same order as the Linux driver it was ported from, and
that its golden register table is the driver's, value for value.

  check_sdma3_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c

For each ported function it takes the ordered list of register names (mm...) in the kernel function and in the port and compares them. The values written are checked by
tests/AmdSdma3Test.cpp. Not ported on purpose: the phase quantum of the context switch, the SR-IOV poll memory branch and the big-endian swaps; the golden table is compared by value.
"""
import os
import re
import sys


def body(src, header_regex):
    m = re.search(header_regex, src)
    if not m:
        sys.exit(f'function not found: {header_regex}')
    i = m.end() - 1   # (the pattern ends at the opening brace)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    sys.exit('unbalanced braces')


def strip(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return '\n'.join(l for l in text.split('\n') if not l.lstrip().startswith('#'))


def tokens(text):
    text = strip(text)
    text = re.sub(r'WREG32_FIELD\(\s*(\w+),[^;]*;', lambda m: f'mm{m.group(1)} mm{m.group(1)};', text)
    return re.findall(r'\b(mm[A-Z][A-Z0-9_]*)\b', text)


def golden(src, names, local):
    rows = []
    for name in names:
        m = re.search(r'(?:static )?const(?:expr)? (?:u32|GoldenRegister)\s+%s\[\]\s*=\s*\{(.*?)\};' % name, src, re.S)
        if not m:
            sys.exit(f'table not found: {name}')
        rows += re.findall(r'\{?\s*(mm\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)', strip(m.group(1)))
    return [(r, int(a, 16), int(o, 16)) for r, a, o in rows]


def golden(src, name):
    m = re.search(r'(?:static )?const(?:expr)? (?:u32|GoldenRegister)\s+%s\[\]\s*=\s*\{(.*?)\};' % name, src, re.S)
    if not m:
        sys.exit(f'table not found: {name}')
    rows = re.findall(r'\{?\s*(mm\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)', strip(m.group(1)))
    return [(r, int(a, 16), int(o, 16)) for r, a, o in rows]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    kernel = open(sys.argv[1]).read()
    port = open(os.path.join(here, '..', 'src/Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.cpp')).read()
    failed = False
    k, p = golden(kernel, 'golden_settings_polaris11_a11'), golden(port, 'kGolden')
    print(f'{"golden registers":28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
    failed = k != p

    def kbody(name):
        return body(kernel, r'static\s+(?:int|void|u32|uint64_t)\s+%s\([^)]*\)\s*\{' % name)

    def pbody(name):
        return body(port, r'(?:void|bool|uint32_t) AmdSdma3::%s\([^)]*\)\s*\{' % name)

    start_k = kbody('sdma_v3_0_gfx_resume')
    pairs = [
        ('gfx_stop', tokens(kbody('sdma_v3_0_gfx_stop')), tokens(pbody('gfx_stop'))),
        ('ctx_switch_enable', [t for t in tokens(kbody('sdma_v3_0_ctx_switch_enable')) if not t.startswith('mmSDMA0_PHASE')], tokens(pbody('ctx_switch_enable'))),
        ('enable', tokens(kbody('sdma_v3_0_enable')), tokens(pbody('enable'))),
        ('gfx_resume', [t for t in tokens(start_k) if t not in ('mmSDMA0_GFX_RB_WPTR',)], [t for t in tokens(pbody('gfx_resume')) if t != 'mmSRBM_GFX_CNTL']),   # (vi_srbm_select is written out in the port)
        ('ring test (register use)', tokens(kbody('sdma_v3_0_ring_test_ring')), tokens(pbody('test_ring'))),
    ]
    for name, kk, pp in pairs:
        print(f'{name:28s} kernel {len(kk):3d}  port {len(pp):3d}  {"same" if kk == pp else "DIFFERENT"}')
        if kk != pp:
            failed = True
            print(f'    kernel {kk}\n    port   {pp}')
    sys.exit(1 if failed else 0)


main()
