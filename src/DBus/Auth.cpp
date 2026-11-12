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

#include "DBus/Auth.hpp"

namespace dbus {

namespace {

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// The uid an EXTERNAL response names: hex digits of the decimal digits. False if it is not one.
bool decode_uid(StringView hex, uint32_t* uid) {
  if (hex.empty() || hex.size() % 2 != 0 || hex.size() > 20) return false;
  uint64_t value = 0;
  for (uint64_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_digit(hex[i]), lo = hex_digit(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    const char c = char(hi * 16 + lo);
    if (c < '0' || c > '9') return false;
    value = value * 10 + uint64_t(c - '0');
    if (value > 0xFFFFFFFEull) return false;
  }
  *uid = uint32_t(value);
  return true;
}

// The first word of a line and what follows it (after one space).
void split_command(StringView text, StringView* command, StringView* rest) {
  const uint64_t space = text.find(' ');
  if (space == StringView::npos) {
    *command = text;
    *rest = StringView();
  } else {
    *command = text.first(space);
    *rest = text.without_prefix(space + 1);
  }
}

}  // namespace

AuthServer::AuthServer(allocators::IAllocator& allocator, uint32_t uid, StringView guid, bool can_pass_fds)
    : uid_(uid), can_pass_fds_(can_pass_fds), pending_(allocator) {
  guid.first(guid.size() < 32 ? guid.size() : 32).copy_to(ArrayView<char>(guid_, 33));
}

void AuthServer::reply(StringView text, ByteBuffer* out) {
  if (!out->append(text.bytes()) || !out->append(StringView("\r\n").bytes())) state_ = State::Failed;
}

void AuthServer::reject(ByteBuffer* out) {
  reply("REJECTED EXTERNAL", out);
  if (++rejections_ >= kMaxRejections) state_ = State::Failed;
  else if (state_ != State::Failed) state_ = State::WaitingForAuth;
}

void AuthServer::external(StringView hex, bool have_response, ByteBuffer* out) {
  uint32_t claimed = 0;
  if (have_response && !hex.empty()) {
    if (!decode_uid(hex, &claimed) || (uid_ != ~0u && claimed != uid_)) { reject(out); return; }
  } else {
    if (uid_ == ~0u) { reject(out); return; }      // (nothing claimed and nothing known)
    claimed = uid_;
  }
  authorized_uid_ = claimed;
  char text[3 + 32 + 1] = {'O', 'K', ' '};
  StringView(guid_).copy_to(ArrayView<char>(text + 3, 33));
  reply(StringView(text), out);
  if (state_ != State::Failed) state_ = State::WaitingForBegin;
}

void AuthServer::line(StringView text, ByteBuffer* out) {
  StringView command, rest;
  split_command(text, &command, &rest);
  switch (state_) {
    case State::WaitingForAuth:
      if (command == "AUTH") {
        StringView mechanism, response;
        split_command(rest, &mechanism, &response);
        if (mechanism == "EXTERNAL") {
          if (rest.find(' ') == StringView::npos) {      // no initial response: ask for it
            reply("DATA", out);
            if (state_ != State::Failed) state_ = State::WaitingForData;
          } else {
            external(response, true, out);
          }
        } else {
          reject(out);      // (no mechanism named: the list; any other: not offered)
        }
      } else if (command == "BEGIN") {
        state_ = State::Failed;
      } else if (command == "ERROR" || command == "CANCEL" || command == "DATA") {
        reject(out);
      } else {
        reply("ERROR \"Unknown command\"", out);
      }
      return;
    case State::WaitingForData:
      if (command == "DATA") external(rest, true, out);
      else if (command == "BEGIN") state_ = State::Failed;
      else if (command == "CANCEL" || command == "ERROR" || command == "AUTH") reject(out);
      else reply("ERROR \"Expected DATA\"", out);
      return;
    case State::WaitingForBegin:
      if (command == "BEGIN") {
        state_ = State::Done;
      } else if (command == "NEGOTIATE_UNIX_FD") {
        if (can_pass_fds_) {
          unix_fds_ = true;
          reply("AGREE_UNIX_FD", out);
        } else {
          reply("ERROR \"Descriptors cannot be passed on this connection\"", out);
        }
      } else if (command == "CANCEL" || command == "ERROR") {
        unix_fds_ = false;
        authorized_uid_ = ~0u;
        reject(out);
      } else {
        reply("ERROR \"Expected BEGIN\"", out);
      }
      return;
    default: return;
  }
}

uint64_t AuthServer::feed(ConstByteView bytes, ByteBuffer* out) {
  uint64_t used = 0;
  while (used < bytes.size() && state_ != State::Done && state_ != State::Failed) {
    if (state_ == State::WaitingForNul) {
      if (bytes[used++] != 0) { state_ = State::Failed; break; }
      state_ = State::WaitingForAuth;
      ++total_;
      continue;
    }
    const uint8_t c = bytes[used++];
    if (++total_ > kMaxBytes) { state_ = State::Failed; break; }
    if (c == '\n') {
      const uint64_t n = pending_.size();
      if (n == 0 || pending_.view()[n - 1] != '\r') { state_ = State::Failed; break; }
      const StringView text(pending_.view().first(n - 1));
      for (char ch : text)
        if (ch < 0x20 || ch > 0x7E) { state_ = State::Failed; break; }
      if (state_ == State::Failed) break;
      line(text, out);
      pending_.clear();
      continue;
    }
    if (pending_.size() >= kMaxLine || !pending_.put8(c)) { state_ = State::Failed; break; }
  }
  return used;
}

}  // namespace dbus
