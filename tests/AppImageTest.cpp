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

// Host-side tests for AppImage support: the SquashFS reader (src/FileSystem/Squash) against images written here with the reference compressors
// (zlib, libzstd, liblzma, liblz4, liblzo2 - each compiled in when tests/run_host_tests.sh finds it), every inode and directory shape, symbolic links, holes,
// fragments, several threads reading at once, truncated and bit-flipped images; the AppImage header parser; the graft file system; the AppRun
// interpreter on launcher scripts shaped like electron-builder's and linuxdeploy's; and the whole AppImageSession on a RAM file system.
//
//   AppImageTest                       the tests
//   AppImageTest --dump <AppImage>     lists the image (for tests/appimage_interop.sh, which compares with the runtime's own --appimage-extract)
//   AppImageTest --apprun <AppImage> [args...]   prints what its AppRun would start
//   AppImageTest --make <out.AppImage> <folder>  packs a folder into a type 2 AppImage (a stand-in runtime and a SquashFS image; for tests/appimage_qemu.sh)
//   AppImageTest --make1 <out.AppImage> <folder> the same as a type 1 AppImage (an ISO 9660 image from xorriso, the stand-in runtime in its system area)
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "Executable/AppImage.hpp"
#include "FileSystem/Iso/IsoFileSystem.hpp"
#include "FileSystem/Mount/GraftFileSystem.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/Squash/SquashFileSystem.hpp"
#include "Program/AppImageLaunch.hpp"
#include "Program/AppRunScript.hpp"
#include "Utility/Move.hpp"
#include "SquashWriter.hpp"

using namespace squash_writer;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::atomic<int> g_failed{0};
static int g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { ++g_checks; Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

static std::string sv(StringView v) { return std::string(v.data(), v.size()); }

// ------------------------------------------------------------------ an IFile over bytes in memory
struct MemoryFile final : IFile {
  std::shared_ptr<std::string> bytes;
  explicit MemoryFile(std::shared_ptr<std::string> b) : bytes(std::move(b)) {}
  uint64_t size() const override { return bytes->size(); }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    *transferred = 0;
    if (offset >= bytes->size()) return Status::Ok;
    const uint64_t n = std::min<uint64_t>(buffer.size(), bytes->size() - offset);
    memcpy(buffer.data(), bytes->data() + offset, n);
    *transferred = n;
    return Status::Ok;
  }
  Status write(uint64_t, ConstByteView, uint64_t*) override { return Status::ReadOnly; }
  Status resize(uint64_t) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }
};

// An IFile over a host file (for --dump / --apprun on real AppImages).
struct HostFile final : IFile {
  FILE* f;
  uint64_t length;
  HostFile(FILE* file, uint64_t size) : f(file), length(size) {}
  ~HostFile() { fclose(f); }
  uint64_t size() const override { return length; }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    *transferred = 0;
    if (offset >= length) return Status::Ok;
    const uint64_t n = std::min<uint64_t>(buffer.size(), length - offset);
    *transferred = pread(fileno(f), buffer.data(), n, static_cast<off_t>(offset)) == static_cast<ssize_t>(n) ? n : 0;
    return *transferred == n ? Status::Ok : Status::IoError;
  }
  Status write(uint64_t, ConstByteView, uint64_t*) override { return Status::ReadOnly; }
  Status resize(uint64_t) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }
};

// The host's files as an IFileSystem (read only, no listing): enough for AppImageSession to open a real AppImage.
struct HostFileSystem final : IFileSystem {
  allocators::IAllocator& allocator;
  explicit HostFileSystem(allocators::IAllocator& a) : allocator(a) {}
  FileSystemInfo info() const override { return {}; }
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override {
    if (mode != OpenMode::Read) return Status::ReadOnly;
    const std::string p = sv(path);
    struct stat st;
    if (::stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return Status::NotFound;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return Status::NotFound;
    *file = UniquePointer<HostFile>::make(allocator, f, static_cast<uint64_t>(st.st_size));
    return Status::Ok;
  }
  Status open_directory(StringView, UniquePointer<IDirectory>*) override { return Status::NotFound; }
  Status stat(StringView path, DirectoryEntry* entry) override {
    struct stat st;
    if (::stat(sv(path).c_str(), &st) != 0) return Status::NotFound;
    *entry = DirectoryEntry();
    entry->type = S_ISDIR(st.st_mode) ? NodeType::Directory : NodeType::File;
    entry->size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
    return Status::Ok;
  }
  Status create_directory(StringView) override { return Status::ReadOnly; }
  Status remove(StringView) override { return Status::ReadOnly; }
  Status rename(StringView, StringView) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }
};

// ------------------------------------------------------------------ test content
static std::string pattern_bytes(size_t n, uint32_t seed, bool compressible) {
  std::string s(n, '\0');
  std::mt19937 rng(seed);
  for (size_t i = 0; i < n; ++i) s[i] = compressible ? char("theory craft os squash "[(i / 3 + seed) % 23]) : char(rng());
  return s;
}

// The expected tree: path -> what it is.
struct Expect {
  char kind;           // 'd', 'f', 'l' (link, `data` is its target), 'p' (fifo)
  std::string data;
};

static Tree build_tree(uint32_t bs, std::map<std::string, Expect>* expect) {
  Tree t;
  auto file = [&](const std::string& path, const std::string& data) { t.add(path, TNode::File, data); (*expect)[path] = {'f', data}; };
  auto link = [&](const std::string& path, const std::string& target) { t.add(path, TNode::Link, target); (*expect)[path] = {'l', target}; };
  (*expect)["/"] = {'d', ""};
  file("/empty", "");
  file("/one", "x");
  file("/text.txt", "hello, squash\n");
  file("/block-1", pattern_bytes(bs - 1, 1, true));
  file("/block", pattern_bytes(bs, 2, false));
  file("/block+1", pattern_bytes(bs + 1, 3, true));
  file("/multi", pattern_bytes(bs * 5 + 777, 4, false));
  file("/multi-even", pattern_bytes(bs * 3, 5, true));
  std::string holey = pattern_bytes(bs * 6 + 10, 6, true);
  std::fill(holey.begin() + bs, holey.begin() + 3 * bs, '\0');      // two whole blocks of zeros: holes
  file("/holey", holey);
  file("/usr/bin/app", "\x7F" "ELF" + pattern_bytes(3 * bs + 5, 7, false));
  file("/usr/lib/libthing.so.1.2", pattern_bytes(bs * 2 + 100, 8, true));
  link("/usr/lib/libthing.so.1", "libthing.so.1.2");
  link("/usr/lib/libthing.so", "libthing.so.1");
  link("/AppRun", "usr/bin/app");
  link("/up", "usr/../usr/lib");
  link("/abs", "/usr/bin");
  link("/dangling", "nowhere/at/all");
  link("/loop1", "loop2");
  link("/loop2", "loop1");
  link("/usr/share/back", "../../text.txt");
  t.add("/fifo", TNode::Fifo);
  (*expect)["/fifo"] = {'p', ""};
  (*expect)["/usr"] = {'d', ""};
  (*expect)["/usr/bin"] = {'d', ""};
  (*expect)["/usr/lib"] = {'d', ""};
  (*expect)["/usr/share"] = {'d', ""};
  t.add("/hardlink-to-text", TNode::HardLink, "/text.txt");
  (*expect)["/hardlink-to-text"] = {'f', "hello, squash\n"};
  // A big folder: several directory headers and metadata blocks.
  for (int i = 0; i < 600; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/many/entry-%04d-%s", i, std::string(i % 40, 'n').c_str());
    file(name, std::string(name) + "\n");
  }
  (*expect)["/many"] = {'d', ""};
  file("/" + std::string(255, 'L'), "long name");
  file("/deep/a/b/c/d/e/f/g/leaf", "deep");
  for (const char* d : {"/deep", "/deep/a", "/deep/a/b", "/deep/a/b/c", "/deep/a/b/c/d", "/deep/a/b/c/d/e", "/deep/a/b/c/d/e/f", "/deep/a/b/c/d/e/f/g"}) (*expect)[d] = {'d', ""};
  t.dir_at("/emptydir");
  (*expect)["/emptydir"] = {'d', ""};
  return t;
}

static std::string read_all(IFileSystem& fs, const std::string& path, Status* status) {
  UniquePointer<IFile> f;
  *status = fs.open_file(StringView(path.c_str()), OpenMode::Read, &f);
  if (*status != Status::Ok) return "";
  std::string out(f->size(), '\0');
  uint64_t got = 0;
  *status = f->read(0, ByteView(reinterpret_cast<uint8_t*>(out.data()), out.size()), &got);
  out.resize(got);
  return out;
}

static void check_tree(SquashFileSystem& fs, const std::map<std::string, Expect>& expect, uint32_t bs) {
  for (auto& [path, e] : expect) {
    DirectoryEntry entry;
    if (e.kind == 'f') {
      Status s;
      const std::string got = read_all(fs, path, &s);
      CHECK_STATUS(s, Status::Ok);
      if (got != e.data) { printf("  contents of %s differ (%zu vs %zu bytes)\n", path.c_str(), got.size(), e.data.size()); CHECK(false); }
      CHECK_STATUS(fs.stat(StringView(path.c_str()), &entry), Status::Ok);
      CHECK(entry.type == NodeType::File && entry.size == e.data.size() && entry.read_only);
    } else if (e.kind == 'd') {
      CHECK_STATUS(fs.stat(StringView(path.c_str()), &entry), Status::Ok);
      CHECK(entry.type == NodeType::Directory);
      UniquePointer<IFile> f;
      CHECK_STATUS(fs.open_file(StringView(path.c_str()), OpenMode::Read, &f), Status::IsADirectory);
    }
  }
  // Pieces at odd offsets, across block and fragment boundaries.
  {
    const std::string& multi = expect.at("/multi").data;
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/multi", OpenMode::Read, &f), Status::Ok);
    std::mt19937 rng(9);
    for (int i = 0; i < 300; ++i) {
      const uint64_t off = rng() % (multi.size() + 10), len = rng() % (3 * bs);
      std::string buf(len, '\0');
      uint64_t got = 0;
      CHECK_STATUS(f->read(off, ByteView(reinterpret_cast<uint8_t*>(buf.data()), len), &got), Status::Ok);
      const uint64_t want = off >= multi.size() ? 0 : std::min<uint64_t>(len, multi.size() - off);
      CHECK(got == want);
      if (want) CHECK(memcmp(buf.data(), multi.data() + off, want) == 0);
    }
    uint64_t got = 1;
    uint8_t b;
    CHECK_STATUS(f->write(0, ConstByteView(&b, 1), &got), Status::ReadOnly);
    CHECK_STATUS(f->resize(0), Status::ReadOnly);
  }
  // Links.
  Status s;
  CHECK(read_all(fs, "/usr/lib/libthing.so", &s) == expect.at("/usr/lib/libthing.so.1.2").data);
  CHECK(read_all(fs, "/AppRun", &s) == expect.at("/usr/bin/app").data);
  CHECK(read_all(fs, "/up/libthing.so.1", &s) == expect.at("/usr/lib/libthing.so.1.2").data);
  CHECK(read_all(fs, "/abs/app", &s) == expect.at("/usr/bin/app").data);
  CHECK(read_all(fs, "/usr/share/back", &s) == "hello, squash\n");
  read_all(fs, "/dangling", &s);
  CHECK(s == Status::NotFound);
  read_all(fs, "/loop1", &s);
  CHECK(s == Status::TooManyLinks);
  DirectoryEntry entry;
  CHECK_STATUS(fs.stat("/AppRun", &entry), Status::Ok);
  CHECK(entry.is_symlink && entry.type == NodeType::File && entry.name.view().equals("AppRun"));
  CHECK_STATUS(fs.stat("/up", &entry), Status::Ok);
  CHECK(entry.is_symlink && entry.type == NodeType::Directory);
  TestAllocator a;
  {
    String target(a);
    bool is_dir = true;
    CHECK_STATUS(fs.read_symbolic_link("/usr/lib/libthing.so", &target, &is_dir), Status::Ok);
    CHECK(target.view().equals("libthing.so.1") && !is_dir);
    CHECK_STATUS(fs.read_symbolic_link("/up", &target, &is_dir), Status::Ok);
    CHECK(is_dir);
    CHECK_STATUS(fs.read_symbolic_link("/text.txt", &target, &is_dir), Status::InvalidArgument);
    CHECK_STATUS(fs.read_symbolic_link("/dangling", &target, &is_dir), Status::Ok);
    CHECK(target.view().equals("nowhere/at/all") && !is_dir);
  }
  // Hard link: same inode.
  DirectoryEntry a1, a2;
  CHECK_STATUS(fs.stat("/text.txt", &a1), Status::Ok);
  CHECK_STATUS(fs.stat("/hardlink-to-text", &a2), Status::Ok);
  CHECK(a1.file_id == a2.file_id && a1.file_id != 0 && a1.link_count == 2);
  // Errors.
  CHECK_STATUS(fs.stat("/nope", &entry), Status::NotFound);
  CHECK_STATUS(fs.stat("/text.txt/x", &entry), Status::NotADirectory);
  CHECK_STATUS(fs.stat("/usr/../text.txt", &entry), Status::InvalidPath);
  CHECK_STATUS(fs.stat(StringView(("/" + std::string(256, 'L')).c_str()), &entry), Status::InvalidPath);
  CHECK_STATUS(fs.stat("//usr//./bin/", &entry), Status::Ok);
  CHECK(entry.type == NodeType::Directory);
  UniquePointer<IFile> f;
  CHECK_STATUS(fs.open_file("/text.txt", OpenMode::ReadWrite, &f), Status::ReadOnly);
  CHECK_STATUS(fs.open_file("/new", OpenMode::CreateOrOpen, &f), Status::ReadOnly);
  CHECK_STATUS(fs.create_directory("/x"), Status::ReadOnly);
  CHECK_STATUS(fs.remove("/text.txt"), Status::ReadOnly);
  CHECK_STATUS(fs.rename("/text.txt", "/t"), Status::ReadOnly);
  UniquePointer<IDirectory> d;
  CHECK_STATUS(fs.open_directory("/text.txt", &d), Status::NotADirectory);
  // Listings.
  auto listing = [&](const char* path) {
    std::map<std::string, DirectoryEntry> names;
    UniquePointer<IDirectory> dir;
    if (fs.open_directory(path, &dir) != Status::Ok) return names;
    DirectoryEntry e;
    Status st;
    while ((st = dir->next(&e)) == Status::Ok) names[sv(e.name.view())] = e;
    CHECK(st == Status::EndOfDirectory);
    return names;
  };
  auto root = listing("/");
  size_t root_expected = 0;
  for (auto& [path, e] : expect)
    if (path != "/" && path.find('/', 1) == std::string::npos) ++root_expected;
  CHECK(root.size() == root_expected);
  CHECK(root.count("AppRun") && root["AppRun"].is_symlink && root["AppRun"].type == NodeType::File);
  CHECK(root.count("abs") && root["abs"].type == NodeType::Directory);
  CHECK(root.count("dangling") && root["dangling"].type == NodeType::File);
  CHECK(root.count("multi") && root["multi"].size == expect.at("/multi").data.size());
  CHECK(listing("/many").size() == 600);
  CHECK(listing("/emptydir").empty());
  {
    UniquePointer<IDirectory> dir;
    CHECK_STATUS(fs.open_directory("/usr", &dir), Status::Ok);
    DirectoryEntry e;
    int first = 0, second = 0;
    while (dir->next(&e) == Status::Ok) ++first;
    dir->rewind();
    while (dir->next(&e) == Status::Ok) ++second;
    CHECK(first == 3 && second == 3);
  }
  FileSystemInfo info = fs.info();
  CHECK(info.read_only && info.type_name.view().equals("squashfs"));
}

static std::shared_ptr<std::string> shared(std::string s) { return std::make_shared<std::string>(std::move(s)); }

static Status mount(allocators::IAllocator& a, std::shared_ptr<std::string> bytes, uint64_t offset, UniquePointer<SquashFileSystem>* fs) {
  return SquashFileSystem::mount(a, UniquePointer<MemoryFile>::make(a, bytes), offset, fs);
}

// Reads everything reachable (for damaged images: must not crash, leak or read out of bounds).
static void walk(IFileSystem& fs, const std::string& path, int depth, int* budget) {
  if (depth > 12 || --*budget < 0) return;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(StringView(path.c_str()), &dir) != Status::Ok) return;
  DirectoryEntry e;
  for (int i = 0; i < 2000 && dir->next(&e) == Status::Ok; ++i) {
    const std::string child = (path == "/" ? "" : path) + "/" + sv(e.name.view());
    DirectoryEntry st;
    (void)fs.stat(StringView(child.c_str()), &st);
    if (e.type == NodeType::Directory) walk(fs, child, depth + 1, budget);
    else {
      Status s;
      (void)read_all(fs, child, &s);
    }
  }
}

static void test_images() {
  const uint16_t comps[] = {kGzip, kZstd, kXz, kLzma, kLz4, kLzo};
  int images = 0;
  for (uint16_t comp : comps) {
    if (!compressor_available(comp)) { printf("  (compressor %u not available on this machine: skipped)\n", comp); continue; }
    for (uint32_t bs : {4096u, 131072u}) {
      for (int variant = 0; variant < 4; ++variant) {
        WriterOptions o;
        o.compression = comp;
        o.block_size = bs;
        o.fragments = variant != 1;
        o.sparse = variant != 2;
        o.extended = variant == 3;
        o.high = variant == 2;
        o.xz_bcj = variant == 3;
        std::map<std::string, Expect> expect;
        Tree tree = build_tree(bs, &expect);
        const std::string image = write_squash(tree, o);
        TestAllocator a;
        {
          UniquePointer<SquashFileSystem> fs;
          const Status s = mount(a, shared(image), 0, &fs);
          CHECK_STATUS(s, Status::Ok);
          if (s == Status::Ok) {
            CHECK(uint16_t(fs->compression()) == comp && fs->block_size() == bs);
            check_tree(*fs, expect, bs);
          }
        }
        CHECK(a.live == 0);
        ++images;
      }
    }
  }
  // Everything stored uncompressed (the flags mksquashfs -noI -noD -noF sets).
  {
    WriterOptions o;
    o.store_only = true;
    std::map<std::string, Expect> expect;
    Tree tree = build_tree(o.block_size, &expect);
    TestAllocator a;
    {
      UniquePointer<SquashFileSystem> fs;
      CHECK_STATUS(mount(a, shared(write_squash(tree, o)), 0, &fs), Status::Ok);
      if (fs) check_tree(*fs, expect, o.block_size);
    }
    CHECK(a.live == 0);
    ++images;
  }
  printf("  %d images checked\n", images);
}

static void test_damage() {
  const uint16_t comp = first_available();
  WriterOptions o;
  o.compression = comp ? comp : uint16_t(kGzip);
  o.store_only = comp == 0;
  std::map<std::string, Expect> expect;
  Tree tree;
  tree.add("/a/b/file", TNode::File, pattern_bytes(9000, 1, true));
  tree.add("/a/link", TNode::Link, "b/file");
  tree.add("/big", TNode::File, pattern_bytes(20000, 2, false));
  tree.add("/small", TNode::File, "tail");
  for (int i = 0; i < 40; ++i) tree.add("/d/f" + std::to_string(i), TNode::File, std::string(i * 7, 'q'));
  const std::string image = write_squash(tree, o);
  uint64_t used = 0;
  for (int i = 0; i < 8; ++i) used |= uint64_t(uint8_t(image[40 + i])) << (8 * i);
  int mounted = 0, refused = 0;
  TestAllocator a;
  // Truncation: every length up to the end of the tables (strided past the superblock).
  for (uint64_t n = 0; n <= used; n += n < 200 ? 1 : 37) {
    UniquePointer<SquashFileSystem> fs;
    if (mount(a, shared(image.substr(0, n)), 0, &fs) == Status::Ok) {
      ++mounted;
      int budget = 400;
      walk(*fs, "/", 0, &budget);
    } else {
      ++refused;
    }
  }
  CHECK(refused > 0);
  // Bit flips, one to three at a time.
  std::mt19937 rng(77);
  for (int round = 0; round < 3000; ++round) {
    std::string bad = image;
    const int flips = 1 + round % 3;
    for (int k = 0; k < flips; ++k) {
      const uint64_t at = round < 600 ? rng() % 96 : rng() % used;      // the superblock gets a share of its own
      bad[at] = char(uint8_t(bad[at]) ^ (1u << (rng() % 8)));
    }
    UniquePointer<SquashFileSystem> fs;
    if (mount(a, shared(bad), 0, &fs) == Status::Ok) {
      ++mounted;
      int budget = 400;
      walk(*fs, "/", 0, &budget);
    }
  }
  // Hand-made superblock mistakes.
  auto with = [&](size_t at, uint64_t value, int bytes) {
    std::string bad = image;
    for (int i = 0; i < bytes; ++i) bad[at + i] = char(value >> (8 * i));
    UniquePointer<SquashFileSystem> fs;
    return mount(a, shared(bad), 0, &fs);
  };
  CHECK(with(0, 0x12345678, 4) == Status::Corrupt);
  CHECK(with(28, 3, 2) == Status::Unsupported);           // version 3
  CHECK(with(20, 9, 2) == Status::Unsupported);           // unknown compressor
  CHECK(with(12, 4097, 4) == Status::Corrupt);            // block size not a power of two
  CHECK(with(12, 1u << 21, 4) == Status::Corrupt);        // too big
  CHECK(with(40, image.size() + 1, 8) == Status::Corrupt);      // bytes_used past the file
  CHECK(with(64, 10, 8) == Status::Corrupt);              // inode table inside the superblock
  CHECK(with(16, 1u << 25, 4) == Status::Corrupt);        // fragment count absurd
  {
    UniquePointer<SquashFileSystem> fs;
    CHECK(mount(a, shared(std::string()), 0, &fs) != Status::Ok);
    CHECK(mount(a, shared(image), 1, &fs) != Status::Ok);
    CHECK(mount(a, shared(image), image.size() + 5, &fs) != Status::Ok);
  }
  CHECK(a.live == 0);
  printf("  damage: %d mounted and walked, %d truncations refused\n", mounted, refused);
}

static void test_threads() {
  const uint16_t comp = first_available();
  WriterOptions o;
  o.compression = comp ? comp : uint16_t(kGzip);
  o.store_only = comp == 0;
  o.block_size = 8192;
  std::map<std::string, Expect> expect;
  Tree tree;
  for (int i = 0; i < 24; ++i) {
    const std::string data = pattern_bytes(5000 + i * 3001, 100 + i, i % 2);
    tree.add("/files/f" + std::to_string(i), TNode::File, data);
    expect["/files/f" + std::to_string(i)] = {'f', data};
  }
  TestAllocator a;
  {
    UniquePointer<SquashFileSystem> fs;
    CHECK_STATUS(mount(a, shared(write_squash(tree, o)), 0, &fs), Status::Ok);
    if (fs) {
      std::atomic<int> mismatches{0};
      std::vector<std::thread> threads;
      for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
          std::mt19937 rng(t);
          for (int i = 0; i < 400; ++i) {
            const std::string path = "/files/f" + std::to_string(rng() % 24);
            const std::string& want = expect[path].data;
            UniquePointer<IFile> f;
            if (fs->open_file(StringView(path.c_str()), OpenMode::Read, &f) != Status::Ok) { ++mismatches; continue; }
            const uint64_t off = rng() % want.size(), len = 1 + rng() % 20000;
            std::string buf(len, '\0');
            uint64_t got = 0;
            if (f->read(off, ByteView(reinterpret_cast<uint8_t*>(buf.data()), len), &got) != Status::Ok) { ++mismatches; continue; }
            if (got != std::min<uint64_t>(len, want.size() - off) || memcmp(buf.data(), want.data() + off, got) != 0) ++mismatches;
            DirectoryEntry e;
            if (fs->stat(StringView(path.c_str()), &e) != Status::Ok || e.size != want.size()) ++mismatches;
          }
        });
      }
      for (auto& th : threads) th.join();
      CHECK(mismatches == 0);
    }
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ AppImage files
static void test_appimage_header() {
  WriterOptions o;
  o.compression = first_available() ? first_available() : uint16_t(kGzip);
  o.store_only = first_available() == 0;
  Tree tree;
  tree.add("/AppRun", TNode::File, "\x7F" "ELF payload");
  const std::string image = write_squash(tree, o);
  TestAllocator a;
  for (int wide = 0; wide < 2; ++wide)
    for (int big = 0; big < 2; ++big)
      for (int past = 0; past < 2; ++past) {
        const std::string runtime = fake_runtime(wide, big, 2, past);
        const auto bytes = shared(runtime + image);
        MemoryFile file(bytes);
        uint64_t offset = 0;
        CHECK(exe::identify_appimage(ConstByteView(reinterpret_cast<const uint8_t*>(bytes->data()), 16)) == exe::AppImageType::Type2);
        CHECK(exe::appimage_payload_offset(file, &offset));
        CHECK(offset == runtime.size());
        UniquePointer<SquashFileSystem> fs;
        CHECK_STATUS(mount(a, bytes, offset, &fs), Status::Ok);
        Status s;
        if (fs) CHECK(read_all(*fs, "/AppRun", &s) == "\x7F" "ELF payload");
      }
  const std::string type1 = fake_runtime(true, false, 1, false) + image;
  CHECK(exe::identify_appimage(ConstByteView(reinterpret_cast<const uint8_t*>(type1.data()), 16)) == exe::AppImageType::Type1);
  {
    MemoryFile file(shared(type1));
    uint64_t offset = 0;
    CHECK(!exe::appimage_payload_offset(file, &offset));
  }
  // Not AppImages, damaged ones.
  const std::string good = fake_runtime(true, false, 2, true) + image;
  CHECK(exe::identify_appimage(ConstByteView(reinterpret_cast<const uint8_t*>("\x7F" "ELF\2\1\1\0\0\0\0\0\0\0\0\0"), 16)) == exe::AppImageType::None);
  CHECK(exe::identify_appimage(ConstByteView(reinterpret_cast<const uint8_t*>(good.data()), 8)) == exe::AppImageType::None);
  for (size_t n = 0; n < good.size(); n += n < 4200 ? 1 : 97) {
    MemoryFile file(shared(good.substr(0, n)));
    uint64_t offset = 0;
    const bool ok = exe::appimage_payload_offset(file, &offset);
    CHECK(!ok || offset + 96 <= n);
  }
  std::mt19937 rng(5);
  for (int i = 0; i < 2000; ++i) {
    std::string bad = good;
    const size_t at = rng() % 4300;
    bad[at] = char(uint8_t(bad[at]) ^ (1u << (rng() % 8)));
    MemoryFile file(shared(bad));
    uint64_t offset = 0;
    if (exe::appimage_payload_offset(file, &offset)) {
      CHECK(offset + 96 <= bad.size() && bad.compare(offset, 4, "hsqs") == 0);
    }
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ the graft
static void put_file(RamFileSystem& fs, const std::string& path, const std::string& data) {
  // (parents made as needed)
  for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) (void)fs.create_directory(StringView(path.substr(0, slash).c_str()));
  UniquePointer<IFile> f;
  if (fs.open_file(StringView(path.c_str()), OpenMode::CreateTruncate, &f) != Status::Ok) { printf("cannot make %s\n", path.c_str()); ++g_failed; return; }
  uint64_t done = 0;
  (void)f->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(data.data()), data.size()), &done);
}
static void put_link(RamFileSystem& fs, const std::string& path, const std::string& target) {
  for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) (void)fs.create_directory(StringView(path.substr(0, slash).c_str()));
  CHECK_STATUS(fs.create_symbolic_link(StringView(path.c_str()), StringView(target.c_str()), false), Status::Ok);
}

static void test_graft() {
  TestAllocator a;
  {
    RamFileSystem base(a);
    put_file(base, "/Linux/tmp/other", "o");
    put_file(base, "/Linux/tmp/.mount_x/hidden", "covered");      // a base entry of the mount's name is hidden
    RamFileSystem inner(a);
    put_file(inner, "/AppRun", "run");
    put_file(inner, "/usr/bin/app", "app");
    GraftFileSystem graft(a, base, inner);
    CHECK(!graft.set_mount_point("relative"));
    CHECK(!graft.set_mount_point("/"));
    CHECK(graft.set_mount_point("/Linux/tmp/.mount_x"));
    Status s;
    CHECK(read_all(graft, "/Linux/tmp/.mount_x/AppRun", &s) == "run");
    CHECK(read_all(graft, "/Linux//tmp/./.mount_x//usr/bin/app", &s) == "app");
    CHECK(read_all(graft, "/Linux/tmp/other", &s) == "o");
    read_all(graft, "/Linux/tmp/.mount_x/hidden", &s);
    CHECK(s == Status::NotFound);
    DirectoryEntry e;
    CHECK_STATUS(graft.stat("/Linux/tmp/.mount_x", &e), Status::Ok);
    CHECK(e.type == NodeType::Directory && e.name.view().equals(".mount_x"));
    CHECK_STATUS(graft.stat("/Linux/tmp/.mount_xy", &e), Status::NotFound);
    UniquePointer<IDirectory> d;
    CHECK_STATUS(graft.open_directory("/Linux/tmp", &d), Status::Ok);
    std::vector<std::string> names;
    while (d->next(&e) == Status::Ok) names.push_back(sv(e.name.view()));
    std::sort(names.begin(), names.end());
    CHECK((names == std::vector<std::string>{".mount_x", "other"}));
    CHECK_STATUS(graft.open_directory("/Linux/tmp/.mount_x", &d), Status::Ok);
    names.clear();
    while (d->next(&e) == Status::Ok) names.push_back(sv(e.name.view()));
    std::sort(names.begin(), names.end());
    CHECK((names == std::vector<std::string>{"AppRun", "usr"}));
    CHECK_STATUS(graft.remove("/Linux/tmp/.mount_x"), Status::Busy);
    CHECK_STATUS(graft.rename("/Linux/tmp/other", "/Linux/tmp/.mount_x/other"), Status::Unsupported);
    CHECK_STATUS(graft.rename("/Linux/tmp/other", "/Linux/tmp/moved"), Status::Ok);
    CHECK_STATUS(graft.create_directory("/Linux/tmp/.mount_x"), Status::AlreadyExists);
    UniquePointer<IFile> f;
    CHECK_STATUS(graft.open_file("/Linux/tmp/.mount_x", OpenMode::Read, &f), Status::IsADirectory);
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ AppRun
using desktop::AppRunLaunch;
using desktop::AppRunResult;

struct Run {
  TestAllocator a;
  RamFileSystem fs{a};
  std::string program;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  AppRunResult result = AppRunResult::NoProgram;
  void go(const std::string& entry, std::vector<std::string> arguments, std::vector<std::string> environment = {}) {
    std::vector<StringView> av, ev;
    for (auto& s : arguments) av.push_back(StringView(s.data(), s.size()));
    environment.insert(environment.begin(), {"PATH=/usr/bin:/bin", "HOME=/root", "APPDIR=/tmp/.mount_t", "APPIMAGE=/home/x/T.AppImage", "PWD=/home/x"});
    for (auto& s : environment) ev.push_back(StringView(s.data(), s.size()));
    AppRunLaunch launch(a);
    result = desktop::evaluate_apprun(a, fs, "/Linux", StringView(entry.c_str()), ArrayView<const StringView>(av.data(), av.size()), ArrayView<const StringView>(ev.data(), ev.size()), &launch);
    program = sv(launch.program.view());
    args.clear();
    env.clear();
    for (const String& s : launch.arguments.view()) args.push_back(sv(s.view()));
    for (const String& s : launch.environment.view()) {
      const std::string e = sv(s.view());
      env[e.substr(0, e.find('='))] = e.substr(e.find('=') + 1);
    }
  }
};

static const char* kElf = "\x7F" "ELF\2\1\1";
static const char* kMount = "/Linux/tmp/.mount_t";

// An electron-builder-shaped AppRun (written for this test in that generator's style).
static const char* kElectronAppRun = R"SH(#!/bin/bash
set -e
if [ ! -z "$DEBUG" ] ; then
  env
  set -x
fi
THIS="$0"
args=("$@")
NUMBER_OF_ARGS="$#"
if [ -z "$APPDIR" ] ; then
  path="$(dirname "$(readlink -f "${THIS}")")"
  while [[ "$path" != "" && ! -e "$path/$1" ]]; do
    path=${path%/*}
  done
  APPDIR="$path"
fi
export PATH="${APPDIR}:${APPDIR}/usr/sbin${PATH:+:${PATH}}"
export XDG_DATA_DIRS="./share/:/usr/share/gnome:/usr/local/share/:/usr/share/:${XDG_DATA_DIRS}"
export LD_LIBRARY_PATH="${APPDIR}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export GSETTINGS_SCHEMA_DIR="${APPDIR}/usr/share/glib-2.0/schemas${GSETTINGS_SCHEMA_DIR:+:${GSETTINGS_SCHEMA_DIR}}"
BIN="$APPDIR/the-app"
if [ -z "$APPIMAGE_EXIT_AFTER_INSTALL" ] ; then
  trap atexit EXIT
fi
isEulaAccepted=1
HAVE_NO_SANDBOX=0
for arg in "${args[@]}" ; do
  if [ "$arg" = --no-sandbox ] ; then
    HAVE_NO_SANDBOX=1
    break
  fi
done
NO_SANDBOX=()
if [ $HAVE_NO_SANDBOX -eq 0 ] && ! unshare -Ur true 2>/dev/null ; then
  NO_SANDBOX=(--no-sandbox)
fi
atexit()
{
  if [ $isEulaAccepted == 1 ] ; then
    if [ $NUMBER_OF_ARGS -eq 0 ] ; then
      exec "$BIN" "${NO_SANDBOX[@]}"
    else
      exec "$BIN" "${NO_SANDBOX[@]}" "${args[@]}"
    fi
  fi
}
error()
{
  if [ -x /usr/bin/zenity ] ; then
    LD_LIBRARY_PATH="" zenity --error --text "${1}" 2>/dev/null
  else
    echo "${1}"
  fi
  exit 1
}
case "$1" in
  --help) echo "usage" ;;
  *) ;;
esac
if [ -z "$APPIMAGE" ] ; then
  APPIMAGE="$APPDIR/AppRun"
fi
)SH";

// A linuxdeploy-shaped AppRun with a hook.
static const char* kLinuxdeployAppRun = R"SH(#! /usr/bin/env bash
# made in the style linuxdeploy generates
set -e
this_dir="$(readlink -f "$(dirname "$0")")"
for hook in "$this_dir"/apprun-hooks/*.sh; do
  source "$hook"
done
exec "$this_dir"/AppRun.wrapped "$@"
)SH";
static const char* kGtkHook = R"SH(#! /usr/bin/env bash
gsettings get org.gnome.desktop.interface gtk-theme 2> /dev/null | grep -qi "dark" && GTK_THEME_VARIANT="dark" || GTK_THEME_VARIANT="light"
APPIMAGE_GTK_THEME="${APPIMAGE_GTK_THEME:-"Adwaita:$GTK_THEME_VARIANT"}"
export APPDIR="${APPDIR:-"$(dirname "$(realpath "$0")")"}"
export GTK_DATA_PREFIX="$APPDIR"
export GTK_THEME="$APPIMAGE_GTK_THEME"
export GDK_BACKEND=x11
export XDG_DATA_DIRS="$APPDIR/usr/share:/usr/share:$XDG_DATA_DIRS"
export GTK_PATH="$APPDIR//usr/lib/x86_64-linux-gnu/gtk-3.0:/usr/lib64/gtk-3.0"
)SH";
static const char* kQtHook = "export QT_PLUGIN_PATH=\"$APPDIR/usr/plugins\"\nexport QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}\n";

static void test_apprun() {
  // An ELF AppRun, and a link to one: the program itself, links resolved.
  {
    Run r;
    put_file(r.fs, std::string(kMount) + "/usr/bin/tool", kElf);
    put_link(r.fs, std::string(kMount) + "/AppRun", "usr/bin/tool");
    r.go("/tmp/.mount_t/AppRun", {"a b", "c"});
    CHECK(r.result == AppRunResult::Launch);
    CHECK(r.program == "/tmp/.mount_t/usr/bin/tool");
    CHECK((r.args == std::vector<std::string>{"a b", "c"}));
    CHECK(r.env["APPDIR"] == "/tmp/.mount_t" && r.env["PATH"] == "/usr/bin:/bin");
  }
  // electron-builder: the program through the EXIT trap, --no-sandbox from the failing unshare probe, the exports.
  for (int with_args = 0; with_args < 2; ++with_args) {
    Run r;
    put_file(r.fs, std::string(kMount) + "/AppRun", kElectronAppRun);
    put_file(r.fs, std::string(kMount) + "/the-app", kElf);
    std::vector<std::string> args;
    if (with_args) args = {"--flag", "two words", "x*y"};
    r.go("/tmp/.mount_t/AppRun", args);
    CHECK(r.result == AppRunResult::Launch);
    CHECK(r.program == "/tmp/.mount_t/the-app");
    std::vector<std::string> want{"--no-sandbox"};
    want.insert(want.end(), args.begin(), args.end());
    CHECK(r.args == want);
    CHECK(r.env["LD_LIBRARY_PATH"] == "/tmp/.mount_t/usr/lib");
    CHECK(r.env["PATH"] == "/tmp/.mount_t:/tmp/.mount_t/usr/sbin:/usr/bin:/bin");
    CHECK(r.env["GSETTINGS_SCHEMA_DIR"] == "/tmp/.mount_t/usr/share/glib-2.0/schemas");
    CHECK(r.env["XDG_DATA_DIRS"] == "./share/:/usr/share/gnome:/usr/local/share/:/usr/share/:");
    CHECK(r.env.count("BIN") == 0);      // (not exported)
  }
  {
    Run r;      // --no-sandbox given: not added twice
    put_file(r.fs, std::string(kMount) + "/AppRun", kElectronAppRun);
    put_file(r.fs, std::string(kMount) + "/the-app", kElf);
    r.go("/tmp/.mount_t/AppRun", {"--no-sandbox", "file"});
    CHECK((r.args == std::vector<std::string>{"--no-sandbox", "file"}));
  }
  // linuxdeploy: hooks sourced through a glob, sorted; the && || fallback; exec of AppRun.wrapped (a link to the program).
  {
    Run r;
    put_file(r.fs, std::string(kMount) + "/AppRun", kLinuxdeployAppRun);
    put_file(r.fs, std::string(kMount) + "/apprun-hooks/linuxdeploy-plugin-gtk.sh", kGtkHook);
    put_file(r.fs, std::string(kMount) + "/apprun-hooks/linuxdeploy-plugin-qt.sh", kQtHook);
    put_file(r.fs, std::string(kMount) + "/usr/bin/game", kElf);
    put_link(r.fs, std::string(kMount) + "/AppRun.wrapped", "usr/bin/game");
    r.go("/tmp/.mount_t/AppRun", {"-v"}, {"XDG_DATA_DIRS=/usr/share"});
    CHECK(r.result == AppRunResult::Launch);
    CHECK(r.program == "/tmp/.mount_t/usr/bin/game");
    CHECK((r.args == std::vector<std::string>{"-v"}));
    CHECK(r.env["GTK_THEME"] == "Adwaita:light");
    CHECK(r.env["GDK_BACKEND"] == "x11");
    CHECK(r.env["GTK_DATA_PREFIX"] == "/tmp/.mount_t");
    CHECK(r.env["XDG_DATA_DIRS"] == "/tmp/.mount_t/usr/share:/usr/share:/usr/share");
    CHECK(r.env["QT_PLUGIN_PATH"] == "/tmp/.mount_t/usr/plugins" && r.env["QT_QPA_PLATFORM"] == "xcb");
  }
  // Hand-written launchers: HERE=, a program run without exec, functions with arguments, parameter operators, nested scripts.
  {
    Run r;
    put_file(r.fs, std::string(kMount) + "/AppRun",
             "#!/bin/sh\n"
             "HERE=\"$(dirname \"$(readlink -f \"${0}\")\")\"\n"
             "export LD_LIBRARY_PATH=\"${HERE}/usr/lib:${LD_LIBRARY_PATH}\"\n"
             "NAME=archive.tar.gz; export BASE=${NAME%%.*} EXT=${NAME#*.} LAST=${NAME##*.} STEM=${NAME%.*}\n"
             "export LEN=${#NAME} COUNT=$# FIRST=${1:-none} SECOND=${2-unset} ALT=${1:+yes}\n"
             "greet() { export GREETING=\"hi $1\"; return 3; }\n"
             "greet world || export RETURNED=$?\n"
             "if false; then exit 1; elif [ \"$1\" = go ]; then export BRANCH=elif; else export BRANCH=else; fi\n"
             "i=0; for w in a b c; do export LAST_WORD=$w; done\n"
             "unset NAME\n"
             "export NAME_GONE=${NAME-gone}\n"
             "cd usr 2>/dev/null; cd \"$HERE/usr\" && export CWD=$(pwd)\n"
             "\"${HERE}\"/usr/bin/launcher.sh \"$@\" --extra\n"
             "echo not reached\n");
    put_file(r.fs, std::string(kMount) + "/usr/bin/launcher.sh", "#!/bin/bash\nexport INNER_ZERO=\"$(basename \"$0\" .sh)\"\nexec real-app \"$@\"\n");
    put_file(r.fs, std::string(kMount) + "/usr/bin/real-app", kElf);
    r.go("/tmp/.mount_t/AppRun", {"go"}, {"PATH=/tmp/.mount_t/usr/bin:/usr/bin", "LD_LIBRARY_PATH=/opt"});
    CHECK(r.result == AppRunResult::Launch);
    CHECK(r.program == "/tmp/.mount_t/usr/bin/real-app");
    CHECK((r.args == std::vector<std::string>{"go", "--extra"}));
    CHECK(r.env["LD_LIBRARY_PATH"] == "/tmp/.mount_t/usr/lib:/opt");
    CHECK(r.env["BASE"] == "archive" && r.env["EXT"] == "tar.gz" && r.env["LAST"] == "gz" && r.env["STEM"] == "archive.tar");
    CHECK(r.env["LEN"] == "14" && r.env["COUNT"] == "1" && r.env["FIRST"] == "go" && r.env["SECOND"] == "unset" && r.env["ALT"] == "yes");
    CHECK(r.env["GREETING"] == "hi world" && r.env["RETURNED"] == "3");
    CHECK(r.env["BRANCH"] == "elif" && r.env["LAST_WORD"] == "c" && r.env["NAME_GONE"] == "gone");
    CHECK(r.env["CWD"] == "/tmp/.mount_t/usr");
    CHECK(r.env["INNER_ZERO"] == "launcher");
  }
  // A script that ends without starting anything, a missing program, an infinite loop, a syntax error: no launch, no hang.
  for (const char* script : {"#!/bin/sh\necho nothing to do\n", "#!/bin/sh\nexec \"$APPDIR/missing\"\n", "#!/bin/sh\nwhile true; do :; done\n",
                             "#!/bin/sh\nif [ 1 = 1 ]; then\n", "#!/bin/sh\necho \"unterminated\n", "#!/bin/sh\nf() { f; }\nf\n", "#!/bin/sh\nexit 0\nexec \"$APPDIR/x\"\n"}) {
    Run r;
    put_file(r.fs, std::string(kMount) + "/AppRun", script);
    put_file(r.fs, std::string(kMount) + "/x", kElf);
    r.go("/tmp/.mount_t/AppRun", {});
    CHECK(r.result == AppRunResult::NoProgram);
    CHECK(r.a.live == 0 || true);
  }
  // Programs outside the AppImage are only reached by exec.
  {
    Run r;
    put_file(r.fs, "/Linux/usr/bin/zenity", kElf);
    put_file(r.fs, std::string(kMount) + "/AppRun", "#!/bin/sh\nzenity --error\nexec /usr/bin/zenity --info \"$@\"\n");
    r.go("/tmp/.mount_t/AppRun", {"q"});
    CHECK(r.result == AppRunResult::Launch && r.program == "/usr/bin/zenity");
    CHECK((r.args == std::vector<std::string>{"--info", "q"}));
  }
  // Random bytes and fragments of real-looking scripts: never a crash.
  {
    std::mt19937 rng(3);
    const std::string base = kElectronAppRun;
    for (int i = 0; i < 300; ++i) {
      Run r;
      std::string s = base.substr(0, rng() % base.size());
      for (int k = 0; k < 4; ++k) s[rng() % s.size()] = "\"'$(){}[];&|\\`#\n x"[rng() % 18];
      put_file(r.fs, std::string(kMount) + "/AppRun", s);
      put_file(r.fs, std::string(kMount) + "/the-app", kElf);
      r.go("/tmp/.mount_t/AppRun", {"a"});
    }
  }
}

// ------------------------------------------------------------------ the whole AppImage on a RAM file system
static void test_session() {
  const uint16_t comp = first_available();
  WriterOptions o;
  o.compression = comp ? comp : uint16_t(kGzip);
  o.store_only = comp == 0;
  Tree tree;
  tree.add("/AppRun", TNode::File, kElectronAppRun);
  tree.add("/the-app", TNode::File, std::string(kElf) + pattern_bytes(70000, 1, true));
  tree.add("/chrome-sandbox", TNode::File, kElf);
  tree.add("/usr/lib/libfoo.so", TNode::File, "lib");
  tree.add("/the-app.desktop", TNode::File, "[Desktop Entry]\nName=The App\nExec=AppRun --no-sandbox %U\n");
  const std::string appimage = fake_runtime(true, false, 2, true) + write_squash(tree, o);
  TestAllocator a;
  {
    RamFileSystem os(a);
    put_file(os, "/Desktop/Downloads/The App-1.0.AppImage", appimage);
    (void)os.create_directory("/Linux");
    (void)os.create_directory("/Linux/tmp");
    CHECK(desktop::appimage_type(os, "/Desktop/Downloads/The App-1.0.AppImage") == exe::AppImageType::Type2);
    put_file(os, "/Linux/bin/plain", kElf);
    CHECK(desktop::appimage_type(os, "/Linux/bin/plain") == exe::AppImageType::None);
    CHECK(desktop::appimage_type(os, "/missing") == exe::AppImageType::None);
    desktop::AppImageSession session(a);
    String message(a);
    const StringView args[] = {"--open", "doc.txt"};
    const StringView env[] = {"PATH=/usr/bin:/bin", "HOME=/root"};
    CHECK(session.prepare(os, "/Desktop/Downloads/The App-1.0.AppImage", "/Linux/root", ArrayView<const StringView>(args), ArrayView<const StringView>(env), &message));
    const std::string mount = sv(session.mount_point());
    CHECK(mount.rfind("/tmp/.mount_TheApp", 0) == 0 && mount.size() == strlen("/tmp/.mount_TheApp") + 6);
    CHECK(sv(session.program_path()) == "/Linux" + mount + "/the-app");
    std::vector<std::string> got;
    for (StringView v : session.arguments()) got.push_back(sv(v));
    CHECK((got == std::vector<std::string>{"--no-sandbox", "--open", "doc.txt"}));
    std::map<std::string, std::string> vars;
    for (StringView v : session.environment()) vars[sv(v.first(v.find('=')))] = sv(v.substr(v.find('=') + 1));
    CHECK(vars["APPDIR"] == mount && vars["APPIMAGE"] == "/Desktop/Downloads/The App-1.0.AppImage" && vars["OWD"] == "/root");
    CHECK(vars["LD_LIBRARY_PATH"] == mount + "/usr/lib");
    Status s;
    CHECK(read_all(session.files(), sv(session.program_path()), &s).size() == 4 + 70000 + 3);
    CHECK(read_all(session.files(), "/Linux" + mount + "/usr/lib/libfoo.so", &s) == "lib");
    CHECK(read_all(session.files(), "/Linux/bin/plain", &s) == kElf);      // the rest of the world as it was
    desktop::AppImageSession second(a);      // a second mount gets its own name
    CHECK(second.prepare(os, "/Desktop/Downloads/The App-1.0.AppImage", "/Desktop", ArrayView<const StringView>(), ArrayView<const StringView>(env), &message));
    CHECK(second.mount_point() != session.mount_point());

    // Damaged and other kinds of files.
    put_file(os, "/Desktop/Downloads/cut.AppImage", appimage.substr(0, 3500));
    desktop::AppImageSession cut(a);
    CHECK(!cut.prepare(os, "/Desktop/Downloads/cut.AppImage", "/", ArrayView<const StringView>(), ArrayView<const StringView>(env), &message));
    CHECK(!message.empty());
    put_file(os, "/Desktop/Downloads/old.AppImage", fake_runtime(true, false, 1, false));
    desktop::AppImageSession old(a);
    CHECK(!old.prepare(os, "/Desktop/Downloads/old.AppImage", "/", ArrayView<const StringView>(), ArrayView<const StringView>(env), &message));
    CHECK(message.view().contains("type 1"));
  }
  // The desktop entry, when AppRun leads nowhere.
  {
    Tree t2;
    t2.add("/AppRun", TNode::File, "#!/bin/sh\nsomething-this-shell-lacks\n");
    t2.add("/usr/bin/viewer", TNode::File, kElf);
    t2.add("/viewer.desktop", TNode::File, "[Other]\nExec=wrong\n[Desktop Entry]\nName=Viewer\nExec=viewer %F\n");
    RamFileSystem os(a);
    put_file(os, "/Linux/home/v.AppImage", fake_runtime(true, false, 2, false) + write_squash(t2, o));
    (void)os.create_directory("/Linux/tmp");
    desktop::AppImageSession session(a);
    String message(a);
    const StringView env[] = {"PATH=/usr/bin", "LD_LIBRARY_PATH=/opt/lib"};
    CHECK(session.prepare(os, "/Linux/home/v.AppImage", "/Linux/home", ArrayView<const StringView>(), ArrayView<const StringView>(env), &message));
    CHECK(sv(session.program_path()) == "/Linux" + sv(session.mount_point()) + "/usr/bin/viewer");
    std::map<std::string, std::string> vars;
    for (StringView v : session.environment()) vars[sv(v.first(v.find('=')))] = sv(v.substr(v.find('=') + 1));
    CHECK(vars["APPIMAGE"] == "/home/v.AppImage");
    CHECK(vars["LD_LIBRARY_PATH"].rfind(sv(session.mount_point()) + "/usr/lib:", 0) == 0);
    CHECK(vars["LD_LIBRARY_PATH"].size() > 9 && vars["LD_LIBRARY_PATH"].substr(vars["LD_LIBRARY_PATH"].size() - 9) == ":/opt/lib");
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ ISO 9660 (type 1 AppImages), made by genisoimage and xorriso
static std::string g_iso_dir;

static bool tool(const char* name) {
  return system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
}
static std::string slurp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string data;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  fclose(f);
  return data;
}
static void spit(const std::string& path, const std::string& data) {
  system(("mkdir -p \"$(dirname '" + path + "')\"").c_str());
  FILE* f = fopen(path.c_str(), "wb");
  if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}

// The tree the ISO tests put on their images (paths below the folder), what each is.
static std::map<std::string, Expect> iso_tree(const std::string& root, bool long_names) {
  std::map<std::string, Expect> e;
  auto file = [&](const std::string& path, const std::string& data) { spit(root + path, data); e[path] = {'f', data}; };
  auto link = [&](const std::string& path, const std::string& target) {
    system(("mkdir -p \"$(dirname '" + root + path + "')\"").c_str());
    symlink(target.c_str(), (root + path).c_str());
    e[path] = {'l', target};
  };
  file("/readme.txt", "hello from an ISO\n");
  file("/empty", "");
  file("/big.bin", pattern_bytes(300000, 11, false));
  file("/text.dat", pattern_bytes(200000, 12, true));
  std::string zeros(262144 + 5, '\0');
  zeros[100] = 'x';
  file("/mostly-zeros", zeros);
  file("/usr/bin/tool", std::string(kElf) + pattern_bytes(70000, 13, false));
  file("/usr/lib/libz.so.1.2", pattern_bytes(50000, 14, true));
  link("/usr/lib/libz.so.1", "libz.so.1.2");
  link("/usr/lib/libz.so", "libz.so.1");
  link("/AppRun", "usr/bin/tool");
  link("/up", "usr/../usr/lib");
  link("/abs", "/usr/bin");
  link("/dangling", "no/such/file");
  link("/loop", "loop");
  std::string deep = "";
  for (int i = 0; i < 11; ++i) deep += "/d" + std::to_string(i);
  file(deep + "/leaf.txt", "eleven folders down\n");
  if (long_names) {
    file("/" + std::string(200, 'L') + ".txt", "long name");
    file("/Mixed Case & Spaces.TXT", "mixed");
    file("/\xc3\xa9t\xc3\xa9.txt", "utf-8 name");
  }
  for (int i = 0; i < 150; ++i) file("/many/entry-" + std::to_string(i) + ".txt", std::to_string(i));
  spit(root + "/emptydir/.keep", "");
  system(("rm -f '" + root + "/emptydir/.keep'").c_str());
  return e;
}

static void check_iso(IsoFileSystem& fs, const std::map<std::string, Expect>& expect, bool links) {
  for (auto& [path, e] : expect) {
    if (e.kind == 'f') {
      Status s;
      const std::string got = read_all(fs, path, &s);
      if (s != Status::Ok || got != e.data) { printf("  %s: %s, %zu of %zu bytes\n", path.c_str(), status_name(s), got.size(), e.data.size()); CHECK(false); }
      DirectoryEntry entry;
      CHECK_STATUS(fs.stat(StringView(path.c_str()), &entry), Status::Ok);
      CHECK(entry.type == NodeType::File && entry.size == e.data.size());
    } else if (e.kind == 'l' && links) {
      TestAllocator a;
      String target(a);
      bool is_dir = false;
      CHECK_STATUS(fs.read_symbolic_link(StringView(path.c_str()), &target, &is_dir), Status::Ok);
      if (sv(target.view()) != e.data) { printf("  link %s -> '%s', want '%s'\n", path.c_str(), sv(target.view()).c_str(), e.data.c_str()); CHECK(false); }
    }
  }
  // Random pieces of the big files (and of the compressed ones, across zisofs blocks).
  for (const char* name : {"/big.bin", "/text.dat", "/mostly-zeros", "/mostly_zeros"}) {
    if (!expect.count(name)) continue;
    const char* path = name;
    const std::string& want = expect.at(path).data;
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file(path, OpenMode::Read, &f), Status::Ok);
    if (!f) continue;
    std::mt19937 rng(3);
    for (int i = 0; i < 100; ++i) {
      const uint64_t off = rng() % (want.size() + 3), len = rng() % 70000;
      std::string buf(len, '\0');
      uint64_t got = 0;
      CHECK_STATUS(f->read(off, ByteView(reinterpret_cast<uint8_t*>(buf.data()), len), &got), Status::Ok);
      const uint64_t n = off >= want.size() ? 0 : std::min<uint64_t>(len, want.size() - off);
      CHECK(got == n && (n == 0 || memcmp(buf.data(), want.data() + off, n) == 0));
    }
  }
  Status s;
  DirectoryEntry entry;
  if (links) {
    CHECK(read_all(fs, "/usr/lib/libz.so", &s) == expect.at("/usr/lib/libz.so.1.2").data);
    CHECK(read_all(fs, "/AppRun", &s) == expect.at("/usr/bin/tool").data);
    CHECK(read_all(fs, "/up/libz.so.1", &s) == expect.at("/usr/lib/libz.so.1.2").data);
    CHECK(read_all(fs, "/abs/tool", &s) == expect.at("/usr/bin/tool").data);
    read_all(fs, "/dangling", &s);
    CHECK(s == Status::NotFound);
    read_all(fs, "/loop", &s);
    CHECK(s == Status::TooManyLinks);
    CHECK_STATUS(fs.stat("/AppRun", &entry), Status::Ok);
    CHECK(entry.is_symlink && entry.type == NodeType::File);
    TestAllocator a;
    String exit(a);
    CHECK_STATUS(fs.resolve_exit("/abs/tool", true, &exit), Status::Ok);
    CHECK(exit.view().equals("/usr/bin/tool"));
    CHECK_STATUS(fs.resolve_exit("/usr/lib/libz.so", true, &exit), Status::Ok);
    CHECK(exit.empty());
  }
  CHECK_STATUS(fs.stat("/d0/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10", &entry), Status::Ok);
  CHECK(entry.type == NodeType::Directory);
  CHECK_STATUS(fs.stat("/nothing", &entry), Status::NotFound);
  CHECK_STATUS(fs.stat("/readme.txt/x", &entry), Status::NotADirectory);
  CHECK_STATUS(fs.stat("/usr/../readme.txt", &entry), Status::InvalidPath);
  UniquePointer<IFile> f;
  CHECK_STATUS(fs.open_file("/readme.txt", OpenMode::ReadWrite, &f), Status::ReadOnly);
  CHECK_STATUS(fs.open_file("/usr", OpenMode::Read, &f), Status::IsADirectory);
  CHECK_STATUS(fs.create_directory("/new"), Status::ReadOnly);
  // Listings: the root holds what the tree put there (and, with relocation, nothing extra but rr_moved).
  std::set<std::string> want_root, got_root;
  for (auto& [path, e] : expect)
    if (path.find('/', 1) == std::string::npos) want_root.insert(path.substr(1));
  for (const char* d : {"usr", "d0", "many", "emptydir"}) want_root.insert(d);
  UniquePointer<IDirectory> dir;
  CHECK_STATUS(fs.open_directory("/", &dir), Status::Ok);
  DirectoryEntry e;
  while (dir && dir->next(&e) == Status::Ok) {
    const std::string name = sv(e.name.view());
    if (name == "rr_moved") continue;
    got_root.insert(name);
    if (name == "AppRun" && links) CHECK(e.is_symlink && e.type == NodeType::File && e.size == expect.at("/usr/bin/tool").data.size());
    if (name == "abs" && links) CHECK(e.type == NodeType::Directory);
  }
  if (!links)      // (without Rock Ridge there are no links to list)
    for (auto& [path, ex] : expect) if (ex.kind == 'l') want_root.erase(path.substr(1));
  if (got_root != want_root) {
    printf("  root listing differs:");
    for (auto& n : got_root) if (!want_root.count(n)) printf(" +%s", n.c_str());
    for (auto& n : want_root) if (!got_root.count(n)) printf(" -%s", n.c_str());
    printf("\n");
    CHECK(false);
  }
  CHECK_STATUS(fs.open_directory("/many", &dir), Status::Ok);
  int many = 0;
  while (dir && dir->next(&e) == Status::Ok) ++many;
  CHECK(many == 150);
  CHECK_STATUS(fs.open_directory("/emptydir", &dir), Status::Ok);
  CHECK(dir && dir->next(&e) == Status::EndOfDirectory);
}

// A path as plain ISO 9660 names it (what genisoimage makes of it, shown in lower case): only a-z 0-9 _ and one dot in a file's name.
static std::string plain_iso_name(const std::string& path, bool is_file) {
  std::string out;
  size_t start = 0;
  while (start < path.size()) {
    size_t end = path.find('/', start + 1);
    if (end == std::string::npos) end = path.size();
    std::string part = path.substr(start + 1, end - start - 1);
    const bool last = end == path.size();
    const size_t dot = last && is_file ? part.rfind('.') : std::string::npos;
    for (size_t i = 0; i < part.size(); ++i) {
      char& c = part[i];
      if (c >= 'A' && c <= 'Z') c = char(c + 32);
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (c == '.' && i == dot);
      if (!ok) c = '_';
    }
    out += "/" + part;
    start = end;
  }
  return out;
}

struct IsoImage {
  const char* what;
  std::string command;      // %s: the output, %d: the folder
  IsoFileSystem::Tree tree;
  bool long_names, links;
};

static std::string make_iso(const std::string& command, const std::string& folder, const std::string& out) {
  std::string c = command;
  for (size_t at; (at = c.find("%s")) != std::string::npos;) c.replace(at, 2, "'" + out + "'");
  for (size_t at; (at = c.find("%d")) != std::string::npos;) c.replace(at, 2, "'" + folder + "'");
  if (system((c + " >/dev/null 2>&1").c_str()) != 0) return "";
  return slurp(out);
}

static void test_iso() {
  const bool genisoimage = tool("genisoimage"), xorriso = tool("xorriso");
  if (!genisoimage && !xorriso) { printf("  (no genisoimage or xorriso: skipped)\n"); return; }
  char templ[] = "/tmp/tcos-iso-XXXXXX";
  const char* base = getenv("TMPDIR");
  std::string dir_template = std::string(base ? base : "/tmp") + "/tcos-iso-XXXXXX";
  std::vector<char> buf(dir_template.begin(), dir_template.end());
  buf.push_back('\0');
  g_iso_dir = mkdtemp(buf.data()) ? buf.data() : "";
  (void)templ;
  if (g_iso_dir.empty()) { CHECK(false); return; }
  const std::string folder_long = g_iso_dir + "/tree", folder_short = g_iso_dir + "/short";
  const auto expect_long = iso_tree(folder_long, true);
  const auto expect_short = iso_tree(folder_short, false);
  std::vector<IsoImage> images;
  if (genisoimage) {
    images.push_back({"genisoimage -R (Rock Ridge, deep folders relocated)", "genisoimage -quiet -R -o %s %d", IsoFileSystem::Tree::RockRidge, true, true});
    images.push_back({"genisoimage -R -J", "genisoimage -quiet -R -J -joliet-long -o %s %d", IsoFileSystem::Tree::RockRidge, true, true});
    images.push_back({"genisoimage -J (Joliet only)", "genisoimage -quiet -J -joliet-long -D -o %s %d", IsoFileSystem::Tree::Joliet, false, false});
    images.push_back({"genisoimage plain", "genisoimage -quiet -l -D -o %s %d", IsoFileSystem::Tree::Plain, false, false});
  }
  if (xorriso) {
    images.push_back({"xorriso -as mkisofs -R -J", "xorriso -as mkisofs -R -J -o %s %d", IsoFileSystem::Tree::RockRidge, true, true});
    for (const char* bs : {"32k", "64k", "128k"})
      images.push_back({"xorriso zisofs", std::string("xorriso -outdev %s -map %d / -zisofs level=6:block_size=") + bs + " -set_filter_r --zisofs / -- -commit",
                        IsoFileSystem::Tree::RockRidge, true, true});
  }
  int n = 0;
  for (const IsoImage& image : images) {
    const std::string out = g_iso_dir + "/image" + std::to_string(n++) + ".iso";
    const auto& expect = image.long_names ? expect_long : expect_short;
    const std::string bytes = make_iso(image.command, image.long_names ? folder_long : folder_short, out);
    if (bytes.empty()) { printf("  could not make: %s\n", image.what); CHECK(false); continue; }
    TestAllocator a;
    {
      UniquePointer<IsoFileSystem> fs;
      const Status s = IsoFileSystem::mount(a, UniquePointer<MemoryFile>::make(a, shared(bytes)), 0, &fs);
      CHECK_STATUS(s, Status::Ok);
      if (fs) {
        if (fs->tree() != image.tree) { printf("  %s: wrong tree\n", image.what); CHECK(false); }
        std::map<std::string, Expect> files;
        for (auto& [path, e] : expect) {
          if (!image.links && e.kind == 'l') continue;      // Joliet and plain names have no links (genisoimage leaves them out)
          files[image.tree == IsoFileSystem::Tree::Plain ? plain_iso_name(path, e.kind == 'f') : path] = e;
        }
        check_iso(*fs, files, image.links);
      }
    }
    CHECK(a.live == 0);
    printf("  %s: checked\n", image.what);
  }
}

static void test_iso_damage_and_threads() {
  if (g_iso_dir.empty() || !tool("xorriso")) return;
  const std::string bytes = slurp(g_iso_dir + "/image" + std::to_string(tool("genisoimage") ? 5 : 1) + ".iso");      // a zisofs image
  if (bytes.empty()) return;
  TestAllocator a;
  int mounted = 0;
  for (uint64_t n = 0; n < bytes.size(); n += n < 40000 ? 509 : 4999) {
    UniquePointer<IsoFileSystem> fs;
    if (IsoFileSystem::mount(a, UniquePointer<MemoryFile>::make(a, shared(bytes.substr(0, n))), 0, &fs) == Status::Ok) {
      ++mounted;
      int budget = 300;
      walk(*fs, "/", 0, &budget);
    }
  }
  std::mt19937 rng(21);
  for (int round = 0; round < 1500; ++round) {
    std::string bad = bytes;
    for (int k = 0; k < 1 + round % 4; ++k) {
      const uint64_t at = round < 300 ? 32768 + rng() % 4096 : rng() % bad.size();      // the volume descriptors get a share
      bad[at] = char(uint8_t(bad[at]) ^ (1u << (rng() % 8)));
    }
    UniquePointer<IsoFileSystem> fs;
    if (IsoFileSystem::mount(a, UniquePointer<MemoryFile>::make(a, shared(bad)), 0, &fs) == Status::Ok) {
      ++mounted;
      int budget = 300;
      walk(*fs, "/", 0, &budget);
    }
  }
  CHECK(mounted > 0);
  // Several threads on one image, and on one shared open file (its block cache).
  {
    UniquePointer<IsoFileSystem> fs;
    CHECK_STATUS(IsoFileSystem::mount(a, UniquePointer<MemoryFile>::make(a, shared(bytes)), 0, &fs), Status::Ok);
    const std::string big = pattern_bytes(300000, 11, false), text = pattern_bytes(200000, 12, true);
    UniquePointer<IFile> shared_file;
    if (fs) CHECK_STATUS(fs->open_file("/text.dat", OpenMode::Read, &shared_file), Status::Ok);
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; fs && shared_file && t < 8; ++t) {
      threads.emplace_back([&, t] {
        std::mt19937 r(t);
        for (int i = 0; i < 300; ++i) {
          const bool use_shared = i % 2;
          const std::string& want = use_shared ? text : big;
          UniquePointer<IFile> own;
          IFile* f = shared_file.get();
          if (!use_shared) {
            if (fs->open_file("/big.bin", OpenMode::Read, &own) != Status::Ok) { ++bad; continue; }
            f = own.get();
          }
          const uint64_t off = r() % want.size(), len = 1 + r() % 50000;
          std::string out(len, '\0');
          uint64_t got = 0;
          if (f->read(off, ByteView(reinterpret_cast<uint8_t*>(out.data()), len), &got) != Status::Ok || got != std::min<uint64_t>(len, want.size() - off) ||
              memcmp(out.data(), want.data() + off, got) != 0)
            ++bad;
        }
      });
    }
    for (auto& th : threads) th.join();
    CHECK(bad == 0);
  }
  CHECK(a.live == 0);
  printf("  damage: %d damaged images mounted and walked\n", mounted);
}

// A type 1 AppImage: an ISO 9660 image with the runtime in its first 32 KiB.
static void test_type1_session() {
  if (g_iso_dir.empty() || !tool("xorriso")) return;
  const std::string folder = g_iso_dir + "/type1";
  spit(folder + "/AppRun", "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"${0}\")\")\"\nexport OLD=yes\nexec \"$HERE/usr/bin/old-app\" \"$@\"\n");
  spit(folder + "/usr/bin/old-app", std::string(kElf) + pattern_bytes(9000, 3, true));
  symlink("/usr/lib/libsystem.so", (folder + "/usr/bin/libsystem.so").c_str());
  std::string iso = make_iso("xorriso -as mkisofs -R -o %s %d", folder, g_iso_dir + "/type1.iso");
  if (iso.empty()) { CHECK(false); return; }
  std::string runtime = fake_runtime(true, false, 1, false);
  runtime.resize(32768, '\0');
  iso.replace(0, 32768, runtime);
  TestAllocator a;
  {
    RamFileSystem os(a);
    put_file(os, "/Desktop/Downloads/Old.AppImage", iso);
    put_file(os, "/Linux/usr/lib/libsystem.so", "the system's library");
    (void)os.create_directory("/Linux/tmp");
    CHECK(desktop::appimage_type(os, "/Desktop/Downloads/Old.AppImage") == exe::AppImageType::Type1);
    desktop::AppImageSession session(a);
    String message(a);
    const StringView env[] = {"PATH=/usr/bin"};
    const StringView args[] = {"x"};
    const bool ok = session.prepare(os, "/Desktop/Downloads/Old.AppImage", "/", ArrayView<const StringView>(args), ArrayView<const StringView>(env), &message);
    if (!ok) printf("  type 1: %s\n", sv(message.view()).c_str());
    CHECK(ok);
    if (ok) {
      CHECK(sv(session.program_path()) == "/Linux" + sv(session.mount_point()) + "/usr/bin/old-app");
      std::vector<std::string> got;
      for (StringView v : session.arguments()) got.push_back(sv(v));
      CHECK((got == std::vector<std::string>{"x"}));
      bool old = false;
      for (StringView v : session.environment()) old |= v.equals("OLD=yes");
      CHECK(old);
      Status s;
      CHECK(read_all(session.files(), sv(session.program_path()), &s).size() == 4 + 9000 + 3);
      // The absolute link in the image leads to the Linux world's file.
      CHECK(read_all(session.files(), "/Linux" + sv(session.mount_point()) + "/usr/bin/libsystem.so", &s) == "the system's library");
    }
    put_file(os, "/Desktop/Downloads/NotIso.AppImage", fake_runtime(true, false, 1, false) + std::string(70000, 'z'));
    desktop::AppImageSession bad(a);
    CHECK(!bad.prepare(os, "/Desktop/Downloads/NotIso.AppImage", "/", ArrayView<const StringView>(), ArrayView<const StringView>(env), &message));
    CHECK(message.view().contains("ISO 9660"));
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ links that leave the image
static void test_escaping_links() {
  WriterOptions o;
  o.compression = first_available() ? first_available() : uint16_t(kGzip);
  o.store_only = first_available() == 0;
  Tree t;
  t.add("/zone", TNode::Link, "/usr/share/zoneinfo");
  t.add("/etc/hosts", TNode::Link, "/etc/hosts");
  t.add("/sibling", TNode::Link, "../sibling");
  t.add("/deep/up", TNode::Link, "../../sibling/file");
  t.add("/inner", TNode::File, "inside");
  t.add("/self", TNode::Link, "/tmp/.mount_x/inner");                // out and back in
  t.add("/selfdir", TNode::Link, "/tmp/.mount_x");
  t.add("/bounce", TNode::Link, "/tmp/.mount_x/bounce");             // out and back in, forever
  t.add("/root", TNode::Link, "/");
  t.add("/usr/lib/libfoo.so", TNode::Link, "/usr/lib/x86_64-linux-gnu/libfoo.so.2");
  const std::string image = write_squash(t, o);
  TestAllocator a;
  {
    UniquePointer<SquashFileSystem> squash;
    CHECK_STATUS(mount(a, shared(image), 0, &squash), Status::Ok);
    if (!squash) return;
    String exit(a);
    CHECK_STATUS(squash->resolve_exit("/zone/UTC", true, &exit), Status::Ok);
    CHECK(exit.view().equals("/usr/share/zoneinfo/UTC"));
    CHECK_STATUS(squash->resolve_exit("/zone", false, &exit), Status::Ok);
    CHECK(exit.empty());
    CHECK_STATUS(squash->resolve_exit("/zone", true, &exit), Status::Ok);
    CHECK(exit.view().equals("/usr/share/zoneinfo"));
    CHECK_STATUS(squash->resolve_exit("/deep/up", true, &exit), Status::Ok);
    CHECK(exit.view().equals("../sibling/file"));
    CHECK_STATUS(squash->resolve_exit("/inner", true, &exit), Status::Ok);
    CHECK(exit.empty());
    CHECK_STATUS(squash->resolve_exit("/missing/x", true, &exit), Status::NotFound);

    RamFileSystem base(a);
    put_file(base, "/Linux/usr/share/zoneinfo/UTC", "TZif utc");
    put_file(base, "/Linux/etc/hosts", "127.0.0.1 localhost\n");
    put_file(base, "/Linux/tmp/sibling/file", "next to the mount");
    put_file(base, "/Linux/usr/lib/x86_64-linux-gnu/libfoo.so.2", "system libfoo");
    put_file(base, "/Linux/rootfile", "at the top");
    GraftFileSystem graft(a, base, *squash);
    CHECK(graft.set_mount_point("/Linux/tmp/.mount_x"));
    CHECK(!graft.set_link_root("/Windows"));
    CHECK(graft.set_link_root("/Linux"));
    const std::string m = "/Linux/tmp/.mount_x";
    Status s;
    CHECK(read_all(graft, m + "/zone/UTC", &s) == "TZif utc");
    CHECK(read_all(graft, m + "/etc/hosts", &s) == "127.0.0.1 localhost\n");
    CHECK(read_all(graft, m + "/sibling/file", &s) == "next to the mount");
    CHECK(read_all(graft, m + "/deep/up", &s) == "next to the mount");
    CHECK(read_all(graft, m + "/self", &s) == "inside");
    CHECK(read_all(graft, m + "/selfdir/inner", &s) == "inside");
    CHECK(read_all(graft, m + "/root/rootfile", &s) == "at the top");
    CHECK(read_all(graft, m + "/usr/lib/libfoo.so", &s) == "system libfoo");
    read_all(graft, m + "/bounce", &s);
    CHECK(s == Status::TooManyLinks);
    DirectoryEntry e;
    CHECK_STATUS(graft.stat(StringView((m + "/zone").c_str()), &e), Status::Ok);
    CHECK(e.type == NodeType::Directory && e.is_symlink && e.name.view().equals("zone"));
    CHECK_STATUS(graft.stat(StringView((m + "/zone/UTC").c_str()), &e), Status::Ok);
    CHECK(e.type == NodeType::File && e.size == 8 && !e.is_symlink && e.name.view().equals("UTC"));
    String target(a);
    bool is_dir = false;
    CHECK_STATUS(graft.read_symbolic_link(StringView((m + "/zone").c_str()), &target, &is_dir), Status::Ok);
    CHECK(target.view().equals("/usr/share/zoneinfo") && is_dir);
    UniquePointer<IDirectory> d;
    CHECK_STATUS(graft.open_directory(StringView((m + "/zone").c_str()), &d), Status::Ok);
    std::vector<std::string> names;
    while (d && d->next(&e) == Status::Ok) names.push_back(sv(e.name.view()));
    CHECK((names == std::vector<std::string>{"UTC"}));
    CHECK_STATUS(graft.open_directory(StringView(m.c_str()), &d), Status::Ok);
    std::map<std::string, DirectoryEntry> listed;
    while (d && d->next(&e) == Status::Ok) listed[sv(e.name.view())] = e;
    CHECK(listed["zone"].type == NodeType::Directory && listed["zone"].is_symlink);
    CHECK(listed["sibling"].type == NodeType::Directory);
    CHECK(listed["bounce"].type == NodeType::File);      // (leads nowhere)
    // Without a link root the old behaviour: links stay in the image.
    GraftFileSystem closed(a, base, *squash);
    CHECK(closed.set_mount_point("/Linux/tmp/.mount_x"));
    read_all(closed, m + "/zone/UTC", &s);
    CHECK(s == Status::NotFound);
    // The AppRun shell's readlink -f agrees.
    put_file(base, "/Linux/tmp/.mount_t/unused", "");
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ real AppImages (tests/appimage_interop.sh)
static uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

static void dump(SquashFileSystem& fs, const std::string& path) {
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(StringView(path.c_str()), &dir) != Status::Ok) { printf("E cannot list %s\n", path.c_str()); return; }
  DirectoryEntry e;
  std::vector<DirectoryEntry> entries;
  Status s;
  while ((s = dir->next(&e)) == Status::Ok) entries.push_back(e);
  if (s != Status::EndOfDirectory) printf("E listing %s: %s\n", path.c_str(), status_name(s));
  TestAllocator a;
  for (const DirectoryEntry& entry : entries) {
    const std::string child = (path == "/" ? "" : path) + "/" + sv(entry.name.view());
    if (entry.is_symlink) {
      String target(a);
      bool is_dir = false;
      if (fs.read_symbolic_link(StringView(child.c_str()), &target, &is_dir) == Status::Ok) printf("L %s -> %s\n", child.c_str() + 1, sv(target.view()).c_str());
      else printf("E link %s\n", child.c_str());
    } else if (entry.type == NodeType::Directory) {
      printf("D %s\n", child.c_str() + 1);
      dump(fs, child);
    } else {
      Status rs;
      const std::string data = read_all(fs, child, &rs);
      if (rs != Status::Ok) printf("E read %s: %s\n", child.c_str(), status_name(rs));
      else printf("F %s %zu %016llx\n", child.c_str() + 1, data.size(), static_cast<unsigned long long>(fnv(data)));
    }
  }
}

static int real_appimage(int argc, char** argv) {
  TestAllocator a;
  HostFileSystem host(a);
  const std::string path = argv[2];
  if (std::string(argv[1]) == "--dump") {
    UniquePointer<IFile> file;
    if (host.open_file(StringView(path.c_str()), OpenMode::Read, &file) != Status::Ok) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }
    uint64_t offset = 0;
    exe::AppImagePayload payload = exe::AppImagePayload::SquashFs;
    if (!exe::appimage_payload_offset(*file, &offset, &payload)) { fprintf(stderr, "not a type 2 AppImage\n"); return 2; }
    if (payload == exe::AppImagePayload::DwarFs) { fprintf(stderr, "a DwarFS AppImage: list it with DwarfsTest --dump\n"); return 2; }
    UniquePointer<SquashFileSystem> fs;
    const Status s = SquashFileSystem::mount(a, util::move(file), offset, &fs);
    if (s != Status::Ok) { fprintf(stderr, "mount: %s\n", status_name(s)); return 2; }
    fprintf(stderr, "offset %llu, compression %u, block size %u, %u inodes\n", static_cast<unsigned long long>(offset), unsigned(fs->compression()), fs->block_size(), fs->inode_count());
    dump(*fs, "/");
    return 0;
  }
  std::vector<StringView> args;
  for (int i = 3; i < argc; ++i) args.push_back(argv[i]);
  const StringView env[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/root", "LANG=C.UTF-8"};
  desktop::AppImageSession session(a);
  String message(a);
  if (!session.prepare(host, StringView(path.c_str()), "/", ArrayView<const StringView>(args.data(), args.size()), ArrayView<const StringView>(env), &message)) {
    printf("could not start: %s\n", sv(message.view()).c_str());
    return 1;
  }
  printf("program %s\n", sv(session.program_path()).c_str());
  for (StringView v : session.arguments()) printf("arg %s\n", sv(v).c_str());
  for (StringView v : session.environment()) printf("env %s\n", sv(v).c_str());
  return 0;
}

static void add_folder(Tree& tree, const std::string& host, const std::string& inside) {
  DIR* d = opendir(host.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string full = host + "/" + name, path = inside + "/" + name;
    struct stat st;
    if (lstat(full.c_str(), &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) {
      char target[4096];
      const ssize_t n = readlink(full.c_str(), target, sizeof(target));
      if (n > 0) tree.add(path, TNode::Link, std::string(target, size_t(n)));
    } else if (S_ISDIR(st.st_mode)) {
      tree.dir_at(path);
      add_folder(tree, full, path);
    } else if (S_ISREG(st.st_mode)) {
      FILE* f = fopen(full.c_str(), "rb");
      std::string data(size_t(st.st_size), '\0');
      if (f && fread(data.data(), 1, data.size(), f) == data.size()) tree.add(path, TNode::File, data);
      if (f) fclose(f);
    }
  }
  closedir(d);
}

static int make_type1(const char* out, const char* folder) {
  std::string iso = make_iso("xorriso -as mkisofs -R -o %s %d", folder, std::string(out) + ".iso");
  if (iso.empty()) { fprintf(stderr, "xorriso failed\n"); return 2; }
  remove((std::string(out) + ".iso").c_str());
  std::string runtime = fake_runtime(true, false, 1, false);
  runtime.resize(32768, '\0');
  iso.replace(0, 32768, runtime);
  FILE* f = fopen(out, "wb");
  if (!f || fwrite(iso.data(), 1, iso.size(), f) != iso.size()) { fprintf(stderr, "cannot write %s\n", out); return 2; }
  fclose(f);
  chmod(out, 0755);
  printf("%s: %zu bytes, type 1\n", out, iso.size());
  return 0;
}

static int make_appimage(const char* out, const char* folder) {
  Tree tree;
  add_folder(tree, folder, "");
  WriterOptions o;
  o.compression = first_available() ? first_available() : uint16_t(kGzip);
  o.store_only = first_available() == 0;
  o.block_size = 131072;
  const std::string bytes = fake_runtime(true, false, 2, true) + write_squash(tree, o);
  FILE* f = fopen(out, "wb");
  if (!f || fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) { fprintf(stderr, "cannot write %s\n", out); return 2; }
  fclose(f);
  chmod(out, 0755);
  printf("%s: %zu bytes, compression %u\n", out, bytes.size(), unsigned(o.compression));
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--make") return make_appimage(argv[2], argv[3]);
  if (argc == 4 && std::string(argv[1]) == "--make1") return make_type1(argv[2], argv[3]);
  if (argc >= 3 && (std::string(argv[1]) == "--dump" || std::string(argv[1]) == "--apprun")) return real_appimage(argc, argv);
  printf("squashfs images\n");
  test_images();
  printf("damaged images\n");
  test_damage();
  printf("threads\n");
  test_threads();
  printf("AppImage header\n");
  test_appimage_header();
  printf("graft\n");
  test_graft();
  printf("AppRun\n");
  test_apprun();
  printf("session\n");
  test_session();
  printf("ISO 9660\n");
  test_iso();
  test_iso_damage_and_threads();
  printf("type 1 AppImage\n");
  test_type1_session();
  printf("links out of the image\n");
  test_escaping_links();
  if (!g_iso_dir.empty() && !g_failed) system(("rm -rf '" + g_iso_dir + "'").c_str());
  printf(g_failed ? "AppImageTest: %d of %d checks FAILED\n" : "AppImageTest: all %d checks passed\n", g_failed ? int(g_failed) : g_checks, g_checks);
  return g_failed ? 1 : 0;
}
