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

// Host-side tests for BefsFileSystem against BeFS images built here (1 and 2 KiB blocks, small allocation groups, directory trees with
// several levels, files in direct, indirect and double indirect runs), on devices of 512 and 2048 byte blocks, plus truncated and
// bit-flipped images.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "FileSystem/Befs/BefsFileSystem.hpp"

using namespace filesystem;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

struct MemDisk final : hardware::IStorageDevice {
  MemDisk(const uint8_t* image, uint64_t length, uint32_t block, uint64_t lead_blocks = 0, bool writable = false) : block_size(block), can_write(writable) {
    blocks = lead_blocks + (length + block - 1) / block;
    data = static_cast<uint8_t*>(calloc(blocks, block));
    memcpy(data + lead_blocks * block, image, length);
  }
  ~MemDisk() { free(data); }
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
    ++writes;
    memcpy(data + lba * block_size, buffer.data(), buffer.size());
    return true;
  }
  bool flush() override { return true; }
  uint8_t* data; uint64_t blocks; uint32_t block_size; bool can_write; uint64_t writes = 0;
};

static void put16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put64(uint8_t* p, uint64_t v) { put32(p, static_cast<uint32_t>(v)); put32(p + 4, static_cast<uint32_t>(v >> 32)); }

static uint8_t pattern(uint64_t i, uint32_t seed) { return static_cast<uint8_t>(i * 7 + seed + (i >> 8)); }
static std::vector<uint8_t> make_data(uint64_t size, uint32_t seed) {
  std::vector<uint8_t> v(size);
  for (uint64_t i = 0; i < size; ++i) v[i] = pattern(i, seed);
  return v;
}

// Lays out a BeFS volume the way mkbfs would: super block at byte 512, inodes of one block, B+tree directories, block runs per group.
struct BefsImage {
  typedef std::pair<std::string, uint64_t> Entry;  // name, inode block

  BefsImage(uint32_t block_size, uint32_t group_shift, uint32_t groups, uint32_t node_size)
      : bs(block_size), ag_shift(group_shift), ag_count(groups), ns(node_size) {
    for (shift = 0; (1u << shift) < bs; ++shift) {}
    total = uint64_t{groups} << group_shift;
    img.assign(total * bs, 0);
    per_group = ((uint64_t{1} << group_shift) + bs * 8 - 1) / (bs * 8);
    used.assign(total, false);
    next = 1 + groups * per_group;  // the super block and the bitmap
    for (uint64_t b = 0; b < next; ++b) used[b] = true;
  }

  uint8_t* at(uint64_t block) { return img.data() + block * bs; }
  uint64_t alloc(uint64_t n) {
    if (((next + n - 1) >> ag_shift) != (next >> ag_shift)) next = ((next >> ag_shift) + 1) << ag_shift;
    const uint64_t b = next;
    next += n;
    for (uint64_t i = 0; i < n; ++i) used[b + i] = true;
    return b;
  }
  void put_run(uint8_t* p, uint64_t block, uint32_t length) {
    put32(p, static_cast<uint32_t>(block >> ag_shift));
    put16(p + 4, block & ((uint64_t{1} << ag_shift) - 1));
    put16(p + 6, length);
  }

  // An inode whose data (`size` bytes) is one run (scatter false) or one run per block in reverse order (scatter true).
  uint64_t inode(uint32_t mode, const uint8_t* data, uint64_t size, bool scatter) {
    const uint64_t blocks = (size + bs - 1) / bs;
    std::vector<std::pair<uint64_t, uint32_t>> runs;  // block, length
    std::vector<uint64_t> first(blocks);
    if (!scatter && blocks) {
      const uint64_t b = alloc(blocks);
      runs.push_back({b, static_cast<uint32_t>(blocks)});
      for (uint64_t i = 0; i < blocks; ++i) first[i] = b + i;
    } else {
      for (uint64_t i = blocks; i-- > 0;) first[i] = alloc(1);
      for (uint64_t i = 0; i < blocks; ++i) runs.push_back({first[i], 1});
    }
    for (uint64_t i = 0; i < blocks; ++i) {
      const uint64_t n = size - i * bs < bs ? size - i * bs : bs;
      memcpy(at(first[i]), data + i * bs, n);
    }
    return inode_of_runs(mode, runs, size);
  }
  // An inode over runs that exist already.
  uint64_t inode_of_runs(uint32_t mode, const std::vector<std::pair<uint64_t, uint32_t>>& runs, uint64_t size) {
    const uint64_t me = alloc(1);
    uint8_t* p = at(me);
    meta.push_back(me);
    inodes.push_back(me);
    put32(p, 0x3bbe0ad9);
    put_run(p + 4, me, 1);
    put32(p + 20, mode);
    put32(p + 24, 1);
    put32(p + 64, bs);
    uint8_t* stream = p + 72;
    const uint32_t per = bs / 8;
    uint64_t direct_bytes = 0, indirect_bytes = 0, double_bytes = 0;
    size_t used_runs = 0;
    for (; used_runs < runs.size() && used_runs < 12; ++used_runs) {
      put_run(stream + used_runs * 8, runs[used_runs].first, runs[used_runs].second);
      direct_bytes += uint64_t{runs[used_runs].second} * bs;
    }
    if (used_runs < runs.size()) {  // indirect: one block of runs
      const uint64_t a = alloc(1);
      meta.push_back(a);
      for (uint32_t i = 0; i < per && used_runs < runs.size(); ++i, ++used_runs) {
        put_run(at(a) + i * 8, runs[used_runs].first, runs[used_runs].second);
        indirect_bytes += uint64_t{runs[used_runs].second} * bs;
      }
      put_run(stream + 96 + 8, a, 1);
    }
    if (used_runs < runs.size()) {  // double indirect: a block of runs of blocks of runs
      const uint64_t top = alloc(1);
      meta.push_back(top);
      for (uint32_t t = 0; t < per && used_runs < runs.size(); ++t) {
        const uint64_t a = alloc(1);
        meta.push_back(a);
        put_run(at(top) + t * 8, a, 1);
        for (uint32_t i = 0; i < per && used_runs < runs.size(); ++i, ++used_runs) {
          put_run(at(a) + i * 8, runs[used_runs].first, runs[used_runs].second);
          double_bytes += uint64_t{runs[used_runs].second} * bs;
        }
      }
      put_run(stream + 96 + 8 + 8 + 8, top, 1);
    }
    CHECK(used_runs == runs.size());
    put64(stream + 96, direct_bytes);
    if (indirect_bytes) put64(stream + 112, direct_bytes + indirect_bytes);
    if (double_bytes) put64(stream + 128, direct_bytes + indirect_bytes + double_bytes);
    put64(stream + 136, size);
    return me;
  }
  // A file made of 4-block runs with 4 free blocks between them, `count` runs: the free space left in the middle is in pieces of 4.
  uint64_t checkerboard(uint32_t count) {
    const uint64_t start = alloc(uint64_t{count} * 8);
    std::vector<std::pair<uint64_t, uint32_t>> runs;
    for (uint32_t r = 0; r < count; ++r) {
      runs.push_back({start + r * 8, 4});
      for (uint32_t i = 4; i < 8; ++i) used[start + r * 8 + i] = false;
    }
    return inode_of_runs(0x81A4, runs, uint64_t{count} * 4 * bs);
  }
  uint64_t file(const std::vector<uint8_t>& d, bool scatter = false, uint32_t mode = 0x81A4) { return inode(mode, d.data(), d.size(), scatter); }
  uint64_t text(const char* s) { return inode(0x81A4, reinterpret_cast<const uint8_t*>(s), strlen(s), false); }
  uint64_t symlink(const char* target) {
    const uint64_t me = alloc(1);
    uint8_t* p = at(me);
    meta.push_back(me);
    inodes.push_back(me);
    put32(p, 0x3bbe0ad9);
    put_run(p + 4, me, 1);
    put32(p + 20, 0xA1FF);
    put32(p + 24, 1);
    memcpy(p + 72, target, strlen(target));
    return me;
  }

  // A directory: its entries in a B+tree of nodes holding at most four keys, in as many levels as that takes.
  uint64_t directory(std::vector<Entry> entries) {
    std::sort(entries.begin(), entries.end());
    std::vector<uint8_t> tree(ns, 0);
    struct Link { uint64_t offset; std::string max_key; };
    std::vector<Link> level;
    const size_t cap = 4;
    size_t leaves = entries.empty() ? 1 : (entries.size() + cap - 1) / cap;
    for (size_t l = 0; l < leaves; ++l) {
      std::vector<Entry> part;
      for (size_t i = l * cap; i < entries.size() && i < (l + 1) * cap; ++i) part.push_back(entries[i]);
      const uint64_t offset = tree.size();
      const uint64_t left = l ? offset - ns : ~uint64_t{0};
      const uint64_t right = l + 1 < leaves ? offset + ns : ~uint64_t{0};
      add_node(&tree, part, ~uint64_t{0}, left, right);
      level.push_back({offset, part.empty() ? std::string() : part.back().first});
    }
    uint32_t levels = 1;
    while (level.size() > 1) {
      std::vector<Link> upper;
      for (size_t i = 0; i < level.size(); i += cap) {
        const size_t n = level.size() - i < cap ? level.size() - i : cap;
        std::vector<Entry> keys;
        for (size_t k = 0; k + 1 < n; ++k) keys.push_back({level[i + k].max_key, level[i + k].offset});
        const uint64_t offset = tree.size();
        add_node(&tree, keys, level[i + n - 1].offset, ~uint64_t{0}, ~uint64_t{0});
        upper.push_back({offset, level[i + n - 1].max_key});
      }
      level = upper;
      ++levels;
    }
    put32(tree.data(), 0x69f6c2e8);
    put32(tree.data() + 4, ns);
    put32(tree.data() + 8, levels);
    put64(tree.data() + 16, level[0].offset);
    put64(tree.data() + 24, ~uint64_t{0});
    put64(tree.data() + 32, tree.size());
    return inode(0x41ED, tree.data(), tree.size(), false);
  }
  void add_node(std::vector<uint8_t>* tree, const std::vector<Entry>& keys, uint64_t overflow, uint64_t left, uint64_t right) {
    const size_t base = tree->size();
    tree->resize(base + ns, 0);
    uint8_t* n = tree->data() + base;
    put64(n, left);
    put64(n + 8, right);
    put64(n + 16, overflow);
    uint32_t key_bytes = 0;
    for (const Entry& e : keys) key_bytes += e.first.size();
    put16(n + 24, keys.size());
    put16(n + 26, key_bytes);
    const uint32_t ends = (28 + key_bytes + 7) & ~7u;
    uint32_t at_key = 28, end = 0, i = 0;
    for (const Entry& e : keys) {
      memcpy(n + at_key, e.first.data(), e.first.size());
      at_key += e.first.size();
      end += e.first.size();
      put16(n + ends + i * 2, end);
      put64(n + ends + keys.size() * 2 + i * 8, e.second);
      ++i;
    }
    CHECK(ends + keys.size() * 10 <= ns);
  }

  void finish(uint64_t root_inode, const char* label) {
    uint8_t* s = img.data() + 512;
    strncpy(reinterpret_cast<char*>(s), label, 31);
    put32(s + 32, 0x42465331);
    put32(s + 36, 0x42494745);
    put32(s + 40, bs);
    put32(s + 44, shift);
    put64(s + 48, total);
    uint64_t used_count = 0;
    for (uint64_t b = 0; b < total; ++b) {
      if (!used[b]) continue;
      ++used_count;
      const uint64_t group = b >> ag_shift, bit = b & ((uint64_t{1} << ag_shift) - 1);
      at(1 + group * per_group + bit / (bs * 8))[(bit % (bs * 8)) / 8] |= 1u << (bit % 8);
    }
    put64(s + 56, used_count);
    put32(s + 64, bs);
    put32(s + 68, 0xdd121031);
    put32(s + 72, per_group);
    put32(s + 76, ag_shift);
    put32(s + 80, ag_count);
    put32(s + 84, 0x434c454e);
    put32(s + 112, 0x15b6830e);
    put_run(s + 116, root_inode, 1);
    meta.push_back(0);
    used_blocks = used_count;
  }

  uint32_t bs, shift = 0, ag_shift, ag_count, ns;
  uint64_t total, next, used_blocks = 0, per_group = 0;
  std::vector<bool> used;
  std::vector<uint64_t> inodes;
  std::vector<uint8_t> img;
  std::vector<uint64_t> meta;  // inode and run-array blocks, plus block 0
};

static std::string list(IFileSystem& fs, const char* path) {
  std::string out;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path, &dir) != Status::Ok) return "!open";
  DirectoryEntry e;
  while (dir->next(&e) == Status::Ok) {
    if (!out.empty()) out += ",";
    out += e.name.c_str();
    if (e.type == NodeType::Directory) out += "/";
  }
  return out;
}

static bool read_back(IFileSystem& fs, const char* path, std::vector<uint8_t>* out) {
  UniquePointer<IFile> f;
  if (fs.open_file(path, OpenMode::Read, &f) != Status::Ok) return false;
  out->assign(f->size(), 0);
  uint64_t got = 0;
  if (out->empty()) return true;
  return f->read(0, ByteView(out->data(), out->size()), &got) == Status::Ok && got == out->size();
}

struct Built {
  BefsImage image;
  std::vector<uint8_t> frag, big, contig;
  Built(uint32_t bs, uint32_t ag_shift, uint32_t groups, uint32_t ns, uint32_t checker = 0) : image(bs, ag_shift, groups, ns) {
    BefsImage& b = image;
    std::vector<BefsImage::Entry> root, sub, many;
    root.push_back({"hello.txt", b.text("Hello, BeFS!\n")});
    root.push_back({"empty", b.file({})});
    frag = make_data(14 * bs - 7, 1);
    root.push_back({"frag", b.file(frag, true)});
    big = make_data((12 + bs / 8 + 5) * uint64_t{bs} - 100, 2);
    root.push_back({"big", b.file(big, true)});
    contig = make_data(50 * uint64_t{bs} + 123, 3);
    root.push_back({"contig", b.file(contig)});
    root.push_back({"link", b.symlink("hello.txt")});
    root.push_back({"caf\xC3\xA9.txt", b.text("accent")});
    root.push_back({"locked", b.file(make_data(10, 4), false, 0x8124)});
    sub.push_back({"nested.txt", b.text("nested")});
    root.push_back({"sub", b.directory(sub)});
    root.push_back({"emptydir", b.directory({})});
    for (int i = 0; i < 60; ++i) {
      char name[16];
      snprintf(name, sizeof name, "f%03d", i);
      many.push_back({name, b.text(name)});
    }
    root.push_back({"many", b.directory(many)});
    if (checker) root.push_back({"filler", b.checkerboard(checker)});
    b.finish(b.directory(root), "TESTVOL");
  }
};

static void check_volume(IFileSystem& fs, const Built& v, const char* what) {
  printf("%s\n", what);
  const FileSystemInfo info = fs.info();
  CHECK(info.type_name.equals("BeFS") && info.label.equals("TESTVOL") && info.read_only);
  CHECK(info.total_bytes == v.image.total * v.image.bs);

  CHECK(list(fs, "/") == "big,caf\xC3\xA9.txt,contig,empty,emptydir/,frag,hello.txt,link,locked,many/,sub/");
  CHECK(list(fs, "/sub") == "nested.txt");
  CHECK(list(fs, "/emptydir") == "");
  std::string many = list(fs, "/many");
  CHECK(std::count(many.begin(), many.end(), ',') == 59 && many.compare(0, 9, "f000,f001") == 0);
  for (int i = 0; i < 60; ++i) {
    char path[32], name[16];
    snprintf(name, sizeof name, "f%03d", i);
    snprintf(path, sizeof path, "/many/%s", name);
    std::vector<uint8_t> got;
    CHECK(read_back(fs, path, &got) && std::string(got.begin(), got.end()) == name);
  }

  std::vector<uint8_t> got;
  CHECK(read_back(fs, "/hello.txt", &got) && std::string(got.begin(), got.end()) == "Hello, BeFS!\n");
  CHECK(read_back(fs, "hello.txt", &got) && got.size() == 13);
  CHECK(read_back(fs, "/empty", &got) && got.empty());
  CHECK(read_back(fs, "/frag", &got) && got == v.frag);
  CHECK(read_back(fs, "/big", &got) && got == v.big);
  CHECK(read_back(fs, "/contig", &got) && got == v.contig);
  CHECK(read_back(fs, "/link", &got) && std::string(got.begin(), got.end()) == "hello.txt");
  CHECK(read_back(fs, "/caf\xC3\xA9.txt", &got) && std::string(got.begin(), got.end()) == "accent");
  CHECK(read_back(fs, "/sub/nested.txt", &got) && std::string(got.begin(), got.end()) == "nested");
  CHECK(read_back(fs, "//sub/./nested.txt/", &got) && got.size() == 6);

  // Reads that start inside a block, cross runs and the end of the file.
  for (const std::vector<uint8_t>* data : {&v.frag, &v.big, &v.contig}) {
    const char* path = data == &v.frag ? "/frag" : data == &v.big ? "/big" : "/contig";
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file(path, OpenMode::Read, &f), Status::Ok);
    CHECK(f && f->size() == data->size());
    const uint32_t bs = v.image.bs;
    const uint64_t offsets[] = {0, 1, bs - 3, bs, 11 * bs - 5, 12 * bs + 17, 13 * bs + bs / 2, data->size() - 10, data->size() - 1};
    for (uint64_t offset : offsets) {
      for (uint64_t length : {uint64_t{1}, uint64_t{9}, uint64_t{2 * bs + 5}}) {
        std::vector<uint8_t> buffer(length);
        uint64_t transferred = 99;
        CHECK_STATUS(f->read(offset, ByteView(buffer.data(), buffer.size()), &transferred), Status::Ok);
        const uint64_t want = offset >= data->size() ? 0 : std::min<uint64_t>(length, data->size() - offset);
        CHECK(transferred == want);
        CHECK(memcmp(buffer.data(), data->data() + offset, want) == 0);
      }
    }
    uint8_t one[4];
    uint64_t transferred = 99;
    CHECK_STATUS(f->read(data->size(), ByteView(one), &transferred), Status::Ok);
    CHECK(transferred == 0);
    CHECK_STATUS(f->read(data->size() + 1000, ByteView(one), &transferred), Status::Ok);
    CHECK(transferred == 0);
    CHECK_STATUS(f->write(0, ConstByteView(one), &transferred), Status::ReadOnly);
    CHECK_STATUS(f->resize(0), Status::ReadOnly);
    CHECK_STATUS(f->flush(), Status::Ok);
  }

  DirectoryEntry e;
  CHECK_STATUS(fs.stat("/", &e), Status::Ok);
  CHECK(e.type == NodeType::Directory && e.name.empty());
  CHECK_STATUS(fs.stat("/sub", &e), Status::Ok);
  CHECK(e.type == NodeType::Directory && e.name.equals("sub") && e.size == 0);
  CHECK_STATUS(fs.stat("/contig", &e), Status::Ok);
  CHECK(e.type == NodeType::File && e.size == v.contig.size() && !e.read_only);
  CHECK_STATUS(fs.stat("/locked", &e), Status::Ok);
  CHECK(e.read_only);
  CHECK_STATUS(fs.stat("/link", &e), Status::Ok);
  CHECK(e.size == 9);

  UniquePointer<IFile> f;
  UniquePointer<IDirectory> d;
  CHECK_STATUS(fs.stat("/nothing", &e), Status::NotFound);
  CHECK_STATUS(fs.stat("/HELLO.TXT", &e), Status::NotFound);  // names are case sensitive
  CHECK_STATUS(fs.stat("/many/f060", &e), Status::NotFound);
  CHECK_STATUS(fs.stat("/many/f", &e), Status::NotFound);
  CHECK_STATUS(fs.stat("/zzz", &e), Status::NotFound);
  CHECK_STATUS(fs.stat("/hello.txt/x", &e), Status::NotADirectory);
  CHECK_STATUS(fs.stat("/sub/../hello.txt", &e), Status::InvalidPath);
  CHECK_STATUS(fs.open_file("/sub", OpenMode::Read, &f), Status::IsADirectory);
  CHECK_STATUS(fs.open_directory("/hello.txt", &d), Status::NotADirectory);
  CHECK_STATUS(fs.open_file("/hello.txt", OpenMode::ReadWrite, &f), Status::ReadOnly);
  CHECK_STATUS(fs.open_file("/new", OpenMode::CreateOrOpen, &f), Status::ReadOnly);
  CHECK_STATUS(fs.create_directory("/x"), Status::ReadOnly);
  CHECK_STATUS(fs.remove("/hello.txt"), Status::ReadOnly);
  CHECK_STATUS(fs.rename("/hello.txt", "/y"), Status::ReadOnly);
  CHECK_STATUS(fs.flush(), Status::Ok);

  CHECK_STATUS(fs.open_directory("/many", &d), Status::Ok);
  int n = 0;
  while (d->next(&e) == Status::Ok) ++n;
  CHECK(n == 60);
  CHECK_STATUS(d->next(&e), Status::EndOfDirectory);
  d->rewind();
  CHECK_STATUS(d->next(&e), Status::Ok);
  CHECK(e.name.equals("f000"));
}

// Walks everything reachable without trusting sizes: whatever the image says, nothing may crash or loop.
static void walk(IFileSystem& fs, const std::string& path, int depth) {
  UniquePointer<IDirectory> dir;
  if (depth > 4 || fs.open_directory(path.c_str(), &dir) != Status::Ok) return;
  DirectoryEntry e;
  for (int count = 0; count < 500 && dir->next(&e) == Status::Ok; ++count) {
    const std::string child = (path == "/" ? "" : path) + "/" + e.name.c_str();
    DirectoryEntry again;
    fs.stat(child.c_str(), &again);
    if (e.type == NodeType::Directory) {
      walk(fs, child, depth + 1);
    } else {
      UniquePointer<IFile> f;
      if (fs.open_file(child.c_str(), OpenMode::Read, &f) == Status::Ok) {
        static uint8_t buffer[70000];
        uint64_t got = 0;
        f->read(0, ByteView(buffer, sizeof buffer), &got);
        f->read(f->size() > 100 ? f->size() - 100 : 0, ByteView(buffer, 100), &got);
      }
    }
  }
}

static void test_volume(uint32_t bs, uint32_t ag_shift, uint32_t groups, uint32_t ns, uint32_t device_block, const char* what) {
  Built v(bs, ag_shift, groups, ns);
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), device_block);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (fs) check_volume(*fs, v, what);
    fs = UniquePointer<IFileSystem>();

    // Truncated devices: the volume may mount, but nothing may read outside the device.
    const uint64_t cuts[] = {0, 1, 511, 512, 600, 676, bs, 3ull * bs, v.image.used_blocks * bs / 2, v.image.used_blocks * bs - 1};
    for (uint64_t cut : cuts) {
      const uint64_t length = cut / device_block * device_block;  // devices hold whole blocks
      if (length == 0) continue;
      MemDisk small(v.image.img.data(), length, device_block);
      UniquePointer<IFileSystem> t;
      const Status s = BefsFileSystem::mount(a, small, 0, &t);
      if (s == Status::Ok) walk(*t, "/", 0);
    }

    // Bit flips in the super block, inodes and run arrays.
    uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 4000; ++i) {
      const uint64_t block = v.image.meta[rnd() % v.image.meta.size()];
      const uint64_t byte = block * bs + (i % 3 == 0 ? 512 + rnd() % 140 : rnd() % 260);
      const uint8_t bit = 1u << (rnd() % 8);
      disk.data[byte] ^= bit;
      UniquePointer<IFileSystem> t;
      if (BefsFileSystem::mount(a, disk, 0, &t) == Status::Ok) walk(*t, "/", 0);
      disk.data[byte] ^= bit;
    }
    // Bit flips in the directory trees (their nodes live in the blocks after the inodes' data).
    for (int i = 0; i < 4000; ++i) {
      const uint64_t byte = rnd() % (v.image.used_blocks * bs);
      const uint8_t bit = 1u << (rnd() % 8);
      disk.data[byte] ^= bit;
      UniquePointer<IFileSystem> t;
      if (BefsFileSystem::mount(a, disk, 0, &t) == Status::Ok) walk(*t, "/", 0);
      disk.data[byte] ^= bit;
    }
  }
  CHECK(a.live == 0);
}

static void test_offset_and_foreign() {
  Built v(1024, 8, 16, 1024);
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512, 8);  // the volume starts at device block 8
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 8, &fs), Status::Ok);
    if (fs) check_volume(*fs, v, "BeFS at an offset");
    fs = UniquePointer<IFileSystem>();
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Corrupt);  // no super block there
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 1u << 20, &fs), Status::InvalidArgument);

    // The big-endian (PowerPC) variant is recognized but not read.
    std::vector<uint8_t> big_endian = v.image.img;
    put32(big_endian.data() + 512 + 36, 0x45474942);
    MemDisk other(big_endian.data(), big_endian.size(), 512);
    CHECK_STATUS(BefsFileSystem::mount(a, other, 0, &fs), Status::Unsupported);

    std::vector<uint8_t> zeros(8192, 0);
    MemDisk blank(zeros.data(), zeros.size(), 512);
    CHECK_STATUS(BefsFileSystem::mount(a, blank, 0, &fs), Status::Corrupt);
  }
  CHECK(a.live == 0);
}


// ---- Writing -----------------------------------------------------------------------------------------------------------------------

static uint64_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint64_t rd32(const uint8_t* p) { return rd16(p) | (uint64_t{rd16(p + 2)} << 16); }
static uint64_t rd64(const uint8_t* p) { return rd32(p) | (rd32(p + 4) << 32); }

struct RawVolume {
  const uint8_t* img;
  uint32_t bs, ag_shift, ag_count;
  uint64_t total, per_group;
};
typedef std::vector<std::pair<uint64_t, uint32_t>> RunList;
struct RawTable { RunList direct, indirect, dbl, arrays; };

static bool raw_run(const RawVolume& v, const uint8_t* p, uint64_t* block, uint32_t* length) {
  *block = (rd32(p) << v.ag_shift) + rd16(p + 4);
  *length = rd16(p + 6);
  return *length != 0 && *block + *length <= v.total;
}

static RawTable raw_table(const RawVolume& v, uint64_t inode) {
  RawTable table;
  const uint8_t* stream = v.img + inode * v.bs + 72;
  uint64_t block; uint32_t length;
  for (int i = 0; i < 12 && raw_run(v, stream + i * 8, &block, &length); ++i) table.direct.push_back({block, length});
  if (raw_run(v, stream + 104, &block, &length)) {
    table.arrays.push_back({block, length});
    for (uint64_t e = 0; e < uint64_t{length} * v.bs / 8; ++e) {
      uint64_t data; uint32_t n;
      if (!raw_run(v, v.img + block * v.bs + e * 8, &data, &n)) break;
      table.indirect.push_back({data, n});
    }
  }
  if (raw_run(v, stream + 120, &block, &length)) {
    table.arrays.push_back({block, length});
    for (uint64_t e = 0; e < uint64_t{length} * v.bs / 8; ++e) {
      uint64_t array; uint32_t alen;
      if (!raw_run(v, v.img + block * v.bs + e * 8, &array, &alen)) break;
      table.arrays.push_back({array, alen});
      for (uint64_t f = 0; f < uint64_t{alen} * v.bs / 8; ++f) {
        uint64_t data; uint32_t n;
        if (!raw_run(v, v.img + array * v.bs + f * 8, &data, &n)) break;
        table.dbl.push_back({data, n});
      }
    }
  }
  return table;
}

static std::vector<uint8_t> raw_stream(const RawVolume& v, uint64_t inode) {
  const RawTable table = raw_table(v, inode);
  std::vector<uint8_t> out;
  for (const RunList* runs : {&table.direct, &table.indirect, &table.dbl}) {
    for (auto& r : *runs) out.insert(out.end(), v.img + r.first * v.bs, v.img + (r.first + r.second) * v.bs);
  }
  out.resize(rd64(v.img + inode * v.bs + 72 + 136));
  return out;
}

struct TreeInfo { std::vector<std::pair<std::string, uint64_t>> entries; uint32_t nodes = 0, free_nodes = 0, levels = 0; };

// Checks a directory's B+tree from the raw image: order, separators, equal depth, links, free list, and that every node is accounted for.
static bool check_tree(const RawVolume& v, uint64_t dir_inode, TreeInfo* info, const char* what) {
  bool ok = true;
  const std::vector<uint8_t> s = raw_stream(v, dir_inode);
  if (s.size() < 40 || rd32(s.data()) != 0x69f6c2e8) { printf("  %s: no tree header\n", what); return false; }
  const uint32_t ns = rd32(s.data() + 4);
  info->levels = rd32(s.data() + 8);
  const uint64_t root = rd64(s.data() + 16), free_pointer = rd64(s.data() + 24), max_size = rd64(s.data() + 32);
  if (max_size != s.size() || ns == 0 || s.size() % ns) { printf("  %s: tree size %llu vs %zu\n", what, (unsigned long long)max_size, s.size()); return false; }
  std::vector<std::vector<uint64_t>> by_level;
  std::string previous;
  bool have_previous = false;
  int leaf_depth = -1;
  const uint64_t none = ~uint64_t{0};
  std::function<bool(uint64_t, uint32_t, std::string*, std::string*, bool*)> walk = [&](uint64_t off, uint32_t depth, std::string* low, std::string* high, bool* empty) -> bool {
    if (off % 8 || off < ns || off + ns > s.size() || depth > 40) return false;
    const uint8_t* n = s.data() + off;
    const uint32_t count = rd16(n + 24), key_bytes = rd16(n + 26);
    const uint32_t ends_at = (28 + key_bytes + 7) & ~7u, values_at = ends_at + count * 2;
    if (values_at + count * 8 > ns) return false;
    std::vector<std::string> keys;
    std::vector<uint64_t> values;
    uint32_t start = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t end = rd16(n + ends_at + i * 2);
      if (end < start || end > key_bytes) return false;
      keys.push_back(std::string(reinterpret_cast<const char*>(n + 28 + start), end - start));
      values.push_back(rd64(n + values_at + i * 8));
      start = end;
      if (i && !(keys[i - 1] < keys[i])) { printf("  %s: keys out of order in node %llu\n", what, (unsigned long long)off); ok = false; }
    }
    ++info->nodes;
    if (by_level.size() <= depth) by_level.resize(depth + 1);
    by_level[depth].push_back(off);
    const uint64_t overflow = rd64(n + 16);
    if (overflow == none) {
      if (leaf_depth < 0) leaf_depth = depth;
      if (static_cast<int>(depth) != leaf_depth) { printf("  %s: leaves at different depths\n", what); ok = false; }
      for (uint32_t i = 0; i < count; ++i) {
        if (have_previous && !(previous < keys[i])) { printf("  %s: leaves out of order at '%s'\n", what, keys[i].c_str()); ok = false; }
        previous = keys[i];
        have_previous = true;
        info->entries.push_back({keys[i], values[i]});
      }
      *empty = count == 0;
      if (count) { *low = keys.front(); *high = keys.back(); }
      return true;
    }
    values.push_back(overflow);
    bool first = true;
    for (uint32_t c = 0; c < values.size(); ++c) {
      std::string child_low, child_high;
      bool child_empty = false;
      if (!walk(values[c], depth + 1, &child_low, &child_high, &child_empty)) return false;
      if (child_empty) { printf("  %s: empty node below the root\n", what); ok = false; continue; }
      if (c < count && keys[c] < child_high) { printf("  %s: separator '%s' below its child's '%s'\n", what, keys[c].c_str(), child_high.c_str()); ok = false; }
      if (c > 0 && !(keys[c - 1] < child_low)) { printf("  %s: separator '%s' not below next child's '%s'\n", what, keys[c - 1].c_str(), child_low.c_str()); ok = false; }
      if (first) *low = child_low;
      *high = child_high;
      first = false;
    }
    *empty = first;
    return true;
  };
  std::string low, high;
  bool empty = false;
  if (!walk(root, 0, &low, &high, &empty)) { printf("  %s: tree cannot be walked\n", what); return false; }
  if (info->levels != by_level.size()) { printf("  %s: header says %u levels, found %zu\n", what, info->levels, by_level.size()); ok = false; }
  for (size_t l = 0; l < by_level.size(); ++l) {
    for (size_t j = 0; j < by_level[l].size(); ++j) {
      const uint8_t* n = s.data() + by_level[l][j];
      const uint64_t left = rd64(n), right = rd64(n + 8);
      const uint64_t want_left = j ? by_level[l][j - 1] : none, want_right = j + 1 < by_level[l].size() ? by_level[l][j + 1] : none;
      const bool strict = l + 1 == by_level.size();
      if (strict ? (left != want_left || right != want_right) : ((left != want_left && left != none) || (right != want_right && right != none))) {
        printf("  %s: links of node %llu at level %zu are wrong\n", what, (unsigned long long)by_level[l][j], l);
        ok = false;
      }
    }
  }
  uint64_t free_at = free_pointer;
  for (uint32_t guard = 0; free_at != none && guard < 100000; ++guard) {
    if (free_at % 8 || free_at < ns || free_at + ns > s.size() || rd64(s.data() + free_at + 16) != ~uint64_t{1}) { printf("  %s: bad free node\n", what); ok = false; break; }
    ++info->free_nodes;
    free_at = rd64(s.data() + free_at);
  }
  if (info->nodes + info->free_nodes + 1 != s.size() / ns) { printf("  %s: %u nodes + %u free + header != %zu\n", what, info->nodes, info->free_nodes, s.size() / ns); ok = false; }
  return ok;
}

// Checks a volume the way a file system check would: every block that something uses is marked in the bitmap, every marked block is
// used by exactly one thing, the used-blocks count is right, each stream maps what its size and ranges say, and the directory trees
// are well formed. Everything is found by walking from the root directory.
static void check_accounting(const uint8_t* img, const BefsImage& b, const char* what) {
  const RawVolume v{img, b.bs, b.ag_shift, b.ag_count, b.total, b.per_group};
  const uint32_t bs = b.bs;
  std::vector<uint8_t> owner(b.total, 0);
  bool ok = true;
  auto claim = [&](uint64_t block, uint64_t count) {
    for (uint64_t i = 0; i < count; ++i) {
      if (block + i >= b.total || owner[block + i]++) ok = false;
    }
  };
  claim(0, 1 + b.ag_count * b.per_group);
  std::vector<uint64_t> todo{(rd32(img + 512 + 116) << b.ag_shift) + rd16(img + 512 + 120)};
  std::set<uint64_t> seen;
  while (!todo.empty()) {
    const uint64_t inode = todo.back();
    todo.pop_back();
    if (!seen.insert(inode).second) { ok = false; continue; }
    const uint8_t* p = img + inode * bs;
    if (rd32(p) != 0x3bbe0ad9 || !(rd32(p + 24) & 1)) { printf("  inode %llu is not live\n", (unsigned long long)inode); ok = false; continue; }
    claim(inode, 1);
    const uint32_t mode = rd32(p + 20) & 0xF000;
    if (mode == 0xA000 && !(rd32(p + 24) & 0x40)) continue;
    const uint8_t* stream = p + 72;
    const RawTable table = raw_table(v, inode);
    uint64_t direct = 0, indirect = 0, dbl = 0;
    for (auto& r : table.direct) { claim(r.first, r.second); direct += r.second; }
    for (auto& r : table.indirect) { claim(r.first, r.second); indirect += r.second; }
    for (auto& r : table.dbl) { claim(r.first, r.second); dbl += r.second; }
    for (auto& r : table.arrays) claim(r.first, r.second);
    const uint64_t mapped = (direct + indirect + dbl) * bs;
    if (rd64(stream + 136) > mapped) { printf("  inode %llu: size beyond its blocks\n", (unsigned long long)inode); ok = false; }
    const uint64_t want = dbl ? mapped : indirect ? (direct + indirect) * bs : direct * bs;
    const uint64_t have = rd64(stream + 128) ? rd64(stream + 128) : rd64(stream + 112) ? rd64(stream + 112) : rd64(stream + 96);
    if (want != have) { printf("  inode %llu: ranges %llu, blocks %llu\n", (unsigned long long)inode, (unsigned long long)have, (unsigned long long)want); ok = false; }
    if (indirect && rd64(stream + 112) != (direct + indirect) * bs) ok = false;
    if (mode == 0x4000) {
      TreeInfo info;
      if (!check_tree(v, inode, &info, what)) ok = false;
      for (auto& e : info.entries) todo.push_back(e.second);
    }
  }
  uint64_t count = 0, reported = 0;
  for (uint64_t blk = 0; blk < b.total; ++blk) {
    const uint64_t group = blk >> b.ag_shift, bit = blk & ((uint64_t{1} << b.ag_shift) - 1);
    const bool marked = img[(1 + group * b.per_group + bit / (bs * 8)) * bs + (bit % (bs * 8)) / 8] & (1u << (bit % 8));
    if (marked != (owner[blk] == 1) && reported++ < 5) { ok = false; printf("  block %llu: marked %d, used %d\n", (unsigned long long)blk, marked, owner[blk]); }
    count += marked;
  }
  if (reported) ok = false;
  if (rd64(img + 512 + 56) != count) { ok = false; printf("  used_blocks %llu, bitmap %llu\n", (unsigned long long)rd64(img + 512 + 56), (unsigned long long)count); }
  if (!ok) { printf("FAILED accounting: %s\n", what); ++g_failed; }
}

struct Model {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
  std::vector<uint8_t>& at(const std::string& name) {
    for (auto& f : files) if (f.first == name) return f.second;
    abort();
  }
};

static bool write_file(IFileSystem& fs, const std::string& path, uint64_t offset, const std::vector<uint8_t>& data, Model* model) {
  UniquePointer<IFile> f;
  if (fs.open_file(path.c_str(), OpenMode::ReadWrite, &f) != Status::Ok) return false;
  uint64_t done = 0;
  const Status s = f->write(offset, ConstByteView(data.data(), data.size()), &done);
  if (s != Status::Ok) { printf("  write(%s, %llu, %zu) -> %s\n", path.c_str(), (unsigned long long)offset, data.size(), status_name(s)); return false; }
  if (done != data.size()) return false;
  std::vector<uint8_t>& m = model->at(path);
  if (m.size() < offset + data.size()) m.resize(offset + data.size(), 0);
  if (!data.empty()) memcpy(m.data() + offset, data.data(), data.size());
  return true;
}

static bool resize_file(IFileSystem& fs, const std::string& path, uint64_t size, Model* model) {
  UniquePointer<IFile> f;
  if (fs.open_file(path.c_str(), OpenMode::ReadWrite, &f) != Status::Ok) return false;
  const Status s = f->resize(size);
  if (s != Status::Ok) { printf("  resize(%s, %llu) -> %s\n", path.c_str(), (unsigned long long)size, status_name(s)); return false; }
  model->at(path).resize(size, 0);
  return f->size() == size;
}

static void verify_all(IFileSystem& fs, Model& model, const char* what) {
  for (auto& f : model.files) {
    std::vector<uint8_t> got;
    const bool same = read_back(fs, f.first.c_str(), &got) && got == f.second;
    if (!same) { printf("FAILED: %s differs (%s): %zu bytes, want %zu\n", f.first.c_str(), what, got.size(), f.second.size()); ++g_failed; }
  }
}

static Model model_of(const Built& v) {
  Model m;
  m.files.push_back({"/hello.txt", std::vector<uint8_t>{'H','e','l','l','o',',',' ','B','e','F','S','!','\n'}});
  m.files.push_back({"/empty", {}});
  m.files.push_back({"/frag", v.frag});
  m.files.push_back({"/big", v.big});
  m.files.push_back({"/contig", v.contig});
  return m;
}

static void test_write() {
  printf("BeFS writing\n");
  Built v(1024, 13, 3, 1024, 600);
  const uint32_t bs = 1024;
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512, 0, true);
    check_accounting(disk.data, v.image, "fresh image");
    {
      UniquePointer<IFileSystem> fs;
      CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
      if (!fs) return;
      CHECK(!fs->info().read_only && fs->info().free_bytes > 1000000);
      Model m = model_of(v);

      CHECK(write_file(*fs, "/hello.txt", 0, {'J', 'e', 'l', 'l', 'o'}, &m));
      CHECK(write_file(*fs, "/hello.txt", 13, make_data(900, 9), &m));       // grows inside the block
      CHECK(write_file(*fs, "/hello.txt", 5000, make_data(70, 10), &m));     // a gap of zeros, new blocks
      CHECK(write_file(*fs, "/contig", bs - 3, make_data(10, 11), &m));      // across a block boundary
      CHECK(write_file(*fs, "/contig", m.at("/contig").size(), make_data(3 * bs, 12), &m));
      CHECK(write_file(*fs, "/frag", 3 * bs + 5, make_data(2 * bs, 13), &m));
      CHECK(write_file(*fs, "/big", m.at("/big").size() - 50, make_data(5 * bs, 14), &m));  // grows through the double indirect area
      verify_all(*fs, m, "after small writes");
      CHECK_STATUS(fs->flush(), Status::Ok);
      check_accounting(disk.data, v.image, "after small writes");

      // A file that goes through direct, indirect and double indirect runs, because the free space is in pieces of 4 blocks.
      const std::vector<uint8_t> bitmap_before(disk.data + bs, disk.data + bs + 3 * bs);
      const uint64_t used_before = rd64(disk.data + 512 + 56);
      const uint64_t total = 2200ull * bs;
      for (uint64_t at = 0; at < total; at += 37000) {
        const uint64_t n = total - at < 37000 ? total - at : 37000;
        CHECK(write_file(*fs, "/empty", at, make_data(n, static_cast<uint32_t>(at)), &m));
      }
      verify_all(*fs, m, "after the long file");
      check_accounting(disk.data, v.image, "after the long file");

      // Cut it again, step by step, and back to nothing: everything it took is free again.
      for (uint64_t size : {uint64_t{2000} * bs + 17, uint64_t{600} * bs, uint64_t{13} * bs - 1, uint64_t{100}, uint64_t{1}}) {
        CHECK(resize_file(*fs, "/empty", size, &m));
        verify_all(*fs, m, "after cutting");
        check_accounting(disk.data, v.image, "after cutting");
      }
      CHECK(resize_file(*fs, "/empty", 0, &m));
      check_accounting(disk.data, v.image, "emptied");
      CHECK(rd64(disk.data + 512 + 56) == used_before);
      CHECK(std::vector<uint8_t>(disk.data + bs, disk.data + bs + 3 * bs) == bitmap_before);

      // Grow by resize: the new bytes are zeros even where old data was.
      CHECK(resize_file(*fs, "/contig", 10, &m));
      CHECK(resize_file(*fs, "/contig", 7 * bs + 3, &m));
      CHECK(resize_file(*fs, "/hello.txt", 2, &m));
      CHECK(resize_file(*fs, "/hello.txt", 3000, &m));
      verify_all(*fs, m, "after resizing");
      check_accounting(disk.data, v.image, "after resizing");

      // The same thing from a fresh mount: it was all written out.
      fs = UniquePointer<IFileSystem>();
      CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
      if (fs) verify_all(*fs, m, "after remount");

      // Errors and refusals.
      UniquePointer<IFile> f;
      DirectoryEntry entry;
      CHECK_STATUS(fs->open_file("/link", OpenMode::ReadWrite, &f), Status::Unsupported);
      CHECK_STATUS(fs->open_file("/sub", OpenMode::ReadWrite, &f), Status::IsADirectory);
      CHECK_STATUS(fs->open_file("/locked", OpenMode::ReadWrite, &f), Status::ReadOnly);
      CHECK_STATUS(fs->open_file("/hello.txt", OpenMode::ReadWrite, &f), Status::Ok);
      uint64_t done = 0;
      CHECK_STATUS(f->write(uint64_t{1} << 50, ConstByteView(reinterpret_cast<const uint8_t*>("x"), 1), &done), Status::NoSpace);
      CHECK_STATUS(f->resize(uint64_t{1} << 50), Status::NoSpace);
      CHECK_STATUS(fs->open_file("/hello.txt", OpenMode::CreateTruncate, &f), Status::Ok);
      CHECK_STATUS(fs->stat("/hello.txt", &entry), Status::Ok);
      CHECK(entry.size == 0);
      m.at("/hello.txt").clear();
      verify_all(*fs, m, "after truncating");
      f = UniquePointer<IFile>();
      check_accounting(disk.data, v.image, "at the end");
    }
  }
  CHECK(a.live == 0);
}

// A random series of writes and cuts on files of every layout, against a model.
static void test_write_random() {
  printf("BeFS random writes\n");
  Built v(1024, 13, 3, 1024, 200);
  const uint32_t bs = 1024;
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 1024, 0, true);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (!fs) return;
    Model m = model_of(v);
    uint32_t seed = 777;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 400; ++i) {
      const std::string& path = m.files[rnd() % m.files.size()].first;
      const uint64_t size = m.at(path).size();
      if (rnd() % 4 == 0) {
        const uint64_t to = rnd() % 3 == 0 ? 0 : rnd() % (size + 4 * bs + 1);
        if (!resize_file(*fs, path, to, &m)) { printf("  iteration %d\n", i); ++g_failed; break; }
      } else {
        const uint64_t offset = rnd() % (size + 2 * bs + 1);
        if (!write_file(*fs, path, offset, make_data(rnd() % (3 * bs + 1), rnd()), &m)) { printf("  iteration %d\n", i); ++g_failed; break; }
      }
      std::vector<uint8_t> got;
      if (!read_back(*fs, path.c_str(), &got) || got != m.at(path)) { printf("FAILED: %s differs at iteration %d\n", path.c_str(), i); ++g_failed; break; }
    }
    verify_all(*fs, m, "random writes");
    check_accounting(disk.data, v.image, "random writes");
    fs = UniquePointer<IFileSystem>();
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (fs) verify_all(*fs, m, "random writes, remounted");
  }
  CHECK(a.live == 0);
}

// A volume that fills up: the writes that do not fit fail, nothing else is harmed.
static void test_write_full() {
  printf("BeFS full volume\n");
  Built v(1024, 10, 1, 1024);
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512, 0, true);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (!fs) return;
    Model m = model_of(v);
    const std::vector<uint8_t> bitmap_before(disk.data + 1024, disk.data + 2048);
    UniquePointer<IFile> f;
    CHECK_STATUS(fs->open_file("/empty", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(f->resize(900ull * 1024), Status::NoSpace);
    check_accounting(disk.data, v.image, "after the failed grow");
    CHECK(f->size() == 0);
    std::vector<uint8_t> big(800 * 1024, 0x5A);
    uint64_t done = 0;
    CHECK_STATUS(f->write(0, ConstByteView(big.data(), big.size()), &done), Status::NoSpace);
    verify_all(*fs, m, "after the failed writes");
    check_accounting(disk.data, v.image, "after the failed writes");
    CHECK_STATUS(f->resize(0), Status::Ok);  // gives back what the failures left mapped
    check_accounting(disk.data, v.image, "emptied again");
    CHECK(std::vector<uint8_t>(disk.data + 1024, disk.data + 2048) == bitmap_before);
    // What fits does fit.
    std::vector<uint8_t> some(300 * 1024, 0x33);
    CHECK_STATUS(f->write(0, ConstByteView(some.data(), some.size()), &done), Status::Ok);
    CHECK(done == some.size());
    m.at("/empty") = some;
    f = UniquePointer<IFile>();
    verify_all(*fs, m, "after the write that fits");
    check_accounting(disk.data, v.image, "after the write that fits");
  }
  CHECK(a.live == 0);
}

// Volumes that must not be written.
static void test_write_refused() {
  printf("BeFS read-only mounts\n");
  Built v(1024, 8, 16, 1024);
  TestAllocator a;
  auto check_read_only = [&](MemDisk& disk, const char* why) {
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (!fs) return;
    UniquePointer<IFile> f;
    CHECK(fs->info().read_only);
    CHECK_STATUS(fs->open_file("/hello.txt", OpenMode::ReadWrite, &f), Status::ReadOnly);
    CHECK_STATUS(fs->open_file("/missing", OpenMode::CreateOrOpen, &f), Status::ReadOnly);
    CHECK_STATUS(fs->create_directory("/x"), Status::ReadOnly);
    CHECK_STATUS(fs->remove("/hello.txt"), Status::ReadOnly);
    CHECK_STATUS(fs->rename("/hello.txt", "/y"), Status::ReadOnly);
    std::vector<uint8_t> got;
    if (!read_back(*fs, "/hello.txt", &got) || got.size() != 13) { printf("FAILED: cannot read (%s)\n", why); ++g_failed; }
  };
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512);  // a device that cannot be written
    check_read_only(disk, "read-only device");
  }
  {
    std::vector<uint8_t> dirty = v.image.img;
    put32(dirty.data() + 512 + 84, 0x44495254);  // not shut down cleanly
    MemDisk disk(dirty.data(), dirty.size(), 512, 0, true);
    check_read_only(disk, "dirty");
  }
  {
    std::vector<uint8_t> logged = v.image.img;
    put64(logged.data() + 512 + 104, 5);  // something in the journal
    MemDisk disk(logged.data(), logged.size(), 512, 0, true);
    check_read_only(disk, "journal");
  }
  {
    std::vector<uint8_t> broken = v.image.img;
    broken[1024 + 0] &= ~1u;  // the super block's own block shows as free
    MemDisk disk(broken.data(), broken.size(), 512, 0, true);
    check_read_only(disk, "bitmap");
  }
  CHECK(a.live == 0);
}

static std::string long_name(int i, size_t length);

// Damaged volumes that are written to: whatever happens, nothing crashes or reads and writes outside the volume.
static void test_write_fuzz() {
  printf("BeFS writing to damaged volumes\n");
  Built v(1024, 10, 2, 1024, 40);
  const uint32_t bs = 1024;
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512, 0, true);
    const std::vector<uint8_t> pristine(disk.data, disk.data + v.image.img.size());
    uint32_t seed = 4242;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 1500; ++i) {
      memcpy(disk.data, pristine.data(), pristine.size());
      uint64_t block;
      switch (i % 3) {
        case 0: block = v.image.meta[rnd() % v.image.meta.size()]; break;
        case 1: block = 1 + rnd() % (1 + v.image.ag_count * v.image.per_group); break;
        default: block = 0; break;
      }
      const uint64_t byte = block * bs + (block == 0 ? 512 + rnd() % 140 : rnd() % 300);
      disk.data[byte] ^= 1u << (rnd() % 8);
      UniquePointer<IFileSystem> fs;
      if (BefsFileSystem::mount(a, disk, 0, &fs) != Status::Ok) continue;
      const char* paths[] = {"/hello.txt", "/empty", "/frag", "/big", "/contig", "/filler"};
      for (int k = 0; k < 6; ++k) {
        UniquePointer<IFile> f;
        if (fs->open_file(paths[rnd() % 6], OpenMode::ReadWrite, &f) != Status::Ok) continue;
        uint64_t done = 0;
        std::vector<uint8_t> data = make_data(rnd() % 5000, rnd());
        if (rnd() % 3 == 0) f->resize(rnd() % (200 * bs));
        else f->write(rnd() % (60 * bs), ConstByteView(data.data(), data.size()), &done);
        if (rnd() % 4 == 0) f->resize(rnd() % (20 * bs));
      }
      const char* places[] = {"/", "/sub/", "/many/", "/emptydir/", "/filler/"};
      for (int k = 0; k < 8; ++k) {
        const std::string one = std::string(places[rnd() % 5]) + long_name(rnd() % 12, rnd() % 2 ? 90 : 5);
        const std::string two = std::string(places[rnd() % 5]) + long_name(rnd() % 12, rnd() % 2 ? 90 : 5);
        UniquePointer<IFile> f;
        switch (rnd() % 5) {
          case 0: fs->create_directory(one.c_str()); break;
          case 1: fs->open_file(one.c_str(), OpenMode::CreateOrOpen, &f); break;
          case 2: fs->remove(one.c_str()); break;
          case 3: fs->rename(one.c_str(), two.c_str()); break;
          default: fs->remove((std::string(places[rnd() % 5]) + "f0" + std::to_string(10 + rnd() % 50)).c_str()); break;
        }
      }
      fs->flush();
    }
  }
  CHECK(a.live == 0);
}

// ---- Creating, removing, renaming --------------------------------------------------------------------------------------------------

static std::vector<std::string> names_in(IFileSystem& fs, const std::string& path) {
  std::vector<std::string> out;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path.c_str(), &dir) != Status::Ok) { out.push_back("!open"); return out; }
  DirectoryEntry e;
  Status s;
  while ((s = dir->next(&e)) == Status::Ok) out.push_back(e.name.c_str());
  if (s != Status::EndOfDirectory) out.push_back(std::string("!") + status_name(s));
  std::sort(out.begin(), out.end());
  return out;
}

static std::string long_name(int i, size_t length) {
  char number[16];
  snprintf(number, sizeof number, "%05d", i);
  std::string name = "n";
  name += number;
  while (name.size() < length) name += static_cast<char>('a' + (i + name.size()) % 26);
  return name;
}

static bool put_file(IFileSystem& fs, const std::string& path, const std::vector<uint8_t>& data) {
  UniquePointer<IFile> f;
  if (fs.open_file(path.c_str(), OpenMode::CreateTruncate, &f) != Status::Ok) return false;
  uint64_t done = 0;
  return data.empty() || (f->write(0, ConstByteView(data.data(), data.size()), &done) == Status::Ok && done == data.size());
}

static void test_namespace() {
  printf("BeFS creating, removing, renaming\n");
  Built v(1024, 13, 3, 1024);
  const uint32_t bs = 1024;
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512, 0, true);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (!fs) return;
    const std::vector<uint8_t> bitmap_before(disk.data + bs, disk.data + bs + 3 * bs);
    const uint64_t used_before = rd64(disk.data + 512 + 56);
    DirectoryEntry e;
    UniquePointer<IFile> f;

    // Files and directories.
    CHECK(put_file(*fs, "/new.txt", make_data(3000, 20)));
    std::vector<uint8_t> got;
    CHECK(read_back(*fs, "/new.txt", &got) && got == make_data(3000, 20));
    CHECK_STATUS(fs->create_directory("/newdir"), Status::Ok);
    CHECK_STATUS(fs->create_directory("/newdir/inner"), Status::Ok);
    CHECK(put_file(*fs, "/newdir/inner/deep.bin", make_data(70000, 21)));
    CHECK(read_back(*fs, "/newdir/inner/deep.bin", &got) && got == make_data(70000, 21));
    CHECK(names_in(*fs, "/newdir") == std::vector<std::string>{"inner"});
    CHECK_STATUS(fs->stat("/newdir/inner", &e), Status::Ok);
    CHECK(e.type == NodeType::Directory && e.name.equals("inner"));
    CHECK_STATUS(fs->open_file("/new.txt", OpenMode::CreateOrOpen, &f), Status::Ok);  // exists: opened, not truncated
    CHECK(f->size() == 3000);
    f = UniquePointer<IFile>();
    CHECK(names_in(*fs, "/").size() == 13);
    check_accounting(disk.data, v.image, "after creating");

    // Refusals.
    CHECK_STATUS(fs->create_directory("/newdir"), Status::AlreadyExists);
    CHECK_STATUS(fs->create_directory("/new.txt"), Status::AlreadyExists);
    CHECK_STATUS(fs->create_directory("/"), Status::InvalidPath);
    CHECK_STATUS(fs->create_directory("/sub/../b"), Status::InvalidPath);
    CHECK_STATUS(fs->create_directory("/nowhere/x"), Status::NotFound);
    CHECK_STATUS(fs->create_directory("/new.txt/x"), Status::NotADirectory);
    CHECK_STATUS(fs->create_directory(("/" + std::string(256, 'x')).c_str()), Status::InvalidPath);
    CHECK_STATUS(fs->create_directory(("/" + std::string(255, 'x')).c_str()), Status::Ok);
    CHECK_STATUS(fs->open_file("/nowhere/x", OpenMode::CreateOrOpen, &f), Status::NotFound);
    CHECK_STATUS(fs->open_file("/nothing", OpenMode::ReadWrite, &f), Status::NotFound);
    CHECK_STATUS(fs->remove("/"), Status::InvalidPath);
    CHECK_STATUS(fs->remove("/nothing"), Status::NotFound);
    CHECK_STATUS(fs->remove("/newdir"), Status::NotEmpty);
    CHECK_STATUS(fs->remove("/newdir/inner/deep.bin/x"), Status::NotADirectory);
    CHECK_STATUS(fs->rename("/nothing", "/x"), Status::NotFound);
    CHECK_STATUS(fs->rename("/new.txt", "/newdir"), Status::AlreadyExists);
    CHECK_STATUS(fs->rename("/new.txt", "/new.txt"), Status::AlreadyExists);
    CHECK_STATUS(fs->rename("/new.txt", "/nowhere/x"), Status::NotFound);
    CHECK_STATUS(fs->rename("/newdir", "/newdir/inner/moved"), Status::InvalidArgument);
    CHECK_STATUS(fs->rename("/newdir", "/newdir/moved"), Status::InvalidArgument);
    CHECK_STATUS(fs->rename("/", "/x"), Status::InvalidPath);

    // Open things cannot be removed; the handle stays good through a rename.
    CHECK_STATUS(fs->open_file("/new.txt", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(fs->remove("/new.txt"), Status::Busy);
    CHECK_STATUS(fs->rename("/new.txt", "/newdir/renamed.txt"), Status::Ok);
    uint64_t done = 0;
    CHECK_STATUS(f->write(3000, ConstByteView(reinterpret_cast<const uint8_t*>("tail"), 4), &done), Status::Ok);
    CHECK(read_back(*fs, "/newdir/renamed.txt", &got) && got.size() == 3004);
    CHECK_STATUS(fs->stat("/new.txt", &e), Status::NotFound);
    f = UniquePointer<IFile>();
    {
      UniquePointer<IDirectory> d;
      CHECK_STATUS(fs->open_directory("/newdir/inner", &d), Status::Ok);
      CHECK_STATUS(fs->remove("/newdir/inner/deep.bin"), Status::Ok);
      CHECK_STATUS(fs->remove("/newdir/inner"), Status::Busy);
    }
    CHECK_STATUS(fs->remove("/newdir/inner"), Status::Ok);
    check_accounting(disk.data, v.image, "after the first removals");

    // Moving a directory to another place.
    CHECK_STATUS(fs->create_directory("/a"), Status::Ok);
    CHECK_STATUS(fs->create_directory("/a/b"), Status::Ok);
    CHECK(put_file(*fs, "/a/b/file", make_data(5000, 22)));
    CHECK_STATUS(fs->rename("/a/b", "/newdir/b2"), Status::Ok);
    CHECK(names_in(*fs, "/a").empty());
    CHECK(read_back(*fs, "/newdir/b2/file", &got) && got == make_data(5000, 22));
    CHECK_STATUS(fs->rename("/newdir/b2", "/sub/../c"), Status::InvalidPath);
    CHECK_STATUS(fs->rename("/a", "/newdir/b2/a"), Status::Ok);
    CHECK(read_back(*fs, "/newdir/b2/a/../file", &got) == false);
    CHECK_STATUS(fs->rename("/newdir/b2", "/newdir/b2/a/inside"), Status::InvalidArgument);
    check_accounting(disk.data, v.image, "after moving");
    CHECK_STATUS(fs->remove("/newdir/b2/file"), Status::Ok);
    CHECK_STATUS(fs->remove("/newdir/b2/a"), Status::Ok);
    CHECK_STATUS(fs->remove("/newdir/b2"), Status::Ok);
    CHECK_STATUS(fs->remove("/newdir/renamed.txt"), Status::Ok);
    CHECK_STATUS(fs->remove("/newdir"), Status::Ok);
    CHECK_STATUS(fs->remove(("/" + std::string(255, 'x')).c_str()), Status::Ok);
    check_accounting(disk.data, v.image, "after the clean-up");
    CHECK(rd64(disk.data + 512 + 56) == used_before);
    CHECK(std::vector<uint8_t>(disk.data + bs, disk.data + bs + 3 * bs) == bitmap_before);  // everything came back

    // A directory with so many long names that its tree gets several levels.
    CHECK_STATUS(fs->create_directory("/wide"), Status::Ok);
    const int count = 600;
    std::vector<int> order(count);
    for (int i = 0; i < count; ++i) order[i] = i;
    uint32_t seed = 99;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = count - 1; i > 0; --i) std::swap(order[i], order[rnd() % (i + 1)]);
    for (int i : order) CHECK(put_file(*fs, "/wide/" + long_name(i, 100 + i % 100), i % 7 == 0 ? make_data(i * 3, i) : std::vector<uint8_t>()));
    check_accounting(disk.data, v.image, "wide directory");
    std::vector<std::string> expect;
    for (int i = 0; i < count; ++i) expect.push_back(long_name(i, 100 + i % 100));
    std::sort(expect.begin(), expect.end());
    CHECK(names_in(*fs, "/wide") == expect);
    {
      const RawVolume raw{disk.data, bs, v.image.ag_shift, v.image.ag_count, v.image.total, v.image.per_group};
      TreeInfo root_info, wide_info;
      CHECK(check_tree(raw, (rd32(disk.data + 512 + 116) << v.image.ag_shift) + rd16(disk.data + 512 + 120), &root_info, "root"));
      uint64_t wide_inode = 0;
      for (auto& entry : root_info.entries) if (entry.first == "wide") wide_inode = entry.second;
      CHECK(wide_inode != 0 && check_tree(raw, wide_inode, &wide_info, "wide"));
      CHECK(wide_info.levels >= 4 && wide_info.entries.size() == static_cast<size_t>(count));
    }
    for (int i = 0; i < count; ++i) {
      if (i % 7 == 0) {
        CHECK(read_back(*fs, ("/wide/" + long_name(i, 100 + i % 100)).c_str(), &got) && got == make_data(i * 3, i));
      } else {
        CHECK_STATUS(fs->stat(("/wide/" + long_name(i, 100 + i % 100)).c_str(), &e), Status::Ok);
      }
    }
    // Listing while the directory changes underneath: it carries on without error.
    {
      UniquePointer<IDirectory> d;
      CHECK_STATUS(fs->open_directory("/wide", &d), Status::Ok);
      int seen = 0;
      Status s;
      while ((s = d->next(&e)) == Status::Ok) {
        if (seen % 10 == 0) {
          const int victim = order[seen % count];
          fs->remove(("/wide/" + long_name(victim, 100 + victim % 100)).c_str());
          put_file(*fs, "/wide/" + long_name(1000 + seen, 120), {});
        }
        if (++seen > 5000) break;
      }
      CHECK(s == Status::EndOfDirectory);
      CHECK(seen > 100 && seen < 5000);
    }
    check_accounting(disk.data, v.image, "after listing during changes");
    // Empty it in random order, then remove it: the volume is as it was.
    std::vector<std::string> now = names_in(*fs, "/wide");
    for (size_t i = now.size(); i > 1; --i) std::swap(now[i - 1], now[rnd() % i]);
    for (size_t i = 0; i < now.size(); ++i) {
      CHECK_STATUS(fs->remove(("/wide/" + now[i]).c_str()), Status::Ok);
      if (i % 100 == 0) check_accounting(disk.data, v.image, "while emptying");
    }
    CHECK(names_in(*fs, "/wide").empty());
    check_accounting(disk.data, v.image, "emptied directory");
    {  // Fill it again: the nodes freed above are used again before the directory grows.
      const RawVolume raw{disk.data, bs, v.image.ag_shift, v.image.ag_count, v.image.total, v.image.per_group};
      TreeInfo root_info, before, after;
      CHECK(check_tree(raw, (rd32(disk.data + 512 + 116) << v.image.ag_shift) + rd16(disk.data + 512 + 120), &root_info, "root"));
      uint64_t wide_inode = 0;
      for (auto& entry : root_info.entries) if (entry.first == "wide") wide_inode = entry.second;
      CHECK(wide_inode != 0 && check_tree(raw, wide_inode, &before, "wide, empty"));
      CHECK(before.free_nodes > 20);
      const uint64_t size_before = rd64(disk.data + wide_inode * bs + 72 + 136);
      for (int i = 0; i < 250; ++i) CHECK(put_file(*fs, "/wide/" + long_name(i, 100 + i % 100), {}));
      CHECK(check_tree(raw, wide_inode, &after, "wide, refilled"));
      CHECK(after.entries.size() == 250 && after.free_nodes < before.free_nodes);
      CHECK(rd64(disk.data + wide_inode * bs + 72 + 136) == size_before);  // (no new nodes were needed)
      check_accounting(disk.data, v.image, "refilled directory");
      std::vector<std::string> again = names_in(*fs, "/wide");
      for (size_t i = 0; i < again.size(); ++i) CHECK_STATUS(fs->remove(("/wide/" + again[i]).c_str()), Status::Ok);
    }
    CHECK_STATUS(fs->remove("/wide"), Status::Ok);
    check_accounting(disk.data, v.image, "after removing it");
    CHECK(rd64(disk.data + 512 + 56) == used_before);
    CHECK(std::vector<uint8_t>(disk.data + bs, disk.data + bs + 3 * bs) == bitmap_before);

    // Removing files of every layout gives all their blocks back.
    CHECK_STATUS(fs->remove("/big"), Status::Ok);
    CHECK_STATUS(fs->remove("/frag"), Status::Ok);
    CHECK_STATUS(fs->remove("/contig"), Status::Ok);
    CHECK_STATUS(fs->remove("/link"), Status::Ok);
    CHECK_STATUS(fs->remove("/empty"), Status::Ok);
    CHECK_STATUS(fs->remove("/many"), Status::NotEmpty);
    check_accounting(disk.data, v.image, "after removing files");
    CHECK(read_back(*fs, "/hello.txt", &got) && got.size() == 13);
    CHECK(read_back(*fs, "/many/f007", &got) && std::string(got.begin(), got.end()) == "f007");

    fs = UniquePointer<IFileSystem>();
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (fs) CHECK(read_back(*fs, "/sub/nested.txt", &got) && got.size() == 6);
  }
  CHECK(a.live == 0);
}

// Random namespace operations against a model of the tree.
struct NsNode { bool dir; std::vector<uint8_t> data; };

static void test_namespace_random(uint32_t block_size, uint32_t ag_shift, const char* what) {
  printf("BeFS random namespace operations (%s)\n", what);
  Built v(block_size, ag_shift, 2, block_size);
  TestAllocator a;
  {
    MemDisk disk(v.image.img.data(), v.image.img.size(), 512, 0, true);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (!fs) return;
    CHECK_STATUS(fs->create_directory("/t"), Status::Ok);
    std::map<std::string, NsNode> model;
    model["/t"] = {true, {}};
    uint32_t seed = 31337;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    auto pick = [&](bool want_dir, bool any) -> std::string {
      std::vector<std::string> options;
      for (auto& n : model) if (any || n.second.dir == want_dir) options.push_back(n.first);
      return options.empty() ? std::string() : options[rnd() % options.size()];
    };
    auto children = [&](const std::string& dir) {
      std::vector<std::string> out;
      for (auto& n : model) if (n.first.size() > dir.size() + 1 && n.first.compare(0, dir.size() + 1, dir + "/") == 0 && n.first.find('/', dir.size() + 1) == std::string::npos) out.push_back(n.first.substr(dir.size() + 1));
      return out;
    };
    auto fresh_name = [&]() { return long_name(rnd() % 40, rnd() % 3 == 0 ? 150 : 6); };
    const int iterations = 1500;
    for (int i = 0; i < iterations; ++i) {
      const uint32_t op = rnd() % 10;
      if (op < 3) {  // create a file
        const std::string path = pick(true, false) + "/" + fresh_name();
        const bool exists = model.count(path);
        std::vector<uint8_t> data = make_data(rnd() % (3 * block_size), rnd());
        UniquePointer<IFile> f;
        Status s = fs->open_file(path.c_str(), OpenMode::CreateOrOpen, &f);
        if (exists && model[path].dir) { CHECK_STATUS(s, Status::IsADirectory); continue; }
        if (s != Status::Ok) { printf("  create %s -> %s (iteration %d)\n", path.c_str(), status_name(s), i); ++g_failed; break; }
        uint64_t done = 0;
        if (!exists) model[path] = {false, {}};
        if (!data.empty()) { s = f->write(0, ConstByteView(data.data(), data.size()), &done); CHECK_STATUS(s, Status::Ok); }
        std::vector<uint8_t>& m = model[path].data;
        if (m.size() < data.size()) m.resize(data.size(), 0);
        if (!data.empty()) memcpy(m.data(), data.data(), data.size());
      } else if (op < 4) {  // make a directory
        const std::string path = pick(true, false) + "/" + fresh_name();
        const Status s = fs->create_directory(path.c_str());
        if (model.count(path)) { CHECK_STATUS(s, Status::AlreadyExists); } else { CHECK_STATUS(s, Status::Ok); if (s == Status::Ok) model[path] = {true, {}}; }
      } else if (op < 7) {  // remove something
        const std::string path = pick(false, true);
        if (path.empty() || path == "/t") continue;
        const Status s = fs->remove(path.c_str());
        if (model[path].dir && !children(path).empty()) { CHECK_STATUS(s, Status::NotEmpty); } else { CHECK_STATUS(s, Status::Ok); if (s == Status::Ok) model.erase(path); }
      } else if (op < 9) {  // rename
        const std::string from = pick(false, true);
        if (from.empty() || from == "/t") continue;
        const std::string to = pick(true, false) + "/" + fresh_name();
        const Status s = fs->rename(from.c_str(), to.c_str());
        Status want = Status::Ok;
        if (model.count(to)) want = Status::AlreadyExists;
        else if (model[from].dir && (to == from || to.compare(0, from.size() + 1, from + "/") == 0)) want = Status::InvalidArgument;
        CHECK_STATUS(s, want);
        if (s == Status::Ok && want == Status::Ok) {
          std::map<std::string, NsNode> moved;
          for (auto it = model.begin(); it != model.end();) {
            if (it->first == from || it->first.compare(0, from.size() + 1, from + "/") == 0) { moved[to + it->first.substr(from.size())] = it->second; it = model.erase(it); }
            else ++it;
          }
          model.insert(moved.begin(), moved.end());
        }
      } else {  // cut or grow a file
        const std::string path = pick(false, false);
        if (path.empty()) continue;
        UniquePointer<IFile> f;
        if (fs->open_file(path.c_str(), OpenMode::ReadWrite, &f) != Status::Ok) continue;
        if (model.count(path) && !model[path].dir) {
          const uint64_t size = rnd() % (4 * block_size);
          CHECK_STATUS(f->resize(size), Status::Ok);
          model[path].data.resize(size, 0);
        }
      }
      if (i % 50 == 49 || i + 1 == iterations) {
        for (auto& n : model) {
          if (!n.second.dir) continue;
          std::vector<std::string> want = children(n.first);
          std::sort(want.begin(), want.end());
          if (names_in(*fs, n.first) != want) { printf("FAILED: listing of %s differs (iteration %d)\n", n.first.c_str(), i); ++g_failed; }
        }
        for (auto& n : model) {
          if (n.second.dir) continue;
          std::vector<uint8_t> got;
          if (!read_back(*fs, n.first.c_str(), &got) || got != n.second.data) { printf("FAILED: %s differs (iteration %d)\n", n.first.c_str(), i); ++g_failed; }
        }
        if (g_failed) break;
      }
      if (i % 300 == 299) check_accounting(disk.data, v.image, "random namespace");
    }
    check_accounting(disk.data, v.image, "random namespace, end");
    fs = UniquePointer<IFileSystem>();
    CHECK_STATUS(BefsFileSystem::mount(a, disk, 0, &fs), Status::Ok);
    if (fs) {
      for (auto& n : model) {
        if (n.second.dir) continue;
        std::vector<uint8_t> got;
        if (!read_back(*fs, n.first.c_str(), &got) || got != n.second.data) { printf("FAILED: %s differs after remount\n", n.first.c_str()); ++g_failed; }
      }
    }
  }
  CHECK(a.live == 0);
}

int main() {
  test_volume(1024, 8, 16, 1024, 512, "BeFS 1 KiB blocks, 512 byte device");
  test_volume(2048, 7, 32, 2048, 2048, "BeFS 2 KiB blocks, 2 KiB device");
  test_volume(1024, 10, 4, 4096, 512, "BeFS 1 KiB blocks, 4 KiB tree nodes");
  test_offset_and_foreign();
  test_write();
  test_write_random();
  test_write_full();
  test_write_refused();
  test_write_fuzz();
  test_namespace();
  test_namespace_random(1024, 13, "1 KiB blocks");
  test_namespace_random(2048, 12, "2 KiB blocks");
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("befs tests passed\n");
  return 0;
}
