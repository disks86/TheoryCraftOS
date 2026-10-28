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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

// The full reset of the Volcanic Islands generation (Polaris, VEGAM) that the Linux amdgpu driver falls back to when the soft reset of an engine does not help: vi_asic_reset ->
// vi_asic_pci_config_reset. The card is told to reset itself by a magic value written to a dword of its PCI configuration space (offset 0x7c, AMDGPU_ASIC_RESET_DATA), and comes
// back at its power-on state. The VEGAM does not use the other method the driver has for this generation, BACO (power-off through the SMU; amdgpu_dpm_is_baco_supported is only for
// Fiji, Tonga, Polaris 10, 11 and 12 and Topaz), so this is the reset it gets.
//
// The sequence of vi_asic_pci_config_reset: the video BIOS scratch register that says the graphics engine is hung is set (so a POST by the firmware knows), bus mastering is switched
// off, the reset value is written, 100 us later the card is given up to `timeout_us` to answer reads of CONFIG_MEMSIZE again (it reads all ones while it is in reset), bus mastering
// is switched on again if it did, and the hung flag is cleared either way.
//
// What this does not do is what the driver does next: after a hardware reset it marks the card as needing a POST and runs the video BIOS's init tables through the AtomBIOS
// interpreter (the memory controller's training, the clocks, the display PLLs), and then initialises every block again. This OS has no AtomBIOS interpreter, so a card that has been
// reset comes back quiet and safe (no engine running, no bus master, no DMA) but its video memory is not trained; AmdVegaM::bring_up after it is expected to fail on a real card until
// the card is POSTed.
class AmdAsicReset {
 public:
  static constexpr uint32_t kPciResetOffset = 0x7c;
  static constexpr uint32_t kPciResetData = 0x39d5e86b;           // AMDGPU_ASIC_RESET_DATA
  static constexpr uint32_t kPciCommandOffset = 0x04;
  static constexpr uint32_t kPciBusMaster = 1u << 2;
  static constexpr uint32_t kBiosScratch3 = 0x5c9 + 3;            // adev->bios_scratch_reg_offset (mmBIOS_SCRATCH_0) + 3
  static constexpr uint32_t kAtomS3AsicGuiEngineHung = 0x20000000;
  static constexpr uint32_t kConfigMemsize = 0x150a;              // mmCONFIG_MEMSIZE
  static constexpr uint32_t kDefaultTimeoutUs = 100000;           // adev->usec_timeout

  enum class Result : uint8_t {
    Ok,
    NotComingBack,   // CONFIG_MEMSIZE still read all ones after the timeout
  };
  static const char* result_name(Result result);

  // Resets the card at `function` on `pci` whose registers are `registers`. On Ok the card answers again, bus mastering is on; on NotComingBack bus mastering stays off.
  static Result pci_config_reset(IVcnRegisters& registers, const IClock& clock, IPciBus& pci, PciAddress function, uint32_t timeout_us = kDefaultTimeoutUs);
};

}  // namespace hardware
