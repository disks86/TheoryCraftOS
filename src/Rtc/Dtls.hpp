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

// DTLS 1.2 (RFC 6347) for WebRTC: the handshake of RFC 8827 / 5764 - TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 on P-256 with a self-signed certificate on both sides (each checks
// the other's against the fingerprint from the session description), extended master secret (RFC 7627), the use_srtp extension, and the export of the SRTP keys. Application
// data carries SCTP. Datagram in, datagram out: the owner passes what arrives on the ICE path to receive() and sends what next_datagram() gives, and calls tick() for
// the retransmission timer.
#pragma once
#include "Crypto/Aead.hpp"
#include "Crypto/Ecc.hpp"
#include "Crypto/Random.hpp"
#include "ObjectContainer/List.hpp"
#include "Rtc/RtcBytes.hpp"
#include "Rtc/Srtp.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace rtc {

// The key pair and self-signed certificate a peer connection presents (RTCCertificate).
class DtlsIdentity {
 public:
  explicit DtlsIdentity(allocators::IAllocator& allocator) : allocator_(allocator), certificate_(allocator) {}
  // A new P-256 key and a certificate for it, valid 2024 to 2049 (`common_name` is only a label).
  bool generate(crypto::IRandom& random, StringView common_name);
  bool ready() const { return ready_; }
  ConstByteView certificate() const { return certificate_.view(); }
  const uint8_t* private_key() const { return private_key_; }
  // SHA-256 of the certificate, as "AB:CD:...".
  void fingerprint(String* out) const;

 private:
  allocators::IAllocator& allocator_;
  ByteBuffer certificate_;
  uint8_t private_key_[32] = {};
  bool ready_ = false;
};

// "AB:CD:.." <-> bytes (upper or lower case); false for anything else.
bool fingerprint_bytes(StringView text, uint8_t out[32]);

class DtlsTransport {
 public:
  enum class Role : uint8_t { Client, Server };
  enum class State : uint8_t { New, Connecting, Connected, Failed, Closed };

  DtlsTransport(allocators::IAllocator& allocator, const DtlsIdentity& identity, crypto::IRandom& random);

  void set_role(Role role) { role_ = role; }
  Role role() const { return role_; }
  // The fingerprint the peer's certificate must have (SDP a=fingerprint). Without one the handshake fails.
  bool set_expected_fingerprint(StringView algorithm, StringView hex_colon);
  State state() const { return state_; }

  // The client sends its hello; the server just waits for the peer's.
  void start(uint64_t now_ms);
  void receive(ConstByteView datagram, uint64_t now_ms);
  bool next_datagram(ByteBuffer* out);
  void tick(uint64_t now_ms);
  void close();

  bool send_application(ConstByteView data);
  bool receive_application(ByteBuffer* out);

  // After the handshake: the profile and the keys of each side (the client's is what the client protects with).
  bool srtp_keys(SrtpProfile* profile, uint8_t client_key[32], uint8_t server_key[32], uint8_t client_salt[14], uint8_t server_salt[14]) const;
  // Anything the owner may want to say about why the handshake failed.
  const char* failure() const { return failure_; }

 private:
  enum class Step : uint8_t {
    Idle, ServerHello, Certificate, KeyExchange, HelloDone,   // the client waiting for ...
    ClientHello, ClientCertificate, ClientKeyExchange, CertificateVerify,   // the server waiting for ...
    ChangeCipherSpec, Finished, Done
  };
  struct Keys {
    crypto::AesGcm gcm;
    uint8_t iv[4] = {};
  };

  bool fail(const char* why, uint8_t alert);
  void alert(uint8_t level, uint8_t description);
  bool send_record(uint8_t type, ConstByteView payload, bool flight, int epoch_override = -1);
  bool send_handshake(uint8_t type, ConstByteView body);
  bool unprotect(uint8_t type, uint16_t epoch, uint64_t sequence, ConstByteView in, ByteBuffer* plain);
  void handle_record(uint8_t type, uint16_t epoch, uint64_t sequence, ConstByteView body, uint64_t now_ms);
  void handle_handshake_fragment(ConstByteView body);
  bool handle_message(uint8_t type, ConstByteView body, ConstByteView whole_dtls_form);
  void add_transcript(uint8_t type, uint16_t sequence, ConstByteView body);
  void transcript_hash(uint8_t out[32]) const;
  void begin_flight();
  void resend_flight();
  bool derive_keys(ConstByteView premaster);
  bool send_client_flight();
  bool send_server_flight();
  bool send_finished();
  bool check_finished(ConstByteView body);
  bool take_certificate(ConstByteView body, bool* ok);
  bool sign(const uint8_t digest[32], ByteBuffer* der);
  bool verify(const uint8_t digest[32], uint16_t scheme, ConstByteView signature);
  bool parse_hello_extensions(ConstByteView ext, bool server);
  void hello_extensions(ByteBuffer* out, bool server);

  allocators::IAllocator& allocator_;
  const DtlsIdentity& identity_;
  crypto::IRandom& random_;
  Role role_ = Role::Client;
  State state_ = State::New;
  Step step_ = Step::Idle;
  const char* failure_ = "";
  uint8_t expected_fp_[32] = {};
  bool has_expected_ = false;
  // epochs and sequence numbers
  uint16_t write_epoch_ = 0, read_epoch_ = 0;
  uint64_t write_seq_[2] = {}, read_window_[2] = {}, read_highest_[2] = {};
  bool read_seen_[2] = {};
  uint16_t send_message_seq_ = 0, recv_message_seq_ = 0;
  Keys write_keys_, read_keys_;
  bool keys_ready_ = false;
  // handshake
  ByteBuffer transcript_, fragment_, fragment_have_;
  uint8_t fragment_type_ = 0;
  uint32_t fragment_length_ = 0;
  uint8_t client_random_[32] = {}, server_random_[32] = {}, master_[48] = {};
  uint8_t ephemeral_private_[32] = {};
  uint8_t peer_point_[65] = {}, own_point_[65] = {};
  bool use_ems_ = false, peer_ems_ = false, cert_requested_ = false, peer_cert_ok_ = false, have_peer_key_ = false;
  uint8_t peer_public_[65] = {};
  uint16_t srtp_profile_ = 0;
  uint8_t cookie_[32] = {};
  uint32_t cookie_size_ = 0;
  // output
  List<UniquePointer<ByteBuffer>> outbox_, flight_, app_in_;
  uint64_t retransmit_at_ = 0, timeout_ms_ = 1000, last_resend_ms_ = 0;
  uint32_t retransmits_ = 0;
  uint64_t now_ms_ = 0;
};

}  // namespace rtc
