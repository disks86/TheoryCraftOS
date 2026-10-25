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

// Host-side tests for NtfsFileSystem against NTFS images built here: system files, resident and non-resident data (fragmented, sparse,
// LZNT1 compressed, split over an extension record through an attribute list), directories from the index root up to several levels of
// index blocks, 8.3 aliases and hard links; then everything the write side does (create, write, grow, cut, rename, remove, a full MFT, a
// full volume) checked by a verifier that walks the volume the way a chkdsk would (clusters in use against the bitmap, file records
// against the MFT bitmap, folders in order), randomized operations against a model, a device that fails, and truncated and bit-flipped images.
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
#include <sys/stat.h>
#include "FileSystem/Ntfs/NtfsFileSystem.hpp"

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
#include "FileTimes.hpp"

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
static void p16(Bytes& b, size_t o, uint32_t v) { b[o] = v; b[o + 1] = v >> 8; }
static void p32(Bytes& b, size_t o, uint32_t v) { p16(b, o, v); p16(b, o + 2, v >> 16); }
static void p64(Bytes& b, size_t o, uint64_t v) { p32(b, o, static_cast<uint32_t>(v)); p32(b, o + 4, static_cast<uint32_t>(v >> 32)); }
static uint32_t g16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t g32(const uint8_t* p) { return g16(p) | (g16(p + 2) << 16); }
static uint64_t g64(const uint8_t* p) { return g32(p) | (uint64_t{g32(p + 4)} << 32); }
static uint32_t a8(uint32_t x) { return (x + 7) & ~7u; }

static uint8_t pattern(uint64_t i, uint32_t seed) { return static_cast<uint8_t>(i * 7 + seed + (i >> 8)); }
static Bytes make_data(uint64_t size, uint32_t seed) {
  Bytes v(size);
  for (uint64_t i = 0; i < size; ++i) v[i] = pattern(i, seed);
  return v;
}

typedef std::u16string U16;
static U16 u16(const std::string& s) {  // UTF-8 to UTF-16
  U16 out;
  for (size_t i = 0; i < s.size();) {
    uint32_t b = static_cast<uint8_t>(s[i]), cp, extra;
    if (b < 0x80) cp = b, extra = 0; else if ((b & 0xE0) == 0xC0) cp = b & 0x1F, extra = 1; else if ((b & 0xF0) == 0xE0) cp = b & 0x0F, extra = 2; else cp = b & 7, extra = 3;
    for (uint32_t k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    i += extra + 1;
    if (cp >= 0x10000) { cp -= 0x10000; out += static_cast<char16_t>(0xD800 + (cp >> 10)); out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF)); }
    else out += static_cast<char16_t>(cp);
  }
  return out;
}
static char16_t upc(char16_t c) {  // the $UpCase table of the test volumes
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c == 0xE9) return 0xC9;
  if (c >= 0x3B1 && c <= 0x3C9) return c - 32;
  return c;
}
static bool name_less(const U16& a, const U16& b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) if (upc(a[i]) != upc(b[i])) return upc(a[i]) < upc(b[i]);
  if (a.size() != b.size()) return a.size() < b.size();
  return a < b;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// LZNT1 (a small greedy compressor, so that matches of every distance width appear)

static Bytes lznt1_compress(const Bytes& in) {
  Bytes out;
  for (size_t base = 0; base < in.size(); base += 4096) {
    const size_t n = std::min<size_t>(4096, in.size() - base);
    Bytes chunk;
    size_t pos = 0;
    while (pos < n) {
      size_t flag_at = chunk.size();
      chunk.push_back(0);
      for (int bit = 0; bit < 8 && pos < n; ++bit) {
        uint32_t offset_bits = 4;
        while ((1u << offset_bits) < pos) ++offset_bits;
        const uint32_t length_bits = 16 - offset_bits;
        const size_t max_len = std::min<size_t>((1u << length_bits) + 2, n - pos);
        size_t best_len = 0, best_dist = 0;
        for (size_t dist = 1; dist <= pos && dist <= (1u << offset_bits); ++dist) {
          size_t len = 0;
          while (len < max_len && in[base + pos + len] == in[base + pos + len - dist]) ++len;
          if (len > best_len) best_len = len, best_dist = dist;
        }
        if (best_len >= 3) {
          chunk[flag_at] |= 1 << bit;
          const uint32_t token = static_cast<uint32_t>(((best_dist - 1) << length_bits) | (best_len - 3));
          chunk.push_back(token); chunk.push_back(token >> 8);
          pos += best_len;
        } else {
          chunk.push_back(in[base + pos]);
          ++pos;
        }
      }
    }
    if (chunk.size() >= n) {  // stored
      uint32_t header = static_cast<uint32_t>(n - 1) | 0x3000;
      out.push_back(header); out.push_back(header >> 8);
      out.insert(out.end(), in.begin() + base, in.begin() + base + n);
    } else {
      uint32_t header = static_cast<uint32_t>(chunk.size() - 1) | 0xB000;
      out.push_back(header); out.push_back(header >> 8);
      out.insert(out.end(), chunk.begin(), chunk.end());
    }
  }
  out.push_back(0); out.push_back(0);
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The image builder

typedef std::vector<std::pair<int64_t, uint64_t>> RunSpec;  // lcn (-1: hole), length

static Bytes encode_pairs(const RunSpec& runs) {
  Bytes out;
  int64_t previous = 0;
  for (const auto& r : runs) {
    uint32_t lb = 1; while (lb < 8 && (r.second >> (8 * lb - 1))) ++lb;  // (signed, like Windows)
    int64_t delta = 0; uint32_t ob = 0;
    if (r.first >= 0) {
      delta = r.first - previous; previous = r.first; ob = 1;
      while (ob < 8) { const int64_t lim = int64_t{1} << (8 * ob - 1); if (delta >= -lim && delta < lim) break; ++ob; }
    }
    out.push_back(lb | (ob << 4));
    for (uint32_t k = 0; k < lb; ++k) out.push_back(r.second >> (8 * k));
    for (uint32_t k = 0; k < ob; ++k) out.push_back(static_cast<uint64_t>(delta) >> (8 * k));
  }
  out.push_back(0);
  return out;
}

struct Rec {
  uint64_t number = 0; uint16_t seq = 1, flags = 1, links = 1; bool present = true; uint64_t base = 0;
  std::vector<Bytes> attrs; uint16_t next_id = 0;

  Bytes resident(uint32_t type, const std::string& name, const Bytes& value, bool indexed = false) {
    const U16 n = u16(name);
    const uint32_t vo = a8(0x18 + 2 * n.size()), len = a8(vo + value.size());
    Bytes a(len, 0);
    p32(a, 0, type); p32(a, 4, len); a[9] = n.size(); p16(a, 10, n.empty() ? 0 : 0x18); p16(a, 14, next_id++);
    p32(a, 16, value.size()); p16(a, 20, vo); a[22] = indexed;
    for (size_t i = 0; i < n.size(); ++i) p16(a, 0x18 + 2 * i, n[i]);
    if (!value.empty()) memcpy(a.data() + vo, value.data(), value.size());
    return a;
  }
  Bytes nonres(uint32_t type, const std::string& name, const RunSpec& runs, uint64_t size, uint64_t init, uint16_t aflags = 0, uint32_t shift = 0,
               uint64_t lowest = 0, uint64_t cb = 4096, bool first_extent = true) {
    const U16 n = u16(name);
    const uint32_t hdr = (aflags & 0x8001) ? 0x48 : 0x40;
    const uint32_t mo = a8(hdr + 2 * n.size());
    Bytes pairs = encode_pairs(runs);
    const uint32_t len = a8(mo + pairs.size());
    uint64_t clusters = 0; for (auto& r : runs) clusters += r.second;
    Bytes a(len, 0);
    p32(a, 0, type); p32(a, 4, len); a[8] = 1; a[9] = n.size(); p16(a, 10, n.empty() ? 0 : hdr); p16(a, 12, aflags); p16(a, 14, next_id++);
    p64(a, 0x10, lowest); p64(a, 0x18, lowest + clusters - 1); p16(a, 0x20, mo); p16(a, 0x22, shift);
    if (first_extent) { p64(a, 0x28, (lowest + clusters) * cb); p64(a, 0x30, size); p64(a, 0x38, init); }
    if (hdr == 0x48) { uint64_t stored = 0; for (auto& r : runs) if (r.first >= 0) stored += r.second; p64(a, 0x40, stored * cb); }
    for (size_t i = 0; i < n.size(); ++i) p16(a, hdr + 2 * i, n[i]);
    memcpy(a.data() + mo, pairs.data(), pairs.size());
    return a;
  }
  void add(const Bytes& a) { attrs.push_back(a); }

  Bytes build(uint32_t rs) const {
    Bytes r(rs, 0);
    const uint32_t count = rs / 512 + 1, first = a8(0x30 + 2 * count);
    memcpy(r.data(), "FILE", 4); p16(r, 4, 0x30); p16(r, 6, count); p16(r, 0x10, seq); p16(r, 0x12, links); p16(r, 0x14, first); p16(r, 0x16, present ? flags : 0);
    p64(r, 0x20, base); p16(r, 0x28, next_id); p32(r, 0x2C, static_cast<uint32_t>(number));
    uint32_t off = first;
    for (const Bytes& a : attrs) { if (off + a.size() + 8 > rs) { printf("record %llu overflows\n", (unsigned long long)number); abort(); } memcpy(r.data() + off, a.data(), a.size()); off += a.size(); }
    p32(r, off, 0xFFFFFFFF); off += 8;
    p32(r, 0x18, off); p32(r, 0x1C, rs);
    p16(r, 0x30, 1);
    for (uint32_t i = 1; i < count; ++i) { p16(r, 0x30 + 2 * i, g16(&r[i * 512 - 2])); p16(r, i * 512 - 2, 1); }
    return r;
  }
};

struct Opts {
  uint32_t bps = 512, spc = 8, rs = 1024, ibs = 4096, mft_records = 64;
  uint64_t clusters = 4096;
  uint32_t log_mode = 0;  // 0: reset (all 0xFF), 1: clean RSTR, 2: dirty RSTR
  uint32_t volume_flags = 0;
  uint32_t version_major = 3;
  bool hiberfil = false;
};

struct DirItem { U16 name; uint64_t ref; Bytes key; };

struct Layout {
  bool force_nonresident = false, fragmented = false, sparse_middle = false, compressed = false, attr_list = false;
  uint64_t init = ~uint64_t{0};
  bool dos_name = false;
};

struct Vol {
  Opts o; std::vector<Rec> recs; std::vector<bool> used; uint64_t next = 0; Bytes img;
  std::map<uint64_t, std::vector<DirItem>> dirs;
  uint64_t mft_lcn = 0, mirror_lcn = 0, mft_clusters = 0;
  uint32_t cb() const { return o.bps * o.spc; }
  uint32_t unit() const { return cb() <= o.ibs ? cb() : 512; }

  uint64_t alloc(uint64_t n) { const uint64_t b = next; for (uint64_t i = 0; i < n; ++i) used[b + i] = true; next += n; return b; }
  void write(uint64_t lcn, const Bytes& data) { memcpy(img.data() + lcn * cb(), data.data(), data.size()); }
  uint64_t ref(uint64_t number) const { return number | (uint64_t{recs[number].seq} << 48); }

  static Bytes fname(uint64_t parent_ref, const U16& name, uint8_t ns, uint32_t flags, uint64_t alloc_size = 0, uint64_t real = 0) {
    Bytes v(0x42 + 2 * name.size(), 0);
    p64(v, 0, parent_ref); for (int i = 0; i < 4; ++i) p64(v, 8 + 8 * i, 0x01D0000000000000ull);
    p64(v, 0x28, alloc_size); p64(v, 0x30, real); p32(v, 0x38, flags); v[0x40] = name.size(); v[0x41] = ns;
    for (size_t i = 0; i < name.size(); ++i) p16(v, 0x42 + 2 * i, name[i]);
    return v;
  }
  Bytes si(uint32_t attrs = 0x20) {
    Bytes v(0x48, 0); for (int i = 0; i < 4; ++i) p64(v, 8 * i, 0x01D0000000000000ull); p32(v, 0x20, attrs); p32(v, 0x34, 0x100);
    return v;
  }

  Vol(const Opts& options) : o(options) {
    used.assign(o.clusters, false);
    img.assign(o.clusters * cb(), 0);
    recs.resize(o.mft_records);
    for (uint64_t i = 0; i < recs.size(); ++i) recs[i].number = i, recs[i].present = i < 16 || false;
    for (uint64_t i = 16; i < 24 && i < recs.size(); ++i) recs[i].present = false;
    for (uint64_t i = 24; i < recs.size(); ++i) recs[i].present = false, recs[i].flags = 0;
    // $Boot first, then the MFT, its mirror and the other system files
    alloc(2);
    mft_clusters = (uint64_t{o.mft_records} * o.rs + cb() - 1) / cb();
    mft_lcn = alloc(mft_clusters);
    mirror_lcn = alloc((4 * o.rs + cb() - 1) / cb());
    system_files();
  }

  uint32_t user_next = 24;
  uint64_t new_record() { while (user_next < recs.size() && recs[user_next].present) ++user_next; if (user_next >= recs.size()) { printf("out of records\n"); abort(); } recs[user_next].present = true; recs[user_next].flags = 1; return user_next; }

  void sys_record(uint64_t n, const std::string& name, bool directory = false) {
    Rec& r = recs[n]; r.present = true; r.flags = 1 | (directory ? 2 : 0);
    r.add(r.resident(0x10, "", si(0x6)));
    add_entry(5, n, name, 3, directory);
  }
  uint64_t ref_of_root() const { return 5 | (uint64_t{5} << 48); }

  void system_files() {
    for (int i = 0; i < 16; ++i) recs[i].seq = i == 0 ? 1 : i;
    recs[5].seq = 5;
    // 0 $MFT
    sys_record(0, "$MFT");
    sys_record(1, "$MFTMirr");
    sys_record(2, "$LogFile");
    sys_record(3, "$Volume");
    sys_record(4, "$AttrDef");
    sys_record(6, "$Bitmap");
    sys_record(7, "$Boot");
    sys_record(8, "$BadClus");
    sys_record(9, "$Secure");
    sys_record(10, "$UpCase");
    sys_record(11, "$Extend", true);
    for (int i = 12; i < 16; ++i) { recs[i].present = true; recs[i].add(recs[i].resident(0x10, "", si(0x6))); }
    recs[5].flags = 3;
    // the root folder's own entry "." and the folder's attributes come at the end
  }

  // Data attribute for an unnamed stream of contents `data` stored per `spec`.

  // Stores `data` in clusters: contiguous, fragmented (every other cluster) ...
  RunSpec store(const Bytes& data, bool fragmented, uint64_t extra_clusters = 0) {
    RunSpec runs;
    const uint64_t n = (data.size() + cb() - 1) / cb() + extra_clusters;
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t lcn;
      if (fragmented) { lcn = alloc(1); ++next; }  // (the gap stays free)
      else lcn = (i == 0) ? alloc(n) : runs[0].first + i;
      if (!fragmented && i > 0) continue;
      runs.push_back({static_cast<int64_t>(lcn), fragmented ? 1u : n});
      if (!fragmented) break;
    }
    for (uint64_t i = 0, c = 0; i < runs.size(); ++i)
      for (uint64_t k = 0; k < runs[i].second; ++k, ++c) {
        const uint64_t at = c * cb(); if (at >= data.size()) continue;
        memcpy(img.data() + (runs[i].first + k) * cb(), data.data() + at, std::min<uint64_t>(cb(), data.size() - at));
      }
    return runs;
  }

  uint64_t add_dir(uint64_t parent, const std::string& name) {
    const uint64_t n = new_record();
    recs[n].flags = 3;
    dir_names[n] = {parent, u16(name)};
    recs[n].add(recs[n].resident(0x10, "", si(0x10)));
    add_entry(parent, n, name, 3, true);
    return n;
  }
  std::map<uint64_t, std::pair<uint64_t, U16>> dir_names;

  void add_entry(uint64_t parent, uint64_t n, const std::string& name, uint8_t ns, bool directory, uint64_t size = 0, uint64_t alloc_size = 0) {
    Rec& r = recs[n];
    const Bytes key = fname(ref(parent), u16(name), ns, directory ? 0x10000000u : 0x20u, alloc_size, size);
    r.add(r.resident(0x30, "", key, true));
    dirs[parent].push_back({u16(name), ref(n), key});
  }

  // Adds a file (names are shown in the folder as `name`). Returns its record number.
  uint64_t add_file(uint64_t parent, const std::string& name, const Bytes& data, const Layout& l = Layout()) {
    const uint64_t n = new_record();
    Rec& r = recs[n];
    r.add(r.resident(0x10, "", si()));
    if (l.dos_name) {
      add_entry(parent, n, name, 1, false, data.size(), 0);
      add_entry(parent, n, "SHORT~1.TXT", 2, false, data.size(), 0);
    } else {
      add_entry(parent, n, name, 1, false, data.size(), 0);
    }
    attach_data(n, data, l);
    return n;
  }
  void attach_data(uint64_t n, const Bytes& data, const Layout& l) {
    Rec& r = recs[n];
    const uint64_t init = l.init == ~uint64_t{0} ? data.size() : l.init;
    if (l.compressed) {
      const uint64_t unit_bytes = uint64_t{cb()} * 16;
      RunSpec runs;
      uint64_t total_units = (data.size() + unit_bytes - 1) / unit_bytes;
      for (uint64_t u = 0; u < total_units; ++u) {
        Bytes chunk(data.begin() + u * unit_bytes, data.begin() + std::min<uint64_t>(data.size(), (u + 1) * unit_bytes));
        chunk.resize(unit_bytes, 0);
        bool all_zero = std::all_of(chunk.begin(), chunk.end(), [](uint8_t b) { return b == 0; });
        if (all_zero) { runs.push_back({-1, 16}); continue; }
        Bytes packed = lznt1_compress(chunk);
        uint64_t need = (packed.size() + cb() - 1) / cb();
        if (need >= 16) { Bytes raw = chunk; const uint64_t lcn = alloc(16); write(lcn, raw); runs.push_back({static_cast<int64_t>(lcn), 16}); continue; }
        const uint64_t lcn = alloc(need); packed.resize(need * cb(), 0); write(lcn, packed);
        runs.push_back({static_cast<int64_t>(lcn), need}); runs.push_back({-1, 16 - need});
      }
      r.add(r.nonres(0x80, "", runs, data.size(), data.size(), 0x0001, 4, 0, cb()));
      return;
    }
    if (l.sparse_middle) {  // first and last cluster stored, the clusters between are holes
      const uint64_t n_clusters = (data.size() + cb() - 1) / cb();
      RunSpec runs;
      const uint64_t a = alloc(1); write(a, Bytes(data.begin(), data.begin() + std::min<uint64_t>(cb(), data.size())));
      runs.push_back({static_cast<int64_t>(a), 1});
      if (n_clusters > 2) runs.push_back({-1, n_clusters - 2});
      if (n_clusters > 1) { const uint64_t z = alloc(1); Bytes tail(data.begin() + (n_clusters - 1) * cb(), data.end()); write(z, tail); runs.push_back({static_cast<int64_t>(z), 1}); }
      r.add(r.nonres(0x80, "", runs, data.size(), data.size(), 0x8000, 0, 0, cb()));
      return;
    }
    if (l.attr_list) {
      // two extents: the first half of the clusters in the base record, the rest in an extension record
      const uint64_t n_clusters = (data.size() + cb() - 1) / cb();
      RunSpec all = store(data, true);
      const size_t split = all.size() / 2;
      uint64_t first_clusters = 0; for (size_t i = 0; i < split; ++i) first_clusters += all[i].second;
      RunSpec a(all.begin(), all.begin() + split), b(all.begin() + split, all.end());
      const uint64_t ext = new_record();
      Rec& e = recs[ext]; e.base = ref(n); e.flags = 1;
      // base: SI and FILE_NAME are already there; $ATTRIBUTE_LIST goes in between, $DATA first extent after
      Bytes first = r.nonres(0x80, "", a, data.size(), data.size(), 0, 0, 0, cb());
      Bytes second = e.nonres(0x80, "", b, 0, 0, 0, 0, first_clusters, cb(), false);
      (void)n_clusters;
      struct Item { uint32_t type; uint64_t vcn; uint64_t rec; uint16_t id; };
      Bytes list;
      auto add_item = [&](uint32_t type, uint64_t vcn, uint64_t rec, uint16_t id) {
        Bytes it(0x20, 0); p32(it, 0, type); p16(it, 4, 0x20); it[7] = 0x1A; p64(it, 8, vcn); p64(it, 16, ref(rec)); p16(it, 24, id);
        list.insert(list.end(), it.begin(), it.end());
      };
      add_item(0x10, 0, n, 0);
      add_item(0x30, 0, n, 1);
      add_item(0x80, 0, n, 3);
      add_item(0x80, first_clusters, ext, 0);
      // rebuild: SI(id0), FILE_NAME(id1) stay; list id 2; data id 3
      Rec fresh = r; fresh.attrs.clear(); fresh.next_id = 0;
      Bytes si_attr = fresh.resident(0x10, "", si());
      Bytes fn_attr;
      for (const Bytes& at : r.attrs) if (g32(at.data()) == 0x30) fn_attr = at;
      p16(fn_attr, 14, 1); fresh.next_id = 2;
      Bytes la = fresh.resident(0x20, "", list);
      Bytes data_attr = first; p16(data_attr, 14, 3);
      std::vector<Bytes> keep;
      keep.push_back(si_attr);
      keep.push_back(la);
      for (const Bytes& at : r.attrs) if (g32(at.data()) == 0x30) keep.push_back(at);
      keep.push_back(data_attr);
      p64(data_attr, 0x28, n_clusters * cb());
      keep.back() = data_attr;
      r.attrs = keep; r.next_id = 4;
      p16(second, 14, 0); e.attrs.push_back(second); e.next_id = 1;
      return;
    }
    if (!l.force_nonresident && data.size() <= 600) { r.add(r.resident(0x80, "", data)); return; }
    RunSpec runs = store(data, l.fragmented);
    r.add(r.nonres(0x80, "", runs, data.size(), init, 0, 0, 0, cb()));
  }

  void hard_link(uint64_t parent, uint64_t n, const std::string& name, uint64_t size) {
    recs[n].links++;
    // inserted before the data attribute to keep the type order
    Rec& r = recs[n];
    const Bytes key = fname(ref(parent), u16(name), 1, 0x20, 0, size);
    Bytes a = r.resident(0x30, "", key, true);
    size_t at = 0; while (at < r.attrs.size() && g32(r.attrs[at].data()) <= 0x30) ++at;
    r.attrs.insert(r.attrs.begin() + at, a);
    dirs[parent].push_back({u16(name), ref(n), key});
  }

  // ---- folders ----
  static Bytes entry(uint64_t file_ref, const Bytes& key, bool has_child, uint64_t child, bool last) {
    const uint32_t len = a8(16 + key.size() + (has_child ? 8 : 0));
    Bytes e(len, 0); p64(e, 0, file_ref); p16(e, 8, len); p16(e, 10, key.size()); p16(e, 12, (has_child ? 1 : 0) | (last ? 2 : 0));
    if (!key.empty()) memcpy(e.data() + 16, key.data(), key.size());
    if (has_child) p64(e, len - 8, child);
    return e;
  }
  struct Item { Bytes key; uint64_t ref; uint64_t child = 0; bool has_child = false; uint32_t size() const { return a8(16 + key.size() + (has_child ? 8 : 0)); } };
  std::vector<std::pair<uint64_t, Bytes>> blocks_of(uint64_t dir) { return std::vector<std::pair<uint64_t, Bytes>>(); }

  struct BlockSet { std::vector<Bytes> blocks; };
  Bytes make_block(uint64_t vcn, const std::vector<Item>& items, bool terminator_child, uint64_t term_child, bool internal) {
    const uint32_t count = o.ibs / 512 + 1, eo = a8(0x28 + 2 * count) - 0x18;
    Bytes b(o.ibs, 0);
    memcpy(b.data(), "INDX", 4); p16(b, 4, 0x28); p16(b, 6, count); p64(b, 0x10, vcn);
    Bytes body;
    for (const Item& it : items) { Bytes e = entry(it.ref, it.key, it.has_child, it.child, false); body.insert(body.end(), e.begin(), e.end()); }
    Bytes t = entry(0, Bytes(), terminator_child, term_child, true); body.insert(body.end(), t.begin(), t.end());
    if (eo + body.size() > o.ibs - 0x18) { printf("index block overflows\n"); abort(); }
    p32(b, 0x18, eo); p32(b, 0x1C, eo + body.size()); p32(b, 0x20, o.ibs - 0x18); p32(b, 0x24, internal ? 1 : 0);
    memcpy(b.data() + 0x18 + eo, body.data(), body.size());
    p16(b, 0x28, 1);
    for (uint32_t i = 1; i < count; ++i) { p16(b, 0x28 + 2 * i, g16(&b[i * 512 - 2])); p16(b, i * 512 - 2, 1); }
    return b;
  }

  // Builds the folder attributes (root, allocation, bitmap) of a folder from its sorted entries: a B+tree packed level by level.
  void finish_dir(uint64_t n) {
    Rec& r = recs[n];
    std::vector<DirItem> list = dirs[n];
    std::sort(list.begin(), list.end(), [](const DirItem& a, const DirItem& b) { return name_less(a.name, b.name); });
    std::vector<Item> level;
    for (const DirItem& d : list) { Item it; it.key = d.key; it.ref = d.ref; level.push_back(it); }
    const uint32_t eo = a8(0x28 + 2 * (o.ibs / 512 + 1)) - 0x18, cap = o.ibs - 0x18 - eo;
    const uint32_t root_cap = o.rs - 0x38 - 0x80 - 0x80 - 0x30 - 0x50 - 0x28 - 0x20 - 8 - 0x20;
    auto total = [](const std::vector<Item>& v) { uint32_t s = 16; for (auto& i : v) s += i.size(); return s; };
    std::vector<Bytes> blocks;
    bool large = false, internal = false;
    uint64_t rightmost = 0;
    while (total(level) > root_cap) {
      large = true;
      std::vector<Item> seps;
      std::vector<std::vector<Item>> groups;
      std::vector<Item> cur;
      for (const Item& it : level) {
        if (total(cur) + it.size() + 24 > cap && !cur.empty()) { groups.push_back(cur); cur.clear(); seps.push_back(it); }
        else cur.push_back(it);
      }
      groups.push_back(cur);
      std::vector<uint64_t> vcns;
      for (size_t g = 0; g < groups.size(); ++g) {
        const uint64_t vcn = blocks.size() * o.ibs / unit();
        const uint64_t term = g + 1 < groups.size() ? seps[g].child : rightmost;
        blocks.push_back(make_block(vcn, groups[g], internal, term, internal));
        vcns.push_back(vcn);
      }
      std::vector<Item> next;
      for (size_t g = 0; g + 1 < groups.size(); ++g) { Item s = seps[g]; s.has_child = true; s.child = vcns[g]; next.push_back(s); }
      rightmost = vcns.back();
      internal = true;
      level = next;
    }
    Bytes value(0x20, 0);
    p32(value, 0, 0x30); p32(value, 4, 1); p32(value, 8, o.ibs); value[12] = o.ibs >= cb() ? o.ibs / cb() : o.ibs / 512;
    Bytes body;
    for (const Item& it : level) { Bytes e = entry(it.ref, it.key, it.has_child, it.child, false); body.insert(body.end(), e.begin(), e.end()); }
    Bytes t = entry(0, Bytes(), large, rightmost, true); body.insert(body.end(), t.begin(), t.end());
    p32(value, 0x10, 0x10); p32(value, 0x14, 0x10 + body.size()); p32(value, 0x18, 0x10 + body.size()); p32(value, 0x1C, large ? 1 : 0);
    value.insert(value.end(), body.begin(), body.end());
    r.add(r.resident(0x90, "$I30", value));
    if (large) {
      Bytes all; for (const Bytes& b : blocks) all.insert(all.end(), b.begin(), b.end());
      const uint64_t clusters = (all.size() + cb() - 1) / cb();
      const uint64_t lcn = alloc(clusters);
      write(lcn, all);
      r.add(r.nonres(0xA0, "$I30", {{static_cast<int64_t>(lcn), clusters}}, all.size(), all.size(), 0, 0, 0, cb()));
      Bytes bits(((blocks.size() + 7) / 8 + 7) & ~size_t{7}, 0);
      for (size_t i = 0; i < blocks.size(); ++i) bits[i / 8] |= 1 << (i % 8);
      r.add(r.resident(0xB0, "$I30", bits));
    }
  }

  void finalize() {
    // The root's own entry, the system files' data, then every folder.
    dirs[5].push_back({u16("."), ref(5), fname(ref(5), u16("."), 3, 0x10000000u)});
    recs[5].add(recs[5].resident(0x10, "", si(0x6)));
    recs[5].add(recs[5].resident(0x30, "", fname(ref(5), u16("."), 3, 0x10000000u), true));
    if (o.hiberfil) { Bytes h(8192, 0); memcpy(h.data(), "hibr", 4); add_file(5, "hiberfil.sys", h); }
    // $MFT
    {
      Rec& r = recs[0];
      r.add(r.nonres(0x80, "", {{static_cast<int64_t>(mft_lcn), mft_clusters}}, uint64_t{o.mft_records} * o.rs, uint64_t{o.mft_records} * o.rs, 0, 0, 0, cb()));
      mft_bitmap_lcn = alloc(1);
      r.add(r.nonres(0xB0, "", {{static_cast<int64_t>(mft_bitmap_lcn), 1}}, (o.mft_records + 7) / 8, (o.mft_records + 7) / 8, 0, 0, 0, cb()));
    }
    {
      Rec& r = recs[1];
      r.add(r.nonres(0x80, "", {{static_cast<int64_t>(mirror_lcn), (4 * o.rs + cb() - 1) / cb()}}, 4 * o.rs, 4 * o.rs, 0, 0, 0, cb()));
    }
    {
      Rec& r = recs[2];
      const uint64_t size = 64 * 1024, clusters = (size + cb() - 1) / cb(), lcn = alloc(clusters);
      Bytes log(clusters * cb(), 0xFF);
      if (o.log_mode) {
        memset(log.data(), 0, 4096);
        memcpy(log.data(), "RSTR", 4); p16(log, 4, 0x1E); p16(log, 6, 9); p32(log, 0x10, 4096); p32(log, 0x14, 4096); p16(log, 0x18, 0x30);
        p16(log, 0x30 + 8, 1); p16(log, 0x30 + 0xA, 0xFFFF); p16(log, 0x30 + 0xC, 0); p16(log, 0x30 + 0xE, o.log_mode == 1 ? 2 : 0);
      }
      write(lcn, log);
      r.add(r.nonres(0x80, "", {{static_cast<int64_t>(lcn), clusters}}, size, size, 0, 0, 0, cb()));
    }
    {
      Rec& r = recs[3];
      Bytes name; for (char16_t c : u16("TESTVOL")) { name.push_back(c); name.push_back(c >> 8); }
      r.add(r.resident(0x60, "", name));
      Bytes info(12, 0); info[8] = o.version_major; info[9] = 1; p16(info, 10, o.volume_flags);
      r.add(r.resident(0x70, "", info));
      r.add(r.resident(0x80, "", Bytes()));
    }
    recs[4].add(recs[4].resident(0x80, "", Bytes(32, 0)));
    recs[8].add(recs[8].resident(0x80, "", Bytes()));
    recs[9].add(recs[9].resident(0x80, "", Bytes()));
    recs[7].add(recs[7].nonres(0x80, "", {{0, 2}}, 8192, 8192, 0, 0, 0, cb()));
    {
      const uint64_t size = 128 * 1024, clusters = (size + cb() - 1) / cb(), lcn = alloc(clusters);
      Bytes up(clusters * cb(), 0);
      for (uint32_t c = 0; c < 65536; ++c) p16(up, 2 * c, upc(c));
      write(lcn, up);
      recs[10].add(recs[10].nonres(0x80, "", {{static_cast<int64_t>(lcn), clusters}}, size, size, 0, 0, 0, cb()));
    }
    for (uint64_t d = 5; d <= 11; ++d) if (d == 5 || d == 11) finish_dir(d);
    for (auto& d : dir_names) finish_dir(d.first);
    // $Bitmap last: it must describe everything allocated so far (including itself)
    {
      const uint64_t bytes = a8((o.clusters + 7) / 8), clusters = (bytes + cb() - 1) / cb(), lcn = alloc(clusters);
      Bytes bits(clusters * cb(), 0);
      for (uint64_t c = 0; c < o.clusters; ++c) if (used[c]) bits[c / 8] |= 1 << (c % 8);
      for (uint64_t c = o.clusters; c < bytes * 8; ++c) bits[c / 8] |= 1 << (c % 8);
      write(lcn, bits);
      recs[6].add(recs[6].nonres(0x80, "", {{static_cast<int64_t>(lcn), clusters}}, bytes, bytes, 0, 0, 0, cb()));
      for (uint64_t c = lcn; c < lcn + clusters; ++c) { bits[c / 8] |= 1 << (c % 8); }
      write(lcn, bits);
    }
    Bytes mbits(cb(), 0);
    for (uint64_t i = 0; i < recs.size(); ++i) if (recs[i].present) mbits[i / 8] |= 1 << (i % 8);
    write(mft_bitmap_lcn, mbits);
    for (uint64_t i = 0; i < recs.size(); ++i) {
      Bytes raw = recs[i].build(o.rs);
      memcpy(img.data() + mft_lcn * cb() + i * o.rs, raw.data(), o.rs);
      if (i < 4) memcpy(img.data() + mirror_lcn * cb() + i * o.rs, raw.data(), o.rs);
    }
    Bytes boot(512, 0);
    boot[0] = 0xEB; boot[1] = 0x52; boot[2] = 0x90; memcpy(&boot[3], "NTFS    ", 8);
    p16(boot, 0x0B, o.bps); boot[0x0D] = o.spc; p64(boot, 0x28, uint64_t{o.clusters} * o.spc - 1); p64(boot, 0x30, mft_lcn); p64(boot, 0x38, mirror_lcn);
    boot[0x40] = o.rs >= cb() ? o.rs / cb() : static_cast<uint8_t>(-static_cast<int>(__builtin_ctz(o.rs)));
    boot[0x44] = o.ibs >= cb() ? o.ibs / cb() : static_cast<uint8_t>(-static_cast<int>(__builtin_ctz(o.ibs)));
    boot[510] = 0x55; boot[511] = 0xAA;
    memcpy(img.data(), boot.data(), 512);
  }
  uint64_t mft_bitmap_lcn = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Helpers for the tests

static const char kEa[] = "\xC3\xA9" "a.txt", kUpperEa[] = "/\xC3\x89" "A.TXT", kEaPath[] = "/\xC3\xA9" "a.txt";

static std::string str_of(const FixedString<kMaxNameLength + 1>& s) { return std::string(s.c_str()); }

struct Mounted {
  TestAllocator alloc;
  MemDisk* disk = nullptr;
  UniquePointer<IFileSystem> fs;
  ~Mounted() { fs = UniquePointer<IFileSystem>(); delete disk; }
  Status mount(const Bytes& image, uint32_t block, bool writable, uint64_t lead = 0) {
    delete disk;
    disk = new MemDisk(image.data(), image.size(), block, lead, writable);
    return NtfsFileSystem::mount(alloc, *disk, lead, &fs);
  }
};

static std::vector<std::string> list_dir(IFileSystem& fs, const char* path, std::map<std::string, DirectoryEntry>* entries = nullptr) {
  std::vector<std::string> names;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path, &dir) != Status::Ok) { names.push_back("<open failed>"); return names; }
  DirectoryEntry e;
  Status s;
  int guard = 0;
  while ((s = dir->next(&e)) == Status::Ok && ++guard < 100000) { names.push_back(str_of(e.name)); if (entries) (*entries)[str_of(e.name)] = e; }
  if (s != Status::EndOfDirectory) names.push_back(std::string("<") + status_name(s) + ">");
  return names;
}

static bool g_verbose = false;
static bool read_all(IFileSystem& fs, const char* path, Bytes* out) {
  UniquePointer<IFile> f;
  Status s = fs.open_file(path, OpenMode::Read, &f);
  if (s != Status::Ok) { if (g_verbose) printf("  open %s: %s\n", path, status_name(s)); return false; }
  out->assign(f->size(), 0);
  uint64_t got = 0;
  if (!out->empty() && (s = f->read(0, ByteView(out->data(), out->size()), &got)) != Status::Ok) { if (g_verbose) printf("  read %s: %s\n", path, status_name(s)); return false; }
  return got == out->size();
}

static bool write_file(IFileSystem& fs, const std::string& path, const Bytes& data) {
  UniquePointer<IFile> f;
  if (fs.open_file(path.c_str(), OpenMode::CreateTruncate, &f) != Status::Ok) return false;
  uint64_t put = 0;
  if (!data.empty() && f->write(0, ConstByteView(data.data(), data.size()), &put) != Status::Ok) return false;
  return put == data.size();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The verifier: walks a volume image the way chkdsk would, with its own reader (nothing shared with the filesystem under test).

struct Fsck {
  const uint8_t* img;
  uint64_t clusters;
  uint32_t cb = 0, rs = 0, ibs = 0;
  uint64_t mft_lcn = 0;
  std::vector<std::string> errors;
  struct Run { int64_t lcn; uint64_t len; };
  struct A {
    uint32_t type = 0; bool nonres = false; U16 name; uint16_t flags = 0, id = 0;
    Bytes value; uint64_t lowest = 0, highest = 0, alloc = 0, size = 0, init = 0; std::vector<Run> runs; uint32_t shift = 0;
  };
  struct R { uint64_t number = 0; uint16_t flags = 0, links = 0, seq = 0; uint64_t base = 0; std::vector<A> attrs; };
  std::vector<Run> mft_runs; uint64_t mft_size = 0;

  Fsck(const uint8_t* image, uint64_t cluster_count) : img(image), clusters(cluster_count) {
    const int8_t r = static_cast<int8_t>(img[0x40]), b = static_cast<int8_t>(img[0x44]);
    cb = g16(img + 0x0B) * img[0x0D];
    rs = r > 0 ? r * cb : 1u << -r;
    ibs = b > 0 ? b * cb : 1u << -b;
    mft_lcn = g64(img + 0x30);
  }
  void err(const std::string& s) { if (errors.size() < 20) errors.push_back(s); }

  static inline bool bad_runs = false;
  static std::vector<Run> decode(const uint8_t* p, size_t n) {
    std::vector<Run> out; int64_t lcn = 0;
    for (size_t i = 0; i < n && p[i];) {
      const uint32_t lb = p[i] & 15, ob = p[i] >> 4; ++i;
      uint64_t len = 0; for (uint32_t k = 0; k < lb; ++k) len |= uint64_t{p[i + k]} << (8 * k);
      if (p[i + lb - 1] & 0x80) bad_runs = true;  // Windows and ntfs-3g read run lengths as signed numbers
      i += lb;
      if (ob == 0) { out.push_back({-1, len}); continue; }
      int64_t d = 0; for (uint32_t k = 0; k < ob; ++k) d |= int64_t{p[i + k]} << (8 * k);
      if (p[i + ob - 1] & 0x80) d |= -(int64_t{1} << (8 * ob)); i += ob;
      lcn += d; out.push_back({lcn, len});
    }
    return out;
  }
  Bytes read_runs(const std::vector<Run>& runs, uint64_t offset, uint64_t length) {
    Bytes out(length, 0);
    for (uint64_t done = 0; done < length;) {
      uint64_t pos = offset + done, vcn = pos / cb, base = 0; bool ok = false;
      for (const Run& r : runs) {
        if (vcn < base + r.len) {
          const uint64_t within = pos - base * cb, take = std::min<uint64_t>(length - done, r.len * cb - within);
          if (r.lcn >= 0) memcpy(out.data() + done, img + r.lcn * cb + within, take);
          done += take; ok = true; break;
        }
        base += r.len;
      }
      if (!ok) { err("read past runs"); return out; }
    }
    return out;
  }
  bool parse(const Bytes& rec, R* r) {
    if (memcmp(rec.data(), "FILE", 4) != 0) return false;
    Bytes b = rec;
    const uint32_t uo = g16(&b[4]), uc = g16(&b[6]);
    for (uint32_t i = 1; i < uc; ++i) {
      if (g16(&b[i * 512 - 2]) != g16(&b[uo])) { err("bad fixup in record"); return false; }
      p16(b, i * 512 - 2, g16(&b[uo + 2 * i]));
    }
    r->flags = g16(&b[0x16]); r->links = g16(&b[0x12]); r->seq = g16(&b[0x10]); r->base = g64(&b[0x20]);
    uint32_t off = g16(&b[0x14]);
    const uint32_t used = g32(&b[0x18]);
    if (used > rs) { err("record used size too big"); return false; }
    while (off + 8 <= used && g32(&b[off]) != 0xFFFFFFFF) {
      A a; a.type = g32(&b[off]); const uint32_t len = g32(&b[off + 4]);
      if (len < 0x18 || off + len > used) { err("bad attribute length"); return false; }
      a.nonres = b[off + 8]; const uint32_t nl = b[off + 9], no = g16(&b[off + 10]);
      a.flags = g16(&b[off + 12]); a.id = g16(&b[off + 14]);
      for (uint32_t i = 0; i < nl; ++i) a.name += static_cast<char16_t>(g16(&b[off + no + 2 * i]));
      if (!a.nonres) { const uint32_t vl = g32(&b[off + 16]), vo = g16(&b[off + 20]); a.value.assign(b.begin() + off + vo, b.begin() + off + vo + vl); }
      else {
        a.lowest = g64(&b[off + 16]); a.highest = g64(&b[off + 24]); a.shift = g16(&b[off + 34]);
        a.alloc = g64(&b[off + 40]); a.size = g64(&b[off + 48]); a.init = g64(&b[off + 56]);
        const uint32_t mo = g16(&b[off + 32]);
        a.runs = decode(&b[off + mo], len - mo);
      }
      r->attrs.push_back(a);
      off += len;
    }
    return true;
  }
  bool rec(uint64_t n, R* r) {
    if ((n + 1) * rs > mft_size) return false;
    Bytes raw = read_runs(mft_runs, n * rs, rs);
    r->number = n;
    return parse(raw, r);
  }
  const A* find(const R& r, uint32_t type, const char* name = "") {
    const U16 n = u16(name);
    for (const A& a : r.attrs) if (a.type == type && a.name == n && (!a.nonres || a.lowest == 0)) return &a;
    return nullptr;
  }
  Bytes stream_bytes(const R& r, const A* a) {
    if (!a) return Bytes();
    if (!a->nonres) return a->value;
    Bytes b = read_runs(a->runs, 0, (a->size + cb - 1) / cb * cb);
    b.resize(a->size);
    return b;
  }
  static bool bit(const Bytes& b, uint64_t i) { return i / 8 < b.size() && (b[i / 8] >> (i % 8)) & 1; }

  struct Name { uint64_t parent; U16 name; uint8_t ns; };
  static std::vector<Name> names_of(const R& r) {
    std::vector<Name> out;
    for (const A& a : r.attrs) if (a.type == 0x30 && !a.nonres && a.value.size() >= 0x42) {
      Name n; n.parent = g64(a.value.data()) & 0xFFFFFFFFFFFFull; n.ns = a.value[0x41];
      for (uint32_t i = 0; i < a.value[0x40]; ++i) n.name += static_cast<char16_t>(g16(&a.value[0x42 + 2 * i]));
      out.push_back(n);
    }
    return out;
  }

  // Entries of the folder in tree order; checks the tree's structure.
  std::vector<std::pair<uint64_t, Bytes>> index_keys;  // (record, key) of every entry seen
  void walk_dir(const R& d, std::vector<std::pair<U16, uint64_t>>* out, const A* alloc, std::set<uint64_t>* seen, const Bytes& entries, int depth) {
    if (depth > 12) { err("index too deep"); return; }
    uint32_t at = 0;
    for (;;) {
      if (at + 16 > entries.size()) { err("index entry runs off the node"); return; }
      const uint32_t len = g16(&entries[at + 8]), klen = g16(&entries[at + 10]), fl = g16(&entries[at + 12]);
      if (len < 16 || at + len > entries.size() || (len & 7)) { err("bad index entry length"); return; }
      if (fl & 1) {
        const uint64_t child = g64(&entries[at + len - 8]);
        if (!alloc) { err("sub-node without allocation"); return; }
        const uint32_t unit = cb <= ibs ? cb : 512;
        const uint64_t off = child * unit;
        if (off + ibs > alloc->size) { err("sub-node outside the allocation"); return; }
        if (!seen->insert(off / ibs).second) { err("index block reached twice"); return; }
        Bytes blk = read_runs(alloc->runs, off, ibs);
        if (memcmp(blk.data(), "INDX", 4) != 0) { err("block is not INDX"); return; }
        const uint32_t uo = g16(&blk[4]), uc = g16(&blk[6]);
        for (uint32_t i = 1; i < uc; ++i) { if (g16(&blk[i * 512 - 2]) != g16(&blk[uo])) { err("bad block fixup"); return; } p16(blk, i * 512 - 2, g16(&blk[uo + 2 * i])); }
        if (g64(&blk[0x10]) != child) { err("block vcn mismatch"); return; }
        const uint32_t eo = g32(&blk[0x18]), total = g32(&blk[0x1C]);
        if (0x18 + total > ibs) { err("block total too big"); return; }
        Bytes sub(blk.begin() + 0x18 + eo, blk.begin() + 0x18 + total);
        walk_dir(d, out, alloc, seen, sub, depth + 1);
      }
      if (fl & 2) {
        if (at + len != entries.size()) err("end entry is not last");
        return;
      }
      if (klen < 0x42) { err("short key"); return; }
      U16 name; for (uint32_t i = 0; i < entries[at + 16 + 0x40]; ++i) name += static_cast<char16_t>(g16(&entries[at + 16 + 0x42 + 2 * i]));
      out->push_back({name, g64(&entries[at]) & 0xFFFFFFFFFFFFull});
      index_keys.push_back({g64(&entries[at]) & 0xFFFFFFFFFFFFull, Bytes(entries.begin() + at + 16, entries.begin() + at + 16 + klen)});
      at += len;
    }
  }

  // Returns the errors found (empty: the volume is consistent).
  std::vector<std::string> run(bool allow_dirty_flag = false) {
    // MFT
    R r0; {
      Bytes raw(rs); memcpy(raw.data(), img + mft_lcn * cb, rs);
      if (!parse(raw, &r0)) { err("record 0 unreadable"); return errors; }
    }
    const A* md = find(r0, 0x80);
    if (!md || !md->nonres) { err("no $MFT data"); return errors; }
    mft_runs = md->runs; mft_size = md->size;
    if (md->init > md->size || md->size > md->alloc) err("$MFT sizes");
    const uint64_t nrec = mft_size / rs;
    std::vector<R> recs(nrec);
    std::vector<bool> in_use(nrec, false);
    std::vector<int64_t> owner(clusters, -1);
    for (uint64_t n = 0; n < nrec; ++n) {
      Bytes raw = read_runs(mft_runs, n * rs, rs);
      R r; r.number = n;
      if (memcmp(raw.data(), "FILE", 4) != 0) continue;
      if (!parse(raw, &r)) continue;
      recs[n] = r;
      if (!(r.flags & 1)) continue;
      in_use[n] = true;
      for (const A& a : r.attrs) {
        if (!a.nonres) continue;
        uint64_t total = 0;
        for (const Run& run : a.runs) {
          total += run.len;
          if (run.lcn < 0) continue;
          for (uint64_t c = 0; c < run.len; ++c) {
            const uint64_t lcn = run.lcn + c;
            if (lcn >= clusters) { err("run beyond the volume in record " + std::to_string(n)); break; }
            if (owner[lcn] >= 0) err("cluster " + std::to_string(lcn) + " used by records " + std::to_string(owner[lcn]) + " and " + std::to_string(n));
            owner[lcn] = n;
          }
        }
        if (a.lowest == 0 && a.highest + 1 != total && !find(r, 0x20)) err("highest vcn does not match the runs in record " + std::to_string(n));
        if (a.lowest == 0 && !find(r, 0x20) && (a.alloc != total * cb)) err("allocated size does not match the runs in record " + std::to_string(n) + " type " + std::to_string(a.type));
        if (a.lowest == 0 && (a.init > a.size || a.size > a.alloc)) err("sizes out of order in record " + std::to_string(n));
      }
    }
    // The cluster bitmap
    {
      const A* bm = find(recs[6], 0x80);
      if (!bm) { err("no $Bitmap"); return errors; }
      Bytes bits = stream_bytes(recs[6], bm);
      int shown = 0;
      for (uint64_t c = 0; c < clusters; ++c) {
        const bool set = bit(bits, c), owned = owner[c] >= 0;
        if (set != owned && shown++ < 5) err("cluster " + std::to_string(c) + (set ? " is marked used but nobody owns it" : " is owned but marked free"));
      }
    }
    // The MFT bitmap
    {
      const A* mb = find(recs[0], 0xB0);
      Bytes bits = stream_bytes(recs[0], mb);
      for (uint64_t n = 0; n < nrec; ++n) if (bit(bits, n) != in_use[n]) { err("MFT bitmap disagrees with record " + std::to_string(n)); break; }
    }
    // Folders and names
    std::map<uint64_t, std::vector<std::pair<U16, uint64_t>>> listing;
    for (uint64_t n = 0; n < nrec; ++n) {
      if (!in_use[n] || !(recs[n].flags & 2)) continue;
      const R& d = recs[n];
      const A* root = find(d, 0x90, "$I30");
      if (!root || root->nonres || root->value.size() < 0x20) { err("folder without index root: " + std::to_string(n)); continue; }
      const A* alloc = find(d, 0xA0, "$I30");
      const A* ibm = find(d, 0xB0, "$I30");
      const uint32_t eo = g32(&root->value[0x10]), total = g32(&root->value[0x14]);
      Bytes entries(root->value.begin() + 0x10 + eo, root->value.begin() + 0x10 + total);
      std::set<uint64_t> seen;
      std::vector<std::pair<U16, uint64_t>> out;
      walk_dir(d, &out, alloc, &seen, entries, 0);
      for (size_t i = 1; i < out.size(); ++i) if (!name_less(out[i - 1].first, out[i].first)) err("folder " + std::to_string(n) + " out of order");
      if (alloc) {
        Bytes bits = stream_bytes(d, ibm);
        const uint64_t blocks = alloc->size / ibs;
        uint64_t marked = 0;
        for (uint64_t b = 0; b < blocks; ++b) if (bit(bits, b)) ++marked;
        if (marked != seen.size()) err("folder " + std::to_string(n) + ": " + std::to_string(marked) + " index blocks marked, " + std::to_string(seen.size()) + " reachable");
        for (uint64_t b : seen) if (!bit(bits, b)) err("folder " + std::to_string(n) + ": reachable block not marked");
      }
      listing[n] = out;
    }
    for (auto& k : index_keys) {
      if (k.first < 24 || k.first >= nrec) continue;
      bool same = false;
      for (const A& a : recs[k.first].attrs) if (a.type == 0x30 && !a.nonres && a.value == k.second) same = true;
      if (!same) err("folder entry for record " + std::to_string(k.first) + " differs from its $FILE_NAME");
    }
    std::set<uint64_t> reached;
    for (auto& l : listing) {
      for (auto& e : l.second) {
        if (e.second >= nrec || !in_use[e.second]) { err("entry to a record not in use"); continue; }
        reached.insert(e.second);
        bool found = false;
        for (const Name& nm : names_of(recs[e.second])) if (nm.parent == l.first && nm.name == e.first) found = true;
        if (!found) err("entry without matching $FILE_NAME, record " + std::to_string(e.second));
      }
    }
    for (uint64_t n = 24; n < nrec; ++n) {
      if (!in_use[n] || recs[n].base) continue;
      if (!reached.count(n)) err("record " + std::to_string(n) + " is in use but in no folder");
      uint32_t links = 0;
      for (const Name& nm : names_of(recs[n])) {
        if (nm.ns != 2) ++links;
        bool found = false;
        auto it = listing.find(nm.parent);
        if (it != listing.end()) for (auto& e : it->second) if (e.second == n && e.first == nm.name) found = true;
        if (!found) err("name of record " + std::to_string(n) + " missing in its folder");
      }
      if (!(recs[n].flags & 2)) {
        const A* data = find(recs[n], 0x80);
        const uint64_t size = data ? (data->nonres ? data->size : data->value.size()) : 0;
        for (const A& a : recs[n].attrs) if (a.type == 0x30 && !a.nonres && a.value.size() >= 0x42 && g64(&a.value[0x30]) != size)
          err("record " + std::to_string(n) + ": $FILE_NAME says " + std::to_string(g64(&a.value[0x30])) + " bytes, data has " + std::to_string(size));
      }
      if (links != recs[n].links) err("link count of record " + std::to_string(n) + " is " + std::to_string(recs[n].links) + ", names: " + std::to_string(links));
      if (recs[n].links == 0) err("zero links in record " + std::to_string(n));
    }
    // $Volume must not stay dirty unless asked
    if (!allow_dirty_flag) {
      const A* vi = find(recs[3], 0x70);
      if (!vi || vi->value.size() < 12) err("no volume information");
      else if (g16(&vi->value[10]) & 1) err("volume still marked dirty");
    }
    // the mirror
    for (int n = 0; n < 4; ++n) {
      const uint64_t mirror = g64(img + 0x38);
      Bytes live = read_runs(mft_runs, n * rs, rs);
      if (memcmp(live.data(), img + mirror * cb + n * rs, rs) != 0) err("$MFTMirr record " + std::to_string(n) + " differs");
    }
    if (bad_runs) err("a run length has its top bit set");
    return errors;
  }
};

static void expect_clean(const MemDisk& disk, uint64_t clusters, int line, bool allow_dirty = false) {
  Fsck f(disk.data, clusters);
  std::vector<std::string> e = f.run(allow_dirty);
  if (!e.empty()) {
    printf("FAILED line %d: fsck found %zu problems:\n", line, e.size());
    for (size_t i = 0; i < e.size() && i < 8; ++i) printf("   %s\n", e[i].c_str());
    ++g_failed;
  }
}
#define EXPECT_CLEAN(disk, clusters) expect_clean(disk, clusters, __LINE__)
#define EXPECT_CLEAN_DIRTY(disk, clusters) expect_clean(disk, clusters, __LINE__, true)

// ---------------------------------------------------------------------------------------------------------------------------------
// A populated volume

struct Standard {
  Bytes image;
  Opts opts;
  std::map<std::string, Bytes> files;       // path -> content
  std::vector<std::string> root_names;      // in listing order
  uint64_t readme_record = 0;
};

static Bytes compressible(uint64_t size, uint32_t seed) {
  Bytes v(size);
  const char* words[] = {"The quick brown fox ", "jumps over the lazy dog. ", "NTFS stores runs of clusters. ", "0123456789 "};
  uint64_t at = 0; uint32_t w = seed;
  while (at < size) { const char* s = words[w++ % 4]; for (; *s && at < size; ++s) v[at++] = *s; }
  return v;
}

static Standard make_standard(const Opts& given, uint32_t many = 300, uint32_t spare_records = 40) {
  Standard s;
  Opts options = given;
  if (options.mft_records < many + 40 + spare_records) options.mft_records = (many + 40 + spare_records + 7) & ~7u;  // (a whole number of bitmap bytes)
  s.opts = options;
  Vol v(options);
  auto add = [&](uint64_t parent, const std::string& dir, const std::string& name, Bytes data, Layout l = Layout()) {
    v.add_file(parent, name, data, l);
    s.files[dir + "/" + name] = data;
  };
  add(5, "", "readme.txt", make_data(100, 1));
  s.readme_record = v.user_next;
  add(5, "", "Big.bin", make_data(20000, 2), [] { Layout l; l.force_nonresident = true; return l; }());
  add(5, "", "frag.bin", make_data(30000, 3), [] { Layout l; l.fragmented = true; return l; }());
  {
    Bytes d = make_data(6 * v.cb(), 4);
    Layout l; l.sparse_middle = true;
    std::fill(d.begin() + v.cb(), d.begin() + 5 * v.cb(), 0);
    add(5, "", "sparse.bin", d, l);
  }
  if (v.cb() <= 4096) {  // (Windows compresses only on clusters up to 4 KiB)
    const uint64_t unit = uint64_t{v.cb()} * 16;
    Bytes d(3 * unit + 5000, 0);
    Bytes text = compressible(unit, 0), rnd = make_data(unit, 9);
    for (uint64_t i = 0; i < unit; ++i) rnd[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
    std::copy(text.begin(), text.end(), d.begin());
    std::copy(rnd.begin(), rnd.end(), d.begin() + 2 * unit);
    Bytes tail = compressible(5000, 1);
    std::copy(tail.begin(), tail.end(), d.begin() + 3 * unit);
    Layout l; l.compressed = true;
    add(5, "", "comp.txt", d, l);
  }
  add(5, "", "split.bin", make_data(std::max<uint64_t>(40000, 4 * v.cb()), 5), [] { Layout l; l.attr_list = true; return l; }());
  {
    Bytes d = make_data(20000, 6);
    Layout l; l.force_nonresident = true; l.init = 5000;
    Bytes shown = d; std::fill(shown.begin() + 5000, shown.end(), 0);
    v.add_file(5, "init.bin", d, l);
    s.files["/init.bin"] = shown;
  }
  add(5, "", kEa, make_data(10, 7));
  {
    Layout l; l.dos_name = true;
    add(5, "", "long name here.txt", make_data(30, 8), l);
  }
  add(5, "", "empty.txt", Bytes());
  const uint64_t docs = v.add_dir(5, "docs");
  add(docs, "/docs", "a.txt", make_data(50, 10));
  add(docs, "/docs", "B.txt", make_data(900, 11));
  add(docs, "/docs", "c.txt", make_data(5000, 12));
  const uint64_t sub = v.add_dir(docs, "sub");
  add(sub, "/docs/sub", "deep.txt", make_data(77, 13));
  v.hard_link(docs, s.readme_record, "link.txt", 100);
  s.files["/docs/link.txt"] = s.files["/readme.txt"];
  const uint64_t empty_dir = v.add_dir(5, "emptydir");
  (void)empty_dir;
  const uint64_t big = v.add_dir(5, "many");
  for (uint32_t i = 0; i < many; ++i) {
    char name[40]; snprintf(name, sizeof name, "file_%04u.dat", i);
    add(big, "/many", name, make_data(1 + i % 120, i));
  }
  v.finalize();
  s.image = v.img;
  std::vector<U16> names;
  for (const DirItem& d : v.dirs[5]) {
    const U16 n = d.name;
    if (n == u"." || n[0] == '$') continue;
    bool dos = false;
    for (const Bytes& k : {d.key}) dos = k[0x41] == 2;
    if (!dos) names.push_back(n);
  }
  std::sort(names.begin(), names.end(), name_less);
  for (const U16& n : names) {
    std::string out;
    for (char16_t c : n) { if (c < 0x80) out += static_cast<char>(c); else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); } }
    s.root_names.push_back(out);
  }
  return s;
}

static void check_standard_reads(IFileSystem& fs, const Standard& s) {
  CHECK(list_dir(fs, "/") == s.root_names);
  for (const auto& f : s.files) {
    Bytes got;
    if (!read_all(fs, f.first.c_str(), &got) || got != f.second) {
      size_t at = 0; while (at < got.size() && at < f.second.size() && got[at] == f.second[at]) ++at;
      printf("FAILED: content of %s (%zu bytes, want %zu; first difference at %zu)\n", f.first.c_str(), got.size(), f.second.size(), at); ++g_failed;
    }
    DirectoryEntry e;
    CHECK_STATUS(fs.stat(f.first.c_str(), &e), Status::Ok);
    CHECK(e.type == NodeType::File && e.size == f.second.size());
  }
}

static void test_read(uint32_t device_block, uint64_t lead, const Opts& opts) {
  Standard s = make_standard(opts);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, device_block, false, lead), Status::Ok);
  if (!m.fs) return;
  FileSystemInfo info = m.fs->info();
  CHECK(info.type_name.view().equals("NTFS"));
  CHECK(info.label.view().equals("TESTVOL"));
  CHECK(info.read_only);
  CHECK(info.total_bytes == (opts.clusters - 1) * opts.bps * opts.spc);  // (the last sector holds the backup boot sector)
  check_standard_reads(*m.fs, s);
  // case insensitive names, through the volume's own table
  Bytes got;
  CHECK(read_all(*m.fs, "/README.TXT", &got) && got == s.files["/readme.txt"]);
  CHECK(read_all(*m.fs, "/DOCS/B.TXT", &got) && got == s.files["/docs/B.txt"]);
  CHECK(read_all(*m.fs, "/docs/b.txt", &got) && got == s.files["/docs/B.txt"]);
  CHECK(read_all(*m.fs, kUpperEa, &got) && got == s.files[kEaPath]);
  CHECK(read_all(*m.fs, "/docs//sub/./deep.txt", &got) && got == s.files["/docs/sub/deep.txt"]);
  CHECK(read_all(*m.fs, "/long name here.txt", &got));
  CHECK(read_all(*m.fs, "/SHORT~1.TXT", &got));  // the 8.3 alias resolves
  std::vector<std::string> root = list_dir(*m.fs, "/");
  CHECK(std::find(root.begin(), root.end(), "SHORT~1.TXT") == root.end());  // but is not listed
  CHECK(std::find(root.begin(), root.end(), "$MFT") == root.end());
  CHECK(std::find(root.begin(), root.end(), ".") == root.end());
  DirectoryEntry e;
  CHECK_STATUS(m.fs->stat("/$MFT", &e), Status::NotFound);
  CHECK_STATUS(m.fs->stat("/$Bitmap", &e), Status::NotFound);
  CHECK_STATUS(m.fs->stat("/", &e), Status::Ok);
  CHECK(e.type == NodeType::Directory);
  CHECK_STATUS(m.fs->stat("/docs", &e), Status::Ok);
  CHECK(e.type == NodeType::Directory);
  CHECK_STATUS(m.fs->stat("/nothing", &e), Status::NotFound);
  CHECK_STATUS(m.fs->stat("/readme.txt/x", &e), Status::NotADirectory);
  CHECK_STATUS(m.fs->stat("/docs/../x", &e), Status::InvalidPath);
  UniquePointer<IFile> file;
  CHECK_STATUS(m.fs->open_file("/docs", OpenMode::Read, &file), Status::IsADirectory);
  UniquePointer<IDirectory> dir;
  CHECK_STATUS(m.fs->open_directory("/readme.txt", &dir), Status::NotADirectory);
  CHECK_STATUS(m.fs->open_file("/new.txt", OpenMode::CreateOrOpen, &file), Status::ReadOnly);
  CHECK_STATUS(m.fs->open_file("/readme.txt", OpenMode::ReadWrite, &file), Status::ReadOnly);
  CHECK_STATUS(m.fs->create_directory("/x"), Status::ReadOnly);
  CHECK_STATUS(m.fs->remove("/readme.txt"), Status::ReadOnly);
  CHECK_STATUS(m.fs->rename("/readme.txt", "/r2.txt"), Status::ReadOnly);
  // partial reads
  CHECK_STATUS(m.fs->open_file("/frag.bin", OpenMode::Read, &file), Status::Ok);
  Bytes part(5000);
  uint64_t n = 0;
  CHECK_STATUS(file->read(12345, ByteView(part.data(), part.size()), &n), Status::Ok);
  CHECK(n == 5000 && memcmp(part.data(), s.files["/frag.bin"].data() + 12345, 5000) == 0);
  CHECK_STATUS(file->read(28000, ByteView(part.data(), part.size()), &n), Status::Ok);
  CHECK(n == 2000);
  CHECK_STATUS(file->read(30000, ByteView(part.data(), part.size()), &n), Status::Ok);
  CHECK(n == 0);
  CHECK_STATUS(file->read(99999, ByteView(part.data(), part.size()), &n), Status::Ok);
  CHECK(n == 0);
  // directories: rewind, and the same order twice
  CHECK_STATUS(m.fs->open_directory("/many", &dir), Status::Ok);
  std::vector<std::string> first;
  DirectoryEntry de;
  while (dir->next(&de) == Status::Ok) first.push_back(str_of(de.name));
  CHECK(first.size() == 300);
  CHECK(std::is_sorted(first.begin(), first.end()));
  CHECK_STATUS(dir->next(&de), Status::EndOfDirectory);
  dir->rewind();
  CHECK_STATUS(dir->next(&de), Status::Ok);
  CHECK(str_of(de.name) == first[0]);
  // handles must not outlive the filesystem; destroy them before it
  dir = UniquePointer<IDirectory>();
  file = UniquePointer<IFile>();
  CHECK(m.alloc.live > 0);
  m.fs = UniquePointer<IFileSystem>();
  CHECK(m.alloc.live == 0);
}

static void test_big_directory() {
  Opts o; o.ibs = 1024; o.clusters = 16384; o.mft_records = 2300; o.rs = 1024;
  Standard s = make_standard(o, 2000);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, false), Status::Ok);
  if (!m.fs) return;
  std::vector<std::string> many = list_dir(*m.fs, "/many");
  CHECK(many.size() == 2000 && std::is_sorted(many.begin(), many.end()));
  Bytes got;
  for (uint32_t i = 0; i < 2000; i += 37) {
    char name[60]; snprintf(name, sizeof name, "/many/file_%04u.dat", i);
    CHECK(read_all(*m.fs, name, &got) && got == s.files[name]);
  }
  EXPECT_CLEAN_DIRTY(*m.disk, o.clusters);
  // other geometries: 512 byte clusters, 64 KiB clusters, 4 KiB records, 2 KiB blocks
  for (int variant = 0; variant < 4; ++variant) {
    if (g_verbose) printf("variant %d\n", variant);
    Opts v;
    if (variant == 0) { v.spc = 1; v.clusters = 32768; v.ibs = 4096; }
    if (variant == 1) { v.spc = 128; v.clusters = 512; v.ibs = 4096; }
    if (variant == 2) { v.rs = 4096; v.clusters = 4096; v.ibs = 2048; v.mft_records = 64; }
    if (variant == 3) { v.bps = 4096; v.spc = 1; v.clusters = 4096; v.ibs = 4096; }
    Standard t = make_standard(v);
    Mounted w;
    CHECK_STATUS(w.mount(t.image, v.bps >= 4096 ? 4096 : 512, false), Status::Ok);
    if (w.fs) check_standard_reads(*w.fs, t);
  }
}

static void test_mount_rules() {
  Opts o;
  Standard good = make_standard(o, 10);
  Mounted m;
  CHECK_STATUS(m.mount(good.image, 512, true), Status::Ok);
  CHECK(m.fs && !m.fs->info().read_only);
  m.fs = UniquePointer<IFileSystem>();
  // a read-only device
  CHECK_STATUS(m.mount(good.image, 512, false), Status::Ok);
  CHECK(m.fs && m.fs->info().read_only);
  m.fs = UniquePointer<IFileSystem>();
  struct Case { const char* name; Opts opts; };
  Case cases[5];
  cases[0] = {"dirty flag", o}; cases[0].opts.volume_flags = 1;
  cases[1] = {"dirty log", o}; cases[1].opts.log_mode = 2;
  cases[2] = {"hibernated", o}; cases[2].opts.hiberfil = true;
  cases[3] = {"old version", o}; cases[3].opts.version_major = 1;
  cases[4] = {"pending upgrade", o}; cases[4].opts.volume_flags = 0x8;
  for (const Case& c : cases) {
    Standard s = make_standard(c.opts, 10);
    Mounted w;
    CHECK_STATUS(w.mount(s.image, 512, true), Status::Ok);
    if (!w.fs) continue;
    if (!w.fs->info().read_only) { printf("FAILED: %s must mount read only\n", c.name); ++g_failed; }
    CHECK_STATUS(w.fs->create_directory("/x"), Status::ReadOnly);
    UniquePointer<IFile> f;
    CHECK_STATUS(w.fs->open_file("/readme.txt", OpenMode::Read, &f), Status::Ok);
  }
  Opts clean_log = o; clean_log.log_mode = 1;
  Standard s = make_standard(clean_log, 10);
  Mounted w;
  CHECK_STATUS(w.mount(s.image, 512, true), Status::Ok);
  CHECK(w.fs && !w.fs->info().read_only);
  // not an NTFS volume
  Bytes junk(1 << 20, 0x5A);
  CHECK_STATUS(w.mount(junk, 512, true), Status::Corrupt);
  Bytes zeros(1 << 20, 0);
  CHECK_STATUS(w.mount(zeros, 512, true), Status::Corrupt);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Writing

struct Model {
  std::map<std::string, Bytes> files;
  std::set<std::string> dirs;
};

static void walk_tree(IFileSystem& fs, const std::string& path, std::map<std::string, Bytes>* files, std::set<std::string>* dirs, int depth = 0) {
  if (depth > 20) return;
  std::map<std::string, DirectoryEntry> entries;
  std::vector<std::string> names = list_dir(fs, path.empty() ? "/" : path.c_str(), &entries);
  for (const std::string& n : names) {
    if (n[0] == '<') { printf("  listing %s: %s\n", path.c_str(), n.c_str()); ++g_failed; continue; }
    const std::string full = path + "/" + n;
    if (entries[n].type == NodeType::Directory) { dirs->insert(full); walk_tree(fs, full, files, dirs, depth + 1); }
    else { Bytes b; if (!read_all(fs, full.c_str(), &b)) { printf("  cannot read %s\n", full.c_str()); ++g_failed; } (*files)[full] = b; }
  }
}

static void expect_model(IFileSystem& fs, const Model& m, int line) {
  std::map<std::string, Bytes> files;
  std::set<std::string> dirs;
  walk_tree(fs, "", &files, &dirs);
  bool ok = true;
  for (const auto& f : m.files) {
    auto it = files.find(f.first);
    if (it == files.end()) { printf("  missing file %s\n", f.first.c_str()); ok = false; }
    else if (it->second != f.second) { printf("  wrong content in %s (%zu bytes, want %zu)\n", f.first.c_str(), it->second.size(), f.second.size()); ok = false; }
  }
  for (const auto& f : files) if (!m.files.count(f.first)) { printf("  unexpected file %s\n", f.first.c_str()); ok = false; }
  for (const std::string& d : m.dirs) if (!dirs.count(d)) { printf("  missing directory %s\n", d.c_str()); ok = false; }
  for (const std::string& d : dirs) if (!m.dirs.count(d)) { printf("  unexpected directory %s\n", d.c_str()); ok = false; }
  if (!ok) { printf("FAILED line %d: tree differs from the model\n", line); ++g_failed; }
}
#define EXPECT_MODEL(fs, m) expect_model(fs, m, __LINE__)

static Model model_of(const Standard& s) {
  Model m;
  m.files = s.files;
  m.dirs = {"/docs", "/docs/sub", "/emptydir", "/many"};
  return m;
}

// The image on the device now, as a fresh volume (what a reboot would see).
static void remount_check(MemDisk& disk, uint32_t block, const Model& model, uint64_t clusters, int line) {
  Bytes copy(disk.data, disk.data + disk.blocks * disk.block_size);
  Mounted m;
  if (m.mount(copy, block, false) != Status::Ok) { printf("FAILED line %d: remount\n", line); ++g_failed; return; }
  expect_model(*m.fs, model, line);
}

static Mounted* g_unused = nullptr;

static void test_times() {
  Opts o;
  Standard s = make_standard(o, 0);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  if (m.fs) filetimes::check_stamps(*m.fs, "ntfs");
}

static void test_write_files() {
  Opts o;
  Standard s = make_standard(o, 40);
  Model model = model_of(s);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  IFileSystem& fs = *m.fs;
  CHECK(!fs.info().read_only);
  const uint64_t free_before = fs.info().free_bytes;
  CHECK(free_before > 0);

  // create, then grow a resident file until it has to move out of its record
  Bytes a = make_data(200, 21);
  CHECK(write_file(fs, "/new.txt", a));
  model.files["/new.txt"] = a;
  EXPECT_MODEL(fs, model);
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/new.txt", OpenMode::ReadWrite, &f), Status::Ok);
    uint64_t put = 0;
    Bytes more = make_data(3000, 22);
    CHECK_STATUS(f->write(200, ConstByteView(more.data(), more.size()), &put), Status::Ok);
    CHECK(put == 3000 && f->size() == 3200);
    a.insert(a.end(), more.begin(), more.end());
    model.files["/new.txt"] = a;
    // a second handle sees the new size
    UniquePointer<IFile> g;
    CHECK_STATUS(fs.open_file("/new.txt", OpenMode::Read, &g), Status::Ok);
    CHECK(g->size() == 3200);
    Bytes more2 = make_data(100000, 23);
    CHECK_STATUS(f->write(3200, ConstByteView(more2.data(), more2.size()), &put), Status::Ok);
    a.insert(a.end(), more2.begin(), more2.end());
    model.files["/new.txt"] = a;
    CHECK(g->size() == 103200);
    CHECK_STATUS(f->flush(), Status::Ok);
  }
  EXPECT_MODEL(fs, model);
  // a gap reads as zeros
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/gap.bin", OpenMode::CreateOrOpen, &f), Status::Ok);
    uint64_t put = 0;
    Bytes tail = make_data(10, 24);
    CHECK_STATUS(f->write(10000, ConstByteView(tail.data(), tail.size()), &put), Status::Ok);
    Bytes want(10000, 0); want.insert(want.end(), tail.begin(), tail.end());
    model.files["/gap.bin"] = want;
  }
  // cutting and growing
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/new.txt", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(f->resize(5), Status::Ok);
    a.resize(5); model.files["/new.txt"] = a;
    CHECK_STATUS(f->resize(70000), Status::Ok);
    a.resize(70000, 0); model.files["/new.txt"] = a;
    CHECK_STATUS(f->resize(300), Status::Ok);
    a.resize(300); model.files["/new.txt"] = a;
    CHECK_STATUS(f->resize(0), Status::Ok);
    a.clear(); model.files["/new.txt"] = a;
    Bytes again = make_data(9000, 25);
    uint64_t put = 0;
    CHECK_STATUS(f->write(0, ConstByteView(again.data(), again.size()), &put), Status::Ok);
    model.files["/new.txt"] = again;
  }
  EXPECT_MODEL(fs, model);
  // overwriting inside existing files of every layout
  struct Patch { const char* path; uint64_t at, len; uint32_t seed; };
  const Patch patches[] = {{"/Big.bin", 5000, 100, 31}, {"/Big.bin", 19950, 50, 32}, {"/frag.bin", 4090, 20, 33}, {"/frag.bin", 0, 30000, 34},
                           {"/split.bin", 20000, 3000, 35}, {"/readme.txt", 10, 50, 36}, {"/docs/c.txt", 4000, 1000, 37}};
  for (const Patch& p : patches) {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file(p.path, OpenMode::ReadWrite, &f), Status::Ok);
    Bytes d = make_data(p.len, p.seed);
    uint64_t put = 0;
    CHECK_STATUS(f->write(p.at, ConstByteView(d.data(), d.size()), &put), Status::Ok);
    memcpy(model.files[p.path].data() + p.at, d.data(), d.size());
  }
  model.files["/docs/link.txt"] = model.files["/readme.txt"];
  EXPECT_MODEL(fs, model);
  // growing a fragmented file and a file whose initialized size is short
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/frag.bin", OpenMode::ReadWrite, &f), Status::Ok);
    Bytes d = make_data(50000, 38);
    uint64_t put = 0;
    CHECK_STATUS(f->write(30000, ConstByteView(d.data(), d.size()), &put), Status::Ok);
    model.files["/frag.bin"].insert(model.files["/frag.bin"].end(), d.begin(), d.end());
    CHECK_STATUS(fs.open_file("/init.bin", OpenMode::ReadWrite, &f), Status::Ok);
    Bytes e = make_data(10, 39);
    CHECK_STATUS(f->write(9000, ConstByteView(e.data(), e.size()), &put), Status::Ok);
    memcpy(model.files["/init.bin"].data() + 9000, e.data(), 10);
    CHECK_STATUS(f->resize(30000), Status::Ok);
    model.files["/init.bin"].resize(30000, 0);
    CHECK_STATUS(f->write(25000, ConstByteView(e.data(), e.size()), &put), Status::Ok);
    memcpy(model.files["/init.bin"].data() + 25000, e.data(), 10);
  }
  // writing into a hole of a sparse file
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/sparse.bin", OpenMode::ReadWrite, &f), Status::Ok);
    Bytes d = make_data(6000, 40);
    uint64_t put = 0;
    CHECK_STATUS(f->write(2 * 4096 + 100, ConstByteView(d.data(), d.size()), &put), Status::Ok);
    memcpy(model.files["/sparse.bin"].data() + 2 * 4096 + 100, d.data(), d.size());
  }
  EXPECT_MODEL(fs, model);
  // what is not written: compressed files and a file whose data is in an extension record
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/comp.txt", OpenMode::ReadWrite, &f), Status::Unsupported);
    CHECK_STATUS(fs.open_file("/split.bin", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(f->resize(100000), Status::Unsupported);
    uint64_t put = 0;
    Bytes d(10, 1);
    CHECK_STATUS(f->write(40000, ConstByteView(d.data(), d.size()), &put), Status::Unsupported);
  }
  EXPECT_MODEL(fs, model);

  // names
  UniquePointer<IFile> f;
  CHECK_STATUS(fs.open_file("/bad:name", OpenMode::CreateOrOpen, &f), Status::InvalidPath);
  CHECK_STATUS(fs.open_file("/trailing.", OpenMode::CreateOrOpen, &f), Status::InvalidPath);
  CHECK_STATUS(fs.open_file("/what?", OpenMode::CreateOrOpen, &f), Status::InvalidPath);
  CHECK_STATUS(fs.open_file(("/" + std::string(256, 'x')).c_str(), OpenMode::CreateOrOpen, &f), Status::InvalidPath);
  CHECK_STATUS(fs.open_file(("/" + std::string(255, 'x')).c_str(), OpenMode::CreateOrOpen, &f), Status::Ok);
  model.files["/" + std::string(255, 'x')] = Bytes();
  f = UniquePointer<IFile>();
  CHECK_STATUS(fs.open_file("/nodir/file", OpenMode::CreateOrOpen, &f), Status::NotFound);
  CHECK_STATUS(fs.open_file("/readme.txt/file", OpenMode::CreateOrOpen, &f), Status::NotADirectory);
  CHECK_STATUS(fs.open_file("/README.TXT", OpenMode::CreateOrOpen, &f), Status::Ok);  // (the same file, whatever the case)
  CHECK(f->size() == 100);
  f = UniquePointer<IFile>();
  CHECK_STATUS(fs.open_file("/readme.txt", OpenMode::CreateTruncate, &f), Status::Ok);
  CHECK(f->size() == 0);
  model.files["/readme.txt"] = Bytes(); model.files["/docs/link.txt"] = Bytes();
  f = UniquePointer<IFile>();
  EXPECT_MODEL(fs, model);

  // folders
  CHECK_STATUS(fs.create_directory("/newdir"), Status::Ok);
  CHECK_STATUS(fs.create_directory("/NEWDIR"), Status::AlreadyExists);
  CHECK_STATUS(fs.create_directory("/newdir/inner"), Status::Ok);
  CHECK_STATUS(fs.create_directory("/missing/inner"), Status::NotFound);
  CHECK_STATUS(fs.create_directory("/readme.txt"), Status::AlreadyExists);
  model.dirs.insert("/newdir"); model.dirs.insert("/newdir/inner");
  CHECK(write_file(fs, "/newdir/inner/deep.dat", make_data(12345, 41)));
  model.files["/newdir/inner/deep.dat"] = make_data(12345, 41);
  EXPECT_MODEL(fs, model);

  // rename
  CHECK_STATUS(fs.rename("/Big.bin", "/big.bin"), Status::Ok);  // only the case
  model.files["/big.bin"] = model.files["/Big.bin"]; model.files.erase("/Big.bin");
  CHECK_STATUS(fs.rename("/big.bin", "/newdir/moved.bin"), Status::Ok);
  model.files["/newdir/moved.bin"] = model.files["/big.bin"]; model.files.erase("/big.bin");
  CHECK_STATUS(fs.rename("/newdir/moved.bin", "/docs/a.txt"), Status::AlreadyExists);
  CHECK_STATUS(fs.rename("/missing", "/x"), Status::NotFound);
  CHECK_STATUS(fs.rename("/newdir", "/newdir/inner/x"), Status::InvalidArgument);
  CHECK_STATUS(fs.rename("/newdir", "/newdir"), Status::Ok);
  CHECK_STATUS(fs.rename("/", "/x"), Status::InvalidPath);
  CHECK_STATUS(fs.rename("/newdir", "/docs/renamed dir with a longer name"), Status::Ok);
  for (auto it = model.files.begin(); it != model.files.end();) {
    if (it->first.compare(0, 8, "/newdir/") == 0) { model.files["/docs/renamed dir with a longer name/" + it->first.substr(8)] = it->second; it = model.files.erase(it); }
    else ++it;
  }
  model.dirs.erase("/newdir"); model.dirs.erase("/newdir/inner");
  model.dirs.insert("/docs/renamed dir with a longer name"); model.dirs.insert("/docs/renamed dir with a longer name/inner");
  CHECK_STATUS(fs.rename("/split.bin", "/split2.bin"), Status::Unsupported);
  EXPECT_MODEL(fs, model);

  // remove
  CHECK_STATUS(fs.remove("/docs"), Status::NotEmpty);
  CHECK_STATUS(fs.remove("/emptydir"), Status::Ok);
  model.dirs.erase("/emptydir");
  CHECK_STATUS(fs.remove("/emptydir"), Status::NotFound);
  CHECK_STATUS(fs.remove("/"), Status::InvalidPath);
  {
    UniquePointer<IFile> held;
    CHECK_STATUS(fs.open_file("/docs/a.txt", OpenMode::Read, &held), Status::Ok);
    CHECK_STATUS(fs.remove("/docs/a.txt"), Status::Busy);
    CHECK_STATUS(fs.rename("/docs/a.txt", "/docs/a2.txt"), Status::Busy);
    UniquePointer<IDirectory> dir;
    CHECK_STATUS(fs.open_directory("/docs/sub", &dir), Status::Ok);
    CHECK_STATUS(fs.remove("/docs/sub/deep.txt"), Status::Ok);
    model.files.erase("/docs/sub/deep.txt");
    CHECK_STATUS(fs.remove("/docs/sub"), Status::Busy);
  }
  CHECK_STATUS(fs.remove("/docs/sub"), Status::Ok);
  model.dirs.erase("/docs/sub");
  CHECK_STATUS(fs.remove("/readme.txt"), Status::Ok);  // one of two names: the data stays under the other
  model.files.erase("/readme.txt");
  CHECK_STATUS(fs.remove("/comp.txt"), Status::Ok);
  model.files.erase("/comp.txt");
  CHECK_STATUS(fs.remove("/split.bin"), Status::Ok);
  model.files.erase("/split.bin");
  CHECK_STATUS(fs.remove("/sparse.bin"), Status::Ok);
  model.files.erase("/sparse.bin");
  CHECK_STATUS(fs.remove("/long name here.txt"), Status::Ok);  // with its 8.3 alias
  model.files.erase("/long name here.txt");
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  remount_check(*m.disk, 512, model, o.clusters, __LINE__);
  // the link survived under its other name
  Bytes got;
  CHECK(read_all(fs, "/docs/link.txt", &got) && got.empty());
  CHECK_STATUS(fs.remove("/docs/link.txt"), Status::Ok);
  model.files.erase("/docs/link.txt");
  // everything that was added goes again: the free space returns to what it was, less what the folders and the MFT kept
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
}

static void test_dirty_flag() {
  Opts o;
  Standard s = make_standard(o, 5);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  Fsck before(m.disk->data, o.clusters);
  CHECK(before.run(false).empty());
  CHECK(write_file(*m.fs, "/x.txt", make_data(10, 1)));
  Fsck during(m.disk->data, o.clusters);
  std::vector<std::string> e = during.run(false);
  CHECK(e.size() == 1 && e[0] == "volume still marked dirty");  // between the first change and flush(): dirty
  CHECK_STATUS(m.fs->flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  CHECK(write_file(*m.fs, "/y.txt", make_data(10, 1)));
  m.fs = UniquePointer<IFileSystem>();  // unmounting cleans up too
  EXPECT_CLEAN(*m.disk, o.clusters);
}

static void test_many_files_and_mft_growth() {
  Opts o; o.clusters = 8192; o.ibs = 1024;
  Standard s = make_standard(o, 0, 4);  // nearly no spare file records
  Model model = model_of(s);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  IFileSystem& fs = *m.fs;
  CHECK_STATUS(fs.create_directory("/bulk"), Status::Ok);
  model.dirs.insert("/bulk");
  const uint64_t free_start = fs.info().free_bytes;
  for (uint32_t i = 0; i < 1500; ++i) {
    char name[80]; snprintf(name, sizeof name, "/bulk/a_fairly_long_file_name_number_%05u.txt", (i * 7919) % 100000);
    Bytes d = make_data(i % 300, i);
    if (!write_file(fs, name, d)) { printf("FAILED: creating file %u\n", i); ++g_failed; break; }
    model.files[name] = d;
  }
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  EXPECT_MODEL(fs, model);
  // remove every other one, then the rest in reverse order
  uint32_t k = 0;
  std::vector<std::string> names;
  for (const auto& f : model.files) if (f.first.compare(0, 6, "/bulk/") == 0) names.push_back(f.first);
  for (const std::string& n : names) if (k++ % 2 == 0) { CHECK_STATUS(fs.remove(n.c_str()), Status::Ok); model.files.erase(n); }
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  for (size_t i = names.size(); i-- > 0;) if (model.files.count(names[i])) { CHECK_STATUS(fs.remove(names[i].c_str()), Status::Ok); model.files.erase(names[i]); }
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.remove("/bulk"), Status::Ok);
  model.dirs.erase("/bulk");
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  EXPECT_MODEL(fs, model);
  CHECK(fs.info().free_bytes <= free_start);  // (the folder's blocks come back, the MFT keeps the room it grew by: the verifier above proves nothing leaked)
  remount_check(*m.disk, 512, model, o.clusters, __LINE__);
}

static void test_full_volume() {
  Opts o; o.clusters = 700;
  Standard s = make_standard(o, 3);
  Model model = model_of(s);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  IFileSystem& fs = *m.fs;
  const uint64_t free_start = fs.info().free_bytes;
  UniquePointer<IFile> f;
  CHECK_STATUS(fs.open_file("/fill.bin", OpenMode::CreateOrOpen, &f), Status::Ok);
  Bytes chunk = make_data(64 * 1024, 5);
  uint64_t at = 0, put = 0;
  Status last = Status::Ok;
  for (int i = 0; i < 100 && last == Status::Ok; ++i) {
    last = f->write(at, ConstByteView(chunk.data(), chunk.size()), &put);
    if (last == Status::Ok) at += put;
  }
  CHECK(last == Status::NoSpace);
  CHECK(f->size() == at);
  CHECK(fs.info().free_bytes < 64 * 1024);
  f = UniquePointer<IFile>();
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  // creating things on a full volume fails cleanly
  Status c = fs.create_directory("/full1");
  CHECK(c == Status::Ok || c == Status::NoSpace);
  CHECK_STATUS(fs.remove("/fill.bin"), Status::Ok);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  if (c == Status::Ok) CHECK_STATUS(fs.remove("/full1"), Status::Ok);
  CHECK(fs.info().free_bytes == free_start);
}

static void test_blocks_and_partitions() {
  for (uint32_t block : {512u, 2048u, 4096u}) {
    Opts o; o.bps = block >= 4096 ? 4096 : 512; o.spc = block >= 4096 ? 1 : 8; o.clusters = block >= 4096 ? 8192 : 4096;
    Standard s = make_standard(o, 30);
    Model model = model_of(s);
    Mounted m;
    CHECK_STATUS(m.mount(s.image, block, true, 7), Status::Ok);  // a volume that starts 7 blocks into the device
    if (!m.fs) continue;
    CHECK(write_file(*m.fs, "/p.bin", make_data(33333, 3)));
    model.files["/p.bin"] = make_data(33333, 3);
    CHECK_STATUS(m.fs->create_directory("/pd"), Status::Ok);
    model.dirs.insert("/pd");
    CHECK_STATUS(m.fs->rename("/p.bin", "/pd/q.bin"), Status::Ok);
    model.files["/pd/q.bin"] = model.files["/p.bin"]; model.files.erase("/p.bin");
    EXPECT_MODEL(*m.fs, model);
    CHECK_STATUS(m.fs->flush(), Status::Ok);
    Bytes volume(m.disk->data + 7 * block, m.disk->data + m.disk->blocks * block);
    Fsck f(volume.data(), o.clusters);
    std::vector<std::string> e = f.run();
    if (!e.empty()) { printf("FAILED: block size %u: %s\n", block, e[0].c_str()); ++g_failed; }
  }
}

// Random operations against a model; every so often the volume is checked and mounted again from the device.
static void test_random(uint32_t seed, int operations, uint32_t ibs) {
  Opts o; o.clusters = 6144; o.ibs = ibs;
  Standard s = make_standard(o, 0, 8);
  Model model = model_of(s);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  CHECK_STATUS(m.fs->remove("/docs/link.txt"), Status::Ok);  // (a hard link would share its content with the other name: not in the model)
  model.files.erase("/docs/link.txt");
  uint64_t rng = seed * 2654435761u + 12345;
  auto rnd = [&](uint32_t n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>((rng >> 33) % n); };
  std::vector<std::string> dirs = {"", "/docs", "/many", "/emptydir"};
  auto name_in = [&](const std::string& dir) { char b[64]; snprintf(b, sizeof b, "%s/n%u", dir.c_str(), rnd(60)); return std::string(b); };
  auto pick_file = [&]() -> std::string {
    if (model.files.empty()) return "";
    auto it = model.files.begin(); std::advance(it, rnd(model.files.size())); return it->first;
  };
  for (int op = 0; op < operations; ++op) {
    IFileSystem& fs = *m.fs;
    const uint32_t kind = rnd(100);
    const std::string dir = dirs[rnd(dirs.size())];
    if (kind < 30) {  // create or overwrite
      const std::string path = name_in(dir);
      if (model.dirs.count(path)) continue;
      const uint32_t size = rnd(4) == 0 ? rnd(80000) : rnd(2000);
      Bytes d = make_data(size, op);
      if (!write_file(fs, path, d)) { printf("FAILED: op %d create %s\n", op, path.c_str()); ++g_failed; return; }
      model.files[path] = d;
    } else if (kind < 45) {  // write into a file
      const std::string path = pick_file();
      if (path.empty() || path.find("comp.txt") != std::string::npos || path.find("split.bin") != std::string::npos) continue;
      UniquePointer<IFile> f;
      Status os = fs.open_file(path.c_str(), OpenMode::ReadWrite, &f);
      if (os != Status::Ok) { printf("FAILED: op %d open %s: %s\n", op, path.c_str(), status_name(os)); ++g_failed; return; }
      Bytes& cur = model.files[path];
      const uint64_t at = cur.empty() ? 0 : rnd(cur.size() + 20000);
      Bytes d = make_data(rnd(9000), op);
      uint64_t put = 0;
      if (f->write(at, ConstByteView(d.data(), d.size()), &put) != Status::Ok) { printf("FAILED: op %d write %s\n", op, path.c_str()); ++g_failed; return; }
      if (!d.empty()) { if (cur.size() < at + d.size()) cur.resize(at + d.size(), 0); memcpy(cur.data() + at, d.data(), d.size()); }
    } else if (kind < 55) {  // resize
      const std::string path = pick_file();
      if (path.empty() || path.find("comp.txt") != std::string::npos || path.find("split.bin") != std::string::npos) continue;
      UniquePointer<IFile> f;
      if (fs.open_file(path.c_str(), OpenMode::ReadWrite, &f) != Status::Ok) { ++g_failed; return; }
      const uint64_t n = rnd(3) == 0 ? rnd(100000) : rnd(1500);
      if (f->resize(n) != Status::Ok) { printf("FAILED: op %d resize %s\n", op, path.c_str()); ++g_failed; return; }
      model.files[path].resize(n, 0);
    } else if (kind < 70) {  // remove
      const std::string path = pick_file();
      if (path.empty()) continue;
      if (fs.remove(path.c_str()) != Status::Ok) { printf("FAILED: op %d remove %s\n", op, path.c_str()); ++g_failed; return; }
      model.files.erase(path);
    } else if (kind < 78) {  // folder
      const std::string path = name_in(dir) + "d";
      if (model.files.count(path) || model.dirs.count(path)) continue;
      Status ms = fs.create_directory(path.c_str());
      if (ms != Status::Ok) { printf("FAILED: op %d mkdir %s: %s\n", op, path.c_str(), status_name(ms)); ++g_failed; return; }
      model.dirs.insert(path); dirs.push_back(path);
    } else if (kind < 92) {  // rename a file
      const std::string path = pick_file();
      if (path.empty() || path.find("split.bin") != std::string::npos || path.find("comp.txt") != std::string::npos) continue;
      const std::string to = name_in(dir) + "r";
      if (model.files.count(to) || model.dirs.count(to)) continue;
      Status r = fs.rename(path.c_str(), to.c_str());
      if (r != Status::Ok) { printf("FAILED: op %d rename %s -> %s: %s\n", op, path.c_str(), to.c_str(), status_name(r)); ++g_failed; return; }
      model.files[to] = model.files[path]; model.files.erase(path);
    } else {  // remove an empty folder
      std::string path;
      for (const std::string& d : model.dirs) {
        bool empty = true;
        for (const auto& f : model.files) if (f.first.compare(0, d.size() + 1, d + "/") == 0) empty = false;
        for (const std::string& e : model.dirs) if (e.compare(0, d.size() + 1, d + "/") == 0) empty = false;
        if (empty && d != "/docs" && d != "/many") { path = d; break; }
      }
      if (path.empty()) continue;
      if (fs.remove(path.c_str()) != Status::Ok) { printf("FAILED: op %d rmdir %s\n", op, path.c_str()); ++g_failed; return; }
      model.dirs.erase(path);
      dirs.erase(std::remove(dirs.begin(), dirs.end(), path), dirs.end());
    }
    if (op % 150 == 149) {
      CHECK_STATUS(m.fs->flush(), Status::Ok);
      EXPECT_CLEAN(*m.disk, o.clusters);
      EXPECT_MODEL(*m.fs, model);
      if (g_failed) { printf("  (seed %u, after operation %d)\n", seed, op); return; }
    }
  }
  CHECK_STATUS(m.fs->flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  remount_check(*m.disk, 512, model, o.clusters, __LINE__);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Damage

static void exercise(IFileSystem& fs, int depth, const std::string& path, int* budget) {
  if (depth > 6 || (*budget)-- <= 0) return;
  std::map<std::string, DirectoryEntry> entries;
  std::vector<std::string> names = list_dir(fs, path.empty() ? "/" : path.c_str(), &entries);
  for (const std::string& n : names) {
    if (n[0] == '<' || (*budget) <= 0) continue;
    const std::string full = path + "/" + n;
    DirectoryEntry e;
    fs.stat(full.c_str(), &e);
    if (entries[n].type == NodeType::Directory) { exercise(fs, depth + 1, full, budget); continue; }
    UniquePointer<IFile> f;
    if (fs.open_file(full.c_str(), OpenMode::Read, &f) != Status::Ok) continue;
    Bytes buf(40000);
    uint64_t got = 0, size = f->size();
    for (uint64_t at = 0; at < size && at < (1u << 20); at += buf.size()) { if (f->read(at, ByteView(buf.data(), buf.size()), &got) != Status::Ok) break; }
    f->read(size, ByteView(buf.data(), 10), &got);
  }
}

static void test_damage() {
  Opts o; o.clusters = 2048; o.ibs = 1024;
  Standard s = make_standard(o, 120);
  const uint64_t meta = std::min<uint64_t>(s.image.size(), 600 * 1024);
  uint64_t rng = 99;
  auto rnd = [&](uint64_t n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (rng >> 33) % n; };
  int mounted = 0, refused = 0;
  for (int iteration = 0; iteration < 400; ++iteration) {
    Bytes img = s.image;
    const int flips = 1 + rnd(4);
    for (int i = 0; i < flips; ++i) {
      // mostly the metadata: boot sector, MFT, folders; sometimes anywhere
      const uint64_t at = rnd(3) ? rnd(iteration % 5 == 0 ? 512 : meta) : rnd(img.size());
      img[at] ^= 1u << rnd(8);
    }
    Mounted m;
    if (m.mount(img, 512, true) != Status::Ok) { ++refused; continue; }
    ++mounted;
    int budget = 3000;
    exercise(*m.fs, 0, "", &budget);
    m.fs->info();
    // writes on a damaged volume may fail but must stay inside the volume
    m.fs->create_directory("/dmg");
    write_file(*m.fs, "/dmg/f.bin", make_data(9000, iteration));
    m.fs->rename("/dmg/f.bin", "/docs/f2.bin");
    m.fs->remove("/many/file_0007.dat");
    m.fs->remove("/docs");
    m.fs->flush();
    exercise(*m.fs, 0, "", &budget);
  }
  // truncated images
  for (int iteration = 0; iteration < 60; ++iteration) {
    const uint64_t length = rnd(s.image.size());
    Bytes img(s.image.begin(), s.image.begin() + length);
    if (img.empty()) img.resize(512);
    Mounted m;
    if (m.mount(img, 512, true) == Status::Ok) { int budget = 1000; exercise(*m.fs, 0, "", &budget); }
  }
  printf("  damage: %d mounted, %d refused\n", mounted, refused);
}

// A device that stops accepting writes after a number of them: the volume left behind must still be walkable.
static void test_write_failures() {
  Opts o; o.clusters = 2048;
  Standard s = make_standard(o, 30, 8);
  int failures_seen = 0;
  for (int64_t limit = 0; limit < 160; limit += (limit < 40 ? 1 : 7)) {
    Mounted m;
    CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
    m.disk->fail_after = limit;
    IFileSystem& fs = *m.fs;
    Status r[6];
    r[0] = fs.create_directory("/fd");
    r[1] = write_file(fs, "/fd/a.bin", make_data(30000, 1)) ? Status::Ok : Status::IoError;
    r[2] = fs.rename("/fd/a.bin", "/docs/a2.bin");
    r[3] = fs.remove("/Big.bin");
    r[4] = write_file(fs, "/many/zz.txt", make_data(3000, 2)) ? Status::Ok : Status::IoError;
    r[5] = fs.flush();
    for (Status st : r) if (st == Status::IoError) { ++failures_seen; break; }
    m.fs = UniquePointer<IFileSystem>();
    Bytes copy(m.disk->data, m.disk->data + m.disk->blocks * m.disk->block_size);
    Mounted again;
    if (again.mount(copy, 512, true) == Status::Ok) { int budget = 2000; exercise(*again.fs, 0, "", &budget); }
  }
  CHECK(failures_seen > 10);
}

static void test_handles() {
  Opts o;
  Standard s = make_standard(o, 5);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  // two handles on one file, one grows it from resident to non-resident while the other reads
  UniquePointer<IFile> a, b;
  CHECK(write_file(*m.fs, "/h.bin", make_data(50, 1)));
  CHECK_STATUS(m.fs->open_file("/h.bin", OpenMode::ReadWrite, &a), Status::Ok);
  CHECK_STATUS(m.fs->open_file("/h.bin", OpenMode::Read, &b), Status::Ok);
  Bytes buf(100000);
  uint64_t got = 0, put = 0;
  Bytes d = make_data(60000, 2);
  CHECK_STATUS(a->write(0, ConstByteView(d.data(), d.size()), &put), Status::Ok);
  CHECK_STATUS(b->read(0, ByteView(buf.data(), buf.size()), &got), Status::Ok);
  CHECK(got == 60000 && memcmp(buf.data(), d.data(), 60000) == 0);
  CHECK_STATUS(b->write(0, ConstByteView(d.data(), 10), &put), Status::ReadOnly);
  CHECK_STATUS(b->resize(0), Status::ReadOnly);
  // a listing in progress while files come and go
  UniquePointer<IDirectory> dir;
  CHECK_STATUS(m.fs->open_directory("/many", &dir), Status::Ok);
  DirectoryEntry e;
  CHECK_STATUS(dir->next(&e), Status::Ok);
  CHECK(write_file(*m.fs, "/many/aaa_new.txt", make_data(5, 3)));
  int n = 0;
  while (dir->next(&e) == Status::Ok && n < 1000) ++n;
  CHECK(n >= 4);
}

// Many names in one folder come and go against a model: the tree must stay ordered, no block may leak.
static void test_tree_stress(uint32_t seed, uint32_t ibs) {
  Opts o; o.clusters = 8192; o.ibs = ibs;
  Standard s = make_standard(o, 0, 8);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  IFileSystem& fs = *m.fs;
  CHECK_STATUS(fs.create_directory("/t"), Status::Ok);
  std::set<std::string> names;
  uint64_t rng = seed * 7919 + 1;
  auto rnd = [&](uint32_t n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return static_cast<uint32_t>((rng >> 33) % n); };
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 1400; ++i) {
      char name[80]; snprintf(name, sizeof name, "%c%0*u_%s", 'a' + rnd(26), 1 + rnd(5), rnd(1000), rnd(3) ? "x" : "a_longer_name_for_variety_in_the_entry_sizes");
      const std::string path = std::string("/t/") + name;
      if (rnd(10) < (round == 1 ? 8u : 6u)) {
        if (names.count(name)) continue;
        std::string lower = name; for (char& c : lower) c = tolower(c);
        bool clash = false;
        for (const std::string& n : names) { std::string l = n; for (char& c : l) c = tolower(c); if (l == lower) clash = true; }
        if (clash) continue;
        UniquePointer<IFile> f;
        if (fs.open_file(path.c_str(), OpenMode::CreateOrOpen, &f) != Status::Ok) { printf("FAILED: create %s\n", path.c_str()); ++g_failed; return; }
        names.insert(name);
      } else if (!names.empty()) {
        auto it = names.begin(); std::advance(it, rnd(names.size()));
        if (fs.remove((std::string("/t/") + *it).c_str()) != Status::Ok) { printf("FAILED: remove %s\n", it->c_str()); ++g_failed; return; }
        names.erase(it);
      }
    }
    CHECK_STATUS(fs.flush(), Status::Ok);
    EXPECT_CLEAN(*m.disk, o.clusters);
    std::vector<std::string> listed = list_dir(fs, "/t");
    std::vector<std::string> want(names.begin(), names.end());
    std::sort(want.begin(), want.end(), [](const std::string& a, const std::string& b) { return name_less(u16(a), u16(b)); });
    if (listed != want) { printf("FAILED: listing differs (%zu vs %zu names), seed %u round %d\n", listed.size(), want.size(), seed, round); ++g_failed; return; }
    for (const std::string& n : names) { DirectoryEntry e; if (fs.stat(("/t/" + n).c_str(), &e) != Status::Ok) { printf("FAILED: stat %s\n", n.c_str()); ++g_failed; return; } }
  }
  for (const std::string& n : std::set<std::string>(names)) CHECK_STATUS(fs.remove(("/t/" + n).c_str()), Status::Ok);
  CHECK_STATUS(fs.remove("/t"), Status::Ok);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
}

static void test_dos_names() {
  Opts o;
  Standard s = make_standard(o, 5);
  Model model = model_of(s);
  Mounted m;
  CHECK_STATUS(m.mount(s.image, 512, true), Status::Ok);
  IFileSystem& fs = *m.fs;
  CHECK_STATUS(fs.rename("/long name here.txt", "/docs/renamed.txt"), Status::Ok);  // the 8.3 alias goes with the old name
  model.files["/docs/renamed.txt"] = model.files["/long name here.txt"]; model.files.erase("/long name here.txt");
  DirectoryEntry e;
  CHECK_STATUS(fs.stat("/SHORT~1.TXT", &e), Status::NotFound);
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  // a hard link renamed and then its other name removed
  CHECK_STATUS(fs.rename("/docs/link.txt", "/docs/other link.txt"), Status::Ok);
  model.files["/docs/other link.txt"] = model.files["/docs/link.txt"]; model.files.erase("/docs/link.txt");
  CHECK_STATUS(fs.remove("/readme.txt"), Status::Ok);
  model.files.erase("/readme.txt");
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
  // the same file opened under both names while it changes
  UniquePointer<IFile> a;
  CHECK_STATUS(fs.open_file("/docs/other link.txt", OpenMode::ReadWrite, &a), Status::Ok);
  Bytes more = make_data(9000, 3);
  uint64_t put = 0;
  CHECK_STATUS(a->write(0, ConstByteView(more.data(), more.size()), &put), Status::Ok);
  a = UniquePointer<IFile>();  // closing brings the sizes in the names up to date
  model.files["/docs/other link.txt"] = more;
  EXPECT_MODEL(fs, model);
  CHECK_STATUS(fs.flush(), Status::Ok);
  EXPECT_CLEAN(*m.disk, o.clusters);
}

// `NtfsTest real in.img out.img expected_dir`: mounts an image made by a real mkntfs, changes it, saves it, and writes the files that
// should be on it to expected_dir, so that ntfs-3g's own tools can check the result (see the scripts the test run documents).
static void put_expected(const std::string& dir, const std::string& path, const Bytes& data) {
  std::string full = dir + path, partial;
  for (size_t i = 1; i < full.size(); ++i) if (full[i] == '/') mkdir(full.substr(0, i).c_str(), 0755);
  FILE* f = fopen(full.c_str(), "wb");
  if (!f) { printf("cannot write %s\n", full.c_str()); exit(2); }
  if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
  fclose(f);
}

static int run_real(const char* in, const char* out, const char* expected) {
  FILE* f = fopen(in, "rb");
  if (!f) { printf("cannot open %s\n", in); return 2; }
  Bytes image;
  uint8_t buffer[65536];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) image.insert(image.end(), buffer, buffer + n);
  fclose(f);
  Mounted m;
  Status s = m.mount(image, 512, true);
  if (s != Status::Ok) { printf("mount failed: %s\n", status_name(s)); return 1; }
  IFileSystem& fs = *m.fs;
  printf("mounted: label '%s', read_only=%d, free %llu bytes\n", fs.info().label.c_str(), fs.info().read_only, (unsigned long long)fs.info().free_bytes);
  if (fs.info().read_only) { printf("volume came up read only\n"); return 1; }
  int bad = 0;
  auto must = [&](Status st, const char* what) { if (st != Status::Ok) { printf("%s: %s\n", what, status_name(st)); ++bad; } };
  auto put = [&](const std::string& path, const Bytes& data) {
    if (!write_file(fs, path, data)) { printf("write %s failed\n", path.c_str()); ++bad; return; }
    put_expected(expected, path, data);
  };
  must(fs.create_directory("/dir1"), "mkdir dir1");
  must(fs.create_directory("/dir1/sub"), "mkdir sub");
  put("/small.txt", make_data(200, 1));
  put("/medium.bin", make_data(50000, 2));
  put("/dir1/large.bin", make_data(3 * 1024 * 1024 + 17, 3));
  put("/dir1/sub/deep.txt", make_data(700, 4));
  put("/Mixed Case Name.TXT", make_data(1234, 5));
  put("/unicode_\xC3\xA9\xE2\x82\xAC.txt", make_data(10, 6));
  must(fs.create_directory("/many"), "mkdir many");
  for (int i = 0; i < 700; ++i) {
    char name[80]; snprintf(name, sizeof name, "/many/a_reasonably_long_file_name_%04d.dat", (i * 37) % 1000);
    put(name, make_data(i % 3000, i));
  }
  // grow a resident file out of its record, cut another, rename, remove
  {
    UniquePointer<IFile> h;
    must(fs.open_file("/small.txt", OpenMode::ReadWrite, &h), "open small");
    Bytes more = make_data(20000, 7), all = make_data(200, 1);
    uint64_t put_bytes = 0;
    must(h->write(200, ConstByteView(more.data(), more.size()), &put_bytes), "grow small");
    all.insert(all.end(), more.begin(), more.end());
    h = UniquePointer<IFile>();
    put_expected(expected, "/small.txt", all);
    must(fs.open_file("/medium.bin", OpenMode::ReadWrite, &h), "open medium");
    must(h->resize(12345), "cut medium");
    h = UniquePointer<IFile>();
    Bytes cut = make_data(50000, 2); cut.resize(12345);
    put_expected(expected, "/medium.bin", cut);
  }
  must(fs.rename("/dir1/sub/deep.txt", "/dir1/moved.txt"), "rename file");
  { Bytes d = make_data(700, 4); std::string p = std::string(expected) + "/dir1/sub/deep.txt"; remove(p.c_str()); put_expected(expected, "/dir1/moved.txt", d); }
  must(fs.rename("/dir1/sub", "/subdir_renamed"), "rename dir");
  for (int i = 0; i < 700; i += 2) {
    char name[80]; snprintf(name, sizeof name, "/many/a_reasonably_long_file_name_%04d.dat", (i * 37) % 1000);
    must(fs.remove(name), "remove");
    std::string p = std::string(expected) + name; ::remove(p.c_str());
  }
  must(fs.flush(), "flush");
  m.fs = UniquePointer<IFileSystem>();
  FILE* o = fopen(out, "wb");
  fwrite(m.disk->data, 1, m.disk->blocks * m.disk->block_size, o);
  fclose(o);
  printf(bad ? "%d operations failed\n" : "all operations succeeded\n", bad);
  return bad ? 1 : 0;
}

// `NtfsTest dump in.img out_dir`: copies every file of an image (read only) to out_dir.
static void dump_tree(IFileSystem& fs, const std::string& path, const std::string& out, int* files) {
  std::map<std::string, DirectoryEntry> entries;
  std::vector<std::string> names = list_dir(fs, path.empty() ? "/" : path.c_str(), &entries);
  mkdir((out + path).c_str(), 0755);
  for (const std::string& n : names) {
    const std::string full = path + "/" + n;
    if (entries[n].type == NodeType::Directory) { dump_tree(fs, full, out, files); continue; }
    Bytes b;
    if (!read_all(fs, full.c_str(), &b)) { printf("cannot read %s\n", full.c_str()); ++g_failed; continue; }
    put_expected(out, full, b);
    ++*files;
  }
}

static int run_dump(const char* in, const char* out) {
  FILE* f = fopen(in, "rb");
  if (!f) return 2;
  Bytes image;
  uint8_t buffer[65536];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) image.insert(image.end(), buffer, buffer + n);
  fclose(f);
  Mounted m;
  Status s = m.mount(image, 512, false);
  if (s != Status::Ok) { printf("mount failed: %s\n", status_name(s)); return 1; }
  int files = 0;
  dump_tree(*m.fs, "", out, &files);
  printf("dumped %d files\n", files);
  return g_failed ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc == 4 && strcmp(argv[1], "dump") == 0) return run_dump(argv[2], argv[3]);
  if (argc == 5 && strcmp(argv[1], "real") == 0) return run_real(argv[2], argv[3], argv[4]);
  g_verbose = getenv("NTFS_VERBOSE") != nullptr;
  auto want = [&](const char* name) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; ++i) if (strcmp(argv[i], name) == 0) return true;
    return false;
  };
  if (want("read")) { Opts o; test_read(512, 0, o); }
  if (want("read")) { Opts o; test_read(2048, 3, o); }
  if (want("bigdir")) test_big_directory();
  if (want("mount")) test_mount_rules();
  if (want("files")) test_write_files();
  if (want("times")) test_times();
  if (want("dirty")) test_dirty_flag();
  if (want("handles")) test_handles();
  if (want("blocks")) test_blocks_and_partitions();
  if (want("bulk")) test_many_files_and_mft_growth();
  if (want("full")) test_full_volume();
  if (want("random")) for (uint32_t seed = 1; seed <= 6; ++seed) test_random(seed, 1500, seed % 2 ? 4096 : 1024);
  if (want("tree")) for (uint32_t seed = 1; seed <= 4; ++seed) test_tree_stress(seed, seed % 2 ? 1024 : 4096);
  if (want("dos")) test_dos_names();
  if (want("failures")) test_write_failures();
  if (want("damage")) test_damage();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("NtfsTest: all passed\n");
  return 0;
}
