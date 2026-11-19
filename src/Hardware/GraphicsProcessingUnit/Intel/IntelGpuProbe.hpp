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

// The probe of a discrete Intel GPU that has no driver here (Iris Xe MAX / DG1, Arc Alchemist / DG2, Arc Battlemage; see Multi/GpuProbe.hpp): the chip from its
// PCI ID, the size of its local memory and the package temperature, read from the register BAR (BAR 0, GTTMMADR) as the Linux xe and i915 drivers read them:
//
// - Local memory: on DG2 and Battlemage SG_TILE_ADDR_RANGE(0) (0x1083A0, bits 17:8 in GiB; xe_vram.c tile_vram_size), the whole tile. DG1 has no such register (xe
//   takes the LMEM BAR's length): the size is GSMBASE (0x108100, 64 bits as two dwords), where the stolen memory at the top starts, which is what i915's
//   intel_region_lmem.c uses for a part without flat CCS. So DG1 reports a little less than its 4 GiB.
// - Temperature: the package temperature in whole degrees, bits 7:0: PCU_CR_PACKAGE_TEMPERATURE (0x140000 + 0x5978, xe_mchbar_regs.h) on DG2 (xe_hwmon.c) and DG1
//   (i915_hwmon.c, PCU_PACKAGE_TEMPERATURE), BMG_PACKAGE_TEMPERATURE (0x138434, xe_pcode_regs.h) on Battlemage. 0 means no reading (xe's mmio_temp_valid).
//
// No register is written, and none of these needs forcewake (they are in the SoC/PCU part of the BAR, outside the GT). resume() reads the size again.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuProbe.hpp"

namespace hardware {

class IntelGpuProbe final : public IGpuProbe {
 public:
  enum class Platform : uint8_t { None, Dg1, Dg2, Battlemage };
  static Platform platform_of(uint16_t device_id);
  static bool supported(uint16_t device_id) { return platform_of(device_id) != Platform::None; }

  static constexpr uint64_t kGsmBase = 0x108100;            // GSMBASE (xe_regs.h; GEN6_GSMBASE in i915_reg.h)
  static constexpr uint64_t kTileAddrRange0 = 0x1083A0;     // SG_TILE_ADDR_RANGE(0) (xe_regs.h)
  static constexpr uint64_t kPcuPackageTemperature = 0x145978;   // PCU_CR_PACKAGE_TEMPERATURE (MCHBAR_MIRROR_BASE_SNB + 0x5978)
  static constexpr uint64_t kBmgPackageTemperature = 0x138434;   // BMG_PACKAGE_TEMPERATURE (xe_pcode_regs.h)
  static constexpr uint64_t kRegisterBytes = 2u << 20;      // the first 2 MiB of BAR 0 (of 16): every register above is in it

  // The conversions: SG_TILE_ADDR_RANGE to bytes, the temperature register to thousandths of a degree (-1: no reading).
  static uint64_t tile_bytes(uint32_t tile_addr_range);
  static int32_t package_temperature(uint32_t value);

  // On the machine: start() maps BAR 0 of `context.device` through `context.registers`.
  explicit IntelGpuProbe(const GpuContext& context);
  // With the registers given (a test's model).
  IntelGpuProbe(intel_gt::IRegisterBank& registers, uint16_t device_id);
  ~IntelGpuProbe() = default;
  IntelGpuProbe(const IntelGpuProbe&) = delete;
  IntelGpuProbe& operator=(const IntelGpuProbe&) = delete;

  const char* name() const override { return info_.name; }
  const char* status() const override { return status_; }
  [[nodiscard]] bool start() override;
  [[nodiscard]] bool resume() override;
  uint64_t vram_bytes() const override { return vram_bytes_; }
  int32_t temperature_millicelsius() override;

  Platform platform() const { return platform_; }

 private:
  bool fail(const char* why) {
    status_ = why;
    return false;
  }
  bool map_registers();
  // False when the GPU does not answer; *bytes is 0 when it answers with no size.
  bool read_vram(uint64_t* bytes);

  GpuContext context_{};   // (a copy: the pool's is gone after the factory); its mapper is null when the registers were given
  IntelGpuInfo info_;
  Platform platform_ = Platform::None;
  intel_gt::MmioRegisterBank mmio_{MmioRegion()};
  intel_gt::IRegisterBank* registers_ = nullptr;
  bool started_ = false;   // start() succeeded (resume() and the sensors need it)
  uint64_t vram_bytes_ = 0;
  const char* status_ = "not started";
};

}  // namespace hardware
