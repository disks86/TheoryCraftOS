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

// The shares ("exports") of NFS version 3 servers as one filesystem, laid out the way SmbFileSystem lays out SMB shares:
//   /                    the servers that have been visited
//   /server              the exports of the server, as the folders their paths make: an export /srv/nfs/data shows up as srv, then nfs, then data
//   /server/srv/nfs/data/dir/file   the files and folders in the export
// Looking a server up goes through the port mapper (TCP port 111, or "server:port" to name another) to the MOUNT service, which lists the exports
// and hands out the root file handle of each, and then to the NFS service itself. NFS has no log-in: the server believes the user and group id a
// request states (AUTH_SYS) as far as its export rules allow; set_credentials() chooses them per server (65534, "nobody", until then). A server
// that answers "permission denied" gets Status::AccessDenied: a program asks the user for other ids and tries again. Names are case sensitive
// here, as on the servers. Safe from any thread (one request goes at a time); calls wait for the network.
#pragma once
#include "FileSystem/IDirectory.hpp"
#include "FileSystem/IFile.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "Network/Nfs/RpcClient.hpp"
#include "Network/ServerNetwork.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace network {

class NfsFileSystem final : public filesystem::IFileSystem {
 public:
  NfsFileSystem(IServerNetwork& network, allocators::IAllocator& allocator);
  ~NfsFileSystem();
  NfsFileSystem(const NfsFileSystem&) = delete;
  NfsFileSystem& operator=(const NfsFileSystem&) = delete;

  // The user and group id requests to `host` carry from now on.
  void set_credentials(StringView host, uint32_t uid, uint32_t gid);
  // Forgets the host: its ids, its exports and its connection.
  void forget(StringView host);
  // Appends the export paths of a server ("/srv/nfs/data") to `paths`.
  filesystem::Status list_exports(StringView host, List<String>* paths);

  // --- IFileSystem
  filesystem::FileSystemInfo info() const override;
  filesystem::Status open_file(StringView path, filesystem::OpenMode mode, UniquePointer<filesystem::IFile>* file) override;
  filesystem::Status open_directory(StringView path, UniquePointer<filesystem::IDirectory>* directory) override;
  filesystem::Status stat(StringView path, filesystem::DirectoryEntry* entry) override;
  filesystem::Status create_directory(StringView path) override;
  filesystem::Status remove(StringView path) override;
  filesystem::Status rename(StringView from, StringView to) override;
  filesystem::Status flush() override { return filesystem::Status::Ok; }

  // For the tests: maps an NFS status code to a filesystem status.
  static filesystem::Status map_status(uint32_t nfs_status);

 private:
  struct FileHandle {
    uint8_t bytes[64] = {};
    uint32_t length = 0;
  };
  struct Attributes {
    uint32_t type = 0;   // 1 file, 2 folder, 5 link, ...
    uint32_t mode = 0;
    uint64_t size = 0;
    bool valid = false;
    bool directory() const { return type == 2; }
  };
  struct Mount {
    explicit Mount(allocators::IAllocator& a) : path(a) {}
    String path;         // the export
    FileHandle root;
  };
  struct Server {
    explicit Server(allocators::IAllocator& a) : host(a), name(a), exports(a), mounts(a) {}
    String host;                  // as typed, "name" or "name:port"
    String name;
    uint16_t port = 111;          // of the port mapper
    uint32_t uid = 65534, gid = 65534;
    bool exports_loaded = false;
    List<String> exports;
    List<Mount> mounts;
    UniquePointer<RpcClient> rpc; // to the NFS service
    uint32_t read_size = 32768, write_size = 32768;
  };
  // Where a path leads.
  struct Located {
    explicit Located(allocators::IAllocator& a) : exports_prefix(a) {}
    SharedPointer<Server> server;
    uint32_t depth = 0;           // 0: the root, 1: a server, 2: inside the server's export tree
    bool in_export = false;       // else a folder made up of export paths (exports_prefix)
    uint32_t mount = 0;           // index into server->mounts
    String exports_prefix;        // the pseudo folder's path, "/srv/nfs"
    FileHandle handle;            // the node itself, when in_export
    Attributes attributes;
  };
  class File;
  class Directory;
  class ListDirectory;
  friend class File;
  friend class Directory;

  struct Parsed {
    explicit Parsed(allocators::IAllocator& a) : server(a), parts(a) {}
    String server;
    List<String> parts;
  };
  bool parse(StringView path, Parsed* out) const;
  SharedPointer<Server> find_server(StringView host, bool create);
  filesystem::Status resolve(const Parsed& where, bool want_attributes, Located* out, uint32_t* tail_from = nullptr);
  filesystem::Status connect_service(Server& server, uint32_t program, uint32_t version, UniquePointer<RpcClient>* out);
  filesystem::Status port_of(Server& server, uint32_t program, uint32_t version, uint16_t* port);
  filesystem::Status ensure_exports(Server& server);
  filesystem::Status mount(Server& server, StringView export_path, uint32_t* index);
  filesystem::Status nfs_call(Server& server, uint32_t procedure, ConstByteView args, ByteBuffer* results);
  filesystem::Status lookup(Server& server, const FileHandle& dir, StringView name, FileHandle* out, Attributes* attributes);
  filesystem::Status walk(Server& server, const FileHandle& from, const Parsed& where, uint32_t first, uint32_t last, FileHandle* out, Attributes* attributes);
  filesystem::Status getattr(Server& server, const FileHandle& handle, Attributes* attributes);
  filesystem::Status create_in(Server& server, const FileHandle& dir, StringView name, bool directory, bool exclusive, FileHandle* out);
  void drop_connection(Server& server);

  IServerNetwork& network_;
  allocators::IAllocator& allocator_;
  mutable kernel::RecursiveMutex mutex_;
  List<SharedPointer<Server>> servers_;
};

}  // namespace network
