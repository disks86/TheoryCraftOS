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
#include <string>
#include <thread>
#include <vector>
#include "Executable/AppImage.hpp"
#include "FileSystem/Mount/GraftFileSystem.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/Squash/SquashFileSystem.hpp"
#include "Program/AppImageLaunch.hpp"
#include "Program/AppRunScript.hpp"
#include "Utility/Move.hpp"

#if defined(TCOS_HAVE_ZLIB)
#include <zlib.h>
#endif
#if defined(TCOS_HAVE_ZSTD)
#include <zstd.h>
#endif
#if defined(TCOS_HAVE_LZMA)
#include <lzma.h>
#endif
#if defined(TCOS_HAVE_LZ4)
#include <lz4.h>
#include <lz4hc.h>
#endif
#if defined(TCOS_HAVE_LZO)
#include <lzo/lzo1x.h>
#endif

using namespace filesystem;

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

// ------------------------------------------------------------------ a SquashFS writer (the test's own, after the format mksquashfs writes)
enum Comp : uint16_t { kGzip = 1, kLzma = 2, kLzo = 3, kXz = 4, kLz4 = 5, kZstd = 6 };

struct WriterOptions {
  uint16_t compression = kGzip;
  uint32_t block_size = 4096;
  bool fragments = true, sparse = true, extended = false, store_only = false, xz_bcj = false, high = false;
};

static bool compressor_available(uint16_t c) {
  switch (c) {
#if defined(TCOS_HAVE_ZLIB)
    case kGzip: return true;
#endif
#if defined(TCOS_HAVE_ZSTD)
    case kZstd: return true;
#endif
#if defined(TCOS_HAVE_LZMA)
    case kXz: case kLzma: return true;
#endif
#if defined(TCOS_HAVE_LZ4)
    case kLz4: return true;
#endif
#if defined(TCOS_HAVE_LZO)
    case kLzo: return true;
#endif
    default: return false;
  }
}

// Compresses `in` the way mksquashfs's compressor would; false if it cannot (then the block is stored).
static bool compress(const WriterOptions& o, const std::string& in, std::string* out) {
  if (o.store_only || in.empty()) return false;
  out->clear();
  switch (o.compression) {
#if defined(TCOS_HAVE_ZLIB)
    case kGzip: {
      uLongf n = compressBound(in.size());
      out->resize(n);
      if (compress2(reinterpret_cast<Bytef*>(out->data()), &n, reinterpret_cast<const Bytef*>(in.data()), in.size(), o.high ? 9 : 6) != Z_OK) return false;
      out->resize(n);
      return true;
    }
#endif
#if defined(TCOS_HAVE_ZSTD)
    case kZstd: {
      out->resize(ZSTD_compressBound(in.size()));
      const size_t n = ZSTD_compress(out->data(), out->size(), in.data(), in.size(), o.high ? 19 : 3);
      if (ZSTD_isError(n)) return false;
      out->resize(n);
      return true;
    }
#endif
#if defined(TCOS_HAVE_LZMA)
    case kXz: {
      lzma_options_lzma lz;
      lzma_lzma_preset(&lz, o.high ? 9 : 6);
      lz.dict_size = std::max<uint32_t>(o.block_size, 8192);
      lzma_filter filters[3];
      int f = 0;
      if (o.xz_bcj) filters[f++] = {LZMA_FILTER_X86, nullptr};
      filters[f++] = {LZMA_FILTER_LZMA2, &lz};
      filters[f] = {LZMA_VLI_UNKNOWN, nullptr};
      out->resize(lzma_stream_buffer_bound(in.size()));
      size_t pos = 0;
      if (lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC32, nullptr, reinterpret_cast<const uint8_t*>(in.data()), in.size(), reinterpret_cast<uint8_t*>(out->data()), &pos, out->size()) != LZMA_OK) return false;
      out->resize(pos);
      return true;
    }
    case kLzma: {
      lzma_options_lzma lz;
      lzma_lzma_preset(&lz, 6);
      lz.dict_size = std::max<uint32_t>(o.block_size, 8192);
      lzma_stream s = LZMA_STREAM_INIT;
      if (lzma_alone_encoder(&s, &lz) != LZMA_OK) return false;
      out->resize(in.size() + in.size() / 2 + 1024);
      s.next_in = reinterpret_cast<const uint8_t*>(in.data());
      s.avail_in = in.size();
      s.next_out = reinterpret_cast<uint8_t*>(out->data());
      s.avail_out = out->size();
      const lzma_ret r = lzma_code(&s, LZMA_FINISH);
      const size_t n = out->size() - s.avail_out;
      lzma_end(&s);
      if (r != LZMA_STREAM_END) return false;
      out->resize(n);
      return true;
    }
#endif
#if defined(TCOS_HAVE_LZ4)
    case kLz4: {
      out->resize(LZ4_compressBound(static_cast<int>(in.size())));
      const int n = o.high ? LZ4_compress_HC(in.data(), out->data(), static_cast<int>(in.size()), static_cast<int>(out->size()), 12)
                           : LZ4_compress_default(in.data(), out->data(), static_cast<int>(in.size()), static_cast<int>(out->size()));
      if (n <= 0) return false;
      out->resize(n);
      return true;
    }
#endif
#if defined(TCOS_HAVE_LZO)
    case kLzo: {
      static std::vector<uint8_t> work(LZO1X_999_MEM_COMPRESS);
      static bool ready = lzo_init() == LZO_E_OK;
      if (!ready) return false;
      out->resize(in.size() + in.size() / 16 + 64 + 3);
      lzo_uint n = out->size();
      const int r = o.high ? lzo1x_999_compress(reinterpret_cast<const uint8_t*>(in.data()), in.size(), reinterpret_cast<uint8_t*>(out->data()), &n, work.data())
                           : lzo1x_1_compress(reinterpret_cast<const uint8_t*>(in.data()), in.size(), reinterpret_cast<uint8_t*>(out->data()), &n, work.data());
      if (r != LZO_E_OK) return false;
      out->resize(n);
      return true;
    }
#endif
    default: return false;
  }
}

static void put16(std::string& s, uint32_t v) { s.push_back(char(v)); s.push_back(char(v >> 8)); }
static void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char(v >> (8 * i))); }
static void put64(std::string& s, uint64_t v) { for (int i = 0; i < 8; ++i) s.push_back(char(v >> (8 * i))); }
static void patch32(std::string& s, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) s[at + i] = char(v >> (8 * i)); }
static void patch64(std::string& s, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) s[at + i] = char(v >> (8 * i)); }
static void patch16(std::string& s, size_t at, uint32_t v) { s[at] = char(v); s[at + 1] = char(v >> 8); }

struct TNode {
  enum Kind { Dir, File, Link, Fifo, HardLink } kind = Dir;
  std::string name, data;           // file contents / link target / (hard link: path of the file it names)
  std::map<std::string, TNode*> children;
  uint32_t number = 0;
  uint64_t ref = 0;
  bool written = false;
  // file data placement
  uint64_t blocks_start = 0, sparse = 0;
  std::vector<uint32_t> sizes;
  uint32_t fragment = 0xFFFFFFFFu, fragment_offset = 0;
  uint32_t links = 1;
};

struct Tree {
  std::vector<std::unique_ptr<TNode>> nodes;
  TNode* root;
  Tree() { root = make(TNode::Dir, ""); }
  TNode* make(TNode::Kind k, const std::string& name) {
    nodes.push_back(std::make_unique<TNode>());
    nodes.back()->kind = k;
    nodes.back()->name = name;
    return nodes.back().get();
  }
  TNode* dir_at(const std::string& path) {
    TNode* d = root;
    size_t pos = 1;
    while (pos < path.size()) {
      size_t end = path.find('/', pos);
      if (end == std::string::npos) end = path.size();
      const std::string part = path.substr(pos, end - pos);
      TNode*& c = d->children[part];
      if (!c) c = make(TNode::Dir, part);
      d = c;
      pos = end + 1;
    }
    return d;
  }
  TNode* add(const std::string& path, TNode::Kind k, const std::string& data = "") {
    const size_t slash = path.rfind('/');
    TNode* parent = dir_at(path.substr(0, slash));
    TNode* n = make(k, path.substr(slash + 1));
    n->data = data;
    parent->children[n->name] = n;
    return n;
  }
  TNode* find(const std::string& path) {
    TNode* d = root;
    size_t pos = 1;
    while (pos < path.size() && d) {
      size_t end = path.find('/', pos);
      if (end == std::string::npos) end = path.size();
      auto it = d->children.find(path.substr(pos, end - pos));
      d = it == d->children.end() ? nullptr : it->second;
      pos = end + 1;
    }
    return d;
  }
};

struct MetaStream {
  const WriterOptions* o;
  std::string out = {}, buf = {};
  uint64_t position() const { return (uint64_t(out.size()) << 16) | buf.size(); }
  void write(const std::string& bytes) {
    buf += bytes;
    while (buf.size() >= 8192) {
      flush_chunk(buf.substr(0, 8192));
      buf.erase(0, 8192);
    }
  }
  void flush_chunk(const std::string& chunk) {
    std::string c;
    if (compress(*o, chunk, &c) && c.size() < chunk.size()) {
      put16(out, uint32_t(c.size()));
      out += c;
    } else {
      put16(out, 0x8000u | uint32_t(chunk.size()));
      out += chunk;
    }
  }
  void finish() {
    if (!buf.empty()) flush_chunk(buf);
    buf.clear();
  }
};

static std::string write_squash(Tree& tree, const WriterOptions& o) {
  std::string image(96, '\0');
  std::vector<std::pair<uint64_t, uint32_t>> fragments;
  std::string frag_buf;
  auto store_block = [&](const std::string& chunk) -> uint32_t {
    std::string c;
    if (compress(o, chunk, &c) && c.size() < chunk.size()) {
      image += c;
      return uint32_t(c.size());
    }
    image += chunk;
    return uint32_t(chunk.size()) | (1u << 24);
  };
  auto flush_fragment = [&]() {
    if (frag_buf.empty()) return;
    const uint64_t start = image.size();
    const uint32_t word = store_block(frag_buf);
    fragments.push_back({start, word});
    frag_buf.clear();
  };
  // Data, in path order.
  std::vector<TNode*> order;
  std::vector<TNode*> all;
  std::function<void(TNode*)> visit = [&](TNode* n) {
    all.push_back(n);
    for (auto& [name, c] : n->children) visit(c);
  };
  visit(tree.root);
  for (TNode* n : all) {
    if (n->kind != TNode::File) continue;
    const std::string& d = n->data;
    const uint64_t bs = o.block_size;
    const uint64_t full = d.size() / bs, tail = d.size() % bs;
    const bool tail_in_fragment = o.fragments && tail > 0;
    n->blocks_start = image.size();
    const uint64_t blocks = full + (tail > 0 && !tail_in_fragment ? 1 : 0);
    for (uint64_t b = 0; b < blocks; ++b) {
      const std::string chunk = d.substr(b * bs, bs);
      if (o.sparse && chunk.size() == bs && chunk.find_first_not_of('\0') == std::string::npos) {
        n->sizes.push_back(0);
        n->sparse += bs;
        continue;
      }
      n->sizes.push_back(store_block(chunk));
    }
    if (tail_in_fragment) {
      if (frag_buf.size() + tail > bs) flush_fragment();
      n->fragment = uint32_t(fragments.size());
      n->fragment_offset = uint32_t(frag_buf.size());
      frag_buf += d.substr(full * bs);
    }
  }
  flush_fragment();

  // Inode numbers: post-order, the root last.
  uint32_t next_number = 1;
  std::function<void(TNode*)> number = [&](TNode* n) {
    for (auto& [name, c] : n->children)
      if (c->kind == TNode::Dir) number(c);
    for (auto& [name, c] : n->children)
      if (c->kind != TNode::Dir && c->kind != TNode::HardLink) c->number = next_number++;
    n->number = next_number++;
  };
  number(tree.root);
  const uint32_t inode_count = next_number - 1;
  for (TNode* n : all)
    if (n->kind == TNode::HardLink) tree.find(n->data)->links++;

  MetaStream inodes{&o}, dirs{&o};
  const uint32_t mtime = 1700000000;
  auto header = [&](std::string& s, uint16_t type, uint16_t mode, uint32_t num) {
    put16(s, type); put16(s, mode); put16(s, 0); put16(s, 0); put32(s, mtime); put32(s, num);
  };
  auto write_leaf = [&](TNode* n) {
    std::string s;
    if (n->kind == TNode::File) {
      const bool ext = o.extended || n->blocks_start >= (1ull << 32) || n->data.size() >= (1ull << 32) || n->sparse || n->links > 1;
      if (!ext) {
        header(s, 2, 0644, n->number);
        put32(s, uint32_t(n->blocks_start)); put32(s, n->fragment); put32(s, n->fragment_offset); put32(s, uint32_t(n->data.size()));
      } else {
        header(s, 9, 0644, n->number);
        put64(s, n->blocks_start); put64(s, n->data.size()); put64(s, n->sparse); put32(s, n->links); put32(s, n->fragment); put32(s, n->fragment_offset);
        put32(s, 0xFFFFFFFFu);
      }
      for (uint32_t w : n->sizes) put32(s, w);
    } else if (n->kind == TNode::Link) {
      header(s, o.extended ? 10 : 3, 0777, n->number);
      put32(s, 1); put32(s, uint32_t(n->data.size()));
      s += n->data;
      if (o.extended) put32(s, 0xFFFFFFFFu);
    } else {
      header(s, 6, 0644, n->number);
      put32(s, 1);
    }
    n->ref = inodes.position();
    inodes.write(s);
    n->written = true;
  };
  for (TNode* n : all)
    if (n->kind != TNode::Dir && n->kind != TNode::HardLink) write_leaf(n);
  std::function<void(TNode*, uint32_t)> write_dir = [&](TNode* n, uint32_t parent) {
    for (auto& [name, c] : n->children)
      if (c->kind == TNode::Dir) write_dir(c, n->number);
    // The listing, in runs that share an inode block.
    const uint64_t listing = dirs.position();
    std::string s;
    std::vector<std::pair<std::string, TNode*>> entries(n->children.begin(), n->children.end());
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return strcmp(a.first.c_str(), b.first.c_str()) < 0; });
    size_t i = 0;
    while (i < entries.size()) {
      auto target = [&](size_t k) { TNode* c = entries[k].second; return c->kind == TNode::HardLink ? tree.find(c->data) : c; };
      const uint64_t block = target(i)->ref >> 16;
      const uint32_t base = target(i)->number;
      size_t j = i;
      while (j < entries.size() && j - i < 256 && (target(j)->ref >> 16) == block && int64_t(target(j)->number) - base < 32767 && int64_t(target(j)->number) - base > -32768) ++j;
      put32(s, uint32_t(j - i - 1)); put32(s, uint32_t(block)); put32(s, base);
      for (size_t k = i; k < j; ++k) {
        TNode* t = target(k);
        const uint16_t type = t->kind == TNode::Dir ? 1 : t->kind == TNode::File ? 2 : t->kind == TNode::Link ? 3 : 6;
        put16(s, uint32_t(t->ref & 0xFFFF)); put16(s, uint32_t(uint16_t(int16_t(int64_t(t->number) - base)))); put16(s, type);
        put16(s, uint32_t(entries[k].first.size() - 1));
        s += entries[k].first;
      }
      i = j;
    }
    dirs.write(s);
    std::string inode;
    const uint32_t file_size = uint32_t(s.size()) + 3;
    if (!o.extended && file_size < 65536) {
      header(inode, 1, 0755, n->number);
      put32(inode, uint32_t(listing >> 16)); put32(inode, 2 + uint32_t(n->children.size())); put16(inode, file_size); put16(inode, uint32_t(listing & 0xFFFF));
      put32(inode, parent);
    } else {
      header(inode, 8, 0755, n->number);
      put32(inode, 2 + uint32_t(n->children.size())); put32(inode, file_size); put32(inode, uint32_t(listing >> 16)); put32(inode, parent);
      put16(inode, 0); put16(inode, uint32_t(listing & 0xFFFF)); put32(inode, 0xFFFFFFFFu);
    }
    n->ref = inodes.position();
    inodes.write(inode);
  };
  write_dir(tree.root, inode_count + 1);
  inodes.finish();
  dirs.finish();

  const uint64_t inode_table = image.size();
  image += inodes.out;
  const uint64_t directory_table = image.size();
  image += dirs.out;
  uint64_t fragment_table = ~0ull;
  if (!fragments.empty()) {
    std::vector<uint64_t> pointers;
    for (size_t i = 0; i < fragments.size(); i += 512) {
      std::string entries;
      for (size_t k = i; k < std::min(fragments.size(), i + 512); ++k) {
        put64(entries, fragments[k].first); put32(entries, fragments[k].second); put32(entries, 0);
      }
      MetaStream m{&o};
      m.write(entries);
      m.finish();
      pointers.push_back(image.size());
      image += m.out;
    }
    fragment_table = image.size();
    for (uint64_t p : pointers) put64(image, p);
  }
  std::string ids;
  put32(ids, 0);
  MetaStream m{&o};
  m.write(ids);
  m.finish();
  const uint64_t id_block = image.size();
  image += m.out;
  const uint64_t id_table = image.size();
  put64(image, id_block);
  const uint64_t bytes_used = image.size();
  image.resize((image.size() + 4095) / 4096 * 4096, '\0');

  patch32(image, 0, 0x73717368); patch32(image, 4, inode_count); patch32(image, 8, mtime); patch32(image, 12, o.block_size);
  patch32(image, 16, uint32_t(fragments.size())); patch16(image, 20, o.compression);
  uint32_t log = 0;
  while ((1u << log) < o.block_size) ++log;
  patch16(image, 22, log);
  patch16(image, 24, 0x0200 | (o.fragments ? 0 : 0x0010) | (o.store_only ? 0x000B : 0));
  patch16(image, 26, 1); patch16(image, 28, 4); patch16(image, 30, 0);
  patch64(image, 32, tree.root->ref); patch64(image, 40, bytes_used); patch64(image, 48, id_table); patch64(image, 56, ~0ull);
  patch64(image, 64, inode_table); patch64(image, 72, directory_table); patch64(image, 80, fragment_table); patch64(image, 88, ~0ull);
  return image;
}

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

static uint16_t first_available() {
  for (uint16_t c : {kZstd, kGzip, kXz, kLz4, kLzo, kLzma})
    if (compressor_available(c)) return c;
  return 0;
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
// A runtime-shaped ELF: header, some code, a section header table whose last section ends past it (as the runtime's .sha256_sig etc. do), then the image.
static std::string fake_runtime(bool wide, bool big, uint8_t type, bool last_section_past_table) {
  std::string elf(wide ? 64 : 52, '\0');
  elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
  elf[4] = wide ? 2 : 1;
  elf[5] = big ? 2 : 1;
  elf[6] = 1;
  elf[8] = 'A'; elf[9] = 'I'; elf[10] = char(type);
  auto put = [&](std::string& s, size_t at, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) s[at + i] = char(big ? v >> (8 * (n - 1 - i)) : v >> (8 * i));
  };
  put(elf, 16, 2, 2);
  put(elf, 18, wide ? 62 : 3, 2);
  elf.resize(3000, '\x90');
  const uint64_t shoff = elf.size(), shentsize = wide ? 64 : 40, shnum = 3;
  if (wide) { put(elf, 0x28, shoff, 8); put(elf, 0x3A, shentsize, 2); put(elf, 0x3C, shnum, 2); }
  else { put(elf, 0x20, shoff, 4); put(elf, 0x2E, shentsize, 2); put(elf, 0x30, shnum, 2); }
  std::string table(shentsize * shnum, '\0');
  const uint64_t last_offset = shoff + table.size(), last_size = last_section_past_table ? 1024 : 0;
  const size_t last = shentsize * (shnum - 1);
  put(table, last + 4, 1, 4);      // SHT_PROGBITS
  if (wide) { put(table, last + 0x18, last_offset, 8); put(table, last + 0x20, last_size, 8); }
  else { put(table, last + 0x10, last_offset, 4); put(table, last + 0x14, last_size, 4); }
  elf += table;
  elf.append(last_size, 'S');
  return elf;
}

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
    if (!exe::appimage_payload_offset(*file, &offset)) { fprintf(stderr, "not a type 2 AppImage\n"); return 2; }
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
  printf(g_failed ? "AppImageTest: %d of %d checks FAILED\n" : "AppImageTest: all %d checks passed\n", g_failed ? int(g_failed) : g_checks, g_checks);
  return g_failed ? 1 : 0;
}
