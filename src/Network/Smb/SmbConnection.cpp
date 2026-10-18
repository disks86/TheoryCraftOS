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

#include "Network/Smb/SmbConnection.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/Move.hpp"
#include "Utility/Utf16.hpp"

namespace network {

using filesystem::Status;

namespace {

enum Command : uint16_t {
  kNegotiate = 0, kSessionSetup = 1, kLogoff = 2, kTreeConnect = 3, kTreeDisconnect = 4, kCreate = 5, kClose = 6, kFlush = 7, kRead = 8, kWrite = 9,
  kIoctl = 11, kQueryDirectory = 14, kSetInfo = 17,
};
constexpr uint32_t kFlagSigned = 0x8, kFlagAsync = 0x2, kFlagResponse = 0x1;
constexpr uint32_t kRequestTimeoutMs = 30000;
constexpr uint32_t kMaxMessage = 16u << 20;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }

bool put64(ByteBuffer* b, uint64_t v) { return b->put32(static_cast<uint32_t>(v)) && b->put32(static_cast<uint32_t>(v >> 32)); }
uint8_t* writable(ByteBuffer& b) { return const_cast<uint8_t*>(b.view().data()); }

// ---- SPNEGO (RFC 4178), just enough to carry NTLMSSP: DER lengths and the two token shapes.
bool der_length(ByteBuffer* b, uint64_t n) {
  if (n < 0x80) return b->put8(static_cast<uint8_t>(n));
  if (n < 0x100) return b->put8(0x81) && b->put8(static_cast<uint8_t>(n));
  return b->put8(0x82) && b->put8(static_cast<uint8_t>(n >> 8)) && b->put8(static_cast<uint8_t>(n));
}
uint64_t der_size(uint64_t n) { return n < 0x80 ? 1 : n < 0x100 ? 2 : 3; }
bool der_wrap(ByteBuffer* b, uint8_t tag, ConstByteView content) { return b->put8(tag) && der_length(b, content.size()) && b->append(content); }

const uint8_t kSpnegoOid[] = {0x06, 0x06, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02};
const uint8_t kNtlmOid[] = {0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0A};

// NegTokenInit { mechTypes { NTLMSSP }, mechToken }
bool spnego_init(allocators::IAllocator& a, ConstByteView ntlm, ByteBuffer* out) {
  ByteBuffer mech_list(a), mech_types(a), token(a), seq(a), init(a), inner(a);
  if (!der_wrap(&mech_list, 0x30, ConstByteView(kNtlmOid, sizeof kNtlmOid)) || !der_wrap(&mech_types, 0xA0, mech_list.view())) return false;
  if (!der_wrap(&token, 0x04, ntlm) || !der_wrap(&seq, 0xA2, token.view())) return false;
  if (!inner.append(mech_types.view()) || !inner.append(seq.view()) || !der_wrap(&init, 0x30, inner.view())) return false;
  ByteBuffer ctx(a), body(a);
  if (!der_wrap(&ctx, 0xA0, init.view())) return false;
  if (!body.append(ConstByteView(kSpnegoOid, sizeof kSpnegoOid)) || !body.append(ctx.view())) return false;
  return der_wrap(out, 0x60, body.view());
}

// NegTokenResp { responseToken }
bool spnego_response(allocators::IAllocator& a, ConstByteView ntlm, ByteBuffer* out) {
  ByteBuffer token(a), seq(a), resp(a);
  if (!der_wrap(&token, 0x04, ntlm) || !der_wrap(&seq, 0xA2, token.view()) || !der_wrap(&resp, 0x30, seq.view())) return false;
  return der_wrap(out, 0xA1, resp.view());
}

// The NTLMSSP message inside a security buffer (SPNEGO wrapped or not): from its signature on.
ConstByteView find_ntlm(ConstByteView blob) {
  for (uint64_t i = 0; i + 8 <= blob.size(); ++i)
    if (__builtin_memcmp(blob.data() + i, "NTLMSSP", 8) == 0) return blob.subview(i);
  return ConstByteView();
}

}  // namespace

Status SmbConnection::map_status(uint32_t s) {
  switch (s) {
    case 0: return Status::Ok;
    case 0x80000006: return Status::EndOfDirectory;
    case 0xC0000034: case 0xC000003A: case 0xC0000039: case 0xC00000CC: case 0xC000000F: return Status::NotFound;   // name / path not found, bad network name
    case 0xC0000035: return Status::AlreadyExists;
    case 0xC0000103: return Status::NotADirectory;
    case 0xC00000BA: return Status::IsADirectory;
    case 0xC0000101: return Status::NotEmpty;
    case 0xC0000043: case 0xC0000056: case 0xC0000121: return Status::Busy;   // sharing violation, delete pending, cannot delete
    case 0xC000007F: case 0xC0000427: return Status::NoSpace;
    case 0xC00000A2: return Status::ReadOnly;
    case 0xC0000022: case 0xC000009C: case 0xC0000120: case 0xC0000061: return Status::AccessDenied;
    case 0xC000006D: case 0xC000006A: case 0xC0000064: case 0xC000006E: case 0xC0000072: case 0xC0000071: case 0xC0000234: case 0xC000006F: case 0xC0000070: return Status::LoginFailed;
    case 0xC000000D: case 0xC0000033: case 0xC0000037: return Status::InvalidPath;
    case 0xC00000BB: case 0xC0000002: return Status::Unsupported;
    case 0xC0000203: case 0xC00000C9: case 0xC000020C: case 0xC000013C: case 0xC0000241: return Status::Network;   // session / tree gone, connection disconnected
    case 0xC0000011: return Status::Ok;   // end of file: the caller reads *got == 0
    default: return Status::IoError;
  }
}

SmbConnection::SmbConnection(INetworkStack& stack, SharedPointer<ITcpSocket> socket, allocators::IAllocator& allocator, const SmbEnvironment& environment)
    : stack_(stack), socket_(util::move(socket)), allocator_(allocator), environment_(environment) {}

SmbConnection::~SmbConnection() {
  if (socket_) socket_->abort();
}

void SmbConnection::fail() {
  alive_ = false;
  if (socket_) socket_->abort();
}

// ---------------------------------------------------------------------------------------------------------------------------------------- transport

Status SmbConnection::send_all(ConstByteView data) {
  while (!data.empty()) {
    if (socket_->failed() || stack_.now_ms() > deadline_ms_) {
      fail();
      return Status::Network;
    }
    const uint32_t n = socket_->send(data);
    if (n == 0) {
      stack_.poll();
      kernel::sleep_us(100);
    } else {
      data = data.subview(n);
    }
  }
  return Status::Ok;
}

Status SmbConnection::receive_exact(uint8_t* out, uint32_t count) {
  uint32_t done = 0;
  while (done < count) {
    const uint32_t n = socket_->receive(ByteView(out + done, count - done));
    if (n > 0) {
      done += n;
      continue;
    }
    if (socket_->failed() || (socket_->peer_closed() && socket_->available() == 0) || stack_.now_ms() > deadline_ms_) {
      fail();
      return Status::Network;
    }
    stack_.poll();
    kernel::sleep_us(100);
  }
  return Status::Ok;
}

Status SmbConnection::receive_message(ByteBuffer* message) {
  for (;;) {
    uint8_t header[4];
    Status s = receive_exact(header, 4);
    if (s != Status::Ok) return s;
    const uint32_t length = (uint32_t{header[1]} << 16) | (uint32_t{header[2]} << 8) | header[3];
    if (header[0] == 0x85) continue;   // a keep-alive
    if (header[0] != 0 || length < 64 || length > kMaxMessage) {
      fail();
      return Status::Network;
    }
    message->clear();
    if (!message->put_zeros(length)) return Status::OutOfMemory;
    s = receive_exact(writable(*message), length);
    if (s != Status::Ok) return s;
    return Status::Ok;
  }
}

bool SmbConnection::sign_message(ByteBuffer* message) {
  uint8_t* m = writable(*message);
  m[16] |= kFlagSigned;
  for (int i = 0; i < 16; ++i) m[48 + i] = 0;
  uint8_t mac[32];
  if (dialect_ >= 0x0300) {
    if (!crypto::aes_cmac(ConstByteView(signing_key_, 16), message->view(), mac)) return false;
  } else {
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(signing_key_, 16), message->view(), mac);
  }
  for (int i = 0; i < 16; ++i) m[48 + i] = mac[i];
  return true;
}

bool SmbConnection::verify_message(ConstByteView message) const {
  ByteBuffer copy(allocator_);
  if (!copy.append(message)) return false;
  uint8_t* m = writable(copy);
  uint8_t given[16];
  for (int i = 0; i < 16; ++i) {
    given[i] = m[48 + i];
    m[48 + i] = 0;
  }
  uint8_t mac[32];
  if (dialect_ >= 0x0300) {
    if (!crypto::aes_cmac(ConstByteView(signing_key_, 16), copy.view(), mac)) return false;
  } else {
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(signing_key_, 16), copy.view(), mac);
  }
  return crypto::equal_constant_time(mac, given, 16);
}

// Sends one request and waits for its answer. *reply is the whole answer message (the body starts at offset 64).
Status SmbConnection::transact(uint16_t command, uint32_t tree, ConstByteView body, ByteBuffer* reply, uint32_t* nt_status, bool sign) {
  if (!alive_) return Status::Network;
  deadline_ms_ = stack_.now_ms() + kRequestTimeoutMs;
  const uint64_t id = message_id_++;
  const bool charge = dialect_ >= 0x0210 && command != kNegotiate;
  ByteBuffer request(allocator_);
  bool ok = request.put8(0xFE) && request.put8('S') && request.put8('M') && request.put8('B') && request.put16(64) && request.put16(charge ? 1 : 0) &&
            request.put32(0) && request.put16(command) && request.put16(8) && request.put32(0) && request.put32(0) && put64(&request, id) && request.put32(0) &&
            request.put32(tree) && put64(&request, session_id_) && request.put_zeros(16) && request.append(body);
  if (!ok) return Status::OutOfMemory;
  if (sign && signing_ && command != kNegotiate && !sign_message(&request)) return Status::OutOfMemory;
  ByteBuffer framed(allocator_);
  if (!framed.put8(0) || !framed.put8(static_cast<uint8_t>(request.size() >> 16)) || !framed.put8(static_cast<uint8_t>(request.size() >> 8)) ||
      !framed.put8(static_cast<uint8_t>(request.size())) || !framed.append(request.view()))
    return Status::OutOfMemory;
  Status s = send_all(framed.view());
  if (s != Status::Ok) return s;
  for (;;) {
    s = receive_message(reply);
    if (s != Status::Ok) return s;
    const uint8_t* m = reply->view().data();
    if (m[0] != 0xFE || m[1] != 'S' || m[2] != 'M' || m[3] != 'B' || le16(m + 4) != 64) {
      fail();
      return Status::Network;
    }
    const uint32_t flags = le32(m + 16), status = le32(m + 8);
    if (!(flags & kFlagResponse) || le64(m + 24) != id) {
      if (le64(m + 24) < id) continue;   // a late answer to something given up on
      fail();
      return Status::Network;
    }
    if (status == nt::kPending && (flags & kFlagAsync)) {   // "still working": the real answer follows
      deadline_ms_ = stack_.now_ms() + kRequestTimeoutMs;
      continue;
    }
    if (signing_ && command != kNegotiate && command != kSessionSetup) {
      if ((flags & kFlagSigned) ? !verify_message(reply->view()) : signing_required_) {
        fail();
        return Status::Network;
      }
    }
    const uint32_t granted = le16(m + 14);
    credits_ = credits_ + granted > (charge ? 1u : 0u) ? credits_ + granted - (charge ? 1u : 0u) : 0;
    if (command == kSessionSetup && session_id_ == 0) session_id_ = le64(m + 40);
    *nt_status = status;
    last_status_ = status;
    return Status::Ok;
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------- session

Status SmbConnection::negotiate() {
  ByteBuffer body(allocator_);
  if (!environment_.random || !environment_.random->fill(ByteView(client_guid_, 16))) return Status::IoError;
  const uint16_t dialects[4] = {0x0202, 0x0210, 0x0300, 0x0302};
  bool ok = body.put16(36) && body.put16(4) && body.put16(1) /* signing enabled */ && body.put16(0) && body.put32(0) && body.append(ConstByteView(client_guid_, 16)) &&
            body.put_zeros(8);
  for (uint16_t d : dialects) ok = ok && body.put16(d);
  if (!ok) return Status::OutOfMemory;
  ByteBuffer reply(allocator_);
  uint32_t nt_status = 0;
  Status s = transact(kNegotiate, 0, body.view(), &reply, &nt_status, false);
  if (s != Status::Ok) return s;
  if (nt_status != 0 || reply.size() < 64 + 64) return nt_status ? map_status(nt_status) : Status::Network;
  const uint8_t* b = reply.view().data() + 64;
  if (le16(b) != 65) return Status::Network;
  const uint32_t security_mode = le16(b + 2);
  dialect_ = static_cast<uint16_t>(le16(b + 4));
  if (dialect_ != 0x0202 && dialect_ != 0x0210 && dialect_ != 0x0300 && dialect_ != 0x0302) return Status::Unsupported;
  signing_required_ = (security_mode & 2) != 0;
  server_signing_ = (security_mode & 3) != 0;
  const uint32_t max_read = le32(b + 32), max_write = le32(b + 36);
  max_io_ = max_read < max_write ? max_read : max_write;
  if (max_io_ > kMaxIo) max_io_ = kMaxIo;
  if (max_io_ < 4096) max_io_ = 4096;
  negotiated_ = true;
  return Status::Ok;
}

Status SmbConnection::login(const NtlmCredentials& credentials) {
  if (!alive_) return Status::Network;
  if (!negotiated_) {
    const Status s = negotiate();
    if (s != Status::Ok) return s;
  }
  session_id_ = 0;
  signing_ = false;
  guest_ = false;
  NtlmClient ntlm(allocator_);
  ByteBuffer token(allocator_), spnego(allocator_), body(allocator_), reply(allocator_);
  if (!ntlm.negotiate(&token) || !spnego_init(allocator_, token.view(), &spnego)) return Status::OutOfMemory;
  auto session_body = [&](ConstByteView security) {
    body.clear();
    return body.put16(25) && body.put8(0) && body.put8(1) /* signing enabled */ && body.put32(0) && body.put32(0) && body.put16(64 + 24) && body.put16(static_cast<uint32_t>(security.size())) &&
           put64(&body, 0) && body.append(security);
  };
  if (!session_body(spnego.view())) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kSessionSetup, 0, body.view(), &reply, &nt_status, false);
  if (s != Status::Ok) return s;
  if (nt_status != nt::kMoreProcessingRequired) {
    if (nt_status == 0) return Status::Ok;   // a server that lets anyone in without a challenge
    const Status mapped = map_status(nt_status);
    return mapped == Status::IoError ? Status::Network : mapped == Status::AccessDenied ? (credentials.anonymous() ? Status::LoginRequired : Status::LoginFailed) : mapped;
  }
  // The challenge, in the security buffer of the answer.
  if (reply.size() < 64 + 8) return Status::Network;
  const uint8_t* b = reply.view().data() + 64;
  const uint32_t offset = le16(b + 4), length = le16(b + 6);
  if (uint64_t{offset} + length > reply.size()) return Status::Network;
  const ConstByteView challenge = find_ntlm(reply.view().subview(offset, length));
  ByteBuffer authenticate(allocator_), response(allocator_);
  const uint64_t now = environment_.filetime ? environment_.filetime(environment_.context) : 0;
  if (challenge.empty() || !ntlm.authenticate(challenge, credentials, environment_.workstation, now, *environment_.random, &authenticate)) return Status::Network;
  if (!spnego_response(allocator_, authenticate.view(), &response) || !session_body(response.view())) return Status::OutOfMemory;
  s = transact(kSessionSetup, 0, body.view(), &reply, &nt_status, false);
  if (s != Status::Ok) return s;
  if (nt_status != 0) {
    const Status mapped = map_status(nt_status);
    if (mapped == Status::AccessDenied || mapped == Status::LoginFailed) return credentials.anonymous() ? Status::LoginRequired : Status::LoginFailed;
    return mapped == Status::IoError ? Status::Network : mapped;
  }
  const uint32_t session_flags = reply.size() >= 64 + 4 ? le16(reply.view().data() + 66) : 0;
  guest_ = (session_flags & 0x3) != 0;   // a guest or a null session: no key, no signing
  if (!guest_ && ntlm.has_session_key() && server_signing_) {
    if (dialect_ >= 0x0300) {   // SMB 3: the signing key is derived from the session key
      const uint8_t label[] = {'S', 'M', 'B', '2', 'A', 'E', 'S', 'C', 'M', 'A', 'C', 0}, context[] = {'S', 'm', 'b', 'S', 'i', 'g', 'n', 0};
      if (!crypto::kdf_hmac_sha256(ConstByteView(ntlm.session_key(), 16), ConstByteView(label, sizeof label), ConstByteView(context, sizeof context), ByteView(signing_key_, 16)))
        return Status::IoError;
    } else {
      __builtin_memcpy(signing_key_, ntlm.session_key(), 16);
    }
    signing_ = true;
  }
  return Status::Ok;
}

Status SmbConnection::tree_connect(StringView server, StringView share, uint32_t* tree) {
  ByteBuffer path(allocator_), body(allocator_), reply(allocator_);
  if (!util::append_utf16le(&path, "\\\\") || !util::append_utf16le(&path, server) || !util::append_utf16le(&path, "\\") || !util::append_utf16le(&path, share)) return Status::InvalidPath;
  if (!(body.put16(9) && body.put16(0) && body.put16(64 + 8) && body.put16(static_cast<uint32_t>(path.size())) && body.append(path.view()))) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kTreeConnect, 0, body.view(), &reply, &nt_status);
  if (s != Status::Ok) return s;
  if (nt_status != 0) return map_status(nt_status);
  *tree = le32(reply.view().data() + 36);
  return Status::Ok;
}

Status SmbConnection::tree_disconnect(uint32_t tree) {
  ByteBuffer body(allocator_), reply(allocator_);
  if (!(body.put16(4) && body.put16(0))) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  const Status s = transact(kTreeDisconnect, tree, body.view(), &reply, &nt_status);
  return s != Status::Ok ? s : map_status(nt_status);
}

// ---------------------------------------------------------------------------------------------------------------------------------------- files

Status SmbConnection::create(uint32_t tree, StringView path, uint32_t access, uint32_t disposition, uint32_t options, uint32_t attributes, SmbFileId* id, SmbFileInfo* info) {
  ByteBuffer name(allocator_), body(allocator_), reply(allocator_);
  String backslashed(allocator_);
  for (char c : path) backslashed.push_back(c == '/' ? '\\' : c);
  StringView text = backslashed.view();
  while (!text.empty() && text[0] == '\\') text = text.without_prefix(1);
  if (!util::append_utf16le(&name, text)) return Status::InvalidPath;
  bool ok = body.put16(57) && body.put8(0) && body.put8(0) && body.put32(2) /* impersonation */ && put64(&body, 0) && put64(&body, 0) && body.put32(access) &&
            body.put32(attributes) && body.put32(7) /* share read, write and delete */ && body.put32(disposition) && body.put32(options) && body.put16(64 + 56) &&
            body.put16(static_cast<uint32_t>(name.size())) && body.put32(0) && body.put32(0) && body.append(name.view());
  if (ok && name.size() == 0) ok = body.put8(0);
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kCreate, tree, body.view(), &reply, &nt_status);
  if (s != Status::Ok) return s;
  if (nt_status != 0) return map_status(nt_status);
  if (reply.size() < 64 + 88) return Status::Network;
  const uint8_t* b = reply.view().data() + 64;
  if (info) {
    info->created = le64(b + 8);
    info->accessed = le64(b + 16);
    info->written = le64(b + 24);
    info->changed = le64(b + 32);
    info->allocation = le64(b + 40);
    info->size = le64(b + 48);
    info->attributes = le32(b + 56);
  }
  if (id) __builtin_memcpy(id->bytes, b + 64, 16);
  return Status::Ok;
}

Status SmbConnection::close(uint32_t tree, const SmbFileId& id) {
  ByteBuffer body(allocator_), reply(allocator_);
  if (!(body.put16(24) && body.put16(0) && body.put32(0) && body.append(ConstByteView(id.bytes, 16)))) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  const Status s = transact(kClose, tree, body.view(), &reply, &nt_status);
  return s != Status::Ok ? s : map_status(nt_status);
}

Status SmbConnection::flush(uint32_t tree, const SmbFileId& id) {
  ByteBuffer body(allocator_), reply(allocator_);
  if (!(body.put16(24) && body.put16(0) && body.put32(0) && body.append(ConstByteView(id.bytes, 16)))) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  const Status s = transact(kFlush, tree, body.view(), &reply, &nt_status);
  return s != Status::Ok ? s : map_status(nt_status);
}

Status SmbConnection::read(uint32_t tree, const SmbFileId& id, uint64_t offset, ByteView out, uint32_t* got) {
  *got = 0;
  if (out.empty()) return Status::Ok;
  uint32_t want = out.size() < max_io_ ? static_cast<uint32_t>(out.size()) : max_io_;
  ByteBuffer body(allocator_), reply(allocator_);
  bool ok = body.put16(49) && body.put8(0x50) && body.put8(0) && body.put32(want) && put64(&body, offset) && body.append(ConstByteView(id.bytes, 16)) && body.put32(0) &&
            body.put32(0) && body.put32(0) && body.put16(0) && body.put16(0) && body.put8(0);
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kRead, tree, body.view(), &reply, &nt_status);
  if (s != Status::Ok) return s;
  if (nt_status == nt::kEndOfFile) return Status::Ok;
  if (nt_status != 0) return map_status(nt_status);
  if (reply.size() < 64 + 16) return Status::Network;
  const uint8_t* b = reply.view().data() + 64;
  const uint32_t data_offset = b[2], length = le32(b + 4);
  if (uint64_t{data_offset} + length > reply.size() || length > want) return Status::Network;
  out.first(length).copy_from(reply.view().subview(data_offset, length));
  *got = length;
  return Status::Ok;
}

Status SmbConnection::write(uint32_t tree, const SmbFileId& id, uint64_t offset, ConstByteView data, uint32_t* written) {
  *written = 0;
  if (data.empty()) return Status::Ok;
  const uint32_t count = data.size() < max_io_ ? static_cast<uint32_t>(data.size()) : max_io_;
  ByteBuffer body(allocator_), reply(allocator_);
  bool ok = body.put16(49) && body.put16(64 + 48) && body.put32(count) && put64(&body, offset) && body.append(ConstByteView(id.bytes, 16)) && body.put32(0) && body.put32(0) &&
            body.put16(0) && body.put16(0) && body.put32(0) && body.append(data.first(count));
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kWrite, tree, body.view(), &reply, &nt_status);
  if (s != Status::Ok) return s;
  if (nt_status != 0) return map_status(nt_status);
  if (reply.size() < 64 + 8) return Status::Network;
  *written = le32(reply.view().data() + 64 + 4);
  return Status::Ok;
}

Status SmbConnection::query_directory(uint32_t tree, const SmbFileId& id, bool restart, ByteBuffer* out) {
  ByteBuffer pattern(allocator_), body(allocator_), reply(allocator_);
  if (!util::append_utf16le(&pattern, "*")) return Status::OutOfMemory;
  bool ok = body.put16(33) && body.put8(1) /* FileDirectoryInformation */ && body.put8(restart ? 1 : 0) && body.put32(0) && body.append(ConstByteView(id.bytes, 16)) &&
            body.put16(64 + 32) && body.put16(static_cast<uint32_t>(pattern.size())) && body.put32(max_io_) && body.append(pattern.view());
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kQueryDirectory, tree, body.view(), &reply, &nt_status);
  if (s != Status::Ok) return s;
  if (nt_status == nt::kNoMoreFiles) return Status::EndOfDirectory;
  if (nt_status != 0) return map_status(nt_status);
  if (reply.size() < 64 + 8) return Status::Network;
  const uint8_t* b = reply.view().data() + 64;
  const uint32_t offset = le16(b + 2), length = le32(b + 4);
  if (uint64_t{offset} + length > reply.size()) return Status::Network;
  return out->append(reply.view().subview(offset, length)) ? Status::Ok : Status::OutOfMemory;
}

Status SmbConnection::delete_on_close(uint32_t tree, const SmbFileId& id) {
  ByteBuffer body(allocator_), reply(allocator_);
  const uint8_t one = 1;
  bool ok = body.put16(33) && body.put8(1) && body.put8(13) /* FileDispositionInformation */ && body.put32(1) && body.put16(64 + 32) && body.put16(0) && body.put32(0) &&
            body.append(ConstByteView(id.bytes, 16)) && body.put8(one);
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  const Status s = transact(kSetInfo, tree, body.view(), &reply, &nt_status);
  return s != Status::Ok ? s : map_status(nt_status);
}

Status SmbConnection::rename(uint32_t tree, const SmbFileId& id, StringView new_path) {
  ByteBuffer name(allocator_), info(allocator_), body(allocator_), reply(allocator_);
  String backslashed(allocator_);
  for (char c : new_path) backslashed.push_back(c == '/' ? '\\' : c);
  StringView text = backslashed.view();
  while (!text.empty() && text[0] == '\\') text = text.without_prefix(1);
  if (!util::append_utf16le(&name, text)) return Status::InvalidPath;
  // FILE_RENAME_INFORMATION: ReplaceIfExists, 7 reserved bytes, RootDirectory, FileNameLength, FileName.
  if (!(info.put8(0) && info.put_zeros(7) && put64(&info, 0) && info.put32(static_cast<uint32_t>(name.size())) && info.append(name.view()))) return Status::OutOfMemory;
  bool ok = body.put16(33) && body.put8(1) && body.put8(10) && body.put32(static_cast<uint32_t>(info.size())) && body.put16(64 + 32) && body.put16(0) && body.put32(0) &&
            body.append(ConstByteView(id.bytes, 16)) && body.append(info.view());
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  const Status s = transact(kSetInfo, tree, body.view(), &reply, &nt_status);
  return s != Status::Ok ? s : map_status(nt_status);
}

Status SmbConnection::set_end_of_file(uint32_t tree, const SmbFileId& id, uint64_t size) {
  ByteBuffer body(allocator_), reply(allocator_);
  bool ok = body.put16(33) && body.put8(1) && body.put8(20) /* FileEndOfFileInformation */ && body.put32(8) && body.put16(64 + 32) && body.put16(0) && body.put32(0) &&
            body.append(ConstByteView(id.bytes, 16)) && put64(&body, size);
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  const Status s = transact(kSetInfo, tree, body.view(), &reply, &nt_status);
  return s != Status::Ok ? s : map_status(nt_status);
}

Status SmbConnection::pipe_transceive(uint32_t tree, const SmbFileId& id, ConstByteView input, ByteBuffer* output) {
  ByteBuffer body(allocator_), reply(allocator_);
  bool ok = body.put16(57) && body.put16(0) && body.put32(0x0011C017) /* FSCTL_PIPE_TRANSCEIVE */ && body.append(ConstByteView(id.bytes, 16)) && body.put32(64 + 56) &&
            body.put32(static_cast<uint32_t>(input.size())) && body.put32(0) && body.put32(0) && body.put32(0) && body.put32(max_io_) && body.put32(1) && body.put32(0) &&
            body.append(input);
  if (!ok) return Status::OutOfMemory;
  uint32_t nt_status = 0;
  Status s = transact(kIoctl, tree, body.view(), &reply, &nt_status);
  if (s != Status::Ok) return s;
  if (nt_status != 0 && nt_status != nt::kBufferOverflow) return map_status(nt_status);
  if (reply.size() < 64 + 48) return Status::Network;
  const uint8_t* b = reply.view().data() + 64;
  const uint32_t offset = le32(b + 32), count = le32(b + 36);
  if (uint64_t{offset} + count > reply.size()) return Status::Network;
  return output->append(reply.view().subview(offset, count)) ? Status::Ok : Status::OutOfMemory;
}

}  // namespace network
