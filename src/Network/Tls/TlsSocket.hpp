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
#include "Crypto/Random.hpp"
#include "Crypto/X509.hpp"
#include "Network/ByteRing.hpp"
#include "Network/INetworkStack.hpp"
#include "Network/Tls/TlsKeys.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace network {

enum class TlsError : uint8_t {
  None,
  Transport,      // the TCP connection failed or closed before the handshake was done
  Timeout,        // the handshake took too long
  Protocol,       // the peer sent something that is not valid TLS, or in the wrong order
  Alert,          // the peer sent a fatal alert (alert_received())
  BadRecord,      // a record failed authentication or was too large
  Certificate,    // the server's certificate was refused (certificate_status())
  Unsupported,    // no version, cipher suite, group or signature algorithm in common
  Internal,       // out of memory, or no random numbers
  Truncated,      // the connection was closed without close_notify (after the handshake)
};

struct TlsConfig {
  const char* server_name = nullptr;           // sent as SNI and checked against the certificate; copied
  uint32_t server_ipv4 = 0;                    // checked against the certificate's IP addresses when there is no name (connect() fills it in)
  // INSECURE: with neither a name nor an address to check (an inner socket without server_name), accept a certificate whatever host it names.
  // Without this the handshake fails then: a valid certificate for some other host proves nothing.
  bool skip_hostname_check = false;
  const crypto::TrustStore* trust = nullptr;   // the certificates to trust (must outlive the socket)
  // INSECURE: accept any certificate (only passive eavesdropping is then prevented). The default is to verify.
  bool verify_certificates = true;
  // The current time as seconds since 1970 for certificate dates, or null to skip the date checks.
  uint64_t (*clock)(void* context) = nullptr;
  void* clock_context = nullptr;
  crypto::IRandom* random = nullptr;           // required (must outlive the socket)
  bool allow_tls12 = true, allow_tls13 = true;
  bool require_extended_master_secret = false; // TLS 1.2: refuse servers that do not do RFC 7627
  uint32_t min_rsa_bits = 2048;
  bool allow_sha1_certificates = false;
  uint32_t handshake_timeout_ms = 30000;
  const char* alpn = nullptr;                  // one application protocol to offer ("http/1.1", "ipp"); copied
};

// A TLS client connection that looks like a TCP socket: everything that takes an ITcpSocket (the print transports, HTTP, ...) can be given a
// TlsSocket instead. Over an inner TCP connection, polled like it: the handshake and the record layer advance whenever one of the calls below
// is made while the network stack is being polled. is_connected() (and state() == Established) only turn true once the handshake is done and
// the server's certificate was accepted; send() before that returns 0. TLS 1.2 and 1.3 with ECDHE (X25519, P-256, P-384) and AEAD suites
// (AES-GCM, ChaCha20-Poly1305); client certificates, session resumption, 0-RTT, renegotiation and SSL 3.0 / TLS 1.0 / 1.1 are not supported.
class TlsSocket final : public ITcpSocket {
 public:
  // `inner` is an existing connection (connected or connecting); leave it empty and call connect() to have the socket opened for you.
  TlsSocket(INetworkStack& stack, allocators::IAllocator& allocator, const TlsConfig& config, SharedPointer<ITcpSocket> inner = SharedPointer<ITcpSocket>());
  ~TlsSocket();
  TlsSocket(const TlsSocket&) = delete;
  TlsSocket& operator=(const TlsSocket&) = delete;

  // ITcpSocket
  TcpState state() const override;
  bool connect(Ipv4Address address, uint16_t port) override;
  bool listen(uint16_t) override { return false; }
  SharedPointer<ITcpSocket> accept() override { return SharedPointer<ITcpSocket>(); }
  uint32_t send(ConstByteView data) override;
  uint32_t receive(ByteView buffer) override;
  uint32_t available() const override;
  uint32_t unsent_or_unacknowledged() const override;
  bool is_connected() const override;
  bool peer_closed() const override;
  bool failed() const override;
  void close() override;
  void abort() override;
  uint16_t local_port() const override { return inner_ ? inner_->local_port() : 0; }
  Ipv4Address remote_address() const override { return inner_ ? inner_->remote_address() : Ipv4Address(); }
  uint16_t remote_port() const override { return inner_ ? inner_->remote_port() : 0; }

  // TLS details
  bool handshake_complete() const { return state_ == St::Open || state_ == St::Closed; }
  TlsError error() const { return error_; }
  uint8_t alert_received() const { return alert_received_; }
  crypto::CertStatus certificate_status() const { return cert_status_; }
  uint16_t protocol_version() const { return tls13_ ? 0x0304 : 0x0303; }   // meaningful once the handshake is done
  const char* cipher_suite_name() const { return suite_ ? suite_->name : ""; }
  const char* alpn_selected() const { return alpn_selected_; }
  bool extended_master_secret() const { return ems_; }
  // The server's certificate (DER) once the handshake is done: what a program that pins certificates compares.
  ConstByteView server_certificate() const { return server_certificate_.view(); }

 private:
  enum class St : uint8_t {
    Idle, Connecting, ServerHello,
    EncryptedExtensions, CertificateOrRequest13, Certificate13, CertificateVerify13, Finished13,
    Certificate12, KeyExchange12, RequestOrDone12, Done12, ChangeCipherSpec12, Finished12,
    Open, Closed, Failed,
  };
  struct Handshake;

  void pump();                      // all the work: send queued bytes, read and process records, advance the handshake
  bool read_records();
  bool handle_record(uint8_t type, uint8_t* payload, uint32_t length);
  bool handle_handshake_data(ConstByteView data);
  bool handle_message(uint8_t type, ConstByteView body, ConstByteView whole);
  bool handle_alert(ConstByteView data);
  void flush();
  bool start_handshake();
  bool send_record(uint8_t type, ConstByteView data);
  bool send_handshake(const uint8_t* message, uint32_t length);
  void send_alert(uint8_t level, uint8_t description);
  void fail(TlsError error, uint8_t alert);
  bool build_client_hello(bool retry);

  bool on_server_hello(ConstByteView body, ConstByteView whole);
  bool on_hello_retry_request(ConstByteView body, ConstByteView whole, uint16_t suite_id, uint16_t group, ConstByteView cookie);
  bool derive_handshake_keys_13(ConstByteView shared);
  bool on_encrypted_extensions(ConstByteView body);
  bool on_certificate(ConstByteView body, bool tls13);
  bool on_certificate_verify(ConstByteView body);
  bool on_finished_13(ConstByteView body, ConstByteView whole);
  bool on_server_key_exchange(ConstByteView body);
  bool on_server_hello_done();
  bool on_finished_12(ConstByteView body);
  bool on_post_handshake(uint8_t type, ConstByteView body);
  bool verify_signature_with_leaf(uint16_t scheme, const uint8_t* digest_content, uint32_t content_length, ConstByteView signature, bool tls13);
  bool set_traffic_keys(tls::RecordProtection& side, const uint8_t* secret);
  void transcript_hash(uint8_t* out);
  void transcript_add(ConstByteView message);
  bool update_keys_13(bool send_side);
  void finish_handshake();

  INetworkStack& stack_;
  allocators::IAllocator& allocator_;
  TlsConfig config_;
  char server_name_[256] = {};
  char alpn_[32] = {};
  SharedPointer<ITcpSocket> inner_;
  St state_ = St::Idle;
  TlsError error_ = TlsError::None;
  uint8_t alert_received_ = 0;
  crypto::CertStatus cert_status_ = crypto::CertStatus::Ok;
  uint64_t started_ms_ = 0;

  UniquePointer<Handshake> hs_;
  ByteBuffer server_certificate_;
  const tls::CipherSuite* suite_ = nullptr;
  bool tls13_ = false, ems_ = false;
  char alpn_selected_[32] = {};
  tls::RecordProtection read_, write_;
  // after the handshake (TLS 1.3 key updates)
  uint8_t client_traffic_secret_[48] = {}, server_traffic_secret_[48] = {};

  // record input
  uint8_t rx_[tls::kRecordHeader + tls::kMaxRecordPayload + 64];
  uint32_t rx_length_ = 0;
  uint8_t tx_[tls::kRecordHeader + tls::kMaxPlaintext + 512];
  ByteBuffer handshake_data_;
  uint32_t handshake_pos_ = 0;   // bytes of handshake_data_ already processed
  bool pumping_ = false;
  network::ByteRing in_, out_;
  bool peer_closed_ = false, sent_close_ = false;
};

}  // namespace network
