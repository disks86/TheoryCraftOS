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
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWirelessInterface.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

// A Wi-Fi radio in software: the "air" is a list of access points added with add_access_point(), and the data path is another interface (the
// uplink - for a virtual machine, the NIC the hypervisor presents, which already carries the guest's traffic to the real network). Scanning lists
// the access points, connecting checks the password against the access point's, and frames pass to and from the uplink only while joined - so the
// network stack above sees a link that comes and goes with the connection, as it would with a chipset. The uplink must outlive this object and is
// not owned (initialize() it elsewhere).
class SoftwareWirelessInterface final : public IWirelessInterface {
 public:
  SoftwareWirelessInterface(INetworkInterface& uplink, allocators::IAllocator& allocator) : uplink_(uplink), access_points_(allocator), results_(allocator) {}

  // An access point in range. `passphrase` is what it expects (ignored when it is Open). False if the name is empty or too long, or memory ran out.
  bool add_access_point(StringView ssid, const MacAddress& bssid, WifiSecurity security, uint8_t channel, uint8_t signal_percent, StringView passphrase = StringView());
  // The access point with this name goes away (a scan will no longer find it, and a connection to it drops). False if there is none.
  bool remove_access_point(StringView ssid);
  // Changes how strong an access point's signal is, as the next scan sees it.
  bool set_signal(StringView ssid, uint8_t signal_percent);

  const char* name() const override { return "Virtual Wi-Fi adapter"; }
  bool initialize() override { return true; }
  void shutdown() override { disconnect(); }

  MacAddress mac_address() const override { return uplink_.mac_address(); }
  bool link_up() const override;
  uint32_t link_speed_mbps() const override;
  LinkDuplex link_duplex() const override { return LinkDuplex::Unknown; }
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override { return connected() && uplink_.accept_all_multicast(enable); }
  NetworkStatistics statistics() const override;

  bool start_scan() override;
  uint32_t scan_results(ArrayView<WifiNetwork> out) const override;
  WifiResult connect(StringView ssid, StringView passphrase) override;
  void disconnect() override;
  WifiStatus status() const override;

 private:
  struct AccessPoint {
    WifiNetwork network;
    char passphrase[kMaxPassphraseLength + 1] = {};
  };

  bool connected() const;

  INetworkInterface& uplink_;
  mutable kernel::Mutex mutex_;   // the stack's thread moves frames while the desktop scans and connects
  List<AccessPoint> access_points_;
  List<WifiNetwork> results_;     // the latest scan, strongest first
  WifiStatus status_;
  NetworkStatistics own_;         // frames refused while not joined
};

}  // namespace hardware
