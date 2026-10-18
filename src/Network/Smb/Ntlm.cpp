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

#include "Network/Smb/Ntlm.hpp"
#include "Utility/Utf16.hpp"

namespace network {

namespace {

constexpr uint32_t kUnicode = 0x00000001, kRequestTarget = 0x00000004, kSign = 0x00000010, kNtlm = 0x00000200, kAnonymous = 0x00000800,
                   kAlwaysSign = 0x00008000, kExtendedSecurity = 0x00080000, kTargetInfo = 0x00800000, k128 = 0x20000000, k56 = 0x80000000u;
constexpr uint32_t kAvEol = 0, kAvFlags = 6, kAvTimestamp = 7;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }

void put_field(ByteBuffer* b, uint64_t length, uint64_t offset) {
  b->put16(static_cast<uint32_t>(length));
  b->put16(static_cast<uint32_t>(length));
  b->put32(static_cast<uint32_t>(offset));
}

char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

}  // namespace

bool NtlmClient::negotiate(ByteBuffer* out) {
  negotiate_.clear();
  const uint32_t flags = kUnicode | kRequestTarget | kSign | kNtlm | kAlwaysSign | kExtendedSecurity | kTargetInfo | k128 | k56;
  bool ok = negotiate_.append(ConstByteView(reinterpret_cast<const uint8_t*>("NTLMSSP"), 8)) && negotiate_.put32(1) && negotiate_.put32(flags);
  ok = ok && negotiate_.put_zeros(16);   // no domain, no workstation: their fields are empty and point at the end of the header
  if (!ok) return false;
  negotiate_.patch32(20, 32);
  negotiate_.patch32(28, 32);
  return out->append(negotiate_.view());
}

bool NtlmClient::ntowf_v2(allocators::IAllocator& allocator, const NtlmCredentials& credentials, uint8_t out[16]) {
  ByteBuffer password(allocator), identity(allocator);
  if (!util::append_utf16le(&password, credentials.password)) return false;
  uint8_t hash[16];
  crypto::md4(password.view(), hash);
  String name(allocator);
  for (char c : credentials.user) name.push_back(upper(c));
  if (!util::append_utf16le(&identity, name.view()) || !util::append_utf16le(&identity, credentials.domain)) return false;
  crypto::hmac(crypto::HashKind::Md5, ConstByteView(hash, 16), identity.view(), out);
  return true;
}

bool NtlmClient::ntlm_v2_response(allocators::IAllocator& allocator, const uint8_t ntowf[16], const uint8_t server_challenge[8], const uint8_t client_challenge[8],
                                  uint64_t filetime, ConstByteView target_info, ByteBuffer* out, uint8_t session_base_key[16]) {
  // The blob: 01 01 0000 00000000, the time, the client's challenge, 00000000, the target info, 00000000.
  ByteBuffer blob(allocator);
  bool ok = blob.put8(1) && blob.put8(1) && blob.put_zeros(6) && blob.put32(static_cast<uint32_t>(filetime)) && blob.put32(static_cast<uint32_t>(filetime >> 32)) &&
            blob.append(ConstByteView(client_challenge, 8)) && blob.put_zeros(4) && blob.append(target_info) && blob.put_zeros(4);
  if (!ok) return false;
  // proof = HMAC-MD5(NTOWFv2, server challenge || blob)
  ByteBuffer input(allocator);
  if (!input.append(ConstByteView(server_challenge, 8)) || !input.append(blob.view())) return false;
  uint8_t proof[16];
  crypto::hmac(crypto::HashKind::Md5, ConstByteView(ntowf, 16), input.view(), proof);
  crypto::hmac(crypto::HashKind::Md5, ConstByteView(ntowf, 16), ConstByteView(proof, 16), session_base_key);
  return out->append(ConstByteView(proof, 16)) && out->append(blob.view());
}

bool NtlmClient::authenticate(ConstByteView challenge, const NtlmCredentials& credentials, StringView workstation, uint64_t now_filetime, crypto::IRandom& random,
                              ByteBuffer* out) {
  have_key_ = false;
  // The server's CHALLENGE_MESSAGE.
  if (challenge.size() < 48 || __builtin_memcmp(challenge.data(), "NTLMSSP", 8) != 0 || le32(challenge.data() + 8) != 2) return false;
  const uint8_t* c = challenge.data();
  const uint32_t server_flags = le32(c + 20);
  const uint8_t* server_challenge = c + 24;
  const uint32_t info_length = le16(c + 40), info_offset = le32(c + 44);
  ConstByteView target_info;
  if (info_length) {
    if (uint64_t{info_offset} + info_length > challenge.size()) return false;
    target_info = challenge.subview(info_offset, info_length);
  }
  if (!(server_flags & kUnicode)) return false;   // OEM-only servers are not supported

  // The target info the response is bound to: the server's pairs, with "this message has a MIC" added to the flags pair - when the server gave a
  // time, which is what makes the MIC mandatory.
  uint64_t server_time = 0;
  bool have_time = false;
  ByteBuffer pairs(allocator_);
  bool flags_pair_seen = false;
  for (uint64_t at = 0; at + 4 <= target_info.size();) {
    const uint32_t id = le16(target_info.data() + at), length = le16(target_info.data() + at + 2);
    if (at + 4 + length > target_info.size()) return false;
    const uint8_t* value = target_info.data() + at + 4;
    if (id == kAvEol) break;
    if (id == kAvTimestamp && length == 8) {
      server_time = uint64_t{le32(value)} | (uint64_t{le32(value + 4)} << 32);
      have_time = true;
    }
    if (id == kAvFlags && length == 4 && !credentials.anonymous()) {
      flags_pair_seen = true;
      if (!pairs.put16(id) || !pairs.put16(4) || !pairs.put32(le32(value) | 2)) return false;
    } else if (!pairs.append(target_info.subview(at, 4 + length))) {
      return false;
    }
    at += 4 + length;
  }
  if (have_time && !flags_pair_seen && !credentials.anonymous() && (!pairs.put16(kAvFlags) || !pairs.put16(4) || !pairs.put32(2))) return false;
  if (!pairs.put_zeros(4)) return false;   // MsvAvEOL

  uint32_t flags = (server_flags & (kUnicode | kRequestTarget | kSign | kNtlm | kAlwaysSign | kExtendedSecurity | kTargetInfo | k128 | k56)) | kUnicode;
  ByteBuffer domain(allocator_), user(allocator_), host(allocator_), lm(allocator_), nt(allocator_);
  if (!util::append_utf16le(&domain, credentials.domain) || !util::append_utf16le(&user, credentials.user) || !util::append_utf16le(&host, workstation)) return false;
  uint8_t base_key[16] = {};
  if (credentials.anonymous()) {
    flags |= kAnonymous;
    if (!lm.put8(0)) return false;   // a single zero byte, and no NT response
  } else {
    uint8_t ntowf[16], client_challenge[8];
    if (!ntowf_v2(allocator_, credentials, ntowf) || !random.fill(ByteView(client_challenge, 8))) return false;
    if (!ntlm_v2_response(allocator_, ntowf, server_challenge, client_challenge, have_time ? server_time : now_filetime, pairs.view(), &nt, base_key)) return false;
    if (have_time) {
      if (!lm.put_zeros(24)) return false;
    } else {   // LMv2: HMAC-MD5(NTOWFv2, server challenge || client challenge) and the client challenge
      uint8_t both[16], mac[16];
      __builtin_memcpy(both, server_challenge, 8);
      __builtin_memcpy(both + 8, client_challenge, 8);
      crypto::hmac(crypto::HashKind::Md5, ConstByteView(ntowf, 16), ConstByteView(both, 16), mac);
      if (!lm.append(ConstByteView(mac, 16)) || !lm.append(ConstByteView(client_challenge, 8))) return false;
    }
  }

  // The AUTHENTICATE_MESSAGE: an 88 byte header (with room for the MIC), then domain, user, workstation, LM, NT and no session key.
  ByteBuffer message(allocator_);
  uint64_t offset = 88;
  bool ok = message.append(ConstByteView(reinterpret_cast<const uint8_t*>("NTLMSSP"), 8)) && message.put32(3);
  put_field(&message, lm.size(), offset + domain.size() + user.size() + host.size());
  put_field(&message, nt.size(), offset + domain.size() + user.size() + host.size() + lm.size());
  put_field(&message, domain.size(), offset);
  put_field(&message, user.size(), offset + domain.size());
  put_field(&message, host.size(), offset + domain.size() + user.size());
  put_field(&message, 0, offset + domain.size() + user.size() + host.size() + lm.size() + nt.size());
  ok = ok && message.put32(flags) && message.put_zeros(8) /* no version */ && message.put_zeros(16) /* the MIC, filled in below */;
  ok = ok && message.append(domain.view()) && message.append(user.view()) && message.append(host.view()) && message.append(lm.view()) && message.append(nt.view());
  if (!ok) return false;
  if (!credentials.anonymous()) {
    __builtin_memcpy(session_key_, base_key, 16);   // (no key exchange: the session key is the base key)
    have_key_ = true;
    if (have_time) {   // MIC = HMAC-MD5(session key, negotiate || challenge || authenticate with a zero MIC)
      ByteBuffer all(allocator_);
      if (!all.append(negotiate_.view()) || !all.append(challenge) || !all.append(message.view())) return false;
      uint8_t mic[16];
      crypto::hmac(crypto::HashKind::Md5, ConstByteView(session_key_, 16), all.view(), mic);
      for (int i = 0; i < 16; ++i) message.patch8(72 + i, mic[i]);
    }
  }
  return out->append(message.view());
}

}  // namespace network
