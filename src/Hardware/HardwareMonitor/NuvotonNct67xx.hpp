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

// The hardware monitor of the Nuvoton NCT6775 .. NCT6799 (Linux nct6775). Registers are 16-bit numbers, bank << 8 | index, reached through the
// address (base+5) and data (base+6) ports after the bank is written to index 0x4E. Bank 0 index 0x4F gives the vendor id (0xA3 or 0x5C).
//
//   Voltages: 8 mV per count, doubled for the inputs behind an internal divider (AVCC/AVSB, 3VCC, 3VSB, VBAT).
//   Temperatures: 0x027 one byte in degrees, 0x150 and 0x250 a word in half degrees (high byte degrees, bit 7 of the low byte the half); what each
//     reads is chosen by its source register (0x621..0x623), which also gives the label.
//   Fans: NCT6775 a 16-bit count, NCT6776 a 13-bit count (both RPM = 1350000 / count), NCT6779 and later the RPM itself.
//   PWM: the duty written at 0x109, 0x209, ... (bank 1, 2, 3, 8, 9, A, B index 9), the duty in effect read back at 0x001, 0x003, 0x011, ...,
//     and the mode in bits 7:4 of 0x102, 0x202, ... (0 manual, otherwise one of the Smart Fan modes).
// Per-chip differences are in tables (NuvotonLayout), not in the code.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"
#include "Hardware/HardwareMonitor/SuperIo.hpp"

namespace hardware {

struct NuvotonTemperatureChannel {
  uint16_t reg;
  bool half_degrees;   // a word with the half degree in bit 7 of the second byte
  uint16_t source_reg;
};

enum class NuvotonFanFormat : uint8_t { Count16, Count13, Rpm };

struct NuvotonLayout {
  ArrayView<const uint16_t> voltage_regs;
  ArrayView<const uint16_t> voltage_scale;   // 8 mV * internal divider * 100, per input (as Linux's scale_in)
  ArrayView<const char* const> voltage_labels;
  ArrayView<const NuvotonTemperatureChannel> temperatures;
  ArrayView<const char* const> source_labels;   // by source number; "" for an unused source
  NuvotonFanFormat fan_format;
  ArrayView<const uint16_t> fan_regs;
  uint8_t pwm_count;
};

// The layout for a chip, or nullptr for one this driver does not handle (the NCT668x: NuvotonNct668x).
const NuvotonLayout* nuvoton_layout(SuperIoChip chip);

class NuvotonNct67xx final : public IHardwareMonitor {
 public:
  static constexpr uint8_t kRegBank = 0x4E, kRegVendor = 0x4F;
  static constexpr uint32_t kMaxTemperatures = 3, kMaxPwm = 7;

  NuvotonNct67xx(IPortIo& io, const SuperIoChipFound& found) : ports_(io, found.sensor_base), found_(found) {}

  const char* name() const override { return found_.info ? found_.info->name : "Nuvoton"; }
  // Checks the vendor id, picks the temperature sources and remembers each fan output's mode so set_pwm_mode(Automatic) can return to it.
  bool initialize() override;
  void shutdown() override {}
  void describe(IPropertySink& sink) const override;

  uint32_t channel_count(MonitorChannel kind) const override;
  const char* label(MonitorChannel kind, uint32_t channel) const override;
  SensorRole role(MonitorChannel kind, uint32_t channel) const override;
  [[nodiscard]] Optional<int32_t> temperature_millicelsius(uint32_t channel) override;
  [[nodiscard]] Optional<uint32_t> fan_rpm(uint32_t channel) override;
  [[nodiscard]] Optional<int32_t> voltage_millivolts(uint32_t channel) override;
  [[nodiscard]] Optional<PwmState> pwm(uint32_t channel) override;
  [[nodiscard]] bool set_pwm_duty(uint32_t channel, uint8_t duty) override;
  [[nodiscard]] bool set_pwm_mode(uint32_t channel, PwmMode mode) override;

  // Register access (bank selected on every access, so the firmware switching banks behind the driver's back does no harm).
  uint8_t read8(uint16_t reg);
  uint16_t read16(uint16_t reg);
  void write8(uint16_t reg, uint8_t value);

 private:
  struct Temperature {
    const NuvotonTemperatureChannel* channel = nullptr;
    const char* label = nullptr;
    SensorRole role = SensorRole::Other;
  };

  IndexedPorts ports_;
  SuperIoChipFound found_;
  const NuvotonLayout* layout_ = nullptr;
  Temperature temperatures_[kMaxTemperatures];
  uint32_t temperature_count_ = 0;
  uint8_t firmware_mode_[kMaxPwm] = {};   // bits 7:4 of the mode register at initialize()
};

}  // namespace hardware
