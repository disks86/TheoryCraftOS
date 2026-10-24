#!/usr/bin/env python3
"""Checks that the VCN 1.0 port (src/Hardware/VideoCodec/AMD/AmdVcn1.cpp) touches registers in the same order as the Linux driver it was ported from.

  check_vcn1_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/vcn_v1_0.c

For each ported function it takes the ordered list of register names (mm...) and register field names (REG__FIELD_MASK / __SHIFT) in the kernel function and
in the port, and compares them. A port that drops, adds, reorders or mistypes a register access fails. The values written are not compared (they are checked by
tests/AmdVcn1Test.cpp). Not ported on purpose: the PSP branch of mc_resume, the DPG mode, and jpeg_v1_0_start.
"""
import os
import re
import sys


def body(src, header_regex):
    m = re.search(header_regex, src)
    if not m:
        sys.exit(f'function not found: {header_regex}')
    i = src.index('{', m.end())
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
    return re.findall(r'\b(mm[A-Z][A-Z0-9_]*|[A-Z][A-Z0-9_]*__[A-Z0-9_]+)\b', text)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    kernel = open(sys.argv[1]).read()
    port = open(os.path.join(here, '..', 'src/Hardware/VideoCodec/AMD/AmdVcn1.cpp')).read()
    pairs = [
        ('disable_clock_gating', r'static void vcn_v1_0_disable_clock_gating\([^)]*\)', r'void AmdVcn1::disable_clock_gating\(\)'),
        ('enable_clock_gating', r'static void vcn_v1_0_enable_clock_gating\([^)]*\)', r'void AmdVcn1::enable_clock_gating\(\)'),
        ('disable_static_power_gating', r'static void vcn_1_0_disable_static_power_gating\([^)]*\)', r'void AmdVcn1::disable_static_power_gating\(\)'),
        ('enable_static_power_gating', r'static void vcn_1_0_enable_static_power_gating\([^)]*\)', r'void AmdVcn1::enable_static_power_gating\(\)'),
        ('stop_spg_mode', r'static int vcn_v1_0_stop_spg_mode\([^)]*\)', r'void AmdVcn1::stop\(\)'),
        ('start_spg_mode', r'static int vcn_v1_0_start_spg_mode\([^)]*\)', r'AmdVcn1::Status AmdVcn1::start_spg_mode\([^)]*\)'),
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
    # mc_resume: the kernel branches on the firmware load type; the port is the direct-load branch (everything before the branch, then the else part).
    kb = body(kernel, r'static void vcn_v1_0_mc_resume_spg_mode\([^)]*\)')
    k = tokens(kb.split('if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP)', 1)[0]) + tokens(kb.split('} else {', 1)[1])
    p = tokens(body(port, r'void AmdVcn1::mc_resume\([^)]*\)'))
    print(f'{"mc_resume (direct load)":28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
    failed = failed or k != p
    sys.exit(1 if failed else 0)


main()
