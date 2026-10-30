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


// The Intel Wireless-AC 9000 series card as a network interface the rest of the system can use: an IWirelessInterface (scan, connect, disconnect, status) whose Ethernet frames
// go through the firmware once joined. It owns the driver, the scanner, the station link and the association state machine. Everything is polled: each call (and the network
// stack's receive() on its service thread) first lets the card's queues run, so scans finish, joins progress and frames arrive without a thread of its own. A mutex serialises the
// desktop's calls with the stack's.
//
// One network at a time; scanning is refused while joined or joining (the firmware does not scan in the background here). Joinable: open networks and WPA2-Personal with CCMP.
#pragma once
#include <stdint.h>
#include "Crypto/Random.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/WirelessInterface/IWirelessInterface.hpp"
#include "Hardware/WirelessInterface/Ieee80211Association.hpp"
#include "Hardware/WirelessInterface/Intel/IwlDriver.hpp"
#include "Hardware/WirelessInterface/Intel/IwlLink.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

class IwlWirelessInterface final : public IWirelessInterface, private IWifiDataSink {
 public:
  struct Inputs {
    IPciBus* pci = nullptr;
    PciDeviceInfo device{};
    IRegisterMapper* mapper = nullptr;
    IIwlFirmwareSource* firmware = nullptr;
    allocators::IAllocator* allocator = nullptr;
    const IClock* clock = nullptr;
    crypto::IRandom* random = nullptr;
  };
  static constexpr uint32_t kReceiveSlots = 32, kFrameBytes = 1536;

  explicit IwlWirelessInterface(const Inputs& inputs);
  ~IwlWirelessInterface() { shutdown(); }

  // Starts the card from its PCI function (binds it, loads the firmware from `inputs.firmware`, runs the handshake, prepares scanning). False with failure() saying why;
  // the interface then reports no link and refuses scans.
  bool initialize() override;
  // For tests and callers that already have the registers: the same, from the register window.
  bool start(IIwlRegisters& registers, bool integrated);
  void shutdown() override;
  const char* name() const override { return "Intel Wireless-AC (Wi-Fi)"; }
  bool link_comes_and_goes() const override { return true; }
  const char* failure() const { return failure_; }
  const IwlDriver& driver() const { return driver_; }

  // INetworkInterface
  MacAddress mac_address() const override;
  bool link_up() const override;
  uint32_t link_speed_mbps() const override { return link_up() ? 24 : 0; }
  LinkDuplex link_duplex() const override { return LinkDuplex::Unknown; }
  bool transmit(ConstByteView frame) override;
  bool receive(ByteView buffer, uint32_t* length) override;
  bool accept_all_multicast(bool enable) override { (void)enable; return false; }
  NetworkStatistics statistics() const override;

  // IWirelessInterface
  bool start_scan() override;
  uint32_t scan_results(ArrayView<WifiNetwork> out) const override;
  WifiResult connect(StringView ssid, StringView passphrase) override;
  void disconnect() override;
  WifiStatus status() const override;

 private:
  struct Slot {
    uint16_t length = 0;
    uint8_t data[kFrameBytes];
  };
  void on_ethernet_frame(ConstByteView frame) override;
  // Lets the card run (locked by the caller): the scanner polls the firmware's messages, scans end, joins progress.
  void pump() const;
  void finish_connection(WifiResult result) const;
  bool attach();                                       // after the driver is running: scanner, link, state machine, receive ring
  void leave_locked();

  Inputs in_;
  mutable kernel::Mutex mutex_;
  IwlDriver driver_;
  mutable UniquePointer<IwlStationLink> link_;
  mutable UniquePointer<Ieee80211Association> association_;
  mutable List<WifiNetwork> results_;
  mutable WifiStatus status_;
  mutable bool scan_pending_ = false;
  mutable bool started_ = false, connected_ = false;
  mutable uint32_t scan_generation_ = 0;
  ArrayView<Slot> ring_;
  mutable uint32_t ring_head_ = 0, ring_tail_ = 0, ring_count_ = 0;
  mutable NetworkStatistics own_;
  const char* failure_ = "";
};

}  // namespace hardware
