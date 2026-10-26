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
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdVce3.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Work submission to a ring of the Video Coding Engine 3.x: what the Linux driver does to run a job on a chip from Stoney on (the "VM" ring functions of vce_v3_0.c:
// vce_v3_0_ring_emit_ib; amdgpu_vce.c: amdgpu_vce_ring_emit_fence, amdgpu_vce_ring_test_ring; the ring pointers through GRBM_GFX_INDEX as vce_v3_0_ring_get_rptr /
// _get_wptr / _set_wptr do; the nop command and the ring's align_mask of 0xf; the commands' values from vid.h). The ring is a list of command words; the one that
// matters is the indirect buffer: it holds the firmware's own task commands (the encode messages of the next piece), and the ring points the engine at it.
//
// Every job is: ring: IB_VM (VMID 0, address, length in dwords), FENCE (address, sequence), TRAP, END, padding to 16 dwords. The job's end is seen by the fence: the
// firmware writes the job's number to a word of memory the CPU polls. A job that does not finish in time leaves the engine in an unknown state: the ring refuses further
// jobs (wedged()) until the caller stops and restarts the engine.
//
// The ring, the indirect buffer and the fence word are in the card's memory and are written through its memory BAR, so the host data path is flushed before the engine is
// told (vi_flush_hdp). Like the start sequence, this has not been run on real hardware.
class AmdVce3Ring {
 public:
  static constexpr uint32_t kRingDwords = AmdVce3::kRingBytes / 4;
  static constexpr uint32_t kIndirectBufferBytes = 4096;
  static constexpr uint32_t kMaxIndirectBufferDwords = kIndirectBufferBytes / 4;
  static constexpr uint32_t kFenceBytes = 64;
  static constexpr uint32_t kAlignDwords = 16;   // align_mask 0xf plus one
  // vid.h
  static constexpr uint32_t kCmdNoOp = 0x00000000, kCmdEnd = 0x00000001, kCmdIb = 0x00000002, kCmdFence = 0x00000003, kCmdTrap = 0x00000004, kCmdIbVm = 0x00000102;

  // Where everything lives: each window with the address the GPU uses for the same memory. The ring is one of the rings AmdVce3::start programmed (Memory::ring[index]); the
  // indirect buffer and the fence word must not overlap anything else and are only written by the CPU (IB) or by the engine (fence).
  struct Memory {
    uint64_t ib_gpu_address = 0;
    MmioRegion ib;           // kIndirectBufferBytes
    uint64_t fence_gpu_address = 0;
    MmioRegion fence;        // kFenceBytes; the engine's fence writes go to the first word
    MmioRegion ring;         // AmdVce3::kRingBytes
  };
  enum class Result : uint8_t { Ok, BadArgument, TimedOut, Wedged };

  // `ring_index` is the ring (0 to 2: VCE_RB_*, VCE_RB_*2, VCE_RB_*3), `harvest_config` the fuses AmdVce3 was started with (the ring's pointers are those of the instance
  // that is there).
  AmdVce3Ring(IVcnRegisters& registers, const IClock& clock, const Memory& memory, uint32_t ring_index, uint32_t harvest_config);

  // amdgpu_vce_ring_test_ring: writes an END command through the ring and waits for the engine to have moved its read pointer. False if it never does (the engine is not running).
  bool test(uint32_t timeout_us = 100000);
  // Runs one job: `ib` is the task commands for the firmware (at most kMaxIndirectBufferDwords; padded here with no-ops to a multiple of 16 dwords), written to the indirect
  // buffer and executed. Waits up to `timeout_ms` for the fence.
  Result submit(ArrayView<const uint32_t> ib, uint32_t timeout_ms);
  bool wedged() const { return wedged_; }
  uint32_t jobs_completed() const { return sequence_ - 1; }
  // Where the next command goes (the ring's write pointer, to hand to AmdVce3::Config::ring_write_pointer when the engine is restarted).
  uint32_t write_pointer() const { return wptr_; }
  uint32_t read_pointer();

  // The ring commands of a job in front of the padding, for tests and diagnostics: `out` receives the dwords, the count is returned (0 if it does not fit).
  static uint32_t build_job_commands(uint64_t ib_gpu_address, uint32_t ib_dwords, uint64_t fence_gpu_address, uint32_t sequence, ArrayView<uint32_t> out);

 private:
  void ring_write(uint32_t dword);
  void commit();
  uint32_t instance() const { return (harvest_config_ & AmdVce3::kHarvestVce0) ? 1 : 0; }   // the instance the ring's registers are those of
  uint32_t read_ring_register(uint32_t which);
  void write_write_pointer(uint32_t value);

  IVcnRegisters& registers_;
  const IClock& clock_;
  Memory memory_;
  uint32_t index_;
  uint32_t harvest_config_;
  uint32_t wptr_ = 0;       // dwords written, modulo the ring
  uint32_t sequence_ = 1;   // the number the next job's fence writes
  bool wedged_ = false;
};

}  // namespace hardware
