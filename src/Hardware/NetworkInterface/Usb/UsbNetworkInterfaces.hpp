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

// Finds the USB network adapters this OS drives: the ASIX AX88179 family by its ids, and any device with a CDC-ECM or CDC-NCM function, in
// the configuration it is in or, when that one is vendor-specific only (the Realtek RTL8152/8153/8156 start in their vendor mode), in another
// of its configurations.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbHostController.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Takes over the network adapter on a root port and brings it up. Null when the device is no adapter this code drives, or it does not come up.
UniquePointer<INetworkInterface> open_usb_network_interface(IUsbHostController& host, uint32_t port, allocators::IAllocator& allocator);

}  // namespace hardware
