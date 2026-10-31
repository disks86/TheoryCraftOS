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

#include "Hardware/Bluetooth/Att.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace att;

AttClient::AttClient(HciController& controller, L2cap& l2cap) : controller_(controller), l2cap_(l2cap) { l2cap_.register_fixed(L2cap::kCidAtt, this); }

AttClient::~AttClient() {}

AttClient::State* AttClient::state_for(uint16_t link, bool create) {
  State* free_slot = nullptr;
  for (State& s : states_) {
    if (s.used && s.link == link) return &s;
    if (!s.used && !free_slot) free_slot = &s;
  }
  if (!create || !free_slot) return nullptr;
  *free_slot = State();
  free_slot->used = true;
  free_slot->link = link;
  return free_slot;
}

uint16_t AttClient::mtu(uint16_t link) const {
  for (const State& s : states_)
    if (s.used && s.link == link) return s.mtu;
  return kDefaultMtu;
}

void AttClient::on_link(const HciLink& link, bool up) {
  if (up) return;
  for (State& s : states_)
    if (s.used && s.link == link.handle) s.used = false;
  if (waiting_ && waiting_link_ == link.handle) { waiting_ = false; answered_ = false; }
}

void AttClient::on_data(const HciLink& link, ConstByteView sdu) {
  if (sdu.empty()) return;
  const uint8_t opcode = sdu[0];
  if (opcode == kHandleValueNotification || opcode == kHandleValueIndication) {
    if (sdu.size() < 3) return;
    const uint16_t handle = static_cast<uint16_t>(sdu[1] | (sdu[2] << 8));
    if (opcode == kHandleValueIndication) {
      const uint8_t confirm = kHandleValueConfirmation;
      l2cap_.send_fixed(link.handle, L2cap::kCidAtt, ConstByteView(&confirm, 1));
    }
    if (notify_) notify_(notify_context_, link.handle, handle, sdu.dropped(3), opcode == kHandleValueIndication);
    return;
  }
  if (!waiting_ || waiting_link_ != link.handle || answered_) return;     // (an answer to nothing we asked, or a request of the peer: a client has no server)
  if (opcode == kErrorResponse) {
    if (sdu.size() < 5 || sdu[1] != waiting_request_opcode_) return;
    errored_ = true;
    error_code_ = sdu[4];
    error_handle_ = static_cast<uint16_t>(sdu[2] | (sdu[3] << 8));
    answered_ = true;
    return;
  }
  if (opcode != waiting_expected_) return;
  const uint64_t n = sdu.size() < sizeof response_ ? sdu.size() : sizeof response_;
  for (uint64_t i = 0; i < n; ++i) response_[i] = sdu[i];
  response_size_ = static_cast<uint32_t>(n);
  answered_ = true;
}

AttStatus AttClient::send_and_wait(uint16_t link, ConstByteView pdu, uint8_t expected, uint32_t timeout_ms) {
  waiting_ = true;
  answered_ = false;
  errored_ = false;
  waiting_link_ = link;
  waiting_expected_ = expected;
  waiting_request_opcode_ = pdu[0];
  response_size_ = 0;
  if (l2cap_.send_fixed(link, L2cap::kCidAtt, pdu) != HciResult::Ok) { waiting_ = false; return AttStatus::NoLink; }
  const uint64_t limit = uint64_t{timeout_ms} * 10;
  for (uint64_t round = 0; round < limit && waiting_ && !answered_; ++round)
    if (controller_.poll() == 0) delay_us(100);
  const bool got = waiting_ && answered_;
  waiting_ = false;
  if (!got) return controller_.link(link) ? AttStatus::Timeout : AttStatus::NoLink;
  return errored_ ? AttStatus::Error : AttStatus::Ok;
}

AttStatus AttClient::request(uint16_t link, ConstByteView pdu, uint8_t expected, uint32_t timeout_ms) {
  if (pdu.empty() || !controller_.link(link)) return AttStatus::NoLink;
  if (waiting_) return AttStatus::Busy;
  AttStatus status = send_and_wait(link, pdu, expected, timeout_ms);
  if (status == AttStatus::Error && security_ && (error_code_ == kInsufficientAuthentication || error_code_ == kInsufficientEncryption || error_code_ == kInsufficientKeySize)) {
    if (security_(security_context_, link, error_code_)) status = send_and_wait(link, pdu, expected, timeout_ms);   // (once more, now that the link is secure)
  }
  return status;
}

AttStatus AttClient::command(uint16_t link, ConstByteView pdu) {
  if (pdu.empty() || !controller_.link(link)) return AttStatus::NoLink;
  return l2cap_.send_fixed(link, L2cap::kCidAtt, pdu) == HciResult::Ok ? AttStatus::Ok : AttStatus::NoLink;
}

AttStatus AttClient::exchange_mtu(uint16_t link, uint16_t wanted) {
  if (wanted > kMaxMtu) wanted = kMaxMtu;
  if (wanted < kDefaultMtu) wanted = kDefaultMtu;
  const uint8_t pdu[3] = {kMtuRequest, static_cast<uint8_t>(wanted), static_cast<uint8_t>(wanted >> 8)};
  const AttStatus status = request(link, ConstByteView(pdu, 3), kMtuResponse);
  if (status != AttStatus::Ok) return status;
  if (response_size_ < 3) return AttStatus::Malformed;
  uint16_t server = static_cast<uint16_t>(response_[1] | (response_[2] << 8));
  if (server < kDefaultMtu) server = kDefaultMtu;
  State* s = state_for(link, true);
  if (s) s->mtu = server < wanted ? server : wanted;
  return AttStatus::Ok;
}

AttStatus AttClient::read(uint16_t link, uint16_t handle, ByteView out, uint32_t* length) {
  *length = 0;
  const uint16_t step = static_cast<uint16_t>(mtu(link) - 1);
  uint32_t have = 0;
  for (uint32_t blob = 0; blob < 64; ++blob) {
    uint8_t pdu[5];
    ByteWriter w{ByteView(pdu)};
    if (blob == 0) {
      w.u8(kReadRequest);
      w.le16(handle);
    } else {
      w.u8(kReadBlobRequest);
      w.le16(handle);
      w.le16(static_cast<uint16_t>(have));
    }
    const AttStatus status = request(link, w.written(), blob == 0 ? kReadResponse : kReadBlobResponse);
    if (status != AttStatus::Ok) {
      if (blob > 0 && status == AttStatus::Error && (error_code_ == kInvalidOffset || error_code_ == kAttributeNotLong)) break;      // (the value ended exactly at the end of the last blob)
      return status;
    }
    const uint32_t got = response_size_ > 1 ? response_size_ - 1 : 0;
    for (uint32_t i = 0; i < got && have + i < out.size(); ++i) out[have + i] = response_[1 + i];
    have += got;
    if (got < step || have >= out.size()) break;                      // (a short answer is the last)
  }
  *length = have < out.size() ? have : static_cast<uint32_t>(out.size());
  return AttStatus::Ok;
}

AttStatus AttClient::write(uint16_t link, uint16_t handle, ConstByteView value) {
  if (value.size() > mtu(link) - 3u) return AttStatus::Malformed;
  uint8_t pdu[att::kMaxMtu];
  ByteWriter w{ByteView(pdu)};
  w.u8(kWriteRequest);
  w.le16(handle);
  w.bytes(value);
  return request(link, w.written(), kWriteResponse);
}

AttStatus AttClient::write_without_response(uint16_t link, uint16_t handle, ConstByteView value) {
  if (value.size() > mtu(link) - 3u) return AttStatus::Malformed;
  uint8_t pdu[att::kMaxMtu];
  ByteWriter w{ByteView(pdu)};
  w.u8(kWriteCommand);
  w.le16(handle);
  w.bytes(value);
  return command(link, w.written());
}

}  // namespace hardware
