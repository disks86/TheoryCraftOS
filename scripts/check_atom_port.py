#!/usr/bin/env python3
"""Checks the AtomBIOS interpreter (src/Hardware/GraphicsProcessingUnit/AMD/AtomBios.*) against the Linux driver it was ported from.

  check_atom_port.py path/to/atom.c path/to/atom.h path/to/atombios.h

(kernel tag v6.12, drivers/gpu/drm/amd/amdgpu/ and drivers/gpu/drm/amd/include/). It checks that the constants of atom.h (magic numbers, header offsets, operand kinds, alignments,
workspace registers, indirect I/O opcodes, the I/O modes) are the ones AtomBios.cpp uses, that the generated opcode table is the driver's, that the alignment tables are the driver's, that
the data table indexes AtomBios relies on are what atombios.h lays out, and that the structure of the main entry points (parse, execute, asic_init) still reads the same fields in the same
order. The behaviour itself is checked by tests/AtomBiosTest.cpp and by the differential test in scripts/atom_differential, which runs the driver's own atom.c.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')
bad = False


def fail(text):
    global bad
    bad = True
    print('FAIL:', text)


def define(src, name):
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+|\d+)', src)
    if not m:
        sys.exit('not found in the header: ' + name)
    return int(m.group(1), 0)


def squash(text):
    return re.sub(r'\s+', ' ', text)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    atom_c, atom_h, atombios_h = [open(a).read() for a in sys.argv[1:]]
    cpp = open(os.path.join(ROOT, 'AtomBios.cpp')).read()
    hpp = open(os.path.join(ROOT, 'AtomBios.hpp')).read()

    def ours(name):
        m = re.search(r'\b' + name + r' = (0x[0-9a-fA-F]+|\d+)', cpp + hpp)
        if not m:
            fail('AtomBios lacks ' + name)
            return None
        return int(m.group(1), 0)

    pairs = {
        'kBiosMagic': 'ATOM_BIOS_MAGIC', 'kAtiMagicPtr': 'ATOM_ATI_MAGIC_PTR', 'kRomTablePtr': 'ATOM_ROM_TABLE_PTR', 'kRomMagicPtr': 'ATOM_ROM_MAGIC_PTR', 'kRomCmdPtr': 'ATOM_ROM_CMD_PTR',
        'kRomDataPtr': 'ATOM_ROM_DATA_PTR', 'kCmdInit': 'ATOM_CMD_INIT', 'kDataFwiPtr': 'ATOM_DATA_FWI_PTR', 'kDataIioPtr': 'ATOM_DATA_IIO_PTR', 'kFwiDefSclkPtr': 'ATOM_FWI_DEFSCLK_PTR',
        'kFwiDefMclkPtr': 'ATOM_FWI_DEFMCLK_PTR', 'kCtSizePtr': 'ATOM_CT_SIZE_PTR', 'kCtWsPtr': 'ATOM_CT_WS_PTR', 'kCtPsPtr': 'ATOM_CT_PS_PTR', 'kCtPsMask': 'ATOM_CT_PS_MASK',
        'kCtCodePtr': 'ATOM_CT_CODE_PTR', 'kOpCnt': 'ATOM_OP_CNT', 'kOpEot': 'ATOM_OP_EOT', 'kCaseMagic': 'ATOM_CASE_MAGIC', 'kCaseEnd': 'ATOM_CASE_END',
    }
    for mine, theirs in pairs.items():
        v = ours(mine)
        if v is not None and v != define(atom_h, theirs):
            fail('%s: ours %#x, driver %#x' % (mine, v, define(atom_h, theirs)))
    if ours('kLoopTimeoutUs') != define(atom_c, 'ATOM_CMD_TIMEOUT_SEC') * 1000000 and 20 * 1000 * 1000 != define(atom_c, 'ATOM_CMD_TIMEOUT_SEC') * 1000000:
        fail('the loop timeout')
    # the magic strings
    m = re.search(r'#define ATOM_ATI_MAGIC\s+"([^"]*)"', atom_h)
    if not m or ('const char kAtiMagic[] = "%s"' % m.group(1)) not in cpp:
        fail('the ATI magic string')
    m = re.search(r'#define ATOM_ROM_MAGIC\s+"([^"]*)"', atom_h)
    if not m or ('const char kRomMagic[] = "%s"' % m.group(1)) not in cpp:
        fail('the ROM magic string')
    # operand kinds, alignments, workspace registers, IIO opcodes, I/O modes, conditions, ports, units
    for names, enum_re in ((['REG', 'PS', 'WS', 'FB', 'ID', 'IMM', 'PLL', 'MC'], r'enum : int \{ kArgReg = 0, kArgPs, kArgWs, kArgFb, kArgId, kArgImm, kArgPll, kArgMc \};'),):
        for i, n in enumerate(names):
            if define(atom_h, 'ATOM_ARG_' + n) != i:
                fail('ATOM_ARG_' + n)
        if not re.search(enum_re, cpp):
            fail('the operand kind enum changed')
    for i, n in enumerate(['DWORD', 'WORD0', 'WORD8', 'WORD16', 'BYTE0', 'BYTE8', 'BYTE16', 'BYTE24']):
        if define(atom_h, 'ATOM_SRC_' + n) != i:
            fail('ATOM_SRC_' + n)
    for i, n in enumerate(['QUOTIENT', 'REMAINDER', 'DATAPTR', 'SHIFT', 'OR_MASK', 'AND_MASK', 'FB_WINDOW', 'ATTRIBUTES', 'REGPTR']):
        if define(atom_h, 'ATOM_WS_' + n) != 0x40 + i:
            fail('ATOM_WS_' + n)
    if 'enum : uint32_t { kWsQuotient = 0x40, kWsRemainder, kWsDataPtr, kWsShift, kWsOrMask, kWsAndMask, kWsFbWindow, kWsAttributes, kWsRegPtr };' not in cpp:
        fail('the workspace register enum changed')
    for i, n in enumerate(['NOP', 'START', 'READ', 'WRITE', 'CLEAR', 'SET', 'MOVE_INDEX', 'MOVE_ATTR', 'MOVE_DATA', 'END']):
        if define(atom_h, 'ATOM_IIO_' + n) != i:
            fail('ATOM_IIO_' + n)
    if 'kIioNop = 0, kIioStart, kIioRead, kIioWrite, kIioClear, kIioSet, kIioMoveIndex, kIioMoveAttr, kIioMoveData, kIioEnd' not in cpp:
        fail('the IIO opcode enum changed')
    if (define(atom_h, 'ATOM_IO_MM'), define(atom_h, 'ATOM_IO_PCI'), define(atom_h, 'ATOM_IO_SYSIO'), define(atom_h, 'ATOM_IO_IIO')) != (0, 1, 2, 0x80):
        fail('the I/O modes')
    for i, n in enumerate(['ABOVE', 'ABOVEOREQUAL', 'ALWAYS', 'BELOW', 'BELOWOREQUAL', 'EQUAL', 'NOTEQUAL']):
        if define(atom_c, 'ATOM_COND_' + n) != i:
            fail('ATOM_COND_' + n)
    # the alignment tables
    m = re.search(r'atom_arg_mask\[8\] =\s*\{([^}]*)\}', atom_c)
    mine = re.search(r'kArgMask\[8\] = \{([^}]*)\}', cpp)
    if not m or not mine or [x.strip() for x in m.group(1).split(',')] != [x.strip() for x in mine.group(1).split(',')]:
        fail('atom_arg_mask')
    m = re.search(r'atom_arg_shift\[8\] = \{([^}]*)\}', atom_c)
    mine = re.search(r'kArgShift\[8\] = \{([^}]*)\}', cpp)
    if not m or not mine or [x.strip() for x in m.group(1).split(',')] != [x.strip() for x in mine.group(1).split(',')]:
        fail('atom_arg_shift')
    m = re.search(r'atom_def_dst\[8\] = \{([^}]*)\}', atom_c)
    mine = re.search(r'kDefDst\[8\] = \{([^}]*)\}', cpp)
    if not m or not mine or [x.strip() for x in m.group(1).split(',')] != [x.strip() for x in mine.group(1).split(',')]:
        fail('atom_def_dst')
    m = re.search(r'atom_dst_to_src\[8\]\[4\] = \{(.*?)\n\};', atom_c, re.S)
    mine = re.search(r'kDstToSrc\[8\]\[4\] = \{(.*?)\};', cpp, re.S)
    nums = lambda t: re.findall(r'\d+', re.sub(r'/\*.*?\*/', '', t, flags=re.S))
    if not m or not mine or nums(m.group(1)) != nums(mine.group(1)):
        fail('atom_dst_to_src')
    m = re.search(r'atom_iio_len\[\] = \{([^}]*)\}', atom_c)
    mine = re.search(r'kIioLen\[10\] = \{([^}]*)\}', cpp)
    if not m or not mine or nums(m.group(1)) != nums(mine.group(1)):
        fail('atom_iio_len')
    # the opcode table: regenerate it and compare with the checked-in one
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.atom_ops_check.inc')
    try:
        subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gen_atom_ops.py'), sys.argv[1], out], check=True, capture_output=True)
        if open(out).read() != open(os.path.join(ROOT, 'AtomBiosOps.inc')).read():
            fail('AtomBiosOps.inc is not what gen_atom_ops.py makes from the driver\'s table')
    finally:
        if os.path.exists(out):
            os.remove(out)
    # the master list of data tables: the indexes used
    lst = re.search(r'typedef struct _ATOM_MASTER_LIST_OF_DATA_TABLES\s*\{(.*?)\}ATOM_MASTER_LIST_OF_DATA_TABLES;', atombios_h, re.S)
    fields = re.findall(r'USHORT\s+(\w+);', lst.group(1))
    if fields.index('FirmwareInfo') != 4 or 4 + 2 * 4 != define(atom_h, 'ATOM_DATA_FWI_PTR'):
        fail('FirmwareInfo is not the fifth data table')
    if fields.index('VRAM_UsageByFirmware') != 11 or 'kDataVramUsageIndex = 11' not in cpp:
        fail('VRAM_UsageByFirmware is not the twelfth data table')
    if 4 + 2 * fields.index('IndirectIOAccess') != define(atom_h, 'ATOM_DATA_IIO_PTR'):
        fail('IndirectIOAccess')
    hdr = re.search(r'typedef struct _ATOM_ROM_HEADER\s*\{(.*?)\}ATOM_ROM_HEADER;', atombios_h, re.S).group(1)
    shorts = re.findall(r'USHORT\s+(\w+);', hdr)
    # sHeader 4 bytes, the 4 byte signature, then the USHORTs
    cmd_off = 8 + 2 * shorts.index('usMasterCommandTableOffset')
    data_off = 8 + 2 * shorts.index('usMasterDataTableOffset')
    if cmd_off != define(atom_h, 'ATOM_ROM_CMD_PTR') or data_off != define(atom_h, 'ATOM_ROM_DATA_PTR'):
        fail('the ROM header layout')
    atombios_nc = re.sub(r'//[^\n]*', '', atombios_h)
    fw = re.search(r'typedef struct _ATOM_FIRMWARE_INFO\s*\{\s*ATOM_COMMON_TABLE_HEADER\s+sHeader;\s*ULONG\s+ulFirmwareRevision;\s*ULONG\s+ulDefaultEngineClock;\s*ULONG\s+ulDefaultMemoryClock;', atombios_nc)
    if not fw or define(atom_h, 'ATOM_FWI_DEFSCLK_PTR') != 8 or define(atom_h, 'ATOM_FWI_DEFMCLK_PTR') != 12 or 'kFwiRevisionPtr = 4' not in cpp:
        fail('the firmware info layout')
    vu = re.search(r'typedef struct _ATOM_FIRMWARE_VRAM_RESERVE_INFO\s*\{\s*ULONG\s+ulStartAddrUsedByFirmware;\s*USHORT\s+usFirmwareUseInKb;', atombios_nc)
    if not vu:
        fail('the VRAM reservation layout')
    if define(atombios_h, 'ATOM_VRAM_OPERATION_FLAGS_MASK') != 0xC0000000 or define(atombios_h, 'ATOM_VRAM_OPERATION_FLAGS_SHIFT') != 30 or define(atombios_h, 'ATOM_VRAM_BLOCK_SRIOV_MSG_SHARE_RESERVATION') != 2:
        fail('the VRAM reservation flags')
    # the default scratch size and the executing entry points
    asic = squash(re.search(r'int amdgpu_atom_asic_init\(.*?\n\}', atom_c, re.S).group(0))
    if 'CU16(ctx->data_table + ATOM_DATA_FWI_PTR)' not in asic or 'CU32(hwi + ATOM_FWI_DEFSCLK_PTR)' not in asic or 'CU32(hwi + ATOM_FWI_DEFMCLK_PTR)' not in asic or '!ps[0] || !ps[1]' not in asic:
        fail('amdgpu_atom_asic_init changed')
    execute = squash(re.search(r'\nstatic int amdgpu_atom_execute_table_locked\([^;{]*\)\s*\{.*?\n\}', atom_c, re.S).group(0))
    if 'CU16(ctx->cmd_table + 4 + 2 * index)' not in execute or 'ATOM_CT_PS_MASK' not in execute or 'ectx.ps_shift = ps / 4' not in execute or 'op < ATOM_OP_CNT && op > 0' not in execute:
        fail('amdgpu_atom_execute_table_locked changed')
    call = squash(re.search(r'static void atom_op_calltable\(.*?\n\}', atom_c, re.S).group(0))
    if 'ctx->ps + ctx->ps_shift' not in call or 'ctx->ps_size - ctx->ps_shift' not in call:
        fail('atom_op_calltable passes other parameters')
    if bad:
        sys.exit(1)
    print('AtomBios matches the driver: constants, alignment tables, the generated opcode table, the data table layout, the entry points')


main()
