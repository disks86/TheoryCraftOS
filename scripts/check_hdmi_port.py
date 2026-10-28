#!/usr/bin/env python3
"""Checks the audio clock and packet setup of the display engine port (AudioFormat::setup_packets / enable_channels / start in AmdDce11.cpp and AmdVegaM::enable_audio) against the
Linux driver's dce_v11_0_afmt_setmode.

  check_hdmi_port.py path/to/dce_v11_0.c

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/dce_v11_0.c). It checks that the registers are written in the driver's order (the driver's function, with dce_v11_0_audio_set_dto,
dce_v11_0_afmt_update_ACR and dce_v11_0_afmt_update_avi_infoframe expanded, against ours with the endpoint steps as markers), the constants (the CRC control value, the ramp registers, the DTO
phase, the info frame line, the audio packets per line, the channel enable mask, the channel numbers, the deep colour settings by bits per component and the ACR source rule), and the
AVI InfoFrame words (the checksum and data bytes into AFMT_AVI_INFO0..2, the last two bytes and the version into INFO3).

NOT checked: amdgpu_afmt.c (the predefined N/CTS rows and the calculation: HdmiInfoframeTest checks them against the HDMI formula instead), drm_hdmi_avi_infoframe_from_display_mode and
hdmi_avi_infoframe_pack (drm_edid.c, hdmi.c: the AVI InfoFrame bytes and the CEA mode table are from memory).
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


def func(src, name):
    m = re.search(r'\n(?:static )?[a-z_ ]*\b' + name + r'\(.*?\n\{(.*?)\n\}\n', src, re.S)
    if not m:
        sys.exit('function not found: ' + name)
    return m.group(1)


MARKERS = {'dce_v11_0_audio_write_speaker_allocation': 'SPEAKER', 'dce_v11_0_afmt_audio_select_pin': 'SELECT', 'dce_v11_0_audio_write_sad_regs': 'SADS',
           'dce_v11_0_audio_write_latency_fields': 'LATENCY'}


def driver_sequence(dce):
    setmode = func(dce, 'dce_v11_0_afmt_setmode')
    inner = {'dce_v11_0_audio_set_dto': func(dce, 'dce_v11_0_audio_set_dto'), 'dce_v11_0_afmt_update_ACR': func(dce, 'dce_v11_0_afmt_update_ACR'),
             'dce_v11_0_afmt_update_avi_infoframe': func(dce, 'dce_v11_0_afmt_update_avi_infoframe')}
    seq = []
    for m in re.finditer(r'WREG32\((mm\w+)|(dce_v11_0_\w+)\(', setmode):
        if m.group(1):
            seq.append(m.group(1))
        elif m.group(2) in inner:
            seq += re.findall(r'WREG32\((mm\w+)', inner[m.group(2)])
        elif m.group(2) in MARKERS:
            seq.append(MARKERS[m.group(2)])
        elif m.group(2) == 'dce_v11_0_audio_enable':
            seq.append('ENABLE')
    return seq


def our_sequence(cpp, vega):
    def writes(name):
        body = re.search(r'bool AudioFormat::' + name + r'\(.*?\n\}\n', cpp, re.S).group(0)
        return re.findall(r'r_\.write\((mm\w+)', body)
    flow = re.search(r'bool AmdVegaM::enable_audio\(const AtomConnectors::Connector& connector, ConstByteView edid, const AudioMode& mode\) \{(.*?)\n\}\n', vega, re.S).group(1)
    seq = []
    for m in re.finditer(r'(pins\.set_enabled|format\.setup_packets|pins\.write_speaker_allocation|format\.enable_channels|format\.select_pin|pins\.write_sads|pins\.write_latency|format\.start)\(', flow):
        c = m.group(1)
        if c == 'pins.set_enabled':
            seq.append('ENABLE')
        elif c == 'format.setup_packets':
            seq += writes('setup_packets')
        elif c == 'pins.write_speaker_allocation':
            seq.append('SPEAKER')
        elif c == 'format.enable_channels':
            seq += writes('enable_channels')
        elif c == 'format.select_pin':
            seq.append('SELECT')
        elif c == 'pins.write_sads':
            seq.append('SADS')
        elif c == 'pins.write_latency':
            seq.append('LATENCY')
        else:
            seq += writes('start')
    return seq


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dce = open(sys.argv[1]).read()
    cpp = open(os.path.join(ROOT, 'AmdDce11.cpp')).read()
    vega = open(os.path.join(ROOT, 'AmdVegaM.cpp')).read()

    # the order: the driver's write-by-write sequence (the register writes done with a read before them are the same single step) against ours
    d, o = driver_sequence(dce), our_sequence(cpp, vega)
    # (the driver also enables audio at the start, which the sequence has as the first ENABLE: disabled prior to setting up)
    if d != o:
        for i, (a, b) in enumerate(zip(d, o)):
            if a != b:
                fail('the sequences differ at step %d: driver %s, ours %s' % (i, a, b))
                break
        else:
            fail('the sequences differ in length: driver %d, ours %d' % (len(d), len(o)))

    setmode = re.sub(r'\s+', ' ', func(dce, 'dce_v11_0_afmt_setmode'))
    ours = re.sub(r'\s+', ' ', cpp)
    checks = [
        ('WREG32(mmAFMT_AUDIO_CRC_CONTROL + dig->afmt->offset, 0x1000);', 'kAudioCrcControl = 0x1000;'),
        ('WREG32(mmAFMT_RAMP_CONTROL0 + dig->afmt->offset, 0x00FFFFFF);', 'kRamp0 = 0x00FFFFFF'),
        ('WREG32(mmAFMT_RAMP_CONTROL1 + dig->afmt->offset, 0x007FFFFF);', 'kRamp1 = 0x007FFFFF'),
        ('WREG32(mmAFMT_RAMP_CONTROL2 + dig->afmt->offset, 0x00000001);', 'kRamp2 = 1'),
        ('WREG32(mmAFMT_RAMP_CONTROL3 + dig->afmt->offset, 0x00000001);', 'kRamp3 = 1'),
        ('HDMI_AUDIO_INFO_LINE, 2', 'kInfoframeLine = 2'),
        ('HDMI_AVI_INFO_LINE, 2', 'kInfoframeLine = 2'),
        ('HDMI_AUDIO_PACKETS_PER_LINE, 3', 'kAudioPacketsPerLine = 3'),
        ('HDMI_AUDIO_DELAY_EN, 1', 'HDMI_AUDIO_DELAY_EN, 1)'),
        ('AFMT_60958_CS_UPDATE, 1', 'AFMT_60958_CS_UPDATE, 1)'),
        ('AFMT_AUDIO_SAMPLE_SEND, 1', 'AFMT_AUDIO_SAMPLE_SEND, 1)'),
        ('HDMI_GC_SEND, 1', 'HDMI_GC_SEND, 1)'), ('HDMI_GC_CONT, 1', 'HDMI_GC_CONT, 1)'), ('HDMI_NULL_SEND, 1', 'HDMI_NULL_SEND, 1)'),
        ('HDMI_AUDIO_INFO_SEND, 1', 'HDMI_AUDIO_INFO_SEND, 1)'), ('HDMI_AUDIO_INFO_CONT, 1', 'HDMI_AUDIO_INFO_CONT, 1)'),
        ('HDMI_AVI_INFO_SEND, 1', 'HDMI_AVI_INFO_SEND, 1)'), ('HDMI_AVI_INFO_CONT, 1', 'HDMI_AVI_INFO_CONT, 1)'), ('AFMT_AUDIO_INFO_UPDATE, 1', 'AFMT_AUDIO_INFO_UPDATE, 1)'),
        ('WREG32(mmHDMI_GC + dig->afmt->offset, 0);', 'r_.write(mmHDMI_GC + off, 0);'),
        ('(0xff << AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE__SHIFT)', '0xffu << AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE__SHIFT'),
        ('HDMI_DEEP_COLOR_ENABLE, 1); tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 1);', 'case 10: tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_ENABLE, 1); tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 1);'),
        ('HDMI_DEEP_COLOR_ENABLE, 1); tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 2);', 'case 12: tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_ENABLE, 1); tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 2);'),
        ('HDMI_DEEP_COLOR_ENABLE, 0); tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 0);', 'HDMI_DEEP_COLOR_ENABLE, 0); tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 0);'),
        ('if (bpc > 8) /* clear SW CTS value */ tmp = REG_SET_FIELD(tmp, HDMI_ACR_PACKET_CONTROL, HDMI_ACR_SOURCE, 0); else /* select SW CTS value */ tmp = REG_SET_FIELD(tmp, HDMI_ACR_PACKET_CONTROL, HDMI_ACR_SOURCE, 1);',
         'HDMI_ACR_SOURCE, bpc > 8 ? 0 : 1)'),
    ]
    for drv, mine in checks:
        if drv not in setmode and drv not in re.sub(r'\s+', ' ', func(dce, 'dce_v11_0_afmt_update_ACR')) and drv not in re.sub(r'\s+', ' ', dce):
            fail('the driver no longer has: ' + drv)
        if mine not in ours:
            fail('ours no longer has: ' + mine)
    for n, v in enumerate((3, 4, 5, 6, 7, 8)):
        label = ('CHANNEL_NUMBER_%d, %d' % (n + 2, v))
        if label not in setmode and label.replace('CHANNEL_NUMBER_', 'CS_CHANNEL_NUMBER_') not in setmode and ('AFMT_60958_CS_' + label) not in setmode:
            fail('the driver lost ' + label)
        if ('AFMT_60958_CS_' + label) not in ours:
            fail('ours lost ' + label)
    for label in ('CS_CHANNEL_NUMBER_L, 1', 'CS_CHANNEL_NUMBER_R, 2'):
        if ('AFMT_60958_' + label) not in setmode and label not in setmode:
            fail('the driver lost ' + label)
        if label not in ours:
            fail('ours lost ' + label)
    # the DTO
    dto = re.sub(r'\s+', ' ', func(dce, 'dce_v11_0_audio_set_dto'))
    for needle in ('u32 dto_phase = 24 * 1000;', 'u32 dto_modulo = clock;', 'DCCG_AUDIO_DTO0_SOURCE_SEL, amdgpu_crtc->crtc_id', 'WREG32(mmDCCG_AUDIO_DTO0_PHASE, dto_phase);', 'WREG32(mmDCCG_AUDIO_DTO0_MODULE, dto_modulo);'):
        if needle not in dto:
            fail('the driver\'s audio_set_dto lost: ' + needle)
    for needle in ('kDtoPhase = 24 * 1000', 'DCCG_AUDIO_DTO0_SOURCE_SEL, crtc)', 'r_.write(mmDCCG_AUDIO_DTO0_PHASE, kDtoPhase)', 'r_.write(mmDCCG_AUDIO_DTO0_MODULE, clock_khz)'):
        if needle not in ours:
            fail('ours lost: ' + needle)
    # the AVI words
    avi = re.sub(r'\s+', ' ', func(dce, 'dce_v11_0_afmt_update_avi_infoframe'))
    for needle in ('uint8_t *frame = buffer + 3;', 'uint8_t *header = buffer;', 'frame[0x0] | (frame[0x1] << 8) | (frame[0x2] << 16) | (frame[0x3] << 24)', 'frame[0x4] | (frame[0x5] << 8) | (frame[0x6] << 16) | (frame[0x7] << 24)',
                   'frame[0x8] | (frame[0x9] << 8) | (frame[0xA] << 16) | (frame[0xB] << 24)', 'frame[0xC] | (frame[0xD] << 8) | (header[1] << 24)'):
        if needle not in avi:
            fail('the driver\'s AVI words changed: ' + needle)
    # ours: frame = buffer + 3 -> word(3), word(7), word(11), then buffer[15] | buffer[16] << 8 | buffer[1] << 24
    for needle in ('word(3)', 'word(7)', 'word(11)', 'uint32_t{avi[15]} | uint32_t{avi[16]} << 8 | uint32_t{avi[1]} << 24'):
        if needle not in ours:
            fail('ours lost: ' + needle)
    if bad:
        sys.exit(1)
    print('check_hdmi_port: ok')


main()
