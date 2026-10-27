#!/usr/bin/env python3
"""Checks the interrupt source ids and fault registers of src/Hardware/GraphicsProcessingUnit/AMD/AmdIrq.{hpp,cpp} against the Linux driver they were taken from.

  check_irq_port.py path/to/ivsrcid_vislands30.h path/to/gmc_8_1_d.h path/to/gmc_8_1_sh_mask.h path/to/gfx_v8_0.c path/to/sdma_v3_0.c path/to/gmc_v8_0.c

(from kernel tag v6.12, drivers/gpu/drm/amd/include/ivsrcid/, include/asic_reg/gmc/ and amdgpu/). It checks that the source ids are the ones the driver registers with
amdgpu_irq_add_id in the three blocks' sw_init, that the register numbers and masks are the headers', and that the ring id decoding in the handlers is the one classify() uses.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')


def fail(text):
    print('FAIL:', text)
    global bad
    bad = True


def define(src, name):
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+|\d+)', src)
    if not m:
        sys.exit('not found in header: ' + name)
    return int(m.group(1), 0)


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    ivs, gmc_d, gmc_m, gfx, sdma, gmc_c = [open(a).read() for a in sys.argv[1:]]
    hpp = open(os.path.join(ROOT, 'AmdIrq.hpp')).read()
    cpp = open(os.path.join(ROOT, 'AmdIrq.cpp')).read()

    # source ids in AmdIrq.hpp: kSrcX = N
    ours = {m.group(1): int(m.group(2)) for m in re.finditer(r'\b(kSrc\w+) = (\d+)', hpp)}
    want = {
        'kSrcCpEndOfPipe': define(ivs, 'VISLANDS30_IV_SRCID_CP_END_OF_PIPE'),
        'kSrcCpPrivRegFault': define(ivs, 'VISLANDS30_IV_SRCID_CP_PRIV_REG_FAULT'),
        'kSrcCpPrivInstrFault': define(ivs, 'VISLANDS30_IV_SRCID_CP_PRIV_INSTR_FAULT'),
        'kSrcCpEccError': define(ivs, 'VISLANDS30_IV_SRCID_CP_ECC_ERROR'),
        'kSrcSdmaTrap': define(ivs, 'VISLANDS30_IV_SRCID_SDMA_TRAP'),
        'kSrcSdmaSrbmWrite': define(ivs, 'VISLANDS30_IV_SRCID_SDMA_SRBM_WRITE'),
        'kSrcGfxPageInvFault': define(ivs, 'VISLANDS30_IV_SRCID_GFX_PAGE_INV_FAULT'),
        'kSrcGfxMemProtFault': define(ivs, 'VISLANDS30_IV_SRCID_GFX_MEM_PROT_FAULT'),
        'kSrcSdmaIllegalInstruction': 241,   # a bare number in sdma_v3_0_sw_init, checked below
    }
    for k, v in want.items():
        if ours.get(k) != v:
            fail(f'{k}: ours {ours.get(k)}, driver {v}')
    if not re.search(r'amdgpu_irq_add_id\(adev, AMDGPU_IRQ_CLIENTID_LEGACY, 241,\s*&adev->sdma\.illegal_inst_irq', sdma):
        fail('sdma_v3_0_sw_init no longer registers source 241 for illegal instructions')
    for name, handler in (('VISLANDS30_IV_SRCID_CP_END_OF_PIPE', 'eop_irq'), ('VISLANDS30_IV_SRCID_CP_PRIV_REG_FAULT', 'priv_reg_irq'), ('VISLANDS30_IV_SRCID_CP_PRIV_INSTR_FAULT', 'priv_inst_irq')):
        if not re.search(name + r',\s*&adev->gfx\.' + handler, gfx):
            fail(f'gfx_v8_0_sw_init: {name} is not registered to {handler}')
    for name in ('VISLANDS30_IV_SRCID_GFX_PAGE_INV_FAULT', 'VISLANDS30_IV_SRCID_GFX_MEM_PROT_FAULT'):
        if name + ', &adev->gmc.vm_fault' not in gmc_c:
            fail(f'gmc_v8_0_sw_init: {name} is not registered to vm_fault')

    # registers
    regs = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'constexpr uint32_t (mm\w+) = (0x[0-9a-fA-F]+)', cpp)}
    regs.update({m.group(1): int(m.group(2), 16) for m in re.finditer(r',\s*(mm\w+) = (0x[0-9a-fA-F]+)', cpp)})
    for name in ('mmVM_CONTEXT1_CNTL2', 'mmVM_CONTEXT1_PROTECTION_FAULT_STATUS', 'mmVM_CONTEXT1_PROTECTION_FAULT_MCCLIENT', 'mmVM_CONTEXT1_PROTECTION_FAULT_ADDR'):
        if regs.get(name) != define(gmc_d, name):
            fail(f'{name}: ours {regs.get(name)}, header {define(gmc_d, name)}')
    masks = {
        'kStatusProtectionsMask': 'VM_CONTEXT1_PROTECTION_FAULT_STATUS__PROTECTIONS_MASK',
        'kStatusClientIdMask': 'VM_CONTEXT1_PROTECTION_FAULT_STATUS__MEMORY_CLIENT_ID_MASK',
        'kStatusClientIdShift': 'VM_CONTEXT1_PROTECTION_FAULT_STATUS__MEMORY_CLIENT_ID__SHIFT',
        'kStatusRwMask': 'VM_CONTEXT1_PROTECTION_FAULT_STATUS__MEMORY_CLIENT_RW_MASK',
        'kStatusVmidMask': 'VM_CONTEXT1_PROTECTION_FAULT_STATUS__VMID_MASK',
        'kStatusVmidShift': 'VM_CONTEXT1_PROTECTION_FAULT_STATUS__VMID__SHIFT',
        'kClearProtectionFaultStatusAddrMask': 'VM_CONTEXT1_CNTL2__CLEAR_PROTECTION_FAULT_STATUS_ADDR_MASK',
    }
    for k, h in masks.items():
        m = re.search(r'\b' + k + r' = (0x[0-9a-fA-F]+)', cpp)
        if not m or int(m.group(1), 16) != define(gmc_m, h):
            fail(f'{k}: ours {m.group(1) if m else None}, header {define(gmc_m, h)}')

    # the ring id decoding of the handlers
    if '(entry->ring_id & 0x0c) >> 2' not in gfx or '(entry->ring_id & 0x03) >> 0' not in gfx or '(entry->ring_id & 0x70) >> 4' not in gfx:
        fail('gfx_v8_0 ring id decoding changed')
    if '(entry->ring_id & 0x3) >> 0' not in sdma or '(entry->ring_id & 0xc) >> 2' not in sdma:
        fail('sdma_v3_0 ring id decoding changed')
    for expr in ('(vector.ring_id & 0x0c) >> 2', '(vector.ring_id & 0x03) >> 0', '(vector.ring_id & 0x70) >> 4', 'vector.ring_id & 0x3', '(vector.ring_id & 0xc) >> 2'):
        if expr not in cpp:
            fail('AmdIrq.cpp lacks ' + expr)
    if bad:
        sys.exit(1)
    print('AmdIrq matches the driver: %d source ids, 4 registers, %d masks, ring id decoding' % (len(want), len(masks)))


bad = False
main()
