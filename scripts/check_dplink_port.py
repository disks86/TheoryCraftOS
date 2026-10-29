#!/usr/bin/env python3
"""Checks the DisplayPort link training port (src/Hardware/GraphicsProcessingUnit/AMD/Dpcd.*, DpLinkTrain.*, AmdDpSource.*) against the Linux sources.

  check_dplink_port.py path/to/drm_dp.h path/to/atombios_dp.c path/to/drm_dp_helper.c path/to/atombios.h

(kernel tag v6.12: include/drm/display/drm_dp.h, drivers/gpu/drm/amd/amdgpu/atombios_dp.c, drivers/gpu/drm/display/drm_dp_helper.c, drivers/gpu/drm/amd/include/atombios.h). It checks the
DPCD register addresses and bits Dpcd.hpp names against drm_dp.h, the training pattern and action numbers (ATOM_ENCODER_CMD_DP_LINK_TRAINING_*), and that the constants of the algorithm
(five clock recovery tries, the channel equalization limit, the 400 us waits, the 100 us / 400 us default delays and the 4 ms unit, the 100 us of a DPCD 1.4 sink) are those of the
driver's. The table parameter words are checked by check_modeset_port.py.

NOT checked: the order of the training steps (read from the source by hand: DpLinkTrain.cpp follows amdgpu_atombios_dp_link_train).
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
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    dp_h = open(sys.argv[1]).read()
    dp_c = open(sys.argv[2]).read()
    helper_c = open(sys.argv[3]).read()
    atom_h = open(sys.argv[4]).read()
    hpp = open(os.path.join(ROOT, 'Dpcd.hpp')).read()
    train_hpp = open(os.path.join(ROOT, 'DpLinkTrain.hpp')).read()
    train_cpp = open(os.path.join(ROOT, 'DpLinkTrain.cpp')).read()
    source_hpp = open(os.path.join(ROOT, 'AmdDpSource.hpp')).read()

    def theirs(name):
        m = re.search(r'#\s*define\s+' + name + r'\s+(\(?\s*(?:0x[0-9a-fA-F]+|\d+)(?:\s*<<\s*\d+)?\s*\)?)', dp_h)
        if not m:
            fail('%s not found in drm_dp.h' % name)
            return None
        return eval(m.group(1).replace('(', '').replace(')', ''))

    def ours(name):
        m = re.search(r'\b' + name + r' = (0x[0-9A-Fa-f]+|\d+)', hpp)
        if not m:
            fail('%s not found in Dpcd.hpp' % name)
            return None
        return int(m.group(1), 0)

    pairs = (('kRevision', 'DP_DPCD_REV'), ('kMaxLinkRate', 'DP_MAX_LINK_RATE'), ('kMaxLaneCount', 'DP_MAX_LANE_COUNT'), ('kMaxDownspread', 'DP_MAX_DOWNSPREAD'), ('kNorp', 'DP_NORP'),
             ('kDownstreamPortPresent', 'DP_DOWNSTREAMPORT_PRESENT'), ('kMainLinkChannelCoding', 'DP_MAIN_LINK_CHANNEL_CODING'), ('kDownstreamPortCount', 'DP_DOWN_STREAM_PORT_COUNT'),
             ('kSupportedLinkRates', 'DP_SUPPORTED_LINK_RATES'), ('kLaneCountSet', 'DP_LANE_COUNT_SET'), ('kSinkCount', 'DP_SINK_COUNT'), ('kDeviceServiceIrqVector', 'DP_DEVICE_SERVICE_IRQ_VECTOR'),
             ('kLaneStatus', 'DP_LANE0_1_STATUS'), ('kEdpRevision', 'DP_EDP_DPCD_REV'), ('kLinkBwSet', 'DP_LINK_BW_SET'), ('kLinkRateSet', 'DP_LINK_RATE_SET'),
             ('kTrainingPatternSet', 'DP_TRAINING_PATTERN_SET'), ('kTrainingLane0Set', 'DP_TRAINING_LANE0_SET'), ('kDownspreadCtrl', 'DP_DOWNSPREAD_CTRL'), ('kSetPower', 'DP_SET_POWER'),
             ('kTrainingAuxRdInterval', 'DP_TRAINING_AUX_RD_INTERVAL'), ('kTestRequest', 'DP_TEST_REQUEST'), ('kTestResponse', 'DP_TEST_RESPONSE'), ('kTestEdidChecksum', 'DP_TEST_EDID_CHECKSUM'),
             ('kEdpConfigurationSet', 'DP_EDP_CONFIGURATION_SET'), ('kEdpConfigurationCap', 'DP_EDP_CONFIGURATION_CAP'),
             ('kLaneCountEnhancedFraming', 'DP_LANE_COUNT_ENHANCED_FRAME_EN'), ('kSpreadAmp05', 'DP_SPREAD_AMP_0_5'), ('kPowerD0', 'DP_SET_POWER_D0'), ('kPowerD3', 'DP_SET_POWER_D3'),
             ('kTrainVoltageMask', 'DP_TRAIN_VOLTAGE_SWING_MASK'), ('kTrainMaxSwingReached', 'DP_TRAIN_MAX_SWING_REACHED'), ('kTrainPreEmphasisMask', 'DP_TRAIN_PRE_EMPHASIS_MASK'),
             ('kTrainMaxPreEmphasisReached', 'DP_TRAIN_MAX_PRE_EMPHASIS_REACHED'), ('kIrqAutomatedTest', 'DP_AUTOMATED_TEST_REQUEST'), ('kIrqContentProtection', 'DP_CP_IRQ'),
             ('kIrqSinkSpecific', 'DP_SINK_SPECIFIC_IRQ'), ('kTestLinkTraining', 'DP_TEST_LINK_TRAINING'), ('kTestVideoPattern', 'DP_TEST_LINK_VIDEO_PATTERN'),
             ('kTestEdidRead', 'DP_TEST_LINK_EDID_READ'), ('kTestPhyPattern', 'DP_TEST_LINK_PHY_TEST_PATTERN'), ('kTestAck', 'DP_TEST_ACK'), ('kTestNak', 'DP_TEST_NAK'),
             ('kTestEdidChecksumWrite', 'DP_TEST_EDID_CHECKSUM_WRITE'), ('kEdp14', 'DP_EDP_14'))
    for o, t in pairs:
        a, b = ours(o), theirs(t)
        if a is not None and b is not None and a != b:
            fail('%s is 0x%x, %s is 0x%x' % (o, a, t, b))
    # the training patterns are DP_TRAINING_PATTERN_SET's low bits: 1, 2, 3 (the disable is 0)
    for i, name in ((0, 'kTrainingPatternDisable'), (1, 'kTrainingPattern1'), (2, 'kTrainingPattern2'), (3, 'kTrainingPattern3')):
        if ours(name) != i:
            fail(name)
    # the voltage and pre-emphasis levels: level 3 of each, as the algorithm's maximum
    if not re.search(r'kVoltageMax = 3, kPreEmphasisMax = 3 << 3', hpp):
        fail('the maximum voltage or pre-emphasis')
    if not re.search(r'DP_VOLTAGE_MAX\s+DP_TRAIN_VOLTAGE_SWING_LEVEL_3', dp_c) or not re.search(r'DP_PRE_EMPHASIS_MAX\s+DP_TRAIN_PRE_EMPH_LEVEL_3', dp_c):
        fail('the driver no longer uses level 3 as the maximum')
    # the algorithm's constants
    if not re.search(r'if \(dp_info->tries == 5\)', dp_c) or not re.search(r'kClockRecoveryTries = 5', train_hpp):
        fail('clock recovery tries (5)')
    if not re.search(r'if \(dp_info->tries > 5\)', dp_c) or not re.search(r'if \(tries > kChannelEqTries\)', train_cpp) or not re.search(r'kChannelEqTries = 5', train_hpp):
        fail('channel equalization tries (> 5)')
    if len(re.findall(r'udelay\(400\)', dp_c)) != 2 or len(re.findall(r'source\.delay_us\(400\)', train_cpp)) != 2:
        fail('the 400 us waits (after pattern 1 is set, before the pattern is switched off)')
    if not re.search(r'rd_interval == 0\)\s+return 100;', helper_c) or not re.search(r'rd_interval == 0\)\s+return 400;', helper_c):
        fail('the default delays (100 us, 400 us)')
    if not re.search(r'kDefaultClockRecoveryUs = 100, kDefaultChannelEqUs = 400, kIntervalUnitUs = 4000', train_hpp):
        fail('the default delays and the 4 ms unit')
    if not re.search(r'DP_DPCD_REV_14\)\s+delay_us = 100;', helper_c) or not re.search(r'revision >= 0x14\) t\.source\.delay_us\(100\)', train_cpp):
        fail('the 100 us of a DPCD 1.4 sink')
    if not re.search(r'usleep_range\(1000, 2000\)', dp_c) or not re.search(r'source\.delay_us\(1000\)', train_cpp):
        fail('the 1 ms after the power state')
    if not re.search(r'tmp \|= DP_LANE_COUNT_ENHANCED_FRAME_EN', dp_c) or not re.search(r'DP_EDP_CONFIGURATION_SET, 1\)', dp_c):
        fail('the lane count / alternate scrambler writes')
    if not re.search(r'link_rates\[3\] = \{ 162000, 270000, 540000 \}', dp_c) or not re.search(r'kRates\[\] = \{162000, 270000, 540000\}', train_cpp):
        fail('the link rates')
    # the training actions
    for ours_name, theirs_name in (('kCmdStart', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_START'), ('kCmdPattern1', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1'),
                                   ('kCmdPattern2', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN2'), ('kCmdPattern3', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN3'),
                                   ('kCmdComplete', 'ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE')):
        g = re.search(r'\b' + ours_name + r' = (0x[0-9A-Fa-f]+)', source_hpp)
        if not g or int(g.group(1), 16) != define(atom_h, theirs_name):
            fail('%s is %s, %s is %s' % (ours_name, g.group(1) if g else None, theirs_name, define(atom_h, theirs_name)))
    if bad:
        sys.exit(1)
    print('check_dplink_port: ok')


if __name__ == "__main__":
    main()
