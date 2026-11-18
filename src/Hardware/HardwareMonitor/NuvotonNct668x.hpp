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

// The hardware monitor of the Nuvoton NCT6683D, NCT6686D and NCT6687D (Linux nct6683). These parts run the sensors in an embedded controller
// whose register space is 16-bit (page << 8 | index), reached through four ports above the hardware monitor device's base:
//
//   base+4 page, base+5 index, base+6 data (base+7, the event port, is not used).
//
// An access writes 0xFF to the page port (the "release" value: Linux writes it before every access), then the page, then the index, and
// reads or writes the data port; afterwards it writes 0xFF again so that the firmware's own agent sees the space free. Reading the page port
// gives 0xFF while nobody holds the space (LibreHardwareMonitor waits for that before every access). Here each access polls the page port a
// bounded number of times and gives up - no reading, or a refused write - when it never reads 0xFF, so an EC that hangs cannot hang a core.
//
//   0x602/0x603  customer id (Intel, MSI, ASRock, AMD, Mitac, ...); Linux binds only to the ids it knows, and so does this driver.
//   0x1A0 + i    monitoring channel i's source (bits 6:0): 0 off, 0x01..0x5F temperatures, 0x60 and up voltages. The source names the
//                channel ("PECI 0.0", "AMD TSI Addr 98h", "VCC", "VIN3").
//   0x100 + 2i   channel i's reading: temperatures a word in half degrees (high byte signed degrees, bit 7 of the low byte the half),
//                voltages the high byte at 16 mV per count, doubled for VCC, VSB, AVSB and VBAT.
//   0x1C0 + i    fan input i's configuration; bit 7 set when the input is in use (16 inputs).
//   0x140 + 2i   fan i's speed, the RPM itself in 16 bits (0xFFFF means no reading).
//   0x1E0 + i    fan output i's configuration; bit 7 set when the output is in use (8 outputs).
//   0x160 + i    output i's duty in effect (0..255).
//   0xA28 + i    output i's duty to set, between a request (0x80) and a done (0x40) written to 0xA01.
//   0xA00        (MSI's firmware, as LibreHardwareMonitor drives it) bit i set: output i follows the written duty, clear: its curve.
//   0x604..0x609 firmware build date (year - 2000, month, day), serial, version high and low.
//
// Mainline Linux allows duty writes only on Mitac boards; LibreHardwareMonitor and the out-of-tree nct6687 driver write them on MSI boards
// with the 0xA00 mode bits. Every other customer's outputs are read-only here: their firmware's fan control protocol is not known.
// Per-chip and per-customer differences are in tables (Nct668xLayout, Nct668xCustomer), not in the code.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/IHardwareMonitor.hpp"
#include "Hardware/HardwareMonitor/SuperIo.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

struct Nct668xLayout {
  uint8_t monitor_channels;   // configuration registers 0x1A0..
  uint8_t fan_inputs;         // 0x1C0.. and the RPM registers 0x140..
  uint8_t fan_outputs;        // 0x1E0.., 0x160.., 0xA28..
};

enum class Nct668xPwmControl : uint8_t {
  ReadOnly,    // the duty is shown, never written
  Request,     // Linux's Mitac sequence: 0xA01 = 0x80, duty at 0xA28 + i, 0xA01 = 0x40; no mode register is known
  MsiModeBits, // the same, with bit i of 0xA00 choosing the written duty (set) or the firmware's curve (clear)
};

struct Nct668xCustomer {
  uint16_t id;   // register 0x602 (high byte) and 0x603
  const char* name;
  Nct668xPwmControl pwm;
  ArrayView<const char* const> fan_labels;   // by fan input / output number; empty: "FAN1", "FAN2", ...
};

// The layout for a chip, or nullptr for one this driver does not handle.
const Nct668xLayout* nct668x_layout(SuperIoChip chip);
// The board vendor behind a customer id, or nullptr for an id Linux's driver does not bind to.
const Nct668xCustomer* nct668x_customer(uint16_t id);
// The name of a monitoring source (register 0x1A0 + i, bits 6:0), or nullptr for "off" and reserved numbers.
const char* nct668x_source_label(uint8_t source);

class NuvotonNct668x final : public IHardwareMonitor {
 public:
  static constexpr uint16_t kPagePort = 4, kIndexPort = 5, kDataPort = 6;
  static constexpr uint8_t kRelease = 0xFF;
  // Page-port reads before an access gives up (an I/O read takes about a microsecond on the LPC bus: about a millisecond in all). The caller
  // holds a spin lock, so the wait cannot sleep.
  static constexpr uint32_t kBusyPolls = 1000;
  // Page-port reads between the fan-control request and the duty write (Linux sleeps 1..2 ms there; a spin lock is held, so it is counted in
  // bus reads instead).
  static constexpr uint32_t kSettleReads = 1000;
  static constexpr uint16_t kRegMonitor = 0x100, kRegFanRpm = 0x140, kRegPwm = 0x160, kRegMonitorConfig = 0x1A0, kRegFanInConfig = 0x1C0,
                            kRegFanOutConfig = 0x1E0, kRegCustomerId = 0x602, kRegBuildYear = 0x604, kRegVersion = 0x608,
                            kRegFanMode = 0xA00, kRegFanControl = 0xA01, kRegPwmWrite = 0xA28;
  static constexpr uint8_t kFanControlRequest = 0x80, kFanControlDone = 0x40, kConfigInUse = 0x80;
  static constexpr uint8_t kVoltageSourceStart = 0x60;
  static constexpr uint32_t kMaxMonitors = 32, kMaxFans = 16, kMaxPwm = 8;
  // Temperatures below this are taken for a broken sensor (no board part works there).
  static constexpr int32_t kMinMillicelsius = -60000;

  NuvotonNct668x(IPortIo& io, const SuperIoChipFound& found) : io_(io), found_(found) {}

  const char* name() const override { return found_.info ? found_.info->name : "Nuvoton"; }
  // Reads the customer id (an unknown one is refused, as Linux does), the channel configuration and the firmware's fan output modes.
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
  // Refused (false) unless the customer's fan control protocol is known (Nct668xPwmControl).
  [[nodiscard]] bool set_pwm_duty(uint32_t channel, uint8_t duty) override;
  [[nodiscard]] bool set_pwm_mode(uint32_t channel, PwmMode mode) override;

  const Nct668xCustomer* customer() const { return customer_; }
  uint16_t customer_id() const { return customer_id_; }
  bool pwm_writable() const { return customer_ && customer_->pwm != Nct668xPwmControl::ReadOnly; }

  // EC space access, each one complete (release, page, index, data, release). Empty / false when the page port never showed the space free.
  [[nodiscard]] Optional<uint8_t> read8(uint16_t reg);
  [[nodiscard]] Optional<uint16_t> read16(uint16_t reg);   // high byte at reg, low byte at reg + 1
  [[nodiscard]] bool write8(uint16_t reg, uint8_t value);

 private:
  struct Monitor {
    uint8_t index = 0;    // the hardware channel (0x100 + 2 * index)
    uint8_t source = 0;   // its configuration
  };

  uint16_t port(uint16_t offset) const { return static_cast<uint16_t>(found_.sensor_base + offset); }
  [[nodiscard]] bool wait_free();
  void select(uint16_t reg);
  const char* fan_label(uint8_t index) const;
  [[nodiscard]] bool write_duty(uint8_t output, uint8_t duty);
  [[nodiscard]] bool set_mode_bit(uint8_t output, bool manual);

  IPortIo& io_;
  SuperIoChipFound found_;
  const Nct668xLayout* layout_ = nullptr;
  const Nct668xCustomer* customer_ = nullptr;
  uint16_t customer_id_ = 0;
  uint8_t build_[3] = {};     // year - 2000, month, day
  uint8_t version_[2] = {};   // high, low
  Monitor temperatures_[kMaxMonitors];
  uint32_t temperature_count_ = 0;
  Monitor voltages_[kMaxMonitors];
  uint32_t voltage_count_ = 0;
  uint8_t fans_[kMaxFans] = {};   // hardware fan input of each channel
  uint32_t fan_count_ = 0;
  uint8_t pwms_[kMaxPwm] = {};    // hardware fan output of each channel
  uint32_t pwm_count_ = 0;
  uint8_t firmware_duty_[kMaxPwm] = {};   // by hardware output, at initialize()
  bool firmware_manual_[kMaxPwm] = {};    // MsiModeBits: the firmware had the output on a fixed duty
  bool manual_[kMaxPwm] = {};             // Request: this driver wrote a duty
};

}  // namespace hardware
