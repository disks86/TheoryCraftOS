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


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): tonga_ih.c (tonga_ih_enable_interrupts, _disable_interrupts, _irq_init, _irq_disable,
// _get_wptr, _decode_iv, _set_rptr) and amdgpu_ih.c (amdgpu_ih_ring_init: the ring's write-back words).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIh3.hpp"

namespace hardware {

namespace {
// The register numbers and field masks this file uses, generated from oss_3_0_d.h, oss_3_0_sh_mask.h, bif_5_0_d.h and bif_5_0_sh_mask.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIh3Registers.inc"

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))
#define REG_GET_FIELD(value, reg, field) (((value) & reg##__##field##_MASK) >> reg##__##field##__SHIFT)

constexpr uint32_t kPtrMask = AmdIh3::kRingBytes - 1;   // ih->ptr_mask (bytes)
constexpr uint32_t kMaxDoorbell = 0x1FFFFF;             // IH_DOORBELL_RPTR.OFFSET is 21 bits
// order_base_2(ring_size / 4)
constexpr uint32_t log2_dwords(uint32_t bytes) {
  uint32_t log = 0;
  while ((1u << log) < bytes / 4) ++log;
  return log;
}
}  // namespace

const char* AmdIh3::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::BadArgument: return "bad argument";
  }
  return "?";
}

void AmdIh3::enable_interrupts() {
  uint32_t ih_rb_cntl = rd(mmIH_RB_CNTL);

  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, RB_ENABLE, 1);
  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, ENABLE_INTR, 1);
  wr(mmIH_RB_CNTL, ih_rb_cntl);
  running_ = true;
}

void AmdIh3::disable_interrupts() {
  uint32_t ih_rb_cntl = rd(mmIH_RB_CNTL);

  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, RB_ENABLE, 0);
  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, ENABLE_INTR, 0);
  wr(mmIH_RB_CNTL, ih_rb_cntl);
  // set rptr, wptr to 0
  wr(mmIH_RB_RPTR, 0);
  wr(mmIH_RB_WPTR, 0);
  running_ = false;
  rptr_ = 0;
}

AmdIh3::Result AmdIh3::start(const Memory& memory) {
  const uint64_t ring_end = uint64_t{kRingBytes} + kWritebackBytes;
  if (!memory.ring.valid() || memory.ring.length() < ring_end || (memory.ring_gpu_address & 0xFF) != 0 || memory.ring_gpu_address > 0xFFFFFFFFFFull - ring_end ||
      (memory.ring.address() & 3) != 0 || (memory.dummy_page_gpu_address & 0xFF) != 0 || memory.dummy_page_gpu_address > 0xFFFFFFFFFFull)
    return Result::BadArgument;
  if (config_.use_doorbell && (config_.doorbell_index > kMaxDoorbell || !memory.doorbells.valid() || memory.doorbells.length() < (uint64_t{config_.doorbell_index} + 1) * 4))
    return Result::BadArgument;
  memory_ = memory;
  uint32_t interrupt_cntl, ih_rb_cntl, ih_doorbell_rtpr;

  // disable irqs
  disable_interrupts();

  // setup interrupt control
  wr(mmINTERRUPT_CNTL2, static_cast<uint32_t>(memory.dummy_page_gpu_address >> 8));
  interrupt_cntl = rd(mmINTERRUPT_CNTL);
  // INTERRUPT_CNTL__IH_DUMMY_RD_OVERRIDE_MASK=0 - dummy read disabled with msi, enabled without msi
  // INTERRUPT_CNTL__IH_DUMMY_RD_OVERRIDE_MASK=1 - dummy read controlled by IH_DUMMY_RD_EN
  interrupt_cntl = REG_SET_FIELD(interrupt_cntl, INTERRUPT_CNTL, IH_DUMMY_RD_OVERRIDE, 0);
  // INTERRUPT_CNTL__IH_REQ_NONSNOOP_EN_MASK=1 if ring is in non-cacheable memory, e.g., vram
  interrupt_cntl = REG_SET_FIELD(interrupt_cntl, INTERRUPT_CNTL, IH_REQ_NONSNOOP_EN, config_.ring_in_video_memory ? 1 : 0);
  wr(mmINTERRUPT_CNTL, interrupt_cntl);

  // Ring Buffer base. [39:8] of 40-bit address of the beginning of the ring buffer
  wr(mmIH_RB_BASE, static_cast<uint32_t>(memory.ring_gpu_address >> 8));

  const uint32_t rb_bufsz = log2_dwords(kRingBytes);
  ih_rb_cntl = REG_SET_FIELD(0, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, RB_SIZE, rb_bufsz);
  // Ring Buffer write pointer writeback. If enabled, IH_RB_WPTR register value is written to memory
  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, WPTR_WRITEBACK_ENABLE, 1);
  ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, MC_VMID, 0);

  if (config_.msi) ih_rb_cntl = REG_SET_FIELD(ih_rb_cntl, IH_RB_CNTL, RPTR_REARM, 1);

  wr(mmIH_RB_CNTL, ih_rb_cntl);

  // set the writeback address whether it's enabled or not
  const uint64_t wptr_addr = memory.ring_gpu_address + kRingBytes;
  wr(mmIH_RB_WPTR_ADDR_LO, static_cast<uint32_t>(wptr_addr));
  wr(mmIH_RB_WPTR_ADDR_HI, static_cast<uint32_t>(wptr_addr >> 32) & 0xFF);

  // set rptr, wptr to 0
  wr(mmIH_RB_RPTR, 0);
  wr(mmIH_RB_WPTR, 0);

  ih_doorbell_rtpr = rd(mmIH_DOORBELL_RPTR);
  if (config_.use_doorbell) {
    ih_doorbell_rtpr = REG_SET_FIELD(ih_doorbell_rtpr, IH_DOORBELL_RPTR, OFFSET, config_.doorbell_index);
    ih_doorbell_rtpr = REG_SET_FIELD(ih_doorbell_rtpr, IH_DOORBELL_RPTR, ENABLE, 1);
  } else {
    ih_doorbell_rtpr = REG_SET_FIELD(ih_doorbell_rtpr, IH_DOORBELL_RPTR, ENABLE, 0);
  }
  wr(mmIH_DOORBELL_RPTR, ih_doorbell_rtpr);

  // (pci_set_master is the PCI layer's: bus mastering must be on before the card writes the ring)

  // enable interrupts
  enable_interrupts();
  return Result::Ok;
}

void AmdIh3::stop() {
  disable_interrupts();
  // Wait and acknowledge irq
  const uint64_t begin = clock_.now_us();
  while (clock_.now_us() - begin < 1000) {}
}

// tonga_ih_get_wptr: the write pointer from the write-back word; if the card says the ring overflowed, from the register, and the read pointer is moved to the oldest
// vector that was not overwritten.
uint32_t AmdIh3::get_wptr() {
  uint32_t wptr = memory_.ring.read32(kRingBytes);

  if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW)) return wptr & kPtrMask;

  // Double check that the overflow wasn't already cleared.
  wptr = rd(mmIH_RB_WPTR);

  if (!REG_GET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW)) return wptr & kPtrMask;

  wptr = REG_SET_FIELD(wptr, IH_RB_WPTR, RB_OVERFLOW, 0);

  // When a ring buffer overflow happen start parsing interrupt from the last not overwritten vector (wptr + 16). Hopefully this should allow us to catchup.
  ++overflows_;
  rptr_ = (wptr + 16) & kPtrMask;
  uint32_t tmp = rd(mmIH_RB_CNTL);
  tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 1);
  wr(mmIH_RB_CNTL, tmp);

  // Unset the CLEAR_OVERFLOW bit immediately so new overflows can be detected.
  tmp = REG_SET_FIELD(tmp, IH_RB_CNTL, WPTR_OVERFLOW_CLEAR, 0);
  wr(mmIH_RB_CNTL, tmp);

  return wptr & kPtrMask;
}

void AmdIh3::set_rptr() {
  if (config_.use_doorbell) {
    memory_.ring.write32(kRingBytes + 4, rptr_);
    memory_.doorbells.write32(uint64_t{config_.doorbell_index} * 4, rptr_);
  } else {
    wr(mmIH_RB_RPTR, rptr_);
  }
}

uint32_t AmdIh3::drain(ArrayView<Vector> out) {
  if (!running_) return 0;
  const uint32_t wptr = get_wptr();
  uint32_t count = 0;
  while (rptr_ != wptr && count < out.size()) {
    // wptr/rptr are in bytes! (a pointer that is not on a vector would run off the ring's end: the ring is a whole number of vectors, the pointers are kept on one)
    const uint32_t ring_index = rptr_;
    uint32_t dw[4];
    for (uint32_t i = 0; i < 4; ++i) dw[i] = memory_.ring.read32(ring_index + 4 * i);
    Vector& entry = out[count++];
    entry.src_id = dw[0] & 0xff;
    entry.src_data = dw[1] & 0xfffffff;
    entry.ring_id = dw[2] & 0xff;
    entry.vmid = (dw[2] >> 8) & 0xff;
    entry.pasid = (dw[2] >> 16) & 0xffff;
    rptr_ = (rptr_ + kVectorBytes) & kPtrMask;
  }
  set_rptr();
  return count;
}

}  // namespace hardware
