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

// The server's side of D-Bus authentication (the specification's SASL profile), before any message.
//
// The client sends one NUL byte (the credentials byte), then lines ending in "\r\n". The only mechanism offered is EXTERNAL: the client says which user it
// is (the uid in decimal, hex-encoded: "AUTH EXTERNAL 30" for root) and the bus believes it only when the transport's credentials of the connection say the
// same (SO_PEERCRED). "AUTH EXTERNAL" without that and an empty "DATA" afterwards (what sd-bus does) takes the transport's uid as it is. After "OK <guid>"
// the client may ask for descriptor passing (NEGOTIATE_UNIX_FD, agreed to when the transport can pass them) and ends with "BEGIN"; what follows that line
// is the first message. Clients send it all at once without waiting (sd-bus, GDBus), so each feed() may hold several lines and the start of the messages.
#pragma once
#include <stdint.h>
#include "Utility/ByteBuffer.hpp"
#include "Utility/StringView.hpp"

namespace dbus {

class AuthServer {
 public:
  static constexpr uint32_t kMaxLine = 16 * 1024;
  static constexpr uint32_t kMaxBytes = 64 * 1024;      // all of it, before BEGIN
  static constexpr uint32_t kMaxRejections = 8;

  enum class State : uint8_t { WaitingForNul, WaitingForAuth, WaitingForData, WaitingForBegin, Done, Failed };

  // `uid`: the user the transport says the other end is (~0: unknown, then a claimed uid is taken); `guid`: the bus's 32 hex digits; `can_pass_fds`:
  // whether the transport carries descriptors.
  AuthServer(allocators::IAllocator& allocator, uint32_t uid, StringView guid, bool can_pass_fds);

  // Takes bytes from the client and appends the replies to `out`. Returns how many of `bytes` were used: once the state is Done the rest are the messages'.
  uint64_t feed(ConstByteView bytes, ByteBuffer* out);
  State state() const { return state_; }
  bool done() const { return state_ == State::Done; }
  bool failed() const { return state_ == State::Failed; }
  bool unix_fds() const { return unix_fds_; }
  uint32_t uid() const { return authorized_uid_; }

 private:
  void line(StringView text, ByteBuffer* out);
  void external(StringView hex_response, bool have_response, ByteBuffer* out);
  void reject(ByteBuffer* out);
  void reply(StringView text, ByteBuffer* out);

  uint32_t uid_;
  uint32_t authorized_uid_ = ~0u;
  char guid_[33] = {};
  bool can_pass_fds_;
  bool unix_fds_ = false;
  State state_ = State::WaitingForNul;
  ByteBuffer pending_;      // a line not yet ended
  uint64_t total_ = 0;
  uint32_t rejections_ = 0;
};

}  // namespace dbus
