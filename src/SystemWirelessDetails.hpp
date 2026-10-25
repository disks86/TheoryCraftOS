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

// The Wi-Fi properties window's view of the machine: the wireless adapters among the system's network interfaces, numbered as in the hardware tree.
// The interfaces must outlive it.
#pragma once
#include "Desktop/WirelessDetails.hpp"
#include "Hardware/NetworkInterface/SystemNetworkInterfaces.hpp"

class SystemWirelessDetails final : public desktop::IWirelessDetails {
 public:
  explicit SystemWirelessDetails(hardware::SystemNetworkInterfaces& adapters) : adapters_(adapters) {}

  bool is_wireless(uint32_t id) override;
  bool adapter(uint32_t id, desktop::WirelessAdapterInfo* out) override;
  bool scan(uint32_t id) override;
  uint32_t networks(uint32_t id, desktop::WirelessNetworkInfo* out, uint32_t capacity) override;
  desktop::WirelessResult connect(uint32_t id, StringView ssid, StringView passphrase) override;
  void disconnect(uint32_t id) override;

 private:
  hardware::IWirelessInterface* radio(uint32_t id) const;

  hardware::SystemNetworkInterfaces& adapters_;
};
