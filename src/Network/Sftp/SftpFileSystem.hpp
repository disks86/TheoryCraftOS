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

// The files of SFTP (SSH File Transfer Protocol, version 3) servers as one filesystem, laid out like the other network filesystems:
//   /                      the servers that have been visited
//   /server                the server's root folder ("/" there); "server:port" names another port than 22
//   /server/dir/file       the files and folders on the server, with the server's own absolute paths
// A server is connected to - SSH key exchange, host key check, password login - when it is first used. The first host key a server shows is
// remembered (HostTrust); a different one later gives Status::UntrustedHost. There is no log-in until set_credentials() has been called for the
// server: calls answer Status::LoginRequired (a program asks the user and tries again); a refused password gives LoginFailed. The connection of
// a server that drops is made again by the next call. Names are case sensitive. Safe from any thread (one request goes at a time).
#pragma once
#include "Crypto/Random.hpp"
#include "FileSystem/IDirectory.hpp"
#include "FileSystem/IFile.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "Network/HostTrust.hpp"
#include "Network/ServerNetwork.hpp"
#include "Network/Ssh/SshSession.hpp"
#include "Network/Ssh/SshWire.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace network {

class SftpFileSystem final : public filesystem::IFileSystem {
 public:
  SftpFileSystem(IServerNetwork& network, allocators::IAllocator& allocator, crypto::IRandom& random, HostTrust& trust);
  ~SftpFileSystem();
  SftpFileSystem(const SftpFileSystem&) = delete;
  SftpFileSystem& operator=(const SftpFileSystem&) = delete;

  // The login for `host` from now on; the connection, if there is one, is dropped so that the next call logs in again.
  void set_credentials(StringView host, StringView user, StringView password);
  // Forgets the server: its login and its connection. (Its host key stays in the HostTrust until that is told to forget it.)
  void forget(StringView host);
  // The key HostTrust files a server's host key under.
  static bool trust_key(StringView host, String* key);

  // --- IFileSystem
  filesystem::FileSystemInfo info() const override;
  filesystem::Status open_file(StringView path, filesystem::OpenMode mode, UniquePointer<filesystem::IFile>* file) override;
  filesystem::Status open_directory(StringView path, UniquePointer<filesystem::IDirectory>* directory) override;
  filesystem::Status stat(StringView path, filesystem::DirectoryEntry* entry) override;
  filesystem::Status create_directory(StringView path) override;
  filesystem::Status remove(StringView path) override;
  filesystem::Status rename(StringView from, StringView to) override;
  filesystem::Status flush() override { return filesystem::Status::Ok; }

  // For the tests: maps an SSH_FXP_STATUS code to a filesystem status (EOF is EndOfDirectory).
  static filesystem::Status map_status(uint32_t code);

 private:
  struct Server {
    explicit Server(allocators::IAllocator& a) : host(a), name(a), user(a), password(a) {}
    String host;                  // as typed, "name" or "name:port"
    String name;
    uint16_t port = 22;
    String user, password;
    bool has_login = false;
    UniquePointer<SshSession> ssh;
    uint32_t next_id = 1;
    uint32_t generation = 0;      // changes with every connection: handles from an earlier one are dead
  };
  struct Attributes {
    uint32_t flags = 0;
    uint64_t size = 0;
    uint32_t permissions = 0;
    bool has_size() const { return flags & 1; }
    bool has_permissions() const { return flags & 4; }
    bool directory() const { return has_permissions() && (permissions & 0170000) == 0040000; }
    bool link() const { return has_permissions() && (permissions & 0170000) == 0120000; }
    bool read_only() const { return has_permissions() && (permissions & 0222) == 0; }
  };
  struct Parsed {
    explicit Parsed(allocators::IAllocator& a) : server(a), remote(a), last(a) {}
    String server;
    String remote;                // the absolute path on the server; "/" for the root
    String last;                  // (the last component)
    bool at_root() const { return remote.view().equals("/"); }
  };
  class File;
  class Directory;
  class ListDirectory;
  friend class File;
  friend class Directory;

  bool parse(StringView path, Parsed* out) const;
  SharedPointer<Server> find_server(StringView host, bool create);
  filesystem::Status ensure_connected(Server& server);
  void drop_connection(Server& server);
  // One request and its answer. type/reply: the answer's type and its contents after the request number. Reconnects and tries once more
  // if the connection had been dropped (`retry`); calls on open handles pass false.
  filesystem::Status call(Server& server, uint8_t type, ConstByteView body, uint8_t* reply_type, ByteBuffer* reply, bool retry = true);
  filesystem::Status send_request(Server& server, uint8_t type, uint32_t id, ConstByteView body);
  filesystem::Status read_reply(Server& server, uint32_t* id, uint8_t* type, ByteBuffer* reply);
  // The status in an answer that is not the one expected: Ok for a STATUS that says OK, else what it says.
  static filesystem::Status status_of(uint8_t reply_type, ConstByteView reply);
  static bool read_attributes(SshReader& r, Attributes* out);
  filesystem::Status stat_remote(Server& server, StringView remote, bool follow, Attributes* out);
  filesystem::Status close_handle(Server& server, ConstByteView handle);

  IServerNetwork& network_;
  allocators::IAllocator& allocator_;
  crypto::IRandom& random_;
  HostTrust& trust_;
  mutable kernel::RecursiveMutex mutex_;
  List<SharedPointer<Server>> servers_;
};

}  // namespace network
