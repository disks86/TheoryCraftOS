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


#include "Hardware/WirelessInterface/Intel/IwlScan.hpp"
#include "Hardware/WirelessInterface/Ieee80211Scan.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace iwl_mvm;

namespace {
constexpr uint8_t kAuxStationId = 0;
}

bool IwlScanner::send(const IwlHostCommand& command, ByteView response, int32_t* length, const char** error) {
  const int32_t n = transport_.send_sync(command, response, kCommandTimeoutUs, error);
  if (length) *length = n;
  return n >= 0;
}

bool IwlScanner::bring_up(const IwlFirmware& firmware, const IwlNvmData& nvm, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  shut_down();
  if (!transport_.running()) {
    *error = "the firmware is not running";
    return false;
  }
  if (firmware.command_version_of(0, kAddStaCommand) != kAddStaVersion || firmware.command_version_of(0, kScanRequestUmacCommand) != kScanRequestVersion ||
      !firmware.has_api(kApiStaType) || !firmware.has_api(kApiReducedScanConfig) || !firmware.has_api(kApiAdaptiveDwellV2)) {
    *error = "this firmware's scan or station commands are of a version this driver does not speak";
    return false;
  }
  firmware_ = &firmware;
  nvm_ = nvm;
  transport_.set_handler(this);                    // frames and notifications between the commands are for the scanner
  uint8_t out[kMacContextBytes];
  uint8_t response[16];
  IwlHostCommand command;

  // the transmit antennas
  uint32_t n = iwl_build_tx_ant_config(nvm.tx_antennas, ByteView(out, sizeof out));
  command = IwlHostCommand();
  command.opcode = kTxAntConfigCommand;
  command.data = ConstByteView(out, n);
  if (!send(command, ByteView(response, sizeof response), nullptr, error)) return false;

  // DQA: the queues are assigned by function (the command queue 0, the auxiliary 1, ...)
  if (firmware.has_capability(kCapabilityDqaSupport)) {
    n = iwl_build_dqa_enable(iwl_tx::kCommandQueue, ByteView(out, sizeof out));
    command = IwlHostCommand();
    command.group = kDataPathGroup;
    command.opcode = kDqaEnableCommand;
    command.data = ConstByteView(out, n);
    if (!send(command, ByteView(response, sizeof response), nullptr, error)) return false;
  }

  // the auxiliary queue and the station that uses it (the scan's frames go out through it)
  if (!aux_queue_.init(error) || !aux_queue_.start_secondary(error)) return false;
  n = iwl_build_add_aux_station(kAuxStationId, 1u << iwl_tx::kAuxQueue, ByteView(out, sizeof out));
  command = IwlHostCommand();
  command.opcode = kAddStaCommand;
  command.data = ConstByteView(out, n);
  int32_t length = 0;
  if (!send(command, ByteView(response, sizeof response), &length, error)) return false;
  const uint32_t status = length >= 4 ? static_cast<uint32_t>(response[0]) | (static_cast<uint32_t>(response[1]) << 8) | (static_cast<uint32_t>(response[2]) << 16) |
                                         (static_cast<uint32_t>(response[3]) << 24)
                                     : 0;
  if (length < 4 || (status & kAddStaStatusMask) != kAddStaSuccess) {
    *error = "the firmware refused the auxiliary station";
    return false;
  }

  // the scan configuration
  n = iwl_build_scan_config(kAuxStationId, nvm.tx_antennas, nvm.rx_antennas, ByteView(out, sizeof out));
  command = IwlHostCommand();
  command.group = kLongGroup;
  command.opcode = kScanConfigCommand;
  command.data = ConstByteView(out, n);
  if (!send(command, ByteView(response, sizeof response), nullptr, error)) return false;

  // the station interface the scan runs for
  IwlMacContext context;
  for (uint32_t i = 0; i < 6; ++i) context.address[i] = nvm.hw_address[i];
  n = iwl_build_station_mac_context(context, ByteView(out, sizeof out));
  command = IwlHostCommand();
  command.opcode = kMacContextCommand;
  command.data = ConstByteView(out, n);
  if (!send(command, ByteView(response, sizeof response), nullptr, error)) return false;

  ready_ = true;
  return true;
}

void IwlScanner::shut_down() {
  if (ready_) transport_.set_handler(nullptr);
  ready_ = false;
  scanning_ = false;
  aux_queue_.release();
  networks_.clear();
}

bool IwlScanner::start_scan(bool active, const char** error) {
  const char* local = nullptr;
  if (!error) error = &local;
  if (!ready_ || !firmware_) {
    *error = "the scanner is not ready";
    return false;
  }
  if (scanning_) {
    *error = "a scan is already running";
    return false;
  }
  // the channels the regulatory data allows; the radar channels are only listened on
  IwlScanChannelEntry channels[iwl_nvm::kMaxChannels];
  uint32_t count = 0;
  const uint32_t room = firmware_->scan_channels() < iwl_nvm::kMaxChannels ? firmware_->scan_channels() : iwl_nvm::kMaxChannels;
  for (uint32_t i = 0; i < nvm_.channel_count && count < room; ++i) {
    const IwlChannel& c = nvm_.channels[i];
    if (!c.valid()) continue;
    if (c.is_5ghz ? !nvm_.band_5ghz : !nvm_.band_24ghz) continue;
    channels[count].number = static_cast<uint8_t>(c.number);
    channels[count].is_5ghz = c.is_5ghz;
    channels[count].probe = (c.flags & iwl_nvm::kChannelActive) && !(c.flags & iwl_nvm::kChannelRadar);
    ++count;
  }
  if (count == 0) {
    *error = "no channel is allowed";
    return false;
  }
  IwlScanRequest request;
  request.uid = next_uid_++;
  request.active = active;
  for (uint32_t i = 0; i < 6; ++i) request.source_address[i] = nvm_.hw_address[i];
  request.firmware_channel_slots = firmware_->scan_channels();
  request.channels = channels;
  request.channel_count = count;
  const uint32_t size = iwl_scan_request_bytes(request.firmware_channel_slots);
  ArrayView<uint8_t> buffer = allocate_view<uint8_t>(allocator_, size);
  if (buffer.size() == 0) {
    *error = "no memory for the scan request";
    return false;
  }
  const uint32_t n = iwl_build_scan_request(request, buffer);
  bool ok = n != 0;
  if (!ok) *error = "the scan request does not fit";
  if (ok) {
    // (the firmware may answer, report networks and even finish before the command call returns: the state is ready first)
    networks_.clear();
    scan_uid_ = request.uid;
    scanning_ = true;
    last_completed_ = false;
    IwlHostCommand command;
    command.group = kLongGroup;
    command.opcode = kScanRequestUmacCommand;
    command.data = ConstByteView(buffer.data(), n);
    uint8_t response[16];
    ok = send(command, ByteView(response, sizeof response), nullptr, error);
    if (!ok) scanning_ = false;
  }
  release_view(allocator_, buffer);
  return ok;
}

void IwlScanner::add_network(const WifiNetwork& network) {
  for (uint64_t i = 0; i < networks_.count(); ++i) {
    if (networks_[i].bssid == network.bssid) {
      if (network.signal_percent > networks_[i].signal_percent) networks_[i].signal_percent = network.signal_percent;
      if (!networks_[i].channel) networks_[i].channel = network.channel;
      return;
    }
  }
  if (networks_.count() < kMaxNetworks) networks_.push_back(network);
}

void IwlScanner::on_notification(const IwlRxPacket& packet) {
  if (packet.group == 0 && packet.command == kRxMpduCommand) {
    IwlRxFrame frame;
    if (!iwl_parse_rx_mpdu(packet, &frame) || !frame.crc_ok) return;
    ++frames_seen_;
    WifiNetwork network;
    if (wifi_parse_announcement(frame.frame, frame.channel, frame.signal_dbm, &network)) add_network(network);
    return;
  }
  if (packet.group == 0 && packet.command == kScanCompleteUmacCommand) {
    IwlScanComplete complete;
    if (iwl_parse_scan_complete(packet.payload, &complete) && complete.uid == scan_uid_) {
      scanning_ = false;
      last_completed_ = complete.completed();
    }
  }
}

bool IwlScanner::poll() {
  transport_.poll(this);
  return scanning_;
}

uint32_t IwlScanner::results(ArrayView<WifiNetwork> out) const {
  // strongest first: copy in order of signal by repeated selection (the list is small)
  const uint64_t total = networks_.count();
  bool used[kMaxNetworks] = {};
  uint32_t copied = 0;
  while (copied < out.size() && copied < total) {
    int64_t best = -1;
    for (uint64_t i = 0; i < total; ++i)
      if (!used[i] && (best < 0 || networks_[i].signal_percent > networks_[static_cast<uint64_t>(best)].signal_percent)) best = static_cast<int64_t>(i);
    if (best < 0) break;
    used[best] = true;
    out[copied++] = networks_[static_cast<uint64_t>(best)];
  }
  return copied;
}

}  // namespace hardware
