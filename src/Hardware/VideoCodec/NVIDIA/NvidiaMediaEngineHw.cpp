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

#include "Hardware/VideoCodec/NVIDIA/NvidiaMediaEngineHw.hpp"

namespace hardware {

NvidiaChipStatus NvidiaMediaEngineHw::probe(const MmioRegion& registers) {
  NvidiaChipStatus status;
  if (registers.length() < kRegisterWindow) return status;
  const uint32_t boot0 = registers.read32(kNvidiaBoot0Offset);
  if (boot0 == 0 || boot0 == 0xFFFFFFFFu) return status;
  const NvidiaGpuInfo info = nvidia_gpu_identify_boot0(boot0);
  if (info.architecture == NvidiaGpuArchitecture::Unknown) return status;

  const uint32_t chipset = nvidia_boot0_chipset(boot0);
  // From Turing on the chip ID is repeated in BOOT_42 (architecture 28:24 and implementation 23:20 are the chipset's bits 8:4 and 3:0 again). If it is
  // there, it has to say the same thing; two registers that disagree mean this is not a GPU we are reading right.
  const uint32_t boot42 = registers.read32(kNvidiaBoot42Offset);
  const bool boot42_present = info.architecture >= NvidiaGpuArchitecture::Turing && boot42 != 0 && boot42 != 0xFFFFFFFFu;
  if (boot42_present && nvidia_boot0_chipset(boot42) != chipset) return status;

  status.probed = true;
  status.boot0 = boot0;
  status.chipset = chipset;
  status.stepping_major = static_cast<uint8_t>((boot0 >> 4) & 0xF);
  status.stepping_minor = static_cast<uint8_t>(boot0 & 0xF);
  status.architecture = info.architecture;
  status.chip = nvidia_chip_name(chipset);
  status.boot42_present = boot42_present;
  status.has_nvenc = nvidia_chip_has_nvenc(chipset, info.architecture);
  return status;
}

}  // namespace hardware
