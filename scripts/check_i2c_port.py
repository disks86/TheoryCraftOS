#!/usr/bin/env python3
"""Checks the DDC bus lookup (src/Hardware/GraphicsProcessingUnit/AMD/AtomI2c.*) and the I2C record read by AtomConnectors against the Linux driver.

  check_i2c_port.py path/to/amdgpu_atombios.c path/to/atombios.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/amdgpu_atombios.c, drivers/gpu/drm/amd/include/atombios.h). It checks the index of GPIO_I2C_Info in the master list of data tables, the layout
of ATOM_GPIO_I2C_ASSIGMENT (the registers and shifts and which is which) and of ATOM_I2C_RECORD, the id bits, how the bus record is made from an entry, and that the lookup compares the
access byte. NOT checked: amdgpu_i2c.c (the bit-banging callbacks) and i2c-algo-bit.c, which were not at hand when AmdDdc was written.
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


def struct_fields(src, name):
    m = re.search(r'typedef struct\s+_?' + name + r'\b[^{]*\{(.*?)\}\s*' + name + r'\s*;', src, re.S)
    if not m:
        sys.exit('struct not found: ' + name)
    fields, at = {}, 0
    for line in m.group(1).split(';'):
        line = re.sub(r'//[^\n]*', '', line).strip()
        if not line:
            continue
        t = re.match(r'(\w+)\s+(\w+)(\[(\w*)\])?', line)
        if not t:
            sys.exit('field not understood in %s: %s' % (name, line))
        size = {'USHORT': 2, 'UCHAR': 1, 'ATOM_I2C_ID_CONFIG_ACCESS': 1, 'ATOM_COMMON_RECORD_HEADER': 2, 'ATOM_I2C_ID_CONFIG': 1}.get(t.group(1), 0)
        fields[t.group(2)] = at
        at += size
    return fields, at


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    atombios_c, atombios_h = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AtomI2c.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AtomI2c.cpp')).read()
    conn = open(os.path.join(ROOT, 'AtomConnectors.cpp')).read()

    m = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_DATA_TABLES\s*\{(.*?)\}\s*ATOM_MASTER_LIST_OF_DATA_TABLES', atombios_h, re.S)
    names = re.findall(r'USHORT\s+(\w+);', m.group(1))
    k = re.search(r'kDataGpioI2cInfo = (\d+)', cpp)
    if not k or names.index('GPIO_I2C_Info') != int(k.group(1)):
        fail('GPIO_I2C_Info index %d, ours %s' % (names.index('GPIO_I2C_Info'), k.group(1) if k else None))

    # the entry: eight registers (clock mask, enable, Y, A; data mask, enable, Y, A), the id, eight shifts in the same order, two reserved bytes
    f, size = struct_fields(atombios_h, 'ATOM_GPIO_I2C_ASSIGMENT')
    regs = ['usClkMaskRegisterIndex', 'usClkEnRegisterIndex', 'usClkY_RegisterIndex', 'usClkA_RegisterIndex', 'usDataMaskRegisterIndex', 'usDataEnRegisterIndex', 'usDataY_RegisterIndex', 'usDataA_RegisterIndex']
    shifts = ['ucClkMaskShift', 'ucClkEnShift', 'ucClkY_Shift', 'ucClkA_Shift', 'ucDataMaskShift', 'ucDataEnShift', 'ucDataY_Shift', 'ucDataA_Shift']
    if [f[r] for r in regs] != [0, 2, 4, 6, 8, 10, 12, 14] or f['sucI2cId'] != 16 or [f[x] for x in shifts] != list(range(17, 25)) or size != 27:
        fail('ATOM_GPIO_I2C_ASSIGMENT layout: %s size %d' % (f, size))
    if 'kAssignmentBytes = 27' not in cpp or 'kIdAt = 16, kShiftsAt = 17' not in cpp or 'kTableHeaderBytes = 4' not in cpp:
        fail('our entry layout')
    # which register is which in the bus record
    rec = squash(re.search(r'amdgpu_atombios_get_bus_rec_for_i2c_gpio\(.*?\n}\n', atombios_c, re.S).group(0))
    pairs = [('mask_clk_reg', 'usClkMaskRegisterIndex'), ('mask_data_reg', 'usDataMaskRegisterIndex'), ('en_clk_reg', 'usClkEnRegisterIndex'), ('en_data_reg', 'usDataEnRegisterIndex'),
             ('y_clk_reg', 'usClkY_RegisterIndex'), ('y_data_reg', 'usDataY_RegisterIndex'), ('a_clk_reg', 'usClkA_RegisterIndex'), ('a_data_reg', 'usDataA_RegisterIndex')]
    order_of = {n: i for i, n in enumerate(regs)}
    for field, src in pairs:
        if 'i2c.%s = le16_to_cpu(gpio->%s)' % (field, src) not in rec:
            fail('driver: %s from %s changed' % (field, src))
        if 'bus.%s = reg[%d]' % (field, order_of[src]) not in cpp:
            fail('ours: %s should be reg[%d]' % (field, order_of[src]))
    spairs = [('mask_clk_mask', 'ucClkMaskShift'), ('mask_data_mask', 'ucDataMaskShift'), ('en_clk_mask', 'ucClkEnShift'), ('en_data_mask', 'ucDataEnShift'),
              ('y_clk_mask', 'ucClkY_Shift'), ('y_data_mask', 'ucDataY_Shift'), ('a_clk_mask', 'ucClkA_Shift'), ('a_data_mask', 'ucDataA_Shift')]
    sorder = {n: i for i, n in enumerate(shifts)}
    for field, src in spairs:
        if 'i2c.%s = (1 << gpio->%s)' % (field, src) not in rec:
            fail('driver: %s from %s changed' % (field, src))
        if 'bus.%s = bit(shift[%d])' % (field, sorder[src]) not in cpp:
            fail('ours: %s should be bit(shift[%d])' % (field, sorder[src]))
    if 'if (gpio->sucI2cId.sbfAccess.bfHW_Capable)' not in rec or 'if (gpio->sucI2cId.ucAccess == 0xa0)' not in rec or 'i2c.i2c_id = gpio->sucI2cId.ucAccess' not in rec or 'if (i2c.mask_clk_reg) i2c.valid = true' not in rec:
        fail('driver: hw_capable, mm_i2c, id or valid changed')
    if 'kHwCapable = 0x80, kMmI2cId = 0xa0' not in cpp or 'bus.valid = bus.mask_clk_reg != 0' not in cpp or 'bus.hw_capable = (entry_id & kHwCapable) != 0' not in cpp:
        fail('ours: hw_capable, mm_i2c or valid')
    if not re.search(r'struct _ATOM_I2C_ID_CONFIG\s*\{.*?#else\s*UCHAR\s+bfI2C_LineMux:4;\s*UCHAR\s+bfHW_EngineID:3;\s*UCHAR\s+bfHW_Capable:1;', atombios_h, re.S):
        fail('the id bits: bit 7 is HW_Capable')
    look = squash(re.search(r'amdgpu_atombios_lookup_i2c_gpio\(.*?\n}\n', atombios_c, re.S).group(0))
    if 'num_indices = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) / sizeof(ATOM_GPIO_I2C_ASSIGMENT)' not in look or 'if (gpio->sucI2cId.ucAccess == id)' not in look or '(size - kTableHeaderBytes) / kAssignmentBytes' not in cpp or 'entry_id != id' not in cpp:
        fail('the lookup')
    if not re.search(r'amdgpu_atombios_lookup_i2c_gpio_quirks\([^)]*\)\s*\{\s*\}', atombios_c):
        fail('the driver has quirks for some boards now: ours has none')
    # the I2C record of a connector
    f, size = struct_fields(atombios_h, 'ATOM_I2C_RECORD')
    if (f['sucI2cId'], f['ucI2CAddr']) != (2, 3):
        fail('ATOM_I2C_RECORD layout')
    m = re.search(r'#define ATOM_I2C_RECORD_TYPE\s+(\d+)', atombios_h)
    if not m or 'kI2cRecord = %s' % m.group(1) not in conn or 'image.u8(at + 2, &id)' not in conn:
        fail('ATOM_I2C_RECORD_TYPE or the id byte read')
    if 'ddc_bus = amdgpu_atombios_lookup_i2c_gpio(adev, i2c_config->ucAccess)' not in re.sub(r'\s*->\s*', '->', squash(atombios_c)):
        fail('the connector takes the DDC bus from the access byte')
    if bad:
        sys.exit(1)
    print('AtomI2c matches the driver: GPIO_I2C_Info index, the entry layout, which register and shift is which, the id bits, the lookup, the I2C record')


main()
