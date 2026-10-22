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

// A TLS 1.3 client handshake (RFC 8446) that is not tied to a record layer: handshake messages go in and out as plain bytes, at the encryption level they
// belong to, and the traffic secrets are handed over when they exist. QUIC (RFC 9001) carries the handshake in CRYPTO frames and protects its packets with
// keys made from these secrets; TlsSocket's TCP records do the same job for HTTPS with its own copy of this logic.
//
//   Tls13Client tls(allocator, config, server_ipv4, transport_parameters);
//   tls.start();                                   // the ClientHello is now in pending_output(Initial)
//   tls.receive(TlsLevel::Initial, bytes);         // the server's ServerHello, then Handshake bytes, then Application bytes (tickets)
//   if (tls.has_secrets(TlsLevel::Handshake)) ...  // install the handshake packet keys from client_secret()/server_secret()
//
// The certificate is checked as for TlsSocket (Network/Tls/TlsCertificate.hpp). HelloRetryRequest, key updates by the transport and ALPN are handled;
// session resumption with tickets (not 0-RTT) is; client certificates (an empty answer is given to a request for one), 0-RTT and TLS 1.2 are not. A server that does not choose TLS 1.3 fails the handshake.
#pragma once
#include "Crypto/Crypto.hpp"
#include "Network/Tls/TlsKeys.hpp"
#include "Network/Tls/TlsSession.hpp"
#include "Network/Tls/TlsSocket.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace network {

// The encryption levels QUIC has (0-RTT is not used): the Initial packets protect the hellos, Handshake packets the rest of the handshake, 1-RTT packets the data.
enum class TlsLevel : uint8_t { Initial = 0, Handshake = 1, Application = 2 };

class Tls13Client {
 public:
  // `config` supplies the server name (SNI and certificate check), the trust store, random source and clock, the ALPN protocols (a comma separated list)
  // and the certificate rules. `quic_parameters` is the content of the quic_transport_parameters extension (empty: the extension is left out).
  Tls13Client(allocators::IAllocator& allocator, const TlsConfig& config, uint32_t server_ipv4, ConstByteView quic_parameters = ConstByteView());
  ~Tls13Client();
  Tls13Client(const Tls13Client&) = delete;
  Tls13Client& operator=(const Tls13Client&) = delete;

  // Builds the ClientHello. False (failed()) if there is no random source or memory ran out.
  bool start();
  // Takes handshake bytes the server sent at `level` (in order; a message may be split over calls). False once the handshake has failed.
  bool receive(TlsLevel level, ConstByteView data);

  // Handshake bytes waiting to be sent at a level (in CRYPTO frames); say how many were taken with consume_output().
  ConstByteView pending_output(TlsLevel level) const { return out_[static_cast<uint32_t>(level)].view().subview(out_taken_[static_cast<uint32_t>(level)]); }
  void consume_output(TlsLevel level, uint64_t bytes);

  // Traffic secrets (hash length bytes each): available at Handshake once the ServerHello has been read, at Application once the server's Finished has.
  bool has_secrets(TlsLevel level) const { return level != TlsLevel::Initial && have_secrets_[static_cast<uint32_t>(level)]; }
  ConstByteView client_secret(TlsLevel level) const { return ConstByteView(client_secret_[static_cast<uint32_t>(level)], hash_length_); }
  ConstByteView server_secret(TlsLevel level) const { return ConstByteView(server_secret_[static_cast<uint32_t>(level)], hash_length_); }
  const tls::CipherSuite* suite() const { return suite_; }

  // With a session cache in the configuration (TlsConfig::sessions) the client offers a ticket from an earlier connection to the same server and keeps the
  // tickets this one is sent; both need a clock in milliseconds, which is given here (before start()). The handshake was a resumption: no certificate came.
  void set_time_source(uint64_t (*now_ms)(void* context), void* context) {
    now_fn_ = now_ms;
    now_context_ = context;
  }
  bool resumed() const { return psk_accepted_; }

  // The handshake is over from our side (our Finished is queued) once this is true.
  bool handshake_complete() const { return state_ == St::Connected; }
  bool failed() const { return state_ == St::Failed; }
  TlsError error() const { return error_; }
  // The alert that goes with a failure (a QUIC connection closes with the error 0x100 + alert).
  uint8_t alert() const { return alert_; }
  crypto::CertStatus certificate_status() const { return cert_status_; }
  // The server's quic_transport_parameters (once EncryptedExtensions has been read).
  ConstByteView peer_parameters() const { return peer_parameters_.view(); }
  StringView alpn_selected() const { return StringView(alpn_selected_, alpn_length_); }
  ConstByteView server_certificate() const { return leaf_.view(); }

 private:
  bool certificate_requested_ = false;
  // resumption
  TlsTicket ticket_;
  bool have_ticket_ = false, psk_offered_ = false, psk_accepted_ = false, have_resumption_ = false;
  uint8_t resumption_master_[48] = {};
  uint64_t (*now_fn_)(void* context) = nullptr;
  void* now_context_ = nullptr;
  enum class St : uint8_t { Idle, ServerHello, EncryptedExtensions, CertificateOrRequest, Certificate, CertificateVerify, Finished, Connected, Failed };
  struct Share {
    uint16_t group = 0;
    bool have = false;
    uint8_t priv[48] = {};
    uint8_t pub[97] = {};
    uint32_t pub_length = 0;
  };

  bool fail(TlsError error, uint8_t alert);
  bool process(TlsLevel level);
  bool handle_message(TlsLevel level, uint8_t type, ConstByteView body, ConstByteView whole);
  bool make_share(uint16_t group, Share* share);
  bool build_client_hello(bool retry);
  void send(TlsLevel level, ConstByteView message);
  void transcript_add(ConstByteView message);
  void transcript_hash(uint8_t* out);
  bool on_server_hello(ConstByteView body, ConstByteView whole);
  bool on_hello_retry_request(ConstByteView whole, uint16_t suite_id, uint16_t group, ConstByteView cookie);
  bool derive_handshake_secrets(ConstByteView shared);
  bool on_encrypted_extensions(ConstByteView body);
  bool on_certificate(ConstByteView body);
  bool on_certificate_verify(ConstByteView body);
  bool on_finished(ConstByteView body, ConstByteView whole);

  allocators::IAllocator& allocator_;
  TlsConfig config_;
  char server_name_[256] = {};
  String alpn_list_;
  uint32_t server_ipv4_;
  ByteBuffer quic_parameters_, peer_parameters_, leaf_;
  St state_ = St::Idle;
  TlsError error_ = TlsError::None;
  uint8_t alert_ = 0;
  crypto::CertStatus cert_status_ = crypto::CertStatus::Ok;
  ByteBuffer in_[3], out_[3];
  uint64_t out_taken_[3] = {};
  crypto::Hasher t256_, t384_;
  uint8_t client_random_[32] = {};
  Share shares_[3];
  uint32_t share_count_ = 0;
  uint16_t selected_group_ = 0;
  uint8_t cookie_[256] = {};
  uint32_t cookie_length_ = 0;
  bool retried_ = false, saw_certificate_ = false;
  const tls::CipherSuite* suite_ = nullptr;
  uint32_t hash_length_ = 32;
  uint8_t handshake_secret_[48] = {}, master_secret_[48] = {};
  uint8_t client_secret_[3][48] = {}, server_secret_[3][48] = {};
  bool have_secrets_[3] = {};
  char alpn_selected_[32] = {};
  uint32_t alpn_length_ = 0;
};

}  // namespace network
