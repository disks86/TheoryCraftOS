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


#include "Hardware/WirelessInterface/Intel/IwlWireless.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

IwlWirelessInterface::IwlWirelessInterface(const Inputs& inputs) : in_(inputs), driver_(*inputs.allocator), results_(*inputs.allocator) {}

bool IwlWirelessInterface::attach() {
  const char* error = nullptr;
  IwlScanner* scanner = driver_.enable_scanning(&error);
  if (!scanner) {
    failure_ = error ? error : "scanning could not be prepared";
    driver_.stop();
    return false;
  }
  link_ = UniquePointer<IwlStationLink>::make(*in_.allocator, *driver_.transport(), *in_.allocator, *in_.clock);
  if (!link_) {
    failure_ = "out of memory";
    driver_.stop();
    return false;
  }
  association_ = UniquePointer<Ieee80211Association>::make(*in_.allocator, *link_);
  ring_ = allocate_view<Slot>(*in_.allocator, kReceiveSlots);
  if (!association_ || ring_.size() == 0) {
    failure_ = "out of memory";
    association_ = UniquePointer<Ieee80211Association>();
    link_ = UniquePointer<IwlStationLink>();
    driver_.stop();
    return false;
  }
  link_->set_association(association_.get());
  link_->set_data_sink(this);
  scanner->set_forward(link_.get());
  ring_head_ = ring_tail_ = ring_count_ = 0;
  status_ = WifiStatus();
  started_ = true;
  failure_ = "";
  return true;
}

bool IwlWirelessInterface::open_locked() {
  if (!in_.firmware || !in_.clock || !in_.random || (!registers_ && (!in_.pci || !in_.mapper))) {
    failure_ = "not given what it needs to start";
    return false;
  }
  const IwlDriver::Outcome outcome = registers_ ? driver_.start_device(*registers_, integrated_, *in_.firmware) : driver_.start(*in_.pci, in_.device, *in_.mapper, *in_.firmware);
  if (outcome != IwlDriver::Outcome::Started) {
    failure_ = IwlDriver::outcome_name(outcome);
    return false;
  }
  return attach();
}

bool IwlWirelessInterface::initialize() {
  kernel::ScopedMutex guard(mutex_);
  if (started_) return true;
  registers_ = nullptr;
  return open_locked();
}

bool IwlWirelessInterface::start(IIwlRegisters& registers, bool integrated) {
  kernel::ScopedMutex guard(mutex_);
  if (started_) return true;
  registers_ = &registers;
  integrated_ = integrated;
  return open_locked();
}

void IwlWirelessInterface::leave_locked() {
  if (association_) association_->leave();
  if (link_) link_->teardown();
  connected_ = false;
}

void IwlWirelessInterface::shutdown() {
  kernel::ScopedMutex guard(mutex_);
  shutdown_locked();
}

void IwlWirelessInterface::shutdown_locked() {
  if (!started_) return;
  interrupts_ = gating_ = false;
  leave_locked();
  started_ = false;
  association_ = UniquePointer<Ieee80211Association>();
  link_ = UniquePointer<IwlStationLink>();
  driver_.stop();
  release_view(*in_.allocator, ring_);
  results_.clear();
  status_ = WifiStatus();
}

bool IwlWirelessInterface::use_interrupts(bool on) {
  kernel::ScopedMutex guard(mutex_);
  IwlTransport* transport = driver_.transport();
  if (!started_ || !transport) return false;
  transport->use_interrupts(on);
  interrupts_ = on && transport->interrupts_enabled();
  gating_ = interrupts_;
  missed_ = 0;
  seen_interrupts_ = transport->interrupt_count();
  last_look_ms_ = in_.clock->now_ms();
  return interrupts_ == on;
}

// ECHO_CMD is answered by a frame, which is what the card interrupts for.
bool IwlWirelessInterface::verify_interrupts() {
  kernel::ScopedMutex guard(mutex_);
  IwlTransport* transport = driver_.transport();
  if (!started_ || !transport || !interrupts_) return false;
  const uint32_t before = transport->interrupt_count();
  IwlHostCommand echo;
  echo.opcode = 0x03;
  uint8_t response[16];
  const char* error = nullptr;
  if (transport->send_sync(echo, ByteView(response, sizeof response), 100000, &error) < 0) return false;
  for (uint32_t i = 0; i < 20 && transport->interrupt_count() == before; ++i) delay_us(1000);
  return transport->interrupt_count() != before;
}

MacAddress IwlWirelessInterface::mac_address() const {
  MacAddress mac{};
  if (started_)
    for (uint32_t i = 0; i < 6; ++i) mac.bytes[i] = driver_.nvm().hw_address[i];
  return mac;
}

void IwlWirelessInterface::finish_connection(WifiResult result) const {
  if (link_) link_->teardown();
  connected_ = false;
  status_.state = WifiState::Disconnected;
  status_.last_result = result;
}

// The firmware has stopped: everything is let go of (the commands that would say goodbye fail at once), and the card is started again unless it has already been started too often.
void IwlWirelessInterface::recover() const {
  IwlWirelessInterface* self = const_cast<IwlWirelessInterface*>(this);
  const char* text = self->driver_.transport() ? self->driver_.transport()->failure_text() : "";
  uint32_t n = 0;
  for (; text[n] && n + 1 < sizeof failure_text_; ++n) failure_text_[n] = text[n];
  failure_text_[n] = 0;
  const uint64_t now = in_.clock->now_ms();
  if (now - last_restart_ms_ > kRestartWindowMs) recent_restarts_ = 0;
  self->shutdown_locked();
  if (recent_restarts_ >= kMaxRecentRestarts) {
    self->failure_ = "the firmware keeps failing";
    return;
  }
  ++recent_restarts_;
  ++restarts_;
  last_restart_ms_ = now;
  self->open_locked();
}

void IwlWirelessInterface::pump() const {
  if (!started_) return;
  IwlTransport* transport = const_cast<IwlDriver&>(driver_).transport();
  if (transport && interrupts_ && gating_) {
    // nothing is going on and nothing was said: do not look (the card interrupts for what arrives), except now and then
    const bool idle = !scan_pending_ && status_.state != WifiState::Scanning && status_.state != WifiState::Connecting;
    const uint64_t now = in_.clock->now_ms();
    if (idle && !transport->interrupt_pending() && now - last_look_ms_ < kSafetyPollMs) return;
    const bool safety = !transport->interrupt_pending();
    last_look_ms_ = now;
    const uint32_t frames_before = transport->frames_received();
    if (transport->check_firmware()) {
      recover();
      return;
    }
    transport->poll(nullptr);
    const bool found = transport->frames_received() != frames_before;
    const bool interrupted = transport->interrupt_count() != seen_interrupts_;
    seen_interrupts_ = transport->interrupt_count();
    if (found && !safety) missed_ = 0;                                  // an interrupt told us, and there was something: they work
    else if (found && safety && idle && !interrupted) {                 // frames nobody told us about
      if (++missed_ >= kMissedBeforeGivingUp) gating_ = false;
    }
  } else if (transport && transport->check_firmware()) {
    recover();
    return;
  }
  IwlScanner* scanner = const_cast<IwlDriver&>(driver_).scanner();
  if (!scanner) return;
  scanner->poll();
  if (scan_pending_ && !scanner->scanning()) {
    scan_pending_ = false;
    WifiNetwork found[IwlScanner::kMaxNetworks];
    const uint32_t n = scanner->results(ArrayView<WifiNetwork>(found, IwlScanner::kMaxNetworks));
    results_.clear();
    for (uint32_t i = 0; i < n; ++i) results_.push_back(found[i]);
    ++scan_generation_;
    if (status_.state == WifiState::Scanning) status_.state = WifiState::Disconnected;
  }
  if (!association_ || (status_.state != WifiState::Connecting && status_.state != WifiState::Connected)) return;
  association_->poll();
  if (link_) link_->service();
  // a join is a conversation: each answer the card has already received moves it one step on, so run a few rounds rather than waiting for the next call
  for (uint32_t round = 0; round < 8 && status_.state == WifiState::Connecting; ++round) {
    const Ieee80211Association::State before = association_->state();
    if (before == Ieee80211Association::State::Connected || before == Ieee80211Association::State::Failed) break;
    scanner->poll();
    association_->poll();
    if (association_->state() == before) break;
  }
  switch (association_->state()) {
    case Ieee80211Association::State::Connected:
      if (!connected_) {
        connected_ = true;
        status_.state = WifiState::Connected;
        status_.last_result = WifiResult::Ok;
      }
      break;
    case Ieee80211Association::State::Failed:
      finish_connection(association_->bad_passphrase_suspected() ? WifiResult::BadPassphrase : WifiResult::Failed);
      break;
    default:
      break;
  }
}

uint32_t IwlWirelessInterface::link_speed_mbps() const {
  if (!link_up()) return 0;
  kernel::ScopedMutex guard(mutex_);
  return link_ ? link_->data_rate_kbps() / 1000 : 0;
}

bool IwlWirelessInterface::link_up() const {
  kernel::ScopedMutex guard(mutex_);
  pump();
  return connected_;
}

bool IwlWirelessInterface::start_scan() {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return false;
  pump();
  if (scan_pending_ || status_.state == WifiState::Connecting || status_.state == WifiState::Connected) return false;
  IwlScanner* scanner = driver_.scanner();
  const char* error = nullptr;
  if (!scanner || !scanner->start_scan(true, &error)) return false;
  scan_pending_ = true;
  status_.state = WifiState::Scanning;
  return true;
}

uint32_t IwlWirelessInterface::scan_results(ArrayView<WifiNetwork> out) const {
  kernel::ScopedMutex guard(mutex_);
  pump();
  uint32_t n = 0;
  for (uint64_t i = 0; i < results_.count() && n < out.size(); ++i) out[n++] = results_[i];
  return n;
}

WifiResult IwlWirelessInterface::connect(StringView ssid, StringView passphrase) {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return WifiResult::Failed;
  pump();
  if (ssid.size() == 0 || ssid.size() > kMaxSsidLength) return WifiResult::InvalidArgument;
  if (scan_pending_ || status_.state == WifiState::Connecting || status_.state == WifiState::Connected) return WifiResult::Busy;
  // the network, as the latest scan heard it (the strongest access point of that name)
  const WifiNetwork* target = nullptr;
  for (uint64_t i = 0; i < results_.count() && !target; ++i) {
    const WifiNetwork& n = results_[i];
    uint32_t length = 0;
    while (length < kMaxSsidLength && n.ssid[length]) ++length;
    if (length != ssid.size()) continue;
    bool same = true;
    for (uint32_t k = 0; k < length && same; ++k) same = n.ssid[k] == ssid[k];
    if (same) target = &n;
  }
  if (!target) {
    status_.last_result = WifiResult::NoSuchNetwork;
    return WifiResult::NoSuchNetwork;
  }
  const WifiNetwork network = *target;
  if (network.security != WifiSecurity::Open && network.security != WifiSecurity::Wpa2) {
    status_.last_result = WifiResult::Unsupported;
    return WifiResult::Unsupported;
  }
  if (network.security == WifiSecurity::Wpa2 && (passphrase.size() < kMinPassphraseLength || passphrase.size() > kMaxPassphraseLength)) {
    status_.last_result = WifiResult::BadPassphrase;
    return WifiResult::BadPassphrase;
  }
  uint8_t snonce[wpa2::kNonceBytes];
  if (!in_.random->fill(ByteView(snonce, sizeof snonce))) {
    status_.last_result = WifiResult::Failed;
    return WifiResult::Failed;
  }
  IwlScanner* scanner = driver_.scanner();
  ConstByteView rsn;
  if (network.security == WifiSecurity::Wpa2 && !scanner->rsn_element(network.bssid, &rsn)) {
    status_.last_result = WifiResult::Failed;               // the scan did not keep the access point's RSN element
    return WifiResult::Failed;
  }
  const char* error = nullptr;
  if (!link_->prepare(driver_.firmware(), driver_.nvm(), network, &error)) {
    status_.last_result = WifiResult::Failed;
    return WifiResult::Failed;
  }
  uint8_t address[6];
  for (uint32_t i = 0; i < 6; ++i) address[i] = driver_.nvm().hw_address[i];
  const WifiResult started = association_->start(network, rsn, passphrase, snonce, address);
  if (started != WifiResult::Ok) {
    link_->teardown();
    status_.last_result = started;
    return started;
  }
  status_.state = WifiState::Connecting;
  status_.last_result = WifiResult::Ok;
  status_.network = network;
  return WifiResult::Ok;
}

void IwlWirelessInterface::disconnect() {
  kernel::ScopedMutex guard(mutex_);
  if (!started_) return;
  if (status_.state == WifiState::Connecting || status_.state == WifiState::Connected) {
    leave_locked();
    status_.state = WifiState::Disconnected;
    status_.last_result = WifiResult::Ok;
  }
}

WifiStatus IwlWirelessInterface::status() const {
  kernel::ScopedMutex guard(mutex_);
  pump();
  return status_;
}

bool IwlWirelessInterface::transmit(ConstByteView frame) {
  kernel::ScopedMutex guard(mutex_);
  pump();
  if (!connected_ || frame.size() < kEthernetHeaderSize || frame.size() > kEthernetMaxFrame || !link_->send_ethernet(frame)) {
    ++own_.send_failures;
    return false;
  }
  ++own_.frames_sent;
  return true;
}

void IwlWirelessInterface::on_ethernet_frame(ConstByteView frame) {
  if (frame.size() > kFrameBytes || ring_.size() == 0) {
    ++own_.frames_dropped;
    return;
  }
  if (ring_count_ == kReceiveSlots) {                         // the oldest goes
    ring_tail_ = (ring_tail_ + 1) % kReceiveSlots;
    --ring_count_;
    ++own_.frames_dropped;
  }
  Slot& slot = ring_[ring_head_];
  slot.length = static_cast<uint16_t>(frame.size());
  ByteView(slot.data, frame.size()).copy_from(frame);
  ring_head_ = (ring_head_ + 1) % kReceiveSlots;
  ++ring_count_;
}

bool IwlWirelessInterface::receive(ByteView buffer, uint32_t* length) {
  kernel::ScopedMutex guard(mutex_);
  pump();
  if (ring_count_ == 0) return false;
  const Slot& slot = ring_[ring_tail_];
  const uint32_t n = slot.length < buffer.size() ? slot.length : static_cast<uint32_t>(buffer.size());
  if (n) buffer.subview(0, n).copy_from(ConstByteView(slot.data, n));
  *length = slot.length;
  ring_tail_ = (ring_tail_ + 1) % kReceiveSlots;
  --ring_count_;
  ++own_.frames_received;
  return true;
}

NetworkStatistics IwlWirelessInterface::statistics() const {
  kernel::ScopedMutex guard(mutex_);
  return own_;
}

}  // namespace hardware
