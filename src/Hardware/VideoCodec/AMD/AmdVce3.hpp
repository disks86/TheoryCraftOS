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

// Which VCE 3.4 firmware file a Radeon RX Vega M (Kaby Lake-G, Dell XPS 15 9575; PCI 1002:694C, 694E, 694F; amdgpu's VEGAM) needs, as the amdgpu driver names it
// (amdgpu_vce.c). nullptr for any other device.
const char* amd_vce3_firmware_name(uint16_t pci_device_id);

// The start and stop sequences of the Video Coding Engine 3.x, the H.264 encoder of the Polaris-family graphics cards (VCE 3.4 in the Vega M), ported from the Linux
// amdgpu driver's vce_v3_0.c (vce_v3_0_start, _stop, _mc_resume, _firmware_loaded, the clock gating override and software clock gating, _get_harvest_config) and
// amdgpu_vce.c (the memory layout and the microcode copy). Every register write and wait is in the order the driver makes it; the register numbers are generated from AMD's own
// headers (AmdVce3Registers.inc), and scripts/check_vce3_port.py compares the register order with the driver's. The registers are the card's register BAR (BAR 5), numbered by
// dword.
//
// The engine comes in two instances (VCE0 and VCE1) selected through GRBM_GFX_INDEX; the Polaris chips have the second one fused off, so the start only programs the first.
// Each instance has three rings (the third with firmware 52.8.3 and later).
//
// What this is NOT: it does not power or clock the engine (the driver asks the card's SMC, which AmdSmu7 does for the power; the clocks it sets with amdgpu_asic_set_vce_clocks
// and the dpm tables are not here), does not find where in the card's memory the engine may be given a block (the caller supplies that, as for AmdUvd6), and does not
// submit anything: the commands and the encode messages are for the next pieces. Not run on a real card.
class AmdVce3 {
 public:
  static constexpr uint64_t kPageSize = 4096;
  // vce_v3_0.c / amdgpu_vce.h
  static constexpr uint32_t kFirmwareOffset = 256;           // AMDGPU_VCE_FIRMWARE_OFFSET
  static constexpr uint32_t kFirmwareWindowBytes = 384 * 1024;   // VCE_V3_0_FW_SIZE
  static constexpr uint32_t kStackBytes = 64 * 1024;         // VCE_V3_0_STACK_SIZE
  static constexpr uint32_t kMaxHandles = 16;                // AMDGPU_MAX_VCE_HANDLES
  static constexpr uint32_t kDataBytes = 16 * 1024 * kMaxHandles + 52 * 1024;   // VCE_V3_0_DATA_SIZE
  // One block holds the microcode window and, for each of the two instances, a stack and a data area.
  static constexpr uint64_t kBlockBytes = kFirmwareWindowBytes + uint64_t{kStackBytes + kDataBytes} * 2;
  static constexpr uint32_t kRingBytes = 4096;               // ring_size of the driver's rings (512 dwords of room for two submissions)
  static constexpr uint32_t kMaxRings = 3;
  static constexpr uint32_t kHarvestVce0 = 1, kHarvestVce1 = 2;   // AMDGPU_VCE_HARVEST_VCE0 / _VCE1

  // The memory the engine works in: one block (microcode, stacks, data) and the rings. Each window is the card's memory mapped for the CPU; the GPU address is the same memory as
  // the GPU addresses it (not the CPU address!). Page aligned, the ranges apart; the block at least kBlockBytes, each ring kRingBytes.
  struct Memory {
    uint64_t block_gpu_address = 0;
    MmioRegion block;
    uint64_t ring_gpu_address[kMaxRings] = {};
    MmioRegion ring[kMaxRings];
  };
  // What the driver reads from the machine's configuration and keeps between starts.
  struct Config {
    uint32_t harvest_config = kHarvestVce1;   // which instances are fused off (read_harvest_config())
    uint32_t rings = 3;                       // rings used: 3 with firmware 52.8.3 and later, else 2
    uint32_t ring_write_pointer[kMaxRings] = {};   // ring->wptr: where each ring's pointers restart
    bool medium_grain_clock_gating = false;   // cg_flags & AMD_CG_SUPPORT_VCE_MGCG
    // How many times the ECPU is given its 100 x 10 ms to answer, resetting it between tries; the driver makes 10 (up to ten seconds). At least one is always made.
    uint32_t boot_attempts = 10;
  };
  enum class Status : uint8_t { Ok, BadFirmware, BadMemory, NoEngine, TimedOut };   // NoEngine: both instances are fused off

  // What the engine's registers say, for the first run on a card that does not do what the driver's does: VCE_STATUS (bit 1: the microcode reported itself loaded), the
  // soft reset, VCPU control, the LMI control, the two system interrupt registers and the first ring's pointers - of the first instance the start programs. Reads only.
  // `failure_snapshot()` is the state at the moment a start gave up, before the stop sequence changed it.
  struct Diagnostics {
    uint32_t status = 0, soft_reset = 0, vcpu_cntl = 0, lmi_ctrl = 0, lmi_ctrl2 = 0, rb_rptr = 0, rb_wptr = 0;
  };
  static Diagnostics read_diagnostics(IVcnRegisters& registers, uint32_t harvest_config);

  AmdVce3(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}

  // vce_v3_0_get_harvest_config: which instances are fused off, read from the SMC's CC_HARVEST_FUSES (a discrete card; an APU keeps them elsewhere and is not handled);
  // a Polaris-family chip with no fuse set has its second instance off.
  static uint32_t read_harvest_config(IVcnRegisters& registers);
  // True when the firmware is one VCE 3.x image (IP version 3.x) of a size the block's window holds.
  static bool firmware_usable(const AmdVcnFirmware& firmware);
  // The firmware's encoder version, for the command set (the driver's adev->vce.fw_version: major << 24 | minor << 16 | binary id << 8) and for the ring count.
  static uint32_t firmware_version(const AmdVcnFirmware& firmware);
  static uint32_t rings_for(uint32_t firmware_version);

  // Copies the microcode to the start of the block, clears the rest, ungates the clocks (when asked) and starts every instance that is not fused off. Ok only when each
  // instance's ECPU answered. On BadMemory / BadFirmware nothing was written anywhere; on TimedOut the memory was written and the engine was stopped again.
  Status start(const AmdVcnFirmware& firmware, const Memory& memory, const Config& config);
  // Puts the engine back to sleep (the driver's stop sequence). Safe to call when it is not running.
  void stop();
  bool started() const { return started_; }
  const Diagnostics& failure_snapshot() const { return failure_; }

 private:
  IVcnRegisters& registers_;
  const IClock& clock_;
  Config config_;
  Diagnostics failure_;
  bool started_ = false;

  uint32_t rd(uint32_t dword) { return registers_.read(dword); }
  void wr(uint32_t dword, uint32_t value) { registers_.write(dword, value); }
  void delay_ms(uint32_t ms);
  void override_vce_clock_gating(bool override);
  void set_vce_sw_clock_gating(bool gated);
  void set_clockgating_state(bool enable);
  void mc_resume(const Memory& memory, uint32_t idx);
  bool firmware_loaded();
  Status run_start(const Memory& memory);
};

}  // namespace hardware
