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

// Host-side tests for the SFTP and FTPS clients: the SSH data types, trust on first use, the parsing of FTP listings and replies, the status
// mappings, and how the filesystems treat paths and unreachable servers. Against servers: tests/sftp_live.sh and tests/ftps_live.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Network/Ftp/FtpFileSystem.hpp"
#include "Network/HostPort.hpp"
#include "Network/HostTrust.hpp"
#include "Network/Sftp/SftpFileSystem.hpp"
#include "Network/Ssh/SshWire.hpp"

using namespace network;
using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct FixedRandom final : crypto::IRandom {
  bool fill(ByteView out) override { for (uint8_t& b : out) b = 7; return true; }
};

static void test_ssh_wire() {
  TestAllocator a;
  {
    ByteBuffer b(a);
    SshWriter w(&b);
    w.u8(94);
    w.u32(0x01020304);
    w.u64(0x1112131415161718ull);
    w.boolean(true);
    w.string("abcde");
    w.mpint(ConstByteView());                                                    // zero: no bytes
    const uint8_t high[] = {0x00, 0x00, 0x80, 0x01};
    w.mpint(ConstByteView(high, 4));                                             // leading zeros dropped, one put back for the top bit
    const uint8_t low[] = {0x7F, 0xFF};
    w.mpint(ConstByteView(low, 2));
    CHECK(w.ok());
    static const uint8_t expect[] = {94, 1, 2, 3, 4, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 1, 0, 0, 0, 5, 'a', 'b', 'c', 'd', 'e', 0, 0, 0, 0,
                                     0, 0, 0, 3, 0, 0x80, 0x01, 0, 0, 0, 2, 0x7F, 0xFF};
    CHECK(b.size() == sizeof expect && memcmp(b.view().data(), expect, sizeof expect) == 0);
    SshReader r(b.view());
    CHECK(r.u8() == 94 && r.u32() == 0x01020304 && r.u64() == 0x1112131415161718ull && r.boolean());
    CHECK(r.text().equals("abcde"));
    CHECK(r.mpint().empty());
    const ConstByteView m = r.mpint();
    CHECK(m.size() == 2 && m[0] == 0x80 && m[1] == 0x01);
    CHECK(r.mpint().size() == 2 && r.ok() && r.remaining() == 0);
    r.u8();
    CHECK(!r.ok());
  }
  {   // lying lengths and negative numbers are errors
    static const uint8_t lie[] = {0, 0, 0, 100, 1, 2};
    SshReader r((ConstByteView(lie, sizeof lie)));
    r.string();
    CHECK(!r.ok());
    static const uint8_t negative[] = {0, 0, 0, 1, 0x80};
    SshReader n((ConstByteView(negative, sizeof negative)));
    n.mpint();
    CHECK(!n.ok());
    static const uint8_t big[] = {0, 0, 0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    SshReader l((ConstByteView(big, sizeof big)));
    l.string(8);
    CHECK(!l.ok());
  }
  CHECK(ssh_list_has("a,b,c", "a") && ssh_list_has("a,b,c", "c") && ssh_list_has("a,bb,c", "bb") && !ssh_list_has("a,bb,c", "b") && !ssh_list_has("", "a") && !ssh_list_has("abc", "ab"));
  CHECK(a.live == 0);
}

static void test_host_trust() {
  TestAllocator a;
  {
    HostTrust trust(a);
    uint8_t one[32], two[32];
    memset(one, 1, 32);
    memset(two, 2, 32);
    CHECK(trust.check("sftp:host:22", one) == HostTrust::Result::New);
    CHECK(trust.check("sftp:host:22", one) == HostTrust::Result::Match);
    CHECK(trust.check("sftp:host:22", two) == HostTrust::Result::Changed);
    CHECK(trust.check("sftp:host:22", one) == HostTrust::Result::Match);   // (a change is not remembered)
    CHECK(trust.check("sftp:other:22", two) == HostTrust::Result::New);
    trust.forget("sftp:host:22");
    CHECK(trust.check("sftp:host:22", two) == HostTrust::Result::New);
  }
  CHECK(a.live == 0);
  StringView name;
  uint16_t port = 0;
  split_host_port("example.org", 22, &name, &port);
  CHECK(name.equals("example.org") && port == 22);
  split_host_port("example.org:2222", 22, &name, &port);
  CHECK(name.equals("example.org") && port == 2222);
  split_host_port("example.org:0", 22, &name, &port);
  CHECK(name.equals("example.org:0") && port == 22);
  split_host_port("example.org:70000", 22, &name, &port);
  CHECK(port == 22);
  split_host_port("10.0.0.1:21", 22, &name, &port);
  CHECK(name.equals("10.0.0.1") && port == 21);
}

static void test_ftp_parsing() {
  DirectoryEntry e;
  CHECK(FtpFileSystem::parse_list_line("-rw-r--r--   1 ftp      ftp          1234 Jan 02 12:34 report final.txt", &e) && e.name.view().equals("report final.txt") &&
        e.type == NodeType::File && e.size == 1234 && !e.read_only);
  CHECK(FtpFileSystem::parse_list_line("drwxr-xr-x   2 root     root         4096 Jan  1  2020 sub", &e) && e.name.view().equals("sub") && e.type == NodeType::Directory && e.size == 0);
  CHECK(FtpFileSystem::parse_list_line("-r--r--r--   1 ftp      ftp             5 Mar 03 03:03 locked", &e) && e.read_only);
  CHECK(FtpFileSystem::parse_list_line("lrwxrwxrwx   1 ftp      ftp             9 Jan 01 00:00 link -> target", &e) && e.name.view().equals("link") && e.type == NodeType::File);
  CHECK(FtpFileSystem::parse_list_line("-rw-r--r--   1 owner       777 Jan 02 12:34 nogroup.txt", &e) && e.name.view().equals("nogroup.txt") && e.size == 777);   // (no group column)
  CHECK(FtpFileSystem::parse_list_line("10-02-24  09:15AM       <DIR>          Documents", &e) && e.name.view().equals("Documents") && e.type == NodeType::Directory);
  CHECK(FtpFileSystem::parse_list_line("10-02-24  09:15AM              123456 my file.bin", &e) && e.name.view().equals("my file.bin") && e.size == 123456);
  CHECK(!FtpFileSystem::parse_list_line("total 12", &e));
  CHECK(!FtpFileSystem::parse_list_line("drwxr-xr-x   2 root     root         4096 Jan  1  2020 .", &e));
  CHECK(!FtpFileSystem::parse_list_line("drwxr-xr-x   2 root     root         4096 Jan  1  2020 ..", &e));
  CHECK(!FtpFileSystem::parse_list_line("", &e));

  CHECK(FtpFileSystem::parse_mlsx_line("type=file;size=1234;modify=20240102123456;perm=adfrw; some name.txt", &e) && e.name.view().equals("some name.txt") &&
        e.type == NodeType::File && e.size == 1234 && !e.read_only);
  CHECK(FtpFileSystem::parse_mlsx_line("Type=DIR;Size=4096;perm=flcdmpe; folder", &e) && e.type == NodeType::Directory && e.size == 0 && e.name.view().equals("folder"));
  CHECK(FtpFileSystem::parse_mlsx_line("type=file;size=5;perm=r; locked", &e) && e.read_only);
  CHECK(!FtpFileSystem::parse_mlsx_line("type=cdir;perm=el; .", &e));
  CHECK(!FtpFileSystem::parse_mlsx_line("type=pdir;perm=el; ..", &e));
  CHECK(!FtpFileSystem::parse_mlsx_line("size=5; no-type", &e));
  CHECK(!FtpFileSystem::parse_mlsx_line("type=file;size=5;", &e));

  uint16_t port = 0;
  CHECK(FtpFileSystem::parse_passive_reply(229, "Entering Extended Passive Mode (|||6446|)", &port) && port == 6446);
  CHECK(FtpFileSystem::parse_passive_reply(229, "(!!!2121!)", &port) && port == 2121);
  CHECK(!FtpFileSystem::parse_passive_reply(229, "(|||0|)", &port) && !FtpFileSystem::parse_passive_reply(229, "(|||70000|)", &port) && !FtpFileSystem::parse_passive_reply(229, "(|||x|)", &port));
  CHECK(FtpFileSystem::parse_passive_reply(227, "Entering Passive Mode (192,168,1,2,19,137)", &port) && port == 19 * 256 + 137);
  CHECK(!FtpFileSystem::parse_passive_reply(227, "Entering Passive Mode (192,168,1,2,19)", &port) && !FtpFileSystem::parse_passive_reply(227, "(1,2,3,4,5,300)", &port));
  CHECK(!FtpFileSystem::parse_passive_reply(227, "no brackets", &port));

  CHECK(FtpFileSystem::map_reply(550) == Status::NotFound && FtpFileSystem::map_reply(530) == Status::LoginRequired && FtpFileSystem::map_reply(421) == Status::Network &&
        FtpFileSystem::map_reply(552) == Status::NoSpace && FtpFileSystem::map_reply(553) == Status::AccessDenied && FtpFileSystem::map_reply(502) == Status::Unsupported &&
        FtpFileSystem::map_reply(450) == Status::Busy && FtpFileSystem::map_reply(999) == Status::IoError);
}

static void test_sftp_status() {
  CHECK(SftpFileSystem::map_status(0) == Status::Ok && SftpFileSystem::map_status(1) == Status::EndOfDirectory && SftpFileSystem::map_status(2) == Status::NotFound &&
        SftpFileSystem::map_status(3) == Status::AccessDenied && SftpFileSystem::map_status(4) == Status::IoError && SftpFileSystem::map_status(5) == Status::InvalidArgument &&
        SftpFileSystem::map_status(7) == Status::Network && SftpFileSystem::map_status(8) == Status::Unsupported && SftpFileSystem::map_status(99) == Status::IoError);
}

// A network where no server can be reached.
struct DeadNetwork final : IServerNetwork {
  int attempts = 0;
  Status connect(StringView, uint16_t, uint32_t, SharedPointer<ITcpSocket>*, INetworkStack**) override {
    ++attempts;
    return Status::Network;
  }
};

template <class Fs>
static void check_unreachable(Fs& fs, DeadNetwork& net, TestAllocator& a) {
  fs.set_credentials("10.1.2.3", "alice", "secret");
  UniquePointer<filesystem::IDirectory> dir;
  CHECK(fs.open_directory("/", &dir) == Status::Ok && dir);
  DirectoryEntry e;
  CHECK(dir->next(&e) == Status::Ok && e.name.view().equals("10.1.2.3") && e.type == NodeType::Directory);   // (the server given a login)
  CHECK(dir->next(&e) == Status::EndOfDirectory);
  dir = UniquePointer<filesystem::IDirectory>();
  CHECK(fs.stat("/", &e) == Status::Ok && e.type == NodeType::Directory);
  CHECK(fs.open_directory("/10.1.2.3", &dir) == Status::Network && net.attempts >= 1);
  CHECK(fs.stat("/10.1.2.3/a/b", &e) == Status::Network);
  UniquePointer<filesystem::IFile> file;
  CHECK(fs.open_file("/10.1.2.3/a", filesystem::OpenMode::Read, &file) == Status::Network);
  CHECK(fs.create_directory("/10.1.2.3/new") == Status::Network);
  CHECK(fs.remove("/10.1.2.3/new") == Status::Network);
  CHECK(fs.rename("/10.1.2.3/a", "/10.1.2.3/b") == Status::Network);
  CHECK(fs.open_directory("/server/../x", &dir) != Status::Ok);
  CHECK(fs.open_directory("/10.1.2.3", &dir) == Status::Network);   // (the login does not change the unreachable)
  fs.forget("10.1.2.3");
  fs.forget("never-seen");
  (void)a;
}

static void test_filesystems_without_servers() {
  TestAllocator a;
  {
    DeadNetwork net;
    FixedRandom random;
    HostTrust trust(a);
    SftpFileSystem sftp(net, a, random, trust);
    check_unreachable(sftp, net, a);
    // Without a login the answer is "login required" before any connection is tried.
    DeadNetwork net2;
    SftpFileSystem sftp2(net2, a, random, trust);
    UniquePointer<filesystem::IDirectory> dir;
    CHECK(sftp2.open_directory("/10.1.2.3/", &dir) == Status::Network || sftp2.open_directory("/10.1.2.3/", &dir) == Status::LoginRequired);
    CHECK(sftp2.open_directory("/host/home", &dir) == Status::LoginRequired && net2.attempts == 0);
  }
  {
    DeadNetwork net;
    FixedRandom random;
    HostTrust trust(a);
    FtpEnvironment environment;
    environment.random = &random;
    environment.trust = &trust;
    FtpFileSystem ftps(net, a, environment);
    check_unreachable(ftps, net, a);
  }
  CHECK(a.live == 0);
  String key(a);
  CHECK(SftpFileSystem::trust_key("Example.ORG", &key) && key.view().equals("sftp:example.org:22"));
  CHECK(SftpFileSystem::trust_key("example.org:2222", &key) && key.view().equals("sftp:example.org:2222"));
  CHECK(FtpFileSystem::trust_key("Example.ORG", &key) && key.view().equals("ftps:example.org:21"));
}

int main() {
  test_ssh_wire();
  test_host_trust();
  test_ftp_parsing();
  test_sftp_status();
  test_filesystems_without_servers();
  if (g_failed) {
    printf("%d checks failed\n", g_failed);
    return 1;
  }
  printf("SshFtpTest: all passed\n");
  return 0;
}
