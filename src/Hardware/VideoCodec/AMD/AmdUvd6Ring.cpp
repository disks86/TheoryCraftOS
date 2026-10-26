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

// The packet formats and the order of the writes follow the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): uvd_v6_0.c (uvd_v6_0_ring_emit_ib,
// uvd_v6_0_ring_emit_fence, uvd_v6_0_ring_insert_nop, uvd_v6_0_ring_set_wptr, uvd_v6_0_ring_test_ring; the ring's align_mask of 0xf) and vi.c (vi_flush_hdp), and Mesa's
// radeon_uvd.c (send_cmd, set_reg).
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"

namespace hardware {

namespace {
// The register numbers this file uses, generated from uvd_6_0_d.h and bif_5_0_d.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/VideoCodec/AMD/AmdUvd6RingRegisters.inc"

// PACKET0 of the UVD family: type 0 in the top two bits, the register dword in the low sixteen, the count of further registers.
constexpr uint32_t packet0(uint32_t register_dword, uint32_t count) { return ((register_dword & 0xFFFF) | ((count & 0x3FFF) << 16)); }

constexpr uint32_t lower_32_bits(uint64_t v) { return static_cast<uint32_t>(v); }
constexpr uint32_t upper_32_bits(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

// The commands of the engine's GPCOM register that the ring itself uses (the buffer commands are Mesa's RUVD_CMD_*): the fence and the trap, as the driver writes them
// (command << 1).
constexpr uint32_t kFenceCommand = 0;
constexpr uint32_t kTrapCommand = 2;
constexpr uint32_t kAlignDwords = 16;   // the ring's align_mask 0xf plus one

// A buffer writer over a caller's array of dwords that never writes outside it.
struct DwordSink {
  ArrayView<uint32_t> out;
  uint32_t count = 0;
  bool overflow = false;
  void put(uint32_t v) {
    if (count >= out.size()) { overflow = true; return; }
    out[count++] = v;
  }
  void reg(uint32_t register_dword, uint32_t value) {
    put(packet0(register_dword, 0));
    put(value);
  }
};
}  // namespace

uint32_t AmdUvd6Ring::build_indirect_buffer(ArrayView<const Buffer> buffers, bool start_engine, ArrayView<uint32_t> out) {
  if (buffers.size() > kMaxBuffersPerJob) return 0;
  DwordSink sink{out};
  for (const Buffer& b : buffers) {
    sink.reg(mmUVD_GPCOM_VCPU_DATA0, lower_32_bits(b.gpu_address));
    sink.reg(mmUVD_GPCOM_VCPU_DATA1, upper_32_bits(b.gpu_address));
    sink.reg(mmUVD_GPCOM_VCPU_CMD, b.command << 1);
  }
  if (start_engine) sink.reg(mmUVD_ENGINE_CNTL, 1);
  while (!sink.overflow && sink.count % kAlignDwords != 0) sink.reg(mmUVD_NO_OP, 0);
  return sink.overflow ? 0 : sink.count;
}

AmdUvd6Ring::AmdUvd6Ring(IVcnRegisters& registers, const IClock& clock, const Memory& memory) : registers_(registers), clock_(clock), memory_(memory) {
  // The ring starts where the start sequence left it: both pointers at the read pointer (AmdUvd6::start writes the write pointer from the read pointer).
  wptr_ = registers_.read(mmUVD_RBC_RB_RPTR) % kRingDwords;
}

void AmdUvd6Ring::ring_write(uint32_t dword) {
  memory_.ring.write32(uint64_t{wptr_} * 4, dword);
  wptr_ = (wptr_ + 1) % kRingDwords;
}

void AmdUvd6Ring::ring_register(uint32_t register_dword, uint32_t value) {
  ring_write(packet0(register_dword, 0));
  ring_write(value);
}

// amdgpu_ring_commit: pad with no-ops (uvd_v6_0_ring_insert_nop) to the ring's alignment, flush what was written through the memory BAR (vi_flush_hdp), then publish the
// write pointer (uvd_v6_0_ring_set_wptr).
void AmdUvd6Ring::commit() {
  while (wptr_ % kAlignDwords != 0) ring_register(mmUVD_NO_OP, 0);
  registers_.write(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
  registers_.read(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
  registers_.write(mmUVD_RBC_RB_WPTR, wptr_);
}

bool AmdUvd6Ring::test(uint32_t timeout_us) {
  if (wedged_ || !memory_.ring.valid() || memory_.ring.length() < AmdUvd6::kRingBytes) return false;
  registers_.write(mmUVD_CONTEXT_ID, 0xCAFEDEAD);
  ring_write(packet0(mmUVD_CONTEXT_ID, 0));
  ring_write(0xDEADBEEF);
  commit();
  const uint64_t deadline = clock_.now_us() + timeout_us;
  do {
    if (registers_.read(mmUVD_CONTEXT_ID) == 0xDEADBEEF) return true;
  } while (clock_.now_us() < deadline);
  return false;
}

AmdUvd6Ring::Result AmdUvd6Ring::submit(ArrayView<const Buffer> buffers, bool start_engine, uint32_t timeout_ms) {
  if (wedged_) return Result::Wedged;
  if (buffers.empty() || buffers.size() > kMaxBuffersPerJob || !memory_.ib.valid() || !memory_.fence.valid() || !memory_.ring.valid()) return Result::BadArgument;
  if (memory_.ib.length() < kIndirectBufferBytes || memory_.fence.length() < kFenceBytes || memory_.ring.length() < AmdUvd6::kRingBytes) return Result::BadArgument;

  // The indirect buffer: written whole before the ring points at it.
  uint32_t words[kIndirectBufferBytes / 4];
  const uint32_t count = build_indirect_buffer(buffers, start_engine, ArrayView<uint32_t>(words));
  if (count == 0) return Result::BadArgument;
  for (uint32_t i = 0; i < count; ++i) memory_.ib.write32(uint64_t{i} * 4, words[i]);
  memory_.fence.write32(0, 0);

  const uint32_t sequence = sequence_;
  // The ring commands, in the driver's order.
  ring_register(mmUVD_LMI_RBC_IB_VMID, 0);                                                    // emit_ib (the system context: VMID 0)
  ring_register(mmUVD_LMI_RBC_IB_64BIT_BAR_LOW, lower_32_bits(memory_.ib_gpu_address));
  ring_register(mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH, upper_32_bits(memory_.ib_gpu_address));
  ring_register(mmUVD_RBC_IB_SIZE, count);
  ring_register(mmUVD_CONTEXT_ID, sequence);                                                  // emit_fence: the fence, then the trap
  ring_register(mmUVD_GPCOM_VCPU_DATA0, lower_32_bits(memory_.fence_gpu_address));
  ring_register(mmUVD_GPCOM_VCPU_DATA1, upper_32_bits(memory_.fence_gpu_address) & 0xff);
  ring_register(mmUVD_GPCOM_VCPU_CMD, kFenceCommand);
  ring_register(mmUVD_GPCOM_VCPU_DATA0, 0);
  ring_register(mmUVD_GPCOM_VCPU_DATA1, 0);
  ring_register(mmUVD_GPCOM_VCPU_CMD, kTrapCommand);
  commit();

  const uint64_t deadline = clock_.now_us() + uint64_t{timeout_ms} * 1000;
  for (;;) {
    if (memory_.fence.read32(0) == sequence) {
      ++sequence_;
      return Result::Ok;
    }
    if (clock_.now_us() >= deadline) break;
  }
  wedged_ = true;
  return Result::TimedOut;
}

}  // namespace hardware
