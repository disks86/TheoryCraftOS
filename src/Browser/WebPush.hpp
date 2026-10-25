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

// The cryptography of Web Push: message encryption (RFC 8188 aes128gcm content coding as RFC 8291 uses it) and the VAPID check of who is sending (RFC 8292). The push service
// (ServiceWorkerHub::push_request) decrypts what an application server sends to a subscription's endpoint with the subscription's private key; webpush_encrypt() is what such a
// server does, for programs on this machine that send pushes and for the tests.
#pragma once
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace browser {

constexpr uint32_t kWebPushMaxBody = 4096, kWebPushRecordSize = 4096;

// Base64 with the URL alphabet, no padding (padding is accepted when decoding). False for anything else.
// (the buffers it needs come from the buffer's allocator)
bool base64url_decode(StringView text, ByteBuffer* out);
void base64url_encode(ConstByteView bytes, String* out);

// Encrypts `plain` for the user agent whose public key (an uncompressed P-256 point) and 16 byte auth secret are given, with the sender's one-time private key and a 16 byte
// salt (both random in real use). `record_size` is the size of the records of the body (at least 18). The body is the header (salt, record size, the sender's public key) and records.
bool webpush_encrypt(ConstByteView ua_public, ConstByteView auth, ConstByteView plain, const uint8_t as_private[32], ConstByteView salt, uint32_t record_size, ByteBuffer* body);
// The other way: decrypts a body for the user agent that has the private key. False for a body that is cut, changed or not for this key.
bool webpush_decrypt(const uint8_t ua_private[32], ConstByteView ua_public, ConstByteView auth, ConstByteView body, ByteBuffer* plain);

enum class Vapid : uint8_t { Ok, Missing, Invalid };
// Checks the Authorization header (and Crypto-Key for the older form) of a push: a JWT signed (ES256) by the key the subscription was made for, for this audience
// (the origin of the push service), not expired and valid for at most a day. `now_s` 0: the clock is not known, the expiry is not checked.
Vapid vapid_check(allocators::IAllocator& allocator, StringView authorization, StringView crypto_key, ConstByteView application_server_key, StringView audience, uint64_t now_s);

}  // namespace browser
