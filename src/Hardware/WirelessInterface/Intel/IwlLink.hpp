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


// The Intel card as the radio of a station (IWifiLink): everything between "a network was chosen" and "frames flow". prepare() gives the firmware the channel (a PHY context), the
// interface's address of the access point (the MAC context), the binding of the two, and the access point as a station with a transmit queue; transmit() sends 802.11 frames
// as TX commands (management and EAPOL at the lowest basic rate, protected data at a fixed OFDM rate); the pairwise and group keys go to the firmware's key table and the pairwise key
// is also given to the hardware per frame; received frames are handed to the association state machine and, as Ethernet frames, to the data sink. The hardware does the encryption
// and the decryption (CCMP). The firmware here has no rate control of its own (no TLC offload): the host picks the rates (WifiRateScaler) and the firmware retries along the table.
#pragma once
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/WirelessInterface/Ieee80211Association.hpp"
#include "Hardware/WirelessInterface/RateScaler.hpp"
#include "Hardware/WirelessInterface/Intel/IwlMvm.hpp"
#include "Hardware/WirelessInterface/Intel/IwlScan.hpp"
#include "Hardware/WirelessInterface/Intel/IwlTransport.hpp"

namespace hardware {

class IWifiDataSink {
 public:
  // An Ethernet frame (destination, source, ethertype, payload) received from the access point.
  virtual void on_ethernet_frame(ConstByteView frame) = 0;

 protected:
  constexpr IWifiDataSink() = default;
  ~IWifiDataSink() = default;
};

class IwlStationLink final : public IWifiLink, public IIwlNotificationHandler {
 public:
  static constexpr uint8_t kStationId = 1;                 // the access point (0 is the auxiliary station)
  static constexpr uint32_t kDataQueue = 4, kDataFifo = 3;  // the reserved queue of the BSS client, on the voice FIFO that management frames use

  IwlStationLink(IwlTransport& transport, allocators::IAllocator& allocator, const IClock& clock)
      : transport_(transport), clock_(clock), queue_(transport.device(), allocator, kDataQueue, kDataFifo) {}
  ~IwlStationLink() { release(); }
  IwlStationLink(const IwlStationLink&) = delete;
  IwlStationLink& operator=(const IwlStationLink&) = delete;

  void set_association(Ieee80211Association* association) { association_ = association; }
  void set_data_sink(IWifiDataSink* sink) { sink_ = sink; }

  // The firmware's contexts and queue for `network`. `scanner` must be ready (its station interface exists). False with a message.
  bool prepare(const IwlFirmware& firmware, const IwlNvmData& nvm, const WifiNetwork& network, const char** error);
  // Takes the access point's station and the binding out of the firmware and frees the queue (a disconnect); release() only drops our side.
  void teardown();
  void release();
  bool prepared() const { return prepared_; }

  // Sends an Ethernet frame to the access point as protected data. False until the keys are in or if the queue is full.
  bool send_ethernet(ConstByteView frame);
  uint32_t frames_sent() const { return frames_sent_; }
  uint32_t frames_received() const { return frames_received_; }
  uint32_t tx_failures() const { return tx_failures_; }
  // Rate control: data frames go out at the rates of the station's link quality table, which the scaler keeps up to date from the TX responses (it is sent again from service(), never from
  // inside a notification). Without the firmware's LINK_QUALITY_CMD, or after the firmware refuses the table, the fixed rate is used.
  bool rate_control() const { return rate_control_; }
  uint32_t data_rate_kbps() const { return rate_control_ ? scaler_.current_kbps() : 24000; }
  uint32_t rate_changes() const { return rate_changes_; }
  // Sends the link quality table if the scaler changed it. Called by whoever drives the card, outside of notifications.
  void service();

  // IWifiLink
  bool transmit(ConstByteView frame) override;
  bool install_pairwise_key(const uint8_t tk[wpa2::kTkBytes]) override;
  bool install_group_key(const wpa2::GroupKey& key) override;
  void port_opened(uint16_t association_id) override;
  uint64_t now_ms() override { return clock_.now_ms(); }

  // IIwlNotificationHandler
  void on_notification(const IwlRxPacket& packet) override;

 private:
  bool send_command(const IwlHostCommand& command, ByteView response, int32_t* length, const char** error);
  bool send_station(const IwlStationSpec& spec, const char** error);
  bool send_mac_context(bool associated, uint16_t association_id, const char** error);
  bool enqueue(ConstByteView header, ConstByteView body, uint16_t frame_bytes, bool protect, bool data, bool association_request);

  IwlTransport& transport_;
  const IClock& clock_;
  IwlCommandQueue queue_;
  Ieee80211Association* association_ = nullptr;
  IWifiDataSink* sink_ = nullptr;
  const IwlFirmware* firmware_ = nullptr;
  WifiNetwork network_;
  uint8_t address_[6] = {};
  uint32_t tx_antennas_ = 1, rx_antennas_ = 1;
  bool send_link_quality(const char** error);
  WifiRateScaler scaler_;
  bool rate_control_ = false, lq_dirty_ = false;
  uint32_t lq_failures_ = 0, rate_changes_ = 0;
  bool time_event_active_ = false;
  uint32_t time_event_id_ = 0;
  bool prepared_ = false, have_ptk_ = false, phy_added_ = false;
  uint8_t tk_[16] = {};
  uint64_t pn_ = 0;
  uint32_t frames_sent_ = 0, frames_received_ = 0, tx_failures_ = 0;
};

}  // namespace hardware
