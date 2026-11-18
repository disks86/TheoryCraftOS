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

#include "Hardware/Bluetooth/Sdp.hpp"

namespace hardware {
namespace sdp {

namespace {
constexpr uint8_t kErrorResponse = 0x01, kSearchAttributeRequest = 0x06, kSearchAttributeResponse = 0x07;
}

bool read_element(ByteReader& in, Element* out) {
  const uint8_t header = in.u8();
  if (!in.ok()) return false;
  const uint8_t type = header >> 3, size_index = header & 7;
  if (type > static_cast<uint8_t>(Type::Url)) return false;
  const Type t = static_cast<Type>(type);
  uint64_t length = 0;
  const bool variable = t == Type::Text || t == Type::Sequence || t == Type::Alternative || t == Type::Url;
  if (t == Type::Nil) {
    if (size_index != 0) return false;
  } else if (size_index <= 4) {
    if (variable) return false;
    length = uint64_t{1} << size_index;
    if (t == Type::Uuid && length != 2 && length != 4 && length != 16) return false;
    if (t == Type::Bool && length != 1) return false;
  } else {
    if (!variable) return false;
    length = size_index == 5 ? in.u8() : size_index == 6 ? in.be16() : in.be32();
    if (!in.ok()) return false;
  }
  if (length > in.remaining()) return false;
  out->type = t;
  out->data = in.take(length);
  return in.ok();
}

bool valid(ConstByteView data, uint32_t max_depth) {
  ByteReader in(data);
  while (!in.at_end()) {
    Element e;
    if (!read_element(in, &e)) return false;
    if (e.type == Type::Sequence || e.type == Type::Alternative) {
      if (max_depth == 0 || !valid(e.data, max_depth - 1)) return false;
    }
  }
  return true;
}

bool to_uint(const Element& element, uint32_t* out) {
  if (element.type != Type::Uint || element.data.size() > 4 || element.data.empty()) return false;
  uint32_t v = 0;
  for (uint8_t b : element.data) v = (v << 8) | b;
  *out = v;
  return true;
}

bool find_attribute(const Element& attribute_list, uint16_t id, Element* out) {
  if (attribute_list.type != Type::Sequence) return false;
  ByteReader in(attribute_list.data);
  while (!in.at_end()) {
    Element key, value;
    if (!read_element(in, &key) || !read_element(in, &value)) return false;
    uint32_t number = 0;
    if (key.data.size() != 2 || !to_uint(key, &number)) return false;     // (attribute ids are 16-bit unsigned integers)
    if (number == id) { *out = value; return true; }
  }
  return false;
}

bool hid_report_descriptor(const Element& descriptor_list, ConstByteView* out) {
  if (descriptor_list.type != Type::Sequence) return false;
  ByteReader in(descriptor_list.data);
  while (!in.at_end()) {
    Element entry;
    if (!read_element(in, &entry)) return false;
    if (entry.type != Type::Sequence) continue;
    ByteReader inner(entry.data);
    Element kind, descriptor;
    uint32_t kind_value = 0;
    if (!read_element(inner, &kind) || !read_element(inner, &descriptor) || !to_uint(kind, &kind_value)) continue;
    if (kind_value == 0x22 && (descriptor.type == Type::Text || descriptor.type == Type::Url) && !descriptor.data.empty()) { *out = descriptor.data; return true; }
  }
  return false;
}

bool find_in_records(ConstByteView attribute_lists, uint16_t id, Element* out) {
  ByteReader in(attribute_lists);
  Element all;
  if (!read_element(in, &all) || all.type != Type::Sequence) return false;
  ByteReader records(all.data);
  while (!records.at_end()) {
    Element record;
    if (!read_element(records, &record)) return false;
    if (find_attribute(record, id, out)) return true;
  }
  return false;
}

}  // namespace sdp

// ------------------------------------------------------------------ the client

void SdpClient::on_open(L2capChannel& channel) {
  if (channel.handle == handle_ && channel.local_cid == cid_) open_ = true;
}

void SdpClient::on_close(L2capChannel& channel) {
  if (channel.handle != handle_ || channel.local_cid != cid_) return;
  channel_ = nullptr;
  open_ = false;
  closed_ = true;
}

void SdpClient::on_data(L2capChannel& channel, ConstByteView sdu) {
  if (channel.handle != handle_ || channel.local_cid != cid_ || !waiting_ || sdu.size() < 5) return;
  const uint16_t transaction = static_cast<uint16_t>((sdu[1] << 8) | sdu[2]);
  if (transaction != transaction_ || sdu.size() > sizeof answer_) return;     // (an answer to something else, or late: dropped)
  ByteView(answer_).first(sdu.size()).copy_from(sdu);
  answer_length_ = static_cast<uint32_t>(sdu.size());
  have_answer_ = true;
}

bool SdpClient::channel_settled(void* self) {
  const SdpClient& c = *static_cast<SdpClient*>(self);
  return c.open_ || c.closed_;
}

bool SdpClient::answered(void* self) {
  const SdpClient& c = *static_cast<SdpClient*>(self);
  return c.have_answer_ || c.closed_;
}

void SdpClient::close() {
  if (channel_ && !closed_) {
    L2capChannel* channel = channel_;
    channel_ = nullptr;
    if (channel->state != L2capState::Closed && channel->handle == handle_ && channel->local_cid == cid_) l2cap_.close(*channel);
  }
  channel_ = nullptr;
  open_ = false;
}

SdpResult SdpClient::search_attributes(uint16_t acl_handle, uint16_t service_class, ByteBuffer& out, uint32_t timeout_ms) {
  out.clear();
  error_code_ = 0;
  if (!controller_.link(acl_handle)) return SdpResult::NoLink;
  if (channel_ && handle_ != acl_handle) close();
  if (!channel_ || !open_) {
    close();
    closed_ = open_ = false;
    channel_ = l2cap_.connect(acl_handle, sdp::kPsm, this);
    if (!channel_) return SdpResult::Refused;
    handle_ = acl_handle;
    cid_ = channel_->local_cid;
    controller_.wait_for(&SdpClient::channel_settled, this, timeout_ms);
    if (!open_) {
      const bool refused = closed_;
      close();
      return refused ? SdpResult::Refused : SdpResult::Timeout;
    }
  }
  uint8_t continuation[sdp::kMaxContinuation] = {};
  uint32_t continuation_length = 0;
  for (uint32_t round = 0; round < kMaxRounds; ++round) {
    uint8_t pdu[5 + 5 + 2 + 7 + 1 + sdp::kMaxContinuation];
    ByteWriter w{ByteView(pdu)};
    ++transaction_;
    w.u8(sdp::kSearchAttributeRequest);
    w.be16(transaction_);
    w.be16(static_cast<uint16_t>(5 + 2 + 7 + 1 + continuation_length));
    w.u8(0x35); w.u8(3); w.u8(0x19); w.be16(service_class);              // the search pattern: one 16-bit UUID
    w.be16(kMaxAttributeBytes);
    w.u8(0x35); w.u8(5); w.u8(0x0A); w.be32(0x0000FFFF);                  // every attribute (one range, 0 to 0xFFFF)
    w.u8(static_cast<uint8_t>(continuation_length));
    w.bytes(ConstByteView(continuation).first(continuation_length));
    if (!w.ok()) return SdpResult::Error;
    have_answer_ = false;
    waiting_ = true;
    if (!channel_ || l2cap_.send(*channel_, w.written()) != HciResult::Ok) { waiting_ = false; return closed_ ? SdpResult::NoLink : SdpResult::Error; }
    controller_.wait_for(&SdpClient::answered, this, timeout_ms);
    waiting_ = false;
    if (!have_answer_) return closed_ ? SdpResult::NoLink : SdpResult::Timeout;

    ByteReader in(ConstByteView(answer_).first(answer_length_));
    const uint8_t id = in.u8();
    in.skip(2);
    const uint16_t parameters = in.be16();
    if (!in.ok() || parameters != in.remaining()) return SdpResult::Malformed;
    if (id == sdp::kErrorResponse) {
      error_code_ = in.be16();
      return SdpResult::Error;
    }
    if (id != sdp::kSearchAttributeResponse) return SdpResult::Malformed;
    const uint16_t count = in.be16();
    const ConstByteView lists = in.take(count);
    const uint8_t next = in.u8();
    const ConstByteView state = in.take(next);
    if (!in.ok() || !in.at_end() || next > sdp::kMaxContinuation || count > kMaxAttributeBytes) return SdpResult::Malformed;
    if (lists.size() > kMaxAnswer - out.size()) return SdpResult::TooLarge;
    if (!out.append(lists)) return SdpResult::TooLarge;
    if (next == 0) return sdp::valid(out.view()) ? SdpResult::Ok : SdpResult::Malformed;
    ByteView(continuation).first(next).copy_from(state);
    continuation_length = next;
  }
  return SdpResult::TooLarge;
}

}  // namespace hardware
