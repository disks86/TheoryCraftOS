#!/usr/bin/env python3
"""Checks that the GFX 8 start-up (src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.cpp) touches registers in the same order as the Linux driver it was ported from, and
that its golden register table is the driver's, value for value.

  check_gfx8_port.py path/to/linux/drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c

For each ported function it takes the ordered list of register names (mm...) in the kernel function and in the port and compares them; WREG32_FIELD(REG, ...) counts as
a read and a write of REG (the port spells the read-modify-write out). A port that drops, adds, reorders or mistypes a register access fails. The values written are
compared only for the golden table (the rest are checked by tests/AmdGfx8Test.cpp). Left out of the port on purpose, and so of the comparison: the doorbell range registers
(see AmdGfx8.hpp), the Tonga-only branch of mqd_commit and the queue priority of mqd_init. The GDS registers are numbered by arithmetic in the port, by a table in the kernel, and are not compared.
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
    port = open(os.path.join(here, '..', 'src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.cpp')).read()
    failed = False

    k = golden(kernel, ['golden_settings_vegam_a11', 'vegam_golden_common_all'], False)
    p = golden(port, ['kGoldenVegam'], True)
    print(f'{"golden registers":28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
    if k != p:
        failed = True
        for i, (a, b) in enumerate(zip(k, p)):
            if a != b:
                print(f'    first difference at {i}: kernel {a}, port {b}')
                break

    def kern(name):
        return tokens(body(kernel, r'static\s+(?:int|void|u32)\s+%s\([^)]*\)\s*\{' % name))

    def mine(name):
        return tokens(body(port, r'(?:void|bool|uint32_t) AmdGfx8::%s\([^)]*\)\s*\{' % name))

    def cut(text, start, end):
        return text[text.index(start):text.index(end)]

    resume_k = body(kernel, r'static int gfx_v8_0_cp_gfx_resume\([^)]*\)\s*\{')
    resume_p = body(port, r'void AmdGfx8::cp_resume_ring\([^)]*\)\s*\{')
    start_k = body(kernel, r'static int gfx_v8_0_cp_gfx_start\([^)]*\)\s*\{')
    door_k = body(kernel, r'static void gfx_v8_0_set_cpg_door_bell\([^)]*\)\s*\{')
    compute = open(os.path.join(here, '..', 'src/Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Compute.cpp')).read()

    def mine_c(name):
        return tokens(body(compute, r'(?:void|bool|uint32_t) AmdGfx8Compute::%s\([^)]*\)\s*\{' % name))

    pairs = [
        ('rlc_stop', kern('gfx_v8_0_rlc_stop') + kern('gfx_v8_0_enable_gui_idle_interrupt'), mine('rlc_stop') + mine('gui_idle_interrupt')),
        ('rlc_reset', kern('gfx_v8_0_rlc_reset'), mine('rlc_reset')),
        ('rlc_start', kern('gfx_v8_0_rlc_start') + kern('gfx_v8_0_enable_gui_idle_interrupt'), mine('rlc_start') + mine('gui_idle_interrupt')),
        ('wait_for_rlc_serdes', kern('gfx_v8_0_wait_for_rlc_serdes'), mine('wait_for_rlc_serdes')),
        ('select_se_sh', kern('gfx_v8_0_select_se_sh'), mine('select_se_sh')),
        ('cp_gfx_enable', kern('gfx_v8_0_cp_gfx_enable'), mine('cp_gfx_enable')),
        ('ring programming', tokens(cut(resume_k, 'WREG32(mmCP_RB_WPTR_DELAY', 'gfx_v8_0_set_cpg_door_bell(adev, ring);')),
                              tokens(cut(resume_p, 'wr(mmCP_RB_WPTR_DELAY', '// gfx_v8_0_set_cpg_door_bell'))),
        ('doorbell (enable bit)', tokens(door_k)[:2], tokens(cut(resume_p, '// gfx_v8_0_set_cpg_door_bell', '// amdgpu_ring_clear_ring'))),
        ('cp_gfx_start (init, stream)', tokens(cut(start_k, 'WREG32(mmCP_MAX_CONTEXT', 'gfx_v8_0_cp_gfx_enable(adev, true);')) + tokens(start_k[start_k.index('PACKET3_SET_CONTEXT_REG, 2'):start_k.index('PACKET3_PREAMBLE_END_CLEAR_STATE')]),
                              tokens(cut(resume_p, '// gfx_v8_0_cp_gfx_start', 'cp_gfx_enable(true);')) + tokens(body(port, r'uint32_t AmdGfx8::build_clear_state\([^)]*\)\s*\{'))),
        ('ring test', kern('gfx_v8_0_ring_test_ring'), mine('test_ring')),
        ('constants_init', kern('gfx_v8_0_constants_init'), mine('constants_init')),
        ('srbm_select', tokens(body(open(sys.argv[1].replace('gfx_v8_0.c', 'vi.c')).read(), r'void vi_srbm_select\([^)]*\)\s*\{')) if os.path.exists(sys.argv[1].replace('gfx_v8_0.c', 'vi.c')) else mine('srbm_select'), mine('srbm_select')),
        ('setup_rb', kern('gfx_v8_0_setup_rb'), mine('setup_rb')),
        ('rb_active_bitmap', kern('gfx_v8_0_get_rb_active_bitmap'), mine('rb_active_bitmap')),
        ('write_harvested', kern('gfx_v8_0_write_harvested_raster_configs'), mine('write_harvested_raster_configs')),
        ('init_compute_vmid', kern('gfx_v8_0_init_compute_vmid'), [t for t in mine('init_compute_vmid') if not t.startswith('mmGDS_')]),
        ('init_gds_vmid', kern('gfx_v8_0_init_gds_vmid'), [t for t in mine('init_gds_vmid') if not t.startswith('mmGDS_')]),
        ('init_csb', kern('gfx_v8_0_init_csb'), mine('init_csb')),
        ('compute enable', kern('gfx_v8_0_cp_compute_enable'), mine_c('cp_compute_enable')),
        ('kiq setting', kern('gfx_v8_0_kiq_setting'), mine_c('kiq_setting')),
        ('mqd_init', kern('gfx_v8_0_mqd_init'), mine_c('mqd_init')),
        ('mqd_commit', kern('gfx_v8_0_mqd_commit'), mine_c('mqd_commit')),
        ('mec doorbell range', kern('gfx_v8_0_set_mec_doorbell_range'), mine_c('set_mec_doorbell_range')),
        ('compute ring test', kern('gfx_v8_0_ring_test_ring'), mine_c('test_ring')),
    ]
    for name, k, p in pairs:
        print(f'{name:28s} kernel {len(k):3d}  port {len(p):3d}  {"same" if k == p else "DIFFERENT"}')
        if k != p:
            failed = True
            print(f'    kernel {k}\n    port   {p}')
    sys.exit(1 if failed else 0)


main()
