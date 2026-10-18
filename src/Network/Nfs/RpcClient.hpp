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

// ONC RPC version 2 (RFC 5531) over a TCP connection, the client side: what NFS, its MOUNT protocol and the port mapper are spoken with. One call at a
// time; each waits for its reply, polling the network stack meanwhile. Calls are made as nobody-in-particular (AUTH_NONE) or with AUTH_SYS, which
// states a user and group id and is believed by the server as far as its export rules allow.
#pragma once
#include "FileSystem/FileSystemTypes.hpp"
#include "Network/INetworkStack.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

struct RpcAuth {
  bool sys = false;              // AUTH_SYS instead of AUTH_NONE
  uint32_t uid = 65534, gid = 65534;
  const char* machine = "tcos";
};

class RpcClient {
 public:
  RpcClient(INetworkStack& stack, SharedPointer<ITcpSocket> socket, allocators::IAllocator& allocator);
  ~RpcClient();
  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;

  // Calls `procedure` of `program` (version `version`) with the XDR encoded `args`; the XDR encoded results are appended to *results. Network
  // for a connection that failed or timed out; Unsupported if the server does not have the program, version or procedure; AccessDenied if it
  // refuses the credentials; IoError for any other fault in the call itself. The NFS level status is in the results.
  filesystem::Status call(uint32_t program, uint32_t version, uint32_t procedure, ConstByteView args, const RpcAuth& auth, ByteBuffer* results);

  bool alive() const { return alive_; }
  // For a Unsupported answer: the lowest and highest version the server offers for the program (0 if it did not say).
  uint32_t mismatch_low() const { return low_; }
  uint32_t mismatch_high() const { return high_; }

 private:
  filesystem::Status send_record(ConstByteView data);
  filesystem::Status receive_record(ByteBuffer* record);
  filesystem::Status receive_exact(uint8_t* out, uint32_t count);
  void fail();

  INetworkStack& stack_;
  SharedPointer<ITcpSocket> socket_;
  allocators::IAllocator& allocator_;
  bool alive_ = true;
  uint32_t xid_ = 0x7C050000;
  uint32_t low_ = 0, high_ = 0;
  uint64_t deadline_ms_ = 0;
};

}  // namespace network
