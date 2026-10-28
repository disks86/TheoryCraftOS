#!/usr/bin/env python3
"""Checks the encoder part of the connector reader (src/Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.*) against the Linux driver's dce_v11_0.c.

  check_encoder_port.py path/to/dce_v11_0.c path/to/atombios.h

(kernel tag v6.12: drivers/gpu/drm/amd/amdgpu/dce_v11_0.c, drivers/gpu/drm/amd/include/atombios.h). It checks dce_v11_0_pick_dig_encoder (which DIG encoder each UNIPHY encoder id and link
gives) against AtomConnectors::dig_encoder, that the audio format block is the DIG's number only for DFP devices (dce_v11_0_encoder_prepare), the devices that make up
ATOM_DEVICE_DFP_SUPPORT against kDfpDevices, and the layout of ATOM_DISPLAY_OBJECT_PATH (the encoder object ids follow the connector and GPU ids).

NOT checked: ObjectID.h, which was not at hand (it was empty): the numbers of ENCODER_OBJECT_ID_INTERNAL_UNIPHY (0x1E), _UNIPHY1 (0x20), _UNIPHY2 (0x21), _UNIPHY3 (0x25), the bridges
NUTMEG (0x22) and TRAVIS (0x23), and the object id masks (id 0x00FF, enumeration 0x0700, type 0x7000) are from memory. Also from memory: amdgpu_atombios_get_connector_info_from_object_table's
walk of the path's object ids, and that the enumeration 2 of the encoder object makes the link B (amdgpu_atombios_encoder_get_dig_info).
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
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dce, h = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AtomConnectors.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AtomConnectors.cpp')).read()

    # the driver: case ENCODER_OBJECT_ID_INTERNAL_UNIPHY<n>: [if (dig->linkb) return B; else] return A
    body = re.search(r'dce_v11_0_pick_dig_encoder\(.*?\n\{(.*?)\n\}', dce, re.S).group(1)
    driver = {}
    for name, code in re.findall(r'case ENCODER_OBJECT_ID_INTERNAL_(UNIPHY\d?):(.*?)(?=case |default:)', body, re.S):
        nums = [int(x) for x in re.findall(r'return (\d+);', code)]
        driver[name] = (nums[1], nums[0]) if 'linkb' in code else (nums[0], nums[0])      # (link A, link B)  (the linkb return comes first)
    if len(driver) != 4:
        fail('pick_dig_encoder has %d UNIPHY cases, expected 4' % len(driver))
    # linkb: A is the second return in the driver's listing order (if linkb return 1; else return 0)
    ours = {}
    for name, code in re.findall(r'case kEncoder(Uniphy\d?): return ([^;]*);', cpp):
        n = [int(x) for x in re.findall(r'\d+', code)]
        ours[name.upper()] = (n[1], n[0]) if 'link_b' in code else (n[0], n[0])
    for name, (a, b) in driver.items():
        if ours.get(name) != (a, b):
            fail('%s: driver link A/B -> DIG %s, ours %s' % (name, (a, b), ours.get(name)))

    # the audio format block: afmt[dig_encoder], for ATOM_DEVICE_DFP_SUPPORT devices
    if not re.search(r'if \(amdgpu_encoder->active_device & ATOM_DEVICE_DFP_SUPPORT\)\s*dig->afmt = adev->mode_info\.afmt\[dig->dig_encoder\];', dce):
        fail('the audio block is no longer afmt[dig_encoder] for DFP devices')
    if not re.search(r'afmt_instance\(const Connector& c\) \{ return \(c\.devices & kDfpDevices\) \? dig_encoder\(c\) : kNoDig; \}', hpp):
        fail('afmt_instance is not the DIG of a DFP device')
    # ATOM_DEVICE_DFP_SUPPORT = DFP1..DFP6; the bits from the device indexes
    index = {m.group(1): int(m.group(2), 0) for m in re.finditer(r'#define ATOM_DEVICE_(\w+)_INDEX\s+(0x[0-9A-Fa-f]+|\d+)', h)}
    want = 0
    for n in range(1, 7):
        want |= 1 << index['DFP%d' % n]
    m = re.search(r'kDfpDevices = ([^;]*);', hpp)
    got = eval(m.group(1)) if m else None
    if got != want:
        fail('kDfpDevices is 0x%x, ATOM_DEVICE_DFP_SUPPORT is 0x%x' % (got or 0, want))
    if not re.search(r'#define ATOM_DEVICE_DFP_SUPPORT\s+\(ATOM_DEVICE_DFP1_SUPPORT \| ATOM_DEVICE_DFP2_SUPPORT \|\s+ATOM_DEVICE_DFP3_SUPPORT \| ATOM_DEVICE_DFP4_SUPPORT \| ATOM_DEVICE_DFP5_SUPPORT \| ATOM_DEVICE_DFP6_SUPPORT\)', h):
        fail('ATOM_DEVICE_DFP_SUPPORT is not DFP1..DFP6')

    # ATOM_DISPLAY_OBJECT_PATH: usDeviceTag, usSize, usConnObjectId, usGPUObjectId, then usGraphicObjIds[]: 8 bytes before the encoders
    m = re.search(r'typedef struct\s+_ATOM_DISPLAY_OBJECT_PATH\s*\{(.*?)\}\s*ATOM_DISPLAY_OBJECT_PATH;', h, re.S)
    fields = re.findall(r'USHORT\s+(\w+)', re.sub(r'//[^\n]*', '', m.group(1)))
    if fields != ['usDeviceTag', 'usSize', 'usConnObjectId', 'usGPUObjectId', 'usGraphicObjIds'] or not re.search(r'kPathHeaderBytes = 8;', cpp):
        fail('ATOM_DISPLAY_OBJECT_PATH is %s' % fields)
    if bad:
        sys.exit(1)
    print('check_encoder_port: ok')


main()
