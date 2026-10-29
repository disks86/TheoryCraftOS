#!/usr/bin/env python3
"""Checks the graphics plane setup (amd_dce11::Scanout in src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.cpp) against the Linux driver.

  check_scanout_port.py path/to/dce_11_2_d.h path/to/dce_11_2_sh_mask.h path/to/dce_v11_0.c path/to/dce_11_2_enum.h

(kernel tag v6.12: drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h and _sh_mask.h and _enum.h, drivers/gpu/drm/amd/amdgpu/dce_v11_0.c). It checks the register addresses and bits Scanout uses,
the VGA control registers per CRTC, the GRPH_CONTROL field positions, the pixel format table (depth, format, crossbars, table bypass), the array modes, the look-up table registers and masks of
dce_v11_0_crtc_load_lut and the order of its writes, and that dce_v11_0_crtc_do_set_base still writes the registers in the order Scanout::set does.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    d = open(sys.argv[1]).read()
    m = open(sys.argv[2]).read()
    c = open(sys.argv[3]).read()
    en = open(sys.argv[4]).read()
    cpp = open(os.path.join(ROOT, 'AmdDce11.cpp')).read()
    bad = False

    def fail(text):
        nonlocal bad
        bad = True
        print('FAIL:', text)

    def reg(name):
        g = re.search(r'#define\s+' + name + r'\s+(0x[0-9a-fA-F]+)', d)
        return int(g.group(1), 16) if g else None

    pairs = (('kGrphEnable', 'mmGRPH_ENABLE'), ('kGrphControl', 'mmGRPH_CONTROL'), ('kGrphLut10BitBypass', 'mmGRPH_LUT_10BIT_BYPASS'), ('kGrphSwapCntl', 'mmGRPH_SWAP_CNTL'),
             ('kGrphPrimaryAddress', 'mmGRPH_PRIMARY_SURFACE_ADDRESS'), ('kGrphSecondaryAddress', 'mmGRPH_SECONDARY_SURFACE_ADDRESS'), ('kGrphPitch', 'mmGRPH_PITCH'),
             ('kGrphPrimaryAddressHigh', 'mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH'), ('kGrphSecondaryAddressHigh', 'mmGRPH_SECONDARY_SURFACE_ADDRESS_HIGH'),
             ('kGrphSurfaceOffsetX', 'mmGRPH_SURFACE_OFFSET_X'), ('kGrphSurfaceOffsetY', 'mmGRPH_SURFACE_OFFSET_Y'), ('kGrphXStart', 'mmGRPH_X_START'), ('kGrphYStart', 'mmGRPH_Y_START'),
             ('kGrphXEnd', 'mmGRPH_X_END'), ('kGrphYEnd', 'mmGRPH_Y_END'), ('kGrphFlipControl', 'mmGRPH_FLIP_CONTROL'), ('kLbDesktopHeight', 'mmLB_DESKTOP_HEIGHT'),
             ('kViewportStart', 'mmVIEWPORT_START'), ('kViewportSize', 'mmVIEWPORT_SIZE'), ('kCrtcMasterUpdateMode', 'mmCRTC_MASTER_UPDATE_MODE'))
    for ours, theirs in pairs:
        g = re.search(r'\b' + ours + r' = (0x[0-9a-fA-F]+)', cpp)
        if not g or int(g.group(1), 16) != reg(theirs):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, theirs, reg(theirs)))
    vga = re.search(r'kVgaControl\[kMaxCrtcs\] = \{([^}]*)\}', cpp)
    got = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', vga.group(1))] if vga else None
    want = [reg('mmD%dVGA_CONTROL' % i) for i in range(1, 7)]
    if got != want:
        fail('VGA control registers %s, header %s' % (got, want))
    for ours, theirs in (('kFlipHRetraceEnable', 'GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_H_RETRACE_EN_MASK'), ('kLut10BitBypassEnable', 'GRPH_LUT_10BIT_BYPASS__GRPH_LUT_10BIT_BYPASS_EN_MASK')):
        g = re.search(r'\b' + ours + r' = (0x[0-9a-fA-F]+)', cpp)
        t = re.search(r'#define\s+' + theirs + r'\s+(0x[0-9a-fA-F]+)', m)
        if not g or not t or int(g.group(1), 16) != int(t.group(1), 16):
            fail(ours)
    # the pixel formats: depth and format per DRM format, in our table
    body0 = c[c.index('static int dce_v11_0_crtc_do_set_base'):c.index('static void dce_v11_0_set_interleave')]
    ours = {}
    for m_ in re.finditer(r'case ScanoutFormat::(\w+): \*out = \{(\d+), (\d+), ([^,]+), (true|false)\}', cpp):
        ours[m_.group(1)] = (int(m_.group(2)), int(m_.group(3)), m_.group(4).strip(), m_.group(5) == 'true')
    drm = {'Indexed8': 'C8', 'Argb4444': 'ARGB4444', 'Argb1555': 'ARGB1555', 'Bgra5551': 'BGRA5551', 'Rgb565': 'RGB565', 'Argb8888': 'ARGB8888', 'Argb2101010': 'ARGB2101010',
           'Bgra1010102': 'BGRA1010102', 'Abgr8888': 'ABGR8888'}
    for name, fmt in drm.items():
        g = re.search(r'case DRM_FORMAT_' + fmt + r':(.*?)break;', body0, re.S)
        if not g or name not in ours:
            fail('format %s' % name)
            continue
        t = g.group(1)
        depth = int(re.search(r'GRPH_DEPTH, (\d+)', t).group(1))
        form = int(re.search(r'GRPH_FORMAT, (\d+)', t).group(1)) if 'GRPH_FORMAT' in t else None
        bypass = 'bypass_lut = true' in t
        cross = 'GRPH_RED_CROSSBAR, 2' in t and 'GRPH_BLUE_CROSSBAR, 2' in t
        if ours[name][0] != depth or (form is not None and ours[name][1] != form) or ours[name][3] != bypass or (ours[name][2] != '0') != cross:
            fail('format %s: ours %s, driver depth %s format %s bypass %s crossbars %s' % (name, ours[name], depth, form, bypass, cross))
    for alias in ('XRGB8888', 'XRGB4444', 'XRGB1555', 'BGRX5551', 'XRGB2101010', 'BGRX1010102', 'XBGR8888'):
        if 'case DRM_FORMAT_' + alias + ':' not in body0:
            fail('the driver no longer has %s' % alias)
    # GRPH_CONTROL field positions
    for field, ours_name in (('GRPH_NUM_BANKS', 'kShiftNumBanks'), ('GRPH_BANK_WIDTH', 'kShiftBankWidth'), ('GRPH_FORMAT', 'kShiftFormat'), ('GRPH_BANK_HEIGHT', 'kShiftBankHeight'),
                             ('GRPH_TILE_SPLIT', 'kShiftTileSplit'), ('GRPH_MACRO_TILE_ASPECT', 'kShiftMacroTileAspect'), ('GRPH_ARRAY_MODE', 'kShiftArrayMode'), ('GRPH_PIPE_CONFIG', 'kShiftPipeConfig'),
                             ('GRPH_MICRO_TILE_MODE', 'kShiftMicroTileMode')):
        g = re.search(r'#define GRPH_CONTROL__' + field + r'__SHIFT (0x[0-9a-fA-F]+)', m)
        o = re.search(ours_name + r' = (\d+)', cpp)
        if not g or not o or int(g.group(1), 16) != int(o.group(1)):
            fail('%s shift' % field)
    for field, shift in (('RED', 4), ('BLUE', 8)):
        g = re.search(r'#define GRPH_SWAP_CNTL__GRPH_' + field + r'_CROSSBAR__SHIFT (0x[0-9a-fA-F]+)', m)
        if not g or int(g.group(1), 16) != shift:
            fail('crossbar %s' % field)
    for name, v in (('ARRAY_LINEAR_GENERAL', 0), ('ARRAY_1D_TILED_THIN1', 2), ('ARRAY_2D_TILED_THIN1', 4), ('ADDR_SURF_MICRO_TILING_DISPLAY', 0)):
        g = re.search(name + r'\s*= (0x[0-9a-fA-F]+)', en)
        if not g or int(g.group(1), 16) != v:
            fail(name)
    if not re.search(r'Linear = 0, Tiled1D = 2, Tiled2D = 4', open(os.path.join(ROOT, 'AmdDce11.hpp')).read()):
        fail('the array modes in AmdDce11.hpp')
    # dce_v11_0_crtc_load_lut
    lut_regs = (('kInputCscControl', 'mmINPUT_CSC_CONTROL'), ('kPrescaleGrphControl', 'mmPRESCALE_GRPH_CONTROL'), ('kInputGammaControl', 'mmINPUT_GAMMA_CONTROL'), ('kDcLutControl', 'mmDC_LUT_CONTROL'),
                ('kDcLutBlackBlue', 'mmDC_LUT_BLACK_OFFSET_BLUE'), ('kDcLutBlackGreen', 'mmDC_LUT_BLACK_OFFSET_GREEN'), ('kDcLutBlackRed', 'mmDC_LUT_BLACK_OFFSET_RED'),
                ('kDcLutWhiteBlue', 'mmDC_LUT_WHITE_OFFSET_BLUE'), ('kDcLutWhiteGreen', 'mmDC_LUT_WHITE_OFFSET_GREEN'), ('kDcLutWhiteRed', 'mmDC_LUT_WHITE_OFFSET_RED'),
                ('kDcLutRwMode', 'mmDC_LUT_RW_MODE'), ('kDcLutWriteEnMask', 'mmDC_LUT_WRITE_EN_MASK'), ('kDcLutRwIndex', 'mmDC_LUT_RW_INDEX'), ('kDcLut30Color', 'mmDC_LUT_30_COLOR'),
                ('kDegammaControl', 'mmDEGAMMA_CONTROL'), ('kGamutRemapControl', 'mmGAMUT_REMAP_CONTROL'), ('kRegammaControl', 'mmREGAMMA_CONTROL'), ('kOutputCscControl', 'mmOUTPUT_CSC_CONTROL'),
                ('kDenormControl', 'mmDENORM_CONTROL'), ('kAlphaControl', 'mmALPHA_CONTROL'))
    for ours_name, theirs in lut_regs:
        g = re.search(r'\b' + ours_name + r' = (0x[0-9a-fA-F]+)', cpp)
        if not g or int(g.group(1), 16) != reg(theirs):
            fail('%s is %s, %s is %s' % (ours_name, g.group(1) if g else None, theirs, reg(theirs)))
    for ours_name, theirs in (('kInputCscGrphModeMask', 'INPUT_CSC_CONTROL__INPUT_CSC_GRPH_MODE_MASK'), ('kPrescaleBypass', 'PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK'),
                              ('kInputGammaModeMask', 'INPUT_GAMMA_CONTROL__GRPH_INPUT_GAMMA_MODE_MASK'), ('kGamutModeMask', 'GAMUT_REMAP_CONTROL__GRPH_GAMUT_REMAP_MODE_MASK'),
                              ('kRegammaModeMask', 'REGAMMA_CONTROL__GRPH_REGAMMA_MODE_MASK'), ('kOutputCscModeMask', 'OUTPUT_CSC_CONTROL__OUTPUT_CSC_GRPH_MODE_MASK'),
                              ('kCursorAlphaBlend', 'ALPHA_CONTROL__CURSOR_ALPHA_BLND_ENA_MASK')):
        g = re.search(r'\b' + ours_name + r' = (0x[0-9a-fA-F]+)', cpp)
        t = re.search(r'#define\s+' + theirs + r'\s+(0x[0-9a-fA-F]+)', m)
        if not g or not t or int(g.group(1), 16) != int(t.group(1), 16):
            fail(ours_name)
    deg = sum(int(re.search(r'#define DEGAMMA_CONTROL__' + f + r'_MASK (0x[0-9a-fA-F]+)', m).group(1), 16) for f in ('GRPH_DEGAMMA_MODE', 'CURSOR2_DEGAMMA_MODE', 'CURSOR_DEGAMMA_MODE'))
    g = re.search(r'kDegammaModeMask = (0x[0-9a-fA-F]+) \| (0x[0-9a-fA-F]+) \| (0x[0-9a-fA-F]+)', cpp)
    if not g or sum(int(x, 16) for x in g.groups()) != deg:
        fail('the degamma mask')
    lut = c[c.index('static void dce_v11_0_crtc_load_lut'):]
    lut = lut[:lut.index('\n}\n')]
    at = 0
    for key in ('INPUT_CSC_CONTROL', 'PRESCALE_GRPH_CONTROL', 'INPUT_GAMMA_CONTROL', 'DC_LUT_CONTROL', 'DC_LUT_BLACK_OFFSET_BLUE', 'DC_LUT_WHITE_OFFSET_RED', 'DC_LUT_RW_MODE',
                'DC_LUT_WRITE_EN_MASK', 'DC_LUT_RW_INDEX', 'DC_LUT_30_COLOR', 'DEGAMMA_CONTROL', 'GAMUT_REMAP_CONTROL', 'REGAMMA_CONTROL', 'OUTPUT_CSC_CONTROL', 'DENORM_CONTROL', 'ALPHA_CONTROL'):
        i = lut.find('mm' + key, at)
        if i < 0:
            fail('load_lut no longer writes %s in this order' % key)
            break
        at = i + 1
    if not re.search(r'\(\*r\+\+ & 0xffc0\) << 14\)', lut) or not re.search(r'\(\*g\+\+ & 0xffc0\) << 4\)', lut) or not re.search(r'\(\*b\+\+ >> 6\)', lut):
        fail('the LUT colour packing')
    # the order of the writes in do_set_base
    body = c[c.index('static int dce_v11_0_crtc_do_set_base'):c.index('static void dce_v11_0_set_interleave')]
    order = ['vga_enable(crtc, false)', 'mmGRPH_FLIP_CONTROL', 'mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH', 'mmGRPH_SECONDARY_SURFACE_ADDRESS_HIGH', 'mmGRPH_PRIMARY_SURFACE_ADDRESS +',
             'mmGRPH_SECONDARY_SURFACE_ADDRESS +', 'mmGRPH_CONTROL', 'mmGRPH_SWAP_CNTL', 'mmGRPH_LUT_10BIT_BYPASS', 'mmGRPH_SURFACE_OFFSET_X', 'mmGRPH_SURFACE_OFFSET_Y', 'mmGRPH_X_START',
             'mmGRPH_Y_START', 'mmGRPH_X_END', 'mmGRPH_Y_END', 'mmGRPH_PITCH', 'grph_enable(crtc, true)', 'mmLB_DESKTOP_HEIGHT', 'mmVIEWPORT_START', 'mmVIEWPORT_SIZE', 'mmCRTC_MASTER_UPDATE_MODE']
    at = 0
    for key in order:
        i = body.find(key, at)
        if i < 0:
            fail('do_set_base no longer writes %s in this order' % key)
            break
        at = i + 1
    if not re.search(r'viewport_h = \(crtc->mode\.vdisplay \+ 1\) & ~1', body):
        fail('the viewport height rounding')
    # the FMT block and the interleave
    for ours_name, theirs in (('kFmtBitDepthControl', 'mmFMT_BIT_DEPTH_CONTROL'), ('kLbDataFormat', 'mmLB_DATA_FORMAT')):
        g = re.search(r'\b' + ours_name + r' = (0x[0-9a-fA-F]+)', cpp)
        if not g or int(g.group(1), 16) != reg(theirs):
            fail(ours_name)
    for ours_name, field in (('kFmtTruncateEnable', 'FMT_TRUNCATE_EN'), ('kFmtSpatialDitherEnable', 'FMT_SPATIAL_DITHER_EN'), ('kFmtFrameRandom', 'FMT_FRAME_RANDOM_ENABLE'),
                            ('kFmtRgbRandom', 'FMT_RGB_RANDOM_ENABLE'), ('kFmtHighpassRandom', 'FMT_HIGHPASS_RANDOM_ENABLE'), ('kInterleaveEnable', None)):
        g = re.search(r'\b' + ours_name + r' = (?:1u << (\d+)|(0x[0-9a-fA-F]+))', cpp)
        want = int(re.search(r'#define FMT_BIT_DEPTH_CONTROL__' + field + r'_MASK (0x[0-9a-fA-F]+)', m).group(1), 16) if field else int(re.search(r'#define LB_DATA_FORMAT__INTERLEAVE_EN_MASK (0x[0-9a-fA-F]+)', m).group(1), 16)
        got = (1 << int(g.group(1))) if g and g.group(1) else (int(g.group(2), 16) if g else None)
        if got != want:
            fail('%s is %s, header %s' % (ours_name, got, want))
    for ours_name, field in (('kFmtTruncateDepthShift', 'FMT_TRUNCATE_DEPTH'), ('kFmtSpatialDitherDepthShift', 'FMT_SPATIAL_DITHER_DEPTH')):
        g = re.search(ours_name + r' = (\d+)', cpp)
        t = re.search(r'#define FMT_BIT_DEPTH_CONTROL__' + field + r'__SHIFT (0x[0-9a-fA-F]+)', m)
        if not g or not t or int(g.group(1)) != int(t.group(1), 16):
            fail(ours_name)
    fmt = c[c.index('static void dce_v11_0_program_fmt'):]
    fmt = fmt[:fmt.index('\n}\n')]
    if not (re.search(r'case 6:.*?FMT_SPATIAL_DITHER_DEPTH, 0\).*?FMT_TRUNCATE_DEPTH, 0\)', fmt, re.S) and re.search(r'case 8:.*?FMT_SPATIAL_DITHER_DEPTH, 1\).*?FMT_TRUNCATE_DEPTH, 1\)', fmt, re.S) and
            re.search(r'case 10:.*?FMT_SPATIAL_DITHER_DEPTH, 2\).*?FMT_TRUNCATE_DEPTH, 2\)', fmt, re.S)):
        fail('the FMT depths')
    if fmt.count('FMT_RGB_RANDOM_ENABLE') != 2 or 'if (bpc == 0)' not in fmt:
        fail('the FMT random enables')
    # the watermarks (Watermarks in AmdDce11.cpp): registers, fields, constants of dce_v11_0's bandwidth calculation
    for ours_name, theirs in (('kLbMemoryCtrl', 'mmLB_MEMORY_CTRL'), ('kPipe0DmifBufferControl', 'mmPIPE0_DMIF_BUFFER_CONTROL'), ('kDpgWatermarkMaskControl', 'mmDPG_WATERMARK_MASK_CONTROL'),
                              ('kDpgPipeUrgencyControl', 'mmDPG_PIPE_URGENCY_CONTROL')):
        g = re.search(r'\b' + ours_name + r' = (0x[0-9a-fA-F]+)', cpp)
        if not g or int(g.group(1), 16) != reg(theirs):
            fail(ours_name)
    g = re.search(r'kMcSharedChmap = (0x[0-9a-fA-F]+)', cpp)
    if not g or int(g.group(1), 16) != 0x801:
        fail('MC_SHARED_CHMAP (gmc_8_1_d.h: 0x801)')
    for ours_name, theirs, shift in (('kLbMemoryConfigMask', 'LB_MEMORY_CTRL__LB_MEMORY_CONFIG_MASK', 'LB_MEMORY_CTRL__LB_MEMORY_CONFIG__SHIFT'),
                                     ('kDmifBuffersAllocatedMask', 'PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATED_MASK', None),
                                     ('kDmifBuffersAllocationCompleted', 'PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATION_COMPLETED_MASK', None),
                                     ('kUrgencyWatermarkMaskMask', 'DPG_WATERMARK_MASK_CONTROL__URGENCY_WATERMARK_MASK_MASK', 'DPG_WATERMARK_MASK_CONTROL__URGENCY_WATERMARK_MASK__SHIFT')):
        g = re.search(r'\b' + ours_name + r' = (0x[0-9a-fA-F]+)', cpp)
        t = re.search(r'#define\s+' + theirs + r'\s+(0x[0-9a-fA-F]+)', m)
        if not g or not t or int(g.group(1), 16) != int(t.group(1), 16):
            fail(ours_name)
        if shift:
            sh = re.search(r'#define\s+' + shift + r'\s+(0x[0-9a-fA-F]+)', m)
            o = re.search((ours_name.replace('Mask', 'Shift') if 'Urgency' not in ours_name else 'kUrgencyWatermarkMaskShift') + r' = (\d+)', cpp)
            if not sh or not o or int(sh.group(1), 16) != int(o.group(1)):
                fail(ours_name + ' shift')
    wm = c[c.index('static u32 dce_v11_0_line_buffer_adjust'):c.index('static void dce_v11_0_audio_get_connected_pins')]
    for needle in ('mode->crtc_hdisplay < 1920', 'mode->crtc_hdisplay < 2560', 'mc_latency = 2000', 'dmif_size = 12288', 'dfixed_const(7)', 'dfixed_const(3)', 'dfixed_const(8)', 'dfixed_const(32)',
                   '(512 * 8 * 1000)', '(128 * 4 * 1000)', '40000000 / wm->disp_clk', 'min_t(u32, line_time, 65535)', 'return 1920 * 2', 'return 2560 * 2', 'return 4096 * 2',
                   'wm_high.bytes_per_pixel = 4', 'wm_high.vtaps = 1', 'URGENCY_WATERMARK_MASK, 1', 'URGENCY_WATERMARK_MASK, 2'):
        if needle not in wm:
            fail('the driver no longer has "%s"' % needle)
    for needle in ('kNoOfChanMask = 0xf000', 'mc_latency = 2000', 'dmif_size = 12288', '512 * 8 * 1000', '128 * 4 * 1000', '40000000 / wm.disp_clk', '1920 * 2', '2560 * 2', '4096 * 2', 'fx_const(7), fx_const(10)',
                   'fx_const(8), fx_const(10)', 'fx_const(32)', 'line_time > 65535', 'clocks.apu ? 2 : 4'):
        if needle not in cpp:
            fail('ours lacks "%s"' % needle)
    chan = re.findall(r'case (\d+):\s*(?:\n\s*)?return (\d+);', wm[wm.index('cik_get_number_of_dram_channels'):wm.index('struct dce10_wm_params')])
    mine = re.findall(r'case (\d+): return (\d+);', cpp[cpp.index('Watermarks::dram_channels'):cpp.index('Watermarks::dram_bandwidth')])
    if sorted(chan) != sorted(mine):
        fail('DRAM channel counts %s vs %s' % (chan, mine))
    if bad:
        sys.exit(1)
    print('check_scanout_port: ok')


main()
