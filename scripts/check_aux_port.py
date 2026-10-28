#!/usr/bin/env python3
"""Checks the AUX channel port (src/Hardware/GraphicsProcessingUnit/AMD/AmdAux.*) against the Linux driver's atombios.h.

  check_aux_port.py path/to/atombios.h

(kernel tag v6.12: drivers/gpu/drm/amd/include/atombios.h). It checks the index of ProcessAuxChannelTransaction in the master list of command tables, the layout of
PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2 (which byte of the two parameter dwords is which) against how AmdAux packs and reads them, and the DisplayPort request and reply
codes against the values the DRM helper defines (written here from the DisplayPort specification, not taken from our source). NOT checked: amdgpu_atombios_dp.c and drm_dp_helper.c,
which were not at hand when AmdAux was written (the request framing, the limits of the table, the retry rules).
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
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    h = open(sys.argv[1]).read()
    hpp = open(os.path.join(ROOT, 'AmdAux.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdAux.cpp')).read()

    m = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_COMMAND_TABLES\s*\{(.*?)\}\s*ATOM_MASTER_LIST_OF_COMMAND_TABLES', h, re.S)
    names = re.findall(r'USHORT\s+(\w+);', m.group(1))
    k = re.search(r'kCommandTable = (\d+)', hpp)
    if not k or names.index('ProcessAuxChannelTransaction') != int(k.group(1)):
        fail('ProcessAuxChannelTransaction index %d, ours %s' % (names.index('ProcessAuxChannelTransaction'), k.group(1) if k else None))

    # the parameters: lpAuxRequest, lpDataOut (words), ucChannelID, ucReplyStatus/ucDelay, ucDataOutLen, ucHPD_ID (bytes): 8 bytes, two little-endian dwords
    m = re.search(r'typedef struct _PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2\s*\{(.*?)\}\s*PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2', h, re.S)
    body = re.sub(r'//[^\n]*', '', m.group(1))
    # (a union overlays its members at the same offset: take the union's size as its largest member's)
    at, offsets = 0, {}
    for u, plain in re.findall(r'union\s*\{(.*?)\}|((?:USHORT|UCHAR)\s+\w+\s*;)', body, re.S):
        members = re.findall(r'(USHORT|UCHAR)\s+(\w+)\s*;', u or plain)
        for t, n in members:
            offsets[n] = at
        at += max(2 if t == 'USHORT' else 1 for t, n in members)
    want = {'lpAuxRequest': 0, 'lpDataOut': 2, 'ucChannelID': 4, 'ucReplyStatus': 5, 'ucDelay': 5, 'ucDataOutLen': 6, 'ucHPD_ID': 7}
    for n, o in want.items():
        if offsets.get(n) != o:
            fail('%s at %s, expected %d' % (n, offsets.get(n), o))
    if at != 8:
        fail('the parameters are %d bytes' % at)
    # ours: dword 0 = request | data out << 16; dword 1 = channel | hpd << 24, the reply status read from bits 15:8 and the length from 23:16
    if not re.search(r'kRequestOffset \| \(kDataOutOffset << 16\)', cpp):
        fail('dword 0 is not lpAuxRequest | lpDataOut << 16')
    if not re.search(r'uint32_t\{channel_\} \| \(uint32_t\{hpd_\} << 24\)', cpp):
        fail('dword 1 is not channel | hpd << 24')
    if not re.search(r'\*ack = static_cast<uint8_t>\(params\[1\] >> 8\)', cpp):
        fail('the reply status is not byte 1 of dword 1')
    if not re.search(r'received = static_cast<uint8_t>\(params\[1\] >> 16\)', cpp):
        fail('the data length is not byte 2 of dword 1')
    # the request is at scratch byte 4 (scratch + 1 dword), the data out 16 bytes on
    if not re.search(r'kRequestOffset = 4;', cpp) or not re.search(r'kDataOutOffset = 20;', cpp):
        fail('the request / data out offsets are not 0 + 4 and 16 + 4')
    # the comments on the hot plug id in atombios.h: 0 = HPD1 ... 5 = HPD6
    if not re.search(r'ucHPD_ID;\s*//=0: HPD1, =1: HPD2, =2: HPD3, =3: HPD4, =4: HPD5, =5: HPD6', h):
        fail('the hot plug id comment changed')

    # DisplayPort (DP 1.2 section 2.7.7, and the DRM helper's defines)
    codes = {'kI2cWrite': 0x0, 'kI2cRead': 0x1, 'kI2cWriteStatusUpdate': 0x2, 'kI2cMot': 0x4, 'kNativeWrite': 0x8, 'kNativeRead': 0x9,
             'kNativeNack': 0x1, 'kNativeDefer': 0x2, 'kI2cNack': 0x4, 'kI2cDefer': 0x8, 'kNativeReplyMask': 0x3, 'kI2cReplyMask': 0xC}
    for n, v in codes.items():
        g = re.search(r'\b' + n + r' = 0x([0-9A-Fa-f]+)', hpp)
        if not g or int(g.group(1), 16) != v:
            fail('%s is %s, expected 0x%x' % (n, g.group(1) if g else None, v))
    if bad:
        sys.exit(1)
    print('check_aux_port: ok')


main()
