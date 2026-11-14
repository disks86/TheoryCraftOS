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

// Host-side tests for the DwarFS reader (src/FileSystem/Dwarfs) and XXH3 (src/Media/Xxh3): the images of tests/data/dwarfs (from the
// DwarFS project, with the contents its own tests expect), those images re-encoded here (sections stored plain, or compressed again with
// liblzma / liblz4 when tests/run_host_tests.sh finds them), truncated, bit-flipped and with their metadata scrambled (checksums fixed up
// so the parsers see the damage), several threads reading at once, and a DwarFS AppImage through the AppImage launcher.
//
//   DwarfsTest                       the tests
//   DwarfsTest --dump <image> [offset]  lists an image or a DwarFS AppImage (path, size, XXH3 of the contents, link targets)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "Executable/AppImage.hpp"
#include "FileSystem/Dwarfs/DwarfsFileSystem.hpp"
#include "Media/Xxh3.hpp"
#include "Media/Xz.hpp"
#include "Media/Zstd.hpp"
#include "Program/AppImageLaunch.hpp"
#include "Utility/Move.hpp"
#ifdef TCOS_HAVE_XXHASH
#include <xxhash.h>
#endif
#ifdef TCOS_HAVE_LZMA
#include <lzma.h>
#endif
#ifdef TCOS_HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif

using namespace filesystem;

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int> live{0};
  void* allocate(uint64_t size) override { ++live; return malloc(size ? size : 1); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::atomic<int> g_failed{0};
static std::atomic<int> g_checks{0};
#define CHECK(cond) do { ++g_checks; if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { ++g_checks; Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

static std::string sv(StringView v) { return std::string(v.data(), v.size()); }
static ConstByteView view_of(const std::string& s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }

// ------------------------------------------------------------------ files
struct MemoryFile final : IFile {
  std::shared_ptr<const std::string> bytes;
  explicit MemoryFile(std::shared_ptr<const std::string> b) : bytes(std::move(b)) {}
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

static std::string read_host(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string data;
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data.append(chunk, n);
  fclose(f);
  return data;
}
static std::string fixture(const char* name) {
  const std::string data = read_host(std::string("tests/data/dwarfs/") + name);
  if (data.empty()) printf("missing fixture %s\n", name);
  return data;
}

static Status mount(TestAllocator& a, const std::string& bytes, uint64_t offset, UniquePointer<DwarfsFileSystem>* fs) {
  UniquePointer<IFile> file(UniquePointer<MemoryFile>::make(a, std::make_shared<const std::string>(bytes)));
  return DwarfsFileSystem::mount(a, util::move(file), offset, fs);
}

// ------------------------------------------------------------------ listing a mounted image
struct Node {
  char type = 'F';           // F file, D directory, L link, S special
  uint64_t size = 0;
  uint64_t hash = 0;         // XXH3 of a file's contents
  std::string target;        // a link's
  uint64_t modified = 0;
  uint64_t id = 0;
  uint32_t links = 0;
  SpecialNode special = SpecialNode::None;
  bool operator==(const Node& o) const { return type == o.type && size == o.size && hash == o.hash && target == o.target && modified == o.modified && special == o.special; }
};
using Listing = std::map<std::string, Node>;

// While damaged images are read: a file bigger than this is only sampled (a scrambled image may claim gigabytes).
static uint64_t g_read_limit = ~uint64_t{0};

static std::string read_all(IFileSystem& fs, const std::string& path, Status* status) {
  UniquePointer<IFile> file;
  *status = fs.open_file(StringView(path.c_str()), OpenMode::Read, &file);
  if (*status != Status::Ok) return std::string();
  if (file->size() > g_read_limit) {
    std::string piece(65536, '\0');
    uint64_t got = 0;
    *status = file->read(0, ByteView(reinterpret_cast<uint8_t*>(piece.data()), piece.size()), &got);
    if (*status == Status::Ok) *status = file->read(file->size() - piece.size(), ByteView(reinterpret_cast<uint8_t*>(piece.data()), piece.size()), &got);
    if (*status == Status::Ok) *status = Status::Corrupt;
    return std::string();
  }
  std::string data(file->size(), '\0');
  uint64_t got = 0;
  *status = file->read(0, ByteView(reinterpret_cast<uint8_t*>(data.data()), data.size()), &got);
  if (*status == Status::Ok && got != data.size()) *status = Status::IoError;
  return data;
}

// Walks the tree; the first failure ends up in *status (Ok when everything could be read).
static void walk(IFileSystem& fs, TestAllocator& a, const std::string& path, Listing* out, Status* status, int depth = 0) {
  UniquePointer<IDirectory> dir;
  Status s = fs.open_directory(StringView(path.empty() ? "/" : path.c_str()), &dir);
  if (s != Status::Ok) { if (*status == Status::Ok) *status = s; return; }
  DirectoryEntry e;
  while ((s = dir->next(&e)) == Status::Ok) {
    const std::string full = path + "/" + sv(e.name.view());
    Node node;
    node.modified = e.modified;
    node.id = e.file_id;
    node.links = e.link_count;
    node.special = e.special;
    if (e.is_symlink) {
      node.type = 'L';
      String target(a);
      bool is_dir = false;
      const Status r = fs.read_symbolic_link(StringView(full.c_str()), &target, &is_dir);
      if (r != Status::Ok && *status == Status::Ok) *status = r;
      node.target = sv(target.view());
    } else if (e.type == NodeType::Directory) {
      node.type = 'D';
      if (depth < 64) walk(fs, a, full, out, status, depth + 1);
    } else if (e.special != SpecialNode::None) {
      node.type = 'S';
    } else {
      Status r;
      const std::string data = read_all(fs, full, &r);
      if (r != Status::Ok && *status == Status::Ok) *status = r;
      node.size = e.size;
      node.hash = media::xxh3_64(view_of(data));
      if (r == Status::Ok && data.size() != e.size && *status == Status::Ok) *status = Status::Corrupt;
    }
    (*out)[full] = node;
  }
  if (s != Status::EndOfDirectory && *status == Status::Ok) *status = s;
}

static Listing listing(IFileSystem& fs, TestAllocator& a, Status* status) {
  Listing out;
  *status = Status::Ok;
  walk(fs, a, "", &out, status);
  return out;
}

// ------------------------------------------------------------------ re-encoding images
// The sections of an image, decompressed with the decoders under test (zstd) - only for fixtures whose sections those decode.
struct Section {
  uint16_t type = 0, compression = 0;
  uint32_t number = 0;
  std::string data;        // decompressed
};

static uint64_t le(const std::string& s, size_t at, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; ++i) v |= uint64_t(uint8_t(s[at + i])) << (8 * i);
  return v;
}
static void put_le(std::string& s, uint64_t v, int n) {
  for (int i = 0; i < n; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
}

static bool split(TestAllocator& a, const std::string& image, std::vector<Section>* out) {
  out->clear();
  for (size_t at = 0; at < image.size();) {
    if (image.size() - at < 64 || image.compare(at, 6, "DWARFS") != 0) return false;
    Section s;
    s.number = uint32_t(le(image, at + 0x30, 4));
    s.type = uint16_t(le(image, at + 0x34, 2));
    s.compression = uint16_t(le(image, at + 0x36, 2));
    const uint64_t length = le(image, at + 0x38, 8);
    if (length > image.size() - at - 64) return false;
    const std::string raw = image.substr(at + 64, length);
    if (s.compression == 0) {
      s.data = raw;
    } else if (s.compression == 2) {
      ByteBuffer plain(a);
      if (!media::zstd_decompress(view_of(raw), &plain, uint64_t(1) << 30)) return false;
      s.data.assign(reinterpret_cast<const char*>(plain.view().data()), plain.size());
    } else {
      return false;
    }
    out->push_back(std::move(s));
    at += 64 + length;
  }
  return true;
}

// How join() stores the BLOCK sections.
enum class Store { Plain, Lzma, LzmaBcj, Lz4, Lz4Hc };

static bool compress(Store store, const std::string& in, uint16_t* compression, std::string* out) {
  switch (store) {
    case Store::Plain: *compression = 0; *out = in; return true;
#ifdef TCOS_HAVE_LZMA
    case Store::Lzma:
    case Store::LzmaBcj: {
      lzma_options_lzma options;
      lzma_lzma_preset(&options, 6);
      lzma_filter filters[3];
      int n = 0;
      if (store == Store::LzmaBcj) filters[n++] = {LZMA_FILTER_X86, nullptr};
      filters[n++] = {LZMA_FILTER_LZMA2, &options};
      filters[n] = {LZMA_VLI_UNKNOWN, nullptr};
      out->resize(lzma_stream_buffer_bound(in.size()));
      size_t written = 0;
      if (lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC64, nullptr, reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                    reinterpret_cast<uint8_t*>(out->data()), &written, out->size()) != LZMA_OK)
        return false;
      out->resize(written);
      *compression = 1;
      return true;
    }
#endif
#ifdef TCOS_HAVE_LZ4
    case Store::Lz4:
    case Store::Lz4Hc: {
      std::string body(size_t(LZ4_compressBound(int(in.size()))), '\0');
      const int n = store == Store::Lz4 ? LZ4_compress_default(in.data(), body.data(), int(in.size()), int(body.size()))
                                        : LZ4_compress_HC(in.data(), body.data(), int(in.size()), int(body.size()), 9);
      if (n <= 0) return false;
      out->clear();
      put_le(*out, in.size(), 4);
      out->append(body, 0, size_t(n));
      *compression = store == Store::Lz4 ? 3 : 4;
      return true;
    }
#endif
    default: return false;
  }
}

static std::string section_bytes(const Section& s, uint16_t compression, const std::string& payload) {
  std::string checked;
  put_le(checked, s.number, 4);
  put_le(checked, s.type, 2);
  put_le(checked, compression, 2);
  put_le(checked, payload.size(), 8);
  checked += payload;
  std::string out = "DWARFS";
  out.push_back(2);
  out.push_back(5);
  out.append(32, '\0');      // SHA-512/256: not checked by the reader
  put_le(out, media::xxh3_64(view_of(checked)), 8);
  return out + checked;
}

// The image again, the blocks stored as `store` says and everything else plain. The section index, if any, is left out (it would point
// at the old offsets; the reader does not need it).
static std::string join(const std::vector<Section>& sections, Store store) {
  std::string out;
  for (const Section& s : sections) {
    if (s.type == 9) continue;
    uint16_t compression = 0;
    std::string payload = s.data;
    if (s.type == 0 && !compress(store, s.data, &compression, &payload)) return std::string();
    out += section_bytes(s, compression, payload);
  }
  return out;
}

// ------------------------------------------------------------------ the expected tree of the compat images (compat_test.cpp)
static uint64_t hex_reversed(const char* text) {
  // compat_test.cpp prints the XXH3 value's bytes in memory (little-endian) order.
  const uint64_t v = strtoull(text, nullptr, 16);
  uint64_t r = 0;
  for (int i = 0; i < 8; ++i) r |= ((v >> (8 * i)) & 0xFF) << (56 - 8 * i);
  return r;
}

static Listing compat_reference() {
  Listing ref;
  auto file = [&](const char* path, uint64_t size, const char* hash) {
    Node n;
    n.type = 'F';
    n.size = size;
    n.hash = hash ? hex_reversed(hash) : media::xxh3_64(ConstByteView());
    ref[path] = n;
  };
  auto dir = [&](const char* path) { ref[path].type = 'D'; };
  auto link = [&](const char* path, const char* target) {
    ref[path].type = 'L';
    ref[path].target = target;
  };
  auto special = [&](const char* path, SpecialNode kind) {
    ref[path].type = 'S';
    ref[path].special = kind;
  };
  file("/bench.sh", 1517, "f36b0707f57e12b1");
  dir("/dev");
  special("/dev/null", SpecialNode::CharDevice);
  special("/dev/zero", SpecialNode::CharDevice);
  dir("/empty");
  dir("/empty/alsoempty");
  const char* dirs[] = {"/foo", "/foo/1", "/foo/1/2", "/foo/1/2/3", "/foo/1/2/3/4", "/foo/1/2/3/4/5", "/foo/1/2/3/4/5/6", "/foo/1/2/3/4/5/6/7",
                        "/foo/1/2/3/4/5/6/7/8", "/foo/1/2/3/4/5/6/7/8/9"};
  for (const char* d : dirs) dir(d);
  const std::pair<const char*, const char*> small[] = {{"a", "189a846373f241e8"}, {"b", "4aa8385890ef195b"}, {"c", "f743b2af3e60e91f"}, {"d", "fa2a5714cb578dab"},
                                                       {"e", "13bc5fbf33dc0141"}, {"f", "ea1aa9ade526f068"}, {"g", "aa6aa985ddff426e"}, {"h", "fdb55c845d7030f0"},
                                                       {"i", "a198f272298349aa"}, {"j", "57094636a0f3b971"}, {"k", "f8d0f1f8278733de"}, {"l", "b54e0c6ffb79df4f"}};
  for (const auto& [name, hash] : small) file((std::string("/foo/1/2/3/4/5/6/7/8/9/") + name).c_str(), 2, hash);
  file("/foo/1/2/3/4/5/6/7/8/9/blubb", 1517, "f36b0707f57e12b1");
  file("/foo/1/2/3/4/5/z", 1517, "f36b0707f57e12b1");
  file("/foo/1/2/3/4/y", 1517, "f36b0707f57e12b1");
  file("/foo/1/2/3/copy.sh", 94, "67b2455044ceab01");
  file("/foo/1/2/3/x", 1517, "f36b0707f57e12b1");
  file("/foo/1/2/xxx.sh", 94, "67b2455044ceab01");
  file("/foo/1/fmt.sh", 94, "67b2455044ceab01");
  link("/foo/bad", "../foo");
  file("/foo/bar", 0, "c294d3380580062d");
  file("/foo/bla.sh", 1517, "f36b0707f57e12b1");
  special("/foo/pipe", SpecialNode::Fifo);
  link("/foobar", "foo/bar");
  file("/format.sh", 94, "67b2455044ceab01");
  file("/perl-exec.sh", 87, "bd546eb6e49fb736");
  file("/test.py", 1012, "48d97f43e4e9758d");
  return ref;
}

// Compares type, size, contents and link targets (not times).
static bool same_tree(const Listing& got, const Listing& want, bool report) {
  bool same = got.size() == want.size();
  if (!same && report) printf("  %zu entries, want %zu\n", got.size(), want.size());
  for (const auto& [path, w] : want) {
    auto it = got.find(path);
    if (it == got.end()) {
      if (report) printf("  missing %s\n", path.c_str());
      same = false;
      continue;
    }
    const Node& g = it->second;
    if (g.type != w.type || g.size != w.size || g.hash != w.hash || g.target != w.target || g.special != w.special) {
      if (report) printf("  differs %s: %c %llu %016llx '%s'\n", path.c_str(), g.type, (unsigned long long)g.size, (unsigned long long)g.hash, g.target.c_str());
      same = false;
    }
  }
  return same;
}

// ------------------------------------------------------------------ tests
static void test_xxh3() {
  printf("== XXH3\n");
  CHECK(media::xxh3_64(ConstByteView()) == 0x2D06800538D394C2ull);      // the reference implementation's value for no input
  CHECK(media::xxh3_64(view_of("a")) == 0xE6C632B61E964E1Full);
  CHECK(media::xxh3_64(view_of("abc")) == 0x78AF5F94892F3950ull);
#ifdef TCOS_HAVE_XXHASH
  // Every length class (0-3, 4-8, 9-16, 17-128, 129-240, long with partial and whole 1024-byte blocks) against libxxhash.
  std::string data(70000, '\0');
  std::mt19937_64 rng(7);
  for (char& c : data) c = char(rng());
  int bad = 0;
  for (size_t n = 0; n <= data.size(); n += n < 3000 ? 1 : 499)
    if (media::xxh3_64(ConstByteView(reinterpret_cast<const uint8_t*>(data.data()), n)) != XXH3_64bits(data.data(), n)) ++bad;
  CHECK(bad == 0);
#else
  printf("  (libxxhash not found: only the fixed values)\n");
#endif
}

static void test_compat() {
  printf("== compat images\n");
  TestAllocator a;
  const Listing ref = compat_reference();
  for (const char* name : {"compat-v0.9.10.dwarfs", "compat-v0.5.6.dwarfs"}) {
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, fixture(name), 0, &fs), Status::Ok);
    if (!fs) continue;
    Status s;
    const Listing got = listing(*fs, a, &s);
    CHECK_STATUS(s, Status::Ok);
    CHECK(same_tree(got, ref, true));
    CHECK(fs->block_size() == 1024);

    // Paths through links, stat of a link, hard-link identity of the shared files, read-only.
    Status r;
    CHECK(read_all(*fs, "/foo/bad/bla.sh", &r).size() == 1517 && r == Status::Ok);
    DirectoryEntry e;
    CHECK_STATUS(fs->stat("/foobar", &e), Status::Ok);
    CHECK(e.is_symlink && e.type == NodeType::File && e.size == 0);
    CHECK_STATUS(fs->stat("/foo/bad", &e), Status::Ok);
    CHECK(e.is_symlink && e.type == NodeType::Directory);
    CHECK_STATUS(fs->stat("/foo/1/2/3/4/5/6/7/8/9", &e), Status::Ok);
    CHECK(e.type == NodeType::Directory && !e.is_symlink);
    CHECK_STATUS(fs->stat("/nothing", &e), Status::NotFound);
    CHECK_STATUS(fs->stat("/bench.sh/x", &e), Status::NotADirectory);
    UniquePointer<IFile> f;
    CHECK_STATUS(fs->open_file("/foo", OpenMode::Read, &f), Status::IsADirectory);
    CHECK_STATUS(fs->open_file("/bench.sh", OpenMode::ReadWrite, &f), Status::ReadOnly);
    CHECK_STATUS(fs->open_file("/new", OpenMode::CreateOrOpen, &f), Status::ReadOnly);
    CHECK_STATUS(fs->remove("/bench.sh"), Status::ReadOnly);
    UniquePointer<IDirectory> d;
    CHECK_STATUS(fs->open_directory("/bench.sh", &d), Status::NotADirectory);
    CHECK_STATUS(fs->open_directory("/foo/bad", &d), Status::Ok);      // (through the link to /foo)
    String target(a);
    bool is_dir = false;
    CHECK_STATUS(fs->read_symbolic_link("/bench.sh", &target, &is_dir), Status::InvalidArgument);
    CHECK(fs->info().read_only && sv(fs->info().type_name.view()) == "dwarfs");

    // Reads at every offset and length of a file that spans chunks.
    const std::string whole = read_all(*fs, "/test.py", &r);
    CHECK(whole.size() == 1012);
    CHECK_STATUS(fs->open_file("/test.py", OpenMode::Read, &f), Status::Ok);
    bool reads_ok = true;
    for (uint64_t at = 0; f && at <= whole.size() + 2; at += 7)
      for (uint64_t n : {uint64_t(1), uint64_t(13), uint64_t(500), uint64_t(5000)}) {
        std::string part(n, '\0');
        uint64_t got = 0;
        if (f->read(at, ByteView(reinterpret_cast<uint8_t*>(part.data()), n), &got) != Status::Ok) reads_ok = false;
        const uint64_t expect = at >= whole.size() ? 0 : std::min<uint64_t>(n, whole.size() - at);
        if (got != expect || part.compare(0, got, whole, std::min<uint64_t>(at, whole.size()), got) != 0) reads_ok = false;
      }
    CHECK(reads_ok);
  }
  // Older formats and unknown features do not mount.
  UniquePointer<DwarfsFileSystem> fs;
  CHECK_STATUS(mount(a, fixture("compat-v0.4.1.dwarfs"), 0, &fs), Status::Unsupported);
  CHECK_STATUS(mount(a, fixture("compat-v0.2.0.dwarfs"), 0, &fs), Status::Unsupported);
  CHECK_STATUS(mount(a, fixture("future-features.dwarfs"), 0, &fs), Status::Unsupported);
  CHECK_STATUS(mount(a, std::string(200, 'x'), 0, &fs), Status::Corrupt);
  CHECK_STATUS(mount(a, std::string(), 0, &fs), Status::Corrupt);
  CHECK(a.live == 0);
}

static void test_other_images() {
  printf("== other images\n");
  TestAllocator a;
  // data.dwarfs and gh307.dwarfs hold the compat tree without devices, the FIFO and /foo/bad, and a tree of Unicode names.
  Listing ref = compat_reference();
  for (const char* path : {"/dev/null", "/dev/zero", "/foo/pipe", "/foo/bad"}) ref.erase(path);
  const std::string unicode = "/unicode/\u6211\u7231\u4f60/\u2600\ufe0f Sun/\u0393\u03b5\u03b9\u03ac \u03c3\u03b1\u03c2/"
                              "\u0645\u0631\u062d\u0628\u064b\u0627/\u26bd\ufe0f/\u041a\u0430\u0440\u0438\u0431\u0441\u044c\u043a\u043e\u0433\u043e";
  for (const char* name : {"data.dwarfs", "gh307.dwarfs"}) {
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, fixture(name), 0, &fs), Status::Ok);
    if (!fs) continue;
    Status s;
    Listing got = listing(*fs, a, &s);
    CHECK_STATUS(s, Status::Ok);
    Listing common;
    for (const auto& [path, node] : got)
      if (path.rfind("/unicode", 0) != 0 && path != "/\u05d9\u05d5\u05e0\u05d9\u05e7\u05d5\u05d3") common[path] = node;
    CHECK(same_tree(common, ref, true));
    CHECK(got.count(unicode) && got.at(unicode).size == 8);
    CHECK(got.count("/\u05d9\u05d5\u05e0\u05d9\u05e7\u05d5\u05d3") && got.at("/\u05d9\u05d5\u05e0\u05d9\u05e7\u05d5\u05d3").target == unicode.substr(1));
    Status r;
    CHECK(read_all(*fs, "/\u05d9\u05d5\u05e0\u05d9\u05e7\u05d5\u05d3", &r).size() == 8 && r == Status::Ok);      // (through the link)
  }

  // Modification times (tools_test.cpp, kFileTimes; a listing shows a link's own time).
  {
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, fixture("timestamps.dwarfs"), 0, &fs), Status::Ok);
    Status s;
    const Listing got = fs ? listing(*fs, a, &s) : Listing();
    const std::pair<const char*, uint64_t> times[] = {
        {"/file_1w2s3lb6", 1640508970}, {"/dir_573stdbu", 1647948747}, {"/dir_573stdbu/file_a45sc57n", 1552064622},
        {"/dir_573stdbu/file_xh7183o5", 1573058623}, {"/dir_573stdbu/link_mpfppenu", 1657987461}, {"/dir_sgy2vnnq", 1635176806},
        {"/dir_sgy2vnnq/file_lmyplgqf", 1718039832}, {"/dir_sgy2vnnq/link_pjcnuj7u", 1541647716}};
    for (const auto& [path, when] : times) {
      auto it = got.find(path);
      CHECK(it != got.end() && it->second.modified == when);
    }
    CHECK(got.size() == 8);
  }

  // Link targets written on Unix and on Windows ('\' read as '/').
  for (const char* name : {"unixlink.dwarfs", "winlink.dwarfs"}) {
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, fixture(name), 0, &fs), Status::Ok);
    if (!fs) continue;
    Status s;
    const Listing got = listing(*fs, a, &s);
    CHECK_STATUS(s, Status::Ok);
    CHECK(got.count("/link.txt") && got.at("/link.txt").target == "subdir/test.txt");
    CHECK(got.count("/dir/link.txt") && got.at("/dir/link.txt").target == "../subdir/test.txt");
    Status r;
    const std::string through = read_all(*fs, "/dir/link.txt", &r);
    CHECK(r == Status::Ok && through == read_all(*fs, "/subdir/test.txt", &r) && !through.empty());
  }

  // Entries without a name are skipped (metadata_test.cpp, handle_empty_named_dir_entries_gh370).
  for (const char* name : {"gh370.dwarfs", "gh370-plain.dwarfs"}) {
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, fixture(name), 0, &fs), Status::Ok);
    if (!fs) continue;
    Status s;
    const Listing got = listing(*fs, a, &s);
    CHECK_STATUS(s, Status::Ok);
    std::set<std::string> paths;
    for (const auto& [path, node] : got) paths.insert(path);
    CHECK((paths == std::set<std::string>{"/bug", "/bug/a", "/bug/a/1", "/bug/a/1/hello", "/bug/a/2", "/bug/b", "/bug/c"}));
  }

  // Many blocks.
  {
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, fixture("catdata.dwarfs"), 0, &fs), Status::Ok);
    if (fs) {
      CHECK(fs->block_count() == 51);
      Status s;
      const Listing got = listing(*fs, a, &s);
      CHECK_STATUS(s, Status::Ok);
      CHECK(got.size() > 100);
    }
  }
  CHECK(a.live == 0);
}

// The images with their blocks stored plain and compressed again: the same trees.
static void test_reencoded() {
  printf("== re-encoded images\n");
  TestAllocator a;
  std::vector<Store> stores = {Store::Plain};
#ifdef TCOS_HAVE_LZMA
  stores.push_back(Store::Lzma);
  stores.push_back(Store::LzmaBcj);
#else
  printf("  (liblzma not found: no LZMA blocks)\n");
#endif
#ifdef TCOS_HAVE_LZ4
  stores.push_back(Store::Lz4);
  stores.push_back(Store::Lz4Hc);
#else
  printf("  (liblz4 not found: no LZ4 blocks)\n");
#endif
  for (const char* name : {"compat-v0.9.10.dwarfs", "catdata.dwarfs", "timestamps.dwarfs", "gh307.dwarfs", "winlink.dwarfs"}) {
    const std::string original = fixture(name);
    UniquePointer<DwarfsFileSystem> fs;
    CHECK_STATUS(mount(a, original, 0, &fs), Status::Ok);
    if (!fs) continue;
    Status s;
    const Listing want = listing(*fs, a, &s);
    fs = UniquePointer<DwarfsFileSystem>();
    std::vector<Section> sections;
    CHECK(split(a, original, &sections));
    for (Store store : stores) {
      const std::string image = join(sections, store);
      CHECK(!image.empty());
      CHECK_STATUS(mount(a, image, 0, &fs), Status::Ok);
      if (!fs) continue;
      const Listing got = listing(*fs, a, &s);
      CHECK_STATUS(s, Status::Ok);
      CHECK(got == want);
      fs = UniquePointer<DwarfsFileSystem>();
    }
    // A compression this reader does not have (Brotli) fails the read, not the mount.
    std::vector<Section> brotli = sections;
    std::string image;
    for (const Section& sec : brotli) {
      if (sec.type == 9) continue;
      image += section_bytes(sec, sec.type == 0 ? 5 : 0, sec.data);
    }
    CHECK_STATUS(mount(a, image, 0, &fs), Status::Ok);
    if (fs && fs->block_count() > 0) {
      const Listing got = listing(*fs, a, &s);
      CHECK(s == Status::Unsupported || got.empty());
    }
    fs = UniquePointer<DwarfsFileSystem>();
  }
  CHECK(a.live == 0);
}

// Mounts and reads everything it can; the point is that nothing reads outside what it should (the sanitizers watch).
static void exercise(TestAllocator& a, const std::string& image) {
  UniquePointer<DwarfsFileSystem> fs;
  if (mount(a, image, 0, &fs) != Status::Ok || !fs) return;
  Status s;
  (void)listing(*fs, a, &s);
  DirectoryEntry e;
  (void)fs->stat("/foo/1/2/3/4/5/6/7/8/9/a", &e);
  (void)fs->stat("/foobar", &e);
}

static void test_damage() {
  printf("== damaged images\n");
  TestAllocator a;
  g_read_limit = uint64_t(1) << 20;
  std::mt19937 rng(11);
  for (const char* name : {"compat-v0.9.10.dwarfs", "gh307.dwarfs", "catdata.dwarfs"}) {
    const std::string original = fixture(name);
    // Truncated: mounts only when cut between sections after the metadata (dropping the index or the history); anything read is checked.
    std::set<size_t> boundaries;
    for (size_t at = 0; at + 64 <= original.size(); at += 64 + le(original, at + 0x38, 8)) boundaries.insert(at);
    bool only_at_boundaries = true;
    const bool big = original.size() > 100000;
    for (size_t n = 0; n < original.size(); n += n < 6000 ? (big ? 7 : 1) : (big ? 1999 : 211)) {
      UniquePointer<DwarfsFileSystem> fs;
      if (mount(a, original.substr(0, n), 0, &fs) == Status::Ok && !boundaries.count(n)) only_at_boundaries = false;
      if (fs) {
        Status s;
        (void)listing(*fs, a, &s);
      }
    }
    CHECK(only_at_boundaries);
    // Bit flips in the stored image: the checksums catch them (or a flip in an unchecked byte changes nothing that matters).
    for (int i = 0; i < (big ? 150 : 1500); ++i) {
      std::string bad = original;
      const size_t at = rng() % bad.size();
      bad[at] = char(uint8_t(bad[at]) ^ (1u << (rng() % 8)));
      exercise(a, bad);
    }
  }

  // Scrambled schema and metadata with the checksums fixed up, so that the Thrift and Frozen2 parsers and the table checks see the damage.
  for (const char* name : {"compat-v0.9.10.dwarfs", "gh307.dwarfs", "gh370-plain.dwarfs", "timestamps.dwarfs", "winlink.dwarfs"}) {
    std::vector<Section> sections;
    CHECK(split(a, fixture(name), &sections));
    for (int i = 0; i < 2500; ++i) {
      std::vector<Section> bad = sections;
      for (Section& s : bad) {
        if ((s.type != 7 && s.type != 8) || s.data.empty()) continue;
        if (rng() % 2) continue;
        const int edits = 1 + rng() % 4;
        for (int k = 0; k < edits; ++k) {
          const size_t at = rng() % s.data.size();
          switch (rng() % 4) {
            case 0: s.data[at] = char(uint8_t(s.data[at]) ^ (1u << (rng() % 8))); break;
            case 1: s.data[at] = char(rng()); break;
            case 2: s.data[at] = char(rng() % 2 ? 0xFF : 0x00); break;
            case 3: s.data.resize(at); break;
          }
          if (s.data.empty()) break;
        }
      }
      exercise(a, join(bad, Store::Plain));
    }
  }
  g_read_limit = ~uint64_t{0};
  CHECK(a.live == 0);
}

// Several threads read the same files at once (in different orders and pieces) through the shared block cache.
static void test_threads() {
  printf("== threads\n");
  TestAllocator a;
  UniquePointer<DwarfsFileSystem> fs;
  CHECK_STATUS(mount(a, fixture("catdata.dwarfs"), 0, &fs), Status::Ok);
  if (!fs) return;
  Status s;
  const Listing files = listing(*fs, a, &s);
  std::vector<std::string> paths;
  for (const auto& [path, node] : files)
    if (node.type == 'F') paths.push_back(path);
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&, t] {
      std::mt19937 rng(100 + t);
      std::vector<std::string> order = paths;
      std::shuffle(order.begin(), order.end(), rng);
      for (int round = 0; round < 3; ++round)
        for (const std::string& path : order) {
          UniquePointer<IFile> file;
          if (fs->open_file(StringView(path.c_str()), OpenMode::Read, &file) != Status::Ok) { ++mismatches; continue; }
          std::string data(file->size(), '\0');
          for (uint64_t at = 0; at < data.size();) {
            const uint64_t n = std::min<uint64_t>(1 + rng() % 70000, data.size() - at);
            uint64_t got = 0;
            if (file->read(at, ByteView(reinterpret_cast<uint8_t*>(data.data()) + at, n), &got) != Status::Ok || got != n) { ++mismatches; break; }
            at += n;
          }
          if (media::xxh3_64(view_of(data)) != files.at(path).hash) ++mismatches;
        }
    });
  for (std::thread& t : threads) t.join();
  CHECK(mismatches == 0);
  fs = UniquePointer<DwarfsFileSystem>();
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ a DwarFS AppImage
// A 64-bit little-endian ELF header marked as a type 2 AppImage, with a section header table of one entry right after it.
static std::string fake_runtime() {
  std::string elf(64, '\0');
  const char ident[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0, 'A', 'I', 2};
  memcpy(elf.data(), ident, sizeof(ident));
  auto put = [&](size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) elf[at + i] = char((v >> (8 * i)) & 0xFF); };
  put(0x10, 2, 2);        // ET_EXEC
  put(0x12, 0x3E, 2);     // x86-64
  put(0x14, 1, 4);
  put(0x28, 64, 8);       // section headers at 64
  put(0x34, 64, 2);
  put(0x3A, 64, 2);       // 64 bytes each
  put(0x3C, 1, 2);        // one (the null section)
  return elf + std::string(64, '\0');
}

struct OneFileSystem final : IFileSystem {      // the AppImage as the only file there is
  TestAllocator& allocator;
  std::shared_ptr<const std::string> bytes;
  OneFileSystem(TestAllocator& a, std::string b) : allocator(a), bytes(std::make_shared<const std::string>(std::move(b))) {}
  FileSystemInfo info() const override { return {}; }
  Status open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) override {
    if (!path.equals("/Desktop/Downloads/Tool.AppImage")) return Status::NotFound;
    if (mode != OpenMode::Read) return Status::ReadOnly;
    *file = UniquePointer<MemoryFile>::make(allocator, bytes);
    return Status::Ok;
  }
  Status open_directory(StringView, UniquePointer<IDirectory>*) override { return Status::NotFound; }
  Status stat(StringView, DirectoryEntry*) override { return Status::NotFound; }
  Status create_directory(StringView) override { return Status::ReadOnly; }
  Status remove(StringView) override { return Status::ReadOnly; }
  Status rename(StringView, StringView) override { return Status::ReadOnly; }
  Status flush() override { return Status::Ok; }
  Status set_metadata(StringView, const MetadataChange&) override { return Status::ReadOnly; }
  Status create_hard_link(StringView, StringView) override { return Status::ReadOnly; }
  Status create_symbolic_link(StringView, StringView, bool) override { return Status::ReadOnly; }
  Status read_symbolic_link(StringView, String*, bool*) override { return Status::InvalidArgument; }
};

static void test_appimage() {
  printf("== DwarFS AppImage\n");
  TestAllocator a;
  const std::string runtime = fake_runtime(), image = fixture("compat-v0.9.10.dwarfs");
  {
    MemoryFile file(std::make_shared<const std::string>(runtime + image));
    uint64_t offset = 0;
    exe::AppImagePayload payload = exe::AppImagePayload::SquashFs;
    CHECK(exe::appimage_payload_offset(file, &offset, &payload));
    CHECK(offset == runtime.size() && payload == exe::AppImagePayload::DwarFs);
  }
  {
    OneFileSystem files(a, runtime + image);
    UniquePointer<IFileSystem> mounted;
    String message(a);
    CHECK(desktop::open_appimage_image(a, files, "/Desktop/Downloads/Tool.AppImage", &mounted, &message));
    if (mounted) {
      Status s;
      CHECK(same_tree(listing(*mounted, a, &s), compat_reference(), true));
    }
  }
  {
    // A runtime followed by an image of an older format says so.
    OneFileSystem files(a, runtime + fixture("compat-v0.4.1.dwarfs"));
    UniquePointer<IFileSystem> mounted;
    String message(a);
    CHECK(!desktop::open_appimage_image(a, files, "/Desktop/Downloads/Tool.AppImage", &mounted, &message));
    CHECK(sv(message.view()).find("format version") != std::string::npos);
  }
  {
    // Neither SquashFS nor DwarFS after the runtime.
    OneFileSystem files(a, runtime + std::string(100, 'z'));
    UniquePointer<IFileSystem> mounted;
    String message(a);
    CHECK(!desktop::open_appimage_image(a, files, "/Desktop/Downloads/Tool.AppImage", &mounted, &message));
    CHECK(sv(message.view()).find("no SquashFS or DwarFS image") != std::string::npos);
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------ --dump
static int dump(const char* path, const char* offset_text) {
  TestAllocator a;
  const std::string bytes = read_host(path);
  uint64_t offset = offset_text ? strtoull(offset_text, nullptr, 0) : 0;
  if (!offset_text) {
    MemoryFile file(std::make_shared<const std::string>(bytes));
    exe::AppImagePayload payload;
    if (exe::appimage_payload_offset(file, &offset, &payload) && payload != exe::AppImagePayload::DwarFs) {
      fprintf(stderr, "%s: a SquashFS AppImage\n", path);
      return 2;
    }
  }
  UniquePointer<DwarfsFileSystem> fs;
  const Status s = mount(a, bytes, offset, &fs);
  if (s != Status::Ok) { fprintf(stderr, "mount: %s\n", status_name(s)); return 2; }
  fprintf(stderr, "offset %llu, %u blocks of %u bytes, %u inodes\n", (unsigned long long)offset, fs->block_count(), fs->block_size(), fs->inode_count());
  Status walked;
  const Listing got = listing(*fs, a, &walked);
  for (const auto& [p, n] : got) {
    if (n.type == 'L') printf("L %s -> %s\n", p.c_str(), n.target.c_str());
    else if (n.type == 'D') printf("D %s\n", p.c_str());
    else if (n.type == 'S') printf("S %s\n", p.c_str());
    else printf("F %s %llu %016llx\n", p.c_str(), (unsigned long long)n.size, (unsigned long long)n.hash);
  }
  if (walked != Status::Ok) { fprintf(stderr, "walk: %s\n", status_name(walked)); return 1; }
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--dump") return dump(argv[2], argc >= 4 ? argv[3] : nullptr);
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (void (*test)() : {test_xxh3, test_compat, test_other_images, test_reencoded, test_damage, test_threads, test_appimage}) {
    const time_t start = time(nullptr);
    test();
    printf("  (%lld s)\n", (long long)(time(nullptr) - start));
  }
  printf("%d checks, %d failed\n", g_checks.load(), g_failed.load());
  return g_failed ? 1 : 0;
}
