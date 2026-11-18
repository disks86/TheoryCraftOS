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

#include "Hardware/WirelessInterface/WifiAdapterProbe.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {

uint32_t find_wifi_adapters(const IPciBus* pci, const IUsbBus* usb, ArrayView<DetectedWifiAdapter> out) {
  uint32_t found = 0;
  const auto add = [&](const DetectedWifiAdapter& adapter) {
    if (found < out.size()) out[found] = adapter;
    ++found;
  };
  if (pci) {
    const uint32_t count = pci->device_count();
    for (uint32_t i = 0; i < count; ++i) {
      PciDeviceInfo device{};
      if (!pci->pci_device(i, &device)) continue;
      const WifiAdapterModel* model = wifi_find_pci_adapter(device.vendor_id, device.device_id);
      if (!model) continue;
      DetectedWifiAdapter adapter;
      adapter.model = model;
      adapter.pci = device;
      add(adapter);
    }
  }
  if (usb) {
    for (uint32_t c = 0; c < usb->controller_count(); ++c) {
      for (uint32_t port = 0; port < usb->port_count(c); ++port) {
        UsbDeviceDescriptor descriptor{};
        if (!usb->device_descriptor(c, port, &descriptor)) continue;
        const WifiAdapterModel* model = wifi_find_usb_adapter(descriptor.vendor_id, descriptor.product_id);
        if (!model) continue;
        DetectedWifiAdapter adapter;
        adapter.model = model;
        adapter.usb_controller = c;
        adapter.usb_port = port;
        add(adapter);
      }
    }
  }
  return found;
}

WifiRegisterWindow wifi_pci_register_window(WifiDriverFamily family) {
  switch (family) {
    case WifiDriverFamily::Rtw88:
    case WifiDriverFamily::Rtw89:
    case WifiDriverFamily::Rtl8192ce:
      return {2, 0x10000};
    case WifiDriverFamily::Mt7921:
    case WifiDriverFamily::Mt7925:
    case WifiDriverFamily::Mt7615:
    case WifiDriverFamily::Ath10k:
    case WifiDriverFamily::Ath11k:
    case WifiDriverFamily::Ath12k:
      return {0, 0x100000};
    case WifiDriverFamily::Brcmfmac:
      return {0, 0x8000};      // the register window (BAR 0); the chip's RAM is reached through it too
    case WifiDriverFamily::Ath9k:
      return {0, 0x10000};
    default:
      return {0, 0x2000};      // Intel's CSR space and the rest
  }
}

void WifiChipSlot::stop() {
  if (chip_) chip_->stop();
  chip_ = UniquePointer<IWifiChip>();
  registers_ = UniquePointer<MmioNicRegisters>();
}

WifiChipResult WifiChipSlot::start(const DetectedWifiAdapter& adapter, const Inputs& inputs, IWifiFirmwareSource& firmware) {
  stop();
  if (!adapter.model) return WifiChipResult::NoRegisters;
  WifiChipBus bus;
  switch (adapter.model->bus) {
    case WifiBus::Pci: {
      if (!inputs.pci || !inputs.mapper) return WifiChipResult::NoRegisters;
      const WifiRegisterWindow window = wifi_pci_register_window(adapter.model->family);
      const uint64_t base = inputs.pci->memory_bar(adapter.pci.address, window.bar);
      MmioRegion region;
      if (!base || !inputs.mapper->map_registers(base, window.bytes, &region)) return WifiChipResult::NoRegisters;
      registers_ = UniquePointer<MmioNicRegisters>::make(allocator_, region);
      if (!registers_) return WifiChipResult::NoRegisters;
      bus.registers = registers_.get();
      bus.pci = inputs.pci;
      bus.pci_device = adapter.pci;
      break;
    }
    case WifiBus::Usb:
      if (!inputs.usb) return WifiChipResult::NoRegisters;
      bus.usb = inputs.usb->host_controller(adapter.usb_controller);
      bus.usb_port = adapter.usb_port;
      if (!bus.usb) return WifiChipResult::NoRegisters;
      break;
    case WifiBus::Sdio:
      return WifiChipResult::NoRegisters;     // (no SDIO host driver yet)
  }
  // (making a chip touches nothing; a family without a chip driver leaves the device as it was)
  chip_ = make_wifi_chip(*adapter.model, bus, allocator_);
  if (!chip_) {
    registers_ = UniquePointer<MmioNicRegisters>();
    return WifiChipResult::Unsupported;
  }
  if (adapter.model->bus == WifiBus::Pci) {
    constexpr uint16_t kCommand = 0x04;
    // memory decoding and bus mastering; the status half of the register clears when written, so only the low half is kept
    const uint32_t command = inputs.pci->read32(adapter.pci.address, kCommand) & 0xFFFF;
    inputs.pci->write32(adapter.pci.address, kCommand, command | (1u << 1) | (1u << 2));
  }
  return chip_->start(firmware);
}

}  // namespace hardware
