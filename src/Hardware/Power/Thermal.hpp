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

// Keeping the machine inside its temperature limits. Sources report a temperature and the limits the hardware or the firmware gives them: the processor's digital sensor, and the
// thermal zones of the ACPI tables (_TMP in tenths of a Kelvin, _PSV the passive cooling point, _HOT, _CRT the critical point). The manager is the passive cooling of ACPI 6.4 section
// 11.1.5 reduced to its core: above a source's passive point it lowers a ceiling on the processor's speed one step per second (and raises it again, with a margin, when the source
// cools down), and at the critical point it asks for the system to be turned off before the hardware does it abruptly. Active cooling (fans) is the firmware's, which runs them from
// the embedded controller; this does not touch them.
#pragma once
#include <stdint.h>
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"

namespace hardware {

struct ThermalLimits {
  int32_t passive_mc = -1;           // millidegrees Celsius at which to start slowing down; -1 none
  int32_t critical_mc = -1;          // at which to shut down; -1 none
};

class IThermalSource {
 public:
  virtual const char* name() const = 0;
  virtual bool read(int32_t* millicelsius) = 0;
  virtual ThermalLimits limits() = 0;

 protected:
  constexpr IThermalSource() = default;
  ~IThermalSource() = default;
};

// The processor's sensor (Intel digital thermal sensor, AMD Tctl) with limits below the processor's own throttling point (TjMax 100 on the mobile parts).
class CpuThermalSource final : public IThermalSource {
 public:
  explicit CpuThermalSource(const IPciBus* pci, int32_t passive_mc = 88000, int32_t critical_mc = 98000) : pci_(pci), limits_{passive_mc, critical_mc} {}
  const char* name() const override { return "processor"; }
  bool read(int32_t* millicelsius) override;
  ThermalLimits limits() override { return limits_; }

 private:
  const IPciBus* pci_;
  ThermalLimits limits_;
};

// A thermal zone of the namespace: _TMP is required; _PSV and _CRT (tenths of a Kelvin) give the limits when the zone has them.
class AcpiThermalZone final : public IThermalSource {
 public:
  AcpiThermalZone(acpi::AmlInterpreter& vm, acpi::Node* zone) : vm_(vm), zone_(zone) {}
  static uint32_t find_zones(acpi::AmlInterpreter& vm, acpi::Node** out, uint32_t capacity);
  const char* name() const override { return "ACPI thermal zone"; }
  bool read(int32_t* millicelsius) override;
  ThermalLimits limits() override;

 private:
  bool temperature_of(const char* method, int32_t* millicelsius);
  acpi::AmlInterpreter& vm_;
  acpi::Node* zone_;
};

class IThermalActuator {
 public:
  virtual bool set_ceiling_percent(uint32_t percent) = 0;

 protected:
  constexpr IThermalActuator() = default;
  ~IThermalActuator() = default;
};

class ThermalManager {
 public:
  static constexpr uint32_t kMaxSources = 8;
  static constexpr uint32_t kStepPercent = 10, kMinCeiling = 30, kMarginMc = 5000, kIntervalMs = 1000;
  explicit ThermalManager(IThermalActuator* actuator) : actuator_(actuator) {}

  bool add(IThermalSource* source);
  // Reads the sources (at most once per kIntervalMs) and moves the ceiling. Returns true if something changed.
  bool poll(uint64_t now_ms);
  uint32_t ceiling_percent() const { return ceiling_; }
  // A source reached its critical point: the caller shuts the system down.
  bool critical() const { return critical_; }
  // The hottest reading of the last poll (-1 if none could be read) and which source it was.
  int32_t hottest_millicelsius() const { return hottest_; }
  const char* hottest_source() const { return hottest_name_; }

 private:
  IThermalActuator* actuator_;
  IThermalSource* sources_[kMaxSources] = {};
  uint32_t count_ = 0, ceiling_ = 100;
  uint64_t last_ms_ = 0;
  bool polled_ = false, critical_ = false;
  int32_t hottest_ = -1;
  const char* hottest_name_ = "";
};

}  // namespace hardware
