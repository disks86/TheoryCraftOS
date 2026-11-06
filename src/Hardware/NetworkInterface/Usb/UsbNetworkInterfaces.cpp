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

#include "Hardware/NetworkInterface/Usb/UsbNetworkInterfaces.hpp"
#include "Hardware/NetworkInterface/Usb/Ax88179NetworkInterface.hpp"
#include "Hardware/NetworkInterface/Usb/CdcEthernetInterface.hpp"
#include "Hardware/NetworkInterface/Usb/UsbNetworkCommon.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
constexpr uint32_t kConfigurationBuffer = 4096;
constexpr uint8_t kMaxConfigurations = 8;

// Builds a T, initializes it, and hands it out only if that works.
template <class T, class Info>
UniquePointer<INetworkInterface> try_open(IUsbHostController& host, uint32_t port, const Info& info, allocators::IAllocator& allocator) {
  UniquePointer<T> nic = UniquePointer<T>::make(allocator, host, port, info, allocator);
  if (!nic) return UniquePointer<INetworkInterface>();
  if (!nic->initialize()) {
    nic->shutdown();
    return UniquePointer<INetworkInterface>();
  }
  return UniquePointer<INetworkInterface>(util::move(nic));
}

UniquePointer<INetworkInterface> open_with(IUsbHostController& host, uint32_t port, const UsbDeviceDescriptor& device, ByteView buffer,
                                           allocators::IAllocator& allocator) {
  uint16_t length = 0;
  if (!host.configuration_descriptor(port, buffer, &length)) return UniquePointer<INetworkInterface>();
  const ConstByteView current = ConstByteView(buffer).at_most(length);

  if (Ax88179NetworkInterface::supports(device.vendor_id, device.product_id)) {
    UsbVendorNetworkEndpoints endpoints;
    if (!find_vendor_network_endpoints(current, &endpoints)) return UniquePointer<INetworkInterface>();
    return try_open<Ax88179NetworkInterface>(host, port, endpoints, allocator);
  }

  UsbCdcEthernetFunction function;
  if (find_cdc_ethernet_function(current, &function)) return try_open<CdcEthernetInterface>(host, port, function, allocator);

  // Only a device nothing else could use is moved to another configuration.
  if (device.configuration_count < 2 || !usb_configuration_without_driver(current)) return UniquePointer<INetworkInterface>();
  const uint8_t count = device.configuration_count < kMaxConfigurations ? device.configuration_count : kMaxConfigurations;
  for (uint8_t i = 0; i < count; ++i) {
    ConstByteView other;
    if (read_usb_configuration(host, port, i, buffer, &other) && find_cdc_ethernet_function(other, &function))
      return try_open<CdcEthernetInterface>(host, port, function, allocator);
  }
  return UniquePointer<INetworkInterface>();
}
}  // namespace

UniquePointer<INetworkInterface> open_usb_network_interface(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator) {
  UsbDeviceDescriptor device;
  if (!host.device_descriptor(port, &device)) return UniquePointer<INetworkInterface>();
  ByteView buffer = allocate_view<uint8_t>(allocator, kConfigurationBuffer);
  if (buffer.empty()) return UniquePointer<INetworkInterface>();
  UniquePointer<INetworkInterface> nic = open_with(host, port, device, buffer, allocator);
  release_view(allocator, buffer);
  return nic;
}

}  // namespace hardware
