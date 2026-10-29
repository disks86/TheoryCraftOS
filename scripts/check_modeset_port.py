#!/usr/bin/env python3
"""Checks the mode setting tables (src/Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.*) against the Linux driver's atombios.h and sources.

  check_modeset_port.py path/to/atombios.h path/to/atombios_crtc.c path/to/atombios_encoders.c

(kernel tag v6.12: drivers/gpu/drm/amd/include/atombios.h, drivers/gpu/drm/amd/amdgpu/atombios_crtc.c and atombios_encoders.c). It checks the master list index of every table AmdModeSet
calls (the DIGx encoder control is index 4, which the driver calls for every DIG), the byte layouts of the parameter structures against the packing in AmdModeSet.cpp, the constants (misc
bits, PLL ids, DIG encoder ids, encoder commands, transmitter actions, panel bits per colour) and that the driver's sources still do what the port copies: the tables the driver calls, the
V7 pixel clock deep colour ratios, the V4 DP link rate bits, the V4 transmitter DisplayPort clock source, the lane counts and the order of the DPMS enable sequence.

NOT checked: the order of the whole modeset in dce_v11_0.c (read by hand: AmdVegaM::set_mode follows dce_v11_0_crtc_prepare / encoder_prepare / crtc_mode_set / encoder_mode_set / crtc_commit /
encoder_commit).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_dplink_port as base

ROOT = base.ROOT
fail = base.fail


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    h = open(sys.argv[1]).read()
    crtc_c = open(sys.argv[2]).read()
    enc_c = open(sys.argv[3]).read()
    hpp = open(os.path.join(ROOT, 'AmdModeSet.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdModeSet.cpp')).read()

    m = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_COMMAND_TABLES\s*\{(.*?)\}\s*ATOM_MASTER_LIST_OF_COMMAND_TABLES', h, re.S)
    names = re.findall(r'USHORT\s+(\w+);', m.group(1))
    for n, c in (('DIGxEncoderControl', 'kDigxEncoderControl'), ('SetPixelClock', 'kSetPixelClock'), ('EnableDispPowerGating', 'kEnableDispPowerGating'), ('EnableScaler', 'kEnableScaler'),
                 ('BlankCRTC', 'kBlankCrtc'), ('EnableCRTC', 'kEnableCrtc'), ('SetCRTC_OverScan', 'kSetCrtcOverscan'), ('SelectCRTC_Source', 'kSelectCrtcSource'),
                 ('UpdateCRTC_DoubleBufferRegisters', 'kUpdateDoubleBuffer'), ('SetCRTC_UsingDTDTiming', 'kSetCrtcUsingDtdTiming'), ('DIG1TransmitterControl', 'kTransmitterControl')):
        k = re.search(c + r' = (\d+)', hpp)
        if not k or names.index(n) != int(k.group(1)):
            fail('%s: index %d, ours %s' % (n, names.index(n), k.group(1) if k else None))
    # the driver calls the same tables
    for pat, what in ((r'GetIndexIntoMasterTable\(COMMAND, DIGxEncoderControl\)', 'DIGxEncoderControl (enc)'), (r'GetIndexIntoMasterTable\(COMMAND, SetCRTC_UsingDTDTiming\)', 'SetCRTC_UsingDTDTiming (crtc)'),
                      (r'GetIndexIntoMasterTable\(COMMAND, EnableDispPowerGating\)', 'EnableDispPowerGating (crtc)'), (r'GetIndexIntoMasterTable\(COMMAND, UpdateCRTC_DoubleBufferRegisters\)', 'lock (crtc)'),
                      (r'GetIndexIntoMasterTable\(COMMAND, SetCRTC_OverScan\)', 'overscan (crtc)'), (r'GetIndexIntoMasterTable\(COMMAND, EnableScaler\)', 'scaler (crtc)')):
        src = enc_c if '(enc)' in what else crtc_c
        if not re.search(pat, src):
            fail('the driver no longer calls %s' % what)

    def layout(name, want, size=None):
        lay, total = base.struct_layout(h, name)
        for f, o in want.items():
            if lay.get(f, (None,))[0] != o:
                fail('%s: %s at %s, expected %d' % (name, f, lay.get(f), o))

    # (the layout parser counts the ATOM_MODE_MISC_INFO_ACCESS union as one byte and skips arrays, so only the offsets before them are checked; the sizes are by hand)
    layout('PIXEL_CLOCK_PARAMETERS_V7', {'ulPixelClock': 0, 'ucPpll': 4, 'ucTransmitterID': 5, 'ucEncoderMode': 6, 'ucMiscInfo': 7, 'ucCRTC': 8, 'ucDeepColorRatio': 9})
    layout('SET_CRTC_USING_DTD_TIMING_PARAMETERS', {'usH_Size': 0, 'usH_Blanking_Time': 2, 'usV_Size': 4, 'usV_Blanking_Time': 6, 'usH_SyncOffset': 8, 'usH_SyncWidth': 10, 'usV_SyncOffset': 12,
                                                    'usV_SyncWidth': 14, 'susModeMiscInfo': 16})
    layout('SET_CRTC_OVERSCAN_PARAMETERS', {'usOverscanRight': 0, 'usOverscanLeft': 2, 'usOverscanBottom': 4, 'usOverscanTop': 6, 'ucCRTC': 8})
    layout('ENABLE_SCALER_PARAMETERS', {'ucScaler': 0, 'ucEnable': 1, 'ucTVStandard': 2})
    layout('SELECT_CRTC_SOURCE_PARAMETERS_V2', {'ucCRTC': 0, 'ucEncoderID': 1, 'ucEncodeMode': 2})
    layout('SELECT_CRTC_SOURCE_PARAMETERS_V3', {'ucCRTC': 0, 'ucEncoderID': 1, 'ucEncodeMode': 2, 'ucDstBpc': 3})
    layout('ENABLE_CRTC_PARAMETERS', {'ucCRTC': 0, 'ucEnable': 1})
    layout('ENABLE_DISP_POWER_GATING_PARAMETERS_V2_1', {'ucDispPipeId': 0, 'ucEnable': 1})
    layout('BLANK_CRTC_PARAMETERS', {'ucCRTC': 0, 'ucBlanking': 1, 'usBlackColorRCr': 2, 'usBlackColorGY': 4, 'usBlackColorBCb': 6})
    layout('ENCODER_STREAM_SETUP_PARAMETERS_V5', {'ucDigId': 0, 'ucAction': 1, 'ucDigMode': 2, 'ucLaneNum': 3, 'ulPixelClock': 4, 'ucBitPerColor': 8, 'ucLinkRateIn270Mhz': 9})
    layout('ENCODER_GENERIC_CMD_PARAMETERS_V5', {'ucDigId': 0, 'ucAction': 1})
    layout('DP_PANEL_MODE_SETUP_PARAMETERS_V5', {'ucDigId': 0, 'ucAction': 1, 'ucPanelMode': 2})
    layout('DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6', {'ucPhyId': 0, 'ucAction': 1, 'ucDigMode': 2, 'ucLaneNum': 3, 'ulSymClock': 4, 'ucHPDSel': 8, 'ucDigEncoderSel': 9, 'ucConnObjId': 10})

    # our packing
    for pat, what in ((r'\{mode\.pixel_clock_khz \* 10, uint32_t\{out\.pll\} \| \(uint32_t\{out\.encoder_id\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(misc << 24\),\s*uint32_t\{out\.crtc\} \| \(deep << 8\), 0\}', 'SetPixelClock words'),
                      (r'uint32_t\{out\.crtc\} \| \(uint32_t\{dig_encoder_id\(out\.dig_encoder\)\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(bpc << 24\)', 'SelectCRTC_Source word'),
                      (r'uint32_t\{mode\.h_display\} \| \(uint32_t\(mode\.h_total - mode\.h_display\) << 16\)', 'DTD word 0'),
                      (r'uint32_t\{mode\.v_display\} \| \(uint32_t\(mode\.v_total - mode\.v_display\) << 16\)', 'DTD word 1'),
                      (r'uint32_t\(mode\.h_sync_start - mode\.h_display\) \| \(uint32_t\(mode\.h_sync_end - mode\.h_sync_start\) << 16\)', 'DTD word 2'),
                      (r'uint32_t\(mode\.v_sync_start - mode\.v_display\) \| \(uint32_t\(mode\.v_sync_end - mode\.v_sync_start\) << 16\)', 'DTD word 3'),
                      (r'misc,\s*uint32_t\{out\.crtc\}\}', 'DTD misc and CRTC'),
                      (r'words\[3\] = \{0, 0, uint32_t\{out\.crtc\}\}', 'overscan words'),
                      (r'words\[1\] = \{uint32_t\{out\.crtc\}\}', 'scaler word'),
                      (r'words\[2\] = \{uint32_t\{crtc\} \| \(uint32_t\{blank \? 1u : 0u\} << 8\), 0\}', 'BlankCRTC words'),
                      (r'\(pixel_clock & 0xFFFF\) \| \(config << 16\) \| \(uint32_t\{action\} << 24\), uint32_t\{mode_byte\} \| \(lanes << 8\) \| \(bpc << 16\) \| \(hpd << 24\)', 'encoder V3 / V4 words'),
                      (r'uint32_t\{out\.dig_encoder\} \| \(uint32_t\{action\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(lanes << 24\)', 'encoder V5 stream word 0'),
                      (r'words\[2\] = bpc \| \(\(out\.link_rate_khz / 27000\) << 8\)', 'encoder V5 stream word 2'),
                      (r'uint32_t\{out\.dig_encoder\} \| \(uint32_t\{action\} << 8\) \| \(uint32_t\{panel_mode\} << 16\)', 'encoder V5 panel mode word'),
                      (r'words\[0\] = uint32_t\{out\.dig_encoder\} \| \(uint32_t\{action\} << 8\);', 'encoder V5 command word'),
                      (r'\(symbol_clock & 0xFFFF\) \| \(uint32_t\{out\.dig_encoder\} << 16\) \| \(uint32_t\{action\} << 24\)', 'transmitter V1.5 word 0'),
                      (r'lanes \| \(uint32_t\{out\.connector_object_id\} << 8\) \| \(uint32_t\{out\.encoder_mode\} << 16\) \| \(config << 24\), \(1u << out\.dig_encoder\) \| \(uint32_t\{lane_set\} << 8\)', 'transmitter V1.5 words 1 and 2'),
                      (r'uint32_t\{out\.dig_encoder\} \| \(uint32_t\{action\} << 8\) \| \(third << 16\) \| \(lanes << 24\), symbol_clock,\s*hpd_sel \| \(\(1u << out\.dig_encoder\) << 8\) \| \(uint32_t\{out\.connector_object_id\} << 16\)', 'transmitter V1.6 words'),
                      (r'first \| \(config << 16\) \| \(uint32_t\{action\} << 24\), lanes', 'transmitter V4 words')):
        if not re.search(pat, cpp):
            fail('%s does not follow the layout' % what)
    # V4 config bits and V1.5 config bits
    t4 = base.bitfields(h, 'ATOM_DIG_TRANSMITTER_CONFIG_V4')
    if t4 != {'fDualLinkConnector': (0, 1), 'fCoherentMode': (1, 1), 'ucLinkSel': (2, 1), 'ucEncoderSel': (3, 1), 'ucRefClkSource': (4, 2), 'ucTransmitterSel': (6, 2)}:
        fail('ATOM_DIG_TRANSMITTER_CONFIG_V4 fields: %s' % t4)
    if not re.search(r'\(!dp && out\.dfp && out\.dual_link \? 1u : 0u\) \| \(coherent \? 2u : 0u\) \| \(out\.link_b \? 1u << 2 : 0u\) \| \(uint32_t\{out\.dig_encoder & 1u\} << 3\) \|\s*\(refclk << 4\) \| \(uint32_t\{out\.dig_encoder / 2u\} << 6\)', cpp):
        fail('the transmitter V4 config')
    t5 = base.bitfields(h, 'ATOM_DIG_TRANSMITTER_CONFIG_V5')
    if t5.get('ucCoherentMode') != (1, 1) or t5.get('ucPhyClkSrcId') != (2, 2) or t5.get('ucHPDSel') != (4, 3):
        fail('ATOM_DIG_TRANSMITTER_CONFIG_V5 fields: %s' % t5)
    if not re.search(r'\(coherent \? 2u : 0u\) \| \(uint32_t\{out\.pll & 3u\} << 2\) \| \(hpd_sel << 4\)', cpp):
        fail('the transmitter V1.5 config')
    f4 = base.bitfields(h, 'ATOM_DIG_ENCODER_CONFIG_V4')
    if f4.get('ucDPLinkRate') != (0, 2) or f4.get('ucDigSel') != (4, 3):
        fail('ATOM_DIG_ENCODER_CONFIG_V4 fields: %s' % f4)
    f3 = base.bitfields(h, 'ATOM_DIG_ENCODER_CONFIG_V3')
    if f3.get('ucDPLinkRate') != (0, 1) or f3.get('ucDigSel') != (4, 3):
        fail('ATOM_DIG_ENCODER_CONFIG_V3 fields: %s' % f3)
    if not re.search(r'rate \| \(uint32_t\{out\.dig_encoder\} << 4\)', cpp) or not re.search(r'out\.link_rate_khz == 540000 \? 2 : out\.link_rate_khz == 324000 \? 3 : out\.link_rate_khz == 270000 \? 1 : 0', cpp):
        fail('the encoder config (rate | DIG << 4, V4 rates 5.4 / 3.24 / 2.7 / 1.62)')
    for name, v in (('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_1_62GHZ', 0), ('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_2_70GHZ', 1), ('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_5_40GHZ', 2), ('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_3_24GHZ', 3),
                    ('ATOM_ENCODER_CONFIG_V3_DPLINKRATE_2_70GHZ', 1)):
        if base.define(h, name) != v:
            fail('%s is %s' % (name, base.define(h, name)))
    # the driver's own conditions
    if not re.search(r'if \(dp_clock == 540000\)\s+args\.v1\.ucConfig \|= ATOM_ENCODER_CONFIG_V4_DPLINKRATE_5_40GHZ;\s+else if \(dp_clock == 324000\)\s+args\.v1\.ucConfig \|= ATOM_ENCODER_CONFIG_V4_DPLINKRATE_3_24GHZ;\s+else if \(dp_clock == 270000\)', enc_c):
        fail('the driver\'s V4 link rate bits changed')
    if not re.search(r'ucLinkRateIn270Mhz = dp_clock / 27000', enc_c):
        fail('the driver\'s V5 link rate changed')
    if not re.search(r'args\.v4\.acConfig\.ucRefClkSource = ENCODER_REFCLK_SRC_DCPLL', enc_c) or not re.search(r'ENCODER_REFCLK_SRC_DCPLL\s+0x20|kEncoderRefclkDcpll = 2', cpp + str(base.define(h, 'ATOM_TRANSMITTER_CONFIG_V4_DCPLL'))):
        fail('the V4 DisplayPort PHY clock source (display clock PLL)')
    if base.define(h, 'ATOM_TRANSMITTER_CONFIG_V4_DCPLL') != 0x20:
        fail('ATOM_TRANSMITTER_CONFIG_V4_DCPLL')
    if not re.search(r'args\.v7\.ulPixelClock = cpu_to_le32\(clock \* 10\)', crtc_c) or not re.search(r'PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_2_1', crtc_c):
        fail('the driver\'s V7 pixel clock')
    if not re.search(r'args\.v5\.usSymClock = cpu_to_le16\(dp_clock / 10\)', enc_c) or not re.search(r'args\.v5\.ucDigEncoderSel = 1 << dig_encoder', enc_c):
        fail('the driver\'s V1.5 transmitter')
    if not re.search(r'args\.v4\.usPixelClock = cpu_to_le16\(\(amdgpu_encoder->pixel_clock / 2\) / 10\)', enc_c):
        fail('the driver\'s V4 dual link clock')
    # mode misc bits and constants
    for ours, name in (('kModeMiscHSyncPolarity', 'ATOM_HSYNC_POLARITY'), ('kModeMiscVSyncPolarity', 'ATOM_VSYNC_POLARITY'), ('kModeMiscCompositeSync', 'ATOM_COMPOSITESYNC'),
                       ('kModeMiscInterlace', 'ATOM_INTERLACE'), ('kModeMiscDoubleClock', 'ATOM_DOUBLE_CLOCK_MODE')):
        g = re.search(ours + r' = (0x[0-9A-Fa-f]+)', cpp)
        if not g or int(g.group(1), 16) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    if base.define(h, 'PIXEL_CLOCK_V7_MISC_DVI_DUALLINK_EN') != 8 or not re.search(r'kMiscDviDualLink = 0x01 << 3', cpp):
        fail('kMiscDviDualLink')
    for name, v in (('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_DIS', 0), ('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_5_4', 1), ('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_3_2', 2), ('PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_2_1', 3)):
        if base.define(h, name) != v:
            fail(name)
    if not re.search(r'out\.bits_per_color == 10 \? 1 : out\.bits_per_color == 12 \? 2 : out\.bits_per_color == 16 \? 3 : 0', cpp):
        fail('the deep colour ratio is not 10 bits 5:4, 12 bits 3:2, 16 bits 2:1')
    for ours, name in (('kPpll1', 'ATOM_PPLL1'), ('kPpll2', 'ATOM_PPLL2'), ('kPpll0', 'ATOM_PPLL0'), ('kPllInvalid', 'ATOM_PPLL_INVALID')):
        g = re.search(ours + r' = (0x[0-9A-Fa-f]+|\d+)', hpp)
        if not g or int(g.group(1), 0) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    ids = re.search(r'kIds\[\] = \{([^}]*)\}', cpp)
    want = [base.define(h, 'ASIC_INT_DIG%d_ENCODER_ID' % i) for i in range(1, 8)]
    got = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]+', ids.group(1))] if ids else None
    if got != want:
        fail('DIG encoder ids %s, atombios.h %s' % (got, want))
    for ours, name in (('kEncoderSetup', 'ATOM_ENCODER_CMD_SETUP'), ('kEncoderSetupPanelMode', 'ATOM_ENCODER_CMD_SETUP_PANEL_MODE'), ('kEncoderDpVideoOff', 'ATOM_ENCODER_CMD_DP_VIDEO_OFF'),
                       ('kEncoderDpVideoOn', 'ATOM_ENCODER_CMD_DP_VIDEO_ON'), ('kTransmitterDisable', 'ATOM_TRANSMITTER_ACTION_DISABLE'), ('kTransmitterEnable', 'ATOM_TRANSMITTER_ACTION_ENABLE'),
                       ('kTransmitterSetupVsEmph', 'ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH'), ('kModeDp', 'ATOM_ENCODER_MODE_DP'), ('kModeLvds', 'ATOM_ENCODER_MODE_LVDS'),
                       ('kModeDvi', 'ATOM_ENCODER_MODE_DVI'), ('kModeHdmi', 'ATOM_ENCODER_MODE_HDMI'), ('kModeDpMst', 'ATOM_ENCODER_MODE_DP_MST')):
        g = re.search(r'\b' + ours + r' = (0x[0-9A-Fa-f]+|\d+)', hpp)
        if not g or int(g.group(1), 0) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    for name, v in (('PANEL_BPC_UNDEFINE', 0), ('PANEL_6BIT_PER_COLOR', 1), ('PANEL_8BIT_PER_COLOR', 2), ('PANEL_10BIT_PER_COLOR', 3), ('PANEL_12BIT_PER_COLOR', 4), ('PANEL_16BIT_PER_COLOR', 5)):
        if base.define(h, name) != v:
            fail(name)
    for ours, name in (('kPanelModeExternalDp', 'DP_PANEL_MODE_EXTERNAL_DP_MODE'), ('kPanelModeInternalDp2', 'DP_PANEL_MODE_INTERNAL_DP2_MODE'), ('kPanelModeInternalDp1', 'DP_PANEL_MODE_INTERNAL_DP1_MODE')):
        g = re.search(ours + r' = (0x[0-9A-Fa-f]+|\d+)', hpp)
        if not g or int(g.group(1), 0) != base.define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, base.define(h, name)))
    if bad_flag():
        sys.exit(1)
    print('check_modeset_port: ok')


def bad_flag():
    return base.bad


if __name__ == '__main__':
    main()
