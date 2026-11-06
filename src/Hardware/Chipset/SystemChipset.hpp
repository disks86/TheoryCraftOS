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

// The machine's chipset: finds the Intel or AMD one on the PCI bus, and owns its SMBus host so that every user (the memory-module scan, sensor and
// clock drivers) goes through one lock. Construct once at boot, after the PCI bus is scanned.
#pragma once
#include <stdint.h>
#include "Hardware/Chipset/AMD/AmdFchChipset.hpp"
#include "Hardware/Chipset/AMD/AmdNodes.hpp"
#include "Hardware/Chipset/Intel/IntelPchChipset.hpp"
#include "Hardware/Chipset/Intel/IntelPchThermal.hpp"
#include "Hardware/Chipset/Intel/IntelTcoWatchdog.hpp"
#include "Hardware/Chipset/Smbus/SmbusHost.hpp"
#include "Hardware/Chipset/Smbus/SmbusIoPorts.hpp"
#include "Hardware/Chipset/Smbus/Spd.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Synchronization/Synchronized.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

// The FCH power-management registers through the 0xCD6 (index) / 0xCD7 (data) ports.
class AmdPmIndexPorts final : public IAmdPmRegisters {
 public:
  uint8_t read8(uint8_t offset) override;
};

// The FCH power-management registers in the ACPI MMIO block (0xFED80000 + 0x300).
class AmdPmMmio final : public IAmdPmRegisters {
 public:
  explicit AmdPmMmio(const MmioRegion& region) : region_(region) {}
  static constexpr uint64_t kPhysical = 0xFED80300, kLength = 0x100;
  uint8_t read8(uint8_t offset) override { return region_.read8(offset); }

 private:
  MmioRegion region_;
};

// The TCO watchdog's registers as x86 I/O ports from TCOBASE.
class TcoIoPorts final : public ITcoPorts {
 public:
  explicit constexpr TcoIoPorts(uint16_t base) : base_(base) {}
  uint16_t read16(uint8_t reg) override;
  void write16(uint8_t reg, uint16_t value) override;

 private:
  uint16_t base_;
};

// What the TCO watchdog looked like at boot, and what the probe did about it.
struct WatchdogReport {
  bool present = false;
  IntelTcoKind kind = IntelTcoKind::None;
  bool caused_last_reset = false;   // the previous boot ended with this watchdog running out
  bool was_running = false;         // the firmware left it counting
  bool stopped = false;             // ...and the probe halted it (nothing here pings it yet)
  uint32_t timeout_seconds = 0;
};

// One memory module found on the SMBus.
struct MemoryModuleSlot {
  uint8_t slot = 0;   // SMBus address 0x50 + slot
  SpdModule module;
};

class SystemChipset {
 public:
  explicit SystemChipset(const ICentralProcessingUnit& cpu) : smbus_(cpu), amd_nodes_(cpu), watchdog_(cpu) {}
  ~SystemChipset();
  SystemChipset(const SystemChipset&) = delete;
  SystemChipset& operator=(const SystemChipset&) = delete;

  // Identifies the chipset and attaches the SMBus host if the firmware left it enabled. `mapper` may be null (no MMIO: the newest AMD FCHs then fall
  // back to the index ports).
  void probe(const ChipsetCpu& cpu, const IPciBus& pci, IRegisterMapper* mapper);

  // nullptr when neither an Intel nor an AMD chipset was found (a virtual machine without one, another vendor's platform).
  const IChipset* chipset() const { return active_; }
  const IntelPchChipset* intel() const { return active_ == &intel_ ? &intel_ : nullptr; }
  const AmdFchChipset* amd() const { return active_ == &amd_ ? &amd_ : nullptr; }
  bool smbus_attached() const { return ports_.has_value(); }

  // Runs `operation(ISmbusController&)` with the SMBus lock held and returns what it returns. Without a host every transaction says NoController.
  template <class Operation>
  decltype(auto) with_smbus(Operation&& operation) {
    return smbus_.with([&](SmbusHost& host) -> decltype(auto) { return operation(static_cast<ISmbusController&>(host)); });
  }

  // AMD Zen: the nodes, SMN and Data Fabric, with their lock held. Also installed as the SMN reader of cpu_temperature_millicelsius.
  template <class Operation>
  decltype(auto) with_amd_nodes(Operation&& operation) {
    return amd_nodes_.with([&](AmdNodes& nodes) -> decltype(auto) { return operation(static_cast<const AmdNodes&>(nodes)); });
  }
  uint32_t amd_node_count() { return with_amd_nodes([](const AmdNodes& n) { return n.node_count(); }); }

  // The PCH thermal sensor (Intel, Haswell..Comet Lake); nullptr when there is none to read.
  IntelPchThermal* pch_thermal() { return pch_thermal_.attached() ? &pch_thermal_ : nullptr; }

  // The TCO watchdog, with its lock held (attached() is false when there is none).
  template <class Operation>
  decltype(auto) with_watchdog(Operation&& operation) {
    return watchdog_.with([&](IntelTcoWatchdog& w) -> decltype(auto) { return operation(w); });
  }
  const WatchdogReport& watchdog_report() const { return watchdog_report_; }

  // Reads the SPD of every memory module on the SMBus (addresses 0x50..0x57) into `out`; returns how many answered and decoded. The lock is taken
  // per module, so other users are not held off for the whole scan. Why nothing was scanned goes to `reason` (null when the scan ran).
  uint32_t read_memory_modules(ArrayView<MemoryModuleSlot> out, const char** reason);

 private:
  IntelPchChipset intel_;
  AmdFchChipset amd_;
  const IChipset* active_ = nullptr;
  AmdPmIndexPorts pm_ports_;
  Optional<AmdPmMmio> pm_mmio_;
  Optional<SmbusIoPorts> ports_;
  Synchronized<SmbusHost> smbus_;
  Synchronized<AmdNodes> amd_nodes_;
  IntelPchThermal pch_thermal_;
  Optional<TcoIoPorts> tco_ports_;
  Synchronized<IntelTcoWatchdog> watchdog_;
  WatchdogReport watchdog_report_;
};

}  // namespace hardware
