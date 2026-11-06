#!/usr/bin/env python3
"""Generates src/Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeLpgHevc.inc: the VDEnc commands of HEVC encode on Xe-LPG (Meteor Lake, Arrow Lake).

Instead of transcribing the Intel media driver's (MIT licence) parameter logic by hand, this compiles the driver's own code: the constant-table lambdas that fill VDENC_CMD1 and
VDENC_CMD2 (encode_hevc_vdenc_const_settings*.cpp), the DO_FIELD maps that put the parameters into the commands (mhw_vdbox_vdenc_impl*.h) and the hardware command structures
(mhw_vdbox_vdenc_hwcmd_xe_lpm_plus.h) into a small program, runs them for the one configuration the encoder here uses (constant QP, target usage 7, no tiles, no weighted
prediction, one reference, low-delay B) and writes the resulting command images. CMD1 and CMD2 depend on the slice quantiser in one dword each (checked here), so they are
written for the middle quantiser plus that dword for every quantiser.

usage: gen_intel_hevc_xelpg.py <media-driver-dir> <out.inc> [xelpg | xehpg]

The platform (default xelpg) picks the files and names below: xehpg is Arc Alchemist (Xe_HPM; its HEVC const settings are Xe_XPM_base's) and writes kXeHpg... names.

<media-driver-dir> holds copies of these files from github.com/intel/media-driver (paths in the repository in brackets):
  cmdpar.h            [media_softlet/agnostic/common/hw/vdbox/mhw_vdbox_vdenc_cmdpar.h]
  impl_common.h       [media_softlet/agnostic/common/hw/vdbox/mhw_vdbox_vdenc_impl.h]
  impl_lpmplus_base.h [media_softlet/agnostic/Xe_M_plus/Xe_LPM_plus_base/hw/vdbox/mhw_vdbox_vdenc_impl_xe_lpm_plus_base.h]
  cs_common.cpp       [media_softlet/agnostic/common/codec/hal/enc/hevc/features/encode_hevc_vdenc_const_settings.cpp]
  cs_lpm.cpp          [media_softlet/agnostic/Xe_M_plus/Xe_LPM_plus_base/codec/hal/enc/hevc/features/encode_hevc_vdenc_const_settings_xe_lpm_plus_base.cpp]
  hwcmd.h             [media_softlet/agnostic/Xe_M_plus/Xe_LPM_plus/hw/vdbox/mhw_vdbox_vdenc_hwcmd_xe_lpm_plus.h; the Xe_LPM_plus_base copy]
  For xehpg instead of cs_lpm.cpp / impl_lpmplus_base.h / hwcmd.h: cs_xpm.cpp [media_driver/media_softlet/agnostic/Xe_M/Xe_XPM_base/codec/hal/enc/hevc/features/
  encode_hevc_vdenc_const_settings_xe_xpm_base.cpp] and hwcmd_hpm.h [... Xe_HPM/.../mhw_vdbox_vdenc_hwcmd_xe_hpm.h]; the Xe_HPM implementation adds nothing to the common one.
Needs g++.
"""
import os, re, subprocess, sys, tempfile

QPS = range(10, 52)

PLATFORMS = {
    'xelpg': dict(ns='xe_lpm_plus_base::v0', hwcmd='hwcmd.h', cs='cs_lpm.cpp', cs_class='EncodeHevcVdencConstSettingsXe_Lpm_Plus_Base', pms_impl='impl_lpmplus_base.h', prefix='XeLpg',
                  hwcmd_include='mhw_vdbox_vdenc_hwcmd_xe_lpm_plus.h'),
    'xehpg': dict(ns='xe_hpm', hwcmd='hwcmd_hpm.h', cs='cs_xpm.cpp', cs_class='EncodeHevcVdencConstSettingsXe_Xpm_Base', pms_impl=None, prefix='XeHpg',
                  hwcmd_include='mhw_vdbox_vdenc_hwcmd_xe_hpm.h'),
}


def brace_block(s, start):
    i = s.index('{', start)
    depth, j = 0, i
    while True:
        if s[j] == '{': depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0: return s[i + 1:j]
        j += 1


def function_body(path, signature):
    s = open(path).read()
    return brace_block(s, s.index(signature))


def par_struct(cmdpar, name, cpp_name):
    s = open(cmdpar).read()
    t = brace_block(s, s.index('struct _MHW_PAR_T(%s)' % name))
    t = re.sub(r'__MHW_VDBOX_VDENC_WRAPPER_EXT\([^;]*?\);', '', t, flags=re.S)
    t = re.sub(r'__MHW_VDBOX_VDENC_WRAPPER\(\s*(.*?)\);', r'\1;', t, flags=re.S)
    return 'struct %s {%s};\n' % (cpp_name, t)


def fields_macro(path, command, macro):
    s = open(path).read()
    i = s.index('_MHW_SETCMD_OVERRIDE_DECL(%s)' % command)
    j = s.index('#define DO_FIELDS()', i)
    k = s.index('#include "mhw_hwcmd_process_cmdfields.h"', j)
    t = s[j:k]
    t = t[:t.index('#define DO_FIELDS_EXT')] if '#define DO_FIELDS_EXT' in t else t
    t = t.replace('#define DO_FIELDS()', '#define %s()' % macro).replace('DO_FIELD(', 'DOF(').rstrip().rstrip('\\')
    return t + '\n'


def struct_text(header, name, indent):
    m = re.search(r'^%sstruct %s\s*\n%s\{' % (indent, re.escape(name), indent), header, re.M)
    return brace_block(header, m.start())


def parse_fields(text):
    fields, pend = {}, []
    for line in text.split('\n'):
        f = re.search(r'uint32_t\s+(\w+)\s*:\s*__CODEGEN_BITFIELD\(\s*(\d+),\s*(\d+)\)', line)
        if f: pend.append((f.group(1), int(f.group(2)), int(f.group(3)))); continue
        m = re.match(r'^\s*\}\s*DW(\d+)\s*;', line)
        if m:
            for n, lo, hi in pend:
                if not n.startswith('Reserved'): fields[n] = (int(m.group(1)), lo, hi)
            pend = []
        elif re.match(r'^\s*\}\s*\w+', line): pend = []
    return fields


HARNESS = r'''
#include <vector>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <array>
#include "HWCMD_INCLUDE"
using namespace mhw::vdbox::vdenc::NAMESPACE;
typedef int MOS_STATUS;
#define MOS_STATUS_SUCCESS 0
#define ENCODE_FUNC_CALL()
#define ENCODE_CHK_NULL_RETURN(x)
#define ENCODE_CHK_STATUS_RETURN(x) (x)
#define MOS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLIP3(lo, hi, v) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define MOS_ALIGN_CEIL(v, a) (((v) + (a) - 1) / (a) * (a))
#define MEDIA_IS_WA(t, w) (0)   // (no workaround: current production steppings)
struct WaTable {};
struct OsItf { WaTable *pfnGetWaTable(OsItf *) { static WaTable t; return &t; } };
static int Clip3(int x, int y, int z) { return z < x ? x : (z > y ? y : z); }
enum { I_TYPE = 1, P_TYPE = 2, B_TYPE = 3 };
PAR_STRUCTS
struct Settings {
  std::vector<std::function<int(CMD1Par &, bool)>> vdencCmd1Settings;
  std::vector<std::function<int(CMD2Par &, bool)>> vdencCmd2Settings;
};
#define VDENC_CMD1_LAMBDA() [this](CMD1Par &par, bool isLowDelay) -> int
#define VDENC_CMD2_LAMBDA() [this](CMD2Par &par, bool isLowDelay) -> int
#define HevcVdencFeatureSettings Settings
struct Pic { int CodingType, HierarchLevelPlus1, QpY, pps_curr_pic_ref_enabled_flag, NumROI;
  struct { struct { int EnableCustomRoudingIntra, EnableCustomRoudingInter, RoundingOffsetIntra, RoundingOffsetInter; } fields; } CustomRoundingOffsetsParams; };
struct Seq { int TargetUsage, HierarchicalFlag, palette_mode_enabled_flag, LowDelayMode, GopRefDist, bit_depth_luma_minus8; };
struct Slice { int slice_qp_delta, num_ref_idx_l0_active_minus1; };
struct Ctx {
  Pic *m_hevcPicParams; Seq *m_hevcSeqParams; Slice *m_hevcSliceParams;
  bool m_hevcVdencRoundingPrecisionEnabled = true;   // (the driver's default for "HEVC VDEnc Rounding Enable")
  bool m_hevcRdoqEnabled = false;                    // (target usage 7)
  void *m_featureSetting;
  OsItf *m_osItf = &m_osItfStorage;
  OsItf m_osItfStorage;
  BODIES
};
#define DOF(dw, field, value) cmd.dw.field = (value)
FIELD_MACROS
static void dump(const char *tag, const uint32_t *d, size_t n) { printf("%s", tag); for (size_t i = 0; i < n; i++) printf(" 0x%x", d[i]); printf("\n"); }
int main(int argc, char **argv) {
  const int type = atoi(argv[1]), qp = atoi(argv[2]);
  Pic pic{}; pic.CodingType = type; pic.QpY = qp;
  Seq seq{}; seq.TargetUsage = 7; seq.LowDelayMode = 1; seq.GopRefDist = 1;
  Slice sl{};
  Ctx ctx; ctx.m_hevcPicParams = &pic; ctx.m_hevcSeqParams = &seq; ctx.m_hevcSliceParams = &sl;
  Settings S; ctx.m_featureSetting = &S;
  ctx.SetCmd1Common(); ctx.SetCmd1Lpm(); ctx.SetCmd2Lpm();
  const bool lowDelay = true;
  {
    CMD1Par par; for (auto &f : S.vdencCmd1Settings) f(par, lowDelay);
    _VDENC_CMD1_CMD cmd; auto &params = par;
    DO_FIELDS1();
    dump("CMD1", (uint32_t *)&cmd, 33);
  }
  {
    // The picture-level parameters of HevcBasicFeature's VDENC_CMD2 SETPAR for a low-delay B (or I) picture whose one reference is the previous picture; the values that depend
    // on the picture (size, picture order counts) are set by the encoder afterwards.
    CMD2Par par;
    par.width = 64; par.height = 64;
    par.pictureType = type == I_TYPE ? 0 : 3;
    if (type != I_TYPE) { par.numRefL0 = 1; par.numRefL1 = 1; }
    if (type == I_TYPE) par.pocL0Ref0 = par.pocL1Ref0 = par.pocL0Ref1 = par.pocL1Ref1 = par.pocL0Ref2 = par.pocL1Ref2 = par.pocL0Ref3 = par.pocL1Ref3 = 0;
    else { par.pocL0Ref0 = par.pocL1Ref0 = 1; par.pocL0Ref1 = par.pocL1Ref1 = 2; par.pocL0Ref2 = par.pocL1Ref2 = 3; }   // (entries 3 keep the parameter defaults 4 and -4)
    par.intraRefreshMbSizeMinus1 = 0;
    par.qpPrimeYAc = qp;
    par.frameIdxL0Ref0 = type == I_TYPE ? 7 : 0; par.frameIdxL0Ref1 = par.frameIdxL0Ref2 = par.frameIdxL1Ref0 = 7;
    par.minQp = 10; par.maxQp = 51; par.subPelMode = 3;
    for (auto &f : S.vdencCmd2Settings) f(par, lowDelay);
    _VDENC_CMD2_CMD cmd; auto &params = par;
    DO_FIELDS2();
    for (auto &f : par.extSettings) f((uint32_t *)&cmd);
    dump("CMD2", (uint32_t *)&cmd, 62);
  }
  if (type == B_TYPE && qp == 26) {
    {
      VDENC_PIPE_MODE_SELECT_PAR par;
      par.bitDepthMinus8 = 0; par.chromaType = 1; par.randomAccess = false; par.VdencPipeModeSelectPar0 = 1;
      Cmd::VDENC_PIPE_MODE_SELECT_CMD cmd; auto &params = par;
      DO_FIELDS_PMS();
      PMS_OVERRIDE
      dump("PMS", (uint32_t *)&cmd, 6);
    }
    {
      WEIGHTS_PAR par; par.denomLuma = 1;
      Cmd::VDENC_WEIGHTSOFFSETS_STATE_CMD cmd; auto &params = par;
      DO_FIELDS_WO();
      dump("WO", (uint32_t *)&cmd, 7);
    }
    {
      TILE_PAR par; par.tileWidth = 64; par.tileHeight = 64; par.ctbSize = 64; par.numPipe = 0;
      Cmd::VDENC_HEVC_VP9_TILE_SLICE_STATE_CMD cmd; auto &params = par;
      DO_FIELDS_TSS();
      dump("TSS", (uint32_t *)&cmd, 28);
    }
    {
      WALKER_PAR par; par.firstSuperSlice = false;
      Cmd::VDENC_WALKER_STATE_CMD cmd; auto &params = par;
      DO_FIELDS_WALK();
      dump("WALK", (uint32_t *)&cmd, 3);
    }
    { Cmd::VDENC_PIPE_BUF_ADDR_STATE_CMD cmd; dump("PBA", (uint32_t *)&cmd, 89); }
  }
}
'''


def build_harness(d, tmp, plat):
    bodies = '\n'.join([
        'int SetCmd1Common() {%s}' % function_body(d + '/cs_common.cpp', 'MOS_STATUS EncodeHevcVdencConstSettings::SetVdencCmd1Settings()'),
        'int SetCmd1Lpm() {%s}' % function_body(d + '/' + plat['cs'], 'MOS_STATUS %s::SetVdencCmd1Settings()' % plat['cs_class']).replace(
            'EncodeHevcVdencConstSettings::SetVdencCmd1Settings();', 'SetCmd1Common();'),
        'int SetCmd2Lpm() {%s}' % function_body(d + '/' + plat['cs'], 'MOS_STATUS %s::SetVdencCmd2Settings()' % plat['cs_class']),
    ])
    pars = ''.join([par_struct(d + '/cmdpar.h', 'VDENC_CMD1', 'CMD1Par'), par_struct(d + '/cmdpar.h', 'VDENC_CMD2', 'CMD2Par'),
                    par_struct(d + '/cmdpar.h', 'VDENC_PIPE_MODE_SELECT', 'VDENC_PIPE_MODE_SELECT_PAR'),
                    par_struct(d + '/cmdpar.h', 'VDENC_WEIGHTSOFFSETS_STATE', 'WEIGHTS_PAR'),
                    par_struct(d + '/cmdpar.h', 'VDENC_HEVC_VP9_TILE_SLICE_STATE', 'TILE_PAR'),
                    par_struct(d + '/cmdpar.h', 'VDENC_WALKER_STATE', 'WALKER_PAR')])
    pars = pars.replace('MAX_REF_LIST_NUM', '2').replace('MAX_REF_NUM', '3')
    macros = ''.join([
        fields_macro(d + '/impl_common.h', 'VDENC_CMD1', 'DO_FIELDS1'), fields_macro(d + '/impl_common.h', 'VDENC_CMD2', 'DO_FIELDS2'),
        fields_macro(d + '/impl_common.h', 'VDENC_PIPE_MODE_SELECT', 'DO_FIELDS_PMS'),
        fields_macro(d + '/' + plat['pms_impl'], 'VDENC_PIPE_MODE_SELECT', 'DO_FIELDS_PMS_LPM') if plat['pms_impl'] else '',
        fields_macro(d + '/impl_common.h', 'VDENC_WEIGHTSOFFSETS_STATE', 'DO_FIELDS_WO'), fields_macro(d + '/impl_common.h', 'VDENC_HEVC_VP9_TILE_SLICE_STATE', 'DO_FIELDS_TSS'),
        fields_macro(d + '/impl_common.h', 'VDENC_WALKER_STATE', 'DO_FIELDS_WALK')])
    src = HARNESS.replace('HWCMD_INCLUDE', plat['hwcmd_include']).replace('NAMESPACE', plat['ns']).replace('PMS_OVERRIDE', 'DO_FIELDS_PMS_LPM();' if plat['pms_impl'] else '').replace('PAR_STRUCTS', pars).replace('BODIES', bodies).replace('FIELD_MACROS', macros)
    os.makedirs(tmp + '/stub', exist_ok=True)
    open(tmp + '/stub/mhw_hwcmd.h', 'w').write('''#pragma once
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#define MOS_ZeroMemory(p, s) memset(p, 0, s)
#define __CODEGEN_MAX(_a, _b) (((_a) > (_b)) ? (_a) : (_b))
#define __CODEGEN_BITFIELD(l, h) (h) - (l) + 1
#define __CODEGEN_OP_LENGTH_BIAS 2
#define __CODEGEN_OP_LENGTH(x) (uint32_t)((__CODEGEN_MAX(x, __CODEGEN_OP_LENGTH_BIAS)) - __CODEGEN_OP_LENGTH_BIAS)
''')
    open(tmp + '/stub/mos_utilities.h', 'w').write('#include "mhw_hwcmd.h"\n')   # (the Xe_HPM header includes this one)
    open(tmp + '/main.cpp', 'w').write(src)
    subprocess.check_call(['g++', '-std=c++17', '-w', '-I' + tmp + '/stub', '-I' + tmp + '/hdr', tmp + '/main.cpp', '-o', tmp + '/harness'])
    return tmp + '/harness'


def run(harness, t, qp):
    out = {}
    for line in subprocess.check_output([harness, str(t), str(qp)], text=True).split('\n'):
        if line:
            k, *v = line.split()
            out[k] = [int(x, 16) for x in v]
    return out


def emit_array(name, words):
    return 'constexpr uint32_t %s[%d] = {%s};' % (name, len(words), ', '.join('0x%08X' % w for w in words))


def main():
    d, out_path = sys.argv[1], sys.argv[2]
    plat = PLATFORMS[sys.argv[3] if len(sys.argv) > 3 else 'xelpg']
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(tmp + '/hdr')
        open(tmp + '/hdr/' + plat['hwcmd_include'], 'w').write(open(d + '/' + plat['hwcmd']).read())
        harness = build_harness(d, tmp, plat)
        runs = {(t, qp): run(harness, t, qp) for t in (1, 3) for qp in QPS}
    header = open(d + '/' + plat['hwcmd']).read()
    P = plat['prefix']
    o = ['// Generated by tools/gen_intel_hevc_xelpg.py from the Intel media driver (MIT licence): the VDEnc commands of HEVC encode on Xe-LPG (constant QP, target usage 7, no tiles,',
         '// low-delay B). Do not edit by hand.']
    for tag, name, struct, indent in [('PMS', 'VdencPipeModeSelect', 'VDENC_PIPE_MODE_SELECT_CMD', '    '), ('WO', 'VdencWeightsOffsetsState', 'VDENC_WEIGHTSOFFSETS_STATE_CMD', '    '),
                                      ('TSS', 'VdencTileSliceState', 'VDENC_HEVC_VP9_TILE_SLICE_STATE_CMD', '    '), ('WALK', 'VdencWalkerState', 'VDENC_WALKER_STATE_CMD', '    '),
                                      ('PBA', 'VdencPipeBufAddrState', 'VDENC_PIPE_BUF_ADDR_STATE_CMD', '    ')]:
        img = runs[(3, 26)][tag]
        o += ['', '// %s, %d dwords' % (struct, len(img)), 'constexpr uint32_t k' + P + '%sDwords = %d;' % (name, len(img)), emit_array('k' + P + '%sImage' % name, img)]
        if tag in ('TSS', 'WALK'):
            for n, (dw, lo, hi) in sorted(parse_fields(struct_text(header, struct, indent)).items(), key=lambda kv: kv[1]):
                o.append('constexpr EncodeField k' + P + '%s_%s = {%d, %d, %d};' % (name, n, dw, lo, hi))
    for tag, name, struct, dwords in [('CMD1', 'VdencCmd1', '_VDENC_CMD1_CMD', 33), ('CMD2', 'VdencCmd2', '_VDENC_CMD2_CMD', 62)]:
        varying = sorted({i for t in (1, 3) for qp in QPS for i in range(dwords) if runs[(t, qp)][tag][i] != runs[(t, 26)][tag][i]})
        print('%s varies with the quantiser in dwords %s' % (tag, varying), file=sys.stderr)
        assert len(varying) == 1, 'unexpected quantiser dependence'
        v = varying[0]
        o += ['', '// %s, %d dwords: image of an I picture and of a low-delay B picture at quantiser 26, and dword %d for the quantisers %d .. %d' % (struct, dwords, v, QPS[0], QPS[-1]),
              'constexpr uint32_t k' + P + '%sDwords = %d;' % (name, dwords), 'constexpr uint32_t k' + P + '%sQpDword = %d;' % (name, v),
              emit_array('k' + P + '%sImageI' % name, runs[(1, 26)][tag]), emit_array('k' + P + '%sImageB' % name, runs[(3, 26)][tag]),
              emit_array('k' + P + '%sQpTableI' % name, [runs[(1, qp)][tag][v] for qp in QPS]), emit_array('k' + P + '%sQpTableB' % name, [runs[(3, qp)][tag][v] for qp in QPS])]
        if tag == 'CMD2':
            for n, (dw, lo, hi) in sorted(parse_fields(struct_text(header, struct, '')).items(), key=lambda kv: kv[1]):
                o.append('constexpr EncodeField k' + P + '%s_%s = {%d, %d, %d};' % (name, n, dw, lo, hi))
    open(out_path, 'w').write('\n'.join(o) + '\n')


if __name__ == '__main__':
    main()
