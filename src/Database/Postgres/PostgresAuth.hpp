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

// How a PostgreSQL client proves it knows the password: the old MD5 scheme and SCRAM-SHA-256 (RFC 5802 / RFC 7677, the default since PostgreSQL 14).
#pragma once
#include "Database/DatabaseTypes.hpp"

namespace database::postgres {

// "md5" and 32 hex digits: md5(md5(password + user) + salt), what a PasswordMessage carries when the server asks for MD5.
bool md5_password(StringView user, StringView password, ConstByteView salt, String* out);

// Base64 (standard alphabet, padded) of `data`, appended to `out`.
bool append_base64(String* out, ConstByteView data);

// The client's side of SCRAM-SHA-256. Use: start() gives the first message to send; the server's reply goes to handle_server_first(), which gives the
// final message; the server's last message goes to verify_server_final(), which is how the client knows the server also knew the password.
// PostgreSQL takes the user name from the startup message, so `user` is normally left empty (as libpq does).
class ScramClient {
 public:
  static constexpr uint32_t kMaxIterations = 1000000;

  explicit ScramClient(allocators::IAllocator& allocator) : client_first_bare_(allocator), server_first_(allocator), password_(allocator), nonce_(allocator), expected_(allocator), message_(allocator) {}

  // `nonce` is the client's random text (printable characters without commas; at least 18 random bytes as base64 for a real login).
  bool start(StringView user, StringView password, StringView nonce, String* client_first);
  // Reads "r=...,s=...,i=..." and writes "c=biws,r=...,p=...". False if the server's message is not valid, does not continue the client's nonce, or
  // asks for too few or too many iterations.
  bool handle_server_first(StringView server_first, String* client_final);
  // Checks "v=..." against the signature only the holder of the password could make.
  bool verify_server_final(StringView server_final);

 private:
  String client_first_bare_, server_first_, password_, nonce_;
  String expected_;   // "v=" and the signature the server must send back
  String message_;    // the AuthMessage the signatures are made over
  bool ready_ = false;
};

}  // namespace database::postgres
