#!/usr/bin/env python3
"""Generates src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnOpcodes.inc: the opcode numbers of the GCN3 (GFX8, Polaris) instructions the shader compiler and the emulator use.

  gen_gcn_opcodes.py [llvm-mc]

The numbers are not typed in: each instruction is assembled by LLVM's AMDGPU assembler (llvm-mc -triple=amdgcn -mcpu=polaris10) and the opcode field of its format is cut out of the
encoding. The same assembler is the oracle of tests/GcnIsaTest.cpp (scripts/gen_gcn_fixtures.py).
"""
import re
import subprocess
import sys

MC = sys.argv[1] if len(sys.argv) > 1 else 'llvm-mc'

# (format, mnemonic, a sample line that assembles to that format)
SOP1 = ['s_mov_b32', 's_not_b32', 's_cmov_b32']
SOP1_64 = ['s_mov_b64', 's_not_b64', 's_and_saveexec_b64', 's_or_saveexec_b64', 's_xor_saveexec_b64', 's_andn2_saveexec_b64', 's_orn2_saveexec_b64', 's_nand_saveexec_b64']
SOP2 = ['s_add_u32', 's_sub_u32', 's_add_i32', 's_sub_i32', 's_addc_u32', 's_subb_u32', 's_min_i32', 's_min_u32', 's_max_i32', 's_max_u32', 's_and_b32', 's_or_b32', 's_xor_b32',
        's_andn2_b32', 's_orn2_b32', 's_nand_b32', 's_nor_b32', 's_lshl_b32', 's_lshr_b32', 's_ashr_i32', 's_mul_i32', 's_cselect_b32']
SOP2_64 = ['s_and_b64', 's_or_b64', 's_xor_b64', 's_andn2_b64', 's_orn2_b64', 's_nand_b64', 's_nor_b64', 's_cselect_b64']
SOPK = ['s_movk_i32', 's_addk_i32', 's_mulk_i32', 's_cmovk_i32']
SOPC = ['s_cmp_eq_i32', 's_cmp_lg_i32', 's_cmp_gt_i32', 's_cmp_ge_i32', 's_cmp_lt_i32', 's_cmp_le_i32', 's_cmp_eq_u32', 's_cmp_lg_u32', 's_cmp_gt_u32', 's_cmp_ge_u32',
        's_cmp_lt_u32', 's_cmp_le_u32']
SOPP_NOARG = ['s_nop', 's_endpgm', 's_barrier']
SOPP_LABEL = ['s_branch', 's_cbranch_scc0', 's_cbranch_scc1', 's_cbranch_vccz', 's_cbranch_vccnz', 's_cbranch_execz', 's_cbranch_execnz']
SMEM = {'s_load_dword': 's0, s[4:5]', 's_load_dwordx2': 's[0:1], s[4:5]', 's_load_dwordx4': 's[0:3], s[4:5]', 's_load_dwordx8': 's[0:7], s[4:5]', 's_load_dwordx16': 's[0:15], s[4:5]',
        's_buffer_load_dword': 's0, s[4:7]', 's_buffer_load_dwordx2': 's[0:1], s[4:7]', 's_buffer_load_dwordx4': 's[0:3], s[4:7]', 's_buffer_load_dwordx8': 's[0:7], s[4:7]',
        's_buffer_load_dwordx16': 's[0:15], s[4:7]'}
VOP1 = ['v_nop', 'v_mov_b32', 'v_cvt_i32_f32', 'v_cvt_u32_f32', 'v_cvt_f32_i32', 'v_cvt_f32_u32', 'v_fract_f32', 'v_trunc_f32', 'v_ceil_f32', 'v_rndne_f32', 'v_floor_f32',
        'v_exp_f32', 'v_log_f32', 'v_rcp_f32', 'v_rsq_f32', 'v_sqrt_f32', 'v_sin_f32', 'v_cos_f32', 'v_not_b32', 'v_readfirstlane_b32', 'v_rcp_iflag_f32']
VOP2 = ['v_cndmask_b32', 'v_add_f32', 'v_sub_f32', 'v_subrev_f32', 'v_mul_f32', 'v_min_f32', 'v_max_f32', 'v_min_i32', 'v_max_i32', 'v_min_u32', 'v_max_u32', 'v_lshrrev_b32',
        'v_ashrrev_i32', 'v_lshlrev_b32', 'v_and_b32', 'v_or_b32', 'v_xor_b32', 'v_mac_f32', 'v_add_u32', 'v_sub_u32', 'v_subrev_u32', 'v_mul_i32_i24', 'v_mul_u32_u24']
VOP2_LIT = ['v_madmk_f32', 'v_madak_f32']
CMP_F = ['f', 'lt', 'eq', 'le', 'gt', 'lg', 'ge', 'o', 'u', 'nge', 'nlg', 'ngt', 'nle', 'neq', 'nlt', 'tru']
CMP_I = ['f', 'lt', 'eq', 'le', 'gt', 'ne', 'ge', 't']
VOP3 = ['v_mad_f32', 'v_fma_f32', 'v_mad_i32_i24', 'v_mad_u32_u24', 'v_min3_f32', 'v_max3_f32', 'v_med3_f32', 'v_bfe_u32', 'v_bfe_i32', 'v_bfi_b32', 'v_mad_u64_u32']
VINTRP = ['v_interp_p1_f32', 'v_interp_p2_f32', 'v_interp_mov_f32']
MUBUF = ['buffer_load_format_x', 'buffer_load_format_xy', 'buffer_load_format_xyz', 'buffer_load_format_xyzw', 'buffer_load_dword', 'buffer_load_dwordx2', 'buffer_load_dwordx3',
         'buffer_load_dwordx4', 'buffer_store_dword', 'buffer_store_dwordx2', 'buffer_store_dwordx4', 'buffer_store_format_xyzw']
MIMG = ['image_load', 'image_sample', 'image_sample_l', 'image_sample_b', 'image_sample_lz', 'image_sample_d', 'image_sample_c', 'image_get_resinfo']

lines = []   # (name, format, asm)
def add(name, fmt, asm): lines.append((name, fmt, asm))
for m in SOP1: add(m, 'SOP1', '%s s0, s1' % m)
for m in SOP1_64: add(m, 'SOP1', '%s s[0:1], s[2:3]' % m)
for m in SOP2: add(m, 'SOP2', '%s s0, s1, s2' % m)
for m in SOP2_64: add(m, 'SOP2', '%s s[0:1], s[2:3], s[4:5]' % m)
for m in SOPK: add(m, 'SOPK', '%s s0, 0x1234' % m)
for m in SOPC: add(m, 'SOPC', '%s s0, s1' % m)
for m in SOPP_NOARG: add(m, 'SOPP', 's_nop 0' if m == 's_nop' else m)
for m in SOPP_LABEL: add(m, 'SOPP', '%s 1' % m)
add('s_waitcnt', 'SOPP', 's_waitcnt vmcnt(0)')
for m, a in SMEM.items(): add(m, 'SMEM', '%s %s, 0x10' % (m, a))
for m in VOP1: add(m, 'VOP1', (m if m == 'v_nop' else ('%s s0, v1' % m if m == 'v_readfirstlane_b32' else '%s v0, v1' % m)))
for m in VOP2: add(m, 'VOP2', ('%s v0, vcc, v1, v2' % m) if m in ('v_add_u32', 'v_sub_u32', 'v_subrev_u32') else '%s v0, v1, v2' % m + (', vcc' if m == 'v_cndmask_b32' else ''))
for m in VOP2_LIT: add(m, 'VOP2', '%s v0, v1, 0x3f800000, v2' % m if m == 'v_madmk_f32' else '%s v0, v1, v2, 0x3f800000' % m)
for c in CMP_F: add('v_cmp_%s_f32' % c, 'VOPC', 'v_cmp_%s_f32 vcc, v1, v2' % c)
add('v_cmp_class_f32', 'VOPC', 'v_cmp_class_f32 vcc, v1, v2')
for c in CMP_I:
    add('v_cmp_%s_i32' % c, 'VOPC', 'v_cmp_%s_i32 vcc, v1, v2' % c)
    add('v_cmp_%s_u32' % c, 'VOPC', 'v_cmp_%s_u32 vcc, v1, v2' % c)
for m in VOP3: add(m, 'VOP3', '%s v0, v1, v2, v3' % m if m != 'v_mad_u64_u32' else 'v_mad_u64_u32 v[0:1], s[2:3], v1, v2, v[4:5]')
for m in VOP3[:0]: pass
add('v_cvt_pkrtz_f16_f32', 'VOP3', 'v_cvt_pkrtz_f16_f32 v0, v1, v2')
add('v_mul_lo_u32', 'VOP3', 'v_mul_lo_u32 v0, v1, v2')
add('v_mul_hi_u32', 'VOP3', 'v_mul_hi_u32 v0, v1, v2')
for m in VINTRP: add(m, 'VINTRP', '%s v0, v1, attr0.x' % m if m != 'v_interp_mov_f32' else 'v_interp_mov_f32 v0, p10, attr0.x')
for m in MUBUF:
    n = {'x': 1, 'xy': 2, 'xyz': 3, 'xyzw': 4}.get(m.split('_')[-1], None)
    if n is None: n = {'dword': 1, 'dwordx2': 2, 'dwordx3': 3, 'dwordx4': 4}[m.split('_')[-1]]
    add(m, 'MUBUF', '%s v[0:%d], v4, s[8:11], 0 idxen' % (m, n - 1) if n > 1 else '%s v0, v4, s[8:11], 0 idxen' % m)
for m in MIMG: add(m, 'MIMG', ('%s v[0:3], v[4:5], s[8:15] dmask:0xf' % m) if m == 'image_load' else ('image_get_resinfo v[0:3], v4, s[8:15] dmask:0xf' if m == 'image_get_resinfo' else '%s v[0:3], v[4:5], s[8:15], s[12:15] dmask:0xf' % m))

src = '\n'.join(a for _, _, a in lines) + '\n'
out = subprocess.run([MC, '-triple=amdgcn', '-mcpu=polaris10', '-show-encoding'], input=src, capture_output=True, text=True)
if out.returncode != 0:
    sys.exit(out.stderr)
encs = []
for l in out.stdout.splitlines():
    m = re.search(r'encoding: \[([^\]]*)\]', l)
    if m: encs.append(bytes(int(x, 16) for x in m.group(1).split(',')))
if len(encs) != len(lines):
    sys.exit('got %d encodings for %d lines' % (len(encs), len(lines)))

def w(b, i=0): return int.from_bytes(b[i:i + 4], 'little')
fields = {
    'SOP1': lambda x: (x >> 8) & 0xFF, 'SOP2': lambda x: (x >> 23) & 0x7F, 'SOPK': lambda x: (x >> 23) & 0x1F, 'SOPC': lambda x: (x >> 16) & 0x7F, 'SOPP': lambda x: (x >> 16) & 0x7F,
    'SMEM': lambda x: (x >> 18) & 0xFF, 'VOP1': lambda x: (x >> 9) & 0xFF, 'VOP2': lambda x: (x >> 25) & 0x3F, 'VOPC': lambda x: (x >> 17) & 0xFF, 'VOP3': lambda x: (x >> 16) & 0x3FF,
    'VINTRP': lambda x: (x >> 16) & 0x3, 'MUBUF': lambda x: (x >> 18) & 0x7F, 'MIMG': lambda x: (x >> 18) & 0x7F,
}
tops = {  # the bits that name the format in word 0: (mask, value)
    'SOP1': (0xFF800000, 0xBE800000), 'SOP2': (0xC0000000, 0x80000000), 'SOPK': (0xF0000000, 0xB0000000), 'SOPC': (0xFF800000, 0xBF000000), 'SOPP': (0xFF800000, 0xBF800000),
    'SMEM': (0xFC000000, 0xC0000000), 'VOP1': (0xFE000000, 0x7E000000), 'VOP2': (0x80000000, 0x00000000), 'VOPC': (0xFE000000, 0x7C000000), 'VOP3': (0xFC000000, 0xD0000000),
    'VINTRP': (0xFC000000, 0xD4000000), 'MUBUF': (0xFC000000, 0xE0000000), 'MIMG': (0xFC000000, 0xF0000000),
}
groups = {}
for (name, fmt, asm), e in zip(lines, encs):
    x = w(e)
    mask, val = tops[fmt]
    if (x & mask) != val and not (fmt == 'VOPC' and (x & 0xFC000000) == 0xD0000000):
        sys.exit('%s assembled to %08x, not %s' % (name, x, fmt))
    groups.setdefault(fmt, []).append((name, fields[fmt](x)))
with open('src/Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnOpcodes.inc', 'w') as f:
    f.write('// Generated by scripts/gen_gcn_opcodes.py from the encodings of llvm-mc (-triple=amdgcn -mcpu=polaris10). Do not edit.\n')
    for fmt in ['SOP1', 'SOP2', 'SOPK', 'SOPC', 'SOPP', 'SMEM', 'VOP1', 'VOP2', 'VOPC', 'VOP3', 'VINTRP', 'MUBUF', 'MIMG']:
        f.write('enum class %s : uint16_t {\n' % fmt.capitalize() + ''.join('  %s = 0x%X,\n' % (n, v) for n, v in groups.get(fmt, [])) + '};\n')
print('gen_gcn_opcodes: %d instructions' % len(lines))
