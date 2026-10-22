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

#include "Network/Http/WebSocket.hpp"
#include "Crypto/Crypto.hpp"
#include "Media/Zlib.hpp"

namespace network {

namespace {

constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

}  // namespace

bool ws_base64(ConstByteView bytes, String* out) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (uint64_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t b0 = bytes[i], b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0, b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    if (!out->push_back(kAlphabet[(v >> 18) & 63]) || !out->push_back(kAlphabet[(v >> 12) & 63]) || !out->push_back(i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=') ||
        !out->push_back(i + 2 < bytes.size() ? kAlphabet[v & 63] : '='))
      return false;
  }
  return true;
}

bool ws_accept_value(StringView key, String* accept) {
  crypto::Hasher sha(crypto::HashKind::Sha1);
  sha.update(ConstByteView(reinterpret_cast<const uint8_t*>(key.data()), key.size()));
  sha.update(ConstByteView(reinterpret_cast<const uint8_t*>(kGuid), sizeof kGuid - 1));
  uint8_t digest[20];
  sha.finish(digest);
  accept->clear();
  return ws_base64(ConstByteView(digest), accept);
}

bool ws_utf8_valid(ConstByteView text) {
  uint64_t i = 0;
  while (i < text.size()) {
    const uint8_t c = text[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    uint32_t extra, minimum, cp;
    if (c >= 0xC2 && c <= 0xDF) { extra = 1; minimum = 0x80; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { extra = 2; minimum = 0x800; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { extra = 3; minimum = 0x10000; cp = c & 0x07; }
    else return false;
    if (i + extra >= text.size()) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      const uint8_t d = text[i + k];
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

bool ws_close_code_valid(uint16_t code) {
  if (code >= 3000 && code <= 4999) return true;
  return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014);
}

bool ws_build_frame(WsOpcode opcode, bool fin, bool compressed, ConstByteView payload, const uint8_t mask[4], ByteBuffer* out) {
  const uint64_t n = payload.size();
  bool ok = out->put8(static_cast<uint8_t>((fin ? 0x80 : 0) | (compressed ? 0x40 : 0) | static_cast<uint8_t>(opcode)));
  if (n < 126) {
    ok = ok && out->put8(static_cast<uint8_t>(0x80 | n));
  } else if (n <= 0xFFFF) {
    ok = ok && out->put8(0x80 | 126) && out->put8(static_cast<uint8_t>(n >> 8)) && out->put8(static_cast<uint8_t>(n));
  } else {
    ok = ok && out->put8(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8) ok = ok && out->put8(static_cast<uint8_t>(n >> shift));
  }
  ok = ok && out->put8(mask[0]) && out->put8(mask[1]) && out->put8(mask[2]) && out->put8(mask[3]);
  for (uint64_t i = 0; ok && i < n; ++i) ok = out->put8(static_cast<uint8_t>(payload[i] ^ mask[i & 3]));
  return ok;
}

bool ws_close_payload(uint16_t code, StringView reason, ByteBuffer* out) {
  if (reason.size() > 123) reason = reason.first(123);
  bool ok = out->put8(static_cast<uint8_t>(code >> 8)) && out->put8(static_cast<uint8_t>(code));
  for (uint64_t i = 0; ok && i < reason.size(); ++i) ok = out->put8(static_cast<uint8_t>(reason[i]));
  return ok;
}

bool WsReader::feed(ConstByteView data) {
  if (dead_) return true;
  // drop what was processed: all of it is the usual case, a long processed prefix is copied away
  if (pos_ > 0 && (pos_ == in_.size() || pos_ >= (64u << 10))) {
    ByteBuffer rest(allocator_);
    if (pos_ < in_.size() && !rest.append(in_.view().subview(pos_))) return false;
    in_.clear();
    if (!in_.append(rest.view())) return false;
    pos_ = 0;
  }
  return in_.append(data);
}

WsEvent WsReader::next() {
  WsEvent none;
  if (dead_) return none;
  for (;;) {
    const ConstByteView all = in_.view().subview(pos_);
    if (all.size() < 2) return none;
    const uint8_t b0 = all[0], b1 = all[1];
    const bool fin = (b0 & 0x80) != 0, rsv1 = (b0 & 0x40) != 0, rsv2 = (b0 & 0x20) != 0, rsv3 = (b0 & 0x10) != 0;
    const uint8_t opcode = b0 & 0x0F;
    if ((b1 & 0x80) != 0 || rsv2 || rsv3) return fail(kWsProtocolError);   // a server must not mask; reserved bits mean an extension nobody agreed to
    uint64_t length = b1 & 0x7F;
    uint64_t header = 2;
    if (length == 126) {
      if (all.size() < 4) return none;
      length = (uint64_t{all[2]} << 8) | all[3];
      header = 4;
      if (length < 126) return fail(kWsProtocolError);   // (not the shortest form)
    } else if (length == 127) {
      if (all.size() < 10) return none;
      length = 0;
      for (int i = 0; i < 8; ++i) length = (length << 8) | all[2 + i];
      header = 10;
      if ((length >> 63) != 0 || length < 0x10000) return fail(kWsProtocolError);
    }
    const bool control = (opcode & 0x08) != 0;
    if (control) {
      if (!fin || length > 125 || rsv1) return fail(kWsProtocolError);
      if (opcode != 8 && opcode != 9 && opcode != 10) return fail(kWsProtocolError);
    } else {
      if (opcode > 2) return fail(kWsProtocolError);
      if (rsv1 && (!deflate_ || opcode == 0)) return fail(kWsProtocolError);
      if (length > max_message_ || message_.size() + length > max_message_) return fail(kWsTooBig);
    }
    if (all.size() < header + length) return none;
    const ConstByteView payload = all.subview(header, length);
    pos_ += header + length;
    WsEvent e;
    if (control) {
      e.payload = payload;
      if (opcode == 9) {
        e.kind = WsEvent::Kind::Ping;
      } else if (opcode == 10) {
        e.kind = WsEvent::Kind::Pong;
      } else {
        e.kind = WsEvent::Kind::Close;
        e.code = kWsNoStatus;
        if (payload.size() == 1) return fail(kWsProtocolError);
        if (payload.size() >= 2) {
          e.code = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
          e.payload = payload.subview(2);
          if (!ws_close_code_valid(e.code)) return fail(kWsProtocolError);
          if (!ws_utf8_valid(e.payload)) return fail(kWsInvalidPayload);
        } else {
          e.payload = ConstByteView();
        }
        dead_ = true;   // nothing follows a Close
      }
      return e;
    }
    // data
    if (opcode != 0) {
      if (in_message_) return fail(kWsProtocolError);   // a new message while one is unfinished
      in_message_ = true;
      message_text_ = opcode == 1;
      message_compressed_ = rsv1;
      message_.clear();
    } else if (!in_message_) {
      return fail(kWsProtocolError);                    // a continuation with nothing to continue
    }
    if (!message_.append(payload)) return fail(kWsInternalError);
    if (!fin) continue;
    in_message_ = false;
    ConstByteView data = message_.view();
    if (message_compressed_) {
      // each message stands alone: the data with the empty stored block that a sync flush ends with, inflated as raw deflate
      ByteBuffer with_tail(allocator_);
      static const uint8_t kTail[4] = {0x00, 0x00, 0xFF, 0xFF};
      inflated_.clear();
      if (!with_tail.append(data) || !with_tail.append(ConstByteView(kTail, 4))) return fail(kWsInternalError);
      if (!media::inflate_message(with_tail.view(), &inflated_, max_message_)) return fail(inflated_.size() >= max_message_ ? kWsTooBig : kWsInvalidPayload);
      data = inflated_.view();
    }
    if (message_text_ && !ws_utf8_valid(data)) return fail(kWsInvalidPayload);
    e.kind = WsEvent::Kind::Message;
    e.text = message_text_;
    e.payload = data;
    return e;
  }
}

}  // namespace network
