#!/usr/bin/env python3
"""Checks the interrupt source ids and fault registers of src/Hardware/GraphicsProcessingUnit/AMD/AmdIrq.{hpp,cpp} against the Linux driver they were taken from.

  check_irq_port.py path/to/ivsrcid_vislands30.h path/to/gmc_8_1_d.h path/to/gmc_8_1_sh_mask.h path/to/gfx_v8_0.c path/to/sdma_v3_0.c path/to/gmc_v8_0.c path/to/gfx_8_0_d.h path/to/gfx_8_0_sh_mask.h path/to/dce_v11_0.c vid.h

(from kernel tag v6.12, drivers/gpu/drm/amd/include/ivsrcid/, include/asic_reg/gmc/, include/asic_reg/gca/ and amdgpu/). It checks that the source ids are the ones the driver registers with
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
    if len(sys.argv) != 11:
        sys.exit(__doc__)
    ivs, gmc_d, gmc_m, gfx, sdma, gmc_c, gfx_d, gfx_m, dce_c, vid_h = [open(a).read() for a in sys.argv[1:]]
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
        'kSrcSqInterruptMsg': define(ivs, 'VISLANDS30_IV_SRCID_SQ_INTERRUPT_MSG'),
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

    check_sq(gfx, gfx_d, gfx_m, cpp)
    check_display(dce_c, vid_h, hpp, cpp)
    check_enables(gfx, sdma, gmc_c)
    check_ecc_and_sq_enables(gfx, gfx_m)
    if bad:
        sys.exit(1)
    print('AmdIrq matches the driver: %d source ids, 4 registers, %d masks, ring id decoding, the sources\' enables' % (len(want), len(masks)))


def function_body(src, name):
    m = re.search(r'\n(?:static )?\w[\w \*]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', src)
    if not m:
        sys.exit('function not found: ' + name)
    depth, i = 0, m.end() - 1
    for j in range(i, len(src)):
        depth += (src[j] == '{') - (src[j] == '}')
        if depth == 0:
            return src[i:j + 1]
    sys.exit('unbalanced: ' + name)


def check_display(dce, vid, hpp, cpp):
    """The display interrupts: source ids and data decoding of dce_v11_0.c against AmdIrq, the acknowledgements and pin set-up against AmdDce11.cpp (whose register numbers are generated
    from the headers), and the register block offsets of vid.h."""
    dcpp = open(os.path.join(ROOT, 'AmdDce11.cpp')).read()
    sw = function_body(dce, 'dce_v11_0_sw_init')
    if 'AMDGPU_IRQ_CLIENTID_LEGACY, i + 1, &adev->crtc_irq' not in sw or 'i = VISLANDS30_IV_SRCID_D1_GRPH_PFLIP; i < 20; i += 2' not in sw or \
            'VISLANDS30_IV_SRCID_HOTPLUG_DETECT_A, &adev->hpd_irq' not in sw:
        fail('dce_v11_0_sw_init registers other sources')
    ours = {m.group(1): int(m.group(2)) for m in re.finditer(r'\b(kSrc\w+) = (\d+)', hpp)}
    if ours.get('kSrcDisplayCrtcFirst') != 1 or ours.get('kSrcPageFlipLast') != 18:
        fail('display source id range')
    # (the pageflip source ids start at VISLANDS30_IV_SRCID_D1_GRPH_PFLIP, the hot plug at HOTPLUG_DETECT_A: read from the ivsrcid header by the checks above for the others)
    crtc = function_body(dce, 'dce_v11_0_crtc_irq')
    if 'unsigned crtc = entry->src_id - 1;' not in crtc or 'case 0: /* vblank */' not in crtc or 'case 1: /* vline */' not in crtc:
        fail('crtc interrupt decoding changed')
    if '(entry->src_id - 8) >> 1' not in function_body(dce, 'dce_v11_0_pageflip_irq'):
        fail('page flip controller decoding changed')
    hpd = function_body(dce, 'dce_v11_0_hpd_irq')
    if 'hpd = entry->src_data[0];' not in hpd or 'entry->src_data[0] >= adev->mode_info.num_hpd' not in hpd:
        fail('hot plug pin decoding changed')
    # hpd_init order: DC_HPD_EN, the filter delays, ack, polarity, interrupt on (amdgpu_irq_get)
    init = function_body(dce, 'dce_v11_0_hpd_init')
    order = [init.find(x) for x in ('DC_HPD_CONTROL, DC_HPD_EN, 1', 'DC_HPD_CONNECT_INT_DELAY', 'DC_HPD_DISCONNECT_INT_DELAY', 'dce_v11_0_hpd_int_ack', 'dce_v11_0_hpd_set_polarity', 'amdgpu_irq_get')]
    if min(order) < 0 or order != sorted(order):
        fail('hpd_init order changed')
    mine = function_body(dcpp, 'hpd_init') if False else dcpp[dcpp.index('void Interrupts::hpd_init'):dcpp.index('void Interrupts::hpd_fini')]
    order = [mine.find(x) for x in ('DC_HPD_CONTROL, DC_HPD_EN, 1', 'DC_HPD_CONNECT_INT_DELAY', 'DC_HPD_DISCONNECT_INT_DELAY', 'ack_hpd(pin)', 'hpd_set_polarity(pin)', 'hpd_set_interrupt(pin, true)')]
    if min(order) < 0 or order != sorted(order):
        fail('Interrupts::hpd_init order differs from the driver')
    if 'connected ? 0 : 1' not in dcpp or 'DC_HPD_INT_POLARITY, 0);' not in function_body(dce, 'dce_v11_0_hpd_set_polarity'):
        fail('polarity')
    for name, delay in (('AMDGPU_HPD_CONNECT_INT_DELAY_IN_MS', 50), ('AMDGPU_HPD_DISCONNECT_INT_DELAY_IN_MS', 10)):
        pass
    if 'kHpdConnectDelayMs = 50, kHpdDisconnectDelayMs = 10' not in open(os.path.join(ROOT, 'AmdDce11.hpp')).read():
        fail('hpd filter delays')
    # the offsets of vid.h
    for i, off in enumerate(('0x1898', '0x18a0', '0x18a8', '0x18b0', '0x18b8', '0x18c0')):
        if not re.search(r'HPD%d_REGISTER_OFFSET\s+\(%s - 0x1898\)' % (i, off), vid) or ('%s - 0x1898' % off) not in dcpp:
            fail('HPD%d offset' % i)
    for i, off in enumerate(('0x1b9c', '0x1d9c', '0x1f9c', '0x419c', '0x439c', '0x459c')):
        if not re.search(r'CRTC%d_REGISTER_OFFSET\s+\(%s - 0x1b9c\)' % (i, off), vid) or ('%s - 0x1b9c' % off) not in dcpp:
            fail('CRTC%d offset' % i)
    # the status table of the driver and ours name the same registers and bits
    table = re.findall(r'\.reg = (mm\w+),\s*\.vblank = (\w+),\s*\.vline = (\w+),\s*\.hpd = (\w+)', dce)
    mine = re.findall(r'\{(mmDISP\w+), (\w+),\s*(\w+),\s*(\w+)\}', dcpp)
    if table != mine or len(table) != 6:
        fail('interrupt_status_offsets differs (%d vs %d rows)' % (len(table), len(mine)))
    # the acknowledgements
    for fn, field in (('dce_v11_0_hpd_int_ack', 'DC_HPD_INT_ACK'), ('dce_v11_0_crtc_vblank_int_ack', 'VBLANK_ACK'), ('dce_v11_0_crtc_vline_int_ack', 'VLINE_ACK')):
        if field + ', 1' not in function_body(dce, fn) or field + ', 1' not in dcpp:
            fail(fn)
    if 'GRPH_INTERRUPT_STATUS__GRPH_PFLIP_INT_CLEAR_MASK' not in function_body(dce, 'dce_v11_0_pageflip_irq') or 'GRPH_PFLIP_INT_CLEAR_MASK' not in dcpp:
        fail('page flip acknowledgement')
    for fn, field in (('dce_v11_0_set_pageflip_irq_state', 'GRPH_PFLIP_INT_MASK_MASK'), ('dce_v11_0_set_crtc_vblank_interrupt_state', 'VBLANK_INTERRUPT_MASK'),
                      ('dce_v11_0_set_hpd_irq_state', 'DC_HPD_INT_EN')):
        if field not in function_body(dce, fn) or field not in dcpp:
            fail(fn)


def check_sq(gfx, gfx_d, gfx_m, cpp):
    """The SQ message decoding of AmdIrq.cpp against gfx_v8_0_parse_sq_irq and the register headers."""
    for name, header in (('mmGRBM_GFX_INDEX', gfx_d), ('mmSQ_EDC_INFO', gfx_d)):
        m = re.search(name + r' = (0x[0-9a-fA-F]+)', cpp)
        if not m or int(m.group(1), 16) != define(header, name):
            fail(f'{name}: ours {m.group(1) if m else None}, header {define(header, name)}')
    def shift(mask):
        return mask.bit_length() - bin(mask).count('1') if mask else 0
    fields = {   # constant name -> (header mask name, shift constant or None)
        'kSqEdcSourceMask': 'SQ_EDC_INFO__SOURCE_MASK', 'kSqSeIdMask': 'SQ_INTERRUPT_WORD_CMN__SE_ID_MASK', 'kSqEncodingMask': 'SQ_INTERRUPT_WORD_CMN__ENCODING_MASK',
        'kSqWaveShIdMask': 'SQ_INTERRUPT_WORD_WAVE__SH_ID_MASK', 'kSqWavePrivMask': 'SQ_INTERRUPT_WORD_WAVE__PRIV_MASK', 'kSqWaveVmIdMask': 'SQ_INTERRUPT_WORD_WAVE__VM_ID_MASK',
        'kSqWaveWaveIdMask': 'SQ_INTERRUPT_WORD_WAVE__WAVE_ID_MASK', 'kSqWaveSimdIdMask': 'SQ_INTERRUPT_WORD_WAVE__SIMD_ID_MASK', 'kSqWaveCuIdMask': 'SQ_INTERRUPT_WORD_WAVE__CU_ID_MASK',
    }
    for k, h in fields.items():
        m = re.search(r'\b' + k + r' = (0x[0-9a-fA-F]+)', cpp)
        if not m or int(m.group(1), 16) != define(gfx_m, h):
            fail(f'{k}: ours {m.group(1) if m else None}, header {define(gfx_m, h)}')
        if k == 'kSqWavePrivMask':
            continue   # (a flag: no shift)
        sm = re.search(r'\b' + k.replace('Mask', 'Shift') + r' = (0x[0-9a-fA-F]+|\d+)', cpp)
        if not sm or int(sm.group(1), 0) != shift(define(gfx_m, h)):
            fail(f'{k.replace("Mask", "Shift")}: ours {sm.group(1) if sm else None}, from the mask {shift(define(gfx_m, h))}')
    auto = 0
    for f in ('THREAD_TRACE', 'WLT', 'THREAD_TRACE_BUF_FULL', 'REG_TIMESTAMP', 'CMD_TIMESTAMP', 'HOST_CMD_OVERFLOW', 'HOST_REG_OVERFLOW', 'IMMED_OVERFLOW'):
        auto |= define(gfx_m, 'SQ_INTERRUPT_WORD_AUTO__%s_MASK' % f)
    m = re.search(r'kSqAutoFlagsMask = (0x[0-9a-fA-F]+)', cpp)
    if not m or int(m.group(1), 16) != auto:
        fail('kSqAutoFlagsMask is not the union of the SQ_INTERRUPT_WORD_AUTO flags')
    for idx, bit in (('INSTANCE_INDEX', 'kGrbmInstanceIndexShift'), ('SH_INDEX', 'kGrbmShIndexShift'), ('SE_INDEX', 'kGrbmSeIndexShift')):
        m = re.search(bit + r' = (0x[0-9a-fA-F]+)', cpp)
        if not m or int(m.group(1), 16) != shift(define(gfx_m, 'GRBM_GFX_INDEX__%s_MASK' % idx)):
            fail(bit)
    broadcast = sum(define(gfx_m, 'GRBM_GFX_INDEX__%s_BROADCAST_WRITES_MASK' % b) for b in ('SH', 'INSTANCE', 'SE'))
    if '0x20000000 | 0x40000000 | 0x80000000' not in cpp or broadcast != 0xE0000000:
        fail('GRBM_GFX_INDEX broadcast bits')
    # the driver's order: select, read SQ_EDC_INFO, select everything again; the cu id is the instance
    body = function_body(gfx, 'gfx_v8_0_parse_sq_irq')
    if 'gfx_v8_0_select_se_sh(adev, se_id, sh_id, cu_id, 0)' not in body or 'RREG32(mmSQ_EDC_INFO)' not in body or '0xffffffff, 0xffffffff, 0xffffffff' not in body:
        fail('gfx_v8_0_parse_sq_irq no longer reads SQ_EDC_INFO that way')
    names = re.findall(r'"SQ_EDC_INFO_SOURCE_(\w+):', gfx)
    if names != ['INVALID', 'INST', 'SGPR', 'VGPR', 'LDS', 'GDS', 'TA']:
        fail('sq_edc_source_names changed: %s' % names)


def check_ecc_and_sq_enables(gfx, gfx_m):
    ours = open(os.path.join(ROOT, 'AmdGfx8.cpp')).read()
    body = function_body(gfx, 'gfx_v8_0_set_cp_ecc_int_state')
    regs = re.findall(r'WREG32_FIELD\((\w+), CP_ECC_ERROR_INT_ENABLE', body)
    if len(regs) != 13:
        fail('gfx_v8_0_set_cp_ecc_int_state: %d registers' % len(regs))
    for r in regs:
        mask = '%s__CP_ECC_ERROR_INT_ENABLE_MASK' % r
        define(gfx_m, mask)   # exists
        if 'mm%s,' % r not in ours or mask not in ours:
            fail('AmdGfx8.cpp lacks ' + r)
    masks = {define(gfx_m, '%s__CP_ECC_ERROR_INT_ENABLE_MASK' % r) for r in regs}
    if masks != {0x4000}:
        fail('the ECC enable bit differs between registers (the test assumes 0x4000)')
    sq = function_body(gfx, 'gfx_v8_0_set_sq_int_state')
    if 'WREG32_FIELD(SQ_INTERRUPT_MSG_CTRL, STALL' not in sq or 'AMDGPU_IRQ_STATE_ENABLE:\n\t\tenable_flag = 0' not in sq or 'SQ_INTERRUPT_MSG_CTRL__STALL_MASK' not in ours:
        fail('SQ message stall')


def check_enables(gfx, sdma, gmc_c):
    """The registers and fields the sources are enabled with (the *_set_*_state functions), against the ports in AmdGfx8.cpp, AmdGfx8Compute.cpp, AmdSdma3.cpp and AmdGmc8.cpp."""
    ours = {f: open(os.path.join(ROOT, f)).read() for f in ('AmdGfx8.cpp', 'AmdGfx8Compute.cpp', 'AmdSdma3.cpp', 'AmdGmc8.cpp')}
    # graphics ring: CP_INT_CNTL_RING0 TIME_STAMP / PRIV_REG / PRIV_INSTR
    for fn, field in (('gfx_v8_0_set_gfx_eop_interrupt_state', 'TIME_STAMP_INT_ENABLE'), ('gfx_v8_0_set_priv_reg_fault_state', 'PRIV_REG_INT_ENABLE'),
                      ('gfx_v8_0_set_priv_inst_fault_state', 'PRIV_INSTR_INT_ENABLE')):
        if 'WREG32_FIELD(CP_INT_CNTL_RING0, ' + field not in function_body(gfx, fn):
            fail(fn + ' no longer sets CP_INT_CNTL_RING0.' + field)
        if 'CP_INT_CNTL_RING0__' + field + '_MASK' not in ours['AmdGfx8.cpp']:
            fail('AmdGfx8.cpp lacks CP_INT_CNTL_RING0.' + field)
    # compute pipe: CP_ME1_PIPEn_INT_CNTL, TIME_STAMP_INT_ENABLE of CP_INT_CNTL_RING0, MEC 1 only
    body = function_body(gfx, 'gfx_v8_0_set_compute_eop_interrupt_state')
    for pipe in range(4):
        if 'mmCP_ME1_PIPE%d_INT_CNTL' % pipe not in body or 'mmCP_ME1_PIPE%d_INT_CNTL' % pipe not in ours['AmdGfx8Compute.cpp']:
            fail('compute pipe %d interrupt register' % pipe)
    if 'CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK' not in body or 'CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK' not in ours['AmdGfx8Compute.cpp']:
        fail('compute pipe interrupt field')
    if 'if (me == 1)' not in body or 'compute_me != 1' not in ours['AmdGfx8Compute.cpp']:
        fail('compute pipe interrupts are for MEC 1 only')
    # SDMA: TRAP_ENABLE of SDMA0_CNTL for both engines
    body = function_body(sdma, 'sdma_v3_0_set_trap_irq_state')
    if 'SDMA0_CNTL, TRAP_ENABLE' not in body or 'SDMA0_CNTL, TRAP_ENABLE' not in ours['AmdSdma3.cpp'] or body.count('SDMA1_REGISTER_OFFSET') < 2:
        fail('sdma trap enable')
    # memory controller: the seven *_FAULT_ENABLE_INTERRUPT bits of both contexts
    body = function_body(gmc_c, 'gmc_v8_0_vm_fault_interrupt_state')
    kinds = ('RANGE', 'DUMMY_PAGE', 'PDE0', 'VALID', 'READ', 'WRITE', 'EXECUTE')
    for kind in kinds:
        name = 'VM_CONTEXT1_CNTL__%s_PROTECTION_FAULT_ENABLE_INTERRUPT_MASK' % kind
        if name not in body or name not in ours['AmdGmc8.cpp']:
            fail('memory controller fault enable ' + kind)
    if 'mmVM_CONTEXT0_CNTL' not in body or 'mmVM_CONTEXT1_CNTL' not in body or 'mmVM_CONTEXT0_CNTL' not in ours['AmdGmc8.cpp']:
        fail('memory controller contexts')


bad = False
main()
