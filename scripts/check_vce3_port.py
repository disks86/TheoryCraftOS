#!/usr/bin/env python3
"""Checks that the VCE 3 port (src/Hardware/VideoCodec/AMD/AmdVce3.cpp) touches registers in the same order as the Linux driver it was ported from.

  check_vce3_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/vce_v3_0.c

For each ported function it takes the ordered list of register names (mm... / ix...) and register field names (REG__FIELD_MASK / __SHIFT) in the kernel function and
in the port, and compares them. A port that drops, adds, reorders or mistypes a register access fails. The values written are not compared (tests/AmdVce3Test.cpp checks
them). Left out of the kernel side on purpose: the registers of the chips before Stoney (mmVCE_LMI_VCPU_CACHE_40BIT_BAR and the VCE_VCPU_CNTL.CLK_EN branches, which this port does not
have), the locking and the SMC/clock calls that are not ported. The kernel's vce_v3_0_start has the ring programming for three rings; the port's run_start has the same registers.
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


def tokens(text, drop=()):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    text = '\n'.join(l for l in text.split('\n') if not l.lstrip().startswith('#'))
    found = re.findall(r'\b((?:mm|ix)[A-Z][A-Z0-9_]*|[A-Z][A-Z0-9_]*__[A-Z0-9_]+)\b', text)
    # WREG32_FIELD(REG, FIELD, v) names a field of a register through the macro; count it as the register and its field.
    for m in re.finditer(r'WREG32_FIELD\((\w+),\s*(\w+),', text):
        pass
    return [t for t in found if t not in drop]


def fields(text):
    """The register accesses in source order: register names, and REG__FIELD for WREG32_FIELD(REG, FIELD, ...)."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    text = '\n'.join(l for l in text.split('\n') if not l.lstrip().startswith('#'))
    out = []
    for m in re.finditer(r'\b((?:mm|ix)[A-Z][A-Z0-9_]*|[A-Z][A-Z0-9_]*__[A-Z0-9_]+)\b|WREG32_FIELD\((\w+),\s*(\w+),', text):
        out.append(m.group(1) if m.group(1) else f'{m.group(2)}__{m.group(3)}')
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    kernel = open(sys.argv[1]).read()
    port = open(os.path.join(here, '..', 'src/Hardware/VideoCodec/AMD/AmdVce3.cpp')).read()
    # (name, kernel regex, port regex, kernel tokens to leave out, port tokens to leave out)
    pairs = [
        ('override_vce_clock_gating', r'static void vce_v3_0_override_vce_clock_gating\([^)]*\)\s*\{', r'void AmdVce3::override_vce_clock_gating\([^)]*\)\s*\{', (), ()),
        ('set_vce_sw_clock_gating', r'static void vce_v3_0_set_vce_sw_clock_gating\([^)]*\)\s*\{', r'void AmdVce3::set_vce_sw_clock_gating\([^)]*\)\s*\{', (), ()),
        ('firmware_loaded', r'static int vce_v3_0_firmware_loaded\([^)]*\)\s*\{', r'bool AmdVce3::firmware_loaded\(\)\s*\{', (), ()),
        ('mc_resume', r'static void vce_v3_0_mc_resume\([^)]*\)\s*\{', r'void AmdVce3::mc_resume\([^)]*\)\s*\{', ('mmVCE_LMI_VCPU_CACHE_40BIT_BAR',), ()),
        ('start', r'static int vce_v3_0_start\([^)]*\)\s*\{', r'AmdVce3::Status AmdVce3::run_start\([^)]*\)\s*\{', ('VCE_VCPU_CNTL__CLK_EN',), ()),
        ('stop', r'static int vce_v3_0_stop\([^)]*\)\s*\{', r'void AmdVce3::stop\(\)\s*\{', ('VCE_VCPU_CNTL__CLK_EN',), ()),
        ('set_clockgating_state', r'static int vce_v3_0_set_clockgating_state\([^)]*\)\s*\{', r'void AmdVce3::set_clockgating_state\([^)]*\)\s*\{', (), ()),
        ('get_harvest_config', r'static unsigned vce_v3_0_get_harvest_config\([^)]*\)\s*\{', r'uint32_t AmdVce3::read_harvest_config\([^)]*\)\s*\{', (), ()),
    ]
    failed = False
    for name, kernel_re, port_re, kernel_drop, port_drop in pairs:
        kb, pb = body(kernel, kernel_re), body(port, port_re)
        if name == 'get_harvest_config':
            # Only the fuse register is read through the SMC; the APU branch, Fiji and Stoney are not ported.
            kb = kb[kb.index('tmp = (RREG32_SMC(ixCC_HARVEST_FUSES)'):]
        k = [t for t in fields(kb) if t not in kernel_drop]
        p = [t for t in fields(pb) if t not in port_drop]
        if name == 'get_harvest_config':
            # the port reads the SMC through a helper that writes the index register first
            p = [t for t in p if t not in ('mmSMC_IND_INDEX_0', 'mmSMC_IND_DATA_0')]
        print(f'{name:28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
        if k != p:
            failed = True
            for i, (a, b) in enumerate(zip(k, p)):
                if a != b:
                    print(f'    first difference at {i}: kernel {a}, port {b}')
                    break
            else:
                print(f'    one list is a prefix of the other: kernel {k[len(p):] if len(k) > len(p) else ""} port {p[len(k):] if len(p) > len(k) else ""}')
    sys.exit(1 if failed else 0)


main()
