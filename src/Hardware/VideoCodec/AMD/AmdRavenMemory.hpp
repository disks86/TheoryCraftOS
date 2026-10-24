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

#pragma once
#include <stdint.h>
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

// Where things are on a Raven-family APU (Ryzen 2000/3000 "Raven Ridge", "Picasso", "Raven2"), for starting its video engine (AmdVcn1) without guessing: what the
// memory controller says about video memory, the graphics address configuration the engine must be given, and a block of video memory the engine can be handed
// that nothing else is using. Everything here only READS the card; mapping a window is the only thing it does to the machine.
//
// An APU has no memory of its own. Its "video memory" is a carve-out of system RAM the BIOS sets aside (the UMA frame buffer size setting). The same bytes have
// three addresses: an offset into video memory, the address the GPU uses (the memory controller's base plus the offset), and the address the CPU uses (the
// carve-out's physical address plus the offset). The Linux amdgpu driver reads the three bases from the registers below (gmc_v9_0.c, mmhub_v1_0.c,
// gfxhub_v1_0.c, nbio_v7_0.c) and the registers are read here the same way.

// The register block of these APUs is a 512 KiB BAR (BAR 5); every register below lies inside it.
constexpr uint64_t kAmdRavenRegisterBytes = 0x80000;

struct AmdRavenMemoryMap {
  bool read = false;                  // the registers answered with values that can be video memory
  uint64_t video_memory_bytes = 0;    // RCC_CONFIG_MEMSIZE, in MiB, as bytes: the size of the carve-out
  uint64_t gpu_base = 0;              // MC_VM_FB_LOCATION_BASE << 24: the GPU's address of the first byte of video memory
  uint64_t gpu_top = 0;               // MC_VM_FB_LOCATION_TOP << 24
  uint64_t cpu_base = 0;              // MC_VM_FB_OFFSET << 24: the CPU's (physical) address of the first byte of video memory
  uint32_t hardware_gb_addr_config = 0;   // GB_ADDR_CONFIG as it reads now (0 or all ones when the graphics block has not been set up)
};
AmdRavenMemoryMap amd_read_raven_memory_map(IVcnRegisters& registers);

// The graphics address configuration the engine's memory interfaces must match. The driver does not read it from the hardware: for these chips it is a constant
// per variant (gfx_v9_0.c: RAVEN_GB_ADDR_CONFIG_GOLDEN, RAVEN2_GB_ADDR_CONFIG_GOLDEN) that it then writes into the graphics block. Revision 8 and up is Raven2.
// 0 for a device that is not one of these APUs.
uint32_t amd_raven_gb_addr_config(uint16_t pci_device_id, uint8_t pci_revision);

// Does the register, as the machine's firmware left it, agree with the constant? Unreadable (zero, or all ones from a block that is off) says nothing either way.
enum class AmdGbAddrConfigCheck : uint8_t { Matches, HardwareUnreadable, Differs };
AmdGbAddrConfigCheck amd_check_gb_addr_config(uint32_t expected, uint32_t hardware);

// The firmware keeps the start of video memory: the driver reserves the larger of the framebuffer it left there and 9 MiB (AMDGPU_VBIOS_VGA_ALLOCATION, which covers
// the legacy VGA area) as "stolen" memory it never allocates from. Past that, the driver allocates freely on chips with no IP discovery table (Raven has none), so
// the block goes after that prefix with a guard, and short of the end of video memory by another guard.
constexpr uint64_t kAmdVbiosVgaAllocation = 9ull << 20;
constexpr uint64_t kAmdVcn1GuardBytes = 16ull << 20;       // between the firmware's part of video memory and the block
constexpr uint64_t kAmdVcn1TailGuardBytes = 16ull << 20;   // between the rings and the end of video memory

struct AmdVcn1Plan {
  enum class Result : uint8_t {
    Ok,
    NotRaven,            // not a VCN 1.0 APU
    NoMemoryMap,         // the memory controller's registers did not answer sensibly
    NoFramebuffer,       // the firmware's framebuffer is unknown
    FramebufferOutside,  // the firmware's framebuffer is not inside the carve-out the registers describe: the registers do not describe this machine
    TooSmall,            // not enough room past the firmware's part of video memory
    ConfigDiffers,       // the graphics block reports another address configuration than this chip variant has
  };
  Result result = Result::NoMemoryMap;
  uint32_t gb_addr_config = 0;
  AmdGbAddrConfigCheck gb_check = AmdGbAddrConfigCheck::HardwareUnreadable;
  uint64_t block_offset = 0, rings_offset = 0;                // within video memory; MiB aligned
  uint64_t block_gpu_address = 0, rings_gpu_address = 0;     // as the GPU addresses them
  uint64_t block_physical = 0, rings_physical = 0;           // as the CPU addresses them
  // What is left of video memory past the rings, short of the tail guard: where decoding sessions put their buffers (picture buffers, bitstream, messages). Whole
  // MiB; 0 bytes when there is none.
  uint64_t work_offset = 0, work_bytes = 0;
  uint64_t work_gpu_address = 0, work_physical = 0;
  uint64_t block_bytes = 0;                                   // what AmdVcn1 needs for the firmware
};
const char* amd_vcn1_plan_result_name(AmdVcn1Plan::Result result);

// Chooses the block (for `block_bytes`, AmdVcn1::block_bytes(firmware)) and the rings. `framebuffer_physical`/`framebuffer_bytes` are the firmware's display buffer
// (the UEFI graphics output's), which must lie inside the carve-out. Pure arithmetic: nothing is read or mapped. Ok means every address is consistent and clear of the
// firmware's part; it does not mean the memory is safe to touch (map_amd_vcn1_plan refuses a range that is memory the OS owns).
AmdVcn1Plan amd_plan_vcn1_memory(const AmdRavenMemoryMap& map, uint16_t pci_device_id, uint8_t pci_revision, uint64_t block_bytes, uint64_t framebuffer_physical,
                                 uint64_t framebuffer_bytes);

// Maps the planned block and rings for the CPU. Only a plan that is Ok is mapped; the mapper refuses a range that overlaps memory the OS owns (then false and
// nothing is mapped).
bool amd_map_vcn1_plan(const AmdVcn1Plan& plan, IRegisterMapper& mapper, AmdVcn1::Memory* memory);

}  // namespace hardware
