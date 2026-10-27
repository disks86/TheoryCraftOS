#!/usr/bin/env python3
"""Checks that the DCE 11.2 plane (src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.cpp) touches registers in the same order as the Linux driver it was ported from, and
that its golden register table is the driver's, value for value.

  check_dce11_port.py path/to/dce_v11_0.c (kernel 6.12: later kernels no longer have it)

For each ported function it takes the ordered list of register names (mm...) in the kernel function and in the port and compares them. The values written are checked by
tests/AmdDce11Test.cpp. The port adds the wait for the update and the put-back around the flip, so only the flip's own register accesses (the first six) are compared.
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


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    kernel = open(sys.argv[1]).read()
    port = open(os.path.join(here, '..', 'src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.cpp')).read()
    k = tokens(body(kernel, r'static void dce_v11_0_page_flip\([^)]*\)\s*\{'))
    p = tokens(body(port, r'bool Plane::flip\([^)]*\)\s*\{'))[:len(k)]
    print(f'{"page flip":28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
    if k != p:
        print(f'    kernel {k}\n    port   {p}')
    # the registers set_base programs for the surface are not written by the port (it only moves the address), but every register the port reads to recognise a plane is one set_base writes
    base = set(tokens(body(kernel, r'static int dce_v11_0_crtc_do_set_base\([^)]*\)\s*\{'))) | set(tokens(body(kernel, r'static void dce_v11_0_grph_enable\([^)]*\)\s*\{')))
    probe = set(tokens(body(port, r'bool Plane::probe\([^)]*\)\s*\{'))) - {'mmCRTC_CONTROL', 'mmMC_VM_SYSTEM_APERTURE_LOW_ADDR', 'mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR'}
    missing = sorted(probe - base)
    print(f'{"probe registers":28s} {"all programmed by set_base" if not missing else "NOT in set_base: " + ", ".join(missing)}')
    sys.exit(0 if k == p and not missing else 1)


main()
