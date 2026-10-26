#!/usr/bin/env python3
"""Checks that the GMC 8 set-up (src/Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.cpp) touches registers in the same order as the Linux driver it was ported from, and
that its golden register table is the driver's, value for value.

  check_gmc8_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c

For each ported function it takes the ordered list of register names (mm...) in the kernel function and in the port and compares them. A port that drops, adds, reorders or
mistypes a register access fails. The values written are compared only for the golden table (the rest are checked by tests/AmdGmc8Test.cpp). Left out of the port on purpose,
and so of the comparison: the VGA lockout (display block registers), the SR-IOV branch, and the memory controller microcode load (not run for the VEGAM).
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
    port = open(os.path.join(here, '..', 'src/Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.cpp')).read()
    failed = False

    k = golden(kernel, ['golden_settings_polaris11_a11'], False)
    p = golden(port, ['kGoldenVegam'], True)
    print(f'{"golden registers":28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
    if k != p:
        failed = True

    def kbody(name):
        return body(kernel, r'static (?:int|void|u32) %s\([^)]*\)\s*\{' % name)

    def pbody(name):
        return body(port, r'(?:void|bool|uint32_t) AmdGmc8::%s\([^)]*\)\s*\{' % name)

    def cut_between(text, start, end):
        i = text.index(start)
        return text[:i] + text[text.index(end, i):]

    # mc_program: without the VGA lockout and the SR-IOV branch
    mk = kbody('gmc_v8_0_mc_program')
    mk = cut_between(mk, 'if (adev->mode_info.num_crtc) {', '/* Update configuration */')
    mk = cut_between(mk, 'if (amdgpu_sriov_vf(adev)) {', 'WREG32(mmMC_VM_AGP_BASE')
    # gart_enable: the fault default and the TLB flush are separate functions in the driver and inline calls here
    gk = kbody('gmc_v8_0_gart_enable')
    gk = cut_between(gk, 'if (amdgpu_vm_fault_stop', 'gmc_v8_0_flush_gpu_tlb(adev, 0, 0, 0);')
    gk = gk.replace('gmc_v8_0_flush_gpu_tlb(adev, 0, 0, 0);', kbody('gmc_v8_0_set_fault_enable_default') + kbody('gmc_v8_0_flush_gpu_tlb'))
    gk = gk.replace('amdgpu_gtt_mgr_recover', '')
    gp = pbody('gart_enable').replace('flush_tlb(0);', pbody('flush_tlb'))
    pairs = [
        ('mc_program', tokens(mk), tokens(pbody('mc_program'))),
        ('gart_enable', tokens(gk), tokens(gp)),
        ('gart_disable', tokens(kbody('gmc_v8_0_gart_disable')), tokens(pbody('stop'))),
        ('wait_for_idle', tokens(kbody('gmc_v8_0_wait_for_idle')), tokens(pbody('wait_for_idle'))),
        ('flush tlb', tokens(kbody('gmc_v8_0_flush_gpu_tlb')), tokens(pbody('flush_tlb'))),
    ]
    for name, k, p in pairs:
        print(f'{name:28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
        if k != p:
            failed = True
            print(f'    kernel {k}\n    port   {p}')
    sys.exit(1 if failed else 0)


main()
