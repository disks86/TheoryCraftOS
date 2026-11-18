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

// Finds the Wi-Fi adapters of the catalogue on the machine's buses: PCI functions by vendor and device ID, USB devices on the root ports by idVendor and idProduct.
// Only IDs are read (configuration space, cached descriptors); nothing is written to a device, so it is safe to run on every boot to list what is there.
// WifiChipSlot then starts the chip driver of one adapter (maps its register BAR, or hands over the USB port).
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

struct DetectedWifiAdapter {
  const WifiAdapterModel* model = nullptr;
  PciDeviceInfo pci{};                  // (bus Pci)
  uint32_t usb_controller = 0, usb_port = 0;   // (bus Usb)
};

// Fills `out` with the adapters found (PCI first, then USB, in bus order) and returns how many there are - which may be more than fit. Either bus may be null.
uint32_t find_wifi_adapters(const IPciBus* pci, const IUsbBus* usb, ArrayView<DetectedWifiAdapter> out);

// Which memory BAR holds a PCIe family's registers, and how much of it the driver maps (pci.c of each Linux driver: rtw88/rtw89 BAR 2, the others BAR 0).
struct WifiRegisterWindow {
  uint8_t bar;
  uint32_t bytes;
};
WifiRegisterWindow wifi_pci_register_window(WifiDriverFamily family);

// One adapter's chip driver and what it holds on to (the register window). start() enables memory decoding and bus mastering on a PCI function and maps its
// window, or passes a USB adapter's controller and port; then makes the chip (make_wifi_chip) and starts it.
class WifiChipSlot {
 public:
  explicit WifiChipSlot(allocators::IAllocator& allocator) : allocator_(allocator) {}
  WifiChipSlot(const WifiChipSlot&) = delete;
  WifiChipSlot& operator=(const WifiChipSlot&) = delete;
  ~WifiChipSlot() { stop(); }

  struct Inputs {
    IPciBus* pci = nullptr;
    IRegisterMapper* mapper = nullptr;
    IUsbBus* usb = nullptr;
  };
  // NoRegisters if the bus cannot be reached, Unsupported with chip() null if the family has no chip driver; otherwise the chip's start() result.
  [[nodiscard]] WifiChipResult start(const DetectedWifiAdapter& adapter, const Inputs& inputs, IWifiFirmwareSource& firmware);
  void stop();
  IWifiChip* chip() { return chip_.get(); }
  const IWifiChip* chip() const { return chip_.get(); }

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<MmioNicRegisters> registers_;
  UniquePointer<IWifiChip> chip_;
};

}  // namespace hardware
