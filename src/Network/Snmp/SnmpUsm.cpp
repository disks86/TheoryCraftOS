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

#include "Network/Snmp/SnmpUsm.hpp"
#include "Network/Snmp/SnmpBer.hpp"

namespace network {

using namespace ber;

uint32_t usm_auth_length(UsmAuth a) {
  switch (a) {
    case UsmAuth::HmacMd5:
    case UsmAuth::HmacSha1: return 12;
    case UsmAuth::HmacSha224: return 16;
    case UsmAuth::HmacSha256: return 24;
    default: return 0;
  }
}

crypto::HashKind usm_hash_kind(UsmAuth a) {
  switch (a) {
    case UsmAuth::HmacSha1: return crypto::HashKind::Sha1;
    case UsmAuth::HmacSha224: return crypto::HashKind::Sha224;
    case UsmAuth::HmacSha256: return crypto::HashKind::Sha256;
    default: return crypto::HashKind::Md5;
  }
}

bool usm_localize_password(crypto::HashKind kind, const char* password, const uint8_t* engine_id, uint32_t engine_id_length, uint8_t* out) {
  uint32_t length = 0;
  while (password && password[length]) ++length;
  if (length == 0) return false;
  // Ku: the password, repeated, fed to the hash up to one megabyte.
  crypto::Hasher h(kind);
  uint8_t chunk[64];
  uint32_t at = 0;
  for (uint32_t done = 0; done < 1048576; done += 64) {
    for (uint32_t i = 0; i < 64; ++i) {
      chunk[i] = static_cast<uint8_t>(password[at]);
      at = at + 1 == length ? 0 : at + 1;
    }
    h.update(ConstByteView(chunk, 64));
  }
  uint8_t ku[crypto::kMaxHashSize];
  h.finish(ku);
  // Kul = H(Ku || engineID || Ku)
  const uint32_t size = crypto::hash_size(kind);
  h.update(ConstByteView(ku, size));
  h.update(ConstByteView(engine_id, engine_id_length));
  h.update(ConstByteView(ku, size));
  h.finish(out);
  for (uint32_t i = 0; i < size; ++i) ku[i] = 0;
  return true;
}

bool usm_make_keys(UsmAuth auth, const char* auth_password, UsmPriv priv, const char* priv_password, const uint8_t* engine_id, uint32_t engine_id_length,
                   UsmKeys* out) {
  *out = UsmKeys();
  out->auth = auth;
  out->priv = priv;
  if (auth == UsmAuth::None) return priv == UsmPriv::None;
  const crypto::HashKind kind = usm_hash_kind(auth);
  if (!usm_localize_password(kind, auth_password, engine_id, engine_id_length, out->auth_key)) return false;
  out->auth_key_length = crypto::hash_size(kind);
  if (priv != UsmPriv::None) {
    // The privacy key is made the same way, with the same hash; DES uses 16 bytes (key and pre-IV), AES-128 the first 16.
    if (!usm_localize_password(kind, priv_password, engine_id, engine_id_length, out->priv_key)) return false;
    out->priv_key_length = 16;
  }
  return true;
}

namespace {

void make_iv(UsmPriv priv, const UsmKeys& keys, uint32_t boots, uint32_t time, const uint8_t salt[8], uint8_t* iv) {
  if (priv == UsmPriv::Aes128) {
    for (int i = 0; i < 4; ++i) {
      iv[i] = static_cast<uint8_t>(boots >> (24 - 8 * i));
      iv[4 + i] = static_cast<uint8_t>(time >> (24 - 8 * i));
    }
    for (int i = 0; i < 8; ++i) iv[8 + i] = salt[i];
  } else {
    for (int i = 0; i < 8; ++i) iv[i] = static_cast<uint8_t>(keys.priv_key[8 + i] ^ salt[i]);  // pre-IV xor salt
  }
}

}  // namespace

bool encode_snmp_v3(const SnmpMessage& m, const UsmKeys* keys, uint64_t salt, allocators::IAllocator& a, ByteBuffer* out) {
  out->clear();
  const bool auth = keys && keys->auth != UsmAuth::None;
  const bool priv = keys && keys->priv != UsmPriv::None;
  if (priv && !auth) return false;
  const SnmpV3& v = m.v3;
  uint8_t flags = static_cast<uint8_t>((m.v3.flags & 4) | (auth ? 1 : 0) | (priv ? 2 : 0));

  // Scoped PDU: context engine ID, context name, the PDU.
  ByteBuffer pdu(a), scoped_body(a), scoped(a);
  if (!encode_snmp_pdu(m, a, &pdu)) return false;
  uint32_t name_length = 0;
  while (v.context_name[name_length] && name_length < sizeof v.context_name - 1) ++name_length;
  if (!put_tlv(&scoped_body, 0x04, ConstByteView(v.context_engine_id, v.context_engine_id_length)) ||
      !put_tlv(&scoped_body, 0x04, ConstByteView(reinterpret_cast<const uint8_t*>(v.context_name), name_length)) || !scoped_body.append(pdu.view()) ||
      !put_tlv(&scoped, 0x30, scoped_body.view()))
    return false;

  // Privacy: encrypt the scoped PDU; the data field is then an OCTET STRING of ciphertext.
  uint8_t salt_bytes[8] = {};
  ByteBuffer data(a);
  if (priv) {
    for (int i = 0; i < 8; ++i) salt_bytes[i] = static_cast<uint8_t>(salt >> (56 - 8 * i));
    if (keys->priv == UsmPriv::Des) {  // RFC 3414: the salt is the boots count and a counter
      for (int i = 0; i < 4; ++i) salt_bytes[i] = static_cast<uint8_t>(v.boots >> (24 - 8 * i));
    }
    uint8_t iv[16];
    make_iv(keys->priv, *keys, v.boots, v.time, salt_bytes, iv);
    ByteBuffer plain(a);
    if (!plain.append(scoped.view())) return false;
    if (keys->priv == UsmPriv::Des) {
      const uint64_t padded = (plain.size() + 7) / 8 * 8;
      if (!plain.put_zeros(padded - plain.size())) return false;
    }
    // (ByteBuffer exposes a read-only view; encrypt a copy and append it)
    uint8_t* tmp = static_cast<uint8_t*>(a.allocate(plain.size() ? plain.size() : 1));
    if (!tmp) return false;
    for (uint64_t i = 0; i < plain.size(); ++i) tmp[i] = plain.view()[i];
    if (keys->priv == UsmPriv::Des) {
      crypto::Des des;
      des.set_key(keys->priv_key);
      des.cbc(iv, ByteView(tmp, plain.size()), false);
    } else {
      crypto::Aes aes;
      aes.set_key(ConstByteView(keys->priv_key, 16));
      aes.cfb128(iv, ByteView(tmp, plain.size()), false);
    }
    const bool ok = put_tlv(&data, 0x04, ConstByteView(tmp, plain.size()));
    a.deallocate(tmp);
    if (!ok) return false;
  } else if (!data.append(scoped.view())) {
    return false;
  }

  // Security parameters. The authentication tag is zero for now; its place in the message is remembered.
  const uint32_t auth_length = auth ? usm_auth_length(keys->auth) : 0;
  uint32_t user_length = 0;
  while (v.user[user_length] && user_length < sizeof v.user - 1) ++user_length;
  ByteBuffer sec_body(a), sec(a), sec_octet(a);
  if (!put_tlv(&sec_body, 0x04, ConstByteView(v.engine_id, v.engine_id_length)) || !put_integer(&sec_body, 0x02, v.boots) || !put_integer(&sec_body, 0x02, v.time) ||
      !put_tlv(&sec_body, 0x04, ConstByteView(reinterpret_cast<const uint8_t*>(v.user), user_length)))
    return false;
  const uint64_t auth_in_body = sec_body.size() + 2;  // after the tag and one length byte
  uint8_t zeros[24] = {};
  if (!put_tlv(&sec_body, 0x04, ConstByteView(zeros, auth_length)) || !put_tlv(&sec_body, 0x04, ConstByteView(salt_bytes, priv ? 8 : 0)) ||
      !put_tlv(&sec, 0x30, sec_body.view()) || !put_tlv(&sec_octet, 0x04, sec.view()))
    return false;
  const uint64_t auth_in_octet = (sec_octet.size() - sec_body.size()) + auth_in_body;  // headers of the two wrappers

  // Header data and the whole message.
  ByteBuffer global_body(a), global(a), head(a), body(a);
  if (!put_integer(&global_body, 0x02, v.msg_id) || !put_integer(&global_body, 0x02, v.max_size) ||
      !put_tlv(&global_body, 0x04, ConstByteView(&flags, 1)) || !put_integer(&global_body, 0x02, 3) || !put_tlv(&global, 0x30, global_body.view()))
    return false;
  if (!put_integer(&head, 0x02, 3) || !head.append(global.view())) return false;
  if (!body.append(head.view()) || !body.append(sec_octet.view()) || !body.append(data.view()) || !put_tlv(out, 0x30, body.view())) return false;
  if (auth) {
    const uint64_t outer_header = out->size() - body.size();
    const uint64_t tag_at = outer_header + head.size() + auth_in_octet;
    // HMAC over the message with the tag zeroed (it is), then the truncated result into place.
    uint8_t tag[crypto::kMaxHashSize];
    crypto::hmac(usm_hash_kind(keys->auth), ConstByteView(keys->auth_key, keys->auth_key_length), out->view(), tag);
    for (uint32_t i = 0; i < auth_length; ++i) out->patch8(tag_at + i, tag[i]);
  }
  return true;
}

UsmStatus decode_snmp_v3(ConstByteView data, const UsmKeys* keys, SnmpMessage* m) {
  *m = SnmpMessage();
  Reader top{data};
  uint8_t tag;
  ConstByteView content;
  if (!top.tlv(&tag, &content) || tag != 0x30 || !top.done()) return UsmStatus::Malformed;
  Reader msg{content};
  int64_t n;
  if (!msg.tlv(&tag, &content) || tag != 0x02 || !read_int(content, &n, true) || n != 3) return UsmStatus::Malformed;
  m->version = 3;
  SnmpV3& v = m->v3;
  // Global data
  if (!msg.tlv(&tag, &content) || tag != 0x30) return UsmStatus::Malformed;
  {
    Reader g{content};
    ConstByteView c;
    if (!g.tlv(&tag, &c) || tag != 0x02 || !read_int(c, &n, true)) return UsmStatus::Malformed;
    v.msg_id = static_cast<int32_t>(n);
    if (!g.tlv(&tag, &c) || tag != 0x02 || !read_int(c, &n, true)) return UsmStatus::Malformed;
    v.max_size = static_cast<int32_t>(n);
    if (!g.tlv(&tag, &c) || tag != 0x04 || c.size() != 1) return UsmStatus::Malformed;
    v.flags = c[0];
    if (!g.tlv(&tag, &c) || tag != 0x02 || !read_int(c, &n, true) || n != 3 || !g.done()) return UsmStatus::Malformed;  // security model: USM
  }
  // Security parameters (an OCTET STRING holding a SEQUENCE)
  ConstByteView sec_octet;
  if (!msg.tlv(&tag, &sec_octet) || tag != 0x04) return UsmStatus::Malformed;
  uint64_t auth_offset = 0;
  {
    Reader outer{sec_octet};
    ConstByteView seq;
    if (!outer.tlv(&tag, &seq) || tag != 0x30 || !outer.done()) return UsmStatus::Malformed;
    Reader s{seq};
    ConstByteView c;
    if (!s.tlv(&tag, &c) || tag != 0x04 || c.size() > sizeof v.engine_id) return UsmStatus::Malformed;
    v.engine_id_length = static_cast<uint8_t>(c.size());
    for (uint64_t i = 0; i < c.size(); ++i) v.engine_id[i] = c[i];
    if (!s.tlv(&tag, &c) || tag != 0x02 || !read_int(c, &n, false)) return UsmStatus::Malformed;
    v.boots = static_cast<uint32_t>(n);
    if (!s.tlv(&tag, &c) || tag != 0x02 || !read_int(c, &n, false)) return UsmStatus::Malformed;
    v.time = static_cast<uint32_t>(n);
    if (!s.tlv(&tag, &c) || tag != 0x04 || c.size() >= sizeof v.user) return UsmStatus::Malformed;
    for (uint64_t i = 0; i < c.size(); ++i) v.user[i] = static_cast<char>(c[i]);
    if (!s.tlv(&tag, &c) || tag != 0x04 || c.size() > sizeof v.auth_parameters) return UsmStatus::Malformed;
    v.auth_length = static_cast<uint8_t>(c.size());
    for (uint64_t i = 0; i < c.size(); ++i) v.auth_parameters[i] = c[i];
    auth_offset = static_cast<uint64_t>(c.data() - data.data());
    if (!s.tlv(&tag, &c) || tag != 0x04 || c.size() > sizeof v.priv_parameters || !s.done()) return UsmStatus::Malformed;
    v.priv_length = static_cast<uint8_t>(c.size());
    for (uint64_t i = 0; i < c.size(); ++i) v.priv_parameters[i] = c[i];
  }
  // The security level asked for in the flags must be the one the keys give.
  const bool flag_auth = v.flags & 1, flag_priv = v.flags & 2;
  if (flag_priv && !flag_auth) return UsmStatus::Malformed;
  bool unauthenticated_report = false;  // an agent reports errors (unknown engine, wrong time) without authentication
  if (keys && !flag_auth && !flag_priv && (keys->auth != UsmAuth::None)) {
    unauthenticated_report = true;
    keys = nullptr;
  }
  if (keys) {
    if (flag_auth != (keys->auth != UsmAuth::None) || flag_priv != (keys->priv != UsmPriv::None)) return UsmStatus::WrongLevel;
  } else if (flag_priv) {
    return UsmStatus::WrongLevel;
  }
  if (flag_auth && keys) {
    const uint32_t want = usm_auth_length(keys->auth);
    if (v.auth_length != want) return UsmStatus::AuthFailed;
    // Recompute the tag over the message with the tag field zeroed.
    uint8_t tag_bytes[crypto::kMaxHashSize];
    // HMAC over the message with the tag field zeroed, fed in pieces so no copy is needed.
    const crypto::HashKind kind = usm_hash_kind(keys->auth);
    const uint32_t block = crypto::hash_block_size(kind), size = crypto::hash_size(kind);
    uint8_t k[64] = {0}, pad[64];
    if (keys->auth_key_length > block) crypto::hash(kind, ConstByteView(keys->auth_key, keys->auth_key_length), k);
    else for (uint32_t i = 0; i < keys->auth_key_length; ++i) k[i] = keys->auth_key[i];
    for (uint32_t i = 0; i < block; ++i) pad[i] = k[i] ^ 0x36;
    crypto::Hasher inner(kind);
    inner.update(ConstByteView(pad, block));
    inner.update(data.first(auth_offset));
    uint8_t zeros[24] = {};
    inner.update(ConstByteView(zeros, want));
    inner.update(data.dropped(auth_offset + want));
    uint8_t digest[crypto::kMaxHashSize];
    inner.finish(digest);
    for (uint32_t i = 0; i < block; ++i) pad[i] = k[i] ^ 0x5c;
    crypto::Hasher outer(kind);
    outer.update(ConstByteView(pad, block));
    outer.update(ConstByteView(digest, size));
    outer.finish(tag_bytes);
    if (!crypto::equal_constant_time(tag_bytes, v.auth_parameters, want)) return UsmStatus::AuthFailed;
  }
  // The data: a scoped PDU, or encrypted bytes.
  uint8_t stack_plain[1500];
  ConstByteView scoped_body;
  if (!msg.tlv(&tag, &content) || !msg.done()) return UsmStatus::Malformed;
  if (flag_priv) {
    if (tag != 0x04 || content.size() > sizeof stack_plain || v.priv_length != 8) return UsmStatus::Malformed;
    if (keys->priv == UsmPriv::Des && content.size() % 8 != 0) return UsmStatus::Malformed;
    const uint64_t plain_length = content.size();
    for (uint64_t i = 0; i < plain_length; ++i) stack_plain[i] = content[i];
    uint8_t iv[16];
    make_iv(keys->priv, *keys, v.boots, v.time, v.priv_parameters, iv);
    if (keys->priv == UsmPriv::Des) {
      crypto::Des des;
      des.set_key(keys->priv_key);
      des.cbc(iv, ByteView(stack_plain, plain_length), true);
    } else {
      crypto::Aes aes;
      aes.set_key(ConstByteView(keys->priv_key, 16));
      aes.cfb128(iv, ByteView(stack_plain, plain_length), true);
    }
    Reader sr{ConstByteView(stack_plain, plain_length)};
    uint8_t t;
    if (!sr.tlv(&t, &scoped_body) || t != 0x30) return UsmStatus::DecryptFailed;  // (trailing DES padding is ignored)
  } else {
    if (tag != 0x30) return UsmStatus::Malformed;
    scoped_body = content;
  }
  Reader s{scoped_body};
  ConstByteView c;
  const UsmStatus bad = flag_priv ? UsmStatus::DecryptFailed : UsmStatus::Malformed;
  if (!s.tlv(&tag, &c) || tag != 0x04 || c.size() > sizeof v.context_engine_id) return bad;
  v.context_engine_id_length = static_cast<uint8_t>(c.size());
  for (uint64_t i = 0; i < c.size(); ++i) v.context_engine_id[i] = c[i];
  if (!s.tlv(&tag, &c) || tag != 0x04 || c.size() >= sizeof v.context_name) return bad;
  for (uint64_t i = 0; i < c.size(); ++i) v.context_name[i] = static_cast<char>(c[i]);
  if (!s.tlv(&tag, &c) || !s.done()) return bad;
  if (!decode_snmp_pdu(tag, c, m)) return bad;
  if (unauthenticated_report && m->pdu != SnmpPdu::Report) return UsmStatus::WrongLevel;  // only reports may come without the level asked for
  return UsmStatus::Ok;
}

}  // namespace network
