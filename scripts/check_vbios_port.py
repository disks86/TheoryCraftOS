#!/usr/bin/env python3
"""Checks the video BIOS finder (src/Hardware/GraphicsProcessingUnit/AMD/AmdVbios.*) against the Linux driver it was ported from.

  check_vbios_port.py path/to/amdgpu_bios.c path/to/atombios.h path/to/vi.c path/to/smu_7_1_2_d.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/amdgpu_bios.c, vi.c; drivers/gpu/drm/amd/include/atombios.h; include/asic_reg/smu/smu_7_1_2_d.h). It checks the signature
constants, the order of the sources for a discrete card, the checks of check_atom_bios, the layout of the VFCT table and its image header, the register numbers and the read protocol of
vi_read_bios_from_rom.
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


def squash(t):
    return re.sub(r'\s+', ' ', t)


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
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+)', src)
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
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    bios_c, atombios_h, vi_c, smu_d = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AmdVbios.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdVbios.cpp')).read()

    if '#define AMD_VBIOS_SIGNATURE " 761295520"' not in bios_c or "constexpr char kVbiosSignature[] = \" 761295520\"" not in cpp:
        fail('the AMD signature')
    if 'kSignatureOffset = 0x30' not in hpp or '#define AMD_VBIOS_SIGNATURE_OFFSET 0x30' not in bios_c:
        fail('the signature offset')
    if '#define AMD_IS_VALID_VBIOS(p) ((p)[0] == 0x55 && (p)[1] == 0xAA)' not in bios_c or 'image[0] != 0x55 || image[1] != 0xAA' not in cpp:
        fail('the valid signature')
    if '#define AMD_VBIOS_LENGTH(p) ((p)[2] << 9)' not in bios_c or 'uint32_t length = uint32_t{h[2]} << 9;' not in cpp:
        fail('the image length byte')
    # the order of the sources for a discrete card: ATRM, VFCT, VRAM (not ported), platform (not ported), ROM BAR, ROM registers
    dgpu = body(bios_c, 'amdgpu_get_bios_dgpu')
    if not order(dgpu, ['amdgpu_atrm_get_bios', 'amdgpu_acpi_vfct_bios', 'amdgpu_read_bios_from_vram', 'amdgpu_read_platform_bios', 'amdgpu_read_bios(adev)', 'amdgpu_read_bios_from_rom', 'amdgpu_read_disabled_bios']):
        fail('amdgpu_get_bios_dgpu changed its order')
    mine = body(cpp[cpp.index('AmdVbios::Source AmdVbios::find'):], 'AmdVbios::find') if False else squash(cpp[cpp.index('AmdVbios::Source AmdVbios::find'):])
    if not order(mine, ['read_from_atrm', 'find_in_vfct', 'read_from_rom_bar', 'read_from_rom_registers']):
        fail('AmdVbios::find is not in the order of the sources')
    # the ATRM method: 256 KiB in pages of 4096, ATRM(offset, length), stopping at the first short page, then check_atom_bios
    if '#define ATRM_BIOS_PAGE 4096' not in bios_c or 'kAtrmBiosPage = 4096, kAtrmBiosBytes = 256 * 1024' not in hpp:
        fail('the ATRM page and image sizes')
    atrm = body(bios_c, 'amdgpu_atrm_get_bios')
    call = body(bios_c, 'amdgpu_atrm_call')
    if 'int size = 256 * 1024;' not in atrm or 'for (i = 0; i < size / ATRM_BIOS_PAGE; i++)' not in atrm or 'if (ret < ATRM_BIOS_PAGE) break;' not in atrm or \
            not order(atrm, ['amdgpu_atrm_call(atrm_handle, adev->bios, (i * ATRM_BIOS_PAGE), ATRM_BIOS_PAGE)', 'check_atom_bios(adev->bios, size)']) or 'acpi_get_handle(dhandle, "ATRM", &atrm_handle)' not in atrm:
        fail('amdgpu_atrm_get_bios changed')
    if not order(call, ['atrm_arg_elements[0].integer.value = offset', 'atrm_arg_elements[1].integer.value = len', 'acpi_evaluate_object(atrm_handle, NULL, &atrm_arg, &buffer)', 'memcpy(bios+offset, obj->buffer.pointer, obj->buffer.length)']):
        fail('amdgpu_atrm_call changed')
    mine_atrm = squash(cpp[cpp.index('bool AmdVbios::read_from_atrm'):cpp.index('AmdVbios::Source AmdVbios::find')])
    if not order(mine_atrm, ['arguments[0].integer = uint64_t{page} * kAtrmBiosPage', 'arguments[1].integer = kAtrmBiosPage', 'aml.evaluate(method, arguments, 2, &result)',
                             'if (result.length < kAtrmBiosPage) break;', 'is_atom_bios(image.view())']) or 'page < kAtrmBiosBytes / kAtrmBiosPage' not in mine_atrm or "name[0] == 'A' && name[1] == 'T' && name[2] == 'R' && name[3] == 'M'" not in cpp:
        fail('read_from_atrm does not follow amdgpu_atrm_get_bios')
    # check_atom_bios
    chk = body(bios_c, 'check_atom_bios')
    for token in ('size < 0x49', 'bios[0x48] | (bios[0x49] << 8)', 'tmp = bios_header_start + 4', '"ATOM"', '"MOTA"'):
        if token not in chk:
            fail('check_atom_bios changed: ' + token)
    for token in ('image.size() < 0x49', 'image[0x48] | (image[0x49] << 8)', 'uint64_t{header} + 4', 'kAtom', 'kMota'):
        if token not in cpp:
            fail('AmdVbios::is_atom_bios lacks ' + token)
    # the VFCT table
    for token, mine_token in (('vhdr->PCIBus == adev->pdev->bus->number', 'bus == function.bus'), ('vhdr->PCIDevice == PCI_SLOT(adev->pdev->devfn)', 'device == function.device'),
                              ('vhdr->PCIFunction == PCI_FUNC(adev->pdev->devfn)', 'fn == function.function'), ('vhdr->VendorID == adev->pdev->vendor', 'vendor == vendor_id'),
                              ('vhdr->DeviceID == adev->pdev->device', 'dev_id == device_id'), ('offset += sizeof(VFCT_IMAGE_HEADER)', 'offset += kVfctImageHeaderBytes'),
                              ('offset += vhdr->ImageLength', 'offset += length')):
        if token not in squash(bios_c) or mine_token not in cpp:
            fail('VFCT walk: ' + token)
    vfct = re.search(r'typedef struct \{\s*AMD_ACPI_DESCRIPTION_HEADER SHeader;\s*UCHAR TableUUID\[16\];[^\n]*\n\s*ULONG VBIOSImageOffset;[^\n]*\n\s*ULONG Lib1ImageOffset;[^\n]*\n\s*ULONG Reserved\[4\];[^\n]*\n\}UEFI_ACPI_VFCT;', atombios_h)
    if not vfct or 'kVfctHeaderBytes = 0x4C' not in cpp or 'kVfctVbiosOffsetField = 0x34' not in cpp:
        fail('the VFCT header layout')
    hdr = re.search(r'typedef struct \{\s*ULONG\s+PCIBus;[^\n]*\n\s*ULONG\s+PCIDevice;[^\n]*\n\s*ULONG\s+PCIFunction;[^\n]*\n\s*USHORT\s+VendorID;[^\n]*\n\s*USHORT\s+DeviceID;[^\n]*\n\s*USHORT\s+SSVID;[^\n]*\n\s*USHORT\s+SSID;[^\n]*\n\s*ULONG\s+Revision;[^\n]*\n\s*ULONG\s+ImageLength;[^\n]*\n\}VFCT_IMAGE_HEADER;', atombios_h)
    if not hdr or 'kVfctImageHeaderBytes = 28' not in cpp or 'le32(table, header_at + 24)' not in cpp or 'le16(table, header_at + 12)' not in cpp or 'le16(table, header_at + 14)' not in cpp:
        fail('the VFCT image header layout')
    # the ROM registers
    rom = body(vi_c, 'vi_read_bios_from_rom')
    if not order(rom, ['WREG32(mmSMC_IND_INDEX_11, ixROM_INDEX)', 'WREG32(mmSMC_IND_DATA_11, 0)', 'WREG32(mmSMC_IND_INDEX_11, ixROM_DATA)', 'RREG32(mmSMC_IND_DATA_11)']):
        fail('vi_read_bios_from_rom changed')
    mine_rom = squash(cpp[cpp.index('bool AmdVbios::read_from_rom_registers'):])
    if not order(mine_rom, ['registers.write(kSmcIndex, kRomIndex)', 'registers.write(kSmcData, 0)', 'registers.write(kSmcIndex, kRomData)', 'registers.read(kSmcData)']):
        fail('read_from_rom_registers does not follow vi_read_bios_from_rom')
    for name, mine_name in (('mmSMC_IND_INDEX_11', 'kSmcIndex'), ('mmSMC_IND_DATA_11', 'kSmcData'), ('ixROM_INDEX', 'kRomIndex'), ('ixROM_DATA', 'kRomData')):
        m = re.search(mine_name + r' = (0x[0-9a-fA-F]+)', hpp)
        if not m or int(m.group(1), 16) != define(smu_d, name):
            fail('%s: ours %s, header %#x' % (name, m.group(1) if m else None, define(smu_d, name)))
    # amdgpu_read_bios_from_rom: the header is AMD_VBIOS_SIGNATURE_END + 1 bytes, the length aligned to 4
    fr = body(bios_c, 'amdgpu_read_bios_from_rom')
    if 'header[AMD_VBIOS_SIGNATURE_END+1]' not in fr or 'ALIGN(len, 4)' not in fr or 'kSignatureOffset + sizeof(kVbiosSignature) + 1' not in cpp or '(length + 3) & ~3u' not in cpp:
        fail('the header and length handling of amdgpu_read_bios_from_rom')
    if bad:
        sys.exit(1)
    print('AmdVbios matches the driver: signatures, the order of the sources, check_atom_bios, the VFCT layout and walk, the ROM register protocol')


main()
