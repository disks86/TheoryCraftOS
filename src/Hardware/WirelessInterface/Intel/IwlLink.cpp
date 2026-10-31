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


#include "Hardware/WirelessInterface/Intel/IwlLink.hpp"

namespace hardware {

using namespace iwl_mvm;

namespace {
constexpr uint32_t kTimeoutUs = 2000000;
uint32_t status_of(const uint8_t* r, int32_t length) {
  return length >= 4 ? static_cast<uint32_t>(r[0]) | (static_cast<uint32_t>(r[1]) << 8) | (static_cast<uint32_t>(r[2]) << 16) | (static_cast<uint32_t>(r[3]) << 24) : 0xFFFFFFFFu;
}
}  // namespace

bool IwlStationLink::send_command(const IwlHostCommand& command, ByteView response, int32_t* length, const char** error) {
  const int32_t n = transport_.send_sync(command, response, kTimeoutUs, error);
  if (length) *length = n;
  return n >= 0;
}

bool IwlStationLink::send_station(const IwlStationSpec& spec, const char** error) {
  uint8_t out[kAddStaBytes], response[16];
  const uint32_t n = iwl_build_add_station(spec, ByteView(out, sizeof out));
  IwlHostCommand command;
  command.opcode = kAddStaCommand;
  command.data = ConstByteView(out, n);
  int32_t length = 0;
  if (!send_command(command, ByteView(response, sizeof response), &length, error)) return false;
  if ((status_of(response, length) & kAddStaStatusMask) != kAddStaSuccess) {
    *error = "the firmware refused the access point station";
    return false;
  }
  return true;
}

void IwlStationLink::fill_station(IwlStationSpec* spec) const {
  spec->station_id = kStationId;
  for (uint32_t i = 0; i < 6; ++i) spec->address[i] = network_.bssid.bytes[i];
  spec->queue_mask = 1u << kDataQueue;
  if (phy_.ht) {
    spec->ht = true;
    spec->width = phy_.width_mhz == 80 ? 2 : phy_.width_mhz == 40 ? 1 : 0;
    spec->streams = phy_.streams;
    spec->ampdu_exponent = phy_.ampdu_exponent;
    spec->ampdu_density = phy_.ampdu_density;
  }
}

uint32_t IwlStationLink::index_of_rate(uint32_t rate_n_flags) const {
  for (uint32_t i = 0; i < scaler_.rate_count(); ++i)
    if (((rate_n_flags ^ codes_[i]) & kRateIdentityMask) == 0) return i;
  return scaler_.rate_count();
}

bool IwlStationLink::send_link_quality(const char** error) {
  uint8_t indices[WifiRateScaler::kTableSize];
  scaler_.fill_table(indices);
  uint32_t antenna = tx_antennas_ & 7;
  antenna = antenna & 1 ? 1 : antenna & 2 ? 2 : 4;
  const uint32_t both = (tx_antennas_ & 3) == 3 ? kRateAntA | kRateAntB : antenna << 14;      // two streams use the first two chains
  uint32_t rates[kLinkQualityTableSize];
  uint32_t mimo_delim = 0;
  bool leading = true;
  for (uint32_t i = 0; i < kLinkQualityTableSize; ++i) {
    const bool two = phy_.ht && ladder_.rate[indices[i]].streams >= 2;
    rates[i] = codes_[indices[i]] | (two ? both : antenna << 14);
    leading = leading && two;
    if (leading) ++mimo_delim;
  }
  uint8_t out[kLinkQualityBytes], response[16];
  const uint32_t n = iwl_build_link_quality(kStationId, tx_antennas_, rates, ByteView(out, sizeof out), mimo_delim);
  if (n == 0) return false;
  IwlHostCommand command;
  command.opcode = kLinkQualityCommand;
  command.data = ConstByteView(out, n);
  return send_command(command, ByteView(response, sizeof response), nullptr, error);
}

void IwlStationLink::service() {
  if (!prepared_ || !rate_control_) return;
  scaler_.tick(clock_.now_ms());
  if (scaler_.take_changed()) {
    lq_dirty_ = true;
    ++rate_changes_;
  }
  if (!lq_dirty_) return;
  const char* error = nullptr;
  if (send_link_quality(&error)) {
    lq_dirty_ = false;
    lq_failures_ = 0;
  } else if (++lq_failures_ >= 3) {
    rate_control_ = false;                                       // the firmware will not take the table: back to the fixed rate
    lq_dirty_ = false;
  }
}

bool IwlStationLink::send_mac_context(bool associated, uint16_t association_id, const char** error) {
  IwlMacContext context;
  context.action = kCtxtActionModify;
  for (uint32_t i = 0; i < 6; ++i) {
    context.address[i] = address_[i];
    context.bssid[i] = network_.bssid.bytes[i];
  }
  context.associated = associated;
  context.association_id = association_id;
  uint8_t out[kMacContextBytes], response[16];
  const uint32_t n = iwl_build_station_mac_context(context, ByteView(out, sizeof out));
  IwlHostCommand command;
  command.opcode = kMacContextCommand;
  command.data = ConstByteView(out, n);
  return send_command(command, ByteView(response, sizeof response), nullptr, error);
}

bool IwlStationLink::prepare(const IwlFirmware& firmware, const IwlNvmData& nvm, const WifiNetwork& network, const char** error, const WifiPhyNegotiation& phy) {
  const char* local = nullptr;
  if (!error) error = &local;
  release();
  if (!transport_.running()) {
    *error = "the firmware is not running";
    return false;
  }
  // the structure versions this file writes
  if (firmware.command_version_of(0, kPhyContextCommand) >= 3 || firmware.has_api(48) || firmware.command_version_of(0, kAddStaKeyCommand) != 1 ||
      firmware.command_version_of(0, kTxCommand) != 6) {
    *error = "this firmware's PHY context, key or transmit commands are of a version this driver does not speak";
    return false;
  }
  firmware_ = &firmware;
  network_ = network;
  phy_ = phy;
  wifi_build_rate_ladder(phy_, &ladder_);
  if (ladder_.count <= 1) phy_ = WifiPhyNegotiation();             // no usable HT rates: the legacy ladder
  for (uint32_t i = 0; i < 6; ++i) address_[i] = nvm.hw_address[i];
  tx_antennas_ = nvm.tx_antennas ? nvm.tx_antennas : 1;
  rx_antennas_ = nvm.rx_antennas ? nvm.rx_antennas : 1;
  uint8_t out[kMacContextBytes], response[16];
  IwlHostCommand command;
  int32_t length = 0;

  // the channel
  uint32_t n = iwl_build_phy_context(phy_added_ ? kCtxtActionModify : kCtxtActionAdd, network.channel, network.channel > 14, tx_antennas_, rx_antennas_, ByteView(out, sizeof out),
                                phy_.ht ? (phy_.width_mhz == 80 ? 2 : phy_.width_mhz == 40 ? 1 : 0) : 0, phy_.control_position());
  command.opcode = kPhyContextCommand;
  command.data = ConstByteView(out, n);
  if (!send_command(command, ByteView(response, sizeof response), nullptr, error)) return false;
  phy_added_ = true;
  // the interface now knows the access point
  if (!send_mac_context(false, 0, error)) return false;
  // the interface bound to the channel
  n = iwl_build_binding(kCtxtActionAdd, 0, 0, 0, ByteView(out, sizeof out));
  command = IwlHostCommand();
  command.opcode = kBindingContextCommand;
  command.data = ConstByteView(out, n);
  if (!send_command(command, ByteView(response, sizeof response), &length, error)) return false;
  if (status_of(response, length) != 0) {
    *error = "the firmware refused the binding";
    return false;
  }
  // the transmit queue and the access point as a station served by it
  if (!queue_.init(error)) return false;
  queue_.share_byte_counts_of(transport_.command_queue());
  if (!queue_.start_secondary(error)) return false;
  IwlStationSpec spec;
  fill_station(&spec);
  if (!send_station(spec, error)) return false;
  // the rates: the firmware tries the entries of a table; without the command (or if it refuses) frames go out at the fixed rate
  rate_control_ = false;
  lq_dirty_ = false;
  lq_failures_ = 0;
  if (firmware.command_version_of(0, kLinkQualityCommand) != 0) {
    if (phy_.ht) {
      uint32_t kbps[WifiRateScaler::kMaxRates];
      for (uint32_t i = 0; i < ladder_.count; ++i) {
        kbps[i] = ladder_.rate[i].kbps;
        codes_[i] = iwl_rate_code(ladder_.rate[i]);
      }
      scaler_.start(network.signal_percent, ArrayView<const uint32_t>(kbps, ladder_.count));
    } else {
      for (uint32_t i = 0; i < WifiRateScaler::kRateCount; ++i) codes_[i] = WifiRateScaler::kPlcp[i];
      scaler_.start(network.signal_percent);
    }
    scaler_.take_changed();
    const char* lq_error = nullptr;
    if (send_link_quality(&lq_error)) rate_control_ = true;
    else if (transport_.firmware_failed()) { *error = lq_error; return false; }
  }
  // keep the channel for the join (iwl_mvm_protect_session): a refusal does not stop the join, a failed firmware does
  {
    IwlTimeEvent event;
    event.duration_tu = 500;
    uint8_t te[kTimeEventBytes];
    const uint32_t te_bytes = iwl_build_time_event(event, ByteView(te, sizeof te));
    command = IwlHostCommand();
    command.opcode = kTimeEventCommand;
    command.data = ConstByteView(te, te_bytes);
    uint8_t reply[16];
    int32_t reply_length = 0;
    const char* te_error = nullptr;
    time_event_active_ = false;
    if (send_command(command, ByteView(reply, sizeof reply), &reply_length, &te_error)) {
      uint32_t status = 0, unique = 0;
      if (iwl_parse_time_event_response(ConstByteView(reply, reply_length < 0 ? 0 : static_cast<uint32_t>(reply_length > 16 ? 16 : reply_length)), &status, &unique) && status != 0) {
        time_event_id_ = unique;
        time_event_active_ = true;
      }
    } else if (transport_.firmware_failed()) {
      *error = te_error;
      return false;
    }
  }
  prepared_ = true;
  have_ptk_ = false;
  pn_ = 0;
  return true;
}

void IwlStationLink::teardown() {
  if (prepared_) {
    const char* error = nullptr;
    uint8_t out[24], response[16];
    if (time_event_active_) {                                 // the protection may still be running
      IwlTimeEvent event;
      event.action = kCtxtActionRemove;
      event.event = time_event_id_;
      uint8_t te[kTimeEventBytes];
      const uint32_t te_bytes = iwl_build_time_event(event, ByteView(te, sizeof te));
      IwlHostCommand te_command;
      te_command.opcode = kTimeEventCommand;
      te_command.data = ConstByteView(te, te_bytes);
      send_command(te_command, ByteView(response, sizeof response), nullptr, &error);
      time_event_active_ = false;
    }
    const uint8_t remove_station[4] = {kStationId, 0, 0, 0};
    IwlHostCommand command;
    command.opcode = 0x19;                                    // REMOVE_STA
    command.data = ConstByteView(remove_station, sizeof remove_station);
    send_command(command, ByteView(response, sizeof response), nullptr, &error);
    const uint32_t n = iwl_build_binding(kCtxtActionRemove, 0, 0, 0, ByteView(out, sizeof out));
    command = IwlHostCommand();
    command.opcode = kBindingContextCommand;
    command.data = ConstByteView(out, n);
    send_command(command, ByteView(response, sizeof response), nullptr, &error);
    queue_.deactivate();
  }
  release();
}

void IwlStationLink::release() {
  prepared_ = false;
  have_ptk_ = false;
  rate_control_ = false;
  queue_.release();
  for (uint32_t i = 0; i < sizeof tk_; ++i) tk_[i] = 0;
}

bool IwlStationLink::enqueue(ConstByteView header, ConstByteView body, uint16_t frame_bytes, bool protect, bool data, bool association_request) {
  if (!prepared_) return false;
  IwlTxParams params;
  params.header = header;
  params.frame_bytes = frame_bytes;
  params.station_id = kStationId;
  params.protect = protect;
  params.five_ghz = network_.channel > 14;
  params.data = data;
  params.association_request = association_request;
  params.tx_antennas = tx_antennas_;
  params.station_rate = rate_control_;
  params.data_plcp = rate_control_ ? codes_[scaler_.current()] : kRatePlcp24M;
  if (protect)
    for (uint32_t i = 0; i < 16; ++i) params.tk[i] = tk_[i];
  uint8_t prefix[kTxCommandHeaderBytes + kTxParamBytes + 32];
  const uint32_t n = iwl_build_tx_prefix(params, ByteView(prefix, sizeof prefix));
  if (n == 0) return false;
  const char* error = nullptr;
  if (queue_.enqueue_frame(ConstByteView(prefix, n), body, frame_bytes, kStationId, protect, &error) < 0) {
    ++tx_failures_;
    return false;
  }
  ++frames_sent_;
  return true;
}

bool IwlStationLink::transmit(ConstByteView frame) {
  if (frame.size() < 24) return false;
  const uint16_t control = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
  const bool association_request = (control & 0x00FC) == 0x0000;      // management, subtype association request
  const bool is_data = (control & 0x000C) == 0x0008;
  if (is_data && have_ptk_) {
    // once the pairwise key is in, even the handshake's last frames (the group key's answer) go out encrypted: a CCMP header after the 802.11 header, the hardware does the rest
    uint8_t header[24];
    for (uint32_t i = 0; i < 24; ++i) header[i] = frame[i];
    header[1] |= 0x40;
    uint8_t body[2048];
    const ConstByteView rest = frame.subview(24);
    if (rest.size() + 8 > sizeof body) return false;
    const uint64_t pn = ++pn_;
    body[0] = static_cast<uint8_t>(pn); body[1] = static_cast<uint8_t>(pn >> 8); body[2] = 0; body[3] = 0x20;
    body[4] = static_cast<uint8_t>(pn >> 16); body[5] = static_cast<uint8_t>(pn >> 24); body[6] = static_cast<uint8_t>(pn >> 32); body[7] = static_cast<uint8_t>(pn >> 40);
    for (uint64_t i = 0; i < rest.size(); ++i) body[8 + i] = rest[i];
    return enqueue(ConstByteView(header, 24), ConstByteView(body, 8 + rest.size()), static_cast<uint16_t>(24 + 8 + rest.size()), true, false, false);
  }
  return enqueue(frame.subview(0, 24), frame.subview(24), static_cast<uint16_t>(frame.size()), false, false, association_request);
}

bool IwlStationLink::send_ethernet(ConstByteView frame) {
  if (!prepared_ || !have_ptk_) return false;
  uint8_t header[24];
  uint8_t body[2048];
  const uint32_t n = iwl_ethernet_to_data_frame(frame, network_.bssid.bytes, address_, true, ++pn_, 0, ByteView(header, sizeof header), ByteView(body, sizeof body));
  if (n == 0) return false;
  return enqueue(ConstByteView(header, 24), ConstByteView(body, n), static_cast<uint16_t>(24 + n), true, true, false);
}

bool IwlStationLink::install_pairwise_key(const uint8_t tk[wpa2::kTkBytes]) {
  if (!prepared_) return false;
  const uint8_t rsc[8] = {};
  uint8_t out[kStaKeyBytes], response[16];
  const uint32_t n = iwl_build_station_key(kStationId, 0, 0, false, tk, rsc, ByteView(out, sizeof out));
  IwlHostCommand command;
  command.opcode = kAddStaKeyCommand;
  command.data = ConstByteView(out, n);
  int32_t length = 0;
  const char* error = nullptr;
  if (!send_command(command, ByteView(response, sizeof response), &length, &error) || status_of(response, length) != kAddStaSuccess) return false;
  for (uint32_t i = 0; i < 16; ++i) tk_[i] = tk[i];
  have_ptk_ = true;
  pn_ = 0;
  return true;
}

bool IwlStationLink::install_group_key(const wpa2::GroupKey& key) {
  if (!prepared_ || key.length != 16) return false;
  uint8_t out[kStaKeyBytes], response[16];
  const uint32_t n = iwl_build_station_key(kStationId, 1, key.id, true, key.key, key.rsc, ByteView(out, sizeof out));
  IwlHostCommand command;
  command.opcode = kAddStaKeyCommand;
  command.data = ConstByteView(out, n);
  int32_t length = 0;
  const char* error = nullptr;
  return send_command(command, ByteView(response, sizeof response), &length, &error) && status_of(response, length) == kAddStaSuccess;
}

void IwlStationLink::port_opened(uint16_t association_id) {
  if (!prepared_) return;
  const char* error = nullptr;
  send_mac_context(true, association_id, &error);
  IwlStationSpec spec;
  fill_station(&spec);
  spec.modify = true;
  spec.association_id = association_id;
  send_station(spec, &error);
}

void IwlStationLink::on_notification(const IwlRxPacket& packet) {
  if (!prepared_) return;
  if (packet.group == 0 && packet.command == kRxMpduCommand) {
    IwlRxFrame rx;
    if (!iwl_parse_rx_mpdu(packet, &rx) || !rx.crc_ok || rx.frame.size() < 24) return;
    ++frames_received_;
    const uint16_t control = static_cast<uint16_t>(rx.frame[0] | (rx.frame[1] << 8));
    if ((control & 0x000C) == 0x0008) {                                    // data
      uint8_t ethernet[2048];
      const uint32_t n = iwl_data_frame_to_ethernet(rx.frame, rx.decrypted, ByteView(ethernet, sizeof ethernet));
      if (n >= 14 && ethernet[12] == 0x88 && ethernet[13] == 0x8E) {         // EAPOL is the state machine's, not the network stack's
        if (association_) association_->on_eapol(ConstByteView(ethernet + 14, n - 14));
      } else if (n && sink_) {
        sink_->on_ethernet_frame(ConstByteView(ethernet, n));
      }
    } else if (association_) {
      association_->on_frame(rx.frame);
    }
    return;
  }
  if (packet.group == 0 && packet.command == kTxCommand && packet.queue() == kDataQueue) {
    IwlTxResponse response;
    if (iwl_parse_tx_response(packet.payload, &response)) {
      if (!response.success()) ++tx_failures_;
      if (rate_control_ && response.frame_count == 1 && (response.frame_control & 0x000C) == 0x0008) {         // a data frame: how did it fare?
        uint32_t index = scaler_.rate_count();
        if (!(response.initial_rate & iwl_mvm::kRateCckV1)) index = index_of_rate(response.initial_rate);
        scaler_.on_result(response.success(), static_cast<uint32_t>(response.failure_frame) + 1, index, clock_.now_ms());
      }
      queue_.reclaim_until(response.next_index);
    }
  }
}

}  // namespace hardware
