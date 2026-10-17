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

#pragma once
#include "Crypto/Crypto.hpp"
#include "Network/Snmp/SnmpMessage.hpp"

namespace network {

// The user-based security model of SNMPv3 (RFC 3414, with AES from RFC 3826 and the SHA-2 authentication of RFC 7860):
//   authentication  HMAC-MD5-96, HMAC-SHA-96 (the originals), HMAC-SHA-224 / -256 with tags of 16 / 24 bytes,
//   privacy         CBC-DES and CFB-AES-128.
// A user's passwords become keys that depend on the agent's engine ID ("localization"), so the keys are made once the engine
// ID is known (UsmKeys, from usm_make_keys()), and each message is protected by encode_snmp_v3() / checked by decode_snmp_v3().
enum class UsmAuth : uint8_t { None, HmacMd5, HmacSha1, HmacSha224, HmacSha256 };
enum class UsmPriv : uint8_t { None, Des, Aes128 };

// The length of the (truncated) authentication tag: 12, 12, 16, 24 bytes.
uint32_t usm_auth_length(UsmAuth auth);
crypto::HashKind usm_hash_kind(UsmAuth auth);

struct UsmKeys {
  UsmAuth auth = UsmAuth::None;
  UsmPriv priv = UsmPriv::None;
  uint8_t auth_key[32] = {};
  uint32_t auth_key_length = 0;
  uint8_t priv_key[32] = {};
  uint32_t priv_key_length = 0;
};

// Password to key to localized key (RFC 3414 A.2): the hash of the password repeated to one megabyte, then of that with the engine
// ID on both sides. `out` receives hash_size bytes. The password should be at least 8 characters; an empty one gives false.
bool usm_localize_password(crypto::HashKind kind, const char* password, const uint8_t* engine_id, uint32_t engine_id_length, uint8_t* out);
// Keys for a user with the given passwords at an engine. Privacy needs authentication, and a password each. False otherwise.
bool usm_make_keys(UsmAuth auth, const char* auth_password, UsmPriv priv, const char* priv_password, const uint8_t* engine_id, uint32_t engine_id_length,
                   UsmKeys* out);

enum class UsmStatus : uint8_t {
  Ok,
  Malformed,      // not an SNMPv3 message, or damaged
  WrongLevel,     // the message's security level is not the one the keys call for
  AuthFailed,     // the authentication tag does not match
  DecryptFailed,  // the encrypted part is not a valid scoped PDU (wrong key)
};

// Encodes a version 3 message from message.v3 (ids, flags - only the "reportable" bit is taken, engine ID, boots, time, user, context)
// and the PDU in `message`. With `keys` the message is authenticated and (if keys->priv) encrypted; without it, it goes in the clear.
// `salt` makes the encryption IV unique: pass a counter that grows with every message.
bool encode_snmp_v3(const SnmpMessage& message, const UsmKeys* keys, uint64_t salt, allocators::IAllocator& allocator, ByteBuffer* out);
// Reads a version 3 message. With `keys` it must carry the security level they imply, its tag is verified and the PDU decrypted.
// Without keys (an unauthenticated message: engine discovery, reports) any message without privacy is accepted. With keys, a
// message sent without authentication is still accepted if it is a Report (that is how an agent complains about the engine ID or time).
UsmStatus decode_snmp_v3(ConstByteView data, const UsmKeys* keys, SnmpMessage* out);

}  // namespace network
