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

// WebSocket (RFC 6455) framing for the HTTP client: the opening handshake's key and accept value, frames a client sends (always masked), and a reader
// that takes the bytes a server sent and hands out its messages. The HttpClient (Network/Http/HttpClient.hpp) does the HTTP side and the connection;
// nothing here touches a socket.
//
//   - A reader checks everything RFC 6455 says a client must: server frames are not masked, reserved bits are clear (except RSV1 when
//     permessage-deflate was agreed), control frames are short and not fragmented, data frames come in the right order, a text message and a close reason
//     are valid UTF-8, and a close code is one that may be sent. Anything else ends the connection with the close code to answer with.
//   - permessage-deflate (RFC 7692) is understood without context takeover: every message is compressed and decompressed on its own.
#pragma once
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace network {

enum class WsOpcode : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// Close codes (RFC 6455 7.4.1).
constexpr uint16_t kWsNormal = 1000, kWsGoingAway = 1001, kWsProtocolError = 1002, kWsUnsupportedData = 1003, kWsNoStatus = 1005, kWsAbnormal = 1006,
                   kWsInvalidPayload = 1007, kWsPolicyViolation = 1008, kWsTooBig = 1009, kWsMissingExtension = 1010, kWsInternalError = 1011;

// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + GUID)). False on memory shortage.
bool ws_accept_value(StringView key, String* accept);
// Base64 of `bytes` appended to `out` (the key of a handshake is 16 random bytes in base64).
bool ws_base64(ConstByteView bytes, String* out);
// Is this well formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF)?
bool ws_utf8_valid(ConstByteView text);
// May this code appear in a Close frame (RFC 6455 7.4)?
bool ws_close_code_valid(uint16_t code);

// Appends one frame from a client: the header, the mask and the masked payload. `compressed` sets RSV1 (permessage-deflate). False on memory shortage.
bool ws_build_frame(WsOpcode opcode, bool fin, bool compressed, ConstByteView payload, const uint8_t mask[4], ByteBuffer* out);
// A Close frame's payload: the code (big-endian) and the reason (cut to 123 bytes).
bool ws_close_payload(uint16_t code, StringView reason, ByteBuffer* out);

struct WsEvent {
  enum class Kind : uint8_t {
    None,      // more input is needed
    Message,   // a whole text or binary message: payload, text
    Ping,      // payload (to be answered with a Pong carrying the same bytes)
    Pong,
    Close,     // the server's Close: code (kWsNoStatus if it gave none), reason in payload
    Failure,   // the server broke the rules: close with `code`
  };
  Kind kind = Kind::None;
  bool text = false;
  uint16_t code = 0;
  ConstByteView payload;   // valid until the next call of feed() or next()
};

class WsReader {
 public:
  explicit WsReader(allocators::IAllocator& allocator) : allocator_(allocator), in_(allocator), message_(allocator), inflated_(allocator) {}
  WsReader(const WsReader&) = delete;
  WsReader& operator=(const WsReader&) = delete;

  // The largest message accepted (after decompression); a bigger one is a Failure with kWsTooBig.
  void set_max_message(uint64_t bytes) { max_message_ = bytes; }
  // Frames with RSV1 are compressed messages (permessage-deflate was agreed).
  void set_deflate(bool on) { deflate_ = on; }

  // Takes bytes from the server. False on memory shortage.
  bool feed(ConstByteView data);
  // Takes the next thing out of the input; Kind::None when it is not all here yet. After a Failure or a Close there is nothing more.
  WsEvent next();
  // Bytes received and not processed yet.
  uint64_t buffered() const { return in_.size() - pos_; }

 private:
  WsEvent fail(uint16_t code) {
    dead_ = true;
    WsEvent e;
    e.kind = WsEvent::Kind::Failure;
    e.code = code;
    return e;
  }

  allocators::IAllocator& allocator_;
  ByteBuffer in_;
  uint64_t pos_ = 0;
  ByteBuffer message_;          // the data message being put together from its fragments
  ByteBuffer inflated_;
  bool in_message_ = false, message_text_ = false, message_compressed_ = false;
  bool deflate_ = false, dead_ = false;
  uint64_t max_message_ = 16ull << 20;
};

}  // namespace network
