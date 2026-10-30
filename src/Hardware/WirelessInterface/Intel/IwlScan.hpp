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


// Getting an Intel Wireless-AC 9000 series card from "firmware running and initialised" to "can scan": the runtime commands of iwl_mvm_up and a regular scan (TX antennas, DQA
// mode, the auxiliary queue and station, the scan configuration, a station interface) and then SCAN_REQ_UMAC; beacons and probe responses come back as received frames
// that are read into the list of networks, and SCAN_COMPLETE_UMAC ends the scan. Only the firmware versions named in IwlMvm.hpp are spoken; others are refused with their version.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/Intel/IwlMvm.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"
#include "Hardware/WirelessInterface/Intel/IwlNvm.hpp"
#include "Hardware/WirelessInterface/WirelessTypes.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

class IwlScanner final : private IIwlNotificationHandler {
 public:
  static constexpr uint32_t kMaxNetworks = 64;
  static constexpr uint32_t kCommandTimeoutUs = 2000000;

  IwlScanner(IwlTransport& transport, allocators::IAllocator& allocator)
      : transport_(transport), allocator_(allocator), aux_queue_(transport.device(), allocator, iwl_tx::kAuxQueue, iwl_tx::kMcastFifo), networks_(allocator), rsn_records_(allocator) {}
  ~IwlScanner() { shut_down(); }
  IwlScanner(const IwlScanner&) = delete;
  IwlScanner& operator=(const IwlScanner&) = delete;

  // The runtime commands. `firmware` and `nvm` are those the transport was started with.
  bool bring_up(const IwlFirmware& firmware, const IwlNvmData& nvm, const char** error);
  void shut_down();
  bool ready() const { return ready_; }

  // Starts a scan of the channels the NVM allows. Active scans send probe requests where transmitting is allowed (not on radar channels) and listen elsewhere.
  bool start_scan(bool active, const char** error);
  // Handles what the firmware has sent; true while a scan is under way.
  bool poll();
  bool scanning() const { return scanning_; }
  bool last_scan_completed() const { return last_completed_; }
  uint32_t network_count() const { return static_cast<uint32_t>(networks_.count()); }
  // Copies the networks found, strongest first; returns how many fit.
  uint32_t results(ArrayView<WifiNetwork> out) const;
  uint32_t frames_seen() const { return frames_seen_; }
  // The RSN element of the access point's beacon (id 48 included), as heard in the latest scan; false if it had none.
  bool rsn_element(const MacAddress& bssid, ConstByteView* element) const;
  // Every notification is also passed on (the station link reads the frames and the transmit responses from the same stream).
  void set_forward(IIwlNotificationHandler* next) { forward_ = next; }
  uint8_t auxiliary_station_id() const { return 0; }

 private:
  void on_notification(const IwlRxPacket& packet) override;
  struct RsnRecord {
    MacAddress bssid{};
    uint8_t length = 0;
    uint8_t element[64] = {};
  };
  void add_network(const WifiNetwork& network, ConstByteView rsn);
  bool send(const IwlHostCommand& command, ByteView response, int32_t* length, const char** error);

  IwlTransport& transport_;
  allocators::IAllocator& allocator_;
  IwlCommandQueue aux_queue_;
  List<WifiNetwork> networks_;
  List<RsnRecord> rsn_records_;
  IIwlNotificationHandler* forward_ = nullptr;
  const IwlFirmware* firmware_ = nullptr;
  IwlNvmData nvm_;
  bool ready_ = false, scanning_ = false, last_completed_ = false;
  uint32_t next_uid_ = 1, scan_uid_ = 0, frames_seen_ = 0;
};

}  // namespace hardware
