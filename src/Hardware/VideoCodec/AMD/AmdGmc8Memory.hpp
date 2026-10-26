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
#include "Hardware/VideoCodec/AMD/AmdUvd6.hpp"

namespace hardware {

// Where things are on a Polaris-family graphics card with its own memory (the Radeon RX Vega M of the Dell XPS 15 9575), for starting its video engine (AmdUvd6) without
// guessing: what the memory controller says about video memory, and a block of video memory the engine can be handed that nothing else is using. Everything here only
// READS the card; mapping a window and the flush of the host data path are the only things it does to the machine.
//
// A card's video memory has two addresses for the same byte: the offset into the memory that the CPU adds to the start of the card's memory BAR (BAR 0, the "aperture",
// which on a card without Resizable BAR shows only the first 256 MiB), and the address the GPU uses, the memory controller's base plus the offset. The Linux amdgpu
// driver reads the GPU base from MC_VM_FB_LOCATION and the size from CONFIG_MEMSIZE (gmc_v8_0.c: gmc_v8_0_vram_gtt_location, gmc_v8_0_mc_init) and the registers are read here the
// same way. The card's firmware has already set the memory controller up (it ran the video BIOS to start the card); nothing here writes to it.

// The register block of the card is a 256 KiB BAR (BAR 5) of which every register below is a part.
constexpr uint64_t kAmdGmc8RegisterBytes = 0x40000;

struct AmdGmc8MemoryMap {
  bool read = false;                  // the registers answered with values that can be a card's video memory
  uint64_t video_memory_bytes = 0;    // CONFIG_MEMSIZE, in MiB, as bytes
  uint64_t gpu_base = 0;              // MC_VM_FB_LOCATION.FB_BASE << 24: the GPU's address of the first byte of video memory
  uint64_t gpu_top = 0;               // (MC_VM_FB_LOCATION.FB_TOP + 1) << 24: one past the last
};
AmdGmc8MemoryMap amd_read_gmc8_memory_map(IVcnRegisters& registers);

// Makes what the CPU wrote to video memory (through the aperture, which the host data path buffers) visible to the GPU: vi_flush_hdp, a write of 1 to
// HDP_MEM_COHERENCY_FLUSH_CNTL and a read back. Call after writing and before the engine reads.
void amd_gmc8_flush_hdp(IVcnRegisters& registers);

// The firmware keeps the start of video memory (the framebuffer it left, and at least the 9 MiB the driver reserves for the legacy VGA area), which the planner never
// uses. The block goes at the end of the part of video memory the CPU can reach, short of a guard: video memory past the first 256 MiB is not reachable without
// Resizable BAR, and the start of it is the firmware's.
constexpr uint64_t kAmdUvd6VgaAllocation = 9ull << 20;
constexpr uint64_t kAmdUvd6HeadGuardBytes = 16ull << 20;   // between the firmware's part and the planned memory
constexpr uint64_t kAmdUvd6TailGuardBytes = 16ull << 20;   // between the planned memory and the end of what the CPU can reach
constexpr uint64_t kAmdUvd6MinApertureBytes = 64ull << 20;

struct AmdUvd6Plan {
  enum class Result : uint8_t {
    Ok,
    NoMemoryMap,         // the memory controller's registers did not answer sensibly
    ApertureTooSmall,    // the memory BAR is too small to hold the engine's memory
    TooSmall,            // not enough room past the firmware's part of video memory
  };
  Result result = Result::NoMemoryMap;
  uint64_t visible_bytes = 0;                    // the part of video memory the CPU can reach
  uint64_t block_offset = 0, ring_offset = 0;    // within video memory; MiB aligned
  uint64_t block_bytes = 0;                      // what AmdUvd6 needs for the firmware
  uint64_t block_gpu_address = 0, ring_gpu_address = 0;
  // What is left up to the tail guard: where decoding sessions put their buffers (picture buffers, bitstream, messages). Whole MiB.
  uint64_t work_offset = 0, work_bytes = 0, work_gpu_address = 0;
};
const char* amd_uvd6_plan_result_name(AmdUvd6Plan::Result result);

// Chooses the block (for `block_bytes`, AmdUvd6::block_bytes(firmware, handles)), the ring and the work area, in that order, ending at the tail guard. `aperture_bytes` is the
// size of the memory BAR. The work area is `work_bytes` if there is room for that, else what there is if it is at least `kMinWorkBytes`. Pure arithmetic: nothing is read or
// mapped.
constexpr uint64_t kAmdUvd6MinWorkBytes = 8ull << 20;
constexpr uint64_t kAmdUvd6DefaultWorkBytes = 96ull << 20;
AmdUvd6Plan amd_plan_uvd6_memory(const AmdGmc8MemoryMap& map, uint64_t aperture_bytes, uint64_t block_bytes, uint64_t work_bytes = kAmdUvd6DefaultWorkBytes);

// The engine's memory and the work area, mapped for the CPU.
struct AmdUvd6Memory {
  AmdUvd6::Memory engine;
  MmioRegion work;
  uint64_t work_gpu_address = 0;
};
// Maps the planned block, ring and work area through the memory BAR (`aperture_physical` is its address). Only a plan that is Ok is mapped; the mapper refuses a range
// that overlaps memory the OS owns (then false and nothing is returned).
bool amd_map_uvd6_plan(const AmdUvd6Plan& plan, IRegisterMapper& mapper, uint64_t aperture_physical, AmdUvd6Memory* memory);

}  // namespace hardware
