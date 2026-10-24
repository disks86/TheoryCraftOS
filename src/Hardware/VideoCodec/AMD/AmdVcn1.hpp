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
#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"

namespace hardware {

// The registers of an AMD GPU as the VCN driver sees them: 32-bit words numbered by dword (the byte offset in the register BAR divided by four), which is
// how AMD's register headers number them.
class IVcnRegisters {
 public:
  virtual uint32_t read(uint32_t dword) = 0;
  virtual void write(uint32_t dword, uint32_t value) = 0;

 protected:
  constexpr IVcnRegisters() = default;
  ~IVcnRegisters() = default;
};

// The register BAR of a real card (BAR 5 of a Raven APU), through the checked MMIO window: an offset outside the window stops the OS instead of reaching
// into another device.
class MmioVcnRegisters final : public IVcnRegisters {
 public:
  explicit MmioVcnRegisters(const MmioRegion& bar) : bar_(bar) {}
  uint32_t read(uint32_t dword) override { return bar_.read32(uint64_t{dword} * 4); }
  void write(uint32_t dword, uint32_t value) override { bar_.write32(uint64_t{dword} * 4, value); }

 private:
  MmioRegion bar_;
};

// The start and stop sequences of Video Core Next 1.0, the video engine of the Ryzen 2000/3000 "Raven" APUs (Raven Ridge, Picasso, Raven2), ported from the
// Linux amdgpu driver's vcn_v1_0.c (single power-gated mode, microcode loaded directly into the engine's memory) and amdgpu_vcn.c. Every register write and
// wait is in the order the driver makes it; the register numbers are generated from AMD's own headers (see AmdVcn1Registers.inc).
//
// What this is NOT: nothing here finds out where in memory the engine may be given a block, what the memory controller's base address is, or the GPU's
// address configuration - the caller supplies those (AmdRavenMemory reads them), and a wrong address makes the engine's DMA hit whatever is there. It does not load
// firmware through the PSP (the driver's default on Raven), program the JPEG ring, or decode anything: the decode ring it programs is used by AmdVcn1Ring, and
// AmdVcnVideoEngine starts it at boot (see Memory and Config).
class AmdVcn1 {
 public:
  // The microcode is followed by the engine's stack and context windows and one page of data shared with the firmware; the rings are three more pages.
  static constexpr uint64_t kPageSize = 4096;
  static constexpr uint64_t kStackBytes = 128 * 1024;
  static constexpr uint64_t kContextBytes = 512 * 1024;
  static constexpr uint64_t kSharedBytes = kPageSize;
  static constexpr uint64_t kRingBytes = 4096;      // each of the decode ring and the two encode rings
  static constexpr uint64_t kRingsTotalBytes = 3 * kRingBytes;
  static constexpr uint32_t kFirmwareOffset = 256;  // where the firmware expects to see itself in the engine's address space (AMDGPU_UVD_FIRMWARE_OFFSET)

  // The memory the engine works in. `block` is mapped for the CPU, `block_gpu_address` is the same memory as the GPU addresses it (not the CPU address!);
  // both must be page aligned, and the window at least block_bytes() long. The same for the rings.
  struct Memory {
    uint64_t block_gpu_address = 0;
    MmioRegion block;
    uint64_t rings_gpu_address = 0;
    MmioRegion rings;
  };
  // What the driver reads from the machine's configuration: power gating of the engine (pg_flags & AMD_PG_SUPPORT_VCN), medium-grain clock gating
  // (cg_flags & AMD_CG_SUPPORT_VCN_MGCG), and the graphics block's GB_ADDR_CONFIG (the tiling configuration the engine's memory interfaces must match).
  struct Config {
    bool power_gating = false;
    bool clock_gating = false;
    uint32_t gb_addr_config = 0;
    // How many times the VCPU is given its 100 x 10 ms to answer, resetting it between tries. The driver makes 10 attempts - up to ten seconds when it never
    // answers - which is far too long to hold up boot, so the caller may ask for fewer (at least one is always made). The sequence itself is unchanged.
    uint32_t boot_attempts = 10;
  };
  enum class Status : uint8_t { Ok, BadFirmware, BadMemory, TimedOut, NoMemory };

  AmdVcn1(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}

  // Bytes of memory the block needs for this firmware (the microcode's window, stack, context, shared page).
  static uint64_t block_bytes(const AmdVcnFirmware& firmware);
  // The window in the engine's address space the microcode occupies: the size of the file plus four bytes, in whole pages.
  static uint64_t firmware_window_bytes(const AmdVcnFirmware& firmware);

  // Copies the microcode into the block, clears the rest, and starts the engine. Ok only when the engine's VCPU answered. On BadMemory / BadFirmware nothing
  // was written anywhere; on TimedOut the memory was written and the engine was stopped again (the driver's stop sequence, as its hw_fini does after a failed start).
  Status start(const AmdVcnFirmware& firmware, const Memory& memory, const Config& config);
  // Puts the engine back to sleep (the driver's stop sequence). Safe to call when it is not running.
  void stop();

 private:
  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  bool started_ = false;

  uint32_t rd(uint32_t dword) { return registers_.read(dword); }
  void wr(uint32_t dword, uint32_t value) { registers_.write(dword, value); }
  bool wait_on(uint32_t dword, uint32_t expected, uint32_t mask);
  void delay_ms(uint32_t ms);
  void disable_static_power_gating();
  void enable_static_power_gating();
  void disable_clock_gating();
  void enable_clock_gating();
  void mc_resume(const AmdVcnFirmware& firmware, const Memory& memory);
  Status start_spg_mode(const AmdVcnFirmware& firmware, const Memory& memory);
};

}  // namespace hardware
