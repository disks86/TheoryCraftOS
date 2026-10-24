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
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// Work submission to the decode ring of Video Core Next 1.0: what the Linux driver does to run a job (vcn_v1_0_dec_ring_insert_start, _emit_ib, _emit_fence,
// _insert_end and the ring commit) and what Mesa puts into the indirect buffer it submits (radeon_vcn_dec.c send_cmd: the address of each buffer through the
// GPCOM registers, then the command that names it). The engine's firmware executes the commands; they are register writes made through packets, so the same
// words run from the ring or from an indirect buffer.
//
// Every job is: ring: start, IB (VMID 0, address, length), fence, end, padding; IB: for each buffer its address and command, and for a picture the engine-start
// write. The job's end is seen by the fence: the firmware writes the job's number to a word of memory the CPU polls. A job that does not finish in time leaves
// the engine in an unknown state: the ring refuses further jobs (wedged()) until the caller stops and restarts the engine.
//
// Like the start sequence, this has not been run on real hardware.
class AmdVcn1Ring {
 public:
  static constexpr uint32_t kRingDwords = static_cast<uint32_t>(AmdVcn1::kRingBytes / 4);
  static constexpr uint32_t kIndirectBufferBytes = 4096;
  static constexpr uint32_t kFenceBytes = 64;
  static constexpr uint32_t kMaxBuffersPerJob = 12;   // one picture uses 8

  // Where everything lives: each window with the address the GPU uses for the same memory. The ring is the decode ring AmdVcn1::start programmed (its first
  // page of Memory::rings); the indirect buffer and the fence word must not overlap anything else and are only written by the CPU (IB) or by the engine (fence).
  struct Memory {
    uint64_t ib_gpu_address = 0;
    MmioRegion ib;           // kIndirectBufferBytes
    uint64_t fence_gpu_address = 0;
    MmioRegion fence;        // kFenceBytes; the engine's fence writes go to the first word
    MmioRegion ring;         // AmdVcn1::kRingBytes
  };
  // One buffer for the engine: the command that names it (amd_vcn::kCmd...) and where it is, as the GPU addresses it.
  struct Buffer {
    uint32_t command = 0;
    uint64_t gpu_address = 0;
  };
  enum class Result : uint8_t { Ok, BadArgument, TimedOut, Wedged };

  AmdVcn1Ring(IVcnRegisters& registers, const IClock& clock, const Memory& memory);

  // Runs one job. `start_engine` appends the write of the engine-control register (UVD_ENGINE_CNTL = 1) that makes the firmware decode the picture whose buffers
  // were just named; messages that only create or destroy a session do without. Waits up to `timeout_ms` for the fence.
  Result submit(ArrayView<const Buffer> buffers, bool start_engine, uint32_t timeout_ms);
  bool wedged() const { return wedged_; }
  uint32_t jobs_completed() const { return sequence_ - 1; }

  // The register writes of a job's indirect buffer, for tests and diagnostics: `out` receives the dwords (pairs of register packet and value), the count is returned
  // (0 if it does not fit).
  static uint32_t build_indirect_buffer(ArrayView<const Buffer> buffers, bool start_engine, ArrayView<uint32_t> out);

 private:
  void ring_write(uint32_t dword);
  void ring_register(uint32_t register_dword, uint32_t value);

  IVcnRegisters& registers_;
  const IClock& clock_;
  Memory memory_;
  uint32_t wptr_ = 0;       // dwords written, modulo the ring
  uint32_t sequence_ = 1;   // the number the next job's fence writes
  bool wedged_ = false;
};

}  // namespace hardware
