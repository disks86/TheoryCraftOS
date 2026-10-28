#!/usr/bin/env python3
"""Checks the connector reader (src/Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.*) against the Linux driver it was ported from.

  check_connectors_port.py path/to/amdgpu_atombios.c path/to/atombios.h path/to/dce_11_2_d.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/amdgpu_atombios.c, drivers/gpu/drm/amd/include/atombios.h, include/asic_reg/dce/dce_11_2_d.h). It checks the master list
indexes of the Object_Header and GPIO_Pin_LUT data tables, the layouts of the object header, display path, object table and GPIO pin entries, the record types, the connector
type of each connector object id (object_connector_convert) against the type this reads it as, the mask-to-pin table of the hot plug GPIO and the register it must be on.

ObjectID.h (the object id masks and the CONNECTOR_OBJECT_ID_* numbers) is not checked: the connector numbers are checked through object_connector_convert's order instead.
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


def define(src, name):
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+\(?\s*(0x[0-9a-fA-F]+|\d+)', src)
    if not m:
        sys.exit('not in header: ' + name)
    return int(m.group(1), 0)


def struct_fields(src, name):
    """The (type, name) of the fields of `typedef struct ... }name;`, in order, with the byte offset of each (USHORT 2, UCHAR 1)."""
    m = re.search(r'typedef struct\s+_?' + name + r'\b[^{]*\{(.*?)\}\s*' + name + r'\s*;', src, re.S)
    if not m:
        sys.exit('struct not found: ' + name)
    fields, at = [], 0
    for line in m.group(1).split(';'):
        line = re.sub(r'//[^\n]*', '', line).strip()
        if not line:
            continue
        t = re.match(r'(\w+)\s+(\w+)(\[(\w*)\])?', line)
        if not t:
            sys.exit('field not understood in %s: %s' % (name, line))
        size = {'USHORT': 2, 'UCHAR': 1, 'ULONG': 4, 'ATOM_COMMON_TABLE_HEADER': 4, 'ATOM_COMMON_RECORD_HEADER': 2}.get(t.group(1), 0)   # (the element of a flexible array: no size)
        count = int(t.group(4)) if t.group(4) else 1
        fields.append((t.group(2), at))
        at += size * (count if t.group(3) and t.group(4) else 1)
    return dict(fields), at


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    atombios_c, atombios_h, dce_d = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AtomConnectors.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AtomConnectors.cpp')).read()

    # the master list of data tables: the index of an entry is its position
    m = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_DATA_TABLES\s*\{(.*?)\}\s*ATOM_MASTER_LIST_OF_DATA_TABLES', atombios_h, re.S)
    names = re.findall(r'USHORT\s+(\w+);', m.group(1))
    for name, mine in (('GPIO_Pin_LUT', 'kDataGpioPinLut'), ('Object_Header', 'kDataObjectHeader')):
        k = re.search(mine + r' = (\d+)', cpp)
        if not k or names.index(name) != int(k.group(1)):
            fail('%s: index %d in the master list, ours %s' % (name, names.index(name), k.group(1) if k else None))

    # ATOM_OBJECT_HEADER: the offsets this reads
    f, _ = struct_fields(atombios_h, 'ATOM_OBJECT_HEADER')
    for field, mine in (('usDeviceSupport', 'kObjDeviceSupport'), ('usConnectorObjectTableOffset', 'kObjConnectorTable'), ('usDisplayPathTableOffset', 'kObjPathTable')):
        k = re.search(mine + r' = (\d+)', cpp)
        if not k or f[field] != int(k.group(1)):
            fail('ATOM_OBJECT_HEADER.%s at %d, ours %s' % (field, f[field], k.group(1) if k else None))
    # the display path: tag, size, connector, GPU, then the object ids
    f, _ = struct_fields(atombios_h, 'ATOM_DISPLAY_OBJECT_PATH')
    if (f['usDeviceTag'], f['usSize'], f['usConnObjectId'], f['usGPUObjectId'], f['usGraphicObjIds']) != (0, 2, 4, 6, 8) or 'kPathHeaderBytes = 8' not in cpp:
        fail('ATOM_DISPLAY_OBJECT_PATH layout')
    if 'image.u16(path_at, &tag) || !image.u16(path_at + 2, &size) || !image.u16(path_at + 4, &conn_id)' not in cpp:
        fail('the path fields are not read at 0, 2 and 4')
    f, size = struct_fields(atombios_h, 'ATOM_DISPLAY_OBJECT_PATH_TABLE')
    if f['asDispPath'] != 4 or 'kPathTableHeaderBytes = 4' not in cpp:
        fail('ATOM_DISPLAY_OBJECT_PATH_TABLE layout')
    # the connector object table
    f, size = struct_fields(atombios_h, 'ATOM_OBJECT')
    if (f['usObjectID'], f['usRecordOffset'], size) != (0, 4, 8) or 'kObjectBytes = 8' not in cpp or 'image.u16(object, &id) || !image.u16(object + 4, &record_offset)' not in cpp:
        fail('ATOM_OBJECT layout')
    f, _ = struct_fields(atombios_h, 'ATOM_OBJECT_TABLE')
    if f['asObjects'] != 4 or 'kObjectTableHeaderBytes = 4' not in cpp:
        fail('ATOM_OBJECT_TABLE layout')
    # records
    f, size = struct_fields(atombios_h, 'ATOM_COMMON_RECORD_HEADER')
    if (f['ucRecordType'], f['ucRecordSize']) != (0, 1):
        fail('ATOM_COMMON_RECORD_HEADER layout')
    f, size = struct_fields(atombios_h, 'ATOM_HPD_INT_RECORD')
    hpd = (f['ucHPDIntGPIOID'], f['ucPlugged_PinState'])
    if hpd != (2, 3) or 'image.u8(at + 2, gpio_id) && image.u8(at + 3, plugged_state)' not in cpp:
        fail('ATOM_HPD_INT_RECORD layout')
    if define(atombios_h, 'ATOM_HPD_INT_RECORD_TYPE') != 2 or 'kHpdIntRecord = 2' not in cpp:
        fail('ATOM_HPD_INT_RECORD_TYPE')
    mx = define(atombios_h, 'ATOM_CONNECTOR_FORCED_TMDS_CAP_RECORD_TYPE')
    if 'define ATOM_MAX_OBJECT_RECORD_NUMBER' not in atombios_h or 'ATOM_MAX_OBJECT_RECORD_NUMBER                  ATOM_CONNECTOR_FORCED_TMDS_CAP_RECORD_TYPE' not in atombios_h \
            or 'kMaxRecordType = %d' % mx not in cpp:
        fail('ATOM_MAX_OBJECT_RECORD_NUMBER')
    # the record walk's conditions
    walk = squash(atombios_c)
    if 'while (record->ucRecordSize > 0 && record->ucRecordType > 0 && record->ucRecordType <= ATOM_MAX_OBJECT_RECORD_NUMBER)' not in walk or \
            'if (size == 0 || type == 0 || type > kMaxRecordType) return false;' not in squash(cpp):
        fail('the record walk conditions')
    # the GPIO table
    f, size = struct_fields(atombios_h, 'ATOM_GPIO_PIN_ASSIGNMENT')
    if (f['usGpioPin_AIndex'], f['ucGpioPinBitShift'], f['ucGPIO_ID'], size) != (0, 2, 3, 4) or 'kGpioPinBytes = 4' not in cpp or \
            'image.u16(at, &reg) || !image.u8(at + 2, &shift) || !image.u8(at + 3, &gpio_id)' not in cpp:
        fail('ATOM_GPIO_PIN_ASSIGNMENT layout')
    lookup = squash(re.search(r'amdgpu_atombios_lookup_gpio\(.*?\n}\n', atombios_c, re.S).group(0))
    if 'num_indices = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) / sizeof(ATOM_GPIO_PIN_ASSIGNMENT)' not in lookup or 'if (id == pin->ucGPIO_ID)' not in lookup or \
            'gpio.mask = (1 << pin->ucGpioPinBitShift)' not in lookup or '(size - kTableHeaderBytes) / kGpioPinBytes' not in cpp or 'gpio_id != id' not in cpp:
        fail('amdgpu_atombios_lookup_gpio')
    # the hot plug pin of the GPIO: register, then mask
    hp = squash(re.search(r'amdgpu_atombios_get_hpd_info_from_gpio\(.*?\n}\n', atombios_c, re.S).group(0))
    masks = re.findall(r'case \(1 << (\d+)\): hpd\.hpd = AMDGPU_HPD_(\d);', hp)
    if [(int(a), int(b)) for a, b in masks] != [(0, 1), (8, 2), (16, 3), (24, 4), (26, 5), (28, 6)]:
        fail('the masks of the hot plug pins: %s' % masks)
    mine = re.findall(r'case 1u << (\d+): return (\d);', cpp)
    if [(int(a), int(b) + 1) for a, b in mine] != [(int(a), int(b)) for a, b in masks]:
        fail('our masks of the hot plug pins: %s' % mine)
    if 'if (gpio->reg == reg)' not in hp or 'reg != kHpdGpioRegister' not in cpp:
        fail('the register of the hot plug GPIO is compared')
    if define(dce_d, 'mmDC_GPIO_HPD_A') != 0x488d or 'kHpdGpioRegister = 0x488d' not in cpp:
        fail('mmDC_GPIO_HPD_A')
    # the devices skipped, and their bits
    if define(atombios_h, 'ATOM_DEVICE_TV1_INDEX') != 2 or define(atombios_h, 'ATOM_DEVICE_CV_INDEX') != 8 or 'kDeviceTv1 = 1u << 2, kDeviceCv = 1u << 8' not in cpp:
        fail('the TV and CV device bits')
    walk = squash(re.search(r'amdgpu_atombios_get_connector_info_from_object_table\(.*?\n}\n', atombios_c, re.S).group(0))
    for token in ('if (crev < 2) return false;', 'if (device_support & le16_to_cpu(path->usDeviceTag))', 'ATOM_DEVICE_TV1_SUPPORT', 'ATOM_DEVICE_CV_SUPPORT',
                  'if (con_obj_id >= ARRAY_SIZE(object_connector_convert))', 'if (connector_type == DRM_MODE_CONNECTOR_Unknown) continue;',
                  '(le16_to_cpu(path->usDeviceTag) & (ATOM_DEVICE_TV_SUPPORT | ATOM_DEVICE_CV_SUPPORT)) == 0',
                  'if (le16_to_cpu(path->usConnObjectId) == le16_to_cpu(con_obj->asObjects[j]. usObjectID))', 'path_size += le16_to_cpu(path->usSize);'):
        if token.replace('. ', '.') not in walk.replace('. ', '.'):
            fail('the driver changed: ' + token)
    # object_connector_convert: our type for each connector object id
    table = re.search(r'object_connector_convert\[\] = \{(.*?)\};', atombios_c, re.S).group(1)
    drm = re.findall(r'DRM_MODE_CONNECTOR_(\w+)', table)
    theirs = {'Unknown': None, 'DVII': 'DVI-I', 'DVID': 'DVI-D', 'VGA': 'VGA', 'Composite': 'Composite', 'SVIDEO': 'SVIDEO', '9PinDIN': 'DIN', 'HDMIA': 'HDMI-A', 'HDMIB': 'HDMI-B',
              'LVDS': 'LVDS', 'DisplayPort': 'DP', 'eDP': 'eDP'}
    mine_table = re.search(r'kConnectorOfObject\[\] = \{(.*?)\};', cpp, re.S).group(1)
    mine_ids = re.findall(r'(kUnknown|\d+)(?:\s*/\*([^*]*)\*/)?', mine_table)
    names_of_types = re.search(r'enum class Type : uint8_t \{([^}]*)\}', hpp).group(1)
    order_of_types = [t.strip() for t in names_of_types.split(',')]
    type_names = dict(re.findall(r'case Type::(\w+): return "([^"]+)";', cpp))
    if len(mine_ids) != len(drm):
        fail('object_connector_convert has %d entries, ours %d' % (len(drm), len(mine_ids)))
    last = None
    for i, (d, (idx, comment)) in enumerate(zip(drm, mine_ids)):
        want = theirs[d]
        if idx == 'kUnknown':
            got = None
        else:
            got = type_names[order_of_types[int(idx)]]
        if got != want:
            fail('connector object %d: the driver says %s, ours %s' % (i, want, got))
    if bad:
        sys.exit(1)
    print('AtomConnectors matches the driver: data table indexes, object header, path, object and record layouts, the GPIO and hot plug pin masks, object_connector_convert')


main()
