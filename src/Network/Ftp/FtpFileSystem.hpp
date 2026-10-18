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

// The files of FTP servers reached over TLS (FTPS, RFC 4217) as one filesystem, laid out like the other network filesystems:
//   /                      the servers that have been visited
//   /server                the server's root folder; "server:port" names another port than 21
//   /server/dir/file       the files and folders on the server, with the server's own absolute paths
// The control connection is always encrypted: "explicit" FTPS (AUTH TLS on the usual port 21) or, on port 990, "implicit" FTPS (TLS from the first
// byte). A server that does not offer TLS is refused (Status::Unsupported), never talked to in the clear; the data connections are encrypted too
// (PROT P). The server's certificate is checked against a trust store when the environment has one; without it the first certificate a server
// shows is remembered (HostTrust) and a different one later gives Status::UntrustedHost. The log-in is anonymous until set_credentials() has been
// called for the server; a server that refuses it answers Status::LoginRequired, and a refused user name or password gives LoginFailed. Files are
// read as streams (a sequential reader keeps one data connection open; a read elsewhere starts a new one with REST) and written as streams
// (writes must follow each other; a write at the end of an existing file appends). The connection of a server that drops is made again by
// the next call. Names are case sensitive. Safe from any thread (one request goes at a time).
// Not supported: servers that insist on TLS session reuse for data connections (vsftpd's require_ssl_reuse), IPv6, active mode, proxies.
#pragma once
#include "Crypto/Random.hpp"
#include "Crypto/X509.hpp"
#include "FileSystem/IDirectory.hpp"
#include "FileSystem/IFile.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"
#include "Network/HostTrust.hpp"
#include "Network/ServerNetwork.hpp"
#include "Network/Tls/TlsSocket.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"

namespace network {

struct FtpEnvironment {
  crypto::IRandom* random = nullptr;            // required
  HostTrust* trust = nullptr;                   // required unless `roots` is given: where the certificates seen first are remembered
  const crypto::TrustStore* roots = nullptr;    // certificates must chain to these (and name the host) when given
  uint64_t (*clock)(void* context) = nullptr;   // seconds since 1970, for the certificate dates (none: not checked)
  void* clock_context = nullptr;
};

class FtpFileSystem final : public filesystem::IFileSystem {
 public:
  FtpFileSystem(IServerNetwork& network, allocators::IAllocator& allocator, const FtpEnvironment& environment);
  ~FtpFileSystem();
  FtpFileSystem(const FtpFileSystem&) = delete;
  FtpFileSystem& operator=(const FtpFileSystem&) = delete;

  // The login for `host` from now on; the connection, if there is one, is dropped so that the next call logs in again.
  void set_credentials(StringView host, StringView user, StringView password);
  // Forgets the server: its login and its connection.
  void forget(StringView host);
  // The key HostTrust files a server's certificate under.
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

  // For the tests.
  // What a failed command's reply code means for a file operation.
  static filesystem::Status map_reply(uint32_t code);
  // One line of a "LIST" answer (Unix "ls -l" style or Windows style) or of an "MLSD" answer; false for a line that is not an entry (or is "." / "..").
  static bool parse_list_line(StringView line, filesystem::DirectoryEntry* entry);
  static bool parse_mlsx_line(StringView line, filesystem::DirectoryEntry* entry);
  // The port in a "229 ... (|||port|)" or "227 ... (h1,h2,h3,h4,p1,p2)" reply.
  static bool parse_passive_reply(uint32_t code, StringView text, uint16_t* port);

 private:
  class Transfer {   // the user of a server's data connection, told to let go of it when another command needs the server
   public:
    virtual void stop() = 0;

   protected:
    ~Transfer() = default;
  };
  struct Data {
    SharedPointer<ITcpSocket> socket;
    TlsSocket* tls = nullptr;     // the same object when the connection is encrypted
    bool open() const { return socket.get() != nullptr; }
  };
  struct Server {
    explicit Server(allocators::IAllocator& a) : host(a), name(a), user(a), password(a) {}
    String host;                  // as typed, "name" or "name:port"
    String name;
    uint16_t port = 21;
    String user, password;
    bool has_login = false;
    SharedPointer<ITcpSocket> control;
    TlsSocket* control_tls = nullptr;
    INetworkStack* stack = nullptr;
    uint64_t last_progress_ms = 0;
    uint32_t generation = 0;      // changes with every connection
    bool mlst = false;            // the server lists with MLSD / MLST
    bool rest = true;
    Transfer* active = nullptr;
  };
  struct Parsed {
    explicit Parsed(allocators::IAllocator& a) : server(a), remote(a), last(a) {}
    String server;
    String remote;                // the absolute path on the server; "/" for the root
    String last;
    bool at_root() const { return remote.view().equals("/"); }
  };
  class File;
  class ListDirectory;
  friend class File;

  bool parse(StringView path, Parsed* out) const;
  SharedPointer<Server> find_server(StringView host, bool create);
  filesystem::Status ensure_connected(Server& server);
  void drop_connection(Server& server);
  void stop_active(Server& server);
  // TLS over `inner`: waits for the handshake and checks (or remembers) the certificate.
  filesystem::Status secure(Server& server, SharedPointer<ITcpSocket> inner, SharedPointer<ITcpSocket>* out, TlsSocket** tls);
  filesystem::Status read_reply(Server& server, uint32_t* code, String* text);
  filesystem::Status send_line(Server& server, StringView line);
  // A command and its reply (other transfers are stopped first).
  filesystem::Status command(Server& server, StringView text, uint32_t* code, String* reply);
  filesystem::Status raw_command(Server& server, StringView text, uint32_t* code, String* reply);
  filesystem::Status open_data(Server& server, Data* data);
  // Opens a data connection and starts `verb path` on it (after REST `rest` if that is above 0); the transfer is then under way.
  filesystem::Status start_transfer(Server& server, StringView verb, StringView path, uint64_t rest, Data* data);
  filesystem::Status data_read(Server& server, Data& data, ByteView out, uint32_t* got);
  filesystem::Status data_write(Server& server, Data& data, ConstByteView bytes);
  // Ends a transfer: closes the data connection (after sending what is queued, unless `abort`) and reads the server's last reply.
  filesystem::Status finish_transfer(Server& server, Data& data, bool abort);
  filesystem::Status list_remote(Server& server, StringView path, List<filesystem::DirectoryEntry>* entries);
  filesystem::Status stat_remote(Server& server, const Parsed& where, filesystem::DirectoryEntry* entry);

  IServerNetwork& network_;
  allocators::IAllocator& allocator_;
  FtpEnvironment environment_;
  mutable kernel::RecursiveMutex mutex_;
  List<SharedPointer<Server>> servers_;
};

}  // namespace network
