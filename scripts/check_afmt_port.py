#!/usr/bin/env python3
"""Checks the audio format block part of the display engine port (AudioFormat in src/Hardware/GraphicsProcessingUnit/AMD/AmdDce11.*) against the Linux driver.

  check_afmt_port.py path/to/dce_v11_0.c path/to/vid.h path/to/dce_11_2_d.h path/to/dce_11_2_sh_mask.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/dce_v11_0.c, vid.h, include/asic_reg/dce/dce_11_2_d.h and _sh_mask.h). It checks dce_v11_0_afmt_audio_select_pin (a read-modify-write of
AFMT_AUDIO_SRC_CONTROL at the block's offset, setting AFMT_AUDIO_SRC_SELECT to the pin's id), the register number and the field's mask and shift, the register offset of each DIG block
(dig_offsets, DIGn_REGISTER_OFFSET), that the audio blocks are tied to the DIG encoders (afmt[i]->offset = dig_offsets[i]), and the number of DIGs of CHIP_VEGAM (6, so kBlocks).
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


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    dce, vid, regs, masks = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AmdDce11.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdDce11.cpp')).read()
    inc = open(os.path.join(ROOT, 'AmdDce11Registers.inc')).read()

    body = re.search(r'dce_v11_0_afmt_audio_select_pin\(.*?\n\{(.*?)\n\}', dce, re.S).group(1)
    body = re.sub(r'\s+', ' ', body)
    if 'tmp = RREG32(mmAFMT_AUDIO_SRC_CONTROL + dig->afmt->offset); tmp = REG_SET_FIELD(tmp, AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT, dig->afmt->pin->id); WREG32(mmAFMT_AUDIO_SRC_CONTROL + dig->afmt->offset, tmp);' not in body:
        fail('dce_v11_0_afmt_audio_select_pin is not a read-modify-write of AFMT_AUDIO_SRC_SELECT with the pin id')
    if not re.search(r'tmp = REG_SET_FIELD\(tmp, AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT, pin\);', cpp) or 'r_.read(mmAFMT_AUDIO_SRC_CONTROL + kDigOffsets[block])' not in cpp or \
            'r_.write(mmAFMT_AUDIO_SRC_CONTROL + kDigOffsets[block], tmp)' not in cpp:
        fail('AudioFormat::select_pin is not the same read-modify-write')

    # the register and the field, in the generated constants and in the headers
    reg = int(re.search(r'#define mmAFMT_AUDIO_SRC_CONTROL\s+(0x[0-9a-fA-F]+)', regs).group(1), 16)
    mask = int(re.search(r'#define AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT_MASK\s+(0x[0-9a-fA-F]+)', masks).group(1), 16)
    shift = int(re.search(r'#define AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT__SHIFT\s+(0x[0-9a-fA-F]+)', masks).group(1), 16)
    for name, want in (('mmAFMT_AUDIO_SRC_CONTROL', reg), ('AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT_MASK', mask), ('AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT__SHIFT', shift)):
        m = re.search(r'constexpr uint32_t ' + name + r' = (0x[0-9A-Fa-f]+);', inc)
        if not m or int(m.group(1), 16) != want:
            fail('%s is %s in AmdDce11Registers.inc, 0x%x in the headers' % (name, m.group(1) if m else None, want))
    # every AFMT register of DIG n is the first one's plus the DIG's offset
    for n in range(0, 9):
        m = re.search(r'#define mmDIG%d_AFMT_AUDIO_SRC_CONTROL\s+(0x[0-9a-fA-F]+)' % n, regs)
        off = re.search(r'#define DIG%d_REGISTER_OFFSET\s+\((0x[0-9a-fA-F]+) - (0x[0-9a-fA-F]+)\)' % n, vid)
        if not m or not off or int(m.group(1), 16) != reg + int(off.group(1), 16) - int(off.group(2), 16):
            fail('mmDIG%d_AFMT_AUDIO_SRC_CONTROL does not equal the first one plus DIG%d_REGISTER_OFFSET' % (n, n))

    # the offsets we use (blocks 0..kBlocks - 1) are the first dig_offsets entries
    table = re.search(r'dig_offsets\[\] = \{(.*?)\};', dce, re.S).group(1)
    names = re.findall(r'DIG(\d)_REGISTER_OFFSET', table)
    if names != [str(i) for i in range(9)]:
        fail('dig_offsets is not DIG0..DIG8 in order: %s' % names)
    ours = re.search(r'kDigOffsets\[AudioFormat::kBlocks\] = \{(.*?)\};', cpp, re.S).group(1)
    mine = [int(a, 16) - int(b, 16) for a, b in re.findall(r'(0x[0-9a-fA-F]+) - (0x[0-9a-fA-F]+)', ours)]
    k = int(re.search(r'kBlocks = (\d+);', hpp).group(1))
    want = []
    for n in range(k):
        off = re.search(r'#define DIG%d_REGISTER_OFFSET\s+\((0x[0-9a-fA-F]+) - (0x[0-9a-fA-F]+)\)' % n, vid)
        want.append(int(off.group(1), 16) - int(off.group(2), 16))
    if mine != want:
        fail('our DIG offsets %s, the driver %s' % (mine, want))

    # the audio blocks are one per DIG encoder, and CHIP_VEGAM has 6 of them
    if not re.search(r'adev->mode_info\.afmt\[i\]->offset = dig_offsets\[i\];', dce):
        fail('afmt[i]->offset is no longer dig_offsets[i]')
    m = re.search(r'case CHIP_POLARIS10:\s*case CHIP_VEGAM:\s*adev->mode_info\.num_hpd = 6;\s*adev->mode_info\.num_dig = (\d+);', dce)
    if not m or int(m.group(1)) != k:
        fail('num_dig of CHIP_VEGAM is %s, kBlocks %d' % (m.group(1) if m else None, k))
    # the pin ids are the endpoint numbers (pin[i].id = i) and there are 8 of them
    if not re.search(r'adev->mode_info\.audio\.pin\[i\]\.id = i;', dce) or not re.search(r'case CHIP_POLARIS10:\s*case CHIP_VEGAM:\s*adev->mode_info\.audio\.num_pins = 8;', dce):
        fail('the audio pin ids or their number changed')
    if bad:
        sys.exit(1)
    print('check_afmt_port: ok')


main()
