#!/usr/bin/env python3
"""Checks the engine recovery of the Vega M (AmdGfx8::recover, AmdGfx8Compute::deactivate_queues, AmdSdma3::recover in src/Hardware/GraphicsProcessingUnit/AMD) against the Linux driver's
soft reset functions they were ported from.

  check_reset_port.py path/to/gfx_v8_0.c path/to/sdma_v3_0.c path/to/gfx_8_0_sh_mask.h path/to/oss_3_0_sh_mask.h path/to/gmc_8_1_sh_mask.h

(kernel tag v6.12). It compares the status bits gfx_v8_0_check_soft_reset looks at, the reset bits it sets for each, the order of the pre/soft/post steps, the sequence of the reset
pulses of gfx_v8_0_soft_reset and sdma_v3_0_soft_reset, and the register field masks.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Hardware', 'GraphicsProcessingUnit', 'AMD')
bad = False


def squash(text):
    return re.sub(r'\s+', ' ', text)


def fail(text):
    global bad
    bad = True
    print('FAIL:', text)


def function_body(src, name):
    return squash(raw_function_body(src, name))


def raw_function_body(src, name):
    m = re.search(r'\n(?:static )?\w[\w \*]*\b' + re.escape(name) + r'\([^;{]*\)\s*\{', src)
    if not m:
        sys.exit('function not found: ' + name)
    depth, i = 0, m.end() - 1
    for j in range(i, len(src)):
        depth += (src[j] == '{') - (src[j] == '}')
        if depth == 0:
            return src[i:j + 1]
    sys.exit('unbalanced: ' + name)


def define(src, name):
    m = re.search(r'#define\s+' + re.escape(name) + r'\s+(0x[0-9a-fA-F]+|\d+)', src)
    if not m:
        sys.exit('not in header: ' + name)
    return int(m.group(1), 0)


def order(text, needles):
    """Each needle is found after the one before it."""
    at = 0
    for n in needles:
        i = text.find(n, at)
        if i < 0:
            return False
        at = i + len(n)
    return True





def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    gfx, sdma, gfx_m, oss_m, gmc_m = [open(a).read() for a in sys.argv[1:]]
    ours = open(os.path.join(ROOT, 'AmdGfx8.cpp')).read()
    compute = open(os.path.join(ROOT, 'AmdGfx8Compute.cpp')).read()
    sdma_ours = open(os.path.join(ROOT, 'AmdSdma3.cpp')).read()

    # gfx_v8_0_check_soft_reset: the same status bits, in the same registers
    check = function_body(gfx, 'gfx_v8_0_check_soft_reset')
    mine = ours[ours.index('void AmdGfx8::check_soft_reset'):ours.index('AmdGfx8::Result AmdGfx8::recover')]
    want = set(re.findall(r'GRBM_STATUS__(\w+)_MASK', check))
    got = set(re.findall(r'GRBM_STATUS__(\w+)_MASK', mine))
    if want != got:
        fail('GRBM_STATUS bits: driver %s, ours %s' % (sorted(want), sorted(got)))
    # (the GRBM_STATUS2 field names are spelled with REG_GET_FIELD in the driver)
    want = set(re.findall(r'REG_GET_FIELD\(tmp, GRBM_STATUS2, (\w+)\)', check))
    got = set(re.findall(r'GRBM_STATUS2__(\w+)_MASK', mine))
    if want != got:
        fail('GRBM_STATUS2 fields: driver %s, ours %s' % (sorted(want), sorted(got)))
    want = set(re.findall(r'REG_GET_FIELD\(tmp, SRBM_STATUS, (\w+)\)', check))
    got = set(re.findall(r'SRBM_STATUS__(\w+)_MASK', mine))
    if want != got:
        fail('SRBM_STATUS fields: driver %s, ours %s' % (sorted(want), sorted(got)))
    # the reset bits set for them
    for field in ('SOFT_RESET_CP', 'SOFT_RESET_GFX', 'SOFT_RESET_RLC', 'SOFT_RESET_CPF', 'SOFT_RESET_CPC', 'SOFT_RESET_CPG'):
        if 'GRBM_SOFT_RESET, %s, 1' % field not in check or 'GRBM_SOFT_RESET__%s_MASK' % field not in mine:
            fail('check_soft_reset: ' + field)
    for field in ('SOFT_RESET_GRBM', 'SOFT_RESET_SEM'):
        if 'SRBM_SOFT_RESET, %s, 1' % field not in check or 'SRBM_SOFT_RESET__%s_MASK' % field not in mine:
            fail('check_soft_reset: ' + field)

    # pre / soft / post: the steps and their order
    pre = function_body(gfx, 'gfx_v8_0_pre_soft_reset')
    soft = function_body(gfx, 'gfx_v8_0_soft_reset')
    post = function_body(gfx, 'gfx_v8_0_post_soft_reset')
    if not order(pre, ['rlc.funcs->stop', 'gfx_v8_0_cp_gfx_enable(adev, false)', 'gfx_v8_0_deactivate_hqd(adev, 2)', 'gfx_v8_0_cp_compute_enable(adev, false)']):
        fail('gfx_v8_0_pre_soft_reset steps changed')
    if not order(post, ['gfx_v8_0_deactivate_hqd(adev, 2)', 'gfx_v8_0_kiq_resume', 'gfx_v8_0_kcq_resume', 'gfx_v8_0_cp_gfx_resume', 'gfx_v8_0_cp_test_all_rings', 'rlc.funcs->start']):
        fail('gfx_v8_0_post_soft_reset steps changed')
    recover = ours[ours.index('AmdGfx8::Result AmdGfx8::recover'):ours.index('void AmdGfx8::stop()')]
    if not order(recover, ['rlc_stop(', 'cp_gfx_enable(false)', 'deactivate_queues(', 'mmCP_MEC_CNTL', 'mmGMCON_DEBUG', 'mmGRBM_SOFT_RESET', 'mmSRBM_SOFT_RESET', 'deactivate_queues(', 'kiq_resume()',
                          'kcq_resume()', 'cp_resume_ring(', 'test_ring(', 'test_rings(', 'rlc_start()']):
        fail('AmdGfx8::recover steps differ from the driver\'s')
    # gfx_v8_0_soft_reset: GMCON_DEBUG stall and clear, the GRBM pulse, the SRBM pulse, GMCON_DEBUG released
    if not order(soft, ['mmGMCON_DEBUG', 'GFX_STALL, 1', 'GFX_CLEAR, 1', 'tmp |= grbm_soft_reset', 'tmp &= ~grbm_soft_reset', 'tmp |= srbm_soft_reset',
                        'tmp &= ~srbm_soft_reset', 'GFX_STALL, 0', 'GFX_CLEAR, 0']):
        fail('gfx_v8_0_soft_reset sequence changed')
    if not order(recover, ['GMCON_DEBUG__GFX_STALL_MASK | GMCON_DEBUG__GFX_CLEAR_MASK', 'tmp |= grbm_soft_reset', 'tmp &= ~grbm_soft_reset', 'tmp |= srbm_soft_reset', 'tmp &= ~srbm_soft_reset',
                          '~(GMCON_DEBUG__GFX_STALL_MASK | GMCON_DEBUG__GFX_CLEAR_MASK)']):
        fail('AmdGfx8::recover reset pulses differ from the driver\'s')
    # deactivate_hqd
    hqd = function_body(gfx, 'gfx_v8_0_deactivate_hqd')
    mine = compute[compute.index('void AmdGfx8Compute::deactivate_queues'):compute.index('void AmdGfx8Compute::clear_queue_state')]
    for token in ('mmCP_HQD_ACTIVE', 'mmCP_HQD_DEQUEUE_REQUEST', 'mmCP_HQD_PQ_RPTR', 'mmCP_HQD_PQ_WPTR'):
        if token not in hqd or token not in mine:
            fail('deactivate_hqd: ' + token)
    if not order(mine, ['mmCP_HQD_ACTIVE', 'mmCP_HQD_DEQUEUE_REQUEST', 'mmCP_HQD_ACTIVE', 'mmCP_HQD_DEQUEUE_REQUEST, 0', 'mmCP_HQD_PQ_RPTR, 0', 'mmCP_HQD_PQ_WPTR, 0']):
        fail('deactivate_queues order')
    if 'DEQUEUE_REQ, req' not in hqd or '(2u << CP_HQD_DEQUEUE_REQUEST__DEQUEUE_REQ__SHIFT)' not in mine:
        fail('the dequeue request type (2) differs')

    # the masks
    for name, header, text in (('SOFT_RESET_CP', 'GRBM_SOFT_RESET__SOFT_RESET_CP_MASK', gfx_m), ('SOFT_RESET_SDMA', 'SRBM_SOFT_RESET__SOFT_RESET_SDMA_MASK', oss_m)):
        pass
    for header, text in (('GRBM_SOFT_RESET__SOFT_RESET_CP_MASK', gfx_m), ('GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK', gfx_m), ('GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK', gfx_m),
                         ('GMCON_DEBUG__GFX_STALL_MASK', gmc_m), ('GMCON_DEBUG__GFX_CLEAR_MASK', gmc_m), ('SRBM_SOFT_RESET__SOFT_RESET_SDMA_MASK', oss_m),
                         ('SRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK', oss_m), ('SRBM_SOFT_RESET__SOFT_RESET_GRBM_MASK', oss_m)):
        value = define(text, header)
        inc = open(os.path.join(ROOT, 'AmdGfx8Registers.inc' if 'SDMA' not in header else 'AmdSdma3Registers.inc')).read()
        m = re.search(re.escape(header) + r' = (0x[0-9a-fA-F]+)', inc)
        if not m or int(m.group(1), 16) != value:
            fail('%s: ours %s, header %d' % (header, m.group(1) if m else None, value))

    # sdma_v3_0: the bits, the steps, the pulse
    check = function_body(sdma, 'sdma_v3_0_check_soft_reset')
    if 'SOFT_RESET_SDMA_MASK' not in check or 'SOFT_RESET_SDMA1_MASK' not in check or 'SDMA_BUSY_MASK' not in check:
        fail('sdma_v3_0_check_soft_reset changed')
    pre = function_body(sdma, 'sdma_v3_0_pre_soft_reset')
    post = function_body(sdma, 'sdma_v3_0_post_soft_reset')
    soft = function_body(sdma, 'sdma_v3_0_soft_reset')
    if not order(pre, ['sdma_v3_0_ctx_switch_enable(adev, false)', 'sdma_v3_0_enable(adev, false)']) or not order(post, ['sdma_v3_0_gfx_resume', 'sdma_v3_0_rlc_resume']):
        fail('sdma pre/post steps changed')
    if not order(soft, ['tmp |= srbm_soft_reset', 'WREG32(mmSRBM_SOFT_RESET, tmp)', 'tmp = RREG32(mmSRBM_SOFT_RESET)', 'udelay(50)', 'tmp &= ~srbm_soft_reset', 'WREG32(mmSRBM_SOFT_RESET, tmp)',
                        'tmp = RREG32(mmSRBM_SOFT_RESET)', 'udelay(50)']):
        fail('sdma_v3_0_soft_reset sequence changed')
    mine = sdma_ours[sdma_ours.index('AmdSdma3::Result AmdSdma3::recover'):sdma_ours.index('void AmdSdma3::stop()')]
    if not order(mine, ['ctx_switch_enable(false)', 'enable(false)', 'tmp |= srbm_soft_reset', 'wr(mmSRBM_SOFT_RESET, tmp)', 'tmp = rd(mmSRBM_SOFT_RESET)', 'delay_us(50)', 'tmp &= ~srbm_soft_reset',
                       'wr(mmSRBM_SOFT_RESET, tmp)', 'tmp = rd(mmSRBM_SOFT_RESET)', 'delay_us(50)', 'start(memory']):
        fail('AmdSdma3::recover differs from the driver\'s')
    if 'SOFT_RESET_SDMA_MASK | SRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK' not in mine:
        fail('AmdSdma3::recover resets other bits')
    if bad:
        sys.exit(1)
    print('recovery matches the driver: status bits, reset bits, pre/soft/post order, the reset pulses, the HQD dequeue, the SDMA reset')


main()
