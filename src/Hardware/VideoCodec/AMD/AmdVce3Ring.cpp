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


// Ported from the Linux kernel's drivers/gpu/drm/amd/amdgpu/vce_v3_0.c and amdgpu_vce.c, vid.h and vi.c (Copyright Advanced Micro Devices, Inc.; MIT license).
#include "Hardware/VideoCodec/AMD/AmdVce3Ring.hpp"

namespace hardware {

namespace {
constexpr uint32_t mmGRBM_GFX_INDEX_DEFAULT = 0xE0000000;
constexpr uint32_t GRBM_GFX_INDEX__VCE_INSTANCE__SHIFT = 0x04;
constexpr uint32_t GRBM_GFX_INDEX__VCE_ALL_PIPE = 0x07;
// The register numbers this file uses, generated from vce_3_0_d.h, gfx_8_0_d.h and bif_5_0_d.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/VideoCodec/AMD/AmdVce3RingRegisters.inc"

constexpr uint32_t lower_32_bits(uint64_t v) { return static_cast<uint32_t>(v); }
constexpr uint32_t upper_32_bits(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
constexpr uint32_t GET_VCE_INSTANCE(uint32_t i) { return (i << GRBM_GFX_INDEX__VCE_INSTANCE__SHIFT) | GRBM_GFX_INDEX__VCE_ALL_PIPE; }

// A buffer writer over a caller's array of dwords that never writes outside it.
struct DwordSink {
  ArrayView<uint32_t> out;
  uint32_t count = 0;
  bool overflow = false;
  void put(uint32_t v) {
    if (count >= out.size()) { overflow = true; return; }
    out[count++] = v;
  }
};
}  // namespace

// The commands of one job, in the driver's order: vce_v3_0_ring_emit_ib, then amdgpu_vce_ring_emit_fence.
uint32_t AmdVce3Ring::build_job_commands(uint64_t ib_gpu_address, uint32_t ib_dwords, uint64_t fence_gpu_address, uint32_t sequence, ArrayView<uint32_t> out) {
  DwordSink sink{out};
  sink.put(kCmdIbVm);
  sink.put(0);   // the VMID: the system context
  sink.put(lower_32_bits(ib_gpu_address));
  sink.put(upper_32_bits(ib_gpu_address));
  sink.put(ib_dwords);
  sink.put(kCmdFence);
  sink.put(lower_32_bits(fence_gpu_address));
  sink.put(upper_32_bits(fence_gpu_address));
  sink.put(sequence);
  sink.put(kCmdTrap);
  sink.put(kCmdEnd);
  return sink.overflow ? 0 : sink.count;
}

AmdVce3Ring::AmdVce3Ring(IVcnRegisters& registers, const IClock& clock, const Memory& memory, uint32_t ring_index, uint32_t harvest_config)
    : registers_(registers), clock_(clock), memory_(memory), index_(ring_index < AmdVce3::kMaxRings ? ring_index : 0), harvest_config_(harvest_config) {
  // The ring starts where the start sequence left it: both pointers at the write pointer it was given (AmdVce3::start writes the read pointer from it).
  wptr_ = read_pointer() % kRingDwords;
}

// vce_v3_0_ring_get_rptr (the registers of the ring `index_`; the driver takes the mutex that guards GRBM_GFX_INDEX around this)
uint32_t AmdVce3Ring::read_ring_register(uint32_t which) {
  static constexpr uint32_t rptr[AmdVce3::kMaxRings] = {mmVCE_RB_RPTR, mmVCE_RB_RPTR2, mmVCE_RB_RPTR3};
  static constexpr uint32_t wptr[AmdVce3::kMaxRings] = {mmVCE_RB_WPTR, mmVCE_RB_WPTR2, mmVCE_RB_WPTR3};
  registers_.write(mmGRBM_GFX_INDEX, GET_VCE_INSTANCE(instance()));
  const uint32_t v = registers_.read(which == 0 ? rptr[index_] : wptr[index_]);
  registers_.write(mmGRBM_GFX_INDEX, mmGRBM_GFX_INDEX_DEFAULT);
  return v;
}

uint32_t AmdVce3Ring::read_pointer() { return read_ring_register(0); }

// vce_v3_0_ring_set_wptr
void AmdVce3Ring::write_write_pointer(uint32_t value) {
  static constexpr uint32_t wptr[AmdVce3::kMaxRings] = {mmVCE_RB_WPTR, mmVCE_RB_WPTR2, mmVCE_RB_WPTR3};
  registers_.write(mmGRBM_GFX_INDEX, GET_VCE_INSTANCE(instance()));
  registers_.write(wptr[index_], value);
  registers_.write(mmGRBM_GFX_INDEX, mmGRBM_GFX_INDEX_DEFAULT);
}

void AmdVce3Ring::ring_write(uint32_t dword) {
  memory_.ring.write32(uint64_t{wptr_} * 4, dword);
  wptr_ = (wptr_ + 1) % kRingDwords;
}

// amdgpu_ring_commit: pad with no-ops (the ring's nop, VCE_CMD_NO_OP) to the ring's alignment, flush what was written through the memory BAR (vi_flush_hdp), then publish
// the write pointer (vce_v3_0_ring_set_wptr).
void AmdVce3Ring::commit() {
  while (wptr_ % kAlignDwords != 0) ring_write(kCmdNoOp);
  registers_.write(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
  registers_.read(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
  write_write_pointer(wptr_);
}

bool AmdVce3Ring::test(uint32_t timeout_us) {
  if (wedged_ || !memory_.ring.valid() || memory_.ring.length() < AmdVce3::kRingBytes) return false;
  const uint32_t rptr = read_pointer();
  ring_write(kCmdEnd);
  commit();
  const uint64_t deadline = clock_.now_us() + timeout_us;
  do {
    if (read_pointer() != rptr) return true;
  } while (clock_.now_us() < deadline);
  return false;
}

AmdVce3Ring::Result AmdVce3Ring::submit(ArrayView<const uint32_t> ib, uint32_t timeout_ms) {
  if (wedged_) return Result::Wedged;
  if (ib.empty() || ib.size() > kMaxIndirectBufferDwords || !memory_.ib.valid() || !memory_.fence.valid() || !memory_.ring.valid()) return Result::BadArgument;
  if (memory_.ib.length() < kIndirectBufferBytes || memory_.fence.length() < kFenceBytes || memory_.ring.length() < AmdVce3::kRingBytes) return Result::BadArgument;
  // amdgpu_ring_generic_pad_ib: the indirect buffer is padded with no-ops to the ring's alignment.
  const uint64_t padded = (ib.size() + kAlignDwords - 1) / kAlignDwords * kAlignDwords;
  if (padded > kMaxIndirectBufferDwords) return Result::BadArgument;

  // The indirect buffer: written whole before the ring points at it.
  for (uint32_t i = 0; i < ib.size(); ++i) memory_.ib.write32(uint64_t{i} * 4, ib[i]);
  for (uint64_t i = ib.size(); i < padded; ++i) memory_.ib.write32(i * 4, kCmdNoOp);
  memory_.fence.write32(0, 0);

  const uint32_t sequence = sequence_;
  uint32_t commands[16];
  const uint32_t count = build_job_commands(memory_.ib_gpu_address, static_cast<uint32_t>(padded), memory_.fence_gpu_address, sequence, ArrayView<uint32_t>(commands));
  if (count == 0) return Result::BadArgument;
  for (uint32_t i = 0; i < count; ++i) ring_write(commands[i]);
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
