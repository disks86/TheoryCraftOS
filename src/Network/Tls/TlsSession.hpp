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

// TLS 1.3 session resumption (RFC 8446 2.2, 4.2.11, 4.6.1): the tickets servers hand out after a handshake, kept per server name, and the pieces that put one into
// the next ClientHello. A resumed handshake skips the certificate (the ticket's secret proves the server is the one that was checked before) and costs a
// round trip less of computation; the key exchange is still done (psk_dhe_ke), so forward secrecy stays. Tickets are used once, as the RFC advises.
//
//   TlsSessionCache sessions(allocator);
//   TlsConfig config;  config.sessions = &sessions;     // TlsSocket and QuicConnection store tickets in it and offer them to the same server later
#pragma once
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "Crypto/Crypto.hpp"

namespace network {

constexpr uint32_t kMaxTicketBytes = 2048;

struct TlsTicket {
  char server_name[256] = {};     // lower case
  bool quic = false;              // tickets of the two transports are kept apart
  uint16_t suite_id = 0;
  uint32_t lifetime_s = 0, age_add = 0;
  uint64_t received_ms = 0;
  uint16_t ticket_length = 0;
  uint8_t ticket[kMaxTicketBytes] = {};
  uint8_t psk_length = 0;
  uint8_t psk[48] = {};
  ~TlsTicket() {
    for (uint8_t& b : psk) b = 0;
  }
};

class TlsSessionCache {
 public:
  static constexpr uint32_t kMaxTickets = 32, kPerServer = 4;

  explicit TlsSessionCache(allocators::IAllocator& allocator) : allocator_(allocator), tickets_(allocator) {}
  TlsSessionCache(const TlsSessionCache&) = delete;
  TlsSessionCache& operator=(const TlsSessionCache&) = delete;

  // Keeps a copy of the ticket (the oldest ones go when there are too many). False on memory shortage.
  bool store(const TlsTicket& ticket);
  // Takes the newest ticket for the server that has not expired (`now_ms` is the stack's clock); it is removed from the cache.
  bool take(StringView server_name, bool quic, uint64_t now_ms, TlsTicket* out);
  uint32_t count() const {
    kernel::ScopedMutex lock(mutex_);
    return static_cast<uint32_t>(tickets_.count());
  }
  void clear() {
    kernel::ScopedMutex lock(mutex_);
    tickets_.clear();
  }

 private:
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  List<UniquePointer<TlsTicket>> tickets_;
};

namespace tls {

// A NewSessionTicket message body (RFC 8446 4.6.1) and the resumption master secret of the connection it came on, made into a ticket the cache can keep.
// False if the message is damaged, or the ticket is of no use (no lifetime, too long to keep): the connection goes on either way.
bool make_ticket(crypto::HashKind hash, uint16_t suite_id, ConstByteView resumption_master, ConstByteView body, StringView server_name, bool quic, uint64_t now_ms, TlsTicket* out);

// The psk_key_exchange_modes extension (psk_dhe_ke): a ClientHello that wants tickets sent (not only one that offers a ticket) has it, or servers send none.
// Written to `out` (`capacity` bytes); returns its length, 0 if it does not fit.
uint32_t write_psk_modes(uint8_t* out, uint32_t capacity);
// The pre_shared_key extension with one identity, which a resuming ClientHello ends with, with the binder zeroed; returns its length, 0 if it does not fit.
// The binder entry is the last `1 + hash_size` bytes of it.
uint32_t write_pre_shared_key(const TlsTicket& ticket, crypto::HashKind ticket_hash, uint64_t now_ms, uint8_t* out, uint32_t capacity);

// The binder for a ClientHello: `truncated` is the message up to, not including, the binders list of the pre_shared_key extension (RFC 8446 4.2.11.2).
// For a first ClientHello (the only kind that resumes) the transcript is just this message. Writes hash_size(hash) bytes.
bool psk_binder(crypto::HashKind hash, ConstByteView psk, ConstByteView truncated, uint8_t* out);

// The early secret of the key schedule for a PSK (HKDF-Extract with a zero salt).
void early_secret(crypto::HashKind hash, ConstByteView psk, uint8_t* out);

// The resumption master secret from the master secret and the transcript hash up to and including the client's Finished.
bool resumption_master_secret(crypto::HashKind hash, ConstByteView master_secret, ConstByteView transcript_hash, uint8_t* out);

}  // namespace tls
}  // namespace network
