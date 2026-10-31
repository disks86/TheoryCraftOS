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

// The charge of the machine's battery(ies) as the firmware reports it: a "control method battery" (PNP0C0A) is a device in the ACPI tables whose methods
// (_STA, _BST, _BIF or _BIX) read the embedded controller, so reading it means running that AML (see AmlInterpreter). A machine without a battery (a
// desktop, most virtual machines) has none of these devices, and then nothing at all is run.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Acpi/AcpiHost.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class BatteryState : uint8_t { Unknown, Charging, Discharging, Full, NotCharging };

struct BatteryStatus {
  bool present = false;
  int32_t percent = -1;                      // 0-100 of the last full charge, -1 unknown
  BatteryState state = BatteryState::Unknown;
};

class AcpiBattery {
 public:
  static constexpr uint32_t kMaxBatteries = 4;
  static constexpr uint32_t kMaxTables = 64;

  AcpiBattery(IAcpiHost& host, allocators::IAllocator& allocator) : host_(host), vm_(allocator, host) {}

  // Loads the definition blocks (the DSDT first, then the SSDTs) and looks for batteries; true if there is at least one. `tables` must stay valid.
  bool initialize(const ConstByteView* tables, uint32_t count);
  // The same, taking the tables from the firmware.
  bool initialize(const AcpiTables& acpi);

  uint32_t battery_count() const { return battery_count_; }
  // What loading the tables came to, for the boot log: objects in the namespace, tables that loaded whole and tables that stopped at a fault.
  uint32_t object_count() const { return vm_.node_count(); }
  uint32_t tables_loaded() const { return tables_loaded_; }
  uint32_t tables_faulty() const { return tables_faulty_; }
  const char* last_load_error() const { return load_error_; }
  // Reads the batteries now (a few reads of the embedded controller). Safe to call from several threads (one reads at a time). `present` is false when the machine has none (or none is inserted).
  BatteryStatus read();

 private:
  struct Battery {
    acpi::Node* device = nullptr;
    acpi::Node* status = nullptr;
    acpi::Node* state = nullptr;          // _BST
    acpi::Node* info = nullptr;           // _BIX or _BIF
    bool extended = false;                // _BIX (the capacities are one place further on)
    uint64_t design = 0, last_full = 0;
    uint32_t info_age = ~0u;
  };
  acpi::Node* own(acpi::Node* device, const char* name) const;
  void find_batteries(acpi::Node* node, uint32_t depth, acpi::Node** ec);
  void set_up_controller(acpi::Node* ec);
  void run_init(acpi::Node* device);

  kernel::Mutex read_mutex_;
  IAcpiHost& host_;
  acpi::AmlInterpreter vm_;
  Battery batteries_[kMaxBatteries];
  uint32_t battery_count_ = 0;
  uint32_t failures_ = 0;
  uint32_t tables_loaded_ = 0, tables_faulty_ = 0;
  const char* load_error_ = "";
};

}  // namespace hardware
