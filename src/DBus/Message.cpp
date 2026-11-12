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

#include "DBus/Message.hpp"
#include "DBus/Wire.hpp"

namespace dbus {

namespace {

uint32_t read32(ConstByteView b, uint64_t at, bool big_endian) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) v |= uint32_t(b[at + (big_endian ? 3 - i : i)]) << (8 * i);
  return v;
}

bool endian_of(uint8_t marker, bool* big_endian) {
  if (marker == 'l') { *big_endian = false; return true; }
  if (marker == 'B') { *big_endian = true; return true; }
  return false;
}

}  // namespace

bool Message::copy_to(Message* out) const {
  out->big_endian = big_endian;
  out->type = type;
  out->flags = flags;
  out->serial = serial;
  out->has_reply_serial = has_reply_serial;
  out->reply_serial = reply_serial;
  out->unix_fds = unix_fds;
  out->body.clear();
  out->fds.clear();
  if (!out->path.assign(path) || !out->interface.assign(interface) || !out->member.assign(member) || !out->error_name.assign(error_name) ||
      !out->destination.assign(destination) || !out->sender.assign(sender) || !out->signature.assign(signature) || !out->body.append(body.view()))
    return false;
  for (const FdRef& fd : fds.view())
    if (!out->fds.push_back(fd)) return false;
  return true;
}

int64_t framed_size(ConstByteView prefix) {
  if (prefix.size() < 16) return 0;
  bool big_endian = false;
  if (!endian_of(prefix[0], &big_endian) || prefix[3] != 1) return -1;
  const uint64_t body = read32(prefix, 4, big_endian);
  const uint64_t fields = read32(prefix, 12, big_endian);
  if (fields > kMaxArrayBytes) return -1;
  uint64_t header = 16 + fields;
  header += (8 - header % 8) % 8;
  if (body > kMaxMessageBytes || header + body > kMaxMessageBytes) return -1;
  return int64_t(header + body);
}

DecodeStatus decode(ConstByteView bytes, Message* m) {
  const int64_t size = framed_size(bytes);
  if (size <= 0 || uint64_t(size) != bytes.size()) return DecodeStatus::Invalid;
  bool big_endian = false;
  endian_of(bytes[0], &big_endian);
  Reader r(bytes, big_endian);
  uint8_t marker = 0, version = 0;
  uint32_t body_length = 0;
  r.u8(&marker);
  r.u8(&m->type);
  r.u8(&m->flags);
  r.u8(&version);
  r.u32(&body_length);
  r.u32(&m->serial);
  m->big_endian = big_endian;
  if (m->type < kMethodCall || m->type > kSignal || m->serial == 0) return DecodeStatus::Invalid;
  m->path.clear(); m->interface.clear(); m->member.clear(); m->error_name.clear(); m->destination.clear(); m->sender.clear(); m->signature.clear();
  m->has_reply_serial = false;
  m->reply_serial = 0;
  m->unix_fds = 0;
  m->fds.clear();
  uint64_t fields_end = 0;
  if (!r.begin_array(8, &fields_end)) return DecodeStatus::Invalid;
  uint32_t seen = 0;
  while (r.position() < fields_end) {
    uint8_t code = 0;
    StringView type;
    if (!r.align(8) || !r.u8(&code) || !r.signature(&type)) return DecodeStatus::Invalid;
    if (code == 0) return DecodeStatus::Invalid;
    if (code > kFieldUnixFds) {      // (one the specification does not name: checked and dropped)
      if (!read_value(r, type, UINT32_MAX)) return DecodeStatus::Invalid;
      continue;
    }
    if (seen & (1u << code)) return DecodeStatus::Invalid;
    seen |= 1u << code;
    StringView text;
    bool ok = true;
    switch (code) {
      case kFieldPath: ok = type == "o" && r.object_path(&text) && m->path.assign(text); break;
      case kFieldInterface: ok = type == "s" && r.string(&text) && valid_interface_name(text) && m->interface.assign(text); break;
      case kFieldMember: ok = type == "s" && r.string(&text) && valid_member_name(text) && m->member.assign(text); break;
      case kFieldErrorName: ok = type == "s" && r.string(&text) && valid_interface_name(text) && m->error_name.assign(text); break;
      case kFieldDestination: ok = type == "s" && r.string(&text) && valid_bus_name(text) && m->destination.assign(text); break;
      case kFieldSender: ok = type == "s" && r.string(&text) && valid_bus_name(text) && m->sender.assign(text); break;
      case kFieldSignature: ok = type == "g" && r.signature(&text) && m->signature.assign(text); break;
      case kFieldReplySerial: ok = type == "u" && r.u32(&m->reply_serial) && m->reply_serial != 0; m->has_reply_serial = true; break;
      case kFieldUnixFds: ok = type == "u" && r.u32(&m->unix_fds); break;
    }
    if (!ok) return DecodeStatus::Invalid;
  }
  if (r.position() != fields_end || !r.align(8) || r.failed()) return DecodeStatus::Invalid;
  if (bytes.size() - r.position() != body_length) return DecodeStatus::Invalid;
  switch (m->type) {
    case kMethodCall: if (!(seen & (1u << kFieldPath)) || !(seen & (1u << kFieldMember))) return DecodeStatus::Invalid; break;
    case kMethodReturn: if (!m->has_reply_serial) return DecodeStatus::Invalid; break;
    case kError: if (!m->has_reply_serial || m->error_name.empty()) return DecodeStatus::Invalid; break;
    case kSignal: if (!(seen & (1u << kFieldPath)) || m->interface.empty() || m->member.empty()) return DecodeStatus::Invalid; break;
  }
  const ConstByteView body = bytes.subview(r.position(), body_length);
  if (!validate_body(body, m->signature.view(), big_endian, m->unix_fds)) return DecodeStatus::Invalid;
  m->body.clear();
  if (!m->body.append(body)) return DecodeStatus::NoMemory;
  return DecodeStatus::Ok;
}

bool encode(const Message& m, ByteBuffer* out) {
  if (m.body.size() > kMaxMessageBytes) return false;
  const uint64_t start = out->size();
  Writer w(*out, m.big_endian);
  w.u8(m.big_endian ? 'B' : 'l');
  w.u8(m.type);
  w.u8(m.flags);
  w.u8(1);
  w.u32(uint32_t(m.body.size()));
  w.u32(m.serial);
  const Writer::ArrayMark fields = w.begin_array(8);
  auto text = [&](uint8_t code, const char* type, StringView value) {
    if (value.empty()) return;
    w.begin_struct();
    w.u8(code);
    w.variant(type);
    if (type[0] == 'g') w.signature(value);
    else w.string(value);
  };
  auto number = [&](uint8_t code, uint32_t value) {
    w.begin_struct();
    w.u8(code);
    w.variant("u");
    w.u32(value);
  };
  text(kFieldPath, "o", m.path.view());
  text(kFieldInterface, "s", m.interface.view());
  text(kFieldMember, "s", m.member.view());
  text(kFieldErrorName, "s", m.error_name.view());
  if (m.has_reply_serial) number(kFieldReplySerial, m.reply_serial);
  text(kFieldDestination, "s", m.destination.view());
  text(kFieldSender, "s", m.sender.view());
  text(kFieldSignature, "g", m.signature.view());
  if (m.unix_fds) number(kFieldUnixFds, m.unix_fds);
  w.end_array(fields);
  w.pad(8);
  w.raw(m.body.view());
  if (!w.ok() || out->size() - start > kMaxMessageBytes) {
    out->truncate(start);
    return false;
  }
  return true;
}

namespace {
void reset(Message* m, uint8_t type) {
  m->big_endian = false;
  m->type = type;
  m->flags = kNoReplyExpected;
  m->serial = 0;
  m->path.clear(); m->interface.clear(); m->member.clear(); m->error_name.clear(); m->destination.clear(); m->sender.clear(); m->signature.clear();
  m->has_reply_serial = false;
  m->reply_serial = 0;
  m->unix_fds = 0;
  m->body.clear();
  m->fds.clear();
}
}  // namespace

bool make_reply(const Message& call, Message* out) {
  reset(out, kMethodReturn);
  out->has_reply_serial = true;
  out->reply_serial = call.serial;
  return out->destination.assign(call.sender.view());
}

bool make_error(const Message& call, StringView error_name, StringView text, Message* out) {
  reset(out, kError);
  out->has_reply_serial = true;
  out->reply_serial = call.serial;
  if (!out->destination.assign(call.sender.view()) || !out->error_name.assign(error_name) || !out->signature.assign("s")) return false;
  Writer w(out->body);
  w.string(text);
  return w.ok();
}

bool make_signal(StringView path, StringView interface, StringView member, Message* out) {
  reset(out, kSignal);
  return out->path.assign(path) && out->interface.assign(interface) && out->member.assign(member);
}

}  // namespace dbus
