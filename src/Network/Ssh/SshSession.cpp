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

#include "Network/Ssh/SshSession.hpp"
#include "Crypto/Aead.hpp"
#include "Crypto/Ecc.hpp"
#include "Crypto/Rsa.hpp"
#include "Kernel/Thread.hpp"
#include "Network/Ssh/SshWire.hpp"
#include "Utility/Move.hpp"


namespace network {

using filesystem::Status;
using namespace crypto;

namespace {

constexpr uint32_t kStallMs = 30000;
constexpr uint32_t kMaxPacket = 262144;
constexpr uint32_t kWindow = 2u << 20;
constexpr uint32_t kChannelPacket = 32768;
constexpr const char* kVersion = "SSH-2.0-TheoryCraftOS";

constexpr uint8_t kDisconnect = 1, kIgnore = 2, kUnimplemented = 3, kDebug = 4, kServiceRequest = 5, kServiceAccept = 6, kKexInit = 20, kNewKeys = 21,
                  kEcdhInit = 30, kEcdhReply = 31, kUserauthRequest = 50, kUserauthFailure = 51, kUserauthSuccess = 52, kUserauthBanner = 53,
                  kUserauthChangeRequest = 60, kGlobalRequest = 80, kRequestFailure = 82, kChannelOpen = 90, kChannelConfirm = 91, kChannelOpenFailure = 92,
                  kChannelWindowAdjust = 93, kChannelData = 94, kChannelExtendedData = 95, kChannelEof = 96, kChannelClose = 97, kChannelRequest = 98,
                  kChannelSuccess = 99, kChannelFailure = 100;

const char* const kKexAlgorithms[] = {"curve25519-sha256", "curve25519-sha256@libssh.org"};
const char* const kHostKeyAlgorithms[] = {"ssh-ed25519", "ecdsa-sha2-nistp256", "rsa-sha2-512", "rsa-sha2-256"};
const char* const kCiphers[] = {"aes128-ctr", "aes256-ctr"};
const char* const kMacs[] = {"hmac-sha2-256-etm@openssh.com", "hmac-sha2-512-etm@openssh.com", "hmac-sha2-256", "hmac-sha2-512"};

// Writes a name list: the names joined with commas, then `extra` (if any).
bool write_list(SshWriter& w, allocators::IAllocator& allocator, const char* const* names, uint32_t count, const char* extra = nullptr) {
  String joined(allocator);
  for (uint32_t i = 0; i < count; ++i) {
    if ((i && !joined.push_back(',')) || !joined.append(StringView(names[i]))) return false;
  }
  if (extra && ((!joined.empty() && !joined.push_back(',')) || !joined.append(StringView(extra)))) return false;
  w.string(joined.view());
  return true;
}

// The first of our names (in our order) that the other side lists.
const char* pick(const char* const* ours, uint32_t count, StringView theirs) {
  for (uint32_t i = 0; i < count; ++i)
    if (ssh_list_has(theirs, StringView(ours[i]))) return ours[i];
  return nullptr;
}

void copy_name(char* out, uint32_t capacity, const char* name) {
  uint32_t i = 0;
  for (; name[i] && i + 1 < capacity; ++i) out[i] = name[i];
  out[i] = 0;
}

bool names_equal(const char* a, const char* b) { return StringView(a).equals(StringView(b)); }

// 32 bytes, big endian, from a magnitude of at most 32 bytes.
bool pad32(ConstByteView in, uint8_t out[32]) {
  if (in.size() > 32) return false;
  for (uint32_t i = 0; i < 32; ++i) out[i] = 0;
  for (uint64_t i = 0; i < in.size(); ++i) out[32 - in.size() + i] = in[i];
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------- the packet layer

void SshSession::Direction::crypt(ByteView data) {
  for (uint8_t& b : data) {
    if (stream_used == 16) {
      aes.encrypt_block(counter, stream);
      for (int i = 15; i >= 0; --i)
        if (++counter[i] != 0) break;
      stream_used = 0;
    }
    b ^= stream[stream_used++];
  }
}

SshSession::SshSession(INetworkStack& stack, SharedPointer<ITcpSocket> socket, allocators::IAllocator& allocator, IRandom& random)
    : stack_(stack), socket_(util::move(socket)), allocator_(allocator), random_(random), rx_(allocator) {
  last_progress_ms_ = stack.now_ms();
}

SshSession::~SshSession() {
  if (alive_ && out_.active) {
    ByteBuffer bye(allocator_);
    SshWriter w(&bye);
    w.u8(kDisconnect);
    w.u32(11);   // SSH_DISCONNECT_BY_APPLICATION
    w.string("");
    w.string("");
    if (w.ok()) send_packet(bye.view());
  }
  if (socket_) socket_->close();
}

void SshSession::fail() {
  alive_ = false;
  if (socket_) socket_->abort();
}

Status SshSession::write_all(ConstByteView data) {
  while (!data.empty()) {
    if (socket_->failed()) {
      fail();
      return Status::Network;
    }
    const uint32_t n = socket_->send(data);
    if (n > 0) {
      data = data.subview(n);
      last_progress_ms_ = stack_.now_ms();
      continue;
    }
    if (stack_.now_ms() - last_progress_ms_ > kStallMs) {
      fail();
      return Status::Network;
    }
    stack_.poll();
    kernel::sleep_us(100);
  }
  return Status::Ok;
}

Status SshSession::read_exact(uint8_t* out, uint32_t count) {
  uint32_t done = 0;
  while (done < count) {
    const uint32_t n = socket_->receive(ByteView(out + done, count - done));
    if (n > 0) {
      done += n;
      last_progress_ms_ = stack_.now_ms();
      continue;
    }
    if (socket_->failed() || (socket_->peer_closed() && socket_->available() == 0) || stack_.now_ms() - last_progress_ms_ > kStallMs) {
      fail();
      return Status::Network;
    }
    stack_.poll();
    kernel::sleep_us(100);
  }
  return Status::Ok;
}

Status SshSession::send_packet(ConstByteView payload) {
  if (!alive_) return Status::Network;
  const uint32_t block = out_.active ? 16 : 8;
  const uint64_t base = out_.active && out_.etm ? 1 + payload.size() : 5 + payload.size();
  uint32_t padding = static_cast<uint32_t>(block - base % block);
  if (padding < 4) padding += block;
  ByteBuffer packet(allocator_);
  const uint32_t length = static_cast<uint32_t>(1 + payload.size() + padding);
  SshWriter w(&packet);
  w.u32(length);
  w.u8(static_cast<uint8_t>(padding));
  w.raw(payload);
  if (!w.ok() || !packet.put_zeros(padding)) return Status::OutOfMemory;
  if (!random_.fill(ByteView(packet.writable_view().data() + packet.size() - padding, padding))) return Status::IoError;
  uint8_t sequence[4] = {static_cast<uint8_t>(out_.sequence >> 24), static_cast<uint8_t>(out_.sequence >> 16), static_cast<uint8_t>(out_.sequence >> 8),
                         static_cast<uint8_t>(out_.sequence)};
  uint8_t mac[kMaxHashSize];
  ByteView bytes(packet.writable_view().data(), packet.size());
  if (out_.active) {
    ByteBuffer macdata(allocator_);
    if (out_.etm) {
      out_.crypt(bytes.subview(4));
      if (!macdata.append(ConstByteView(sequence, 4)) || !macdata.append(packet.view())) return Status::OutOfMemory;
    } else {
      if (!macdata.append(ConstByteView(sequence, 4)) || !macdata.append(packet.view())) return Status::OutOfMemory;
      out_.crypt(bytes);
    }
    hmac(out_.mac_hash, ConstByteView(out_.mac_key, hash_size(out_.mac_hash)), macdata.view(), mac);
    if (!packet.append(ConstByteView(mac, out_.mac_length))) return Status::OutOfMemory;
  }
  ++out_.sequence;
  return write_all(packet.view());
}

Status SshSession::receive_packet(ByteBuffer* payload) {
  payload->clear();
  uint8_t first[16];
  ByteBuffer body(allocator_);   // everything after the length word, decrypted: padding length, payload, padding
  uint8_t sequence[4] = {static_cast<uint8_t>(in_.sequence >> 24), static_cast<uint8_t>(in_.sequence >> 16), static_cast<uint8_t>(in_.sequence >> 8),
                         static_cast<uint8_t>(in_.sequence)};
  uint8_t mac[kMaxHashSize], expected[kMaxHashSize];
  uint32_t length = 0;
  if (!in_.active) {
    Status s = read_exact(first, 4);
    if (s != Status::Ok) return s;
    length = (uint32_t{first[0]} << 24) | (uint32_t{first[1]} << 16) | (uint32_t{first[2]} << 8) | first[3];
    if (length < 6 || length > kMaxPacket) {
      fail();
      return Status::Network;
    }
    if (!body.put_zeros(length)) return Status::OutOfMemory;
    s = read_exact(body.writable_view().data(), length);
    if (s != Status::Ok) return s;
  } else if (in_.etm) {
    Status s = read_exact(first, 4);
    if (s != Status::Ok) return s;
    length = (uint32_t{first[0]} << 24) | (uint32_t{first[1]} << 16) | (uint32_t{first[2]} << 8) | first[3];
    if (length < 6 || length > kMaxPacket || length % 16 != 0) {
      fail();
      return Status::Network;
    }
    if (!body.put_zeros(length)) return Status::OutOfMemory;
    s = read_exact(body.writable_view().data(), length);
    if (s != Status::Ok) return s;
    s = read_exact(mac, in_.mac_length);
    if (s != Status::Ok) return s;
    ByteBuffer macdata(allocator_);
    if (!macdata.append(ConstByteView(sequence, 4)) || !macdata.append(ConstByteView(first, 4)) || !macdata.append(body.view())) return Status::OutOfMemory;
    hmac(in_.mac_hash, ConstByteView(in_.mac_key, hash_size(in_.mac_hash)), macdata.view(), expected);
    if (!equal_constant_time(mac, expected, in_.mac_length)) {
      fail();
      return Status::Network;
    }
    in_.crypt(ByteView(body.writable_view().data(), body.size()));
  } else {
    Status s = read_exact(first, 16);
    if (s != Status::Ok) return s;
    in_.crypt(ByteView(first, 16));
    length = (uint32_t{first[0]} << 24) | (uint32_t{first[1]} << 16) | (uint32_t{first[2]} << 8) | first[3];
    if (length < 6 || length > kMaxPacket || (length + 4) % 16 != 0) {
      fail();
      return Status::Network;
    }
    if (!body.append(ConstByteView(first + 4, 12)) || !body.put_zeros(length - 12)) return Status::OutOfMemory;
    if (length > 12) {
      s = read_exact(body.writable_view().data() + 12, length - 12);
      if (s != Status::Ok) return s;
      in_.crypt(ByteView(body.writable_view().data() + 12, length - 12));
    }
    s = read_exact(mac, in_.mac_length);
    if (s != Status::Ok) return s;
    ByteBuffer macdata(allocator_);
    if (!macdata.append(ConstByteView(sequence, 4)) || !macdata.append(ConstByteView(first, 4)) || !macdata.append(body.view())) return Status::OutOfMemory;
    hmac(in_.mac_hash, ConstByteView(in_.mac_key, hash_size(in_.mac_hash)), macdata.view(), expected);
    if (!equal_constant_time(mac, expected, in_.mac_length)) {
      fail();
      return Status::Network;
    }
  }
  ++in_.sequence;
  const uint32_t padding = body.view()[0];
  if (padding < 4 || 1 + padding > length) {
    fail();
    return Status::Network;
  }
  if (!payload->append(body.view().subview(1, length - 1 - padding))) return Status::OutOfMemory;
  return Status::Ok;
}

Status SshSession::next_message(ByteBuffer* message) {
  for (;;) {
    Status s = receive_packet(message);
    if (s != Status::Ok) return s;
    if (message->size() == 0) {
      fail();
      return Status::Network;
    }
    switch (message->view()[0]) {
      case kDisconnect:
        fail();
        return Status::Network;
      case kIgnore:
      case kDebug:
      case kUnimplemented:
        continue;
      case kGlobalRequest: {
        SshReader r(message->view());
        r.u8();
        r.string();
        if (r.boolean()) {   // a server's request (host keys, keep alives) that wants an answer: no
          ByteBuffer no(allocator_);
          no.put8(kRequestFailure);
          s = send_packet(no.view());
          if (s != Status::Ok) return s;
        }
        continue;
      }
      default:
        return Status::Ok;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------- key exchange

bool SshSession::derive(const uint8_t* shared, uint32_t shared_length, const uint8_t* exchange_hash, char letter, ByteView out) {
  uint8_t block[kMaxHashSize];
  uint64_t produced = 0;
  while (produced < out.size()) {
    Hasher h(HashKind::Sha256);
    h.update(ConstByteView(shared, shared_length));
    h.update(ConstByteView(exchange_hash, 32));
    if (produced == 0) {
      const uint8_t l = static_cast<uint8_t>(letter);
      h.update(ConstByteView(&l, 1));
      h.update(ConstByteView(session_id_, 32));
    } else {
      h.update(ConstByteView(out.data(), produced));
    }
    h.finish(block);
    const uint64_t take = out.size() - produced < 32 ? out.size() - produced : 32;
    for (uint64_t i = 0; i < take; ++i) out[produced + i] = block[i];
    produced += take;
  }
  return true;
}

bool SshSession::verify_host_key(StringView algorithm, ConstByteView host_key, ConstByteView signature, const uint8_t* exchange_hash) {
  SshReader key(host_key), sig(signature);
  const StringView key_type = key.text(64), sig_type = sig.text(64);
  if (!key.ok() || !sig.ok()) return false;
  if (algorithm.equals("ssh-ed25519")) {
    const ConstByteView pk = key.string(64), sg = sig.string(128);
    if (!key.ok() || !sig.ok() || !key_type.equals("ssh-ed25519") || !sig_type.equals("ssh-ed25519") || pk.size() != 32 || sg.size() != 64) return false;
    return ed25519_verify(pk.data(), ConstByteView(exchange_hash, 32), sg.data());
  }
  if (algorithm.equals("ecdsa-sha2-nistp256")) {
    const StringView curve = key.text(32);
    const ConstByteView point = key.string(256), blob = sig.string(256);
    if (!key.ok() || !sig.ok() || !key_type.equals("ecdsa-sha2-nistp256") || !sig_type.equals("ecdsa-sha2-nistp256") || !curve.equals("nistp256")) return false;
    SshReader rs(blob);
    const ConstByteView r = rs.mpint(40), s = rs.mpint(40);
    uint8_t r32[32], s32[32];
    if (!rs.ok() || !pad32(r, r32) || !pad32(s, s32)) return false;
    uint8_t digest[32];
    hash(HashKind::Sha256, ConstByteView(exchange_hash, 32), digest);
    return ecdsa_verify(EcCurve::P256, point, digest, 32, ConstByteView(r32, 32), ConstByteView(s32, 32));
  }
  if (algorithm.equals("rsa-sha2-256") || algorithm.equals("rsa-sha2-512")) {
    const ConstByteView e = key.mpint(16), n = key.mpint(1024), sg = sig.string(1024);
    if (!key.ok() || !sig.ok() || !key_type.equals("ssh-rsa") || !sig_type.equals(algorithm) || rsa_modulus_bits(n) < 2048) return false;
    const HashKind kind = algorithm.equals("rsa-sha2-256") ? HashKind::Sha256 : HashKind::Sha512;
    uint8_t digest[kMaxHashSize];
    hash(kind, ConstByteView(exchange_hash, 32), digest);
    return rsa_verify(RsaPadding::Pkcs1V15, kind, n, e, digest, sg);
  }
  return false;
}

Status SshSession::handshake(HostTrust& trust, StringView trust_key) {
  last_progress_ms_ = stack_.now_ms();
  // The versions.
  ByteBuffer line(allocator_);
  line.append(ConstByteView(reinterpret_cast<const uint8_t*>(kVersion), StringView(kVersion).size()));
  line.put8('\r');
  line.put8('\n');
  Status s = write_all(line.view());
  if (s != Status::Ok) return s;
  String server_version(allocator_);
  for (uint32_t lines = 0;; ++lines) {   // (a server may say something first; the version line starts with "SSH-")
    server_version.clear();
    for (;;) {
      uint8_t c = 0;
      s = read_exact(&c, 1);
      if (s != Status::Ok) return s;
      if (c == '\n') break;
      if (c != '\r' && !server_version.push_back(static_cast<char>(c))) return Status::OutOfMemory;
      if (server_version.size() > 255) {
        fail();
        return Status::Network;
      }
    }
    if (server_version.view().starts_with(StringView("SSH-"))) break;
    if (lines > 32) {
      fail();
      return Status::Network;
    }
  }
  if (!server_version.view().starts_with(StringView("SSH-2.0-")) && !server_version.view().starts_with(StringView("SSH-1.99-"))) {
    fail();
    return Status::Unsupported;
  }

  // Our KEXINIT.
  ByteBuffer client_init(allocator_);
  {
    SshWriter w(&client_init);
    w.u8(kKexInit);
    uint8_t cookie[16];
    if (!random_.fill(ByteView(cookie, 16))) return Status::IoError;
    w.raw(ConstByteView(cookie, 16));
    bool ok = write_list(w, allocator_, kKexAlgorithms, 2, "kex-strict-c-v00@openssh.com");
    ok = ok && write_list(w, allocator_, kHostKeyAlgorithms, 4);
    ok = ok && write_list(w, allocator_, kCiphers, 2) && write_list(w, allocator_, kCiphers, 2);
    ok = ok && write_list(w, allocator_, kMacs, 4) && write_list(w, allocator_, kMacs, 4);
    static const char* const none[] = {"none"};
    ok = ok && write_list(w, allocator_, none, 1) && write_list(w, allocator_, none, 1);
    w.string("");
    w.string("");
    w.boolean(false);
    w.u32(0);
    if (!ok || !w.ok()) return Status::OutOfMemory;
  }
  s = send_packet(client_init.view());
  if (s != Status::Ok) return s;

  ByteBuffer server_init(allocator_);
  s = next_message(&server_init);
  if (s != Status::Ok) return s;
  if (server_init.view()[0] != kKexInit) {
    fail();
    return Status::Network;
  }
  SshReader si(server_init.view());
  si.u8();
  si.raw(16);
  const StringView kex_list = si.text(), host_list = si.text(), cipher_c2s = si.text(), cipher_s2c = si.text(), mac_c2s = si.text(), mac_s2c = si.text(),
                   comp_c2s = si.text(), comp_s2c = si.text();
  si.text();
  si.text();
  const bool first_follows = si.boolean();
  if (!si.ok()) {
    fail();
    return Status::Network;
  }
  const char* kex = pick(kKexAlgorithms, 2, kex_list);
  const char* host_key = pick(kHostKeyAlgorithms, 4, host_list);
  const char* cipher_out = pick(kCiphers, 2, cipher_c2s);
  const char* cipher_in = pick(kCiphers, 2, cipher_s2c);
  const char* mac_out = pick(kMacs, 4, mac_c2s);
  const char* mac_in = pick(kMacs, 4, mac_s2c);
  if (!kex || !host_key || !cipher_out || !cipher_in || !mac_out || !mac_in || !ssh_list_has(comp_c2s, "none") || !ssh_list_has(comp_s2c, "none")) {
    fail();
    return Status::Unsupported;
  }
  strict_ = ssh_list_has(kex_list, "kex-strict-s-v00@openssh.com");
  copy_name(host_key_name_, sizeof host_key_name_, host_key);
  copy_name(cipher_name_, sizeof cipher_name_, cipher_out);
  copy_name(mac_name_, sizeof mac_name_, mac_out);
  if (first_follows) {   // a guessed key exchange packet follows: ignored unless the server guessed right
    const uint64_t comma_kex = kex_list.find(','), comma_host = host_list.find(',');
    const StringView guess_kex = comma_kex == StringView::npos ? kex_list : kex_list.first(comma_kex);
    const StringView guess_host = comma_host == StringView::npos ? host_list : host_list.first(comma_host);
    if (!guess_kex.equals(StringView(kex)) || !guess_host.equals(StringView(host_key))) {
      ByteBuffer ignored(allocator_);
      s = receive_packet(&ignored);
      if (s != Status::Ok) return s;
    }
  }

  // The Diffie-Hellman exchange.
  uint8_t secret[32], mine[32];
  if (!random_.fill(ByteView(secret, 32))) return Status::IoError;
  x25519_public_key(secret, mine);
  ByteBuffer init(allocator_);
  {
    SshWriter w(&init);
    w.u8(kEcdhInit);
    w.string(ConstByteView(mine, 32));
    if (!w.ok()) return Status::OutOfMemory;
  }
  s = send_packet(init.view());
  if (s != Status::Ok) return s;
  ByteBuffer reply(allocator_);
  s = next_message(&reply);
  if (s != Status::Ok) return s;
  SshReader r(reply.view());
  if (r.u8() != kEcdhReply) {
    fail();
    return Status::Network;
  }
  const ConstByteView host_key_blob = r.string(8192), theirs = r.string(64), signature = r.string(8192);
  if (!r.ok() || theirs.size() != 32) {
    fail();
    return Status::Network;
  }
  uint8_t shared[32];
  if (!x25519_shared_secret(secret, theirs.data(), shared)) {
    fail();
    return Status::Network;
  }
  ByteBuffer shared_mpint(allocator_), hashed(allocator_);
  {
    SshWriter k(&shared_mpint);
    k.mpint(ConstByteView(shared, 32));
    SshWriter w(&hashed);
    w.string(StringView(kVersion));
    w.string(server_version.view());
    w.string(client_init.view());
    w.string(server_init.view());
    w.string(host_key_blob);
    w.string(ConstByteView(mine, 32));
    w.string(theirs);
    w.raw(shared_mpint.view());
    if (!k.ok() || !w.ok()) return Status::OutOfMemory;
  }
  uint8_t exchange_hash[32];
  hash(HashKind::Sha256, hashed.view(), exchange_hash);
  if (!verify_host_key(StringView(host_key), host_key_blob, signature, exchange_hash)) {
    fail();
    return Status::UntrustedHost;
  }
  uint8_t fingerprint[32];
  hash(HashKind::Sha256, host_key_blob, fingerprint);
  const HostTrust::Result trusted = trust.check(trust_key, fingerprint);
  if (trusted == HostTrust::Result::Changed) {
    fail();
    return Status::UntrustedHost;
  }
  if (trusted == HostTrust::Result::OutOfMemory) return Status::OutOfMemory;
  if (!have_session_id_) {
    for (int i = 0; i < 32; ++i) session_id_[i] = exchange_hash[i];
    have_session_id_ = true;
  }

  // The keys: initial IVs, encryption keys and integrity keys, each way.
  const uint32_t key_out = names_equal(cipher_out, "aes256-ctr") ? 32 : 16, key_in = names_equal(cipher_in, "aes256-ctr") ? 32 : 16;
  const bool etm_out = StringView(mac_out).ends_with(StringView("-etm@openssh.com")),
             etm_in = StringView(mac_in).ends_with(StringView("-etm@openssh.com"));
  const HashKind kind_out = StringView(mac_out).starts_with(StringView("hmac-sha2-512")) ? HashKind::Sha512 : HashKind::Sha256;
  const HashKind kind_in = StringView(mac_in).starts_with(StringView("hmac-sha2-512")) ? HashKind::Sha512 : HashKind::Sha256;
  uint8_t iv_out[16], iv_in[16], enc_out[32], enc_in[32], int_out[64], int_in[64];
  derive(shared_mpint.view().data(), static_cast<uint32_t>(shared_mpint.size()), exchange_hash, 'A', ByteView(iv_out, 16));
  derive(shared_mpint.view().data(), static_cast<uint32_t>(shared_mpint.size()), exchange_hash, 'B', ByteView(iv_in, 16));
  derive(shared_mpint.view().data(), static_cast<uint32_t>(shared_mpint.size()), exchange_hash, 'C', ByteView(enc_out, key_out));
  derive(shared_mpint.view().data(), static_cast<uint32_t>(shared_mpint.size()), exchange_hash, 'D', ByteView(enc_in, key_in));
  derive(shared_mpint.view().data(), static_cast<uint32_t>(shared_mpint.size()), exchange_hash, 'E', ByteView(int_out, hash_size(kind_out)));
  derive(shared_mpint.view().data(), static_cast<uint32_t>(shared_mpint.size()), exchange_hash, 'F', ByteView(int_in, hash_size(kind_in)));

  ByteBuffer newkeys(allocator_);
  newkeys.put8(kNewKeys);
  s = send_packet(newkeys.view());
  if (s != Status::Ok) return s;
  if (strict_) out_.sequence = 0;
  if (!out_.aes.set_key(ConstByteView(enc_out, key_out))) return Status::IoError;
  for (int i = 0; i < 16; ++i) out_.counter[i] = iv_out[i];
  for (uint32_t i = 0; i < hash_size(kind_out); ++i) out_.mac_key[i] = int_out[i];
  out_.stream_used = 16;
  out_.block = 16;
  out_.etm = etm_out;
  out_.mac_hash = kind_out;
  out_.mac_length = hash_size(kind_out);
  out_.active = true;

  ByteBuffer theirs_new(allocator_);
  s = next_message(&theirs_new);
  if (s != Status::Ok) return s;
  if (theirs_new.view()[0] != kNewKeys) {
    fail();
    return Status::Network;
  }
  if (strict_) in_.sequence = 0;
  if (!in_.aes.set_key(ConstByteView(enc_in, key_in))) return Status::IoError;
  for (int i = 0; i < 16; ++i) in_.counter[i] = iv_in[i];
  for (uint32_t i = 0; i < hash_size(kind_in); ++i) in_.mac_key[i] = int_in[i];
  in_.stream_used = 16;
  in_.block = 16;
  in_.etm = etm_in;
  in_.mac_hash = kind_in;
  in_.mac_length = hash_size(kind_in);
  in_.active = true;
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- login and channel

Status SshSession::login_password(StringView user, StringView password) {
  last_progress_ms_ = stack_.now_ms();
  ByteBuffer request(allocator_), reply(allocator_);
  {
    SshWriter w(&request);
    w.u8(kServiceRequest);
    w.string("ssh-userauth");
    if (!w.ok()) return Status::OutOfMemory;
  }
  Status s = send_packet(request.view());
  if (s != Status::Ok) return s;
  s = next_message(&reply);
  if (s != Status::Ok) return s;
  if (reply.view()[0] != kServiceAccept) {
    fail();
    return Status::Network;
  }
  request.clear();
  {
    SshWriter w(&request);
    w.u8(kUserauthRequest);
    w.string(user);
    w.string("ssh-connection");
    w.string("password");
    w.boolean(false);
    w.string(password);
    if (!w.ok()) return Status::OutOfMemory;
  }
  s = send_packet(request.view());
  if (s != Status::Ok) return s;
  for (;;) {
    s = next_message(&reply);
    if (s != Status::Ok) return s;
    switch (reply.view()[0]) {
      case kUserauthSuccess: return Status::Ok;
      case kUserauthBanner: continue;
      case kUserauthFailure: {
        SshReader r(reply.view());
        r.u8();
        const StringView methods = r.text();
        return r.ok() && !ssh_list_has(methods, "password") ? Status::Unsupported : Status::LoginFailed;
      }
      case kUserauthChangeRequest: return Status::LoginFailed;   // (the password has expired: not handled)
      default: continue;
    }
  }
}

Status SshSession::take_channel_message(ConstByteView message, bool* handled) {
  *handled = true;
  SshReader r(message);
  const uint8_t type = r.u8();
  switch (type) {
    case kChannelWindowAdjust: {
      r.u32();
      const uint32_t add = r.u32();
      if (!r.ok()) break;
      const uint64_t total = uint64_t{remote_window_} + add;
      remote_window_ = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
      return Status::Ok;
    }
    case kChannelData: {
      r.u32();
      const ConstByteView data = r.string(kMaxPacket);
      if (!r.ok()) break;
      local_window_ = data.size() > local_window_ ? 0 : local_window_ - static_cast<uint32_t>(data.size());
      return rx_.append(data) ? Status::Ok : Status::OutOfMemory;
    }
    case kChannelExtendedData: {   // (standard error: dropped)
      r.u32();
      r.u32();
      const ConstByteView data = r.string(kMaxPacket);
      if (!r.ok()) break;
      local_window_ = data.size() > local_window_ ? 0 : local_window_ - static_cast<uint32_t>(data.size());
      return Status::Ok;
    }
    case kChannelEof:
    case kChannelClose:
      channel_closed_ = true;
      return Status::Ok;
    case kChannelRequest: {   // exit status, keep-alive and the like: only the ones that want an answer get one (no)
      r.u32();
      r.string();
      if (!r.ok()) break;
      if (r.boolean()) {
        ByteBuffer no(allocator_);
        SshWriter w(&no);
        w.u8(kChannelFailure);
        w.u32(remote_channel_);
        if (!w.ok()) return Status::OutOfMemory;
        return send_packet(no.view());
      }
      return Status::Ok;
    }
    default:
      *handled = false;
      return Status::Ok;
  }
  fail();   // a channel message that does not parse
  return Status::Network;
}

Status SshSession::next_channel_message(ByteBuffer* message, bool return_after_handled) {
  for (;;) {
    Status s = next_message(message);
    if (s != Status::Ok) return s;
    bool handled = false;
    s = take_channel_message(message->view(), &handled);
    if (s != Status::Ok) return s;
    if (!handled || return_after_handled) return Status::Ok;
  }
}

Status SshSession::open_subsystem(StringView name) {
  last_progress_ms_ = stack_.now_ms();
  ByteBuffer request(allocator_), reply(allocator_);
  {
    SshWriter w(&request);
    w.u8(kChannelOpen);
    w.string("session");
    w.u32(0);
    w.u32(kWindow);
    w.u32(kChannelPacket);
    if (!w.ok()) return Status::OutOfMemory;
  }
  local_window_ = kWindow;
  Status s = send_packet(request.view());
  if (s != Status::Ok) return s;
  for (;;) {
    s = next_message(&reply);
    if (s != Status::Ok) return s;
    if (reply.view()[0] == kChannelOpenFailure) return Status::Unsupported;
    if (reply.view()[0] == kChannelConfirm) break;
  }
  {
    SshReader r(reply.view());
    r.u8();
    r.u32();
    remote_channel_ = r.u32();
    remote_window_ = r.u32();
    remote_max_packet_ = r.u32();
    if (!r.ok() || remote_max_packet_ == 0) {
      fail();
      return Status::Network;
    }
  }
  request.clear();
  {
    SshWriter w(&request);
    w.u8(kChannelRequest);
    w.u32(remote_channel_);
    w.string("subsystem");
    w.boolean(true);
    w.string(name);
    if (!w.ok()) return Status::OutOfMemory;
  }
  s = send_packet(request.view());
  if (s != Status::Ok) return s;
  for (;;) {
    s = next_channel_message(&reply);
    if (s != Status::Ok) return s;
    if (reply.view()[0] == kChannelSuccess) break;
    if (reply.view()[0] == kChannelFailure) return Status::Unsupported;
  }
  channel_open_ = true;
  return Status::Ok;
}

Status SshSession::send(ConstByteView data) {
  last_progress_ms_ = stack_.now_ms();
  while (!data.empty()) {
    if (!alive_ || channel_closed_ || !channel_open_) return Status::Network;
    if (remote_window_ == 0) {
      ByteBuffer ignored(allocator_);
      const Status s = next_channel_message(&ignored, true);   // (waits for the window to open)
      if (s != Status::Ok) return s;
      continue;
    }
    uint32_t chunk = remote_max_packet_ < kChannelPacket ? remote_max_packet_ : kChannelPacket;
    if (chunk > remote_window_) chunk = remote_window_;
    if (chunk > data.size()) chunk = static_cast<uint32_t>(data.size());
    ByteBuffer message(allocator_);
    SshWriter w(&message);
    w.u8(kChannelData);
    w.u32(remote_channel_);
    w.string(data.first(chunk));
    if (!w.ok()) return Status::OutOfMemory;
    const Status s = send_packet(message.view());
    if (s != Status::Ok) return s;
    remote_window_ -= chunk;
    data = data.subview(chunk);
  }
  return Status::Ok;
}

Status SshSession::adjust_window() {
  if (local_window_ >= kWindow / 2) return Status::Ok;
  ByteBuffer message(allocator_);
  SshWriter w(&message);
  w.u8(kChannelWindowAdjust);
  w.u32(remote_channel_);
  w.u32(kWindow - local_window_);
  if (!w.ok()) return Status::OutOfMemory;
  local_window_ = kWindow;
  return send_packet(message.view());
}

Status SshSession::receive(uint8_t* out, uint32_t count) {
  last_progress_ms_ = stack_.now_ms();
  while (rx_.size() - rx_at_ < count) {
    if (!alive_ || channel_closed_ || !channel_open_) return Status::Network;
    ByteBuffer ignored(allocator_);
    const Status s = next_channel_message(&ignored, true);
    if (s != Status::Ok) return s;
  }
  ByteView(out, count).copy_from(rx_.view().subview(rx_at_, count));
  rx_at_ += count;
  if (rx_at_ == rx_.size()) {
    rx_.clear();
    rx_at_ = 0;
  }
  return adjust_window();
}

}  // namespace network
