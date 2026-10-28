#!/usr/bin/env python3
"""Checks the audio endpoint part of the display engine port (AudioPins::set_enabled / write_speaker_allocation / write_sads / write_latency in AmdDce11.cpp, and AmdVegaM::enable_audio)
against the Linux driver.

  check_audio_port.py path/to/dce_v11_0.c path/to/dce_11_2_d.h path/to/dce_11_2_sh_mask.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/dce_v11_0.c, include/asic_reg/dce/dce_11_2_d.h and _sh_mask.h). It checks: the descriptor registers and the audio coding type of each (eld_reg_to_type in
dce_v11_0_audio_write_sad_regs) against ours; that the endpoint indexed registers (HOT_PLUG_CONTROL, CHANNEL_SPEAKER, RESPONSE_LIPSYNC, the descriptors) and the fields of them have the numbers the
generated AmdDce11Registers.inc has; the speaker allocation rules (DP_CONNECTION 0, HDMI_CONNECTION 1, the allocation or 5); the SAD loop's rules (the descriptor with the most channels, the stereo
rates of PCM only, the break for the other formats); the enable/disable writes; and the order of the audio steps in dce_v11_0_afmt_setmode against enable_audio's.

NOT checked: the coding type numbers (HDMI_AUDIO_CODING_TYPE_* in include/linux/hdmi.h, PCM 1 ... WMA Pro 14: from memory), and drm_edid.c (drm_edid_to_sad, drm_edid_to_speaker_allocation,
drm_parse_hdmi_vsdb_audio), which EdidAudio is written from memory of. KNOWN DIFFERENCE: dce_v11_0_audio_write_latency_fields sets VIDEO_LIPSYNC and then AUDIO_LIPSYNC starting from 0 again, so the
video latency never gets into the register (a slip in the driver); write_latency puts both in.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')
bad = False
# include/linux/hdmi.h enum hdmi_audio_coding_type (from memory)
CODING = {'STREAM': 0, 'PCM': 1, 'AC3': 2, 'MPEG1': 3, 'MP3': 4, 'MPEG2': 5, 'AAC_LC': 6, 'DTS': 7, 'ATRAC': 8, 'DSD': 9, 'EAC3': 10, 'DTS_HD': 11, 'MLP': 12, 'DST': 13, 'WMA_PRO': 14, 'CXT': 15}


def fail(text):
    global bad
    bad = True
    print('FAIL:', text)


def squash(t):
    return re.sub(r'\s+', ' ', t)


def func(src, name):
    m = re.search(r'\n(?:static )?[a-z_ ]*\b' + name + r'\(.*?\n\{(.*?)\n\}\n', src, re.S)
    if not m:
        sys.exit('function not found: ' + name)
    return m.group(1)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    dce, regs, masks = [open(a).read() for a in sys.argv[1:]]
    cpp = open(os.path.join(ROOT, 'AmdDce11.cpp')).read()
    vega = open(os.path.join(ROOT, 'AmdVegaM.cpp')).read()
    inc = open(os.path.join(ROOT, 'AmdDce11Registers.inc')).read()

    # the descriptor registers and their coding types
    sad = func(dce, 'dce_v11_0_audio_write_sad_regs')
    drv = re.findall(r'\{ ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR(\d+), HDMI_AUDIO_CODING_TYPE_(\w+) \}', sad)
    table = re.search(r'kDescriptorRegisters\[\] = \{(.*?)\};', cpp, re.S).group(1)
    ours = re.findall(r'\{ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR(\d+),\s*(\w+)\}', table)
    ours = [(n, str(CODING['PCM']) if t == 'kFormatPcm' else t) for n, t in ours]
    want = [(n, str(CODING[t])) for n, t in drv]
    if len(drv) != 12 or ours != want:
        fail('descriptor registers / coding types: driver %s, ours %s' % (want, ours))
    if not re.search(r'constexpr uint8_t kFormatPcm = %d;' % CODING['PCM'], cpp):
        fail('kFormatPcm is not the PCM coding type')

    # the SAD loop
    s = squash(sad)
    for needle in ('if (sad->channels > max_channels)', 'MAX_CHANNELS, sad->channels', 'DESCRIPTOR_BYTE_2, sad->byte2', 'SUPPORTED_FREQUENCIES, sad->freq', 'max_channels = sad->channels;',
                   'if (sad->format == HDMI_AUDIO_CODING_TYPE_PCM) stereo_freqs |= sad->freq; else break;', 'SUPPORTED_FREQUENCIES_STEREO, stereo_freqs', 'if (sad_count <= 0) return;', 'int max_channels = -1;'):
        if needle not in s:
            fail('the driver\'s SAD loop lost: ' + needle)
    c = squash(re.search(r'bool AudioPins::write_sads\(.*?\n\}\n', cpp, re.S).group(0))
    for needle in ('if (sad.channels > max_channels)', 'MAX_CHANNELS, sad.channels', 'DESCRIPTOR_BYTE_2, sad.byte2', 'SUPPORTED_FREQUENCIES, sad.freq', 'max_channels = sad.channels;',
                   'if (sad.format == kFormatPcm) stereo_freqs |= sad.freq; else break;', 'SUPPORTED_FREQUENCIES_STEREO, stereo_freqs', 'info.sad_count == 0) return false;', 'int max_channels = -1;'):
        if needle not in c:
            fail('write_sads lost: ' + needle)

    # speaker allocation
    spk = squash(func(dce, 'dce_v11_0_audio_write_speaker_allocation'))
    for needle in ('DP_CONNECTION, 0', 'HDMI_CONNECTION, 1', 'SPEAKER_ALLOCATION, sadb[0]', 'SPEAKER_ALLOCATION, 5'):
        if needle not in spk:
            fail('the driver\'s speaker allocation lost: ' + needle)
    c = squash(re.search(r'bool AudioPins::write_speaker_allocation\(.*?\n\}\n', cpp, re.S).group(0))
    for needle in ('DP_CONNECTION, 0', 'HDMI_CONNECTION, 1', 'info.speaker_allocation : kStereoSpeakers'):
        if needle not in c:
            fail('write_speaker_allocation lost: ' + needle)
    if not re.search(r'kStereoSpeakers = 5;', open(os.path.join(ROOT, 'AmdDce11.hpp')).read()):
        fail('kStereoSpeakers is not 5')

    # enable / disable: the whole HOT_PLUG_CONTROL register is the AUDIO_ENABLED bit or zero
    en = squash(func(dce, 'dce_v11_0_audio_enable'))
    if 'ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL, enable ? AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK : 0' not in en:
        fail('dce_v11_0_audio_enable changed')
    if 'ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL, enable ? AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK : 0' not in squash(cpp):
        fail('AudioPins::set_enabled is not the same write')

    # the latency (with the driver's slip noted in the docstring)
    lat = squash(func(dce, 'dce_v11_0_audio_write_latency_fields'))
    if 'VIDEO_LIPSYNC, connector->video_latency[interlace]' not in lat or 'AUDIO_LIPSYNC, connector->audio_latency[interlace]' not in lat or 'connector->latency_present[interlace]' not in lat:
        fail('the driver\'s latency fields changed')
    if not re.search(r'tmp = REG_SET_FIELD\(tmp, AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC, VIDEO_LIPSYNC, info\.video_latency\[which\]\);\s*tmp = REG_SET_FIELD\(tmp, AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC, AUDIO_LIPSYNC, info\.audio_latency\[which\]\);', cpp):
        fail('write_latency is not the two fields')

    # the numbers of the endpoint registers and fields, in the generated constants
    def hdr(name, src):
        m = re.search(r'#define\s+' + name + r'\s+(0x[0-9a-fA-F]+)', src)
        return int(m.group(1), 16) if m else None
    names = ['ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL', 'ixAZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER', 'ixAZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC'] + \
            ['ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR%d' % n for n in range(14) if n not in (8, 12)]       # (the DSD and DST registers are not written)
    masks_n = []
    for r, fields in (('AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL', ['AUDIO_ENABLED']), ('AZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER', ['SPEAKER_ALLOCATION', 'HDMI_CONNECTION', 'DP_CONNECTION']),
                      ('AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC', ['VIDEO_LIPSYNC', 'AUDIO_LIPSYNC']),
                      ('AZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0', ['MAX_CHANNELS', 'DESCRIPTOR_BYTE_2', 'SUPPORTED_FREQUENCIES', 'SUPPORTED_FREQUENCIES_STEREO'])):
        for f in fields:
            masks_n += ['%s__%s_MASK' % (r, f)] + ([] if f == 'AUDIO_ENABLED' else ['%s__%s__SHIFT' % (r, f)])
    for n in names:
        h = hdr(n, regs)
        m = re.search(r'constexpr uint32_t ' + n + r' = (0x[0-9A-Fa-f]+);', inc)
        if h is None or not m or int(m.group(1), 16) != h:
            fail('%s: header %s, generated %s' % (n, h, m.group(1) if m else None))
    for n in masks_n:
        h = hdr(n, masks)
        m = re.search(r'constexpr uint32_t ' + n + r' = (0x[0-9A-Fa-f]+);', inc)
        if h is None or not m or int(m.group(1), 16) != h:
            fail('%s: header %s, generated %s' % (n, h, m.group(1) if m else None))

    # the order of the steps: afmt_setmode disables first, writes the speaker allocation, selects the pin, the SADs, the latency, enables last
    setmode = func(dce, 'dce_v11_0_afmt_setmode')
    steps = ['dce_v11_0_audio_enable(adev, dig->afmt->pin, false)', 'dce_v11_0_audio_write_speaker_allocation(encoder)', 'dce_v11_0_afmt_audio_select_pin(encoder)',
             'dce_v11_0_audio_write_sad_regs(encoder)', 'dce_v11_0_audio_write_latency_fields(encoder, mode)', 'dce_v11_0_audio_enable(adev, dig->afmt->pin, true)']
    at = [setmode.find(x) for x in steps]
    if -1 in at or at != sorted(at):
        fail('the audio steps of dce_v11_0_afmt_setmode are not in the expected order: %s' % at)
    body = re.search(r'bool AmdVegaM::enable_audio\(const AtomConnectors::Connector& connector, ConstByteView edid, const AudioMode& mode\) \{(.*?)\n\}\n', vega, re.S).group(1)
    mine = ['pins.set_enabled(pin, false)', 'pins.write_speaker_allocation(pin, info)', 'format.select_pin(block, pin)', 'pins.write_sads(pin, info)',
            'pins.write_latency(pin, info, mode.interlaced)', 'pins.set_enabled(pin, true)']       # (the packets and the InfoFrame in between are scripts/check_hdmi_port.py's)
    at = [body.find(x) for x in mine]
    if -1 in at or at != sorted(at):
        fail('enable_audio\'s steps are not in the driver\'s order: %s' % at)
    if bad:
        sys.exit(1)
    print('check_audio_port: ok')


main()
