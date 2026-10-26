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

// The register reads follow the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): gmc_v8_0.c (gmc_v8_0_vram_gtt_location, gmc_v8_0_mc_init) and vi.c
// (vi_flush_hdp).
#include "Hardware/VideoCodec/AMD/AmdGmc8Memory.hpp"

namespace hardware {

namespace {
// The register numbers and field masks this file uses, generated from gmc_8_1_d.h, gmc_8_1_sh_mask.h and bif_5_0_d.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/VideoCodec/AMD/AmdGmc8MemoryRegisters.inc"

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kMaxVideoMemory = 64ull << 30;   // far more than any card has; a bigger answer is a register that is not what it should be
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t align_down(uint64_t v, uint64_t a) { return v / a * a; }
}  // namespace

AmdGmc8MemoryMap amd_read_gmc8_memory_map(IVcnRegisters& registers) {
  AmdGmc8MemoryMap map;
  // gmc_v8_0_mc_init: the size in MiB, "some boards may have garbage in the upper 16 bits" (then the low half is used if it is not zero).
  uint32_t size = registers.read(mmCONFIG_MEMSIZE);
  if (size & 0xffff0000) {
    if (size & 0xffff) size &= 0xffff;
  }
  if (size == 0 || size == 0xFFFFFFFFu) return map;
  // gmc_v8_0_vram_gtt_location: the base in units of 16 MiB. (The top is the same field of the same register, in the upper half.)
  const uint32_t location = registers.read(mmMC_VM_FB_LOCATION);
  if (location == 0xFFFFFFFFu) return map;
  const uint64_t base = static_cast<uint64_t>(location & MC_VM_FB_LOCATION__FB_BASE_MASK) << 24;
  const uint64_t top = (static_cast<uint64_t>((location & MC_VM_FB_LOCATION__FB_TOP_MASK) >> MC_VM_FB_LOCATION__FB_TOP__SHIFT) + 1) << 24;
  const uint64_t bytes = uint64_t{size} * kMiB;
  // The span the controller covers holds the memory (it is rounded up to 16 MiB), and not much more.
  if (bytes > kMaxVideoMemory || top <= base || top - base < bytes || top - base - bytes >= 16 * kMiB) return map;
  map.read = true;
  map.video_memory_bytes = bytes;
  map.gpu_base = base;
  map.gpu_top = top;
  return map;
}

// vi_flush_hdp (without a ring)
void amd_gmc8_flush_hdp(IVcnRegisters& registers) {
  registers.write(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
  registers.read(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
}

const char* amd_uvd6_plan_result_name(AmdUvd6Plan::Result result) {
  switch (result) {
    case AmdUvd6Plan::Result::Ok: return "ok";
    case AmdUvd6Plan::Result::NoMemoryMap: return "the memory controller's registers did not answer";
    case AmdUvd6Plan::Result::ApertureTooSmall: return "the memory BAR is too small";
    case AmdUvd6Plan::Result::TooSmall: return "not enough video memory the CPU can reach";
  }
  return "unknown";
}

AmdUvd6Plan amd_plan_uvd6_memory(const AmdGmc8MemoryMap& map, uint64_t aperture_bytes, uint64_t block_bytes, uint64_t work_bytes) {
  AmdUvd6Plan plan;
  if (!map.read) return plan;
  if (aperture_bytes < kAmdUvd6MinApertureBytes) {
    plan.result = AmdUvd6Plan::Result::ApertureTooSmall;
    return plan;
  }
  plan.result = AmdUvd6Plan::Result::TooSmall;
  const uint64_t visible = align_down(aperture_bytes < map.video_memory_bytes ? aperture_bytes : map.video_memory_bytes, kMiB);
  plan.visible_bytes = visible;
  if (block_bytes == 0 || block_bytes > visible) return plan;
  const uint64_t block = align_up(block_bytes, kMiB), ring = kMiB;
  const uint64_t lowest = align_up(kAmdUvd6VgaAllocation + kAmdUvd6HeadGuardBytes, kMiB);
  if (visible < kAmdUvd6TailGuardBytes) return plan;
  const uint64_t end = visible - kAmdUvd6TailGuardBytes;
  if (end < lowest || end - lowest < block + ring + kAmdUvd6MinWorkBytes) return plan;
  // The work area is what was asked for, or what room there is (whole MiB).
  uint64_t work = align_up(work_bytes < visible ? work_bytes : visible, kMiB);   // (clamped first: rounding up a request near 2^64 would wrap)
  const uint64_t room = align_down(end - lowest - block - ring, kMiB);
  if (work > room) work = room;
  if (work < kAmdUvd6MinWorkBytes) return plan;
  plan.work_offset = end - work;
  plan.work_bytes = work;
  plan.ring_offset = plan.work_offset - ring;
  plan.block_offset = plan.ring_offset - block;
  plan.block_bytes = block_bytes;
  plan.block_gpu_address = map.gpu_base + plan.block_offset;
  plan.ring_gpu_address = map.gpu_base + plan.ring_offset;
  plan.work_gpu_address = map.gpu_base + plan.work_offset;
  plan.result = AmdUvd6Plan::Result::Ok;
  return plan;
}

bool amd_map_uvd6_plan(const AmdUvd6Plan& plan, IRegisterMapper& mapper, uint64_t aperture_physical, AmdUvd6Memory* memory) {
  if (plan.result != AmdUvd6Plan::Result::Ok || !memory || aperture_physical == 0) return false;
  const uint64_t block_bytes = align_up(plan.block_bytes, kMiB);
  if (plan.block_offset > UINT64_MAX - aperture_physical || plan.work_offset > UINT64_MAX - aperture_physical || plan.work_bytes > UINT64_MAX - aperture_physical - plan.work_offset) return false;
  MmioRegion block, ring, work;
  if (!mapper.map_registers(aperture_physical + plan.block_offset, block_bytes, &block)) return false;
  if (!mapper.map_registers(aperture_physical + plan.ring_offset, kMiB, &ring)) return false;
  if (!mapper.map_registers(aperture_physical + plan.work_offset, plan.work_bytes, &work)) return false;
  memory->engine.block_gpu_address = plan.block_gpu_address;
  memory->engine.block = block;
  memory->engine.ring_gpu_address = plan.ring_gpu_address;
  memory->engine.ring = ring;
  memory->work = work;
  memory->work_gpu_address = plan.work_gpu_address;
  return true;
}

}  // namespace hardware
