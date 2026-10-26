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
#include "Hardware/VideoCodec/AMD/AmdVcnFirmware.hpp"

namespace hardware {

// Which UVD 6.3 firmware file a Radeon RX Vega M (Kaby Lake-G, Dell XPS 15 9575; PCI 1002:694C, 694E, 694F; amdgpu calls the chip VEGAM) needs, as the amdgpu driver
// names it. nullptr for any other device.
const char* amd_uvd6_firmware_name(uint16_t pci_device_id);

// The start and stop sequences of the Unified Video Decoder 6.x, the video engine of the Polaris-family graphics cards (UVD 6.3 in the Vega M), ported from the Linux
// amdgpu driver's uvd_v6_0.c (uvd_v6_0_mc_resume, uvd_v6_0_start, uvd_v6_0_stop, the clock gating "ungate" path of hw_init) and amdgpu_uvd.c (the memory layout and the
// microcode copy). Every register write and wait is in the order the driver makes it; the register numbers are generated from AMD's own headers (AmdUvd6Registers.inc), and
// scripts/check_uvd6_port.py compares the register order with the driver's. The registers are the card's register BAR (BAR 5), numbered by dword.
//
// What this is NOT: it does not power or clock the engine (the driver asks the card's power management controller, the SMC, for that; on a card the firmware has set up
// the engine is usually powered, and a status register that reads all ones or stays at zero says it is not), does not find where in the card's memory the engine may
// be given a block or what the memory controller's base address is (the caller supplies those), and does not decode anything: the decode ring it programs, and the
// messages that go through it, are for the next piece. It also leaves the encode rings alone (decode only) and does not turn medium-grain clock gating on.
class AmdUvd6 {
 public:
  static constexpr uint64_t kPageSize = 4096;
  // amdgpu_uvd.h
  static constexpr uint32_t kStackBytes = 200 * 1024;
  static constexpr uint32_t kHeapBytes = 256 * 1024;
  static constexpr uint32_t kSessionBytes = 50 * 1024;
  static constexpr uint32_t kDefaultHandles = 10, kMaxHandles = 40;
  static constexpr uint32_t kFirmwareOffset = 256;   // AMDGPU_UVD_FIRMWARE_OFFSET: where the engine expects the microcode to start in its address space
  static constexpr uint64_t kRingBytes = 4096;

  // The memory the engine works in: one block (microcode, heap, stack, sessions) and the decode ring. `block` is mapped for the CPU, `block_gpu_address` is the same
  // memory as the GPU addresses it (not the CPU address!); both must be page aligned, the windows at least block_bytes() / kRingBytes long, and the two ranges apart.
  struct Memory {
    uint64_t block_gpu_address = 0;
    MmioRegion block;
    uint64_t ring_gpu_address = 0;
    MmioRegion ring;
  };
  // What the driver reads from the machine's configuration: the graphics block's GB_ADDR_CONFIG (the tiling configuration the engine's memory interfaces must match),
  // and how many sessions the firmware supports (max_handles_for()).
  struct Config {
    uint32_t gb_addr_config = 0;
    uint32_t max_handles = kDefaultHandles;
    // pg_flags & AMD_PG_SUPPORT_UVD and cg_flags & AMD_CG_SUPPORT_UVD_MGCG: whether the machine power gates the engine's VCPU / gates its memory clocks by itself.
    bool power_gating = false;
    bool medium_grain_clock_gating = false;
    // How many times the VCPU is given its 100 x 10 ms to answer, resetting it between tries; the driver makes 10 (up to ten seconds). At least one is always made.
    uint32_t boot_attempts = 10;
  };
  enum class Status : uint8_t { Ok, BadFirmware, BadMemory, TimedOut };

  // What the engine's registers say, for the first run on a card that does not do what the driver's does: UVD_STATUS (bit 1: the VCPU answered), UVD_POWER_STATUS, UVD_SOFT_RESET,
  // UVD_VCPU_CNTL, UVD_LMI_CTRL2, and the ring's read and write pointers. Reads only. `failure_snapshot()` is the state at the moment a start gave up, before the stop sequence
  // changed it.
  struct Diagnostics {
    uint32_t status = 0, power_status = 0, soft_reset = 0, vcpu_cntl = 0, lmi_ctrl2 = 0, rb_rptr = 0, rb_wptr = 0;
  };
  static Diagnostics read_diagnostics(IVcnRegisters& registers);
  const Diagnostics& failure_snapshot() const { return failure_; }

  AmdUvd6(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}

  // The number of decode sessions a firmware supports: 40 from version 1.80 on (the driver's rule), else 10.
  static uint32_t max_handles_for(const AmdVcnFirmware& firmware);
  // The window of the microcode in the engine's address space, as the driver sizes it: the microcode plus 8 bytes, in whole pages.
  static uint64_t firmware_window_bytes(const AmdVcnFirmware& firmware);
  // Bytes of memory the block needs: that window, the heap, the stack and the session data.
  static uint64_t block_bytes(const AmdVcnFirmware& firmware, uint32_t max_handles);

  // Copies the microcode into the block, clears the rest, ungates the engine's clocks and starts it. Ok only when the engine's VCPU answered. On BadMemory / BadFirmware
  // nothing was written anywhere; on TimedOut the memory was written and the engine was stopped again (the driver's stop sequence).
  Status start(const AmdVcnFirmware& firmware, const Memory& memory, const Config& config);
  // Puts the engine back to sleep (the driver's stop sequence). Safe to call when it is not running.
  void stop();
  bool started() const { return started_; }

 private:
  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  Diagnostics failure_;
  bool started_ = false;

  uint32_t rd(uint32_t dword) { return registers_.read(dword); }
  void wr(uint32_t dword, uint32_t value) { registers_.write(dword, value); }
  void delay_ms(uint32_t ms);
  uint32_t read_ctx(uint32_t reg);
  void write_ctx(uint32_t reg, uint32_t value);
  void enable_mgcg(bool enable);
  void enable_clock_gating(bool enable);
  void set_sw_clock_gating();
  void mc_resume(const AmdVcnFirmware& firmware, const Memory& memory);
  Status run_start(const AmdVcnFirmware& firmware, const Memory& memory);
};

}  // namespace hardware
