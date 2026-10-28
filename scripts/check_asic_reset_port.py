#!/usr/bin/env python3
"""Checks the full reset of the Vega M (src/Hardware/GraphicsProcessingUnit/AMD/AmdAsicReset.*) against the Linux driver it was ported from.

  check_asic_reset_port.py path/to/vi.c path/to/amdgpu_device.c path/to/amdgpu.h path/to/amdgpu_atombios.c path/to/atombios.h path/to/bif_5_0_d.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/, drivers/gpu/drm/amd/include/ and include/asic_reg/bif/). It checks the steps and their order in vi_asic_pci_config_reset, the
magic value and the configuration dword, the BIOS scratch register and the hung flag, CONFIG_MEMSIZE, and that the VEGAM is not among the chips the driver resets with BACO.
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


def squash(text):
    return re.sub(r'\s+', ' ', text)


def body(src, name):
    m = re.search(r'\n(?:static )?\w[\w \*\n]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', src)
    if not m:
        sys.exit('function not found: ' + name)
    depth, i = 0, m.end() - 1
    for j in range(i, len(src)):
        depth += (src[j] == '{') - (src[j] == '}')
        if depth == 0:
            return squash(src[i:j + 1])
    sys.exit('unbalanced: ' + name)


def define(src, name):
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+)L?', src)
    if not m:
        sys.exit('not in header: ' + name)
    return int(m.group(1), 16)


def order(text, needles):
    at = 0
    for n in needles:
        i = text.find(n, at)
        if i < 0:
            return False
        at = i + len(n)
    return True


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    vi, device, amdgpu_h, atombios_c, atombios_h, bif_d = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AmdAsicReset.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdAsicReset.cpp')).read()

    reset = body(vi, 'vi_asic_pci_config_reset')
    if not order(reset, ['amdgpu_atombios_scratch_regs_engine_hung(adev, true)', 'pci_clear_master', 'amdgpu_device_pci_config_reset', 'udelay(100)', 'RREG32(mmCONFIG_MEMSIZE) != 0xffffffff',
                         'pci_set_master', 'amdgpu_atombios_scratch_regs_engine_hung(adev, false)']):
        fail('vi_asic_pci_config_reset steps changed')
    mine = squash(cpp[cpp.index('AmdAsicReset::Result AmdAsicReset::pci_config_reset'):])
    if not order(mine, ['scratch_engine_hung(registers, true)', 'set_bus_master(pci, function, false)', 'pci.write32(function, kPciResetOffset, kPciResetData)', 'wait_us(clock, 100)',
                        'registers.read(kConfigMemsize) != 0xffffffff', 'set_bus_master(pci, function, true)', 'scratch_engine_hung(registers, false)']):
        fail('AmdAsicReset::pci_config_reset steps differ from the driver\'s')
    # the clear-master happens only on success in the driver: the set_master is inside the if
    if not re.search(r'if \(RREG32\(mmCONFIG_MEMSIZE\) != 0xffffffff\) \{ /\* enable BM \*/ pci_set_master', reset):
        fail('the driver no longer sets bus mastering only when the card answered')

    # the magic value and the dword
    m = re.search(r'kPciResetData = (0x[0-9a-fA-F]+)', hpp)
    if not m or int(m.group(1), 16) != define(amdgpu_h, 'AMDGPU_ASIC_RESET_DATA'):
        fail('the reset value')
    m = re.search(r'kPciResetOffset = (0x[0-9a-fA-F]+)', hpp)
    if not m or 'pci_write_config_dword(adev->pdev, 0x7c, AMDGPU_ASIC_RESET_DATA)' not in squash(device) or int(m.group(1), 16) != 0x7c:
        fail('the configuration dword')
    # the scratch register: mmBIOS_SCRATCH_0 + 3, the hung flag
    m = re.search(r'kBiosScratch3 = (0x[0-9a-fA-F]+) \+ 3', hpp)
    if not m or int(m.group(1), 16) != define(bif_d, 'mmBIOS_SCRATCH_0'):
        fail('BIOS_SCRATCH_0')
    if 'adev->bios_scratch_reg_offset = mmBIOS_SCRATCH_0' not in atombios_c:
        fail('the scratch register offset of the driver changed')
    s = body(atombios_c, 'amdgpu_atombios_scratch_regs_engine_hung')
    if 'RREG32(adev->bios_scratch_reg_offset + 3)' not in s or 'ATOM_S3_ASIC_GUI_ENGINE_HUNG' not in s:
        fail('amdgpu_atombios_scratch_regs_engine_hung changed')
    m = re.search(r'kAtomS3AsicGuiEngineHung = (0x[0-9a-fA-F]+)', hpp)
    if not m or int(m.group(1), 16) != define(atombios_h, 'ATOM_S3_ASIC_GUI_ENGINE_HUNG'):
        fail('ATOM_S3_ASIC_GUI_ENGINE_HUNG')
    m = re.search(r'kConfigMemsize = (0x[0-9a-fA-F]+)', hpp)
    if not m or int(m.group(1), 16) != define(bif_d, 'mmCONFIG_MEMSIZE'):
        fail('mmCONFIG_MEMSIZE')
    # the timeout: adev->usec_timeout default 100000 us
    if 'kDefaultTimeoutUs = 100000' not in hpp:
        fail('timeout')

    # the VEGAM gets this reset, not BACO
    method = body(vi, 'vi_asic_reset_method')
    baco_cases = re.findall(r'case (CHIP_\w+):', method.split('baco_reset = amdgpu_dpm_is_baco_supported')[0])
    if 'CHIP_VEGAM' in baco_cases:
        fail('the driver resets the VEGAM with BACO: this port is not what it does')
    if 'vi_asic_pci_config_reset' not in body(vi, 'vi_asic_reset'):
        fail('vi_asic_reset no longer falls back to the PCI configuration reset')
    if bad:
        sys.exit(1)
    print('the card reset matches the driver: steps, reset value and dword, scratch flag, CONFIG_MEMSIZE, and the VEGAM is not a BACO chip (%s)' % ', '.join(baco_cases))


main()
