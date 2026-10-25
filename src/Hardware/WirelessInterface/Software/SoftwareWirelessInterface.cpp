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

#include "Hardware/WirelessInterface/Software/SoftwareWirelessInterface.hpp"

namespace hardware {

namespace {
void copy_text(char* out, uint32_t capacity, StringView text) {
  uint32_t n = 0;
  for (; n < text.size() && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}
bool same_ssid(const char* stored, StringView ssid) {
  uint32_t n = 0;
  while (stored[n]) ++n;
  return n == ssid.size() && StringView(stored, n).equals(ssid);
}
}  // namespace

bool SoftwareWirelessInterface::add_access_point(StringView ssid, const MacAddress& bssid, WifiSecurity security, uint8_t channel, uint8_t signal_percent, StringView passphrase) {
  if (ssid.empty() || ssid.size() > kMaxSsidLength || passphrase.size() > kMaxPassphraseLength) return false;
  AccessPoint ap;
  copy_text(ap.network.ssid, sizeof ap.network.ssid, ssid);
  ap.network.bssid = bssid;
  ap.network.security = security;
  ap.network.channel = channel;
  ap.network.frequency_mhz = wifi_channel_frequency_mhz(channel);
  ap.network.signal_percent = signal_percent > 100 ? 100 : signal_percent;
  copy_text(ap.passphrase, sizeof ap.passphrase, passphrase);
  kernel::ScopedMutex guard(mutex_);
  for (const AccessPoint& other : access_points_)
    if (same_ssid(other.network.ssid, ssid)) return false;
  return access_points_.push_back(ap);
}

bool SoftwareWirelessInterface::remove_access_point(StringView ssid) {
  kernel::ScopedMutex guard(mutex_);
  for (uint64_t i = 0; i < access_points_.count(); ++i) {
    if (!same_ssid(access_points_[i].network.ssid, ssid)) continue;
    access_points_.remove_at(i);
    if (status_.state == WifiState::Connected && same_ssid(status_.network.ssid, ssid)) status_ = WifiStatus();
    return true;
  }
  return false;
}

bool SoftwareWirelessInterface::set_signal(StringView ssid, uint8_t signal_percent) {
  kernel::ScopedMutex guard(mutex_);
  for (AccessPoint& ap : access_points_)
    if (same_ssid(ap.network.ssid, ssid)) {
      ap.network.signal_percent = signal_percent > 100 ? 100 : signal_percent;
      return true;
    }
  return false;
}

bool SoftwareWirelessInterface::connected() const {
  kernel::ScopedMutex guard(mutex_);
  return status_.state == WifiState::Connected;
}

bool SoftwareWirelessInterface::link_up() const { return connected() && uplink_.link_up(); }

uint32_t SoftwareWirelessInterface::link_speed_mbps() const { return connected() ? uplink_.link_speed_mbps() : 0; }

bool SoftwareWirelessInterface::transmit(ConstByteView frame) {
  if (!connected()) {
    kernel::ScopedMutex guard(mutex_);
    ++own_.send_failures;
    return false;
  }
  return uplink_.transmit(frame);
}

bool SoftwareWirelessInterface::receive(ByteView buffer, uint32_t* length) {
  // Frames that reach the uplink while this radio is not joined belong to nobody: they are taken off its queue and dropped.
  const bool joined = connected();
  if (!uplink_.receive(buffer, length)) return false;
  if (joined) return true;
  kernel::ScopedMutex guard(mutex_);
  ++own_.frames_dropped;
  return false;
}

NetworkStatistics SoftwareWirelessInterface::statistics() const {
  NetworkStatistics total = uplink_.statistics();
  kernel::ScopedMutex guard(mutex_);
  total.send_failures += own_.send_failures;
  total.frames_dropped += own_.frames_dropped;
  return total;
}

bool SoftwareWirelessInterface::start_scan() {
  kernel::ScopedMutex guard(mutex_);
  if (status_.state == WifiState::Connecting || status_.state == WifiState::Scanning) return false;
  // The air is simulated, so the scan is over at once: the access points, strongest first (insertion sort - there are few).
  results_.clear();
  for (const AccessPoint& ap : access_points_) {
    uint64_t at = results_.count();
    while (at > 0 && results_[at - 1].signal_percent < ap.network.signal_percent) --at;
    if (!results_.insert(at, ap.network)) return false;
  }
  return true;
}

uint32_t SoftwareWirelessInterface::scan_results(ArrayView<WifiNetwork> out) const {
  kernel::ScopedMutex guard(mutex_);
  uint32_t n = 0;
  for (const WifiNetwork& network : results_) {
    if (n >= out.size()) break;
    out[n++] = network;
  }
  return n;
}

WifiResult SoftwareWirelessInterface::connect(StringView ssid, StringView passphrase) {
  if (ssid.empty() || ssid.size() > kMaxSsidLength) return WifiResult::InvalidArgument;
  kernel::ScopedMutex guard(mutex_);
  if (status_.state == WifiState::Connecting || status_.state == WifiState::Scanning) return WifiResult::Busy;
  const AccessPoint* found = nullptr;
  for (const AccessPoint& ap : access_points_)
    if (same_ssid(ap.network.ssid, ssid)) found = &ap;
  WifiResult result = WifiResult::Ok;
  if (!found) {
    result = WifiResult::NoSuchNetwork;
  } else if (found->network.security == WifiSecurity::Enterprise) {
    result = WifiResult::Unsupported;
  } else if (found->network.security != WifiSecurity::Open) {
    // (A chipset proves it knows the key in the four-way handshake; here the key is compared.)
    const bool wpa = found->network.security != WifiSecurity::Wep;
    if (wpa && (passphrase.size() < kMinPassphraseLength || passphrase.size() > kMaxPassphraseLength)) result = WifiResult::BadPassphrase;
    else if (!passphrase.equals(StringView(found->passphrase, __builtin_strlen(found->passphrase)))) result = WifiResult::BadPassphrase;
  }
  if (result != WifiResult::Ok) {
    status_.last_result = result;
    return result;
  }
  status_.state = WifiState::Connected;
  status_.last_result = WifiResult::Ok;
  status_.network = found->network;
  return WifiResult::Ok;
}

void SoftwareWirelessInterface::disconnect() {
  kernel::ScopedMutex guard(mutex_);
  status_ = WifiStatus();
}

WifiStatus SoftwareWirelessInterface::status() const {
  kernel::ScopedMutex guard(mutex_);
  WifiStatus status = status_;
  // The signal moves with the air.
  if (status.state == WifiState::Connected)
    for (const AccessPoint& ap : access_points_)
      if (same_ssid(ap.network.ssid, status.network.ssid)) status.network.signal_percent = ap.network.signal_percent;
  return status;
}

}  // namespace hardware
