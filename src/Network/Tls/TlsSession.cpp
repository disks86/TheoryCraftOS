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

#include "Network/Tls/TlsSession.hpp"
#include "Network/Tls/TlsKeys.hpp"
#include "Network/Tls/TlsWire.hpp"

namespace network {

using namespace crypto;
using namespace tlswire;

namespace {

void lower_name(char* out, StringView name) {
  uint64_t n = 0;
  for (; n < name.size() && n + 1 < 256; ++n) out[n] = name[n] >= 'A' && name[n] <= 'Z' ? static_cast<char>(name[n] + 32) : name[n];
  out[n] = 0;
}

bool same_server(const TlsTicket& t, const char* lower, bool quic) {
  if (t.quic != quic) return false;
  for (uint32_t i = 0;; ++i) {
    if (t.server_name[i] != lower[i]) return false;
    if (!lower[i]) return true;
  }
}

}  // namespace

bool TlsSessionCache::store(const TlsTicket& ticket) {
  kernel::ScopedMutex lock(mutex_);
  // too many for this server: the oldest goes
  uint32_t same = 0;
  for (uint64_t i = 0; i < tickets_.count(); ++i)
    if (same_server(*tickets_[i], ticket.server_name, ticket.quic)) ++same;
  while (same >= kPerServer) {
    for (uint64_t i = 0; i < tickets_.count(); ++i)
      if (same_server(*tickets_[i], ticket.server_name, ticket.quic)) {
        tickets_.remove_at(i);
        break;
      }
    --same;
  }
  if (tickets_.count() >= kMaxTickets) tickets_.remove_at(0);
  UniquePointer<TlsTicket> copy = UniquePointer<TlsTicket>::make(allocator_);
  if (!copy) return false;
  copy->quic = ticket.quic;
  for (uint32_t i = 0; i < sizeof copy->server_name; ++i) copy->server_name[i] = ticket.server_name[i];
  copy->suite_id = ticket.suite_id;
  copy->lifetime_s = ticket.lifetime_s;
  copy->age_add = ticket.age_add;
  copy->received_ms = ticket.received_ms;
  copy->ticket_length = ticket.ticket_length;
  for (uint32_t i = 0; i < ticket.ticket_length; ++i) copy->ticket[i] = ticket.ticket[i];
  copy->psk_length = ticket.psk_length;
  for (uint32_t i = 0; i < ticket.psk_length; ++i) copy->psk[i] = ticket.psk[i];
  return tickets_.push_back(static_cast<UniquePointer<TlsTicket>&&>(copy));
}

bool TlsSessionCache::take(StringView server_name, bool quic, uint64_t now_ms, TlsTicket* out) {
  char lower[256];
  lower_name(lower, server_name);
  kernel::ScopedMutex lock(mutex_);
  for (uint64_t i = tickets_.count(); i > 0; --i) {   // newest first
    const TlsTicket& t = *tickets_[i - 1];
    if (!same_server(t, lower, quic)) continue;
    const bool expired = now_ms < t.received_ms || (now_ms - t.received_ms) / 1000 >= t.lifetime_s;
    if (!expired) {
      out->quic = t.quic;
      for (uint32_t k = 0; k < sizeof out->server_name; ++k) out->server_name[k] = t.server_name[k];
      out->suite_id = t.suite_id;
      out->lifetime_s = t.lifetime_s;
      out->age_add = t.age_add;
      out->received_ms = t.received_ms;
      out->ticket_length = t.ticket_length;
      for (uint32_t k = 0; k < t.ticket_length; ++k) out->ticket[k] = t.ticket[k];
      out->psk_length = t.psk_length;
      for (uint32_t k = 0; k < t.psk_length; ++k) out->psk[k] = t.psk[k];
    }
    tickets_.remove_at(i - 1);   // (an expired one is of no use to anyone, a good one is used up)
    if (!expired) return true;
  }
  return false;
}

namespace tls {

bool make_ticket(HashKind hash, uint16_t suite_id, ConstByteView resumption_master, ConstByteView body, StringView server_name, bool quic, uint64_t now_ms, TlsTicket* out) {
  Reader r(body);
  const uint32_t lifetime = (static_cast<uint32_t>(r.u16()) << 16) | r.u16();
  const uint32_t age_add = (static_cast<uint32_t>(r.u16()) << 16) | r.u16();
  const uint8_t nonce_length = r.u8();
  const ConstByteView nonce = r.take(nonce_length);
  const uint16_t ticket_length = r.u16();
  const ConstByteView ticket = r.take(ticket_length);
  const uint16_t extensions_length = r.u16();
  r.take(extensions_length);
  if (!r.done() || ticket_length == 0 || lifetime == 0 || ticket_length > kMaxTicketBytes || server_name.empty()) return false;
  out->quic = quic;
  lower_name(out->server_name, server_name);
  out->suite_id = suite_id;
  out->lifetime_s = lifetime > 604800 ? 604800 : lifetime;
  out->age_add = age_add;
  out->received_ms = now_ms;
  out->ticket_length = ticket_length;
  for (uint32_t i = 0; i < ticket_length; ++i) out->ticket[i] = ticket[i];
  const uint32_t hl = hash_size(hash);
  out->psk_length = static_cast<uint8_t>(hl);
  return hkdf_expand_label(hash, resumption_master, "resumption", nonce, ByteView(out->psk, hl));
}

uint32_t write_psk_modes(uint8_t* out, uint32_t capacity) {
  Writer w(out, capacity);
  w.u16(kExtPskModes);
  w.u16(2);
  w.u8(1);   // one mode:
  w.u8(1);   // psk_dhe_ke
  return w.ok ? w.n : 0;
}

uint32_t write_pre_shared_key(const TlsTicket& ticket, HashKind ticket_hash, uint64_t now_ms, uint8_t* out, uint32_t capacity) {
  const uint32_t hl = hash_size(ticket_hash);
  Writer w(out, capacity);
  w.u16(kExtPreSharedKey);
  const uint32_t length_mark = w.mark16();
  const uint32_t identities_mark = w.mark16();
  w.u16(ticket.ticket_length);
  w.bytes(ticket.ticket, ticket.ticket_length);
  const uint32_t age_ms = static_cast<uint32_t>(now_ms - ticket.received_ms);   // (modulo 2^32, as the RFC wants)
  const uint32_t obfuscated = age_ms + ticket.age_add;
  w.u16(obfuscated >> 16);
  w.u16(obfuscated & 0xffff);
  w.end16(identities_mark);
  const uint32_t binders_mark = w.mark16();
  w.u8(hl);
  for (uint32_t i = 0; i < hl; ++i) w.u8(0);
  w.end16(binders_mark);
  w.end16(length_mark);
  return w.ok ? w.n : 0;
}

void early_secret(HashKind hash, ConstByteView psk, uint8_t* out) {
  hkdf_extract(hash, ConstByteView(), psk, out);   // (no salt is a salt of zeros)
}

bool psk_binder(HashKind hash, ConstByteView psk, ConstByteView truncated, uint8_t* out) {
  const uint32_t hl = hash_size(hash);
  uint8_t early[kMaxHashSize], empty[kMaxHashSize], binder_key[kMaxHashSize], finished_key[kMaxHashSize], digest[kMaxHashSize];
  early_secret(hash, psk, early);
  crypto::hash(hash, ConstByteView(), empty);
  if (!derive_secret(hash, ConstByteView(early, hl), "res binder", ConstByteView(empty, hl), binder_key)) return false;
  if (!hkdf_expand_label(hash, ConstByteView(binder_key, hl), "finished", ConstByteView(), ByteView(finished_key, hl))) return false;
  crypto::hash(hash, truncated, digest);
  crypto::hmac(hash, ConstByteView(finished_key, hl), ConstByteView(digest, hl), out);
  return true;
}

bool resumption_master_secret(HashKind hash, ConstByteView master_secret, ConstByteView transcript_hash, uint8_t* out) {
  return derive_secret(hash, master_secret, "res master", transcript_hash, out);
}

}  // namespace tls
}  // namespace network
