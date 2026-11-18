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

// Finding the Super I/O chip of a desktop board (Linux nct6775, it87 and f71882fg drivers). The chip answers at an index/data port pair, 0x2E/0x2F
// or 0x4E/0x4F, once a vendor's key has been written to the index port:
//   Nuvoton/Winbond and Fintek: 0x87 0x87, left with 0xAA (Nuvoton also writes 0x02 to register 0x02, as Linux does);
//   ITE: 0x87 0x01 0x55 0x55 (0x55 0xAA at 0x4E), left by writing 0x02 to register 0x02.
// In that mode register 0x20/0x21 is the chip id, 0x07 selects a logical device, and for the selected device 0x30 bit 0 is "active" and
// 0x60/0x61 the base of its I/O window. Fintek parts also give their vendor id (0x1934) at 0x23/0x24, which tells them apart from Nuvoton's.
#pragma once
#include <stdint.h>
#include "Hardware/HardwareMonitor/PortIo.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/Optional.hpp"

namespace hardware {

enum class SuperIoVendor : uint8_t { Nuvoton, Ite, Fintek };

enum class SuperIoChip : uint8_t {
  Nct6775, Nct6776, Nct6779, Nct6791, Nct6792, Nct6793, Nct6795, Nct6796, Nct6797, Nct6798, Nct6799,
  Nct6683, Nct6686, Nct6687,
  It8620, It8622, It8625, It8628, It8655, It8665, It8686, It8688, It8689, It87952, It8705, It8712, It8716, It8718, It8720, It8721, It8726, It8728,
  It8771, It8772, It8792,
  F71858, F71862, F71868, F71869, F71869A, F71882, F71889, F71889E, F71889A, F71808E, F81865,
};

enum class SuperIoSupport : uint8_t {
  Supported,       // a driver here reads its sensors
  IdentifiedOnly,  // known, but its sensors use an interface not implemented here
};

struct SuperIoChipInfo {
  uint16_t id;        // chip id (0x20 << 8 | 0x21) after masking
  uint16_t id_mask;   // the low bits are a revision on Nuvoton parts
  SuperIoVendor vendor;
  SuperIoChip chip;
  const char* name;
  uint8_t sensor_device;   // the logical device of the hardware monitor
  SuperIoSupport support;
  bool unlock_io_space;    // NCT6791 and later: register 0x28 bit 4 of the sensor device locks the I/O window and is cleared (as Linux does)
  const char* note;        // why it is IdentifiedOnly, or nullptr
};

// What the probe found at one configuration port.
struct SuperIoChipFound {
  uint16_t config_port = 0;
  uint16_t raw_id = 0;
  uint8_t revision = 0;   // register 0x22
  const SuperIoChipInfo* info = nullptr;
  uint16_t sensor_base = 0;   // 0 when the window is not set up
  bool sensor_active = false;
  const char* problem = nullptr;   // nullptr when the sensor window can be used
};

namespace superio {
constexpr uint16_t kConfigPorts[2] = {0x2E, 0x4E};
constexpr uint8_t kRegLogicalDevice = 0x07, kRegConfigControl = 0x02, kRegChipId = 0x20, kRegRevision = 0x22, kRegVendorId = 0x23,
                  kRegActivate = 0x30, kRegBase = 0x60, kRegNctIoLock = 0x28;
constexpr uint16_t kFintekVendorId = 0x1934;
constexpr uint8_t kNctIoLockBit = 0x10;
constexpr uint16_t kWindowAlign = 8;
}  // namespace superio

// The configuration index/data pair at one port while a key is in.
class SuperIoConfig {
 public:
  SuperIoConfig(IPortIo& io, uint16_t port) : io_(io), port_(port) {}
  // Writes the key that ITE parts (ite=true) or Nuvoton/Fintek ones (ite=false) listen for.
  void enter(bool ite);
  void exit(SuperIoVendor vendor);
  // Leaves configuration mode whichever chip might have taken the key (after a failed match).
  void exit_unmatched(bool ite);
  uint8_t read(uint8_t reg);
  uint16_t read16(uint8_t reg) { return static_cast<uint16_t>(read(reg) << 8 | read(static_cast<uint8_t>(reg + 1))); }
  void write(uint8_t reg, uint8_t value);
  void select(uint8_t device) { write(superio::kRegLogicalDevice, device); }

 private:
  IPortIo& io_;
  uint16_t port_;
};

ArrayView<const SuperIoChipInfo> superio_chip_table();
// The table entry for an id read with the given key; Fintek needs its vendor id (anything else is taken for Nuvoton). nullptr if unknown.
const SuperIoChipInfo* superio_lookup(bool ite_key, uint16_t chip_id, uint16_t vendor_id);
const char* superio_vendor_name(SuperIoVendor vendor);

// Looks for a chip at `config_port` (0x2E or 0x4E) with both keys. Empty when nothing answers (ids 0xFFFF or 0x0000 with either key). A chip
// whose id is not in the table comes back with `info` nullptr, and a known one with an unusable sensor window with `problem` set. Leaves the chip
// out of configuration mode.
[[nodiscard]] Optional<SuperIoChipFound> superio_probe(IPortIo& io, uint16_t config_port);

}  // namespace hardware
