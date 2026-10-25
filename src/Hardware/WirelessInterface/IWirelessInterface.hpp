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

#pragma once
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/WirelessInterface/WirelessTypes.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace hardware {

// A Wi-Fi adapter: an INetworkInterface (frames in and out once it has joined a network - until then it has no link) that also finds networks and
// joins one. Polled like every interface; connect() may return before the join is over, and status() says how far it got. Implementations live in
// subfolders by vendor/transport (Software/ for the virtual radio); a chipset driver goes next to it.
class IWirelessInterface : public INetworkInterface {
 public:
  // Starts looking for networks. False if the radio is busy joining or scanning already.
  virtual bool start_scan() = 0;
  // Copies the networks found by the latest finished scan - strongest first, one per access point - into `out` (as many as fit); returns how many
  // were copied. The list stays as it was while a new scan runs.
  virtual uint32_t scan_results(ArrayView<WifiNetwork> out) const = 0;
  // Joins the network called `ssid`. `passphrase` is its password (ignored for an open network). Returns Ok once the join is under way or done;
  // otherwise why it did not start. A refusal that only the access point can give (a wrong password) may show up in status() later instead.
  virtual WifiResult connect(StringView ssid, StringView passphrase) = 0;
  // Leaves the network (and stops a join that is under way).
  virtual void disconnect() = 0;
  virtual WifiStatus status() const = 0;

  IWirelessInterface* wireless() override { return this; }

 protected:
  constexpr IWirelessInterface() = default;
  ~IWirelessInterface() = default;
};

}  // namespace hardware
