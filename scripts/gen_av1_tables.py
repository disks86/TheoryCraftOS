#!/usr/bin/env python3
"""Extracts the normative constant tables of AV1 from a dav1d source tree (BSD-2-Clause, Copyright VideoLAN and dav1d authors) into
src/Hardware/VideoCodec/Software/Av1Tables.inc, Av1Scan.inc, Av1Qm.inc and Av1Cdf.inc.

Usage: scripts/gen_av1_tables.py <dav1d-checkout> [output-directory]
The tables are not retyped: a small C program is built against the checkout (the tables are compiled by dav1d's own sources, so enumerators and
macros are evaluated by the compiler) and prints them. Needs gcc. Names, dimensions and enumerator values are dav1d's; Av1Data.hpp defines the
types (the CDF context structures keep dav1d's layout, so the default CDFs are dumped as raw 16-bit words).
"""
import os, re, subprocess, sys, tempfile

root = os.path.abspath(sys.argv[1])
out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'src', 'Hardware', 'VideoCodec', 'Software')

CONFIG = """
#define ARCH_X86_32 0
#define ARCH_X86_64 0
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_X86 0
#define HAVE_ASM 0
#define HAVE_UNISTD_H 1
#define HAVE_ALIGNED_MALLOC 0
#define HAVE_MEMALIGN 0
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_PTHREAD_NP_H 0
#define HAVE_C11_GENERIC 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DLSYM 0
#define HAVE_GETAUXVAL 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
#define CONFIG_LOG 0
#define TRIM_DSP_FUNCTIONS 0
#define ENDIANNESS_BIG 0
#define STACK_ALIGNMENT 32
#define CONFIG_MACOS_KPERF 0
#define CONFIG_THREAD_SANITIZER 0
#define CONFIG_ASAN 0
#define CONFIG_MSAN 0
"""

# (file, name of the C expression, element type, dimensions, C++ name). Dimensions are evaluated by the C compiler.
SMALL = [
    ('al_part_ctx', 'uint8_t', '[2][N_BL_LEVELS][N_PARTITIONS]'),
    ('block_sizes', 'uint8_t', '[N_BL_LEVELS][N_PARTITIONS][2]'),
    ('block_dimensions', 'uint8_t', '[N_BS_SIZES][4]'),
    ('max_txfm_size_for_bs', 'uint8_t', '[N_BS_SIZES][4]'),
    ('txtp_from_uvmode', 'uint8_t', '[N_UV_INTRA_PRED_MODES]'),
    ('comp_inter_pred_modes', 'uint8_t', '[N_COMP_INTER_PRED_MODES][2]'),
    ('partition_type_count', 'uint8_t', '[N_BL_LEVELS]'),
    ('tx_types_per_set', 'uint8_t', '[40]'),
    ('filter_mode_to_y_mode', 'uint8_t', '[5]'),
    ('ymode_size_context', 'uint8_t', '[N_BS_SIZES]'),
    ('lo_ctx_offsets', 'uint8_t', '[3][5][5]'),
    ('skip_ctx', 'uint8_t', '[5][5]'),
    ('tx_type_class', 'uint8_t', '[N_TX_TYPES_PLUS_LL]'),
    ('filter_2d', 'uint8_t', '[DAV1D_N_FILTERS][DAV1D_N_FILTERS]'),
    ('filter_dir', 'uint8_t', '[N_2D_FILTERS][2]'),
    ('intra_mode_context', 'uint8_t', '[N_INTRA_PRED_MODES]'),
    ('wedge_ctx_lut', 'uint8_t', '[N_BS_SIZES]'),
    ('cdef_directions', 'int8_t', '[12][2]'),
    ('sgr_params', 'uint16_t', '[16][2]'),
    ('sgr_x_by_x', 'uint8_t', '[256]'),
    ('mc_subpel_filters', 'int8_t', '[6][15][8]'),
    ('mc_warp_filter', 'int8_t', '[193][8]'),
    ('resize_filter', 'int8_t', '[64][8]'),
    ('sm_weights', 'uint8_t', '[128]'),
    ('dr_intra_derivative', 'uint16_t', '[44]'),
    ('filter_intra_taps', 'int8_t', '[5][64]'),
    ('obmc_masks', 'uint8_t', '[64]'),
    ('gaussian_sequence', 'int16_t', '[2048]'),
    ('dq_tbl', 'uint16_t', '[3][QINDEX_RANGE][2]'),
]
SCANS = ['4x4', '4x8', '4x16', '8x4', '8x8', '8x16', '8x32', '16x4', '16x8', '16x16', '16x32', '32x8', '32x16', '32x32']
QMS = SCANS[:]  # the same sizes (qm_tbl_WxH)

prog = ['#include "config.h"', '#include <stdio.h>', '#include <stddef.h>', '#include "src/internal.h"', '#include "src/tables.h"', '#include "src/dequant_tables.h"',
        '#include "src/cdf.c"', '#include "src/ref.h"', 'Dav1dRef *dav1d_ref_create_using_pool(Dav1dMemPool *const pool, size_t size) { return 0; }', 'void dav1d_ref_dec(Dav1dRef **const pref) {}', '#include "src/scan.c"', '#include "src/qm.c"',
        'static void dump_u(const char*type,const char*name,const char*dims,const void*p,size_t count,size_t esz,int sgn){',
        '  printf("static const %s %s%s = {",type,name,dims);',
        '  for(size_t i=0;i<count;i++){ if(i%16==0) printf("\\n "); long long v=0;',
        '    if(esz==1) v= sgn?((const int8_t*)p)[i]:((const uint8_t*)p)[i];',
        '    else if(esz==2) v= sgn?((const int16_t*)p)[i]:((const uint16_t*)p)[i];',
        '    printf(" %lld,",v);}',
        '  printf("\\n};\\n\\n");}',
        'int main(void){']
for name, t, dims in SMALL:
    sg = 1 if t.startswith('int') else 0
    esz = 2 if '16' in t else 1
    rank = dims.count('[')
    a = 'dav1d_' + name
    fmt, args = '', []
    for i in range(rank):
        outer = a + '[0]' * i
        inner = a + '[0]' * (i + 1) if i + 1 < rank else None
        args.append('sizeof(%s)/%s' % (outer, ('sizeof(%s)' % inner) if inner else ('sizeof(%s[0]%s)' % (a, '[0]' * (rank - 1)) if False else 'sizeof(%s)' % (a + '[0]' * rank))))
        fmt += '[%zu]'
    prog.append('  { char d[64]; snprintf(d, sizeof d, "%s", %s); dump_u("%s","k_%s",d,%s,sizeof(%s)/%d,%d,%d); }' % (fmt, ', '.join(args), t, name, a, a, esz, esz, sg))
    prog.append('  printf("// %s: %s\\n");' % (name, dims))
for s in SCANS:
    prog.append('  dump_u("uint16_t","k_scan_%s","[]",scan_%s,sizeof(scan_%s)/2,2,0);' % (s, s, s))
for s in QMS:
    prog.append('  dump_u("uint8_t","k_qm_tbl_%s","[]",qm_tbl_%s,sizeof(qm_tbl_%s),1,0);' % (s, s, s))
prog.append('  dump_u("uint16_t","k_default_cdf","[]",&default_cdf,sizeof(default_cdf)/2,2,0);')
prog.append('  dump_u("uint16_t","k_default_coef_cdf","[]",default_coef_cdf,sizeof(default_coef_cdf)/2,2,0);')
prog.append('  printf("constexpr unsigned kDefaultCdfWords = %zu, kDefaultCoefCdfWords = %zu, kCdfModeBytes = %zu, kCdfCoefBytes = %zu, kCdfMvBytes = %zu, kCdfContextBytes = %zu;\\n", sizeof(default_cdf)/2, sizeof(default_coef_cdf[0])/2, sizeof(CdfModeContext), sizeof(CdfCoefContext), sizeof(CdfMvContext), sizeof(CdfContext));')
prog.append('  printf("constexpr unsigned kDefaultCdfMvJointOffset = %zu, kDefaultCdfMvCompOffset = %zu, kDefaultCdfKfymOffset = %zu;\\n", offsetof(CdfDefaultContext, mv.joint), offsetof(CdfDefaultContext, mv.comp), offsetof(CdfDefaultContext, kfym));')
prog.append('  return 0; }')

with tempfile.TemporaryDirectory() as tmp:
    open(os.path.join(tmp, 'config.h'), 'w').write(CONFIG)
    open(os.path.join(tmp, 'dump.c'), 'w').write('\n'.join(prog) + '\n')
    exe = os.path.join(tmp, 'dump')
    objs = [os.path.join(tmp, 'tables.o'), os.path.join(tmp, 'dequant_tables.o')]
    inc = ['-I' + tmp, '-I' + root, '-I' + os.path.join(root, 'include'), '-I' + os.path.join(root, 'include', 'dav1d')]
    for f in ('tables', 'dequant_tables'):
        subprocess.run(['gcc', '-std=gnu11', '-O0', '-c'] + inc + [os.path.join(root, 'src', f + '.c'), '-o', os.path.join(tmp, f + '.o')], check=True)
    subprocess.run(['gcc', '-std=gnu11', '-O0', '-w'] + inc + [os.path.join(tmp, 'dump.c')] + objs + ['-o', exe], check=True)
    text = subprocess.run([exe], check=True, capture_output=True).stdout.decode()

def split(prefixes):
    # the dump is a sequence of "static const ... = {...};" blocks, each optionally followed by a "// name: dims" comment line
    blocks = re.findall(r'(static const [^\n]*?= \{.*?\n\};\n)', text, flags=re.S)
    return [b for b in blocks if any(('k_' + p) in b.split('=')[0] for p in prefixes)]

header = '// Generated by scripts/gen_av1_tables.py from dav1d (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors). Do not edit.\n\n'
dims = {n: d for n, t, d in SMALL}
def emit(path, blocks, doc=True):
    with open(os.path.join(out_dir, path), 'w') as f:
        f.write(header)
        for b in blocks:
            name = re.search(r'k_(\w+)', b).group(1)
            if doc and name in dims:
                f.write('// ' + name + dims[name] + '\n')
            f.write(b.replace('static const ', 'constexpr ', 1))
small = split([n for n, _, _ in SMALL])
emit('Av1Tables.inc', small)
emit('Av1Scan.inc', split(['scan_' + s for s in SCANS]), False)
emit('Av1Qm.inc', split(['qm_tbl_' + s for s in QMS]), False)
cdf = split(['default_cdf', 'default_coef_cdf'])
with open(os.path.join(out_dir, 'Av1Cdf.inc'), 'w') as f:
    f.write(header)
    for b in cdf:
        f.write(b.replace('static const ', 'constexpr ', 1))
    f.write(re.search(r'constexpr unsigned kDefaultCdfWords[^\n]*\n', text).group(0))
    f.write(re.search(r'constexpr unsigned kDefaultCdfMvJointOffset[^\n]*\n', text).group(0))
