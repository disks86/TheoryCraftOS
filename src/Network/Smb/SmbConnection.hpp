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

// One SMB2/3 connection to a server (TCP port 445): the protocol itself, request by request, each answered before the next goes out. It
// negotiates a dialect (2.0.2 up to 3.0.2), logs in with NTLMv2 (or anonymously), signs every message once a session key exists (HMAC-SHA-256
// for SMB 2, AES-CMAC for SMB 3) and checks the signatures of the answers, and offers the operations a filesystem needs on a share: connect to
// it, open and close files and folders, read, write, list, query, rename, delete, resize and send a command down a named pipe. Not supported:
// SMB 1, SMB 3.1.1 (its pre-authentication hash), encryption, Kerberos, compounding, multichannel, DFS.
//
// Calls wait for the answer, polling the network stack meanwhile (which is also polled by its own thread, if it has one). One connection is
// used by one thread at a time. Failures are filesystem::Status values; last_nt_status() has the server's own code of the last request.
#pragma once
#include "Crypto/Random.hpp"
#include "FileSystem/FileSystemTypes.hpp"
#include "Network/INetworkStack.hpp"
#include "Network/Smb/Ntlm.hpp"
#include "Utility/ByteBuffer.hpp"

namespace network {

struct SmbFileId {
  uint8_t bytes[16] = {};
};

struct SmbFileInfo {
  uint64_t size = 0, allocation = 0;
  uint64_t created = 0, accessed = 0, written = 0, changed = 0;   // Windows FILETIMEs
  uint32_t attributes = 0;
  bool directory() const { return (attributes & 0x10) != 0; }
  bool read_only() const { return (attributes & 0x01) != 0; }
};

// What the connection needs from the machine besides the network.
struct SmbEnvironment {
  crypto::IRandom* random = nullptr;                    // required
  uint64_t (*filetime)(void* context) = nullptr;        // the time as a Windows FILETIME, or null if the machine does not know it
  void* context = nullptr;
  const char* workstation = "TCOS";                     // the client's name as the server sees it
};

class SmbConnection {
 public:
  static constexpr uint32_t kMaxIo = 65536;   // what one read or write moves

  SmbConnection(INetworkStack& stack, SharedPointer<ITcpSocket> socket, allocators::IAllocator& allocator, const SmbEnvironment& environment);
  ~SmbConnection();
  SmbConnection(const SmbConnection&) = delete;
  SmbConnection& operator=(const SmbConnection&) = delete;

  // Agrees on a dialect with the server (the first call), then logs in. Status::LoginFailed for credentials the server refuses,
  // LoginRequired if an anonymous login was refused, Network for a connection that broke or an answer that makes no sense.
  filesystem::Status login(const NtlmCredentials& credentials);

  filesystem::Status tree_connect(StringView server, StringView share, uint32_t* tree);
  filesystem::Status tree_disconnect(uint32_t tree);

  // Opens (or creates) `path` (UTF-8, '\' separated, relative to the share; empty for the share itself).
  filesystem::Status create(uint32_t tree, StringView path, uint32_t access, uint32_t disposition, uint32_t options, uint32_t attributes, SmbFileId* id, SmbFileInfo* info);
  filesystem::Status close(uint32_t tree, const SmbFileId& id);
  // Reads up to out.size() (at most kMaxIo) bytes at `offset`; *got is 0 at the end of the file.
  filesystem::Status read(uint32_t tree, const SmbFileId& id, uint64_t offset, ByteView out, uint32_t* got);
  filesystem::Status write(uint32_t tree, const SmbFileId& id, uint64_t offset, ConstByteView data, uint32_t* written);
  filesystem::Status flush(uint32_t tree, const SmbFileId& id);
  // The next batch of a folder's entries (FILE_DIRECTORY_INFORMATION records) appended to `out`; EndOfDirectory when there are no more.
  filesystem::Status query_directory(uint32_t tree, const SmbFileId& id, bool restart, ByteBuffer* out);
  filesystem::Status delete_on_close(uint32_t tree, const SmbFileId& id);   // marks an open file or empty folder for deletion
  filesystem::Status rename(uint32_t tree, const SmbFileId& id, StringView new_path);
  filesystem::Status set_end_of_file(uint32_t tree, const SmbFileId& id, uint64_t size);
  // Sends `input` down an open named pipe and collects the answer (appended to `output`).
  filesystem::Status pipe_transceive(uint32_t tree, const SmbFileId& id, ConstByteView input, ByteBuffer* output);

  bool alive() const { return alive_; }
  bool signing() const { return signing_; }
  bool guest() const { return guest_; }
  uint16_t dialect() const { return dialect_; }
  uint32_t last_nt_status() const { return last_status_; }
  uint32_t max_io() const { return max_io_; }

  // --- For the tests
  static filesystem::Status map_status(uint32_t nt_status);

 private:
  struct Reply;
  filesystem::Status transact(uint16_t command, uint32_t tree, ConstByteView body, ByteBuffer* reply, uint32_t* nt_status, bool sign = true);
  filesystem::Status send_all(ConstByteView data);
  filesystem::Status receive_message(ByteBuffer* message);
  filesystem::Status receive_exact(uint8_t* out, uint32_t count);
  filesystem::Status negotiate();
  bool sign_message(ByteBuffer* message);
  bool verify_message(ConstByteView message) const;
  void fail();

  INetworkStack& stack_;
  SharedPointer<ITcpSocket> socket_;
  allocators::IAllocator& allocator_;
  SmbEnvironment environment_;
  bool alive_ = true, negotiated_ = false, signing_ = false, signing_required_ = false, server_signing_ = false, guest_ = false;
  uint16_t dialect_ = 0;
  uint32_t max_io_ = kMaxIo;
  uint64_t message_id_ = 0, session_id_ = 0;
  uint32_t credits_ = 1;
  uint32_t last_status_ = 0;
  uint8_t signing_key_[16] = {};
  uint8_t client_guid_[16] = {};
  uint64_t deadline_ms_ = 0;
};

// NT status codes the protocol uses.
namespace nt {
constexpr uint32_t kSuccess = 0, kPending = 0x103, kBufferOverflow = 0x80000005, kNoMoreFiles = 0x80000006, kMoreProcessingRequired = 0xC0000016,
                   kEndOfFile = 0xC0000011, kLogonFailure = 0xC000006D, kAccessDenied = 0xC0000022;
}  // namespace nt

}  // namespace network
