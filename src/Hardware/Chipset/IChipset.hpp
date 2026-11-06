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

// The chipset: the platform controller hub (Intel PCH, or the PCH part of an Intel SoC) or the fusion controller hub and Promontory chipset (AMD)
// that sits next to the CPU and carries the legacy bridge, SMBus, SPI flash, power management, SATA, USB, audio and the management engine.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Chipset/Smbus/SmbusHost.hpp"
#include "Hardware/IHardware.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

enum class ChipsetVendor : uint8_t { Unknown, Intel, Amd };

// What a PCI function of the chipset does, from its class code (and, where the class is shared, its slot).
enum class ChipsetFunction : uint8_t {
  Other,
  LpcBridge,         // LPC or eSPI bridge (the ISA bridge class): Super I/O, TPM, embedded controller, firmware hub
  Smbus,
  SpiFlash,          // the SPI controller in front of the firmware flash
  PowerManagement,   // Intel PMC
  HdAudio,
  Usb,
  Sata,
  ManagementEngine,  // Intel CSME/SPS (HECI)
  SecurityProcessor, // AMD PSP/ASP (the crypto co-processor function)
  Thermal,           // Intel PCH thermal sensor
  SerialIo,          // I2C, SPI and UART controllers (Intel LPSS)
  Ethernet,
  Wireless,          // Intel CNVi Wi-Fi
  PciePort,          // a root port of the chipset, or a port of its PCIe switch
};
const char* chipset_function_name(ChipsetFunction function);
// The kind of a PCI function that belongs to the chipset.
ChipsetFunction chipset_function_of(const PciDeviceInfo& device);

struct ChipsetFunctionInfo {
  ChipsetFunction function = ChipsetFunction::Other;
  PciAddress address{};
  uint16_t vendor_id = 0, device_id = 0;
};

// The functions of the chipset found on the bus, in discovery order (a fixed list: a chipset has a few dozen).
class ChipsetInventory {
 public:
  static constexpr uint32_t kCapacity = 64;
  void clear() { count_ = 0; }
  bool add(const PciDeviceInfo& device);
  uint32_t count() const { return count_; }
  bool at(uint32_t index, ChipsetFunctionInfo* out) const;
  // The first function of this kind; false if there is none.
  bool find(ChipsetFunction function, ChipsetFunctionInfo* out) const;
  uint32_t count_of(ChipsetFunction function) const;
  // "SMBus, LPC/eSPI, SPI flash, 2 USB, ..." in a fixed order, kinds the chipset has only.
  void summarize(FixedString<256>& out) const;

 private:
  ChipsetFunctionInfo functions_[kCapacity] = {};
  uint32_t count_ = 0;
};

// The CPU the chipset is paired with, as CPUID tells it.
struct ChipsetCpu {
  ChipsetVendor vendor = ChipsetVendor::Unknown;
  uint32_t family = 0, model = 0, stepping = 0;
};

// Where the chipset's SMBus host is: its I/O base and arbitration flavor. `usable` is false when the chipset has none or the firmware left it
// disabled (`reason` says which).
struct ChipsetSmbus {
  bool usable = false;
  uint16_t io_base = 0;
  SmbusHostFlavor flavor = SmbusHostFlavor::IntelI801;
  PciAddress address{};
  const char* reason = "no SMBus function";
};

class IChipset : public IHardware {
 public:
  virtual ChipsetVendor vendor() const = 0;
  // "Cannon Point-H" / "Promontory 21"; "unknown" when the catalogue does not know the part.
  virtual const char* codename() const = 0;
  // The chipsets or processors it ships as: "300 series (Z390, H370, B360, H310, Q370), C240".
  virtual const char* products() const = 0;
  virtual const ChipsetInventory& inventory() const = 0;
  virtual ChipsetSmbus smbus() const = 0;
  // One line for the boot log.
  virtual void summarize(FixedString<256>& out) const = 0;

 protected:
  constexpr IChipset() = default;
  ~IChipset() = default;
};

}  // namespace hardware
