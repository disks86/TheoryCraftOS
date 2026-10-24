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

// The register reads follow the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): gmc_v9_0.c, mmhub_v1_0.c (mmhub_v1_0_get_fb_location),
// gfxhub_v1_0.c (gfxhub_v1_0_get_mc_fb_offset), nbio_v7_0.c (nbio_v7_0_get_memsize) and gfx_v9_0.c (gfx_v9_0_gpu_early_init).
#include "Hardware/VideoCodec/AMD/AmdRavenMemory.hpp"

namespace hardware {

namespace {
// The register block bases of the Vega10 table, which Raven uses (vega10_ip_offset.h), by segment index.
constexpr uint32_t kGcBase[5] = {0x00002000, 0x0000A000, 0, 0, 0};
constexpr uint32_t kMmhubBase[5] = {0x0001A000, 0, 0, 0, 0};
constexpr uint32_t kNbioBase[5] = {0x00000000, 0x00000014, 0x00000D20, 0x00010400, 0};

// gfx_v9_0.c
constexpr uint32_t RAVEN_GB_ADDR_CONFIG_GOLDEN = 0x24000042;
constexpr uint32_t RAVEN2_GB_ADDR_CONFIG_GOLDEN = 0x26013041;

// Register names that more than one block's header defines, each used here from the block the driver reads it from (see scripts/gen_vcn1_regs.py):
// regs-from: mmMC_VM_FB_OFFSET gc_9_1_offset.h
// regs-from: mmMC_VM_FB_OFFSET_BASE_IDX gc_9_1_offset.h
// regs-from: mmMC_VM_FB_LOCATION_BASE mmhub_9_1_offset.h
// regs-from: mmMC_VM_FB_LOCATION_BASE_BASE_IDX mmhub_9_1_offset.h
// regs-from: mmMC_VM_FB_LOCATION_TOP mmhub_9_1_offset.h
// regs-from: mmMC_VM_FB_LOCATION_TOP_BASE_IDX mmhub_9_1_offset.h
#include "Hardware/VideoCodec/AMD/AmdRavenMemoryRegisters.inc"

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kMaxVideoMemory = 64ull << 30;   // far more than any APU carve-out; a bigger answer is a register that is not what it should be
constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
}  // namespace

AmdRavenMemoryMap amd_read_raven_memory_map(IVcnRegisters& registers) {
  AmdRavenMemoryMap map;
  const uint32_t memsize = registers.read(kNbioBase[mmRCC_CONFIG_MEMSIZE_BASE_IDX] + mmRCC_CONFIG_MEMSIZE);             // nbio_v7_0_get_memsize: MiB
  const uint32_t fb_base = registers.read(kMmhubBase[mmMC_VM_FB_LOCATION_BASE_BASE_IDX] + mmMC_VM_FB_LOCATION_BASE);   // mmhub_v1_0_get_fb_location
  const uint32_t fb_top = registers.read(kMmhubBase[mmMC_VM_FB_LOCATION_TOP_BASE_IDX] + mmMC_VM_FB_LOCATION_TOP);
  const uint32_t fb_offset = registers.read(kGcBase[mmMC_VM_FB_OFFSET_BASE_IDX] + mmMC_VM_FB_OFFSET);                  // gfxhub_v1_0_get_mc_fb_offset
  map.hardware_gb_addr_config = registers.read(kGcBase[mmGB_ADDR_CONFIG_BASE_IDX] + mmGB_ADDR_CONFIG);

  // A block that is powered down reads all ones; a size of zero is no memory at all.
  if (memsize == 0 || memsize == 0xFFFFFFFFu || fb_base == 0xFFFFFFFFu || fb_top == 0xFFFFFFFFu || fb_offset == 0xFFFFFFFFu) return map;
  map.video_memory_bytes = uint64_t{memsize} * kMiB;
  map.gpu_base = uint64_t{fb_base & MC_VM_FB_LOCATION_BASE__FB_BASE_MASK} << 24;
  map.gpu_top = uint64_t{fb_top & MC_VM_FB_LOCATION_TOP__FB_TOP_MASK} << 24;
  map.cpu_base = uint64_t{fb_offset} << 24;
  // Plausible: a carve-out from 16 MiB up to the limit, whose ends do not wrap the address space.
  map.read = map.video_memory_bytes >= 16 * kMiB && map.video_memory_bytes <= kMaxVideoMemory && map.gpu_base <= UINT64_MAX - map.video_memory_bytes &&
             map.cpu_base <= UINT64_MAX - map.video_memory_bytes;
  return map;
}

uint32_t amd_raven_gb_addr_config(uint16_t device_id, uint8_t revision) {
  if (device_id != 0x15DD && device_id != 0x15D8) return 0;   // Raven Ridge, Picasso
  return revision >= 0x8 ? RAVEN2_GB_ADDR_CONFIG_GOLDEN : RAVEN_GB_ADDR_CONFIG_GOLDEN;
}

AmdGbAddrConfigCheck amd_check_gb_addr_config(uint32_t expected, uint32_t hardware) {
  if (hardware == 0 || hardware == 0xFFFFFFFFu) return AmdGbAddrConfigCheck::HardwareUnreadable;
  return hardware == expected ? AmdGbAddrConfigCheck::Matches : AmdGbAddrConfigCheck::Differs;
}

const char* amd_vcn1_plan_result_name(AmdVcn1Plan::Result result) {
  switch (result) {
    case AmdVcn1Plan::Result::Ok: return "planned";
    case AmdVcn1Plan::Result::NotRaven: return "not a VCN 1.0 APU";
    case AmdVcn1Plan::Result::NoMemoryMap: return "the memory controller's registers did not answer";
    case AmdVcn1Plan::Result::NoFramebuffer: return "the firmware's framebuffer is unknown";
    case AmdVcn1Plan::Result::FramebufferOutside: return "the firmware's framebuffer is not inside the video memory the registers describe";
    case AmdVcn1Plan::Result::TooSmall: return "video memory is too small";
    case AmdVcn1Plan::Result::ConfigDiffers: return "the graphics address configuration differs from the chip variant's";
  }
  return "unknown";
}

AmdVcn1Plan amd_plan_vcn1_memory(const AmdRavenMemoryMap& map, uint16_t device_id, uint8_t revision, uint64_t block_bytes, uint64_t fb_physical, uint64_t fb_bytes) {
  AmdVcn1Plan plan;
  plan.gb_addr_config = amd_raven_gb_addr_config(device_id, revision);
  plan.block_bytes = block_bytes;
  if (plan.gb_addr_config == 0) {
    plan.result = AmdVcn1Plan::Result::NotRaven;
    return plan;
  }
  plan.gb_check = amd_check_gb_addr_config(plan.gb_addr_config, map.hardware_gb_addr_config);
  if (!map.read) {
    plan.result = AmdVcn1Plan::Result::NoMemoryMap;
    return plan;
  }
  if (plan.gb_check == AmdGbAddrConfigCheck::Differs) {
    plan.result = AmdVcn1Plan::Result::ConfigDiffers;
    return plan;
  }
  if (fb_physical == 0 || fb_bytes == 0) {
    plan.result = AmdVcn1Plan::Result::NoFramebuffer;
    return plan;
  }
  // The firmware's framebuffer must sit in the carve-out the registers describe (n > limit - size, not size + n > limit): if it does not, the registers are
  // describing something else and nothing derived from them can be trusted.
  if (fb_bytes > map.video_memory_bytes || fb_physical < map.cpu_base || fb_physical - map.cpu_base > map.video_memory_bytes - fb_bytes) {
    plan.result = AmdVcn1Plan::Result::FramebufferOutside;
    return plan;
  }
  const uint64_t fb_end = fb_physical - map.cpu_base + fb_bytes;
  const uint64_t firmware_part = fb_end > kAmdVbiosVgaAllocation ? fb_end : kAmdVbiosVgaAllocation;
  const uint64_t block_offset = align_up(firmware_part + kAmdVcn1GuardBytes, kMiB);
  // Each is a whole number of MiB, so the block and the rings are page aligned as AmdVcn1 requires and cannot overlap each other.
  if (block_bytes == 0 || block_bytes > map.video_memory_bytes) {
    plan.result = AmdVcn1Plan::Result::TooSmall;
    return plan;
  }
  const uint64_t rings_offset = block_offset + align_up(block_bytes, kMiB);
  const uint64_t rings_end = rings_offset + AmdVcn1::kRingsTotalBytes;
  if (rings_end > map.video_memory_bytes || map.video_memory_bytes - rings_end < kAmdVcn1TailGuardBytes) {
    plan.result = AmdVcn1Plan::Result::TooSmall;
    return plan;
  }
  plan.block_offset = block_offset;
  plan.rings_offset = rings_offset;
  plan.block_gpu_address = map.gpu_base + block_offset;
  plan.rings_gpu_address = map.gpu_base + rings_offset;
  plan.block_physical = map.cpu_base + block_offset;
  plan.rings_physical = map.cpu_base + rings_offset;
  // The rest of video memory, from the next MiB after the rings to the tail guard, is for decoding sessions.
  const uint64_t work_offset = align_up(rings_end, kMiB);
  const uint64_t work_end = (map.video_memory_bytes - kAmdVcn1TailGuardBytes) / kMiB * kMiB;
  if (work_end > work_offset) {
    plan.work_offset = work_offset;
    plan.work_bytes = work_end - work_offset;
    plan.work_gpu_address = map.gpu_base + work_offset;
    plan.work_physical = map.cpu_base + work_offset;
  }
  plan.result = AmdVcn1Plan::Result::Ok;
  return plan;
}

bool amd_map_vcn1_plan(const AmdVcn1Plan& plan, IRegisterMapper& mapper, AmdVcn1::Memory* memory) {
  if (plan.result != AmdVcn1Plan::Result::Ok || !memory) return false;
  MmioRegion block, rings;
  if (!mapper.map_registers(plan.block_physical, plan.block_bytes, &block)) return false;
  if (!mapper.map_registers(plan.rings_physical, AmdVcn1::kRingsTotalBytes, &rings)) return false;
  memory->block_gpu_address = plan.block_gpu_address;
  memory->block = block;
  memory->rings_gpu_address = plan.rings_gpu_address;
  memory->rings = rings;
  return true;
}

}  // namespace hardware
