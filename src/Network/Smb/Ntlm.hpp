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

// NTLM authentication (MS-NLMP), the client side, NTLMv2 only: the three messages (negotiate, the server's challenge, authenticate) and the
// session key that SMB signs with. Also anonymous logins (no user, no password). The messages are the bare NTLMSSP tokens; wrapping them
// in SPNEGO is for the caller.
#pragma once
#include "Crypto/Random.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace network {

struct NtlmCredentials {
  StringView user;       // empty for an anonymous login
  StringView domain;     // may be empty
  StringView password;
  bool anonymous() const { return user.empty() && password.empty(); }
};

class NtlmClient {
 public:
  explicit NtlmClient(allocators::IAllocator& allocator) : allocator_(allocator), negotiate_(allocator) {}

  // The NEGOTIATE_MESSAGE to send first (appended to `out`).
  bool negotiate(ByteBuffer* out);
  // Given the server's CHALLENGE_MESSAGE, appends the AUTHENTICATE_MESSAGE to `out` and works out the session key. `now_filetime` is the time as a
  // Windows FILETIME (100 ns since 1601), used when the server did not state one; `workstation` is this machine's name. False if the
  // challenge is not valid.
  bool authenticate(ConstByteView challenge, const NtlmCredentials& credentials, StringView workstation, uint64_t now_filetime, crypto::IRandom& random,
                    ByteBuffer* out);

  // The key both sides know after authenticating (16 bytes); only if the login was not anonymous.
  bool has_session_key() const { return have_key_; }
  const uint8_t* session_key() const { return session_key_; }

  // --- Pieces, public for the tests
  // NTOWFv2: HMAC-MD5 of the user name (upper case) and domain under MD4 of the password.
  static bool ntowf_v2(allocators::IAllocator& allocator, const NtlmCredentials& credentials, uint8_t out[16]);
  // The NTLMv2 response for `target_info` (the AV pairs the server sent, empty for none) with the given client challenge and time: the 16-byte
  // proof, then the client blob. Appended to `out`. `session_base_key` gets HMAC-MD5(NTOWFv2, proof).
  static bool ntlm_v2_response(allocators::IAllocator& allocator, const uint8_t ntowf[16], const uint8_t server_challenge[8], const uint8_t client_challenge[8], uint64_t filetime,
                               ConstByteView target_info, ByteBuffer* out, uint8_t session_base_key[16]);

 private:
  allocators::IAllocator& allocator_;
  ByteBuffer negotiate_;
  bool have_key_ = false;
  uint8_t session_key_[16] = {};
};

}  // namespace network
