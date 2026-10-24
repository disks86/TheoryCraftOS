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


// The packet formats and the order of the writes follow the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): vcn_v1_0.c
// (vcn_v1_0_dec_ring_insert_start, _insert_end, _emit_ib, _emit_fence, _insert_nop; the ring's align_mask of 0xf) and amdgpu_vcn.c (amdgpu_vcn_dec_send_msg:
// the indirect buffer of a message), and Mesa's radeon_vcn_dec.c (send_cmd, send_cmd_dec).
#include "Hardware/VideoCodec/AMD/AmdVcn1Ring.hpp"

namespace hardware {

namespace {
constexpr uint32_t kUvdBase[5] = {0x00007800, 0x00007E00, 0, 0, 0};   // as in AmdVcn1.cpp

// The commands of the decode ring (amdgpu_vcn.h), which no register header defines.
constexpr uint32_t VCN_DEC_CMD_FENCE = 0x00000000;
constexpr uint32_t VCN_DEC_CMD_TRAP = 0x00000001;
constexpr uint32_t VCN_DEC_CMD_PACKET_START = 0x0000000a;
constexpr uint32_t VCN_DEC_CMD_PACKET_END = 0x0000000b;

#include "Hardware/VideoCodec/AMD/AmdVcn1RingRegisters.inc"

#define SOC15_REG_OFFSET(ip, inst, reg) (kUvdBase[reg##_BASE_IDX] + (reg))

// PACKET0 of the UVD family (amdgpu_uvd / uvd packets): type 0 in the top two bits, the register dword in the low sixteen, the count of further registers.
constexpr uint32_t packet0(uint32_t register_dword, uint32_t count) { return ((register_dword & 0xFFFF) | ((count & 0x3FFF) << 16)); }

constexpr uint32_t lower_32_bits(uint64_t v) { return static_cast<uint32_t>(v); }
constexpr uint32_t upper_32_bits(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

constexpr uint32_t kNopPacket = packet0(SOC15_REG_OFFSET(UVD, 0, mmUVD_NO_OP), 0);
constexpr uint32_t kRegisterGpcomData0 = SOC15_REG_OFFSET(UVD, 0, mmUVD_GPCOM_VCPU_DATA0);
constexpr uint32_t kRegisterGpcomData1 = SOC15_REG_OFFSET(UVD, 0, mmUVD_GPCOM_VCPU_DATA1);
constexpr uint32_t kRegisterGpcomCmd = SOC15_REG_OFFSET(UVD, 0, mmUVD_GPCOM_VCPU_CMD);
constexpr uint32_t kRegisterEngineCntl = SOC15_REG_OFFSET(UVD, 0, mmUVD_ENGINE_CNTL);
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

uint32_t AmdVcn1Ring::build_indirect_buffer(ArrayView<const Buffer> buffers, bool start_engine, ArrayView<uint32_t> out) {
  if (buffers.size() > kMaxBuffersPerJob) return 0;
  DwordSink sink{out};
  for (const Buffer& b : buffers) {
    sink.reg(kRegisterGpcomData0, lower_32_bits(b.gpu_address));
    sink.reg(kRegisterGpcomData1, upper_32_bits(b.gpu_address));
    sink.reg(kRegisterGpcomCmd, b.command << 1);
  }
  if (start_engine) sink.reg(kRegisterEngineCntl, 1);
  while (!sink.overflow && sink.count % kAlignDwords != 0) { sink.put(kNopPacket); sink.put(0); }
  return sink.overflow ? 0 : sink.count;
}

AmdVcn1Ring::AmdVcn1Ring(IVcnRegisters& registers, const IClock& clock, const Memory& memory) : registers_(registers), clock_(clock), memory_(memory) {
  // The ring starts where the start sequence left it: both pointers at the read pointer (AmdVcn1::start writes the write pointer from the read pointer).
  wptr_ = registers_.read(SOC15_REG_OFFSET(UVD, 0, mmUVD_RBC_RB_RPTR)) % kRingDwords;
}

void AmdVcn1Ring::ring_write(uint32_t dword) {
  memory_.ring.write32(uint64_t{wptr_} * 4, dword);
  wptr_ = (wptr_ + 1) % kRingDwords;
}

void AmdVcn1Ring::ring_register(uint32_t register_dword, uint32_t value) {
  ring_write(packet0(register_dword, 0));
  ring_write(value);
}

AmdVcn1Ring::Result AmdVcn1Ring::submit(ArrayView<const Buffer> buffers, bool start_engine, uint32_t timeout_ms) {
  if (wedged_) return Result::Wedged;
  if (buffers.empty() || buffers.size() > kMaxBuffersPerJob || !memory_.ib.valid() || !memory_.fence.valid() || !memory_.ring.valid()) return Result::BadArgument;
  if (memory_.ib.length() < kIndirectBufferBytes || memory_.fence.length() < kFenceBytes || memory_.ring.length() < AmdVcn1::kRingBytes) return Result::BadArgument;

  // The indirect buffer: written whole before the ring points at it.
  uint32_t words[kIndirectBufferBytes / 4];
  const uint32_t count = build_indirect_buffer(buffers, start_engine, ArrayView<uint32_t>(words));
  if (count == 0) return Result::BadArgument;
  for (uint32_t i = 0; i < count; ++i) memory_.ib.write32(uint64_t{i} * 4, words[i]);
  memory_.fence.write32(0, 0);

  const uint32_t sequence = sequence_;
  // The ring commands, in the driver's order.
  ring_register(kRegisterGpcomData0, 0);                                                               // insert_start
  ring_register(kRegisterGpcomCmd, VCN_DEC_CMD_PACKET_START << 1);
  ring_register(SOC15_REG_OFFSET(UVD, 0, mmUVD_LMI_RBC_IB_VMID), 0);                                   // emit_ib (the system context: VMID 0)
  ring_register(SOC15_REG_OFFSET(UVD, 0, mmUVD_LMI_RBC_IB_64BIT_BAR_LOW), lower_32_bits(memory_.ib_gpu_address));
  ring_register(SOC15_REG_OFFSET(UVD, 0, mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH), upper_32_bits(memory_.ib_gpu_address));
  ring_register(SOC15_REG_OFFSET(UVD, 0, mmUVD_RBC_IB_SIZE), count);
  ring_register(SOC15_REG_OFFSET(UVD, 0, mmUVD_CONTEXT_ID), sequence);                                 // emit_fence: the fence, then the trap
  ring_register(kRegisterGpcomData0, lower_32_bits(memory_.fence_gpu_address));
  ring_register(kRegisterGpcomData1, upper_32_bits(memory_.fence_gpu_address) & 0xff);
  ring_register(kRegisterGpcomCmd, VCN_DEC_CMD_FENCE << 1);
  ring_register(kRegisterGpcomData0, 0);
  ring_register(kRegisterGpcomData1, 0);
  ring_register(kRegisterGpcomCmd, VCN_DEC_CMD_TRAP << 1);
  ring_register(kRegisterGpcomCmd, VCN_DEC_CMD_PACKET_END << 1);                                       // insert_end
  // amdgpu_ring_commit: pad with no-ops to the ring's alignment, then publish the write pointer.
  while (wptr_ % kAlignDwords != 0) ring_register(SOC15_REG_OFFSET(UVD, 0, mmUVD_NO_OP), 0);
  registers_.write(SOC15_REG_OFFSET(UVD, 0, mmUVD_RBC_RB_WPTR), wptr_);

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
