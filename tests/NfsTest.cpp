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

// Host-side tests for the NFS client: the XDR encoding, the NFS status mapping and how NfsFileSystem treats paths and unreachable servers. Against a
// server: tests/nfs_live.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Network/Nfs/NfsFileSystem.hpp"
#include "Network/Nfs/Xdr.hpp"

using namespace network;
using filesystem::Status;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { if (!size) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static void test_xdr_write() {
  TestAllocator a;
  {
    ByteBuffer b(a);
    XdrWriter w(&b);
    w.u32(0x01020304);
    w.u64(0x1112131415161718ull);
    w.boolean(true);
    w.string("abcde");   // length 5, 5 bytes, 3 bytes of padding
    w.fixed(ConstByteView(reinterpret_cast<const uint8_t*>("xy"), 2));
    CHECK(w.ok());
    static const uint8_t expect[] = {1, 2, 3, 4, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0, 0, 0, 1, 0, 0, 0, 5, 'a', 'b', 'c', 'd', 'e', 0, 0, 0, 'x', 'y', 0, 0};
    CHECK(b.size() == sizeof expect && memcmp(b.view().data(), expect, sizeof expect) == 0);
  }
  CHECK(a.live == 0);
}

static void test_xdr_read() {
  TestAllocator a;
  {
    ByteBuffer b(a);
    XdrWriter w(&b);
    w.u32(7);
    w.u64(1ull << 40);
    w.string("hello");
    w.opaque(ConstByteView(reinterpret_cast<const uint8_t*>("0123456789"), 10));
    w.boolean(false);
    XdrReader r(b.view());
    CHECK(r.u32() == 7);
    CHECK(r.u64() == (1ull << 40));
    const StringView s = r.string();
    CHECK(s.size() == 5 && s.equals("hello"));
    CHECK(r.opaque().size() == 10);
    CHECK(!r.boolean());
    CHECK(r.ok() && r.remaining() == 0);
    r.u32();   // past the end
    CHECK(!r.ok());
    CHECK(r.u32() == 0 && r.u64() == 0);   // (an error state reads zeros)
  }
  {   // a length that lies is an error, not an overrun
    ByteBuffer b(a);
    XdrWriter w(&b);
    w.u32(1000);
    w.u32(0);
    XdrReader r(b.view());
    CHECK(r.opaque().empty());
    CHECK(!r.ok());
  }
  {   // a limit on the length
    ByteBuffer b(a);
    XdrWriter w(&b);
    w.string("too long for the limit");
    XdrReader r(b.view());
    r.string(4);
    CHECK(!r.ok());
  }
  {   // padding that does not fit
    static const uint8_t bytes[] = {0, 0, 0, 2, 'a', 'b'};
    XdrReader r((ConstByteView(bytes, sizeof bytes)));
    r.opaque();
    CHECK(!r.ok());
  }
  CHECK(a.live == 0);
}

static void test_status_mapping() {
  CHECK(NfsFileSystem::map_status(0) == Status::Ok);
  CHECK(NfsFileSystem::map_status(1) == Status::AccessDenied);
  CHECK(NfsFileSystem::map_status(13) == Status::AccessDenied);
  CHECK(NfsFileSystem::map_status(2) == Status::NotFound);
  CHECK(NfsFileSystem::map_status(17) == Status::AlreadyExists);
  CHECK(NfsFileSystem::map_status(20) == Status::NotADirectory);
  CHECK(NfsFileSystem::map_status(21) == Status::IsADirectory);
  CHECK(NfsFileSystem::map_status(66) == Status::NotEmpty);
  CHECK(NfsFileSystem::map_status(28) == Status::NoSpace);
  CHECK(NfsFileSystem::map_status(30) == Status::ReadOnly);
  CHECK(NfsFileSystem::map_status(63) == Status::InvalidPath);
  CHECK(NfsFileSystem::map_status(70) == Status::Network);
  CHECK(NfsFileSystem::map_status(10004) == Status::Unsupported);
  CHECK(NfsFileSystem::map_status(5) == Status::IoError);
}

// A network where no server can be reached.
struct DeadNetwork final : IServerNetwork {
  int attempts = 0;
  Status connect(StringView, uint16_t, uint32_t, SharedPointer<ITcpSocket>*, INetworkStack**) override {
    ++attempts;
    return Status::Network;
  }
};

static void test_filesystem_without_servers() {
  TestAllocator a;
  {
    DeadNetwork net;
    NfsFileSystem fs(net, a);
    // The root lists the servers visited: none yet.
    UniquePointer<filesystem::IDirectory> dir;
    CHECK(fs.open_directory("/", &dir) == Status::Ok && dir);
    filesystem::DirectoryEntry e;
    CHECK(dir->next(&e) == Status::EndOfDirectory);
    dir = UniquePointer<filesystem::IDirectory>();
    filesystem::DirectoryEntry root;
    CHECK(fs.stat("/", &root) == Status::Ok && root.type == filesystem::NodeType::Directory);
    // A server that cannot be reached: a network error, and no files.
    CHECK(fs.open_directory("/10.1.2.3", &dir) == Status::Network);
    CHECK(net.attempts >= 1);
    CHECK(fs.stat("/10.1.2.3/export/data/file", &root) == Status::Network);
    UniquePointer<filesystem::IFile> file;
    CHECK(fs.open_file("/10.1.2.3/export/file", filesystem::OpenMode::Read, &file) == Status::Network);
    CHECK(fs.create_directory("/10.1.2.3/export/new") == Status::Network);
    CHECK(fs.remove("/10.1.2.3/export/new") == Status::Network);
    CHECK(fs.rename("/10.1.2.3/export/a", "/10.1.2.3/export/b") == Status::Network);
    List<String> exports(a);
    CHECK(fs.list_exports("10.1.2.3", &exports) == Status::Network);
    CHECK(fs.open_directory("/server/../x", &dir) != Status::Ok);
    // Ids and forgetting are harmless for any host.
    fs.set_credentials("10.1.2.3", 1000, 1000);
    fs.forget("10.1.2.3");
    fs.forget("never-seen");
  }
  CHECK(a.live == 0);
}

int main() {
  test_xdr_write();
  test_xdr_read();
  test_status_mapping();
  test_filesystem_without_servers();
  if (g_failed) {
    printf("%d checks failed\n", g_failed);
    return 1;
  }
  printf("NfsTest: all passed\n");
  return 0;
}
