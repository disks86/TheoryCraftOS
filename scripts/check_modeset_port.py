#!/usr/bin/env python3
"""Checks the mode setting tables (src/Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.*) against the Linux driver's atombios.h.

  check_modeset_port.py path/to/atombios.h

(kernel tag v6.12: drivers/gpu/drm/amd/include/atombios.h). It checks the master list index of SetPixelClock, SetCRTC_Timing, SelectCRTC_Source, EnableCRTC, BlankCRTC and
UpdateCRTC_DoubleBufferRegisters, the byte layout of PIXEL_CLOCK_PARAMETERS_V7, SET_CRTC_TIMING_PARAMETERS, SELECT_CRTC_SOURCE_PARAMETERS_V2/V3, ENABLE_CRTC_PARAMETERS and
BLANK_CRTC_PARAMETERS against the packing in AmdModeSet.cpp, the mode misc bits, the ASIC_INT_DIGn_ENCODER_IDs, the ATOM_ENCODER_CMD_* and transmitter actions, the PIXEL_CLOCK_V7 flags
and deep colour ratios, and the transmitter V4 / V1.5 words for the non-DP modes (the DP ones are checked by check_dplink_port.py).

NOT checked: amdgpu_atombios_crtc.c and amdgpu_atombios_encoder.c (which values the driver puts in these parameters, the order of the mode set): from memory.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_dplink_port as base

ROOT = base.ROOT
fail = base.fail


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    h = open(sys.argv[1]).read()
    hpp = open(os.path.join(ROOT, 'AmdModeSet.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdModeSet.cpp')).read()

    m = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_COMMAND_TABLES\s*\{(.*?)\}\s*ATOM_MASTER_LIST_OF_COMMAND_TABLES', h, re.S)
    names = re.findall(r'USHORT\s+(\w+);', m.group(1))
    for n, c in (('SetPixelClock', 'kSetPixelClock'), ('SetCRTC_Timing', 'kSetCrtcTiming'), ('SelectCRTC_Source', 'kSelectCrtcSource'), ('EnableCRTC', 'kEnableCrtc'),
                 ('BlankCRTC', 'kBlankCrtc'), ('UpdateCRTC_DoubleBufferRegisters', 'kUpdateDoubleBuffer'), ('DIG1EncoderControl', 'kEncoder1Table'),
                 ('DIG2EncoderControl', 'kEncoder2Table'), ('DIG1TransmitterControl', 'kTransmitterTable')):
        k = re.search(c + r' = (\d+)', hpp)
        if not k or names.index(n) != int(k.group(1)):
            fail('%s: index %d, ours %s' % (n, names.index(n), k.group(1) if k else None))

    # (the layout parser counts the ATOM_MODE_MISC_INFO_ACCESS union as one byte and skips arrays, so only the offsets before them are checked; the sizes 16, 24, 4 and 8 are by hand: the
    # misc info is a USHORT at 16 and the CRTC byte at 18 in SET_CRTC_TIMING_PARAMETERS, the padding makes the other structs four bytes)
    def layout(name, want, size=None):
        lay, total = base.struct_layout(h, name)
        for f, o in want.items():
            if lay.get(f, (None,))[0] != o:
                fail('%s: %s at %s, expected %d' % (name, f, lay.get(f), o))
        if size is not None and total != size:
            fail('%s: %d bytes, expected %d' % (name, total, size))

    layout('PIXEL_CLOCK_PARAMETERS_V7', {'ulPixelClock': 0, 'ucPpll': 4, 'ucTransmitterID': 5, 'ucEncoderMode': 6, 'ucMiscInfo': 7, 'ucCRTC': 8, 'ucDeepColorRatio': 9})
    layout('SET_CRTC_TIMING_PARAMETERS', {'usH_Total': 0, 'usH_Disp': 2, 'usH_SyncStart': 4, 'usH_SyncWidth': 6, 'usV_Total': 8, 'usV_Disp': 10, 'usV_SyncStart': 12, 'usV_SyncWidth': 14,
                                          'susModeMiscInfo': 16})
    layout('SELECT_CRTC_SOURCE_PARAMETERS_V2', {'ucCRTC': 0, 'ucEncoderID': 1, 'ucEncodeMode': 2}, 4)
    layout('SELECT_CRTC_SOURCE_PARAMETERS_V3', {'ucCRTC': 0, 'ucEncoderID': 1, 'ucEncodeMode': 2, 'ucDstBpc': 3}, 4)
    layout('ENABLE_CRTC_PARAMETERS', {'ucCRTC': 0, 'ucEnable': 1})
    layout('BLANK_CRTC_PARAMETERS', {'ucCRTC': 0, 'ucBlanking': 1, 'usBlackColorRCr': 2, 'usBlackColorGY': 4, 'usBlackColorBCb': 6})

    # our packing
    for pat, what in ((r'\{mode\.pixel_clock_khz \* 10, uint32_t\{out\.pll\} \| \(uint32_t\{out\.encoder_id\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(misc << 24\),\s*uint32_t\{out\.crtc\} \| \(deep << 8\), 0\}', 'SetPixelClock words'),
                      (r'uint32_t\{out\.crtc\} \| \(uint32_t\{dig_encoder_id\(out\.dig_encoder\)\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(\(crev == 3 \? bpc : 0u\) << 24\)', 'SelectCRTC_Source word'),
                      (r'misc \| \(uint32_t\{out\.crtc\} << 16\)', 'SetCRTC_Timing misc | CRTC << 16'),
                      (r'uint32_t\{mode\.h_total\} \| \(uint32_t\{mode\.h_display\} << 16\)', 'timing word 0'),
                      (r'uint32_t\{mode\.h_sync_start\} \| \(uint32_t\(mode\.h_sync_end - mode\.h_sync_start\) << 16\)', 'timing word 1'),
                      (r'uint32_t\{mode\.v_total\} \| \(uint32_t\{mode\.v_display\} << 16\)', 'timing word 2'),
                      (r'uint32_t\{mode\.v_sync_start\} \| \(uint32_t\(mode\.v_sync_end - mode\.v_sync_start\) << 16\)', 'timing word 3'),
                      (r'words\[2\] = \{uint32_t\{crtc\} \| \(uint32_t\{blank \? 1u : 0u\} << 8\), 0\}', 'BlankCRTC words')):
        if not re.search(pat, cpp):
            fail('%s does not follow the layout' % what)
    # mode misc bits
    for ours, name in (('kModeMiscHSyncPolarity', 'ATOM_HSYNC_POLARITY'), ('kModeMiscVSyncPolarity', 'ATOM_VSYNC_POLARITY'), ('kModeMiscInterlace', 'ATOM_INTERLACE'), ('kModeMiscDoubleClock', 'ATOM_DOUBLE_CLOCK_MODE')):
        g = re.search(ours + r' = (0x[0-9A-Fa-f]+)', cpp)
        if not g or int(g.group(1), 16) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    g = re.search(r'kMiscDviDualLink = (0x[0-9A-Fa-f]+)', cpp)
    if not g or int(g.group(1), 16) != base.define(h, 'PIXEL_CLOCK_V7_MISC_DVI_DUALLINK_EN'):
        fail('kMiscDviDualLink')
    for name, v in (('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_DIS', 0), ('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_5_4', 1), ('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_3_2', 2)):
        if base.define(h, name) != v:
            fail(name)
    if not re.search(r'out\.bits_per_color == 10 \? 1 : out\.bits_per_color == 12 \? 2 : 0', cpp):
        fail('the deep colour ratio is not 10 bits 5:4, 12 bits 3:2')
    # PLL ids, DIG encoder ids
    for ours, name in (('kPpll1', 'ATOM_PPLL1'), ('kPpll2', 'ATOM_PPLL2'), ('kPpll0', 'ATOM_PPLL0'), ('kPllInvalid', 'ATOM_PPLL_INVALID')):
        g = re.search(ours + r' = (0x[0-9A-Fa-f]+|\d+)', hpp)
        if not g or int(g.group(1), 0) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    ids = re.search(r'kIds\[\] = \{([^}]*)\}', cpp)
    want = [base.define(h, 'ASIC_INT_DIG%d_ENCODER_ID' % i) for i in range(1, 8)]
    got = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', ids.group(1))] if ids else None
    if got != want:
        fail('DIG encoder ids %s, atombios.h %s' % (got, want))
    # actions and modes
    for ours, name in (('kEncoderDisable', 'ATOM_DISABLE'), ('kEncoderEnable', 'ATOM_ENABLE'), ('kEncoderDpVideoOff', 'ATOM_ENCODER_CMD_DP_VIDEO_OFF'), ('kEncoderDpVideoOn', 'ATOM_ENCODER_CMD_DP_VIDEO_ON'),
                       ('kEncoderSetup', 'ATOM_ENCODER_CMD_SETUP'), ('kTransmitterDisable', 'ATOM_TRANSMITTER_ACTION_DISABLE'), ('kTransmitterEnable', 'ATOM_TRANSMITTER_ACTION_ENABLE'),
                       ('kTransmitterDisableOutput', 'ATOM_TRANSMITTER_ACTION_DISABLE_OUTPUT'), ('kTransmitterEnableOutput', 'ATOM_TRANSMITTER_ACTION_ENABLE_OUTPUT'),
                       ('kTransmitterSetup', 'ATOM_TRANSMITTER_ACTION_SETUP'), ('kModeDp', 'ATOM_ENCODER_MODE_DP'), ('kModeLvds', 'ATOM_ENCODER_MODE_LVDS'), ('kModeDvi', 'ATOM_ENCODER_MODE_DVI'),
                       ('kModeHdmi', 'ATOM_ENCODER_MODE_HDMI')):
        g = re.search(r'\b' + ours + r' = (0x[0-9A-Fa-f]+|\d+)', hpp)
        if not g or int(g.group(1), 0) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    # the transmitter V4 / V1.5 words, same layout as the training ones
    if not re.search(r'\(symbol_clock & 0xFFFF\) \| \(config << 16\) \| \(uint32_t\{action\} << 24\), lanes', cpp):
        fail('the transmitter V4 words')
    if not re.search(r'\(symbol_clock & 0xFFFF\) \| \(uint32_t\{out\.dig_encoder\} << 16\) \| \(uint32_t\{action\} << 24\)', cpp) or \
            not re.search(r'lanes \| \(uint32_t\{out\.connector_object_id\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(config << 24\)', cpp):
        fail('the transmitter V1.5 words')
    if not re.search(r'\(out\.dual_link \? 1u : 0u\) \| \(coherent \? 2u : 0u\) \| \(out\.link_b \? 1u << 2 : 0u\) \| \(uint32_t\{out\.dig_encoder & 1u\} << 3\) \|\s*\(uint32_t\{out\.reference_clock_source & 3u\} << 4\) \| \(uint32_t\{out\.dig_encoder / 2u\} << 6\)', cpp):
        fail('the transmitter V4 config')
    if not re.search(r'\(coherent \? 2u : 0u\) \| \(uint32_t\{out\.reference_clock_source & 3u\} << 2\) \| \(hpd_sel << 4\)', cpp):
        fail('the transmitter V1.5 config')
    if base.bad:
        sys.exit(1)
    print('check_modeset_port: ok')


if __name__ == '__main__':
    main()
