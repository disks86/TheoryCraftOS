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

// The network shares of SMB servers as one filesystem, so that anything that browses a filesystem (the file explorer, the dialogs, the programs)
// can browse them:
//   /                     the servers that have been visited
//   /server               the disk shares the server offers (asked of it with the srvsvc service on IPC$)
//   /server/share/dir/f   the files and folders on the share
// A server is connected to - and logged in to - when it is first used: anonymously, unless set_credentials() gave a user and password for
// it. Status::LoginRequired says that the server wants a login, LoginFailed that the one given was refused, Network that it could not be reached;
// a program answers LoginRequired by asking the user and calling set_credentials(), then trying again. The connection of a server that
// drops is made again by the next call. Safe from any thread (one request goes at a time); calls wait for the network.
#pragma once
#include "FileSystem/IDirectory.hpp"
#include "FileSystem/IFile.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "Network/Smb/SmbConnection.hpp"
#include "Network/ServerNetwork.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

class SmbFileSystem final : public filesystem::IFileSystem {
 public:
  SmbFileSystem(IServerNetwork& network, allocators::IAllocator& allocator, const SmbEnvironment& environment);
  ~SmbFileSystem();
  SmbFileSystem(const SmbFileSystem&) = delete;
  SmbFileSystem& operator=(const SmbFileSystem&) = delete;

  // The login to use for `host` from now on ("user", "DOMAIN\user" or "user@domain.example"); the connection, if there is one, is dropped so that
  // the next call logs in again. An empty user and password means anonymous.
  void set_credentials(StringView host, StringView user, StringView password);
  // Forgets the login of `host` and drops its connection.
  void forget(StringView host);
  // Hands out the list of shares of a server (the names of the disk shares, no admin shares). Appends to `names`.
  filesystem::Status list_shares(StringView host, List<String>* names);
  // What was agreed with a server (connecting to it if need be): the SMB dialect (0x0202 ... 0x0302), whether messages are signed, and
  // whether the login only got a guest session.
  struct ConnectionInfo {
    uint16_t dialect = 0;
    bool signing = false, guest = false;
  };
  filesystem::Status connection_info(StringView host, ConnectionInfo* info);

  // --- IFileSystem
  filesystem::FileSystemInfo info() const override;
  filesystem::Status open_file(StringView path, filesystem::OpenMode mode, UniquePointer<filesystem::IFile>* file) override;
  filesystem::Status open_directory(StringView path, UniquePointer<filesystem::IDirectory>* directory) override;
  filesystem::Status stat(StringView path, filesystem::DirectoryEntry* entry) override;
  filesystem::Status create_directory(StringView path) override;
  filesystem::Status remove(StringView path) override;
  filesystem::Status rename(StringView from, StringView to) override;
  filesystem::Status flush() override { return filesystem::Status::Ok; }

 private:
  struct Share {
    String name;
    uint32_t tree;
  };
  struct Server {
    explicit Server(allocators::IAllocator& a) : host(a), name(a), user(a), domain(a), password(a), shares(a) {}
    String host;                 // as typed, "name" or "name:port"
    String name;                 // the name part, which goes into the share's UNC path
    uint16_t port = 445;
    String user, domain, password;
    bool have_credentials = false;
    UniquePointer<SmbConnection> connection;
    List<Share> shares;
    uint32_t generation = 0;   // changes whenever the connection is made again
  };
  class File;
  class Directory;
  class ListDirectory;
  friend class File;
  friend class Directory;

  SharedPointer<Server> find_server(StringView host, bool create);
  filesystem::Status connect(Server& server);
  // A connected server with the tree of `share` connected (retrying once with a fresh connection if the old one had died).
  filesystem::Status tree_for(Server& server, StringView share, uint32_t* tree);
  struct Parsed {
    explicit Parsed(allocators::IAllocator& a) : server(a), share(a), rest(a) {}
    String server, share, rest;   // rest: the path inside the share, '/' separated, no leading slash
    uint32_t depth = 0;           // 0: the root, 1: a server, 2: a share (and below)
  };
  bool parse(StringView path, Parsed* out) const;
  filesystem::Status open_node(const Parsed& where, uint32_t access, uint32_t disposition, uint32_t options, SharedPointer<Server>* server, uint32_t* tree, SmbFileId* id, SmbFileInfo* info);
  filesystem::Status list_shares_locked(Server& server, List<String>* names);
  void drop(Server& server);

  IServerNetwork& network_;
  allocators::IAllocator& allocator_;
  SmbEnvironment environment_;
  mutable kernel::RecursiveMutex mutex_;
  List<SharedPointer<Server>> servers_;
};

}  // namespace network
