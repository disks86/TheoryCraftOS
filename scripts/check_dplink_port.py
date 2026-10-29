#!/usr/bin/env python3
"""Checks the DisplayPort link training port (src/Hardware/GraphicsProcessingUnit/AMD/AmdDpSource.* and DpLinkTrain.*) against the Linux driver's atombios.h.

  check_dplink_port.py path/to/atombios.h

(kernel tag v6.12: drivers/gpu/drm/amd/include/atombios.h). It checks the master list index of DIG1EncoderControl, DIG2EncoderControl and UNIPHYTransmitterControl, the byte layout of
DIG_ENCODER_CONTROL_PARAMETERS_V3 and _V4, DIG_TRANSMITTER_CONTROL_PARAMETERS_V4 and _V1_5 (computed from the header, bit fields and unions included) against the packing in AmdDpSource.cpp,
the ATOM_ENCODER_CMD_DP_LINK_TRAINING_* actions, ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH, the link rate and DIG select bits of the encoder configuration and the transmitter
configuration bits of V4 and V1.5, and the PHY ids against the DIG numbers.

NOT checked: atombios_encoders.c / amdgpu_atombios_dp.c (what the driver puts in these parameters for training, the order and conditions of the training algorithm), drm_dp.h and
drm_dp_helper.c (the DPCD register numbers and bits, the delays, drm_dp_clock_recovery_ok and drm_dp_channel_eq_ok's use of the status): all from memory.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')
bad = False


def fail(text):
    global bad
    bad = True
    print('FAIL:', text)


def struct_layout(h, name):
    """{field: (byte offset, size)} of `typedef struct _NAME {...} NAME;` with unions overlaying their members and bit field structs taking the size of their type;
    bit fields as {field: (bit offset, width)} under 'bits:<struct>' for the nested struct types."""
    m = re.search(r'typedef struct\s+_' + name + r'\s*\{(.*?)\}\s*' + name + r'\s*;', h, re.S)
    if not m:
        sys.exit('struct not found: ' + name)
    body = re.sub(r'#if ATOM_BIG_ENDIAN.*?#else', '', m.group(1), flags=re.S)
    body = re.sub(r'#endif', '', body)
    body = re.sub(r'//[^\n]*', '', body)
    sizes = {'USHORT': 2, 'UCHAR': 1, 'ULONG': 4}
    layout, at = {}, 0

    def member_size(decl):
        d = decl.strip()
        t = re.match(r'(\w+)\s+(\w+)', d)
        if not t:
            return None
        if t.group(1) in sizes:
            return sizes[t.group(1)]
        return 1 if t.group(1).startswith('ATOM_') else None

    pos = 0
    while pos < len(body):
        u = re.match(r'\s*union\s*\{(.*?)\}\s*;', body[pos:], re.S)
        if u:
            members = [x for x in u.group(1).split(';') if x.strip()]
            width = 0
            for x in members:
                x = x.strip()
                sub = re.match(r'(\w+)\s+(\w+)', x)
                if not sub:
                    continue
                size = member_size(x)
                if size is None:
                    size = 2      # (a nested struct: ATOM_DP_VS_MODE_V4 is two bytes)
                layout[sub.group(2)] = (at, size)
                width = max(width, size)
            at += width
            pos += u.end()
            continue
        d = re.match(r'\s*(\w+)\s+(\w+)\s*;', body[pos:])
        if not d:
            break
        size = member_size(d.group(0))
        layout[d.group(2)] = (at, size if size else 1)
        at += size if size else 1
        pos += d.end()
    return layout, at


def bitfields(h, name):
    """{field: (bit offset, width)} of a little-endian bit field struct `_NAME`."""
    m = re.search(r'typedef struct\s+_' + name + r'\s*\{(.*?)\}\s*' + name + r'\s*;', h, re.S)
    body = m.group(1)
    le = re.search(r'#else(.*?)#endif', body, re.S).group(1)
    out, at = {}, 0
    for f, w in re.findall(r'UCHAR\s+(\w+)\s*:\s*(\d+)\s*;', le):
        out[f] = (at, int(w))
        at += int(w)
    return out


def define(h, name):
    m = re.search(r'#define\s+' + name + r'\s+(0x[0-9a-fA-F]+|\d+)', h)
    return int(m.group(1), 0) if m else None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    h = open(sys.argv[1]).read()
    hpp = open(os.path.join(ROOT, 'AmdDpSource.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdDpSource.cpp')).read()

    m = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_COMMAND_TABLES\s*\{(.*?)\}\s*ATOM_MASTER_LIST_OF_COMMAND_TABLES', h, re.S)
    names = re.findall(r'USHORT\s+(\w+);', m.group(1))
    for n, c in (('DIG1EncoderControl', 'kEncoder1Table'), ('DIG2EncoderControl', 'kEncoder2Table'), ('DIG1TransmitterControl', 'kTransmitterTable')):
        k = re.search(c + r' = (\d+)', hpp)
        if not k or names.index(n) != int(k.group(1)):
            fail('%s: index %d, ours %s' % (n, names.index(n), k.group(1) if k else None))
    if not re.search(r'#define UNIPHYTransmitterControl\s+DIG1TransmitterControl', h):
        fail('UNIPHYTransmitterControl is no longer DIG1TransmitterControl')

    # the encoder control V3 and V4 (8 bytes): usPixelClock 0, config 2, action 3, mode 4, lane number 5, bit per colour 6, HPD id (V4) / reserved 7
    for v, last in (('V3', 'ucReserved'), ('V4', 'ucHPD_ID')):
        lay, size = struct_layout(h, 'DIG_ENCODER_CONTROL_PARAMETERS_' + v)
        want = {'usPixelClock': 0, 'ucAction': 3, 'ucEncoderMode': 4, 'ucLaneNum': 5, 'ucBitPerColor': 6, last: 7}
        for f, o in want.items():
            if f not in lay or lay[f][0] != o:
                fail('encoder %s: %s at %s, expected %d' % (v, f, lay.get(f), o))
        cfg = lay.get('acConfig') or lay.get('ucConfig')
        if size != 8 or not cfg or cfg[0] != 2:
            fail('encoder %s: %d bytes, config at %s' % (v, size, cfg))
    # ours: pixel clock | config << 16 | action << 24; mode | lanes << 8 | bpc << 16 | hpd << 24
    if not re.search(r'\(pixel_clock & 0xFFFF\) \| \(config << 16\) \| \(uint32_t\{action\} << 24\)', cpp):
        fail('the first encoder word is not pixel clock | config << 16 | action << 24')
    if not re.search(r'uint32_t\{kEncoderModeDp\} \| \(config_\.lanes << 8\) \| \(uint32_t\{config_\.bits_per_color\} << 16\) \| \(hpd << 24\)', cpp):
        fail('the second encoder word is not mode | lanes << 8 | bpc << 16 | hpd << 24')
    # the configuration bit fields of V4: link rate in bits 1:0, DIG select in 6:4
    f4 = bitfields(h, 'ATOM_DIG_ENCODER_CONFIG_V4')
    if f4.get('ucDPLinkRate') != (0, 2) or f4.get('ucDigSel') != (4, 3):
        fail('ATOM_DIG_ENCODER_CONFIG_V4 fields: %s' % f4)
    f3 = bitfields(h, 'ATOM_DIG_ENCODER_CONFIG_V3')
    if f3.get('ucDPLinkRate') != (0, 1) or f3.get('ucDigSel') != (4, 3):
        fail('ATOM_DIG_ENCODER_CONFIG_V3 fields: %s' % f3)
    if not re.search(r'static_cast<uint32_t>\(rate\) \| \(uint32_t\{config_\.dig_encoder\} << 4\)', cpp):
        fail('the encoder config is not rate | DIG << 4')
    for name, v in (('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_1_62GHZ', 0), ('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_2_70GHZ', 1), ('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_5_40GHZ', 2), ('ATOM_ENCODER_CONFIG_V4_DPLINKRATE_3_24GHZ', 3)):
        if define(h, name) != v:
            fail('%s is %s' % (name, define(h, name)))
    for rate, bits in ((162000, 0), (270000, 1), (540000, 2), (324000, 3)):
        if not re.search(r'case %d: return (%d;|crev >= 4 \? %d : -1;)' % (rate, bits, bits), cpp):
            fail('rate %d is not %d in rate_bits' % (rate, bits))
    # the actions
    for ours, name in (('kCmdStart', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_START'), ('kCmdPattern1', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1'), ('kCmdPattern2', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN2'),
                       ('kCmdPattern3', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN3'), ('kCmdComplete', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE')):
        g = re.search(r'\b' + ours + r' = (0x[0-9A-Fa-f]+)', hpp)
        if not g or int(g.group(1), 16) != define(h, name):
            fail('%s is %s, %s is %s' % (ours, g.group(1) if g else None, name, define(h, name)))
    g = re.search(r'kActionSetupVsEmph = (\d+)', hpp)
    if not g or int(g.group(1)) != define(h, 'ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH'):
        fail('kActionSetupVsEmph')
    if define(h, 'ATOM_ENCODER_MODE_DP') != 0 or define(h, 'ATOM_TRANSMITTER_DIGMODE_V5_DP') != 0:
        fail('the DP modes are not 0')

    # the transmitter V4: asMode (lane select 0, lane set 1), config 2, action 3, lane number 4
    lay, size = struct_layout(h, 'DIG_TRANSMITTER_CONTROL_PARAMETERS_V4')
    for f, o in (('ucAction', 3), ('ucLaneNum', 4)):
        if lay.get(f, (None,))[0] != o:
            fail('transmitter V4: %s at %s, expected %d' % (f, lay.get(f), o))
    cfg = lay.get('acConfig') or lay.get('ucConfig')
    mode = lay.get('asMode')
    vs = struct_layout(h, 'ATOM_DP_VS_MODE_V4')[0]
    if not cfg or cfg[0] != 2 or not mode or mode[0] != 0 or vs.get('ucLaneSel', (None,))[0] != 0 or vs.get('ucLaneSet', (None,))[0] != 1:
        fail('transmitter V4: config %s asMode %s lane select/set %s' % (cfg, mode, vs))
    if not re.search(r'0u \| \(uint32_t\{lane_set\} << 8\) \| \(config << 16\) \| \(uint32_t\{kActionSetupVsEmph\} << 24\), config_\.lanes', cpp):
        fail('the transmitter V4 words are not lane select 0 | lane set << 8 | config << 16 | action << 24; lanes')
    t4 = bitfields(h, 'ATOM_DIG_TRANSMITTER_CONFIG_V4')
    if t4 != {'fDualLinkConnector': (0, 1), 'fCoherentMode': (1, 1), 'ucLinkSel': (2, 1), 'ucEncoderSel': (3, 1), 'ucRefClkSource': (4, 2), 'ucTransmitterSel': (6, 2)}:
        fail('ATOM_DIG_TRANSMITTER_CONFIG_V4 fields: %s' % t4)
    if not re.search(r'\(uint32_t\{config_\.dig_encoder / 2u\} << 6\) \| \(uint32_t\{config_\.reference_clock_source & 3u\} << 4\) \| \(uint32_t\{config_\.dig_encoder & 1u\} << 3\) \|\s*\(config_\.link_b \? 1u << 2 : 0u\)', cpp):
        fail('the transmitter V4 config is not transmitter << 6 | reference clock << 4 | front end << 3 | link << 2')

    # the transmitter V1.5 (12 bytes): usSymClock 0, ucPhyId 2, ucAction 3, ucLaneNum 4, ucConnObjId 5, ucDigMode 6, config 7, ucDigEncoderSel 8, ucDPLaneSet 9
    lay, size = struct_layout(h, 'DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5')
    want = {'usSymClock': 0, 'ucPhyId': 2, 'ucAction': 3, 'ucLaneNum': 4, 'ucConnObjId': 5, 'ucDigMode': 6, 'ucDigEncoderSel': 8, 'ucDPLaneSet': 9}
    for f, o in want.items():
        if lay.get(f, (None,))[0] != o:
            fail('transmitter V1.5: %s at %s, expected %d' % (f, lay.get(f), o))
    cfg = lay.get('asConfig') or lay.get('ucConfig')
    if size != 12 or not cfg or cfg[0] != 7:
        fail('transmitter V1.5: %d bytes, config at %s' % (size, cfg))
    if not re.search(r'\(config_\.rate_khz / 10\) & 0xFFFF\) \| \(uint32_t\{config_\.dig_encoder\} << 16\) \| \(uint32_t\{kActionSetupVsEmph\} << 24\)', cpp) or \
            not re.search(r'config_\.lanes \| \(uint32_t\{config_\.connector_object_id\} << 8\) \| \(uint32_t\{kDigModeDp\} << 16\) \| \(config << 24\)', cpp) or \
            not re.search(r'\(1u << config_\.dig_encoder\) \| \(uint32_t\{lane_set\} << 8\)', cpp):
        fail('the transmitter V1.5 words do not follow the layout')
    t5 = bitfields(h, 'ATOM_DIG_TRANSMITTER_CONFIG_V5')
    if t5.get('ucCoherentMode') != (1, 1) or t5.get('ucPhyClkSrcId') != (2, 2) or t5.get('ucHPDSel') != (4, 3):
        fail('ATOM_DIG_TRANSMITTER_CONFIG_V5 fields: %s' % t5)
    if not re.search(r'\(hpd_sel << 4\) \| \(uint32_t\{config_\.reference_clock_source & 3u\} << 2\)', cpp):
        fail('the transmitter V1.5 config is not HPD select << 4 | PLL << 2')
    # the PHY id is the DIG number: UNIPHYA..G are 0..6 and DIG0..6 (ATOM_ENCODER_CONFIG_V5_DIGn_ENCODER) are 0..6; the DIG front end select is a bit per DIG
    for i, l in enumerate('ABCDEFG'):
        if define(h, 'ATOM_PHY_ID_UNIPHY' + l) != i or define(h, 'ATOM_ENCODER_CONFIG_V5_DIG%d_ENCODER' % i) != i:
            fail('PHY id / DIG %s' % l)
        if define(h, 'ATOM_TRANMSITTER_V5__DIG%s_SEL' % l) != 1 << i:
            fail('DIG%s select is not bit %d' % (l, i))
    if bad:
        sys.exit(1)
    print('check_dplink_port: ok')


main()
