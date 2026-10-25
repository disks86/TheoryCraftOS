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

#include "Browser/WebPush.hpp"
#include "Crypto/Aead.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"

namespace browser {

namespace {

constexpr uint32_t kPoint = 65, kSalt = 16, kHeaderFixed = kSalt + 4 + 1;
const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

// The key and base nonce of RFC 8291 section 3.3-3.4.
bool derive(const uint8_t private_key[32], ConstByteView peer_public, ConstByteView ua_public, ConstByteView as_public, ConstByteView auth, ConstByteView salt,
            uint8_t cek[16], uint8_t nonce[12]) {
  using namespace crypto;
  uint8_t shared[32], prk_key[32], ikm[32], prk[32];
  if (peer_public.size() != kPoint || ua_public.size() != kPoint || as_public.size() != kPoint || salt.size() != kSalt) return false;
  if (!ec_ecdh(EcCurve::P256, private_key, peer_public, shared)) return false;
  hkdf_extract(HashKind::Sha256, auth, ConstByteView(shared, 32), prk_key);
  uint8_t info[14 + 2 * kPoint] = {'W', 'e', 'b', 'P', 'u', 's', 'h', ':', ' ', 'i', 'n', 'f', 'o', 0};
  ByteView(info, sizeof(info)).subview(14, kPoint).copy_from(ua_public);
  ByteView(info, sizeof(info)).subview(14 + kPoint, kPoint).copy_from(as_public);
  if (!hkdf_expand(HashKind::Sha256, ConstByteView(prk_key, 32), ConstByteView(info, sizeof(info)), ByteView(ikm, 32))) return false;
  hkdf_extract(HashKind::Sha256, salt, ConstByteView(ikm, 32), prk);
  static const uint8_t kCekInfo[] = "Content-Encoding: aes128gcm";   // (with the terminating zero)
  static const uint8_t kNonceInfo[] = "Content-Encoding: nonce";
  return hkdf_expand(HashKind::Sha256, ConstByteView(prk, 32), ConstByteView(kCekInfo, sizeof(kCekInfo)), ByteView(cek, 16)) &&
         hkdf_expand(HashKind::Sha256, ConstByteView(prk, 32), ConstByteView(kNonceInfo, sizeof(kNonceInfo)), ByteView(nonce, 12));
}

void record_nonce(const uint8_t base[12], uint32_t sequence, uint8_t out[12]) {
  for (uint32_t i = 0; i < 12; ++i) out[i] = base[i];
  for (uint32_t i = 0; i < 4; ++i) out[11 - i] ^= static_cast<uint8_t>(sequence >> (8 * i));
}

}  // namespace

bool base64url_decode(StringView text, ByteBuffer* out) {
  out->clear();
  while (text.ends_with('=')) text = text.without_suffix(1);
  uint32_t acc = 0, bits = 0;
  for (const char c : text) {
    const int v = b64_value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (!out->put8(static_cast<uint8_t>(acc >> bits))) return false;
      acc &= (1u << bits) - 1;
    }
  }
  return text.size() % 4 != 1;   // (a single left-over character carries no byte)
}

void base64url_encode(ConstByteView bytes, String* out) {
  uint32_t acc = 0, bits = 0;
  for (const uint8_t b : bytes) {
    acc = (acc << 8) | b;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out->push_back(kAlphabet[(acc >> bits) & 63]);
    }
    acc &= (1u << bits) - 1;
  }
  if (bits) out->push_back(kAlphabet[(acc << (6 - bits)) & 63]);
}

bool webpush_encrypt(ConstByteView ua_public, ConstByteView auth, ConstByteView plain, const uint8_t as_private[32], ConstByteView salt, uint32_t record_size, ByteBuffer* body) {
  using namespace crypto;
  uint8_t as_public[kPoint], cek[16], base[12];
  if (record_size < 18 || record_size > kWebPushRecordSize || !ec_public_key(EcCurve::P256, as_private, as_public)) return false;
  if (!derive(as_private, ua_public, ua_public, ConstByteView(as_public, kPoint), auth, salt, cek, base)) return false;
  AesGcm gcm;
  if (!gcm.set_key(ConstByteView(cek, 16))) return false;
  body->clear();
  if (!body->append(salt) || !body->put8(record_size >> 24) || !body->put8(record_size >> 16) || !body->put8(record_size >> 8) || !body->put8(record_size) || !body->put8(kPoint) || !body->append(ConstByteView(as_public, kPoint))) return false;
  const uint64_t chunk = record_size - 17;   // (data per record: the delimiter and the tag take the rest)
  uint64_t pos = 0;
  uint32_t sequence = 0;
  do {
    const uint64_t n = plain.size() - pos < chunk ? plain.size() - pos : chunk;
    const bool last = pos + n == plain.size();
    const uint64_t start = body->size();
    if (!body->append(plain.subview(pos, n)) || !body->put8(last ? 2 : 1) || !body->put_zeros(kAeadTagSize)) return false;
    uint8_t record[12], tag[kAeadTagSize];
    record_nonce(base, sequence++, record);
    gcm.seal(record, ConstByteView(), body->writable_view().subview(start, n + 1), tag);
    body->writable_view().subview(start + n + 1, kAeadTagSize).copy_from(ConstByteView(tag, kAeadTagSize));
    pos += n;
  } while (pos < plain.size());
  return true;
}

bool webpush_decrypt(const uint8_t ua_private[32], ConstByteView ua_public, ConstByteView auth, ConstByteView body, ByteBuffer* plain) {
  using namespace crypto;
  plain->clear();
  if (body.size() < kHeaderFixed + kPoint + 17 || body[kSalt + 4] != kPoint) return false;
  const uint32_t rs = (uint32_t{body[kSalt]} << 24) | (uint32_t{body[kSalt + 1]} << 16) | (uint32_t{body[kSalt + 2]} << 8) | body[kSalt + 3];
  if (rs < 18 || rs > kWebPushRecordSize) return false;
  const ConstByteView salt = body.first(kSalt), as_public = body.subview(kHeaderFixed, kPoint), records = body.subview(kHeaderFixed + kPoint);
  uint8_t cek[16], base[12];
  if (!derive(ua_private, as_public, ua_public, as_public, auth, salt, cek, base)) return false;
  AesGcm gcm;
  if (!gcm.set_key(ConstByteView(cek, 16))) return false;
  uint64_t pos = 0;
  uint32_t sequence = 0;
  while (pos < records.size()) {
    const uint64_t len = records.size() - pos < rs ? records.size() - pos : rs;
    const bool last = pos + len == records.size();
    if (len < 17) return false;
    const uint64_t data = len - kAeadTagSize, start = plain->size();
    uint8_t record[12], tag[kAeadTagSize];
    record_nonce(base, sequence++, record);
    ByteView(tag, kAeadTagSize).copy_from(records.subview(pos + data, kAeadTagSize));
    if (!plain->append(records.subview(pos, data))) return false;
    if (!gcm.open(record, ConstByteView(), plain->writable_view().subview(start, data), tag)) return false;
    uint64_t end = plain->size();   // (the padding: zeros after the delimiter, which is 1 and, in the last record, 2)
    while (end > start && plain->view()[end - 1] == 0) --end;
    if (end == start) return false;
    const uint8_t delimiter = plain->view()[end - 1];
    if (delimiter != (last ? 2 : 1)) return false;
    plain->truncate(end - 1);
    pos += len;
  }
  return sequence > 0;
}

namespace {

// The value of a claim in a JSON object (a string up to its closing quote, or a number's digits); `needle` is the quoted name. Good enough for claims whose signature was checked.
bool claim_text(StringView json, StringView needle, StringView* out) {
  uint64_t at = json.find(needle);
  if (at == StringView::npos) return false;
  at += needle.size();
  while (at < json.size() && (json[at] == ' ' || json[at] == ':')) ++at;
  if (at >= json.size()) return false;
  const bool quoted = json[at] == '"';
  if (quoted) ++at;
  uint64_t end = at;
  while (end < json.size() && (quoted ? json[end] != '"' : (json[end] >= '0' && json[end] <= '9'))) ++end;
  if (quoted && end == json.size()) return false;
  *out = json.substr(at, end - at);
  return !out->empty();
}

// Text and bytes seen as each other (the same memory, char and uint8_t: no alignment or lifetime question).
ConstByteView bytes_of_text(StringView text) { return ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }
StringView text_of_bytes(ConstByteView bytes) { return StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

bool starts_with_word(StringView text, StringView word) { return text.size() >= word.size() && text.first(word.size()).equals_ignore_case(word); }

}  // namespace

Vapid vapid_check(allocators::IAllocator& allocator, StringView authorization, StringView crypto_key, ConstByteView application_server_key, StringView audience, uint64_t now_s) {
  using namespace crypto;
  const StringView auth = authorization.trimmed();
  if (auth.empty()) return Vapid::Missing;
  StringView jwt, key_text, token;
  if (starts_with_word(auth, "vapid")) {
    StringView rest = auth.substr(5);
    while (rest.next_token(',', &token)) {
      token = token.trimmed();
      if (token.starts_with("t=")) jwt = token.substr(2);
      else if (token.starts_with("k=")) key_text = token.substr(2);
    }
  } else if (starts_with_word(auth, "webpush ")) {
    jwt = auth.substr(8).trimmed();
    StringView rest = crypto_key;
    while (rest.next_token(';', &token)) {
      token = token.trimmed();
      if (token.starts_with("p256ecdsa=")) key_text = token.substr(10);
    }
  } else {
    return Vapid::Invalid;
  }
  ByteBuffer key(allocator), signature(allocator), header(allocator), payload(allocator);
  if (!base64url_decode(key_text, &key) || key.size() != application_server_key.size() || application_server_key.size() != kPoint ||
      !equal_constant_time(key.view().data(), application_server_key.data(), kPoint))
    return Vapid::Invalid;
  const uint64_t dot1 = jwt.find('.');
  const uint64_t dot2 = dot1 == StringView::npos ? StringView::npos : jwt.find('.', dot1 + 1);
  if (dot2 == StringView::npos || !base64url_decode(jwt.first(dot1), &header) || !base64url_decode(jwt.substr(dot1 + 1, dot2 - dot1 - 1), &payload) ||
      !base64url_decode(jwt.substr(dot2 + 1), &signature) || signature.size() != 64)
    return Vapid::Invalid;
  uint8_t digest[32];
  Hasher hasher(HashKind::Sha256);
  hasher.update(bytes_of_text(jwt.first(dot2)));   // (the signed text is the first two parts as sent)
  hasher.finish(digest);
  if (!ecdsa_verify(EcCurve::P256, key.view(), digest, 32, signature.view().first(32), signature.view().subview(32, 32))) return Vapid::Invalid;
  const StringView head = text_of_bytes(header.view()), claims = text_of_bytes(payload.view());
  StringView alg, aud, exp;
  uint64_t expiry = 0;
  if (!claim_text(head, "\"alg\"", &alg) || !alg.equals("ES256") || !claim_text(claims, "\"aud\"", &aud) || !aud.equals(audience) || !claim_text(claims, "\"exp\"", &exp) ||
      !exp.parse_uint(&expiry))
    return Vapid::Invalid;
  if (now_s != 0 && (expiry <= now_s || expiry - now_s > 86400)) return Vapid::Invalid;
  return Vapid::Ok;
}

}  // namespace browser
