#!/usr/bin/env python3
"""Generates src/Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeLpgAvc.inc: the VDEnc commands of H.264 encode on Xe-LPG (Meteor Lake, Arrow Lake).

Same method as gen_intel_hevc_xelpg.py: the Intel media driver's (MIT licence) own lambdas, tables, field maps and command structures are compiled into a small program and run for
the configuration the encoder here uses (constant QP, target usage 7, one slice, frame pictures, no 8x8 transform, I and P pictures, reference pictures), and the images are written.

usage: gen_intel_avc_xelpg.py <media-driver-dir> <out.inc> [xelpg | xe2lpm | xe2hpm]

xe2lpm (Lunar Lake) and xe2hpm (Arc Battlemage) read cs_avc_xe2_lpm.cpp / cs_avc_xe2_hpm.cpp and the matching *_open.h tables instead of cs_avc_lpm.cpp, and hwcmd_xe2_lpm.h / hwcmd_xe2_hpm.h, impl_xe2_hpm.h instead
of hwcmd.h / impl_lpmplus_base.h; they write kXe2Lpm... / kXe2Hpm... names.

<media-driver-dir> holds, besides the files gen_intel_hevc_xelpg.py reads (cmdpar.h, impl_common.h, impl_lpmplus_base.h, hwcmd.h):
  cs_avc.cpp      [media_softlet/agnostic/common/codec/hal/enc/avc/features/encode_avc_vdenc_const_settings.cpp]
  cs_avc.h        [... /encode_avc_vdenc_const_settings.h]
  cs_avc_lpm.cpp  [media_softlet/agnostic/Xe_M_plus/Xe_LPM_plus_base/codec/hal/enc/avc/features/encode_avc_vdenc_const_settings_xe_lpm_plus_base.cpp]
"""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_intel_hevc_xelpg import brace_block, function_body, par_struct, fields_macro, struct_text, parse_fields, emit_array

QPS = range(10, 52)
LEVELS = range(0, 53)

PLATFORMS = {
    'xelpg': dict(ns='xe_lpm_plus_base::v0', hwcmd='hwcmd.h', hwcmd_include='mhw_vdbox_vdenc_hwcmd_xe_lpm_plus.h', cs='cs_avc_lpm.cpp', cs_class='EncodeAvcVdencConstSettingsXe_Lpm_Plus_Base', pms=['impl_lpmplus_base.h'], prefix='XeLpg'),
    'xe2lpm': dict(ns='xe2_lpm_base::xe2_lpm', hwcmd='hwcmd_xe2_lpm.h', hwcmd_include='mhw_vdbox_vdenc_hwcmd_xe2_lpm.h', cs='cs_avc_xe2_lpm.cpp', cs_class='EncodeAvcVdencConstSettingsXe2_Lpm', pms=[], prefix='Xe2Lpm'),
    'xe2hpm': dict(ns='xe_lpm_plus_base::v1', hwcmd='hwcmd_xe2_hpm.h', hwcmd_include='mhw_vdbox_vdenc_hwcmd_xe2_hpm.h', cs='cs_avc_xe2_hpm.cpp', cs_class='EncodeAvcVdencConstSettingsXe2_Hpm', pms=['impl_lpmplus_base.h', 'impl_xe2_hpm.h'], prefix='Xe2Hpm'),
}

HARNESS = r'''
#include <vector>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <array>
#include "HWCMD_INCLUDE"
using namespace mhw::vdbox::vdenc::NAMESPACE;
typedef int MOS_STATUS;
#define MOS_STATUS_SUCCESS 0
#define ENCODE_FUNC_CALL()
#define ENCODE_CHK_NULL_RETURN(x)
#define ENCODE_CHK_STATUS_RETURN(x) (x)
#define CodecHal_Clip3(lo, hi, v) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define CODECHAL_GET_WIDTH_IN_MACROBLOCKS(w) (((w) + 15) / 16)
#define CODECHAL_GET_HEIGHT_IN_MACROBLOCKS(h) (((h) + 15) / 16)
#define MEDIA_IS_WA(t, w) (0)
static int Clip3(int x, int y, int z) { return z < x ? x : (z > y ? y : z); }
enum { I_TYPE = 1, P_TYPE = 2, B_TYPE = 3 };
namespace encode {
struct RdModeConstSettings {
    static constexpr uint32_t INTRA_MPM = 0, INTRA_16X16 = 1, INTRA_8X8 = 2, INTRA_4X4 = 3, INTER_16X8 = 4, INTER_8X8 = 5, INTER_8X4 = 6, INTER_16X16 = 7, INTER_BWD = 8, REF_ID = 9,
                                  INTRA_CHROMA = 10, SKIP_16X16 = 11, DIRECT_16X16 = 12;
};
STRUCT_DECL
TABLES
}
using namespace encode;
PAR_STRUCTS
struct Settings {
  std::vector<std::function<int(CMD3Par &)>> vdencCmd3Settings;
  std::vector<std::function<int(IMGPar &)>> vdencAvcImgStateSettings;
};
#define VDENC_CMD3_LAMBDA() [this](CMD3Par &par) -> int
#define VDENC_AVC_IMG_STATE_LAMBDA() [this](IMGPar &par) -> int
#define AvcVdencFeatureSettings Settings
struct WaTable {};
struct OsItf { WaTable *pfnGetWaTable(OsItf *) { static WaTable t; return &t; } };
struct Pic { int CodingType, pic_init_qp_minus26, RefPicFlag, EnableRollingIntraRefresh; };
struct Seq { int GopRefDist, GopPicSize, FrameWidth, FrameHeight, TargetUsage, Level, LowDelayMode, EnableSliceLevelRateCtrl; };
struct Slice { int slice_qp_delta, num_ref_idx_l0_active_minus1; };
struct Ctx {
  Pic *m_avcPicParams; Seq *m_avcSeqParams; Slice *m_avcSliceParams;
  AvcVdencCMD3ConstSettings m_CMD3Settings;
  uint8_t m_qp = 0;
  OsItf *m_osItf; OsItf m_osItfStorage;
  void *m_featureSetting;
  BODIES
};
#define DOF(dw, field, value) cmd.dw.field = (value)
CMD3_FUNC
FIELD_MACROS
static void dump(const char *tag, const uint32_t *d, size_t n) { printf("%s", tag); for (size_t i = 0; i < n; i++) printf(" 0x%x", d[i]); printf("\n"); }
int main(int argc, char **argv) {
  const int type = atoi(argv[1]), qp = atoi(argv[2]), level = atoi(argv[3]);
  Pic pic{}; pic.CodingType = type; pic.pic_init_qp_minus26 = qp - 26; pic.RefPicFlag = 1;
  Seq seq{}; seq.TargetUsage = 7; seq.GopRefDist = 1; seq.LowDelayMode = 1; seq.GopPicSize = 30; seq.FrameWidth = 64; seq.FrameHeight = 64; seq.Level = level;
  Slice sl{};
  Ctx ctx; ctx.m_avcPicParams = &pic; ctx.m_avcSeqParams = &seq; ctx.m_avcSliceParams = &sl; ctx.m_osItf = &ctx.m_osItfStorage;
  Settings S; ctx.m_featureSetting = &S;
  ctx.SetCmd3(); ctx.SetImg();
  {
    CMD3Par par; for (auto &f : S.vdencCmd3Settings) f(par);
    _VDENC_CMD3_CMD cmd; impl_cmd3(par, cmd);
    dump("CMD3", (uint32_t *)&cmd, sizeof(cmd) / 4);
  }
  {
    // VDENC_AVC_IMG_STATE: SETPAR of AvcBasicFeature and AvcReferenceFrames for a progressive frame: one reference (frame store 0) for P; colocated MV writing is left off (only
    // B pictures read it). The picture-dependent fields (size, quantiser, POCs, reference frame store) are set by the encoder.
    IMGPar par;
    par.pictureType = type - 1; par.transform8X8Flag = 0; par.pictureHeightMinusOne = 3; par.pictureWidth = 4;
    if (type != I_TYPE) { par.fwdRefIdx0ReferencePicture = 0; par.fwdRefIdx1ReferencePicture = 0xf; par.fwdRefIdx2ReferencePicture = 0xf; par.numberOfL0ReferencesMinusOne = 0; par.numberOfL1ReferencesMinusOne = 0; }
    par.qpPrimeY = qp;
    for (auto &f : S.vdencAvcImgStateSettings) f(par);
    Cmd::VDENC_AVC_IMG_STATE_CMD cmd; auto &params = par;
    DO_FIELDS_IMG();
    for (auto &f : par.extSettings) f((uint32_t *)&cmd);
    dump("IMG", (uint32_t *)&cmd, sizeof(cmd) / 4);
  }
  if (type == P_TYPE && qp == 26 && level == 40) {
    {
      PMS_PAR par;
      par.standardSelect = 2; par.chromaType = 1; par.randomAccess = false;
      par.verticalShift32Minus1 = 0; par.hzShift32Minus1 = 15; par.numVerticalReqMinus1 = 5; par.numHzReqMinus1 = 0;
      static const uint8_t par1table[] = {0, 0, 1, 1, 1, 1, 1};
      par.VdencPipeModeSelectPar1 = par1table[7 - 1]; par.VdencPipeModeSelectPar0 = 1;
      Cmd::VDENC_PIPE_MODE_SELECT_CMD cmd; auto &params = par;
      DO_FIELDS_PMS();
      PMS_LAYERS
      dump("PMS", (uint32_t *)&cmd, sizeof(cmd) / 4);
    }
    { Cmd::VDENC_AVC_SLICE_STATE_CMD cmd; dump("SLICE", (uint32_t *)&cmd, 4); }
    { Cmd::VDENC_WALKER_STATE_CMD cmd; dump("WALK", (uint32_t *)&cmd, 3); }
  }
}
'''


def tables_text(cs_avc):
    s = open(cs_avc).read()
    a = s.index('const uint8_t AvcVdencCMD3ConstSettings::AvcVdencCMD3ConstSettings_0')
    b = s.index('const bool EncodeAvcVdencConstSettings::m_perfModeEnabled')
    return s[a:b]


def struct_decl(cs_h):
    s = open(cs_h).read()
    i = s.index('struct AvcVdencCMD3ConstSettings')
    return s[i:s.index('};', i) + 2]


def cmd3_impl(impl):
    s = open(impl).read()
    i = s.index('_MHW_SETCMD_OVERRIDE_DECL(VDENC_CMD3)')
    body = brace_block(s, i)
    body = body.replace('_MHW_SETCMD_CALLBASE(VDENC_CMD3);', '')
    j = body.index('#define DO_FIELDS()')
    k = body.index('#include "mhw_hwcmd_process_cmdfields.h"')
    pre, macro = body[:j], body[j:k]
    macro = macro.replace('#define DO_FIELDS()', '#define DO_FIELDS3()').replace('DO_FIELD(', 'DOF(')
    return 'static void impl_cmd3(CMD3Par &params, _VDENC_CMD3_CMD &cmd) {\n' + pre + macro + '\nDO_FIELDS3();\n}\n'


def build(d, tmp, plat):
    bodies = '\n'.join([
        'int SetCmd3() {%s}' % function_body(d + '/cs_avc.cpp', 'MOS_STATUS EncodeAvcVdencConstSettings::SetVdencCmd3Settings()'),
        'int SetImg() {%s}' % function_body(d + '/' + plat['cs'], 'MOS_STATUS %s::SetVdencAvcImgStateSettings()' % plat['cs_class']),
    ])
    pars = ''.join([par_struct(d + '/cmdpar.h', 'VDENC_CMD3', 'CMD3Par'), par_struct(d + '/cmdpar.h', 'VDENC_AVC_IMG_STATE', 'IMGPar'),
                    par_struct(d + '/cmdpar.h', 'VDENC_PIPE_MODE_SELECT', 'PMS_PAR')])
    macros = ''.join([fields_macro(d + '/impl_common.h', 'VDENC_AVC_IMG_STATE', 'DO_FIELDS_IMG'), fields_macro(d + '/impl_common.h', 'VDENC_PIPE_MODE_SELECT', 'DO_FIELDS_PMS')] +
                     [fields_macro(d + '/' + f, 'VDENC_PIPE_MODE_SELECT', 'DO_FIELDS_PMS_%d' % i) for i, f in enumerate(plat['pms'])])
    src = (HARNESS.replace('HWCMD_INCLUDE', plat['hwcmd_include']).replace('NAMESPACE', plat['ns']).replace('PMS_LAYERS', ' '.join('DO_FIELDS_PMS_%d();' % i for i in range(len(plat['pms'])))).replace('STRUCT_DECL', struct_decl(d + '/cs_avc.h')).replace('TABLES', tables_text(d + '/cs_avc.cpp')).replace('PAR_STRUCTS', pars)
           .replace('BODIES', bodies).replace('CMD3_FUNC', cmd3_impl(d + '/impl_common.h')).replace('FIELD_MACROS', macros))
    os.makedirs(tmp + '/stub', exist_ok=True)
    os.makedirs(tmp + '/hdr', exist_ok=True)
    open(tmp + '/stub/mhw_hwcmd.h', 'w').write('''#pragma once
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#define MOS_ZeroMemory(p, s) memset(p, 0, s)
#define MEDIA_CLASS_DEFINE_END(x)
#define __CODEGEN_MAX(_a, _b) (((_a) > (_b)) ? (_a) : (_b))
#define __CODEGEN_BITFIELD(l, h) (h) - (l) + 1
#define __CODEGEN_OP_LENGTH_BIAS 2
#define __CODEGEN_OP_LENGTH(x) (uint32_t)((__CODEGEN_MAX(x, __CODEGEN_OP_LENGTH_BIAS)) - __CODEGEN_OP_LENGTH_BIAS)
''')
    open(tmp + '/hdr/' + plat['hwcmd_include'], 'w').write(open(d + '/' + plat['hwcmd']).read())
    open(tmp + '/stub/mhw_vdbox_vdenc_hwcmd_ext.h', 'w').write('')
    for f in os.listdir(d):   # (the const settings include their image state tables from *_open.h files)
        if f.endswith('_open.h') or f.startswith('encode_'): open(tmp + '/stub/' + f, 'w').write(open(d + '/' + f).read())
    open(tmp + '/main.cpp', 'w').write(src)
    subprocess.check_call(['g++', '-std=c++17', '-w', '-I' + tmp + '/stub', '-I' + tmp + '/hdr', tmp + '/main.cpp', '-o', tmp + '/harness'])
    return tmp + '/harness'


def run(h, t, qp, level):
    out = {}
    for line in subprocess.check_output([h, str(t), str(qp), str(level)], text=True).split('\n'):
        if line:
            k, *v = line.split()
            out[k] = [int(x, 16) for x in v]
    return out


def main():
    d, out_path = sys.argv[1], sys.argv[2]
    plat = PLATFORMS[sys.argv[3] if len(sys.argv) > 3 else 'xelpg']
    with tempfile.TemporaryDirectory() as tmp:
        h = build(d, tmp, plat)
        runs = {(t, qp): run(h, t, qp, 40) for t in (1, 2) for qp in QPS}
        level_runs = {lv: run(h, 2, 26, lv) for lv in LEVELS}
        level_runs_i = {lv: run(h, 1, 26, lv) for lv in LEVELS}
    header = open(d + '/' + plat['hwcmd']).read()
    P = plat['prefix']
    o = ['// Generated by tools/gen_intel_avc_xelpg.py from the Intel media driver (MIT licence): the VDEnc commands of H.264 encode on Xe-LPG (constant QP, target usage 7, one slice).',
         '// Do not edit by hand.']
    base = runs[(2, 26)]
    for tag, name, struct, indent in [('PMS', 'AvcVdencPipeModeSelect', 'VDENC_PIPE_MODE_SELECT_CMD', '    '), ('SLICE', 'AvcVdencSliceState', 'VDENC_AVC_SLICE_STATE_CMD', '    '),
                                      ('WALK', 'AvcVdencWalkerState', 'VDENC_WALKER_STATE_CMD', '    ')]:
        img = base[tag]
        o += ['', '// %s, %d dwords' % (struct, len(img)), 'constexpr uint32_t k' + P + '%sDwords = %d;' % (name, len(img)), emit_array('k' + P + '%sImage' % name, img)]
        if tag in ('SLICE', 'WALK'):
            for n, (dw, lo, hi) in sorted(parse_fields(struct_text(header, struct, indent)).items(), key=lambda kv: kv[1]):
                o.append('constexpr EncodeField k' + P + '%s_%s = {%d, %d, %d};' % (name, n, dw, lo, hi))
    # CMD3 depends on the quantiser all over; keep the whole command for every quantiser.
    for t, tn in ((1, 'I'), (2, 'P')):
        words = [w for qp in QPS for w in runs[(t, qp)]['CMD3']]
        o += ['', '// VDENC_CMD3, 23 dwords for each quantiser %d .. %d, %s pictures' % (QPS[0], QPS[-1], tn), emit_array('k' + P + 'AvcVdencCmd3Table%s' % tn, words)]
    o += ['', 'constexpr uint32_t k' + P + 'AvcVdencCmd3Dwords = 23;']
    # VDENC_AVC_IMG_STATE: the image at quantiser 26 and level 40, plus the dwords that vary.
    for t, tn in ((1, 'I'), (2, 'P')):
        img = runs[(t, 26)]['IMG']
        varying_qp = sorted({i for qp in QPS for i in range(26) if runs[(t, qp)]['IMG'][i] != img[i]})
        lv = level_runs[40] if t == 2 else level_runs_i[40]
        lrs = level_runs if t == 2 else level_runs_i
        varying_lv = sorted({i for l in LEVELS for i in range(26) if lrs[l]['IMG'][i] != lv['IMG'][i]})
        print('IMG %s varies with qp in %s, with level in %s' % (tn, varying_qp, varying_lv), file=sys.stderr)
        assert varying_qp == [14] and varying_lv == [8], 'unexpected dependence'
        o += ['', '// VDENC_AVC_IMG_STATE, 26 dwords, %s pictures at level 40 / quantiser 26; dword 8 by level_idc 0 .. 52 (the media driver indexes its table by the level).' % tn,
              emit_array('k' + P + 'AvcVdencImgState%s' % tn, runs[(t, 26)]['IMG']), emit_array('k' + P + 'AvcVdencImgState%sLevelDword8' % tn, [lrs[l]['IMG'][8] for l in LEVELS])]
    o += ['', 'constexpr uint32_t k' + P + 'AvcVdencImgStateDwords = 26;']
    for n, (dw, lo, hi) in sorted(parse_fields(struct_text(header, 'VDENC_AVC_IMG_STATE_CMD' if False else '_VDENC_AVC_IMG_STATE_CMD', '')).items(), key=lambda kv: kv[1]):
        o.append('constexpr EncodeField k' + P + 'AvcVdencImgState_%s = {%d, %d, %d};' % (n, dw, lo, hi))
    open(out_path, 'w').write('\n'.join(o) + '\n')


if __name__ == '__main__':
    main()
