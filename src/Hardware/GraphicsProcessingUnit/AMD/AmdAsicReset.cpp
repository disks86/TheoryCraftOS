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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdAsicReset.hpp"

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): vi.c (vi_asic_reset, vi_asic_pci_config_reset), amdgpu_device.c
// (amdgpu_device_pci_config_reset), amdgpu_atombios.c (amdgpu_atombios_scratch_regs_engine_hung). scripts/check_asic_reset_port.py checks the numbers against the kernel's sources.

namespace hardware {

const char* AmdAsicReset::result_name(Result result) {
  switch (result) {
    case Result::Ok: return "the card is back";
    case Result::NotComingBack: return "the card did not answer after the reset";
  }
  return "unknown";
}

namespace {
// pci_set_master / pci_clear_master: the bus master bit of the command register (the low half of the dword at 4; the status register in the high half clears when written, so zeros).
void set_bus_master(IPciBus& pci, PciAddress function, bool on) {
  const uint32_t command = pci.read32(function, AmdAsicReset::kPciCommandOffset) & 0xFFFF;
  pci.write32(function, AmdAsicReset::kPciCommandOffset, on ? (command | AmdAsicReset::kPciBusMaster) : (command & ~AmdAsicReset::kPciBusMaster));
}

// amdgpu_atombios_scratch_regs_engine_hung
void scratch_engine_hung(IVcnRegisters& registers, bool hung) {
  uint32_t tmp = registers.read(AmdAsicReset::kBiosScratch3);
  if (hung) tmp |= AmdAsicReset::kAtomS3AsicGuiEngineHung;
  else tmp &= ~AmdAsicReset::kAtomS3AsicGuiEngineHung;
  registers.write(AmdAsicReset::kBiosScratch3, tmp);
}

void wait_us(const IClock& clock, uint32_t us) {
  const uint64_t begin = clock.now_us();
  while (clock.now_us() - begin < us) {}
}
}  // namespace

AmdAsicReset::Result AmdAsicReset::pci_config_reset(IVcnRegisters& registers, const IClock& clock, IPciBus& pci, PciAddress function, uint32_t timeout_us) {
  Result result = Result::NotComingBack;
  scratch_engine_hung(registers, true);

  /* disable BM */
  set_bus_master(pci, function, false);
  /* reset */
  pci.write32(function, kPciResetOffset, kPciResetData);

  wait_us(clock, 100);

  /* wait for asic to come out of reset */
  const uint64_t begin = clock.now_us();
  do {
    if (registers.read(kConfigMemsize) != 0xffffffff) {
      /* enable BM */
      set_bus_master(pci, function, true);
      result = Result::Ok;
      break;
    }
  } while (clock.now_us() - begin < timeout_us);

  scratch_engine_hung(registers, false);
  return result;
}

}  // namespace hardware
