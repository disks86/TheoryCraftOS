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

// Host-test helpers shared by AppImageTest and AppManTest: a SquashFS writer after the format mksquashfs writes (compressing with the reference
// libraries the build found: TCOS_HAVE_ZLIB, _ZSTD, _LZMA, _LZ4, _LZO), and a stand-in AppImage runtime (an ELF with the "AI" mark and a section
// table) to put in front of an image.
#pragma once
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
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


namespace squash_writer {

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

static uint16_t first_available() {
  for (uint16_t c : {kZstd, kGzip, kXz, kLz4, kLzo, kLzma})
    if (compressor_available(c)) return c;
  return 0;
}


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

}  // namespace squash_writer
