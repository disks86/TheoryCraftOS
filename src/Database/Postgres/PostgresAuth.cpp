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

#include "Database/Postgres/PostgresAuth.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/X509.hpp"
#include "Database/NumberText.hpp"

namespace database::postgres {

namespace {
constexpr uint32_t kSha256 = 32;

bool append_hex(String* out, const uint8_t* data, uint32_t n) {
  static const char kHex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < n; ++i)
    if (!out->push_back(kHex[data[i] >> 4]) || !out->push_back(kHex[data[i] & 15])) return false;
  return true;
}

void sha256_hmac(ConstByteView key, ConstByteView data, uint8_t out[kSha256]) { crypto::hmac(crypto::HashKind::Sha256, key, data, out); }

// PBKDF2 with HMAC-SHA-256, one block of 32 bytes (RFC 2898, section 5.2).
void pbkdf2_sha256(ConstByteView password, ConstByteView salt, uint32_t iterations, uint8_t out[kSha256]) {
  uint8_t block[kSha256], u[kSha256];
  uint8_t first[80];
  uint32_t n = 0;
  for (uint64_t i = 0; i < salt.size() && n < 76; ++i) first[n++] = salt[i];
  first[n++] = 0;
  first[n++] = 0;
  first[n++] = 0;
  first[n++] = 1;
  sha256_hmac(password, ConstByteView(first, n), u);
  for (uint32_t i = 0; i < kSha256; ++i) block[i] = u[i];
  for (uint32_t it = 1; it < iterations; ++it) {
    uint8_t next[kSha256];
    sha256_hmac(password, ConstByteView(u, kSha256), next);
    for (uint32_t i = 0; i < kSha256; ++i) {
      u[i] = next[i];
      block[i] ^= next[i];
    }
  }
  for (uint32_t i = 0; i < kSha256; ++i) out[i] = block[i];
}

// The value of attribute `name` ("r", "s", "i", "v") in a comma separated list of "x=value".
bool attribute(StringView list, char name, StringView* value) {
  uint64_t at = 0;
  while (at < list.size()) {
    uint64_t end = list.find(',', at);
    if (end == StringView::npos) end = list.size();
    const StringView item = list.substr(at, end - at);
    if (item.size() >= 2 && item[0] == name && item[1] == '=') {
      *value = item.substr(2);
      return true;
    }
    at = end + 1;
  }
  return false;
}
}  // namespace

bool md5_password(StringView user, StringView password, ConstByteView salt, String* out) {
  uint8_t inner[16], outer[16];
  crypto::Hasher first(crypto::HashKind::Md5);
  first.update(password.bytes());
  first.update(user.bytes());
  first.finish(inner);
  char hex[32];
  static const char kHex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < 16; ++i) {
    hex[2 * i] = kHex[inner[i] >> 4];
    hex[2 * i + 1] = kHex[inner[i] & 15];
  }
  crypto::Hasher second(crypto::HashKind::Md5);
  second.update(ConstByteView(reinterpret_cast<const uint8_t*>(hex), 32));
  second.update(salt);
  second.finish(outer);
  out->clear();
  return out->append("md5") && append_hex(out, outer, 16);
}

bool append_base64(String* out, ConstByteView data) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (uint64_t i = 0; i < data.size(); i += 3) {
    const uint32_t a = data[i], b = i + 1 < data.size() ? data[i + 1] : 0, c = i + 2 < data.size() ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    const char quad[4] = {kAlphabet[(triple >> 18) & 63], kAlphabet[(triple >> 12) & 63], i + 1 < data.size() ? kAlphabet[(triple >> 6) & 63] : '=',
                          i + 2 < data.size() ? kAlphabet[triple & 63] : '='};
    if (!out->append(StringView(quad, 4))) return false;
  }
  return true;
}

bool ScramClient::start(StringView user, StringView password, StringView nonce, String* client_first) {
  ready_ = false;
  if (nonce.empty() || nonce.contains(',')) return false;
  if (!password_.assign(password) || !nonce_.assign(nonce)) return false;
  client_first_bare_.clear();
  if (!client_first_bare_.append("n=")) return false;
  for (uint64_t i = 0; i < user.size(); ++i) {   // "=" and "," in a name are written =3D and =2C
    const bool ok = user[i] == '=' ? client_first_bare_.append("=3D") : user[i] == ',' ? client_first_bare_.append("=2C") : client_first_bare_.push_back(user[i]);
    if (!ok) return false;
  }
  if (!client_first_bare_.append(",r=") || !client_first_bare_.append(nonce)) return false;
  client_first->clear();
  return client_first->append("n,,") && client_first->append(client_first_bare_.view());
}

bool ScramClient::handle_server_first(StringView server_first, String* client_final) {
  StringView r, s, i;
  if (!attribute(server_first, 'r', &r) || !attribute(server_first, 's', &s) || !attribute(server_first, 'i', &i)) return false;
  if (r.size() <= nonce_.size() || r.substr(0, nonce_.size()).equals(nonce_.view()) == false) return false;   // it must continue our nonce
  int64_t iterations;
  if (!parse_int64(i, &iterations) || iterations < 1 || iterations > kMaxIterations) return false;
  uint8_t salt[64];
  const int64_t salt_size = crypto::base64_decode(s.data(), s.size(), salt, sizeof salt);
  if (salt_size <= 0) return false;
  if (!server_first_.assign(server_first)) return false;

  uint8_t salted[kSha256], client_key[kSha256], stored_key[kSha256], signature[kSha256], proof[kSha256], server_key[kSha256], server_signature[kSha256];
  pbkdf2_sha256(password_.view().bytes(), ConstByteView(salt, static_cast<uint64_t>(salt_size)), static_cast<uint32_t>(iterations), salted);
  sha256_hmac(ConstByteView(salted, kSha256), StringView("Client Key").bytes(), client_key);
  crypto::hash(crypto::HashKind::Sha256, ConstByteView(client_key, kSha256), stored_key);

  // client-final-message-without-proof, then the AuthMessage the signatures are made over.
  client_final->clear();
  if (!client_final->append("c=biws,r=") || !client_final->append(r)) return false;
  message_.clear();
  if (!message_.append(client_first_bare_.view()) || !message_.push_back(',') || !message_.append(server_first_.view()) || !message_.push_back(',') ||
      !message_.append(client_final->view()))
    return false;
  sha256_hmac(ConstByteView(stored_key, kSha256), message_.view().bytes(), signature);
  for (uint32_t k = 0; k < kSha256; ++k) proof[k] = client_key[k] ^ signature[k];
  if (!client_final->append(",p=") || !append_base64(client_final, ConstByteView(proof, kSha256))) return false;

  sha256_hmac(ConstByteView(salted, kSha256), StringView("Server Key").bytes(), server_key);
  sha256_hmac(ConstByteView(server_key, kSha256), message_.view().bytes(), server_signature);
  expected_.clear();
  if (!expected_.append("v=") || !append_base64(&expected_, ConstByteView(server_signature, kSha256))) return false;
  ready_ = true;
  return true;
}

bool ScramClient::verify_server_final(StringView server_final) {
  if (!ready_) return false;
  StringView v;
  if (!attribute(server_final, 'v', &v)) return false;
  const StringView expected = expected_.view().substr(2);
  return v.size() == expected.size() && crypto::equal_constant_time(reinterpret_cast<const uint8_t*>(v.data()), reinterpret_cast<const uint8_t*>(expected.data()), static_cast<uint32_t>(v.size()));
}

}  // namespace database::postgres
