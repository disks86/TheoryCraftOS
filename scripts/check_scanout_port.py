#!/usr/bin/env python3
"""Checks the graphics plane setup (amd_dce11::Scanout in src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.cpp) against the Linux driver.

  check_scanout_port.py path/to/dce_11_2_d.h path/to/dce_11_2_sh_mask.h path/to/dce_v11_0.c

(kernel tag v6.12: drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h and _sh_mask.h, drivers/gpu/drm/amd/amdgpu/dce_v11_0.c). It checks the register addresses and bits Scanout uses,
the VGA control registers per CRTC, and that dce_v11_0_crtc_do_set_base still writes the registers in the order Scanout::set does.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    d = open(sys.argv[1]).read()
    m = open(sys.argv[2]).read()
    c = open(sys.argv[3]).read()
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
    # GRPH_DEPTH 2, GRPH_FORMAT 0 for DRM_FORMAT_XRGB8888 / ARGB8888
    if not re.search(r'case DRM_FORMAT_XRGB8888:\s*case DRM_FORMAT_ARGB8888:\s*fb_format = REG_SET_FIELD\(0, GRPH_CONTROL, GRPH_DEPTH, 2\);\s*fb_format = REG_SET_FIELD\(fb_format, GRPH_CONTROL, GRPH_FORMAT, 0\);', c) or \
            not re.search(r'kGrphControlArgb8888 = 2;', cpp):
        fail('the ARGB8888 control value')
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
    if bad:
        sys.exit(1)
    print('check_scanout_port: ok')


main()
