/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): sdma_v3_0.c (sdma_v3_0_init_golden_registers, _gfx_stop, _ctx_switch_enable, _enable,
// _gfx_resume, _start, _ring_test_ring, _ring_set_wptr, _ring_insert_nop, _ring_emit_fence, _emit_copy_buffer, _emit_fill_buffer, _hw_fini) and amdgpu_ring.c (amdgpu_ring_commit).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.hpp"
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"

namespace hardware {

namespace {
// The register numbers and field masks this file uses, generated from oss_3_0_d.h and oss_3_0_sh_mask.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSdma3Registers.inc"

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))

// golden_settings_polaris11_a11 of sdma_v3_0.c (the table the Polaris 11, 12 and the VEGAM use): register, owned bits, value.
struct GoldenRegister {
  uint32_t reg, and_mask, or_mask;
};
constexpr GoldenRegister kGolden[] = {
    {mmSDMA0_CHICKEN_BITS, 0xfc910007, 0x00810007},
    {mmSDMA0_CLK_CTRL, 0xff000fff, 0x00000000},
    {mmSDMA0_GFX_IB_CNTL, 0x800f0111, 0x00000100},
    {mmSDMA0_RLC0_IB_CNTL, 0x800f0111, 0x00000100},
    {mmSDMA0_RLC1_IB_CNTL, 0x800f0111, 0x00000100},
    {mmSDMA1_CHICKEN_BITS, 0xfc910007, 0x00810007},
    {mmSDMA1_CLK_CTRL, 0xff000fff, 0x00000000},
    {mmSDMA1_GFX_IB_CNTL, 0x800f0111, 0x00000100},
    {mmSDMA1_RLC0_IB_CNTL, 0x800f0111, 0x00000100},
    {mmSDMA1_RLC1_IB_CNTL, 0x800f0111, 0x00000100},
};

// tonga_sdma_pkt_open.h: SDMA_PKT_HEADER_OP(op) | SDMA_PKT_HEADER_SUB_OP(sub_op) is op | sub_op << 8; SDMA_PKT_NOP_HEADER_COUNT(n) is n << 16.
constexpr uint32_t kOpNop = 0, kOpCopy = 1, kOpWrite = 2, kOpIndirect = 4, kOpFence = 5, kOpTrap = 6, kOpConstFill = 11;
constexpr uint32_t kSubopCopyLinear = 0, kSubopWriteLinear = 0;
constexpr uint32_t header(uint32_t op, uint32_t sub_op) { return op | sub_op << 8; }
constexpr uint32_t kNop = kOpNop;
constexpr uint32_t kTestInitial = 0xCAFEDEAD, kTestValue = 0xDEADBEEF;
constexpr uint64_t kAddressMask = 0xFFFFFFFFFFull;   // the 40-bit address space of the memory controller
// The write-back words of a slot
constexpr uint64_t kRptrWord = 0, kWptrWord = 8, kTestWord = 16, kFenceWord = 24;
// A copy or fill is queued in one go: never more than the ring can take with its padding and the fence.
constexpr uint32_t kFenceDwords = 1 + 4 + 2;   // the hang workaround NOP, the fence packet, the trap
}  // namespace

constexpr uint32_t AmdSdma3::kRegisterOffset[AmdSdma3::kInstances];
constexpr uint32_t AmdSdma3::kDefaultDoorbell[AmdSdma3::kInstances];

const char* AmdSdma3::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::BadArgument: return "bad argument";
    case Result::RingTestFailed: return "ring test failed";
  }
  return "?";
}

void AmdSdma3::delay_us(uint32_t us) {
  const uint64_t begin = clock_.now_us();
  while (clock_.now_us() - begin < us) {}
}

void AmdSdma3::program_golden_registers() {
  for (const GoldenRegister& g : kGolden) {
    uint32_t value = g.or_mask;
    if (g.and_mask != 0xFFFFFFFFu) value = (rd(g.reg) & ~g.and_mask) | g.or_mask;
    wr(g.reg, value);
  }
}

void AmdSdma3::gfx_stop() {
  for (uint32_t i = 0; i < kInstances; i++) {
    uint32_t rb_cntl = rd(mmSDMA0_GFX_RB_CNTL + kRegisterOffset[i]);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 0);
    wr(mmSDMA0_GFX_RB_CNTL + kRegisterOffset[i], rb_cntl);
    uint32_t ib_cntl = rd(mmSDMA0_GFX_IB_CNTL + kRegisterOffset[i]);
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 0);
    wr(mmSDMA0_GFX_IB_CNTL + kRegisterOffset[i], ib_cntl);
  }
}

// (without the phase quantum: the driver only sets it when asked)
void AmdSdma3::ctx_switch_enable(bool enable) {
  for (uint32_t i = 0; i < kInstances; i++) {
    uint32_t f32_cntl = rd(mmSDMA0_CNTL + kRegisterOffset[i]);
    if (enable) {
      f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL, AUTO_CTXSW_ENABLE, 1);
      f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL, ATC_L1_ENABLE, 1);
    } else {
      f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL, AUTO_CTXSW_ENABLE, 0);
      f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL, ATC_L1_ENABLE, 1);
    }

    wr(mmSDMA0_CNTL + kRegisterOffset[i], f32_cntl);
  }
}

void AmdSdma3::enable(bool enable) {
  if (!enable) {
    gfx_stop();
  }

  for (uint32_t i = 0; i < kInstances; i++) {
    uint32_t f32_cntl = rd(mmSDMA0_F32_CNTL + kRegisterOffset[i]);
    if (enable)
      f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, 0);
    else
      f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, 1);
    wr(mmSDMA0_F32_CNTL + kRegisterOffset[i], f32_cntl);
  }
}

void AmdSdma3::ring_write(State& s, uint32_t dword) {
  s.ring.write32(uint64_t{s.wptr} * 4, dword);
  s.wptr = (s.wptr + 1) & (kRingDwords - 1);
}

void AmdSdma3::insert_nop(State& s, uint32_t count, bool burst) {
  if (count && burst) {
    --count;
    ring_write(s, kNop | (count << 16));
  }
  for (uint32_t i = 0; i < count; ++i) ring_write(s, kNop);   // amdgpu_ring_fill
}

// amdgpu_ring_commit + sdma_v3_0_ring_set_wptr (doorbell form): pad, flush the host data path, the write pointer in bytes to the write-back word and the doorbell.
void AmdSdma3::ring_commit(uint32_t instance) {
  State& s = state_[instance];
  const uint32_t count = (kAlignMask + 1 - (s.wptr & kAlignMask)) & kAlignMask;
  insert_nop(s, count, config_.burst_nop[instance]);
  amd_gmc8_flush_hdp(registers_);
  s.wb.write32(kWptrWord, s.wptr << 2);
  memory_.doorbells.write32(uint64_t{config_.doorbell[instance]} * 4, s.wptr << 2);
}

bool AmdSdma3::gfx_resume(uint32_t timeout_us) {
  for (uint32_t i = 0; i < kInstances; i++) {
    State& ring = state_[i];
    for (uint64_t o = 0; o < kRingBytes; o += 4) ring.ring.write32(o, 0);   // amdgpu_ring_clear_ring

    for (uint32_t j = 0; j < 16; j++) {
      // vi_srbm_select(adev, 0, 0, 0, j)
      wr(mmSRBM_GFX_CNTL, REG_SET_FIELD(0, SRBM_GFX_CNTL, VMID, j));
      // SDMA GFX
      wr(mmSDMA0_GFX_VIRTUAL_ADDR + kRegisterOffset[i], 0);
      wr(mmSDMA0_GFX_APE1_CNTL + kRegisterOffset[i], 0);
    }
    wr(mmSRBM_GFX_CNTL, 0);

    wr(mmSDMA0_TILING_CONFIG + kRegisterOffset[i], config_.gb_addr_config & 0x70);

    wr(mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL + kRegisterOffset[i], 0);

    // Set ring buffer size in dwords
    uint32_t rb_bufsz = 0;
    while ((1u << rb_bufsz) < kRingBytes / 4) ++rb_bufsz;
    uint32_t rb_cntl = rd(mmSDMA0_GFX_RB_CNTL + kRegisterOffset[i]);
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_SIZE, rb_bufsz);
    wr(mmSDMA0_GFX_RB_CNTL + kRegisterOffset[i], rb_cntl);

    // Initialize the ring buffer's read and write pointers
    ring.wptr = 0;
    wr(mmSDMA0_GFX_RB_RPTR + kRegisterOffset[i], 0);
    ring.wb.write32(kWptrWord, 0);   // sdma_v3_0_ring_set_wptr with a doorbell
    memory_.doorbells.write32(uint64_t{config_.doorbell[i]} * 4, 0);
    wr(mmSDMA0_GFX_IB_RPTR + kRegisterOffset[i], 0);
    wr(mmSDMA0_GFX_IB_OFFSET + kRegisterOffset[i], 0);

    // set the wb address whether it's enabled or not
    const uint64_t rptr_gpu_addr = ring.wb_gpu + kRptrWord;
    wr(mmSDMA0_GFX_RB_RPTR_ADDR_HI + kRegisterOffset[i], static_cast<uint32_t>(rptr_gpu_addr >> 32) & 0xFFFFFFFF);
    wr(mmSDMA0_GFX_RB_RPTR_ADDR_LO + kRegisterOffset[i], static_cast<uint32_t>(rptr_gpu_addr) & 0xFFFFFFFC);

    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);

    wr(mmSDMA0_GFX_RB_BASE + kRegisterOffset[i], static_cast<uint32_t>(ring.ring_gpu >> 8));
    wr(mmSDMA0_GFX_RB_BASE_HI + kRegisterOffset[i], static_cast<uint32_t>(ring.ring_gpu >> 40));

    uint32_t doorbell = rd(mmSDMA0_GFX_DOORBELL + kRegisterOffset[i]);

    doorbell = REG_SET_FIELD(doorbell, SDMA0_GFX_DOORBELL, OFFSET, config_.doorbell[i]);
    doorbell = REG_SET_FIELD(doorbell, SDMA0_GFX_DOORBELL, ENABLE, 1);
    wr(mmSDMA0_GFX_DOORBELL + kRegisterOffset[i], doorbell);

    // setup the wptr shadow polling
    const uint64_t wptr_gpu_addr = ring.wb_gpu + kWptrWord;

    wr(mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO + kRegisterOffset[i], static_cast<uint32_t>(wptr_gpu_addr));
    wr(mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI + kRegisterOffset[i], static_cast<uint32_t>(wptr_gpu_addr >> 32));
    uint32_t wptr_poll_cntl = rd(mmSDMA0_GFX_RB_WPTR_POLL_CNTL + kRegisterOffset[i]);
    wptr_poll_cntl = REG_SET_FIELD(wptr_poll_cntl, SDMA0_GFX_RB_WPTR_POLL_CNTL, ENABLE, 0);
    wr(mmSDMA0_GFX_RB_WPTR_POLL_CNTL + kRegisterOffset[i], wptr_poll_cntl);

    // enable DMA RB
    rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 1);
    wr(mmSDMA0_GFX_RB_CNTL + kRegisterOffset[i], rb_cntl);

    uint32_t ib_cntl = rd(mmSDMA0_GFX_IB_CNTL + kRegisterOffset[i]);
    ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 1);
    // enable DMA IBs
    wr(mmSDMA0_GFX_IB_CNTL + kRegisterOffset[i], ib_cntl);
  }

  // unhalt the MEs
  enable(true);
  // enable sdma ring preemption
  ctx_switch_enable(true);

  for (uint32_t i = 0; i < kInstances; i++) {
    if (!test_ring(i, timeout_us)) {
      failed_instance_ = static_cast<int>(i);
      return false;
    }
  }
  return true;
}

// sdma_v3_0_ring_test_ring: the engine writes a word of memory.
bool AmdSdma3::test_ring(uint32_t instance, uint32_t timeout_us) {
  State& s = state_[instance];
  const uint64_t gpu_addr = s.wb_gpu + kTestWord;
  s.wb.write32(kTestWord, kTestInitial);

  ring_write(s, header(kOpWrite, kSubopWriteLinear));
  ring_write(s, static_cast<uint32_t>(gpu_addr));
  ring_write(s, static_cast<uint32_t>(gpu_addr >> 32));
  ring_write(s, 1);   // SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(1)
  ring_write(s, kTestValue);
  ring_commit(instance);

  const uint64_t begin = clock_.now_us();
  do {
    if (s.wb.read32(kTestWord) == kTestValue) return true;
  } while (clock_.now_us() - begin < timeout_us);
  return false;
}

AmdSdma3::Result AmdSdma3::start(const Memory& memory, uint32_t timeout_us) {
  failed_instance_ = -1;
  running_ = false;
  const uint64_t needed = kInstances * kSlotBytes;
  if (!memory.block.valid() || memory.block.length() < needed || (memory.gpu_address & 4095) != 0 || (memory.block.address() & 4095) != 0 ||
      memory.gpu_address > kAddressMask - needed || !memory.doorbells.valid())
    return Result::BadArgument;
  for (uint32_t i = 0; i < kInstances; ++i)
    if (config_.doorbell[i] > 0x1FFFFF || memory.doorbells.length() < (uint64_t{config_.doorbell[i]} + 1) * 4) return Result::BadArgument;
  memory_ = memory;
  for (uint32_t i = 0; i < kInstances; ++i) {
    State& s = state_[i];
    s = State();
    s.ring_gpu = memory.gpu_address + i * kSlotBytes;
    s.wb_gpu = s.ring_gpu + kRingBytes;
    s.ring = memory.block.subregion(i * kSlotBytes, kRingBytes);
    s.wb = memory.block.subregion(i * kSlotBytes + kRingBytes, 4096);
    for (uint64_t o = 0; o < 4096; o += 4) s.wb.write32(o, 0);
  }
  program_golden_registers();
  // sdma_v3_0_start: disable sdma engine before programing it
  ctx_switch_enable(false);
  enable(false);
  // start the gfx rings and rlc compute queues
  if (!gfx_resume(timeout_us)) return Result::RingTestFailed;
  running_ = true;
  return Result::Ok;
}

void AmdSdma3::set_trap_interrupts(bool enable) {
  trap_interrupts_ = enable;
  for (uint32_t i = 0; i < kInstances; ++i) {
    uint32_t sdma_cntl = rd(mmSDMA0_CNTL + kRegisterOffset[i]);
    sdma_cntl = REG_SET_FIELD(sdma_cntl, SDMA0_CNTL, TRAP_ENABLE, enable ? 1 : 0);
    wr(mmSDMA0_CNTL + kRegisterOffset[i], sdma_cntl);
  }
}

AmdSdma3::Result AmdSdma3::recover(uint32_t timeout_us) {
  if (!memory_.block.valid()) return Result::BadArgument;   // never started
  ++recoveries_;
  const uint32_t srbm_soft_reset = SRBM_SOFT_RESET__SOFT_RESET_SDMA_MASK | SRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK;   // (sdma_v3_0_check_soft_reset sets both when either is busy)
  // sdma_v3_0_pre_soft_reset
  ctx_switch_enable(false);
  enable(false);
  running_ = false;
  // sdma_v3_0_soft_reset
  uint32_t tmp = rd(mmSRBM_SOFT_RESET);
  tmp |= srbm_soft_reset;
  wr(mmSRBM_SOFT_RESET, tmp);
  tmp = rd(mmSRBM_SOFT_RESET);
  delay_us(50);
  tmp &= ~srbm_soft_reset;
  wr(mmSRBM_SOFT_RESET, tmp);
  tmp = rd(mmSRBM_SOFT_RESET);
  delay_us(50);   // wait a little for things to settle down
  // sdma_v3_0_post_soft_reset
  const Memory memory = memory_;
  const Result result = start(memory, timeout_us);
  if (result == Result::Ok && trap_interrupts_) set_trap_interrupts(true);
  return result;
}

void AmdSdma3::stop() {
  ctx_switch_enable(false);
  enable(false);
  running_ = false;
}

bool AmdSdma3::can_queue(uint32_t instance, uint64_t dwords) const {
  // Every operation is waited for before the next, so the ring is empty when one starts: it takes anything that fits with its padding (and one dword to tell full from empty).
  return running_ && instance < kInstances && !state_[instance].wedged && dwords + kAlignMask + 1 < kRingDwords;
}

// sdma_v3_0_ring_emit_fence (32-bit) and the commit, then the wait for the fence word.
bool AmdSdma3::finish(uint32_t instance, uint32_t timeout_us) {
  State& s = state_[instance];
  const uint64_t addr = s.wb_gpu + kFenceWord;
  const uint32_t sequence = ++s.sequence;
  // Hang workaround
  ring_write(s, kNop);
  // write the fence
  ring_write(s, header(kOpFence, 0));
  ring_write(s, static_cast<uint32_t>(addr));
  ring_write(s, static_cast<uint32_t>(addr >> 32));
  ring_write(s, sequence);
  // generate an interrupt
  ring_write(s, header(kOpTrap, 0));
  ring_write(s, 0);   // SDMA_PKT_TRAP_INT_CONTEXT_INT_CONTEXT(0)
  ring_commit(instance);
  const uint64_t begin = clock_.now_us();
  do {
    if (s.wb.read32(kFenceWord) == sequence) return true;
  } while (clock_.now_us() - begin < timeout_us);
  s.wedged = true;
  return false;
}

// sdma_v3_0_ring_pad_ib
uint32_t AmdSdma3::pad_ib(uint32_t instance, ArrayView<uint32_t> words, uint32_t count) const {
  if (instance >= kInstances || count > words.size()) return 0;
  const uint32_t pad = (8 - (count & 7)) & 7;
  if (pad > words.size() - count) return 0;
  uint32_t i = 0;
  if (pad && config_.burst_nop[instance]) words[count + i++] = kNop | ((pad - 1) << 16);
  for (; i < pad; ++i) words[count + i] = kNop;
  return count + pad;
}

// sdma_v3_0_ring_emit_ib (VMID 0), then the fence
bool AmdSdma3::submit_ib(uint32_t instance, uint64_t ib_gpu_address, uint32_t dwords, uint32_t timeout_us) {
  if (dwords == 0 || dwords > kMaxIbDwords || (ib_gpu_address & 31) != 0 || ib_gpu_address > kAddressMask || !can_queue(instance, 7 + 6 + kFenceDwords)) return false;
  State& s = state_[instance];
  // IB packet must end on a 8 DW boundary
  insert_nop(s, (2 - s.wptr) & 7, config_.burst_nop[instance]);
  ring_write(s, header(kOpIndirect, 0));   // (| SDMA_PKT_INDIRECT_HEADER_VMID(0))
  // base must be 32 byte aligned
  ring_write(s, static_cast<uint32_t>(ib_gpu_address) & 0xffffffe0);
  ring_write(s, static_cast<uint32_t>(ib_gpu_address >> 32));
  ring_write(s, dwords);
  ring_write(s, 0);
  ring_write(s, 0);
  return finish(instance, timeout_us);
}

bool AmdSdma3::copy(uint32_t instance, uint64_t source, uint64_t destination, uint64_t bytes, uint32_t timeout_us) {
  if (bytes == 0 || source > kAddressMask || destination > kAddressMask || bytes > kAddressMask + 1 - source || bytes > kAddressMask + 1 - destination) return false;
  const uint64_t chunks = (bytes + kMaxChunkBytes - 1) / kMaxChunkBytes;
  if (chunks > kRingDwords || !can_queue(instance, chunks * 7 + kFenceDwords)) return false;
  State& s = state_[instance];
  for (uint64_t done = 0; done < bytes;) {
    const uint32_t count = bytes - done < kMaxChunkBytes ? static_cast<uint32_t>(bytes - done) : kMaxChunkBytes;
    const uint64_t src = source + done, dst = destination + done;
    // sdma_v3_0_emit_copy_buffer
    ring_write(s, header(kOpCopy, kSubopCopyLinear));
    ring_write(s, count);
    ring_write(s, 0);   // src/dst endian swap
    ring_write(s, static_cast<uint32_t>(src));
    ring_write(s, static_cast<uint32_t>(src >> 32));
    ring_write(s, static_cast<uint32_t>(dst));
    ring_write(s, static_cast<uint32_t>(dst >> 32));
    done += count;
  }
  return finish(instance, timeout_us);
}

bool AmdSdma3::fill(uint32_t instance, uint64_t destination, uint32_t value, uint64_t bytes, uint32_t timeout_us) {
  if (bytes == 0 || (bytes & 3) != 0 || destination > kAddressMask || bytes > kAddressMask + 1 - destination) return false;
  const uint64_t chunks = (bytes + kMaxChunkBytes - 1) / kMaxChunkBytes;
  if (chunks > kRingDwords || !can_queue(instance, chunks * 5 + kFenceDwords)) return false;
  State& s = state_[instance];
  for (uint64_t done = 0; done < bytes;) {
    const uint32_t count = bytes - done < kMaxChunkBytes ? static_cast<uint32_t>(bytes - done) : kMaxChunkBytes;
    const uint64_t dst = destination + done;
    // sdma_v3_0_emit_fill_buffer
    ring_write(s, header(kOpConstFill, 0));
    ring_write(s, static_cast<uint32_t>(dst));
    ring_write(s, static_cast<uint32_t>(dst >> 32));
    ring_write(s, value);
    ring_write(s, count);
    done += count;
  }
  return finish(instance, timeout_us);
}

}  // namespace hardware
