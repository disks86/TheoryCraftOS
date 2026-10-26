#!/usr/bin/env python3
"""Checks that the UVD 6 port (src/Hardware/VideoCodec/AMD/AmdUvd6.cpp) touches registers in the same order as the Linux driver it was ported from.

  check_uvd6_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/uvd_v6_0.c

For each ported function it takes the ordered list of register names (mm...) and register field names (REG__FIELD_MASK / __SHIFT) in the kernel function and
in the port, and compares them. A port that drops, adds, reorders or mistypes a register access fails. The values written are not compared (they are checked by
tests/AmdVcn1Test.cpp). Not ported on purpose: the encode rings of uvd_v6_0_start (uvd_v6_0_enc_support), the hardware clock gating that is compiled out, and hw_init's clock and ring tests.
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


def tokens(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    text = '\n'.join(l for l in text.split('\n') if not l.lstrip().startswith('#'))   # preprocessor lines (the big-endian swap) are not ported
    return re.findall(r'\b((?:mm|ix)[A-Z][A-Z0-9_]*|[A-Z][A-Z0-9_]*__[A-Z0-9_]+)\b', text)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    kernel = open(sys.argv[1]).read()
    port = open(os.path.join(here, '..', 'src/Hardware/VideoCodec/AMD/AmdUvd6.cpp')).read()
    pairs = [
        ('enable_mgcg', r'static void uvd_v6_0_enable_mgcg\([^)]*\)\s*\{', r'void AmdUvd6::enable_mgcg\([^)]*\)\s*\{'),
        ('enable_clock_gating', r'static void uvd_v6_0_enable_clock_gating\([^)]*\)\s*\{', r'void AmdUvd6::enable_clock_gating\([^)]*\)\s*\{'),
        ('set_sw_clock_gating', r'static void uvd_v6_0_set_sw_clock_gating\([^)]*\)\s*\{', r'void AmdUvd6::set_sw_clock_gating\(\)\s*\{'),
        ('mc_resume', r'static void uvd_v6_0_mc_resume\([^)]*\)\s*\{', r'void AmdUvd6::mc_resume\([^)]*\)\s*\{'),
        ('stop', r'static void uvd_v6_0_stop\([^)]*\)\s*\{', r'void AmdUvd6::stop\(\)\s*\{'),
    ]
    failed = False
    for name, kernel_re, port_re in pairs:
        k, p = tokens(body(kernel, kernel_re)), tokens(body(port, port_re))
        print(f'{name:28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
        if k != p:
            failed = True
            for i, (a, b) in enumerate(zip(k, p)):
                if a != b:
                    print(f'    first difference at {i}: kernel {a}, port {b}')
                    break
    # start: the kernel's encode rings are the one part left out.
    kb = body(kernel, r'static int uvd_v6_0_start\([^)]*\)\s*\{')
    enc = kb.index('if (uvd_v6_0_enc_support(adev))')
    depth, end = 0, enc
    for j in range(kb.index('{', enc), len(kb)):
        depth += kb[j] == '{'
        depth -= kb[j] == '}'
        if depth == 0:
            end = j + 1
            break
    k = tokens(kb[:enc] + kb[end:])
    p = tokens(body(port, r'AmdUvd6::Status AmdUvd6::run_start\([^)]*\)\s*\{'))
    print(f'{"start (decode ring)":28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
    if k != p:
        failed = True
        for i, (a, b) in enumerate(zip(k, p)):
            if a != b:
                print(f'    first difference at {i}: kernel {a}, port {b}')
                break
    sys.exit(1 if failed else 0)


main()
