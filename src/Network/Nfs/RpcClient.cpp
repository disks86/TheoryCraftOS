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

#include "Network/Nfs/RpcClient.hpp"
#include "Kernel/Thread.hpp"
#include "Network/Nfs/Xdr.hpp"
#include "Utility/Move.hpp"

namespace network {

using filesystem::Status;

namespace {
constexpr uint32_t kCallTimeoutMs = 30000;
constexpr uint32_t kMaxRecord = 4u << 20;
}  // namespace

RpcClient::RpcClient(INetworkStack& stack, SharedPointer<ITcpSocket> socket, allocators::IAllocator& allocator)
    : stack_(stack), socket_(util::move(socket)), allocator_(allocator) {
  xid_ += static_cast<uint32_t>(stack.now_ms() & 0xFFFF);   // (different connections start at different numbers)
}

RpcClient::~RpcClient() {
  if (socket_) socket_->close();
}

void RpcClient::fail() {
  alive_ = false;
  if (socket_) socket_->abort();
}

Status RpcClient::send_record(ConstByteView data) {
  ByteBuffer framed(allocator_);
  const uint32_t header = 0x80000000u | static_cast<uint32_t>(data.size());   // one fragment, the last
  if (!framed.put8(header >> 24) || !framed.put8(header >> 16) || !framed.put8(header >> 8) || !framed.put8(header) || !framed.append(data)) return Status::OutOfMemory;
  ConstByteView rest = framed.view();
  while (!rest.empty()) {
    if (socket_->failed() || stack_.now_ms() > deadline_ms_) {
      fail();
      return Status::Network;
    }
    const uint32_t n = socket_->send(rest);
    if (n == 0) {
      stack_.poll();
      kernel::sleep_us(100);
    } else {
      rest = rest.subview(n);
    }
  }
  return Status::Ok;
}

Status RpcClient::receive_exact(uint8_t* out, uint32_t count) {
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

// A record is one or more fragments; the last has the top bit of its length word set.
Status RpcClient::receive_record(ByteBuffer* record) {
  record->clear();
  for (;;) {
    uint8_t header[4];
    Status s = receive_exact(header, 4);
    if (s != Status::Ok) return s;
    const uint32_t word = (uint32_t{header[0]} << 24) | (uint32_t{header[1]} << 16) | (uint32_t{header[2]} << 8) | header[3];
    const uint32_t length = word & 0x7FFFFFFF;
    if (record->size() + length > kMaxRecord) {
      fail();
      return Status::Network;
    }
    const uint64_t at = record->size();
    if (!record->put_zeros(length)) return Status::OutOfMemory;
    if (length) {
      s = receive_exact(record->writable_view().data() + at, length);
      if (s != Status::Ok) return s;
    }
    if (word & 0x80000000u) return Status::Ok;
  }
}

Status RpcClient::call(uint32_t program, uint32_t version, uint32_t procedure, ConstByteView args, const RpcAuth& auth, ByteBuffer* results) {
  if (!alive_) return Status::Network;
  deadline_ms_ = stack_.now_ms() + kCallTimeoutMs;
  const uint32_t xid = ++xid_;
  ByteBuffer request(allocator_);
  {
    XdrWriter w(&request);
    w.u32(xid);
    w.u32(0);   // CALL
    w.u32(2);   // RPC version 2
    w.u32(program);
    w.u32(version);
    w.u32(procedure);
    if (auth.sys) {   // AUTH_SYS: a time stamp, the machine name, the user, the group and no more groups
      ByteBuffer body(allocator_);
      XdrWriter b(&body);
      b.u32(static_cast<uint32_t>(stack_.now_ms()));
      b.string(auth.machine);
      b.u32(auth.uid);
      b.u32(auth.gid);
      b.u32(0);
      if (!b.ok()) return Status::OutOfMemory;
      w.u32(1);
      w.opaque(body.view());
    } else {
      w.u32(0);
      w.u32(0);
    }
    w.u32(0);   // verifier: AUTH_NONE
    w.u32(0);
    if (!w.ok() || !request.append(args)) return Status::OutOfMemory;
  }
  Status s = send_record(request.view());
  if (s != Status::Ok) return s;
  ByteBuffer record(allocator_);
  for (;;) {
    s = receive_record(&record);
    if (s != Status::Ok) return s;
    XdrReader r(record.view());
    const uint32_t reply_xid = r.u32(), type = r.u32();
    if (!r.ok() || type != 1) {
      fail();
      return Status::Network;
    }
    if (reply_xid != xid) continue;   // a late reply to a call given up on
    const uint32_t reply_status = r.u32();
    if (reply_status != 0) {   // MSG_DENIED: RPC version mismatch (0) or an authentication error (1)
      return r.u32() == 1 ? Status::AccessDenied : Status::Unsupported;
    }
    r.u32();            // the verifier's flavor
    r.opaque(400);      // and body
    const uint32_t accept = r.u32();
    if (!r.ok()) {
      fail();
      return Status::Network;
    }
    if (accept == 2) {   // PROG_MISMATCH: lowest and highest version
      low_ = r.u32();
      high_ = r.u32();
      return Status::Unsupported;
    }
    if (accept == 1 || accept == 3) return Status::Unsupported;   // PROG_UNAVAIL, PROC_UNAVAIL
    if (accept != 0) return Status::IoError;                       // GARBAGE_ARGS, SYSTEM_ERR
    const uint64_t at = record.size() - r.remaining();
    return results->append(record.view().subview(at)) ? Status::Ok : Status::OutOfMemory;
  }
}

}  // namespace network
