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

#include "Hardware/Bus/Ish/Ishtp.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint8_t kHostStartReq = 0x01, kHostStartRes = 0x81, kHostEnumReq = 0x04, kHostEnumRes = 0x84, kPropsReq = 0x05, kPropsRes = 0x85;
constexpr uint8_t kConnectReq = 0x06, kConnectRes = 0x86, kDisconnectReq = 0x07, kDisconnectRes = 0x87, kFlowControl = 0x08, kFwStopReq = 0x03;
constexpr uint32_t kBusPolls = 14000, kConnectPolls = 30000, kCreditPolls = 4000, kPollUs = 500;     // 7 s, 15 s and 2 s (the kernel driver's timeouts)
constexpr uint64_t kMaxAssembly = 256 * 1024;
}  // namespace

bool IshtpBus::wait_flag(const bool* flag, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    poll();
    if (*flag) return true;
    delay_us(kPollUs);
  }
  return false;
}

bool IshtpBus::send_hbm(ConstByteView body) { return ipc_.send_ishtp(0, 0, true, body); }

bool IshtpBus::start() {
  connections_.clear();
  client_count_ = 0;
  started_ = start_supported_ = enumerated_ = false;
  reset_ = false;
  const uint8_t start[4] = {kHostStartReq, 0, 0, 1};                                 // the host bus message version 1.0
  if (!send_hbm(ConstByteView(start, 4)) || !wait_flag(&started_, kBusPolls) || !start_supported_) return false;
  const uint8_t enumerate[4] = {kHostEnumReq, 0, 0, 0};
  if (!send_hbm(ConstByteView(enumerate, 4)) || !wait_flag(&enumerated_, kBusPolls)) return false;
  for (uint32_t address = 0; address < 256; ++address) {
    if (!(map_[address / 8] & (1u << (address % 8)))) continue;
    props_for_ = static_cast<uint8_t>(address);
    props_answered_ = props_ok_ = false;
    const uint8_t request[4] = {kPropsReq, props_for_, 0, 0};
    if (!send_hbm(ConstByteView(request, 4)) || !wait_flag(&props_answered_, kBusPolls)) return false;
    if (props_ok_ && client_count_ < kMaxClients) clients_[client_count_++] = props_;
  }
  return true;
}

const IshtpClientProperties* IshtpBus::find_client(const uint8_t guid[16]) const {
  for (uint32_t i = 0; i < client_count_; ++i) {
    bool same = true;
    for (uint32_t k = 0; k < 16; ++k) same = same && clients_[i].guid[k] == guid[k];
    if (same) return &clients_[i];
  }
  return nullptr;
}

IshtpConnection* IshtpBus::connect(const IshtpClientProperties& client) {
  if (connections_.count() >= kMaxConnections) return nullptr;
  bool used[kMaxConnections + 2] = {};
  for (uint64_t i = 0; i < connections_.count(); ++i)
    if (connections_[i]->host_address < kMaxConnections + 2) used[connections_[i]->host_address] = true;
  uint8_t host = 1;
  while (host < kMaxConnections + 2 && used[host]) ++host;
  UniquePointer<IshtpConnection> c = UniquePointer<IshtpConnection>::make(allocator_, allocator_);
  if (!c) return nullptr;
  c->fw_address = client.address;
  c->host_address = host;
  c->max_message = client.max_message;
  IshtpConnection* raw = c.get();
  if (!connections_.push_back(static_cast<UniquePointer<IshtpConnection>&&>(c))) return nullptr;      // (listed first: the firmware's credit can come right behind the answer)
  connecting_ = raw;
  connect_answered_ = false;
  connect_status_ = 0xFF;
  const uint8_t request[4] = {kConnectReq, client.address, host, 0};
  const bool ok = send_hbm(ConstByteView(request, 4)) && wait_flag(&connect_answered_, kConnectPolls) && connect_status_ == 0;
  connecting_ = nullptr;
  if (!ok) {
    for (uint64_t i = 0; i < connections_.count(); ++i)
      if (connections_[i].get() == raw) { connections_.remove_at(i); break; }
    return nullptr;
  }
  raw->connected = true;
  grant(*raw);
  return raw;
}

bool IshtpBus::grant(IshtpConnection& c) {
  if (c.granted) return true;                                                        // (one outstanding credit at a time)
  const uint8_t body[8] = {kFlowControl, c.fw_address, c.host_address, 0, 0, 0, 0, 0};
  if (!send_hbm(ConstByteView(body, 8))) return false;
  c.granted = true;
  return true;
}

bool IshtpBus::send(IshtpConnection& c, ConstByteView message) {
  if (!c.connected || (c.max_message != 0 && message.size() > c.max_message)) return false;
  for (uint32_t i = 0; i < kCreditPolls && c.credits == 0; ++i) {
    poll();
    if (!c.connected) return false;
    if (c.credits == 0) delay_us(kPollUs);
  }
  if (c.credits == 0) return false;
  --c.credits;
  uint64_t at = 0;
  do {
    const uint64_t n = message.size() - at < kMtu ? message.size() - at : kMtu;
    const bool last = at + n == message.size();
    if (!ipc_.send_ishtp(c.fw_address, c.host_address, last, message.subview(at, n))) return false;
    at += n;
  } while (at < message.size());
  return true;
}

IshtpConnection* IshtpBus::connection_for(uint8_t fw, uint8_t host) {
  for (uint64_t i = 0; i < connections_.count(); ++i)
    if (connections_[i]->fw_address == fw && connections_[i]->host_address == host) return connections_[i].get();
  return nullptr;
}

void IshtpBus::handle_client_piece(uint8_t fw, uint8_t host, bool complete, ConstByteView payload) {
  IshtpConnection* c = connection_for(fw, host);
  if (!c || !c->connected) return;
  if (c->assembly.size() + payload.size() > kMaxAssembly || !c->assembly.append(payload)) {
    c->assembly.clear();                                                             // too long to be real: the message is dropped
    return;
  }
  if (!complete) return;
  c->granted = false;                                                                // the firmware used the credit for this message
  if (c->messages.count() < kMaxQueued) {
    UniquePointer<ByteBuffer> done = UniquePointer<ByteBuffer>::make(allocator_, allocator_);
    if (done && done->append(c->assembly.view())) c->messages.push_back(static_cast<UniquePointer<ByteBuffer>&&>(done));
  }
  c->assembly.clear();
  grant(*c);
}

void IshtpBus::handle_hbm(ConstByteView body) {
  if (body.empty()) return;
  switch (body[0]) {
    case kHostStartRes:
      if (body.size() >= 4) { start_supported_ = body[1] != 0; started_ = true; }
      break;
    case kHostEnumRes:
      if (body.size() >= 4 + 32) {
        for (uint32_t i = 0; i < 32; ++i) map_[i] = body[4 + i];
        enumerated_ = true;
      }
      break;
    case kPropsRes:
      if (body.size() >= 4 + 28 && body[1] == props_for_) {
        props_ = IshtpClientProperties();
        props_.address = body[1];
        for (uint32_t i = 0; i < 16; ++i) props_.guid[i] = body[4 + i];
        props_.version = body[4 + 16];
        props_.max_connections = body[4 + 17];
        props_.max_message = body[4 + 20] | (body[4 + 21] << 8) | (body[4 + 22] << 16) | (static_cast<uint32_t>(body[4 + 23]) << 24);
        props_ok_ = body[2] == 0;
        props_answered_ = true;
      }
      break;
    case kConnectRes:
      if (body.size() >= 4 && connecting_ && connecting_->fw_address == body[1] && connecting_->host_address == body[2]) {
        connect_status_ = body[3];
        connect_answered_ = true;
      }
      break;
    case kDisconnectReq:
      if (body.size() >= 4) {
        if (IshtpConnection* c = connection_for(body[1], body[2])) c->connected = false;
        const uint8_t answer[4] = {kDisconnectRes, body[1], body[2], 0};
        send_hbm(ConstByteView(answer, 4));
      }
      break;
    case kFlowControl:
      if (body.size() >= 3)
        if (IshtpConnection* c = connection_for(body[1], body[2])) ++c->credits;
      break;
    case kFwStopReq:
      for (uint64_t i = 0; i < connections_.count(); ++i) connections_[i]->connected = false;
      break;
    default: break;
  }
}

void IshtpBus::handle(const IshMessage& message) {
  if (message.length < 4) return;
  const uint8_t fw = message.data[0], host = message.data[1];
  const uint32_t length = (message.data[2] | (message.data[3] << 8)) & 0x1FF;
  const bool complete = message.data[3] & 0x80;
  if (length + 4 > message.length) return;                                           // a header that claims more than came
  const ConstByteView payload(message.data + 4, length);
  if (fw == 0 && host == 0) handle_hbm(payload);
  else handle_client_piece(fw, host, complete, payload);
}

void IshtpBus::poll() {
  IshMessage m;
  while (ipc_.receive(&m)) handle(m);
  if (ipc_.take_reset_seen()) {
    reset_ = true;
    for (uint64_t i = 0; i < connections_.count(); ++i) connections_[i]->connected = false;
  }
}

bool IshtpBus::take(IshtpConnection& c, ByteBuffer* out) {
  if (c.messages.count() == 0) return false;
  out->clear();
  out->append(c.messages[0]->view());
  c.messages.remove_at(0);
  return true;
}

bool IshtpBus::take_reset() {
  const bool r = reset_;
  reset_ = false;
  return r;
}

}  // namespace hardware
