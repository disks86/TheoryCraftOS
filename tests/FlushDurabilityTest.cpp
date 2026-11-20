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

// Power cuts under the FAT driver and the write-back block cache (the system volume's stack: FatFileSystem -> CachedStorageDevice ->
// device). The device below the cache is a PowerCutDisk: what it was asked to write sits in its own write cache until a flush puts it on
// the medium. A power cut keeps the medium and, in the harsher model, a random subset of the unflushed writes applied in random order
// (a device cache and the block cache both reorder). Everything the cache held is lost.
//
// - After IFile::flush() (FlushFileBuffers) returns, the file's size and content, and every other change made before it, survive a cut
//   that keeps only the medium. Checked at every flush of a random workload, with the cache's writeback thread running alongside.
// - A cut between flushes keeps every file nobody touched since the last flush intact, and leaves the volume without the damage that
//   needs more than "lost clusters" to repair: no directory without its "." and ".." entries, no cluster in two files (a freed cluster is
//   not handed out again before the free is durable), and a renamed file is found under its old or its new name.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "BlockCache/BlockCache.hpp"
#include "FatImage.hpp"
#include "FileSystem/Fat/FatFileSystem.hpp"

using namespace filesystem;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

namespace {

struct TestAllocator final : allocators::IAllocator {
  std::atomic<int64_t> live{0};
  void* allocate(uint64_t size) override {
    if (size == 0) return nullptr;
    ++live;
    return aligned_alloc(64, (size + 63) / 64 * 64);
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override {
    if (p) {
      --live;
      free(p);
    }
  }
};

struct HostEnv {
  struct Mutex {
    std::mutex m;
    void lock() { m.lock(); }
    void unlock() { m.unlock(); }
  };
  struct Condition {
    std::condition_variable_any cv;
    bool wait(Mutex& m, uint64_t us) { return cv.wait_for(m, std::chrono::microseconds(us)) == std::cv_status::no_timeout; }
    void notify_all() { cv.notify_all(); }
  };
  static uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
};

using Image = std::vector<uint8_t>;

// The device under the cache: writes are held in its write cache until flush() puts them on the medium.
struct PowerCutDisk final : blockcache::IBlockBackend {
  struct Pending {
    uint64_t lba;
    std::vector<uint8_t> data;
  };
  uint32_t bs;
  uint64_t blocks;
  std::mutex mutex;
  Image medium;
  std::vector<Pending> pending;
  std::atomic<uint64_t> flushes{0}, writes{0};

  PowerCutDisk(const uint8_t* image, uint64_t bytes, uint32_t block_size) : bs(block_size), blocks(bytes / block_size), medium(image, image + bytes) {}

  bool in_range(uint64_t lba, uint64_t bytes) const { return bytes % bs == 0 && lba <= blocks && bytes / bs <= blocks - lba; }
  bool read(uint64_t lba, ByteView buffer) override {
    if (!in_range(lba, buffer.size())) return false;
    std::lock_guard<std::mutex> g(mutex);
    memcpy(buffer.data(), medium.data() + lba * bs, buffer.size());
    for (const Pending& p : pending) {   // (the device answers from its cache: newest last)
      const uint64_t from = std::max(lba, p.lba), to = std::min(lba + buffer.size() / bs, p.lba + p.data.size() / bs);
      if (from < to) memcpy(buffer.data() + (from - lba) * bs, p.data.data() + (from - p.lba) * bs, (to - from) * bs);
    }
    return true;
  }
  bool write(uint64_t lba, ConstByteView buffer) override {
    if (!in_range(lba, buffer.size())) return false;
    std::lock_guard<std::mutex> g(mutex);
    pending.push_back(Pending{lba, std::vector<uint8_t>(buffer.data(), buffer.data() + buffer.size())});
    ++writes;
    return true;
  }
  bool flush() override {
    std::lock_guard<std::mutex> g(mutex);
    for (const Pending& p : pending) memcpy(medium.data() + p.lba * bs, p.data.data(), p.data.size());
    pending.clear();
    ++flushes;
    return true;
  }
  bool fill(uint64_t lba, uint64_t n) {
    if (lba > blocks || n > blocks - lba) return false;
    std::lock_guard<std::mutex> g(mutex);
    pending.push_back(Pending{lba, std::vector<uint8_t>(n * bs, 0)});
    return true;
  }
  bool discard(uint64_t lba, uint64_t n) override { return fill(lba, n); }
  bool write_zeroes(uint64_t lba, uint64_t n) override { return fill(lba, n); }

  // What the medium holds after the power goes: with `rng`, a random subset of the unflushed writes landed too, in random order.
  Image cut(std::mt19937* rng) {
    std::lock_guard<std::mutex> g(mutex);
    Image out = medium;
    if (!rng) return out;
    std::vector<size_t> order;
    for (size_t i = 0; i < pending.size(); ++i)
      if ((*rng)() % 2) order.push_back(i);
    std::shuffle(order.begin(), order.end(), *rng);
    for (size_t i : order) memcpy(out.data() + pending[i].lba * bs, pending[i].data.data(), pending[i].data.size());
    return out;
  }
};

// The block cache as an IStorageDevice (what CachedStorageDevice is in the kernel), with a writeback thread like the kernel's.
struct CachedDisk final : hardware::IStorageDevice {
  TestAllocator& allocator;
  PowerCutDisk& disk;
  blockcache::BlockCache<HostEnv> cache;
  std::thread flusher;
  std::atomic<bool> stop{false};

  CachedDisk(TestAllocator& a, PowerCutDisk& d, bool background) : allocator(a), disk(d), cache(a) {
    blockcache::BlockCacheConfig config;
    config.block_size = d.bs;
    config.block_count = d.blocks;
    config.page_bytes = 4096;
    config.capacity_bytes = 512u << 10;   // (smaller than the volume: writers write back too)
    config.expire_us = 500;
    if (!cache.initialize(disk, config)) abort();
    if (background)
      flusher = std::thread([this] {
        while (!stop) {
          cache.write_back_due(HostEnv::now_us());
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      });
  }
  ~CachedDisk() { power_off(); }
  // The power goes: the writeback thread stops where it is, and what the cache holds is gone.
  void power_off() {
    stop = true;
    if (flusher.joinable()) flusher.join();
  }

  const char* name() const override { return "cached"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::StorageInfo info() const override {
    hardware::StorageInfo i;
    i.block_count = disk.blocks;
    i.block_size = disk.bs;
    i.write_zeroes = true;
    return i;
  }
  hardware::StorageStatistics statistics() const override { return {}; }
  bool read(uint64_t lba, ByteView buffer) override { return cache.read(lba, buffer); }
  bool write(uint64_t lba, ConstByteView buffer) override { return cache.write(lba, buffer); }
  bool flush() override { return cache.flush(); }
  bool write_zeroes(uint64_t lba, uint64_t blocks) override { return cache.write_zeroes(lba, blocks); }
};

// A read-only device over an image (what is mounted after the cut).
struct ImageDisk final : hardware::IStorageDevice {
  const Image& image;
  uint32_t bs;
  ImageDisk(const Image& i, uint32_t block_size) : image(i), bs(block_size) {}
  const char* name() const override { return "image"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::StorageInfo info() const override {
    hardware::StorageInfo i;
    i.block_count = image.size() / bs;
    i.block_size = bs;
    i.read_only = true;
    return i;
  }
  hardware::StorageStatistics statistics() const override { return {}; }
  bool read(uint64_t lba, ByteView buffer) override {
    if (buffer.size() % bs || lba > image.size() / bs || buffer.size() / bs > image.size() / bs - lba) return false;
    memcpy(buffer.data(), image.data() + lba * bs, buffer.size());
    return true;
  }
  bool write(uint64_t, ConstByteView) override { return false; }
  bool flush() override { return true; }
};

// ---------------------------------------------------------------- a raw check of the volume (what fsck.fat looks at)

struct FsckResult {
  bool mounted_layout = false;
  int bad_dots = 0;        // a directory without "." and ".." pointing where they should
  int cross_links = 0;     // a cluster in two chains
  int duplicates = 0;      // two entries for one chain (a rename cut short: fsck keeps one name, nothing is lost)
  int broken_chains = 0;   // a chain running into a free or out-of-range cluster (fsck truncates it: a repair, not damage)
  int lost = 0;            // clusters marked used that no chain holds (fsck frees them)
};

struct RawFat {
  const Image& img;
  uint32_t bps = 0, spc = 0, reserved = 0, fats = 0, fat_sectors = 0, root_entries = 0, root_cluster = 0, data_start = 0, clusters = 0;
  int kind = 16;
  explicit RawFat(const Image& i) : img(i) {}
  static uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
  static uint32_t le32(const uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }
  bool parse() {
    if (img.size() < 512) return false;
    const uint8_t* b = img.data();
    bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16], root_entries = le16(b + 17);
    uint32_t total = le16(b + 19);
    if (!total) total = le32(b + 32);
    fat_sectors = le16(b + 22);
    if (!fat_sectors) fat_sectors = le32(b + 36), root_cluster = le32(b + 44);
    if (bps != 512 || !spc) return false;
    data_start = reserved + fats * fat_sectors + (root_entries * 32 + bps - 1) / bps;
    clusters = (total - data_start) / spc;
    kind = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    return true;
  }
  uint32_t fat(uint32_t c) const {
    const uint8_t* f = img.data() + reserved * bps;
    if (kind == 12) {
      const uint32_t w = le16(f + c + c / 2);
      return (c & 1) ? w >> 4 : w & 0xFFF;
    }
    return kind == 16 ? le16(f + c * 2) : le32(f + c * 4) & 0x0FFFFFFF;
  }
  bool is_end(uint32_t v) const { return v >= (kind == 12 ? 0xFF8u : kind == 16 ? 0xFFF8u : 0x0FFFFFF8u); }
  const uint8_t* cluster(uint32_t c) const { return img.data() + (uint64_t{data_start} + uint64_t{c - 2} * spc) * bps; }
};

FsckResult fsck(const Image& image) {
  FsckResult r;
  RawFat fat(image);
  if (!fat.parse()) return r;
  r.mounted_layout = true;
  std::vector<uint8_t> owned(fat.clusters + 2, 0);
  std::set<uint32_t> firsts;   // first clusters of the entries seen
  // Claims a chain; returns its clusters.
  auto claim = [&](uint32_t first) {
    std::vector<uint32_t> chain;
    if (owned[first] && firsts.count(first)) {
      ++r.duplicates;
      return chain;
    }
    firsts.insert(first);
    for (uint32_t c = first; c >= 2 && c < fat.clusters + 2;) {
      if (owned[c]) {
        ++r.cross_links;
        break;
      }
      owned[c] = 1;
      chain.push_back(c);
      const uint32_t next = fat.fat(c);
      if (fat.is_end(next)) break;
      if (next < 2 || next >= fat.clusters + 2) {
        ++r.broken_chains;
        break;
      }
      c = next;
    }
    return chain;
  };
  // Walks a directory: `chain` empty is the FAT12/16 root area.
  struct Todo {
    uint32_t self, parent;
    std::vector<uint32_t> chain;
  };
  std::vector<Todo> todo;
  {
    Todo root{0, 0, {}};
    if (fat.kind == 32) root.chain = claim(fat.root_cluster);
    todo.push_back(root);
  }
  int directories = 0;
  while (!todo.empty() && directories < 10000) {
    Todo d = todo.back();
    todo.pop_back();
    ++directories;
    std::vector<const uint8_t*> slots;
    if (d.chain.empty() && d.self == 0) {
      const uint8_t* area = image.data() + uint64_t{fat.reserved + fat.fats * fat.fat_sectors} * fat.bps;
      for (uint32_t i = 0; i < fat.root_entries; ++i) slots.push_back(area + i * 32);
    } else {
      for (uint32_t c : d.chain)
        for (uint32_t i = 0; i < fat.spc * fat.bps / 32; ++i) slots.push_back(fat.cluster(c) + i * 32);
    }
    const bool is_root = d.self == 0 || (fat.kind == 32 && d.self == fat.root_cluster);
    if (!is_root) {
      const uint32_t want_parent = d.parent == fat.root_cluster && fat.kind == 32 ? 0 : d.parent;
      auto first_of = [&](const uint8_t* e) { return fat.le16(e + 26) | (fat.kind == 32 ? fat.le16(e + 20) << 16 : 0); };
      if (slots.size() < 2 || memcmp(slots[0], ".          ", 11) != 0 || memcmp(slots[1], "..         ", 11) != 0 || first_of(slots[0]) != d.self ||
          first_of(slots[1]) != want_parent)
        ++r.bad_dots;
    }
    for (const uint8_t* e : slots) {
      if (e[0] == 0) break;
      if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08) || e[0] == '.') continue;
      const uint32_t first = fat.le16(e + 26) | (fat.kind == 32 ? fat.le16(e + 20) << 16 : 0);
      if (first < 2 || first >= fat.clusters + 2) continue;
      std::vector<uint32_t> chain = claim(first);
      if ((e[11] & 0x10) && !chain.empty()) todo.push_back(Todo{first, d.self == 0 && fat.kind == 32 ? fat.root_cluster : d.self, chain});
    }
  }
  for (uint32_t c = 2; c < fat.clusters + 2; ++c)
    if (!owned[c] && fat.fat(c) != 0) ++r.lost;
  return r;
}

// ---------------------------------------------------------------- what the files should hold

using Model = std::map<std::string, std::vector<uint8_t>>;   // file path -> content
using Dirs = std::set<std::string>;

bool read_file(IFileSystem& fs, const std::string& path, std::vector<uint8_t>* out) {
  UniquePointer<IFile> f;
  if (fs.open_file(StringView(path.c_str()), OpenMode::Read, &f) != Status::Ok) return false;
  out->assign(f->size(), 0);
  uint64_t got = 0;
  if (!out->empty() && (f->read(0, ByteView(out->data(), out->size()), &got) != Status::Ok || got != out->size())) return false;
  return true;
}

// Mounts `image` and compares every file of `model` with it, and the tree with `model` + `dirs` (nothing more, nothing less).
bool matches(const Image& image, const Model& model, const Dirs& dirs, const char* what) {
  TestAllocator a;
  ImageDisk disk(image, 512);
  UniquePointer<IFileSystem> fs;
  if (FatFileSystem::mount(a, disk, 0, &fs) != Status::Ok) {
    printf("  %s: does not mount\n", what);
    return false;
  }
  bool ok = true;
  for (const auto& [path, content] : model) {
    std::vector<uint8_t> got;
    if (!read_file(*fs, path, &got)) {
      printf("  %s: %s is missing\n", what, path.c_str());
      ok = false;
    } else if (got != content) {
      printf("  %s: %s has %zu bytes, want %zu%s\n", what, path.c_str(), got.size(), content.size(), got.size() == content.size() ? " (content differs)" : "");
      ok = false;
    }
  }
  // Nothing else is there.
  std::vector<std::string> stack{""};
  while (!stack.empty()) {
    const std::string dir = stack.back();
    stack.pop_back();
    UniquePointer<IDirectory> d;
    if (fs->open_directory(StringView(dir.empty() ? "/" : dir.c_str()), &d) != Status::Ok) {
      printf("  %s: cannot list %s\n", what, dir.c_str());
      ok = false;
      continue;
    }
    DirectoryEntry e;
    while (d->next(&e) == Status::Ok) {
      const std::string path = dir + "/" + e.name.c_str();
      if (e.type == NodeType::Directory) {
        if (!dirs.count(path)) {
          printf("  %s: unexpected folder %s\n", what, path.c_str());
          ok = false;
        }
        stack.push_back(path);
      } else if (!model.count(path)) {
        printf("  %s: unexpected file %s\n", what, path.c_str());
        ok = false;
      }
    }
  }
  for (const std::string& dir : dirs) {
    DirectoryEntry e;
    if (fs->stat(StringView(dir.c_str()), &e) != Status::Ok || e.type != NodeType::Directory) {
      printf("  %s: folder %s is missing\n", what, dir.c_str());
      ok = false;
    }
  }
  return ok;
}

std::vector<uint8_t> random_bytes(std::mt19937& rng, size_t n) {
  std::vector<uint8_t> v(n);
  for (uint8_t& b : v) b = static_cast<uint8_t>(rng());
  return v;
}

bool write_all(IFile& f, uint64_t offset, const std::vector<uint8_t>& data) {
  uint64_t done = 0;
  return f.write(offset, ConstByteView(data.data(), data.size()), &done) == Status::Ok && done == data.size();
}

// ---------------------------------------------------------------- the tests

// One file through the steps SQLite takes (create, write, grow, overwrite, truncate), flushed after each through the file: a cut right after
// any flush keeps exactly what the file held then.
void test_file_flush(FatImage::Kind kind, const char* label) {
  TestAllocator a;
  FatImage im(kind);
  PowerCutDisk disk(im.img, uint64_t{im.total} * 512, 512);
  std::mt19937 rng(7);
  std::vector<std::pair<Image, std::vector<uint8_t>>> cuts;
  {
    CachedDisk cached(a, disk, true);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(FatFileSystem::mount(a, cached, 0, &fs), Status::Ok);
    CHECK_STATUS(fs->create_directory("/Profile"), Status::Ok);
    UniquePointer<IFile> f;
    CHECK_STATUS(fs->open_file("/Profile/Web Data", OpenMode::CreateTruncate, &f), Status::Ok);
    std::vector<uint8_t> content;
    auto step = [&](uint64_t offset, size_t n) {
      const std::vector<uint8_t> data = random_bytes(rng, n);
      CHECK(write_all(*f, offset, data));
      if (content.size() < offset + n) content.resize(offset + n, 0);
      std::copy(data.begin(), data.end(), content.begin() + offset);
    };
    auto flush_and_cut = [&] {
      CHECK_STATUS(f->flush(), Status::Ok);
      cuts.emplace_back(disk.cut(nullptr), content);
    };
    step(0, 4096);
    flush_and_cut();
    step(4096, 3 * 4096 + 100);   // grows into new clusters, an odd size
    flush_and_cut();
    step(1000, 9000);             // overwrites across clusters
    flush_and_cut();
    step(40000, 777);             // a gap of zeros
    flush_and_cut();
    CHECK_STATUS(f->resize(5000), Status::Ok);
    content.resize(5000);
    flush_and_cut();
    CHECK_STATUS(f->resize(0), Status::Ok);
    content.clear();
    flush_and_cut();
    step(0, 12345);
    flush_and_cut();
    f = UniquePointer<IFile>();
    fs = UniquePointer<IFileSystem>();
  }
  int n = 0;
  for (const auto& [image, content] : cuts) {
    char what[64];
    snprintf(what, sizeof what, "%s cut %d", label, n++);
    CHECK(matches(image, Model{{"/Profile/Web Data", content}}, Dirs{"/Profile"}, what));
    const FsckResult r = fsck(image);
    CHECK(r.mounted_layout && r.bad_dots == 0 && r.cross_links == 0 && r.broken_chains == 0 && r.lost == 0);
  }
  printf("  %s: %zu cuts after FlushFileBuffers, %llu device flushes\n", label, cuts.size(), (unsigned long long)disk.flushes.load());
}

// A random workload on several files and folders, with the writeback thread running. At random points a file is flushed and the medium
// snapshotted: each snapshot must hold exactly the tree and contents of that moment.
struct Workload {
  IFileSystem& fs;
  std::mt19937& rng;
  Model model;
  Dirs dirs;
  std::set<std::string> touched;   // since the last flush
  std::map<std::string, std::string> renamed;   // new name -> old name, since the last flush
  int names = 0;

  Workload(IFileSystem& f, std::mt19937& r) : fs(f), rng(r) {}

  std::string pick_dir() {
    if (dirs.empty() || rng() % 3 == 0) return "";
    auto it = dirs.begin();
    std::advance(it, rng() % dirs.size());
    return *it;
  }
  std::string pick_file() {
    auto it = model.begin();
    std::advance(it, rng() % model.size());
    return it->first;
  }
  void step() {
    const uint32_t op = rng() % 100;
    if (op < 8 && dirs.size() < 12) {   // mkdir
      const std::string path = pick_dir() + "/D" + std::to_string(names++);
      CHECK_STATUS(fs.create_directory(StringView(path.c_str())), Status::Ok);
      dirs.insert(path);
      touched.insert(path);
    } else if ((op < 25 && model.size() < 24) || model.size() < 3) {   // a new file
      const std::string path = pick_dir() + "/file" + std::to_string(names++) + (rng() % 2 ? ".db" : ".db-journal");
      UniquePointer<IFile> f;
      CHECK_STATUS(fs.open_file(StringView(path.c_str()), OpenMode::CreateTruncate, &f), Status::Ok);
      std::vector<uint8_t> data = random_bytes(rng, rng() % 6000);
      if (f) CHECK(write_all(*f, 0, data));
      model[path] = data;
      touched.insert(path);
    } else if (op < 60) {   // write somewhere into a file (maybe past its end)
      const std::string path = pick_file();
      std::vector<uint8_t>& content = model[path];
      const uint64_t offset = rng() % (std::min<size_t>(content.size(), 30000) + 3000);
      const std::vector<uint8_t> data = random_bytes(rng, 1 + rng() % 5000);
      UniquePointer<IFile> f;
      CHECK_STATUS(fs.open_file(StringView(path.c_str()), OpenMode::ReadWrite, &f), Status::Ok);
      if (f) CHECK(write_all(*f, offset, data));
      if (content.size() < offset + data.size()) content.resize(offset + data.size(), 0);
      std::copy(data.begin(), data.end(), content.begin() + offset);
      touched.insert(path);
    } else if (op < 72) {   // truncate (the journal of TRUNCATE mode)
      const std::string path = pick_file();
      std::vector<uint8_t>& content = model[path];
      const uint64_t size = content.empty() ? 0 : rng() % (content.size() + 1);
      UniquePointer<IFile> f;
      CHECK_STATUS(fs.open_file(StringView(path.c_str()), OpenMode::ReadWrite, &f), Status::Ok);
      if (f) CHECK_STATUS(f->resize(size), Status::Ok);
      content.resize(size);
      touched.insert(path);
    } else if (op < 86) {   // delete (the journal of DELETE mode)
      const std::string path = pick_file();
      CHECK_STATUS(fs.remove(StringView(path.c_str())), Status::Ok);
      model.erase(path);
      renamed.erase(path);
      touched.insert(path);
    } else if (op < 96) {   // rename (Chrome's ImportantFileWriter: write a new file, move it over the old one)
      const std::string from = pick_file();
      const std::string to = pick_dir() + "/moved" + std::to_string(names++);
      CHECK_STATUS(fs.rename(StringView(from.c_str()), StringView(to.c_str())), Status::Ok);
      model[to] = model[from];
      model.erase(from);
      if (!touched.count(from)) renamed[to] = from;
      else if (renamed.count(from)) renamed[to] = renamed[from];   // (renamed again: the flushed name is still the first one)
      renamed.erase(from);
      touched.insert(from);
      touched.insert(to);
    } else {   // remove an empty folder
      for (const std::string& d : dirs) {
        bool empty = true;
        for (const auto& [path, content] : model)
          if (path.compare(0, d.size() + 1, d + "/") == 0) empty = false;
        for (const std::string& other : dirs)
          if (other.compare(0, d.size() + 1, d + "/") == 0) empty = false;
        if (!empty) continue;
        CHECK_STATUS(fs.remove(StringView(d.c_str())), Status::Ok);
        touched.insert(d);
        dirs.erase(d);
        break;
      }
    }
  }
  // FlushFileBuffers on one of the files (it makes the whole volume durable).
  void flush_one() {
    const std::string path = pick_file();
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file(StringView(path.c_str()), OpenMode::ReadWrite, &f), Status::Ok);
    if (f) CHECK_STATUS(f->flush(), Status::Ok);
    touched.clear();
    renamed.clear();
  }
};

void test_workload_flush(FatImage::Kind kind, uint32_t seed, const char* label) {
  TestAllocator a;
  FatImage im(kind);
  PowerCutDisk disk(im.img, uint64_t{im.total} * 512, 512);
  std::mt19937 rng(seed);
  struct Cut {
    Image image;
    Model model;
    Dirs dirs;
  };
  std::vector<Cut> cuts;
  {
    CachedDisk cached(a, disk, true);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(FatFileSystem::mount(a, cached, 0, &fs), Status::Ok);
    Workload w(*fs, rng);
    for (int i = 0; i < 1500; ++i) {
      w.step();
      if (rng() % 40 == 0) {
        w.flush_one();
        cuts.push_back(Cut{disk.cut(nullptr), w.model, w.dirs});
      }
    }
    fs = UniquePointer<IFileSystem>();
  }
  int n = 0, bad = 0;
  for (const Cut& c : cuts) {
    char what[64];
    snprintf(what, sizeof what, "%s cut %d", label, n++);
    if (!matches(c.image, c.model, c.dirs, what)) ++bad;
    const FsckResult r = fsck(c.image);
    if (r.bad_dots || r.cross_links || r.broken_chains || r.lost) {
      printf("  %s: fsck: %d bad dots, %d cross links, %d broken chains, %d lost\n", what, r.bad_dots, r.cross_links, r.broken_chains, r.lost);
      ++bad;
    }
  }
  CHECK(bad == 0);
  printf("  %s: %zu cuts after flushes, %d wrong\n", label, cuts.size(), bad);
}

// Cuts between flushes, keeping a random part of what was written since: files not touched since the last flush are intact, and the
// volume needs no repair beyond lost clusters and short chains.
void test_cut_between_flushes(FatImage::Kind kind, uint32_t seed, const char* label) {
  int damaged = 0, checked = 0, total_lost = 0;
  for (int round = 0; round < 40; ++round) {
    TestAllocator a;
    FatImage im(kind);
    PowerCutDisk disk(im.img, uint64_t{im.total} * 512, 512);
    std::mt19937 rng(seed * 1000 + round);
    Image image;
    Model at_flush;
    Dirs dirs_at_flush;
    std::set<std::string> touched;
    std::map<std::string, std::string> renamed;
    {
      CachedDisk cached(a, disk, true);
      UniquePointer<IFileSystem> fs;
      CHECK_STATUS(FatFileSystem::mount(a, cached, 0, &fs), Status::Ok);
      Workload w(*fs, rng);
      const int before = 100 + rng() % 300, after = 1 + rng() % 60;
      for (int i = 0; i < before; ++i) w.step();
      w.flush_one();
      at_flush = w.model;
      dirs_at_flush = w.dirs;
      for (int i = 0; i < after; ++i) w.step();
      touched = w.touched;
      renamed = w.renamed;
      cached.power_off();
      image = disk.cut(&rng);
      fs = UniquePointer<IFileSystem>();
    }
    char what[64];
    snprintf(what, sizeof what, "%s round %d", label, round);
    bool ok = true;
    const FsckResult r = fsck(image);
    total_lost += r.lost;
    if (r.bad_dots || r.cross_links) {
      printf("  %s: %d folders without . and .., %d cross-linked clusters\n", what, r.bad_dots, r.cross_links);
      ok = false;
    }
    TestAllocator b;
    ImageDisk id(image, 512);
    UniquePointer<IFileSystem> fs;
    if (FatFileSystem::mount(b, id, 0, &fs) != Status::Ok) {
      printf("  %s: does not mount\n", what);
      ok = false;
    } else {
      for (const auto& [path, content] : at_flush) {
        if (touched.count(path)) continue;
        std::vector<uint8_t> got;
        if (!read_file(*fs, path, &got) || got != content) {
          printf("  %s: untouched %s lost or changed\n", what, path.c_str());
          ok = false;
        }
      }
      for (const auto& [to, from] : renamed) {   // a file renamed since the flush, unchanged otherwise: under one of its names
        const auto it = at_flush.find(from);
        if (it == at_flush.end()) continue;
        std::vector<uint8_t> got;
        const bool old_name = read_file(*fs, from, &got) && got == it->second;
        const bool new_name = read_file(*fs, to, &got) && got == it->second;
        if (!old_name && !new_name) {
          // (written to after the rename: then the content may be newer under the new name)
          DirectoryEntry e;
          if (fs->stat(StringView(from.c_str()), &e) != Status::Ok && fs->stat(StringView(to.c_str()), &e) != Status::Ok) {
            printf("  %s: renamed %s -> %s is under neither name\n", what, from.c_str(), to.c_str());
            ok = false;
          }
        }
      }
    }
    fs = UniquePointer<IFileSystem>();
    if (!ok) ++damaged;
    ++checked;
  }
  CHECK(damaged == 0);
  printf("  %s: %d cuts between flushes, %d damaged, %d lost clusters in all (repairable)\n", label, checked, damaged, total_lost);
}

}  // namespace

int main() {
  const FatImage::Kind kinds[3] = {FatImage::F12, FatImage::F16, FatImage::F32};
  const char* names[3] = {"FAT12", "FAT16", "FAT32"};
  for (int k = 0; k < 3; ++k) test_file_flush(kinds[k], names[k]);
  for (int k = 0; k < 3; ++k) test_workload_flush(kinds[k], 11 + k, names[k]);
  for (int k = 0; k < 3; ++k) test_cut_between_flushes(kinds[k], 21 + k, names[k]);
  if (g_failed) {
    printf("%d flush durability checks FAILED\n", g_failed);
    return 1;
  }
  printf("flush durability tests passed\n");
  return 0;
}
