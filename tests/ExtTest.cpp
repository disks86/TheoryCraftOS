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

// Host-side tests for ExtFileSystem against volumes made by the real mke2fs and judged by the real e2fsck (e2fsprogs): reading trees that
// mke2fs -d fills (every file size class of the block map, symbolic links, long names, directories of many blocks), then everything the
// write side does (create, write, grow, cut, rename, remove, a full volume) followed by e2fsck -fn, randomized operations against a model,
// mount rules, open handles, and truncated and bit-flipped images. Skips quietly if mke2fs or e2fsck is missing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "FileSystem/Ext/ExtFileSystem.hpp"

using namespace filesystem;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
static bool g_verbose = false;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

struct MemDisk : hardware::IStorageDevice {
  MemDisk(const uint8_t* image, uint64_t length, uint32_t block, uint64_t lead_blocks = 0, bool writable = false) : block_size(block), can_write(writable) {
    blocks = lead_blocks + (length + block - 1) / block;
    data = static_cast<uint8_t*>(calloc(blocks, block));
    memcpy(data + lead_blocks * block, image, length);
  }
  virtual ~MemDisk() { free(data); }
  const char* name() const override { return "memdisk"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::StorageInfo info() const override { hardware::StorageInfo i; i.block_count = blocks; i.block_size = block_size; i.read_only = !can_write; return i; }
  hardware::StorageStatistics statistics() const override { return {}; }
  bool read(uint64_t lba, ByteView buffer) override {
    if (buffer.size() % block_size || lba + buffer.size() / block_size > blocks) return false;
    memcpy(buffer.data(), data + lba * block_size, buffer.size());
    return true;
  }
  bool write(uint64_t lba, ConstByteView buffer) override {
    if (!can_write || buffer.size() % block_size || lba + buffer.size() / block_size > blocks) return false;
    if (fail_after >= 0 && writes >= static_cast<uint64_t>(fail_after)) return false;
    ++writes;
    memcpy(data + lba * block_size, buffer.data(), buffer.size());
    return true;
  }
  bool flush() override { return true; }
  uint8_t* data; uint64_t blocks; uint32_t block_size; bool can_write; uint64_t writes = 0; int64_t fail_after = -1;
};

typedef std::vector<uint8_t> Bytes;

static std::string g_work;  // scratch directory
static int sh(const std::string& command) {
  const int r = system(command.c_str());
  return WIFEXITED(r) ? WEXITSTATUS(r) : 255;
}
static bool have_tool(const char* name) { return sh(std::string("command -v ") + name + " >/dev/null 2>&1") == 0; }
static Bytes slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void spit(const std::string& path, const Bytes& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
}

static uint8_t pattern(uint64_t i, uint32_t seed) { return static_cast<uint8_t>(i * 7 + seed + (i >> 8) + (i >> 17)); }
static Bytes make_data(uint64_t size, uint32_t seed) {
  Bytes v(size);
  for (uint64_t i = 0; i < size; ++i) v[i] = pattern(i, seed);
  return v;
}
static std::string str_of(const FixedString<kMaxNameLength + 1>& s) { return std::string(s.view().data(), s.view().size()); }

// ---------------------------------------------------------------------------------------------------------------------------------
// A tree of files, on the host (to fill an image with) and in the filesystem under test

struct Model {
  std::map<std::string, Bytes> files;
  std::set<std::string> dirs;                       // without the root
  std::map<std::string, std::string> links;         // symbolic links: path -> target
};

static std::string parent_of(const std::string& path) { return path.substr(0, path.rfind('/')); }

static void populate(const std::string& root, const Model& m) {
  for (const std::string& d : m.dirs) sh("mkdir -p '" + root + d + "'");
  for (const auto& f : m.files) spit(root + f.first, f.second);
  for (const auto& l : m.links) sh("ln -s '" + l.second + "' '" + root + l.first + "'");
}

static Model standard_model(uint32_t block_size, bool big) {
  Model m;
  m.dirs = {"/dir1", "/dir1/dir2", "/many", "/emptydir"};
  m.files["/empty"] = Bytes();
  m.files["/tiny"] = make_data(5, 1);
  m.files["/one block"] = make_data(block_size, 2);
  m.files["/block+1"] = make_data(block_size + 1, 3);
  m.files["/direct12"] = make_data(12ull * block_size, 4);
  m.files["/indirect"] = make_data(40ull * block_size + 77, 5);
  m.files["/double"] = make_data((12 + block_size / 4 + 300) * uint64_t{block_size} + 3, 6);
  m.files["/dir1/dir2/deep file.txt"] = make_data(1234, 7);
  m.files[std::string("/") + std::string(255, 'n')] = make_data(10, 8);
  m.files["/h\xc3\xa9llo w\xc3\xb6rld.txt"] = make_data(300, 9);
  m.links["/link_fast"] = "tiny";
  m.links["/link_slow"] = std::string(150, 'x') + "/target";
  for (int i = 0; i < 1500; ++i) {
    char name[64];
    snprintf(name, sizeof name, "/many/file_with_a_longish_name_%05d.dat", i);
    m.files[name] = make_data(i % 7 == 0 ? 600 : 30, i);
  }
  (void)big;
  return m;
}

// Builds a sparse file the host way: data at the start and at the given offsets.
static void make_sparse(const std::string& path, const std::vector<std::pair<uint64_t, Bytes>>& pieces) {
  std::string cmd = "rm -f '" + path + "'; : > '" + path + "'";
  sh(cmd);
  FILE* f = fopen(path.c_str(), "wb");
  for (const auto& p : pieces) { fseek(f, static_cast<long>(p.first), SEEK_SET); fwrite(p.second.data(), 1, p.second.size(), f); }
  fclose(f);
}

struct Mounted {
  TestAllocator alloc;
  MemDisk* disk = nullptr;
  UniquePointer<IFileSystem> fs;
  uint64_t lead = 0;
  uint64_t length = 0;
  ~Mounted() { fs = UniquePointer<IFileSystem>(); delete disk; }
  Status mount(const Bytes& image, uint32_t block, bool writable, uint64_t lead_blocks = 0) {
    fs = UniquePointer<IFileSystem>();
    delete disk;
    lead = lead_blocks;
    length = image.size();
    disk = new MemDisk(image.data(), image.size(), block, lead_blocks, writable);
    return ExtFileSystem::mount(alloc, *disk, lead_blocks, &fs);
  }
  Bytes image() { return Bytes(disk->data + lead * disk->block_size, disk->data + lead * disk->block_size + length); }
};

static std::vector<std::string> list_dir(IFileSystem& fs, const std::string& path, std::map<std::string, DirectoryEntry>* entries = nullptr) {
  std::vector<std::string> names;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path.c_str(), &dir) != Status::Ok) { names.push_back("<open failed>"); return names; }
  DirectoryEntry e;
  Status s;
  int guard = 0;
  while ((s = dir->next(&e)) == Status::Ok && ++guard < 100000) { names.push_back(str_of(e.name)); if (entries) (*entries)[str_of(e.name)] = e; }
  if (s != Status::EndOfDirectory) names.push_back(std::string("<") + status_name(s) + ">");
  return names;
}

static bool read_all(IFileSystem& fs, const std::string& path, Bytes* out) {
  UniquePointer<IFile> f;
  Status s = fs.open_file(path.c_str(), OpenMode::Read, &f);
  if (s != Status::Ok) { if (g_verbose) printf("  open %s: %s\n", path.c_str(), status_name(s)); return false; }
  out->assign(f->size(), 0);
  uint64_t got = 0;
  if (!out->empty() && (s = f->read(0, ByteView(out->data(), out->size()), &got)) != Status::Ok) { if (g_verbose) printf("  read %s: %s\n", path.c_str(), status_name(s)); return false; }
  return got == out->size();
}

static Status write_file(IFileSystem& fs, const std::string& path, const Bytes& data) {
  UniquePointer<IFile> f;
  Status s = fs.open_file(path.c_str(), OpenMode::CreateTruncate, &f);
  if (s != Status::Ok) return s;
  uint64_t put = 0;
  if (!data.empty() && (s = f->write(0, ConstByteView(data.data(), data.size()), &put)) != Status::Ok) return s;
  return put == data.size() ? Status::Ok : Status::IoError;
}

// Compares the whole tree of `fs` with the model. Returns the number of differences found (and prints the first few).
static int compare_tree(IFileSystem& fs, const Model& m, const char* label) {
  int bad = 0;
  auto complain = [&](const std::string& what) { if (bad++ < 8) printf("  [%s] %s\n", label, what.c_str()); };
  std::set<std::string> all_dirs = m.dirs;
  all_dirs.insert("");
  for (const std::string& d : all_dirs) {
    std::set<std::string> want;
    for (const auto& f : m.files) if (parent_of(f.first) == d) want.insert(f.first.substr(d.size() + 1));
    for (const auto& l : m.links) if (parent_of(l.first) == d) want.insert(l.first.substr(d.size() + 1));
    for (const std::string& x : m.dirs) if (parent_of(x) == d) want.insert(x.substr(d.size() + 1));
    if (d.empty()) want.insert("lost+found");
    std::map<std::string, DirectoryEntry> entries;
    std::vector<std::string> got = list_dir(fs, d.empty() ? "/" : d, &entries);
    std::set<std::string> got_set(got.begin(), got.end());
    if (got_set.size() != got.size()) complain("duplicate names in " + d);
    if (got_set != want) {
      std::string extra, missing;
      for (const auto& g : got_set) if (!want.count(g)) extra += " +" + g.substr(0, 40);
      for (const auto& w : want) if (!got_set.count(w)) missing += " -" + w.substr(0, 40);
      complain("listing of '" + d + "' differs:" + extra.substr(0, 200) + missing.substr(0, 200));
    }
    for (const auto& e : entries) {
      const std::string path = d + "/" + e.first;
      if (m.dirs.count(path) || path == "/lost+found") { if (e.second.type != NodeType::Directory) complain("not a directory: " + path); }
      else if (e.second.type != NodeType::File) complain("not a file: " + path);
    }
  }
  for (const auto& f : m.files) {
    Bytes got;
    if (!read_all(fs, f.first, &got)) { complain("cannot read " + f.first.substr(0, 60)); continue; }
    if (got != f.second) {
      size_t at = 0;
      while (at < got.size() && at < f.second.size() && got[at] == f.second[at]) ++at;
      char b[160];
      snprintf(b, sizeof b, " differs (size %zu want %zu, first difference at %zu)", got.size(), f.second.size(), at);
      complain(f.first.substr(0, 60) + b);
    }
    DirectoryEntry st;
    if (fs.stat(f.first.c_str(), &st) != Status::Ok || st.size != f.second.size()) complain("stat " + f.first.substr(0, 60));
  }
  for (const auto& l : m.links) {
    Bytes got;
    if (!read_all(fs, l.first, &got) || std::string(got.begin(), got.end()) != l.second) complain("link " + l.first);
  }
  return bad;
}

// e2fsck -fn over an image: 0 when it finds nothing wrong.
static int fsck(const Bytes& image, std::string* log = nullptr) {
  const std::string img = g_work + "/fsck.img", out = g_work + "/fsck.log";
  spit(img, image);
  const int r = sh("e2fsck -fn '" + img + "' > '" + out + "' 2>&1");
  if (r != 0 || g_verbose) {
    Bytes text = slurp(out);
    if (log) *log = std::string(text.begin(), text.end());
    if (r != 0) printf("--- e2fsck exit %d:\n%.1500s\n---\n", r, std::string(text.begin(), text.end()).c_str());
  }
  return r;
}
#define CHECK_FSCK(image) do { if (fsck(image) != 0) { printf("FAILED line %d: e2fsck\n", __LINE__); ++g_failed; } } while (0)

struct Variant {
  const char* name;
  const char* options;  // for mke2fs
  uint32_t megabytes;
  uint32_t block_size;
};

static Bytes make_image(const Variant& v, const std::string& populate_dir = "", const char* extra = "") {
  const std::string img = g_work + "/made.img";
  sh("rm -f '" + img + "'; truncate -s " + std::to_string(v.megabytes) + "M '" + img + "'");
  std::string cmd = std::string("mke2fs -q -F ") + v.options + " " + extra;
  if (!populate_dir.empty()) cmd += " -d '" + populate_dir + "'";
  cmd += " '" + img + "' >" + g_work + "/mke2fs.log 2>&1";
  if (sh(cmd) != 0) { printf("mke2fs failed: %s\n", cmd.c_str()); ++g_failed; return Bytes(); }
  return slurp(img);
}

static std::vector<Variant> ext2_variants() {
  return {
      {"ext2 1k", "-t ext2 -b 1024 -I 128 -N 4000", 24, 1024},
      {"ext2 2k", "-t ext2 -b 2048 -I 256 -N 4000", 24, 2048},
      {"ext2 4k", "-t ext2 -b 4096 -I 256 -N 4000", 32, 4096},
      {"ext2 rev0", "-t ext2 -E revision=0 -b 1024 -N 4000", 24, 1024},
      {"ext2 no sparse", "-t ext2 -b 1024 -O ^sparse_super,^resize_inode -N 4000", 24, 1024},
  };
}

// ---------------------------------------------------------------------------------------------------------------------------------

static void test_read(const Variant& v, uint32_t device_block, uint64_t lead) {
  if (g_verbose) printf("read: %s (device blocks of %u, lead %llu)\n", v.name, device_block, static_cast<unsigned long long>(lead));
  const std::string dir = g_work + "/src";
  sh("rm -rf '" + dir + "'; mkdir -p '" + dir + "'");
  Model m = standard_model(v.block_size, false);
  populate(dir, m);
  Bytes image = make_image(v, dir);
  Mounted mt;
  CHECK_STATUS(mt.mount(image, device_block, false, lead), Status::Ok);
  if (!mt.fs) return;
  const FileSystemInfo info = mt.fs->info();
  CHECK(info.read_only);
  CHECK(info.total_bytes > 0);
  CHECK(strncmp(info.type_name.view().data(), "ext2", 4) == 0);
  CHECK(compare_tree(*mt.fs, m, v.name) == 0);
  // The disk was only read.
  CHECK(mt.disk->writes == 0);
  // Errors
  UniquePointer<IFile> f;
  CHECK_STATUS(mt.fs->open_file("/nothing", OpenMode::Read, &f), Status::NotFound);
  CHECK_STATUS(mt.fs->open_file("/dir1", OpenMode::Read, &f), Status::IsADirectory);
  CHECK_STATUS(mt.fs->open_file("/tiny/x", OpenMode::Read, &f), Status::NotADirectory);
  CHECK_STATUS(mt.fs->open_file("/../tiny", OpenMode::Read, &f), Status::InvalidPath);
  CHECK_STATUS(mt.fs->open_file("/tiny", OpenMode::ReadWrite, &f), Status::ReadOnly);
  CHECK_STATUS(mt.fs->create_directory("/newdir"), Status::ReadOnly);
  CHECK_STATUS(mt.fs->remove("/tiny"), Status::ReadOnly);
  CHECK_STATUS(mt.fs->rename("/tiny", "/other"), Status::ReadOnly);
  UniquePointer<IDirectory> d;
  CHECK_STATUS(mt.fs->open_directory("/tiny", &d), Status::NotADirectory);
  // Reading in odd pieces
  UniquePointer<IFile> g;
  CHECK_STATUS(mt.fs->open_file("/double", OpenMode::Read, &g), Status::Ok);
  if (g) {
    const Bytes& want = m.files["/double"];
    Bytes got(5000);
    for (uint64_t at : {uint64_t{0}, uint64_t{1}, uint64_t{v.block_size - 3}, uint64_t{12} * v.block_size - 100, want.size() - 2000, want.size() - 7}) {
      uint64_t n = 0;
      CHECK_STATUS(g->read(at, ByteView(got.data(), got.size()), &n), Status::Ok);
      CHECK(n == std::min<uint64_t>(5000, want.size() - at));
      CHECK(memcmp(got.data(), want.data() + at, n) == 0);
    }
    uint64_t n = 99;
    CHECK_STATUS(g->read(want.size(), ByteView(got.data(), got.size()), &n), Status::Ok);
    CHECK(n == 0);
    CHECK_STATUS(g->read(want.size() + 100, ByteView(got.data(), got.size()), &n), Status::Ok);
    CHECK(n == 0);
  }
}

// A sparse file reaching the triple indirect range.
static void test_read_sparse() {
  const Variant v = {"ext2 1k sparse", "-t ext2 -b 1024 -I 128 -N 200", 96, 1024};
  const std::string dir = g_work + "/src";
  sh("rm -rf '" + dir + "'; mkdir -p '" + dir + "'");
  Bytes a = make_data(2000, 1), b = make_data(777, 2), c = make_data(1000, 3), d = make_data(10, 4);
  make_sparse(dir + "/sparse", {{0, a}, {300000, b}, {40000000, c}, {70000000, d}});
  Bytes image = make_image(v, dir);
  Mounted mt;
  CHECK_STATUS(mt.mount(image, 512, false), Status::Ok);
  if (!mt.fs) return;
  Model m;
  m.files["/sparse"] = Bytes(70000010, 0);
  std::copy(a.begin(), a.end(), m.files["/sparse"].begin());
  std::copy(b.begin(), b.end(), m.files["/sparse"].begin() + 300000);
  std::copy(c.begin(), c.end(), m.files["/sparse"].begin() + 40000000);
  std::copy(d.begin(), d.end(), m.files["/sparse"].begin() + 70000000);
  CHECK(compare_tree(*mt.fs, m, "sparse") == 0);
}

// Writing: a fresh volume gets files of every size class, then they are changed, renamed and removed.
static void test_write(const Variant& v) {
  if (g_verbose) printf("write: %s\n", v.name);
  Bytes image = make_image(v);
  Mounted mt;
  CHECK_STATUS(mt.mount(image, 512, true), Status::Ok);
  if (!mt.fs) return;
  IFileSystem& fs = *mt.fs;
  CHECK(!fs.info().read_only);
  const uint64_t free_before = fs.info().free_bytes;
  CHECK(free_before > 0);
  Model m;
  CHECK_STATUS(fs.create_directory("/a"), Status::Ok); m.dirs.insert("/a");
  CHECK_STATUS(fs.create_directory("/a/b"), Status::Ok); m.dirs.insert("/a/b");
  CHECK_STATUS(fs.create_directory("/a"), Status::AlreadyExists);
  CHECK_STATUS(fs.create_directory("/nodir/x"), Status::NotFound);
  const uint32_t bs = v.block_size, n = bs / 4;
  const uint64_t sizes[] = {0, 1, bs - 1, bs, bs + 1, 12ull * bs, 12ull * bs + 1, (12 + n) * uint64_t{bs} + 5, (12 + n + 3) * uint64_t{bs}, (12 + n + n * 2ull) * bs + 100};
  int i = 0;
  for (uint64_t size : sizes) {
    const std::string path = "/a/file" + std::to_string(i);
    Bytes d = make_data(size, i);
    CHECK_STATUS(write_file(fs, path, d), Status::Ok);
    m.files[path] = d;
    ++i;
  }
  CHECK(compare_tree(fs, m, v.name) == 0);
  CHECK(fs.info().free_bytes < free_before);
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());

  // overwrite in the middle, append, write past the end (a hole), cut, grow
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/a/file5", OpenMode::ReadWrite, &f), Status::Ok);
    Bytes patch = make_data(3 * bs + 17, 99);
    uint64_t put = 0;
    CHECK_STATUS(f->write(bs - 5, ConstByteView(patch.data(), patch.size()), &put), Status::Ok);
    CHECK(put == patch.size());
    memcpy(m.files["/a/file5"].data() + bs - 5, patch.data(), patch.size());
    Bytes& cur = m.files["/a/file5"];
    Bytes tail = make_data(5000, 55);
    const uint64_t hole_at = cur.size() + 3 * bs + 11;
    CHECK_STATUS(f->write(hole_at, ConstByteView(tail.data(), tail.size()), &put), Status::Ok);
    cur.resize(hole_at + tail.size(), 0);
    memcpy(cur.data() + hole_at, tail.data(), tail.size());
    CHECK(f->size() == cur.size());
    CHECK_STATUS(f->resize(7 * bs + 3), Status::Ok);
    cur.resize(7 * bs + 3);
    CHECK_STATUS(f->resize(9 * bs + 500), Status::Ok);  // (grows with zeros, also where a block's tail held data once)
    cur.resize(9 * bs + 500, 0);
    CHECK_STATUS(f->flush(), Status::Ok);
  }
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/a/file9", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(f->resize(3), Status::Ok);  // from the double indirect range down to a few bytes
    m.files["/a/file9"].resize(3);
    CHECK_STATUS(f->resize(0), Status::Ok);
    m.files["/a/file9"].clear();
    uint64_t put = 0;
    Bytes one = make_data(1, 3);
    CHECK_STATUS(f->write(0, ConstByteView(one.data(), 1), &put), Status::Ok);
    m.files["/a/file9"] = one;
  }
  CHECK(compare_tree(fs, m, v.name) == 0);
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());

  // rename and remove
  CHECK_STATUS(fs.rename("/a/file3", "/a/b/moved"), Status::Ok);
  m.files["/a/b/moved"] = m.files["/a/file3"]; m.files.erase("/a/file3");
  CHECK_STATUS(fs.rename("/a/b", "/b2"), Status::Ok);  // a folder to another parent
  m.dirs.erase("/a/b"); m.dirs.insert("/b2");
  m.files["/b2/moved"] = m.files["/a/b/moved"]; m.files.erase("/a/b/moved");
  CHECK_STATUS(fs.rename("/a", "/b2/a"), Status::Ok);   // a folder with files into another
  {
    Model n2;
    n2.dirs = {"/b2", "/b2/a"};
    for (const auto& f : m.files) n2.files[f.first.compare(0, 3, "/a/") == 0 ? "/b2" + f.first : f.first] = f.second;
    m = n2;
  }
  CHECK(compare_tree(fs, m, v.name) == 0);
  CHECK_STATUS(fs.rename("/b2", "/b2/a/inside"), Status::InvalidArgument);
  CHECK_STATUS(fs.rename("/b2/a", "/b2/a"), Status::AlreadyExists);
  CHECK_STATUS(fs.rename("/missing", "/x"), Status::NotFound);
  CHECK_STATUS(fs.remove("/b2/a"), Status::NotEmpty);
  CHECK_STATUS(fs.remove("/missing"), Status::NotFound);
  CHECK_STATUS(fs.remove("/"), Status::InvalidPath);
  for (auto it = m.files.begin(); it != m.files.end();) {
    if (it->first.compare(0, 6, "/b2/a/") == 0) { CHECK_STATUS(fs.remove(it->first.c_str()), Status::Ok); it = m.files.erase(it); } else ++it;
  }
  CHECK_STATUS(fs.remove("/b2/a"), Status::Ok);
  m.dirs.erase("/b2/a");
  CHECK(compare_tree(fs, m, v.name) == 0);
  CHECK_STATUS(fs.flush(), Status::Ok);
  Bytes after = mt.image();
  CHECK_FSCK(after);

  // The volume mounts again with the same content.
  Mounted again;
  CHECK_STATUS(again.mount(after, 512, true), Status::Ok);
  if (again.fs) CHECK(compare_tree(*again.fs, m, "remount") == 0);

  // Everything removed gives all the space back.
  for (const auto& f : m.files) CHECK_STATUS(fs.remove(f.first.c_str()), Status::Ok);
  CHECK_STATUS(fs.remove("/b2/moved"), Status::NotFound);
  CHECK_STATUS(fs.remove("/b2"), Status::Ok);
  CHECK(fs.info().free_bytes == free_before);
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());
}

static void test_big_directory(const Variant& v) {
  if (g_verbose) printf("bigdir: %s\n", v.name);
  Bytes image = make_image(v);
  Mounted mt;
  CHECK_STATUS(mt.mount(image, 512, true), Status::Ok);
  if (!mt.fs) return;
  IFileSystem& fs = *mt.fs;
  Model m;
  CHECK_STATUS(fs.create_directory("/big"), Status::Ok); m.dirs.insert("/big");
  for (int i = 0; i < 2500; ++i) {
    char name[80];
    snprintf(name, sizeof name, "/big/entry_number_%d_%s", i, std::string(i % 40, 'z').c_str());
    m.files[name] = make_data(i % 5, i);
    CHECK_STATUS(write_file(fs, name, m.files[name]), Status::Ok);
  }
  CHECK(compare_tree(fs, m, v.name) == 0);
  // remove every other one, then add again into the holes
  int k = 0;
  for (auto it = m.files.begin(); it != m.files.end();) {
    if (k++ % 2 == 0) { CHECK_STATUS(fs.remove(it->first.c_str()), Status::Ok); it = m.files.erase(it); } else ++it;
  }
  for (int i = 0; i < 700; ++i) {
    char name[80];
    snprintf(name, sizeof name, "/big/again_%d", i);
    m.files[name] = make_data(i % 9, i);
    CHECK_STATUS(write_file(fs, name, m.files[name]), Status::Ok);
  }
  CHECK(compare_tree(fs, m, v.name) == 0);
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());
}

static void test_full_volume(const Variant& v) {
  if (g_verbose) printf("full: %s\n", v.name);
  Variant small = v;
  small.megabytes = 4;
  Bytes image = make_image(small, "", "-N 200");
  Mounted mt;
  CHECK_STATUS(mt.mount(image, 512, true), Status::Ok);
  if (!mt.fs) return;
  IFileSystem& fs = *mt.fs;
  const uint64_t free_before = fs.info().free_bytes;
  Model m;
  Status s = Status::Ok;
  int count = 0;
  for (; count < 100000; ++count) {
    const std::string path = "/f" + std::to_string(count);
    Bytes d = make_data(150000 + count, count);
    s = write_file(fs, path, d);
    if (s != Status::Ok) { fs.remove(path.c_str()); break; }
    m.files[path] = d;
  }
  CHECK(s == Status::NoSpace);
  CHECK(count > 5);
  CHECK(compare_tree(fs, m, v.name) == 0);
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());
  // a failed big write leaves a consistent file, and removing it all frees everything
  for (const auto& f : m.files) CHECK_STATUS(fs.remove(f.first.c_str()), Status::Ok);
  CHECK(fs.info().free_bytes == free_before);
  // out of inodes
  int made = 0;
  for (; made < 1000; ++made) if (write_file(fs, "/i" + std::to_string(made), Bytes()) != Status::Ok) break;
  CHECK(made > 100 && made < 1000);
  CHECK_STATUS(fs.create_directory("/nodirs"), Status::NoSpace);
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());
}

static void test_random(const Variant& v, uint32_t seed, int operations) {
  if (g_verbose) printf("random: %s seed %u\n", v.name, seed);
  Variant big = v;
  big.megabytes = 48;
  Bytes image = make_image(big, "", "-N 6000");
  Mounted mt;
  CHECK_STATUS(mt.mount(image, 512, true), Status::Ok);
  if (!mt.fs) return;
  IFileSystem& fs = *mt.fs;
  Model model;
  uint64_t rng = seed * 2654435761u + 12345;
  auto rnd = [&](uint32_t n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>((rng >> 33) % n); };
  std::vector<std::string> dirs = {""};
  auto name_in = [&](const std::string& dir) { char b[64]; snprintf(b, sizeof b, "%s/n%u", dir.c_str(), rnd(50)); return std::string(b); };
  auto pick_file = [&]() -> std::string {
    if (model.files.empty()) return "";
    auto it = model.files.begin(); std::advance(it, rnd(model.files.size())); return it->first;
  };
  const uint32_t bs = v.block_size;
  auto big_size = [&]() -> uint32_t { return rnd(8) == 0 ? rnd(400000) : rnd(4) == 0 ? rnd(8 * bs) : rnd(2000); };
  for (int op = 0; op < operations; ++op) {
    const uint32_t kind = rnd(100);
    const std::string dir = dirs[rnd(dirs.size())];
    if (kind < 25) {
      const std::string path = name_in(dir);
      if (model.dirs.count(path)) continue;
      Bytes d = make_data(big_size(), op);
      Status s = write_file(fs, path, d);
      if (s != Status::Ok) { printf("FAILED: op %d create %s: %s\n", op, path.c_str(), status_name(s)); ++g_failed; return; }
      model.files[path] = d;
    } else if (kind < 45) {
      const std::string path = pick_file();
      if (path.empty()) continue;
      UniquePointer<IFile> f;
      Status os = fs.open_file(path.c_str(), OpenMode::ReadWrite, &f);
      if (os != Status::Ok) { printf("FAILED: op %d open %s: %s\n", op, path.c_str(), status_name(os)); ++g_failed; return; }
      Bytes& cur = model.files[path];
      const uint64_t at = cur.empty() ? 0 : rnd(cur.size() + 20000);
      Bytes d = make_data(rnd(3) == 0 ? rnd(60000) : rnd(5000), op);
      uint64_t put = 0;
      Status ws = f->write(at, ConstByteView(d.data(), d.size()), &put);
      if (ws != Status::Ok) { printf("FAILED: op %d write %s: %s\n", op, path.c_str(), status_name(ws)); ++g_failed; return; }
      if (!d.empty()) { if (cur.size() < at + d.size()) cur.resize(at + d.size(), 0); memcpy(cur.data() + at, d.data(), d.size()); }
    } else if (kind < 57) {
      const std::string path = pick_file();
      if (path.empty()) continue;
      UniquePointer<IFile> f;
      if (fs.open_file(path.c_str(), OpenMode::ReadWrite, &f) != Status::Ok) { ++g_failed; return; }
      const uint64_t n = big_size();
      if (f->resize(n) != Status::Ok) { printf("FAILED: op %d resize %s\n", op, path.c_str()); ++g_failed; return; }
      model.files[path].resize(n, 0);
    } else if (kind < 72) {
      const std::string path = pick_file();
      if (path.empty()) continue;
      if (fs.remove(path.c_str()) != Status::Ok) { printf("FAILED: op %d remove %s\n", op, path.c_str()); ++g_failed; return; }
      model.files.erase(path);
    } else if (kind < 80) {
      const std::string path = name_in(dir) + "d";
      if (model.files.count(path) || model.dirs.count(path)) continue;
      Status ms = fs.create_directory(path.c_str());
      if (ms != Status::Ok) { printf("FAILED: op %d mkdir %s: %s\n", op, path.c_str(), status_name(ms)); ++g_failed; return; }
      model.dirs.insert(path); dirs.push_back(path);
    } else if (kind < 94) {
      const std::string path = pick_file();
      if (path.empty()) continue;
      const std::string to = name_in(dir) + "r";
      if (model.files.count(to) || model.dirs.count(to)) continue;
      Status r = fs.rename(path.c_str(), to.c_str());
      if (r != Status::Ok) { printf("FAILED: op %d rename %s -> %s: %s\n", op, path.c_str(), to.c_str(), status_name(r)); ++g_failed; return; }
      model.files[to] = model.files[path]; model.files.erase(path);
    } else {
      // a folder (empty ones are removed, others moved into another folder)
      std::string victim;
      for (const std::string& d : model.dirs) {
        bool empty = true;
        for (const auto& f : model.files) if (f.first.compare(0, d.size() + 1, d + "/") == 0) empty = false;
        for (const auto& x : model.dirs) if (x.compare(0, d.size() + 1, d + "/") == 0) empty = false;
        if (empty) { victim = d; break; }
      }
      if (victim.empty()) continue;
      if (rnd(2)) {
        if (fs.remove(victim.c_str()) != Status::Ok) { printf("FAILED: op %d rmdir %s\n", op, victim.c_str()); ++g_failed; return; }
        model.dirs.erase(victim);
        dirs.erase(std::find(dirs.begin(), dirs.end(), victim));
      }
    }
    if (op % 500 == 499) {
      if (compare_tree(fs, model, "random") != 0) { printf("FAILED: mismatch after op %d\n", op); ++g_failed; return; }
    }
  }
  CHECK(compare_tree(fs, model, "random end") == 0);
  CHECK_STATUS(fs.flush(), Status::Ok);
  Bytes out = mt.image();
  CHECK_FSCK(out);
  Mounted again;
  CHECK_STATUS(again.mount(out, 512, false), Status::Ok);
  if (again.fs) CHECK(compare_tree(*again.fs, model, "random remount") == 0);
}

static void test_handles() {
  const Variant v = ext2_variants()[0];
  Bytes image = make_image(v);
  Mounted mt;
  CHECK_STATUS(mt.mount(image, 512, true), Status::Ok);
  if (!mt.fs) return;
  IFileSystem& fs = *mt.fs;
  CHECK_STATUS(fs.create_directory("/d"), Status::Ok);
  CHECK_STATUS(write_file(fs, "/d/x", make_data(3000, 1)), Status::Ok);
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/d/x", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(fs.remove("/d/x"), Status::Busy);
    CHECK_STATUS(fs.rename("/d/x", "/d/y"), Status::Busy);
    UniquePointer<IDirectory> dir;
    CHECK_STATUS(fs.open_directory("/d", &dir), Status::Ok);
    CHECK_STATUS(fs.remove("/d"), Status::Busy);
    // two handles see each other's writes
    UniquePointer<IFile> g;
    CHECK_STATUS(fs.open_file("/d/x", OpenMode::ReadWrite, &g), Status::Ok);
    Bytes patch = make_data(10, 77);
    uint64_t n = 0;
    CHECK_STATUS(g->write(100, ConstByteView(patch.data(), 10), &n), Status::Ok);
    Bytes got(10);
    CHECK_STATUS(f->read(100, ByteView(got.data(), 10), &n), Status::Ok);
    CHECK(got == patch);
    // a read only handle cannot write
    UniquePointer<IFile> r;
    CHECK_STATUS(fs.open_file("/d/x", OpenMode::Read, &r), Status::Ok);
    CHECK_STATUS(r->write(0, ConstByteView(patch.data(), 10), &n), Status::ReadOnly);
    CHECK_STATUS(r->resize(5), Status::ReadOnly);
  }
  CHECK_STATUS(fs.rename("/d/x", "/d/y"), Status::Ok);
  // names
  CHECK_STATUS(fs.create_directory(("/" + std::string(256, 'a')).c_str()), Status::InvalidPath);
  CHECK_STATUS(fs.create_directory(("/" + std::string(255, 'a')).c_str()), Status::Ok);
  CHECK_STATUS(fs.create_directory("/d/.."), Status::InvalidPath);
  CHECK_STATUS(fs.create_directory("/d/y/z"), Status::NotADirectory);
  UniquePointer<IFile> f;
  CHECK_STATUS(fs.open_file("/d", OpenMode::CreateOrOpen, &f), Status::IsADirectory);
  CHECK_STATUS(fs.open_file("/d/new", OpenMode::Read, &f), Status::NotFound);
  CHECK_STATUS(fs.open_file("/d/new", OpenMode::CreateOrOpen, &f), Status::Ok);
  CHECK(f->size() == 0);
  f = UniquePointer<IFile>();
  CHECK_STATUS(fs.open_file("/d/y", OpenMode::CreateTruncate, &f), Status::Ok);
  CHECK(f->size() == 0);
  f = UniquePointer<IFile>();
  // listing after changes, with rewind
  UniquePointer<IDirectory> dir;
  CHECK_STATUS(fs.open_directory("/d", &dir), Status::Ok);
  DirectoryEntry e;
  int count = 0;
  while (dir->next(&e) == Status::Ok) ++count;
  CHECK(count == 2);
  dir->rewind();
  count = 0;
  while (dir->next(&e) == Status::Ok) ++count;
  CHECK(count == 2);
  dir = UniquePointer<IDirectory>();
  CHECK_STATUS(fs.flush(), Status::Ok);
  CHECK_FSCK(mt.image());
  CHECK(mt.alloc.live > 0);  // (the filesystem itself)
}

// What decides whether a volume is writable.
static void test_mount_rules() {
  const Variant v = ext2_variants()[0];
  const Bytes clean = make_image(v);
  auto with = [&](auto&& change) { Bytes b = clean; change(b); return b; };
  auto put16 = [](Bytes& b, size_t o, uint32_t x) { b[o] = x; b[o + 1] = x >> 8; };
  auto put32 = [&](Bytes& b, size_t o, uint32_t x) { put16(b, o, x); put16(b, o + 2, x >> 16); };
  auto get32 = [](const Bytes& b, size_t o) { return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t{b[o + 3]} << 24); };
  {
    Mounted m;
    CHECK_STATUS(m.mount(clean, 512, true), Status::Ok);
    CHECK(!m.fs->info().read_only);
    CHECK(m.fs->info().type_name.view().equals("ext2"));
  }
  {  // a read only device
    Mounted m;
    CHECK_STATUS(m.mount(clean, 512, false), Status::Ok);
    CHECK(m.fs->info().read_only);
  }
  {  // not cleanly unmounted
    Mounted m;
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put16(b, 1024 + 0x3A, 0); }), 512, true), Status::Ok);
    CHECK(m.fs->info().read_only);
    CHECK_STATUS(m.fs->create_directory("/x"), Status::ReadOnly);
  }
  {  // errors recorded
    Mounted m;
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put16(b, 1024 + 0x3A, 3); }), 512, true), Status::Ok);
    CHECK(m.fs->info().read_only);
  }
  {  // an orphan list waiting
    Mounted m;
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put32(b, 1024 + 0xE8, 12); }), 512, true), Status::Ok);
    CHECK(m.fs->info().read_only);
  }
  {  // a feature this code does not know: unreadable if incompat, read only if ro_compat
    Mounted m;
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put32(b, 1024 + 0x60, get32(b, 1024 + 0x60) | 0x100000); }), 512, true), Status::Unsupported);
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put32(b, 1024 + 0x64, get32(b, 1024 + 0x64) | 0x8000); }), 512, true), Status::Ok);
    CHECK(m.fs->info().read_only);
  }
  {  // no magic, a wrong block size, a size larger than the device
    Mounted m;
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put16(b, 1024 + 0x38, 0); }), 512, true), Status::Corrupt);
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put32(b, 1024 + 0x18, 9); }), 512, true), Status::Corrupt);
    CHECK_STATUS(m.mount(with([&](Bytes& b) { put32(b, 1024 + 0x04, get32(b, 1024 + 0x04) * 2); }), 512, true), Status::Corrupt);
    CHECK_STATUS(m.mount(Bytes(3000, 0), 512, true), Status::Corrupt);
  }
  {  // the state flag: cleared while mounted for writing, set again after flush and when destroyed
    Bytes img;
    {
      Mounted m;
      CHECK_STATUS(m.mount(clean, 512, true), Status::Ok);
      CHECK_STATUS(m.fs->create_directory("/q"), Status::Ok);
      img = m.image();
      CHECK((img[1024 + 0x3A] & 1) == 0);
      CHECK_STATUS(m.fs->flush(), Status::Ok);
      img = m.image();
      CHECK((img[1024 + 0x3A] & 1) == 1);
      CHECK_STATUS(m.fs->create_directory("/q2"), Status::Ok);
      CHECK((m.image()[1024 + 0x3A] & 1) == 0);
      m.fs = UniquePointer<IFileSystem>();
      img = m.image();
      CHECK((img[1024 + 0x3A] & 1) == 1);
    }
    CHECK_FSCK(img);
  }
  {  // a device that fails to write: the error comes back and the volume is not made worse than e2fsck can repair
    Mounted m;
    CHECK_STATUS(m.mount(clean, 512, true), Status::Ok);
    m.disk->fail_after = 3;
    Status s = Status::Ok;
    for (int i = 0; i < 50 && s == Status::Ok; ++i) s = write_file(*m.fs, "/f" + std::to_string(i), make_data(5000, i));
    CHECK(s == Status::IoError);
  }
}

// Damaged images: every outcome is fine except a crash, a hang or a sanitizer report.
static void walk(IFileSystem& fs, const std::string& path, int depth, int* budget) {
  if (depth > 6 || *budget <= 0) return;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path.c_str(), &dir) != Status::Ok) return;
  DirectoryEntry e;
  int guard = 0;
  while (*budget > 0 && ++guard < 4000 && dir->next(&e) == Status::Ok) {
    --*budget;
    const std::string child = (path == "/" ? "" : path) + "/" + str_of(e.name);
    if (e.type == NodeType::Directory) {
      walk(fs, child, depth + 1, budget);
    } else {
      UniquePointer<IFile> f;
      if (fs.open_file(child.c_str(), OpenMode::Read, &f) == Status::Ok) {
        Bytes buf(std::min<uint64_t>(f->size(), 70000));
        uint64_t got = 0;
        if (!buf.empty()) f->read(0, ByteView(buf.data(), buf.size()), &got);
        f->read(f->size() > 100 ? f->size() - 100 : 0, ByteView(buf.data(), std::min<size_t>(buf.size(), 100)), &got);
      }
    }
  }
}

static void test_damage(const Variant& v) {
  const std::string dir = g_work + "/src";
  sh("rm -rf '" + dir + "'; mkdir -p '" + dir + "'");
  Model m = standard_model(v.block_size, false);
  populate(dir, m);
  Bytes clean = make_image(v, dir);
  uint64_t rng = 99;
  auto rnd = [&](uint64_t n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (rng >> 33) % n; };
  // the parts that matter most: superblock, descriptors, bitmaps, inode table, the first blocks of data
  const uint64_t hot = std::min<uint64_t>(clean.size(), 600 * 1024);
  for (int round = 0; round < 120; ++round) {
    Bytes b = clean;
    const int flips = 1 + static_cast<int>(rnd(8));
    for (int i = 0; i < flips; ++i) {
      const uint64_t at = rnd(3) ? 1024 + rnd(1536) : rnd(rnd(2) ? hot : b.size());
      b[at] ^= static_cast<uint8_t>(1u << rnd(8));
    }
    if (round % 10 == 9) b.resize(rnd(b.size()));
    for (int writable = 0; writable < 2; ++writable) {
      Mounted mt;
      if (mt.mount(b, 512, writable != 0) != Status::Ok) continue;
      int budget = 3000;
      walk(*mt.fs, "/", 0, &budget);
      if (writable) {
        write_file(*mt.fs, "/damage_test", make_data(9000, round));
        mt.fs->create_directory("/damage_dir");
        mt.fs->rename("/damage_test", "/damage_dir/moved");
        mt.fs->remove("/dir1/dir2/deep file.txt");
        mt.fs->remove("/many/file_with_a_longish_name_00007.dat");
        mt.fs->flush();
      }
    }
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_verbose = getenv("EXT_VERBOSE") != nullptr;
  if (!have_tool("mke2fs") || !have_tool("e2fsck")) { printf("ExtTest: e2fsprogs not installed: skipped\n"); return 0; }
  char templ[] = "/tmp/exttest.XXXXXX";
  const char* base = getenv("TMPDIR");
  std::string pattern_dir = std::string(base ? base : "/tmp") + "/exttest.XXXXXX";
  std::vector<char> buf(pattern_dir.begin(), pattern_dir.end());
  buf.push_back(0);
  if (!mkdtemp(buf.data())) { perror("mkdtemp"); return 1; }
  (void)templ;
  g_work = buf.data();
  auto want = [&](const char* name) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; ++i) if (strcmp(argv[i], name) == 0) return true;
    return false;
  };
  const std::vector<Variant> variants = ext2_variants();
  if (want("read")) {
    for (size_t i = 0; i < variants.size(); ++i) test_read(variants[i], i == 2 ? 4096 : 512, i == 1 ? 63 : 0);
    test_read_sparse();
  }
  if (want("write")) for (const Variant& v : variants) test_write(v);
  if (want("bigdir")) { test_big_directory(variants[0]); test_big_directory(variants[2]); }
  if (want("full")) for (const Variant& v : variants) test_full_volume(v);
  if (want("random")) for (uint32_t seed = 1; seed <= 6; ++seed) test_random(variants[seed % variants.size()], seed, 1500);
  if (want("handles")) test_handles();
  if (want("mount")) test_mount_rules();
  if (want("damage")) { test_damage(variants[0]); test_damage(variants[2]); }
  sh("rm -rf '" + g_work + "'");
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("ExtTest: all passed\n");
  return 0;
}
