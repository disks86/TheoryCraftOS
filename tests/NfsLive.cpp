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

// Command-line NFS client for trying NfsFileSystem against real servers (and tests/nfs_mock_server.py) on the machine's own sockets:
//   NfsLive [--uid N] [--gid N] COMMAND ARGS...
//   ls PATH | stat PATH | cat PATH | put PATH LOCALFILE | write PATH TEXT | append PATH TEXT | rm PATH | mkdir PATH | mv FROM TO | truncate PATH SIZE
// PATH is a path in the NFS filesystem: /server[:port]/export/dir/file (port: of the port mapper). Exit 0 on success; 1 with the status on failure; 2 for bad arguments.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include "PosixNetwork.hpp"
#include "Network/Nfs/NfsFileSystem.hpp"

using namespace network;
using namespace filesystem;

struct HeapAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return size ? malloc(size) : nullptr; }
  void* allocate_aligned(uint64_t size, uint64_t) override { return size ? malloc(size) : nullptr; }
  void deallocate(void* p) override { free(p); }
};

static int fail(const char* what, Status s) {
  printf("FAILED %s: %s\n", what, status_name(s));
  return 1;
}

int main(int argc, char** argv) {
  HeapAllocator alloc;
  PosixNetworkStack stack(alloc);
  SingleStackNetwork net(stack);
  NfsFileSystem fs(net, alloc);
  uint32_t uid = 65534, gid = 65534;
  bool have_ids = false;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--uid" && i + 1 < argc) { uid = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10)); have_ids = true; }
    else if (a == "--gid" && i + 1 < argc) { gid = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10)); have_ids = true; }
    else break;
  }
  if (i >= argc) { fprintf(stderr, "usage: NfsLive [--uid N] [--gid N] COMMAND ARGS...\n"); return 2; }
  const std::string cmd = argv[i++];
  auto arg = [&](int k) { return k < argc ? std::string(argv[k]) : std::string(); };
  const std::string path = arg(i);
  if (have_ids && path.size() > 1) {   // the ids are for the server named first in the path
    const size_t slash = path.find('/', 1);
    fs.set_credentials(StringView(path.data() + 1, (slash == std::string::npos ? path.size() : slash) - 1), uid, gid);
  }
  auto sv = [](const std::string& s) { return StringView(s.data(), s.size()); };
  if (cmd == "ls") {
    UniquePointer<IDirectory> dir;
    Status s = fs.open_directory(sv(path), &dir);
    if (s != Status::Ok) return fail("open_directory", s);
    DirectoryEntry e;
    while ((s = dir->next(&e)) == Status::Ok) printf("%s%s %llu\n", e.name.c_str(), e.type == NodeType::Directory ? "/" : "", (unsigned long long)e.size);
    return s == Status::EndOfDirectory ? 0 : fail("next", s);
  }
  if (cmd == "stat") {
    DirectoryEntry e;
    const Status s = fs.stat(sv(path), &e);
    if (s != Status::Ok) return fail("stat", s);
    printf("%s %s %llu%s\n", e.name.c_str(), e.type == NodeType::Directory ? "directory" : "file", (unsigned long long)e.size, e.read_only ? " read-only" : "");
    return 0;
  }
  if (cmd == "cat") {
    UniquePointer<IFile> f;
    Status s = fs.open_file(sv(path), OpenMode::Read, &f);
    if (s != Status::Ok) return fail("open_file", s);
    uint64_t offset = 0;
    uint8_t buf[100000];
    for (;;) {
      uint64_t got = 0;
      s = f->read(offset, ByteView(buf, sizeof buf), &got);
      if (s != Status::Ok) return fail("read", s);
      if (got == 0) break;
      fwrite(buf, 1, got, stdout);
      offset += got;
    }
    return 0;
  }
  if (cmd == "put" || cmd == "write" || cmd == "append") {
    std::string data;
    if (cmd == "put") {
      FILE* in = fopen(arg(i + 1).c_str(), "rb");
      if (!in) { fprintf(stderr, "cannot read %s\n", arg(i + 1).c_str()); return 2; }
      char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof buf, in)) > 0) data.append(buf, n);
      fclose(in);
    } else {
      data = arg(i + 1);
    }
    UniquePointer<IFile> f;
    Status s = fs.open_file(sv(path), cmd == "append" ? OpenMode::CreateOrOpen : OpenMode::CreateTruncate, &f);
    if (s != Status::Ok) return fail("open_file", s);
    uint64_t wrote = 0;
    s = f->write(cmd == "append" ? f->size() : 0, ConstByteView(reinterpret_cast<const uint8_t*>(data.data()), data.size()), &wrote);
    if (s != Status::Ok || wrote != data.size()) return fail("write", s);
    s = f->flush();
    if (s != Status::Ok) return fail("flush", s);
    printf("wrote %llu bytes\n", (unsigned long long)wrote);
    return 0;
  }
  if (cmd == "truncate") {
    UniquePointer<IFile> f;
    Status s = fs.open_file(sv(path), OpenMode::ReadWrite, &f);
    if (s != Status::Ok) return fail("open_file", s);
    s = f->resize(strtoull(arg(i + 1).c_str(), nullptr, 10));
    return s == Status::Ok ? 0 : fail("resize", s);
  }
  if (cmd == "rm") { const Status s = fs.remove(sv(path)); return s == Status::Ok ? 0 : fail("remove", s); }
  if (cmd == "mkdir") { const Status s = fs.create_directory(sv(path)); return s == Status::Ok ? 0 : fail("create_directory", s); }
  if (cmd == "mv") { const Status s = fs.rename(sv(path), sv(arg(i + 1))); return s == Status::Ok ? 0 : fail("rename", s); }
  fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
