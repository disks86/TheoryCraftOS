#!/usr/bin/env python3
"""Checks that the interrupt handler ring (src/Hardware/GraphicsProcessingUnit/AMD/AmdIh3.cpp) touches registers in the same order as the Linux driver it was ported from, and
that its golden register table is the driver's, value for value.

  check_ih3_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/tonga_ih.c

For each ported function it takes the ordered list of register names (mm...) in the kernel function and in the port and compares them. The values written are checked by
tests/AmdIh3Test.cpp. Not ported on purpose: the PCI bus mastering call and the software ring.
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
    port = open(os.path.join(here, '..', 'src/Hardware/GraphicsProcessingUnit/AMD/AmdIh3.cpp')).read()
    failed = False

    def kbody(name):
        return body(kernel, r'static\s+(?:int|void|u32)\s+%s\([^)]*\)\s*\{' % name)

    def pbody(name):
        return body(port, r'(?:void|bool|uint32_t|AmdIh3::Result) AmdIh3::%s\([^)]*\)\s*\{' % name)

    pairs = [
        ('enable_interrupts', tokens(kbody('tonga_ih_enable_interrupts')), tokens(pbody('enable_interrupts'))),
        ('disable_interrupts', tokens(kbody('tonga_ih_disable_interrupts')), tokens(pbody('disable_interrupts'))),
        ('irq_init', tokens(kbody('tonga_ih_irq_init')), tokens(pbody('start'))),
        ('get_wptr', tokens(kbody('tonga_ih_get_wptr')), tokens(pbody('get_wptr'))),
        ('set_rptr', tokens(kbody('tonga_ih_set_rptr')), tokens(pbody('set_rptr'))),
    ]
    for name, k, p in pairs:
        print(f'{name:28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
        if k != p:
            failed = True
            print(f'    kernel {k}\n    port   {p}')
    sys.exit(1 if failed else 0)


main()
