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

// The client side of the SSH transport (RFC 4253), authentication (RFC 4252, passwords) and one session channel (RFC 4254) over a TCP connection,
// enough to run a subsystem such as SFTP on. Polled like the sockets underneath: every call waits for the network (polling the stack) and gives
// up when nothing has moved for 30 seconds. One thread at a time.
//   Key exchange: curve25519-sha256. Host keys: ssh-ed25519, ecdsa-sha2-nistp256, rsa-sha2-256 / -512. Ciphers: aes128-ctr, aes256-ctr.
//   MACs: hmac-sha2-256 / -512 (also the encrypt-then-MAC forms). No compression. The "strict key exchange" countermeasure is used when the
//   server offers it.
// Not supported: public key logins, keyboard-interactive, several channels, rekeying started by this side (a server's is answered), port forwarding.
#pragma once
#include "Crypto/Crypto.hpp"
#include "Crypto/Random.hpp"
#include "FileSystem/FileSystemTypes.hpp"
#include "Network/HostTrust.hpp"
#include "Network/INetworkStack.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

class SshSession {
 public:
  SshSession(INetworkStack& stack, SharedPointer<ITcpSocket> socket, allocators::IAllocator& allocator, crypto::IRandom& random);
  ~SshSession();
  SshSession(const SshSession&) = delete;
  SshSession& operator=(const SshSession&) = delete;

  // Exchanges versions and keys, and checks the server's host key signature. The key's fingerprint is looked up in `trust` under `trust_key`:
  // a key that is not the one seen before gives Status::UntrustedHost. Network for a connection that failed, Unsupported if no algorithms
  // are shared (or the server is not SSH 2).
  filesystem::Status handshake(HostTrust& trust, StringView trust_key);
  // Password login. LoginFailed if refused.
  filesystem::Status login_password(StringView user, StringView password);
  // Opens a session channel and starts the subsystem (such as "sftp") in it; then send() and receive() move its data.
  filesystem::Status open_subsystem(StringView name);

  // The channel's data: send() waits for the server's window; receive() waits for exactly `count` bytes.
  filesystem::Status send(ConstByteView data);
  filesystem::Status receive(uint8_t* out, uint32_t count);

  bool alive() const { return alive_; }
  // What was agreed, for tests and logs.
  const char* host_key_algorithm() const { return host_key_name_; }
  const char* cipher_name() const { return cipher_name_; }
  const char* mac_name() const { return mac_name_; }

 private:
  struct Direction {
    bool active = false;
    bool etm = false;
    uint32_t block = 8;
    uint32_t mac_length = 0;
    crypto::HashKind mac_hash = crypto::HashKind::Sha256;
    uint8_t mac_key[64] = {};
    crypto::Aes aes;
    uint8_t counter[16] = {};
    uint8_t stream[16] = {};
    uint32_t stream_used = 16;
    uint32_t sequence = 0;
    void crypt(ByteView data);
  };

  filesystem::Status write_all(ConstByteView data);
  filesystem::Status read_exact(uint8_t* out, uint32_t count);
  filesystem::Status send_packet(ConstByteView payload);
  filesystem::Status receive_packet(ByteBuffer* payload);
  // The next message that is for the caller: IGNORE, DEBUG and global requests are dealt with here.
  filesystem::Status next_message(ByteBuffer* message);
  // The next message that is not about the channel's data and window (those are taken in); with `return_after_handled` it also returns after
  // each one that was taken in (the caller looks at what has arrived).
  filesystem::Status next_channel_message(ByteBuffer* message, bool return_after_handled = false);
  filesystem::Status take_channel_message(ConstByteView message, bool* handled);
  void fail();
  bool derive(const uint8_t* shared_mpint, uint32_t shared_length, const uint8_t* exchange_hash, char letter, ByteView out);
  bool verify_host_key(StringView algorithm, ConstByteView host_key, ConstByteView signature, const uint8_t* exchange_hash);
  filesystem::Status adjust_window();

  INetworkStack& stack_;
  SharedPointer<ITcpSocket> socket_;
  allocators::IAllocator& allocator_;
  crypto::IRandom& random_;
  bool alive_ = true;
  uint64_t last_progress_ms_ = 0;
  Direction out_, in_;
  uint8_t session_id_[32] = {};
  bool have_session_id_ = false;
  bool strict_ = false;
  char host_key_name_[32] = {}, cipher_name_[32] = {}, mac_name_[48] = {};

  // the channel
  uint32_t remote_channel_ = 0, remote_window_ = 0, remote_max_packet_ = 0;
  uint32_t local_window_ = 0;
  bool channel_open_ = false, channel_closed_ = false;
  ByteBuffer rx_;
  uint64_t rx_at_ = 0;
};

}  // namespace network
