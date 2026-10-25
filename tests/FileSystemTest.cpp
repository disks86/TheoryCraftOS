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

// Host-side tests for the filesystem interface, RamFileSystem and FatFileSystem
// (the latter against FAT12/16/32 images built here, on devices of 512 and 2048 byte blocks).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FileSystem/Fat/FatFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/StorageDevice/Partition/PartitionStorageDevice.hpp"

using namespace filesystem;

struct TestAllocator final : allocators::IAllocator {
  int live = 0; bool fail = false;
  void* allocate(uint64_t size) override { if (fail) return nullptr; ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)
#define CHECK_STATUS(expr, want) do { Status got_ = (expr); if (got_ != (want)) { printf("FAILED line %d: %s -> %s, want %s\n", __LINE__, #expr, status_name(got_), status_name(want)); ++g_failed; } } while (0)

#include "FileTimes.hpp"

static ConstByteView bytes_of(const char* s) { return ConstByteView(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

// Everything in a directory, as "name" or "name/" strings joined by ','.
static void list(IFileSystem& fs, const char* path, char* out, size_t cap) {
  out[0] = 0;
  UniquePointer<IDirectory> dir;
  if (fs.open_directory(path, &dir) != Status::Ok) { strcpy(out, "!open"); return; }
  DirectoryEntry e;
  while (dir->next(&e) == Status::Ok) {
    if (out[0]) strncat(out, ",", cap - strlen(out) - 1);
    strncat(out, e.name.c_str(), cap - strlen(out) - 1);
    if (e.type == NodeType::Directory) strncat(out, "/", cap - strlen(out) - 1);
  }
}

static bool read_all(IFileSystem& fs, const char* path, uint8_t* out, uint64_t cap, uint64_t* length) {
  UniquePointer<IFile> f;
  if (fs.open_file(path, OpenMode::Read, &f) != Status::Ok) return false;
  *length = f->size();
  if (*length > cap) return false;
  uint64_t got = 0;
  return f->read(0, ByteView(out, *length), &got) == Status::Ok && got == *length;
}

static void test_paths() {
  StringView rest = "//a/./bb///c/", c;
  CHECK(next_component(&rest, &c) && c.equals("a"));
  CHECK(next_component(&rest, &c) && is_dot(c));
  CHECK(next_component(&rest, &c) && c.equals("bb"));
  CHECK(next_component(&rest, &c) && c.equals("c"));
  CHECK(!next_component(&rest, &c));
  StringView parent, leaf;
  CHECK(split_parent("/a/b/c", &parent, &leaf) && leaf.equals("c") && parent.equals("/a/b/"));
  CHECK(split_parent("x", &parent, &leaf) && leaf.equals("x") && parent.empty());
  CHECK(split_parent("/a/b/./", &parent, &leaf) && leaf.equals("b"));
  CHECK(!split_parent("/", &parent, &leaf) && !split_parent("", &parent, &leaf) && !split_parent("/./", &parent, &leaf));
  CHECK(valid_name("a b.txt") && !valid_name("") && !valid_name(".") && !valid_name("..") && !valid_name(StringView("a\0b", 3)));
  char longname[300]; memset(longname, 'x', sizeof longname);
  CHECK(valid_name(StringView(longname, 255)) && !valid_name(StringView(longname, 256)));
}

static void test_ram() {
  TestAllocator a;
  {
    RamFileSystem fs(a);
    char l[256];
    CHECK(fs.info().type_name.equals("ramfs") && !fs.info().read_only);
    list(fs, "/", l, sizeof l); CHECK(!strcmp(l, ""));
    CHECK_STATUS(fs.create_directory("/etc"), Status::Ok);
    CHECK_STATUS(fs.create_directory("/etc"), Status::AlreadyExists);
    CHECK_STATUS(fs.create_directory("/nope/x"), Status::NotFound);
    CHECK_STATUS(fs.create_directory("/"), Status::InvalidPath);
    CHECK_STATUS(fs.create_directory("/etc/../b"), Status::InvalidPath);

    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/etc/motd", OpenMode::Read, &f), Status::NotFound);
    CHECK_STATUS(fs.open_file("/etc/motd", OpenMode::CreateOrOpen, &f), Status::Ok);
    uint64_t n = 0;
    CHECK_STATUS(f->write(0, bytes_of("hello"), &n), Status::Ok); CHECK(n == 5 && f->size() == 5);
    CHECK_STATUS(f->write(8, bytes_of("!"), &n), Status::Ok); CHECK(f->size() == 9);  // gap of zeros
    uint8_t buf[16];
    CHECK_STATUS(f->read(0, ByteView(buf), &n), Status::Ok);
    CHECK(n == 9 && !memcmp(buf, "hello\0\0\0!", 9));
    CHECK_STATUS(f->read(7, ByteView(buf, 100 > 16 ? 16 : 16), &n), Status::Ok); CHECK(n == 2);
    CHECK_STATUS(f->read(9, ByteView(buf), &n), Status::Ok); CHECK(n == 0);
    CHECK_STATUS(f->read(1000, ByteView(buf), &n), Status::Ok); CHECK(n == 0);
    CHECK_STATUS(f->resize(3), Status::Ok); CHECK(f->size() == 3);
    CHECK_STATUS(f->resize(6), Status::Ok);
    CHECK_STATUS(f->read(0, ByteView(buf), &n), Status::Ok); CHECK(n == 6 && !memcmp(buf, "hel\0\0\0", 6));  // regrown part is zero, not stale
    CHECK_STATUS(f->write(~uint64_t{0}, bytes_of("x"), &n), Status::InvalidArgument);

    // Busy while open; truncating an open file is refused too.
    CHECK_STATUS(fs.remove("/etc/motd"), Status::Busy);
    UniquePointer<IFile> g;
    CHECK_STATUS(fs.open_file("/etc/motd", OpenMode::CreateTruncate, &g), Status::Busy);
    g.reset();
    f.reset();
    DirectoryEntry e;
    CHECK_STATUS(fs.stat("/etc/motd", &e), Status::Ok);
    CHECK(e.name.equals("motd") && e.type == NodeType::File && e.size == 6);
    CHECK_STATUS(fs.stat("/etc", &e), Status::Ok); CHECK(e.type == NodeType::Directory && e.size == 0);
    CHECK_STATUS(fs.stat("/", &e), Status::Ok); CHECK(e.name.empty() && e.type == NodeType::Directory);
    CHECK_STATUS(fs.stat("etc//./motd/", &e), Status::Ok);  // sloppy spelling
    CHECK_STATUS(fs.stat("/ETC/motd", &e), Status::NotFound);  // case sensitive
    CHECK_STATUS(fs.stat("/etc/motd/x", &e), Status::NotADirectory);

    CHECK_STATUS(fs.open_file("/etc", OpenMode::Read, &f), Status::IsADirectory);
    CHECK_STATUS(fs.open_file("/etc", OpenMode::CreateOrOpen, &f), Status::IsADirectory);
    CHECK_STATUS(fs.open_file("/", OpenMode::CreateOrOpen, &f), Status::InvalidPath);
    UniquePointer<IDirectory> d;
    CHECK_STATUS(fs.open_directory("/etc/motd", &d), Status::NotADirectory);

    CHECK_STATUS(fs.open_file("/etc/motd", OpenMode::Read, &f), Status::Ok);
    CHECK_STATUS(f->write(0, bytes_of("x"), &n), Status::ReadOnly);
    CHECK_STATUS(f->resize(0), Status::ReadOnly);
    f.reset();
    CHECK_STATUS(fs.open_file("/etc/motd", OpenMode::CreateTruncate, &f), Status::Ok); CHECK(f->size() == 0);
    f.reset();

    // Rename and move.
    CHECK_STATUS(fs.create_directory("/var"), Status::Ok);
    CHECK_STATUS(fs.rename("/etc/motd", "/var/issue"), Status::Ok);
    list(fs, "/etc", l, sizeof l); CHECK(!strcmp(l, ""));
    list(fs, "/var", l, sizeof l); CHECK(!strcmp(l, "issue"));
    CHECK_STATUS(fs.create_directory("/var/sub"), Status::Ok);
    CHECK_STATUS(fs.rename("/var", "/var/sub/x"), Status::InvalidArgument);
    CHECK_STATUS(fs.rename("/var", "/var"), Status::Ok);
    CHECK_STATUS(fs.rename("/var/issue", "/var/sub"), Status::AlreadyExists);
    CHECK_STATUS(fs.rename("/missing", "/y"), Status::NotFound);
    CHECK_STATUS(fs.rename("/var", "/etc/v2"), Status::Ok);
    list(fs, "/", l, sizeof l); CHECK(!strcmp(l, "etc/"));
    list(fs, "/etc/v2", l, sizeof l); CHECK(!strcmp(l, "issue,sub/"));

    // Remove.
    CHECK_STATUS(fs.remove("/etc"), Status::NotEmpty);
    CHECK_STATUS(fs.remove("/etc/v2/sub"), Status::Ok);
    CHECK_STATUS(fs.remove("/etc/v2/issue"), Status::Ok);
    CHECK_STATUS(fs.remove("/etc/v2/issue"), Status::NotFound);
    CHECK_STATUS(fs.remove("/"), Status::InvalidPath);

    // Directory iteration and rewind; a handle keeps the directory busy.
    CHECK_STATUS(fs.open_directory("/etc/v2", &d), Status::Ok);
    CHECK_STATUS(fs.remove("/etc/v2"), Status::Busy);
    d.reset();
    CHECK_STATUS(fs.remove("/etc/v2"), Status::Ok);
    CHECK_STATUS(fs.remove("/etc"), Status::Ok);
  }
  CHECK(a.live == 0);

  // Size limit.
  {
    RamFileSystem fs(a, 100);
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/a", OpenMode::CreateOrOpen, &f), Status::Ok);
    uint8_t big[200] = {};
    uint64_t n;
    CHECK_STATUS(f->write(0, ConstByteView(big, 60), &n), Status::Ok);
    CHECK(fs.info().total_bytes == 100 && fs.info().free_bytes == 40);
    CHECK_STATUS(f->write(0, ConstByteView(big, 101), &n), Status::NoSpace); CHECK(n == 0 && f->size() == 60);
    UniquePointer<IFile> g;
    CHECK_STATUS(fs.open_file("/b", OpenMode::CreateOrOpen, &g), Status::Ok);
    CHECK_STATUS(g->write(0, ConstByteView(big, 41), &n), Status::NoSpace);
    CHECK_STATUS(g->write(0, ConstByteView(big, 40), &n), Status::Ok);
    CHECK(fs.info().free_bytes == 0);
    f.reset(); g.reset();
    CHECK_STATUS(fs.remove("/a"), Status::Ok);
    CHECK(fs.info().free_bytes == 60);
  }
  CHECK(a.live == 0);

  // Out of memory leaves things consistent.
  {
    RamFileSystem fs(a);
    a.fail = true;
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.create_directory("/d"), Status::OutOfMemory);
    CHECK_STATUS(fs.open_file("/f", OpenMode::CreateOrOpen, &f), Status::OutOfMemory);
    a.fail = false;
    CHECK_STATUS(fs.open_file("/f", OpenMode::CreateOrOpen, &f), Status::Ok);
    a.fail = true;
    uint64_t n;
    CHECK_STATUS(f->write(0, bytes_of("data"), &n), Status::OutOfMemory); CHECK(f->size() == 0);
    a.fail = false;
    f.reset();
    DirectoryEntry e;
    CHECK_STATUS(fs.stat("/d", &e), Status::NotFound);
  }
  CHECK(a.live == 0);

  // Many files, and a big file.
  {
    RamFileSystem fs(a);
    char name[16];
    for (int i = 0; i < 100; ++i) {
      snprintf(name, sizeof name, "/f%d", i);
      UniquePointer<IFile> f;
      CHECK_STATUS(fs.open_file(name, OpenMode::CreateOrOpen, &f), Status::Ok);
      uint64_t n; uint8_t b = static_cast<uint8_t>(i);
      CHECK_STATUS(f->write(0, ConstByteView(&b, 1), &n), Status::Ok);
    }
    UniquePointer<IDirectory> d;
    CHECK_STATUS(fs.open_directory("/", &d), Status::Ok);
    DirectoryEntry e; int count = 0;
    while (d->next(&e) == Status::Ok) ++count;
    CHECK(count == 100);
    d->rewind(); CHECK_STATUS(d->next(&e), Status::Ok); CHECK(e.name.equals("f0") && e.size == 1);
    d.reset();
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/big", OpenMode::CreateOrOpen, &f), Status::Ok);
    uint8_t chunk[1000]; uint64_t n;
    for (int i = 0; i < 1000; ++i) { memset(chunk, i, sizeof chunk); CHECK_STATUS(f->write(uint64_t{1000} * i, ConstByteView(chunk), &n), Status::Ok); }
    CHECK(f->size() == 1000000);
    CHECK_STATUS(f->read(uint64_t{1000} * 777 + 5, ByteView(chunk, 3), &n), Status::Ok);
    CHECK(n == 3 && chunk[0] == (777 & 255));
  }
  CHECK(a.live == 0);
}

// A storage device over a byte array.
struct MemDisk final : hardware::IStorageDevice {
  MemDisk(const uint8_t* image, uint64_t length, uint32_t block, bool writable = false) : block_size(block), can_write(writable) {
    blocks = (length + block - 1) / block;
    data = static_cast<uint8_t*>(calloc(blocks, block));
    memcpy(data, image, length);
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
    memcpy(data + lba * block_size, buffer.data(), buffer.size());
    return true;
  }
  bool flush() override { return true; }
  uint8_t* data; uint64_t blocks; uint32_t block_size; bool can_write;
};

// Builds small FAT images the way mkfs and a driver would lay them out.
struct FatImage {
  enum Kind { F12, F16, F32 };
  struct Dir { uint32_t first = 0; uint32_t chain[64]; uint32_t chain_len = 0; uint32_t entries = 0; };

  FatImage(Kind k) : kind(k) {
    total = k == F12 ? 2880 : k == F16 ? 20000 : 70000;
    reserved = k == F32 ? 32 : 1;
    fat_sectors = k == F12 ? 9 : k == F16 ? 79 : 547;
    root_entries = k == F32 ? 0 : (k == F12 ? 224 : 512);
    img = static_cast<uint8_t*>(calloc(total, 512));
    root_sectors = root_entries * 32 / 512;
    data_start = reserved + 2 * fat_sectors + root_sectors;
    uint8_t* b = img;
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90; memcpy(b + 3, "TESTFMT ", 8);
    put16(b + 11, 512); b[13] = 1; put16(b + 14, reserved); b[16] = 2; put16(b + 17, root_entries);
    put16(b + 19, total < 65536 ? total : 0); b[21] = 0xF8; put16(b + 22, k == F32 ? 0 : fat_sectors);
    put32(b + 32, total < 65536 ? 0 : total);
    if (k == F32) { put32(b + 36, fat_sectors); put32(b + 44, 2); memcpy(b + 71, "FAT32 VOL  ", 11); }
    else memcpy(b + 43, "SMALLVOL   ", 11);
    b[510] = 0x55; b[511] = 0xAA;
    set_fat(0, k == F12 ? 0xFF8 : k == F16 ? 0xFFF8 : 0x0FFFFFF8);
    set_fat(1, k == F12 ? 0xFFF : k == F16 ? 0xFFFF : 0x0FFFFFFF);
    root.first = k == F32 ? alloc_cluster() : 0;
    if (k == F32) { root.chain[root.chain_len++] = root.first; set_fat(root.first, eoc()); }
  }
  ~FatImage() { free(img); }

  static void put16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
  static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
  uint32_t eoc() const { return kind == F12 ? 0xFFF : kind == F16 ? 0xFFFF : 0x0FFFFFFF; }
  uint8_t* fat() { return img + reserved * 512; }
  void set_fat(uint32_t c, uint32_t v) {
    for (int copy = 0; copy < 2; ++copy) {
      uint8_t* f = fat() + copy * fat_sectors * 512;
      if (kind == F12) {
        uint32_t o = c + c / 2;
        if (c & 1) { f[o] = (f[o] & 0x0F) | ((v & 0xF) << 4); f[o + 1] = v >> 4; }
        else { f[o] = v; f[o + 1] = (f[o + 1] & 0xF0) | ((v >> 8) & 0xF); }
      } else if (kind == F16) put16(f + c * 2, v);
      else put32(f + c * 4, v);
    }
  }
  uint32_t alloc_cluster() { return next_free++; }
  uint8_t* cluster_at(uint32_t c) { return img + (data_start + c - 2) * 512; }

  uint8_t* new_entry(Dir& d) {
    const uint32_t per_cluster = 16;
    if (d.first == 0 && &d == &root) return img + (reserved + 2 * fat_sectors) * 512 + (d.entries++) * 32;
    if (d.entries == d.chain_len * per_cluster) {
      uint32_t c = alloc_cluster();
      if (d.chain_len) set_fat(d.chain[d.chain_len - 1], c);
      else d.first = c;
      d.chain[d.chain_len++] = c;
      set_fat(c, eoc());
    }
    uint8_t* p = cluster_at(d.chain[d.entries / per_cluster]) + (d.entries % per_cluster) * 32;
    ++d.entries;
    return p;
  }
  static uint8_t checksum(const uint8_t* n) { uint8_t s = 0; for (int i = 0; i < 11; ++i) s = ((s & 1) << 7) + (s >> 1) + n[i]; return s; }

  // `name83` is 11 characters, space padded. `lfn` (UTF-16, zero terminated) may be null.
  uint8_t* add(Dir& d, const char* name83, const uint16_t* lfn, uint8_t attr, uint32_t first_cluster, uint32_t size) {
    if (lfn) {
      uint32_t len = 0; while (lfn[len]) ++len;
      uint32_t pieces = (len + 12) / 13;
      uint8_t sum = checksum(reinterpret_cast<const uint8_t*>(name83));
      for (uint32_t p = pieces; p >= 1; --p) {
        uint8_t* e = new_entry(d);
        e[0] = p | (p == pieces ? 0x40 : 0); e[11] = 0x0F; e[13] = sum;
        static const uint8_t off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (uint32_t i = 0; i < 13; ++i) {
          uint32_t idx = (p - 1) * 13 + i;
          uint16_t ch = idx < len ? lfn[idx] : idx == len ? 0 : 0xFFFF;
          put16(e + off[i], ch);
        }
      }
    }
    uint8_t* e = new_entry(d);
    memcpy(e, name83, 11); e[11] = attr;
    put16(e + 26, first_cluster); put16(e + 20, first_cluster >> 16); put32(e + 28, size);
    return e;
  }
  void file(Dir& d, const char* name83, const uint16_t* lfn, const uint8_t* data, uint32_t size) {
    uint32_t first = 0, prev = 0;
    for (uint32_t off = 0; off < size; off += 512) {
      uint32_t c = alloc_cluster();
      if (prev) set_fat(prev, c); else first = c;
      set_fat(c, eoc());
      uint32_t n = size - off < 512 ? size - off : 512;
      memcpy(cluster_at(c), data + off, n);
      prev = c;
    }
    add(d, name83, lfn, 0x20, first, size);
  }
  // A subdirectory; "." and ".." entries included like a real one.
  void dir(Dir& parent, Dir& child, const char* name83, const uint16_t* lfn) {
    uint8_t* e = add(parent, name83, lfn, 0x10, 0, 0);
    uint8_t* dot = new_entry(child); memcpy(dot, ".          ", 11); dot[11] = 0x10;
    uint8_t* dotdot = new_entry(child); memcpy(dotdot, "..         ", 11); dotdot[11] = 0x10;
    put16(e + 26, child.first); put16(e + 20, child.first >> 16);
    put16(dot + 26, child.first); put16(dotdot + 26, 0);
  }

  Kind kind; uint8_t* img;
  uint32_t total, reserved, fat_sectors, root_entries, root_sectors, data_start, next_free = 2;
  Dir root;
};

static void test_fat(FatImage::Kind kind, uint32_t block, const char* label) {
  TestAllocator a;
  {
    FatImage im(kind);
    uint8_t pattern[1700];
    for (uint32_t i = 0; i < sizeof pattern; ++i) pattern[i] = static_cast<uint8_t>(i * 7 + 3);
    static const uint16_t long_name[] = {'L','o','n','g',' ','F','i','l','e',' ','N','a','m','e','.','t','x','t', 0};
    static const uint16_t accent[] = {'c','a','f', 0xE9, 0x20AC, 0xD83D, 0xDE00, 0};  // cafe-acute, euro, an emoji
    FatImage::Dir sub, many;
    im.file(im.root, "HELLO   TXT", nullptr, reinterpret_cast<const uint8_t*>("hello fat"), 9);
    im.file(im.root, "LONGFI~1TXT", long_name, pattern, sizeof pattern);
    im.file(im.root, "CAF~1   TXT", accent, pattern, 10);
    im.dir(im.root, sub, "SUB        ", nullptr);
    im.file(sub, "INNER   BIN", nullptr, pattern, 512);  // exactly one cluster
    im.file(sub, "EMPTY   DAT", nullptr, nullptr, 0);
    im.dir(sub, many, "MANY       ", nullptr);
    uint8_t* gone = im.add(im.root, "DELETED TXT", nullptr, 0x20, 0, 0); gone[0] = 0xE5;
    uint8_t* vol = im.add(im.root, "THEVOLUME  ", nullptr, 0x08, 0, 0); (void)vol;
    uint8_t* lower = im.add(im.root, "LOWER   TXT", nullptr, 0x20, 0, 0);
    lower[12] = 0x18;  // the NT lower-case flags: this one is shown as "lower.txt"
    for (int i = 0; i < 40; ++i) {  // more entries than fit in a cluster (16) to make the directory a chain
      char n[16]; snprintf(n, sizeof n, "F%-7dBIN", i);
      im.file(many, n, nullptr, pattern, 3);
    }
    MemDisk disk(im.img, im.total * 512ull, block);

    UniquePointer<IFileSystem> fsp;
    CHECK_STATUS(FatFileSystem::mount(a, disk, 0, &fsp), Status::Ok);
    if (!fsp) { printf("  (%s: mount failed, skipping)\n", label); return; }
    IFileSystem& fs = *fsp;
    FileSystemInfo info = fs.info();
    CHECK(info.read_only);
    CHECK(info.type_name.equals(kind == FatImage::F12 ? "FAT12" : kind == FatImage::F16 ? "FAT16" : "FAT32"));
    CHECK(info.label.equals(kind == FatImage::F32 ? "FAT32 VOL" : "SMALLVOL"));
    CHECK(info.total_bytes > 0);

    char l[1024];
    list(fs, "/", l, sizeof l);
    char want[256];
    snprintf(want, sizeof want, "HELLO.TXT,Long File Name.txt,caf\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80,SUB/,lower.txt");
    if (strcmp(l, want)) { printf("FAILED root listing (%s): %s\n", label, l); ++g_failed; }
    list(fs, "/sub", l, sizeof l); CHECK(!strcmp(l, "INNER.BIN,EMPTY.DAT,MANY/"));
    list(fs, "/SUB/many/", l, sizeof l); CHECK(strstr(l, "F0.BIN,F1.BIN") == l && strstr(l, "F39.BIN"));
    { int commas = 0; for (char* p = l; *p; ++p) commas += *p == ','; CHECK(commas == 39); }

    uint8_t buf[2048]; uint64_t len = 0;
    CHECK(read_all(fs, "/hello.txt", buf, sizeof buf, &len) && len == 9 && !memcmp(buf, "hello fat", 9));  // case insensitive
    CHECK(read_all(fs, "/long file name.TXT", buf, sizeof buf, &len) && len == sizeof pattern && !memcmp(buf, pattern, len));
    CHECK(read_all(fs, "/Sub/Inner.bin", buf, sizeof buf, &len) && len == 512 && !memcmp(buf, pattern, 512));
    CHECK(read_all(fs, "/sub/empty.dat", buf, sizeof buf, &len) && len == 0);
    CHECK(read_all(fs, "/sub/many/F39.BIN", buf, sizeof buf, &len) && len == 3 && !memcmp(buf, pattern, 3));

    // Partial reads across cluster boundaries, forwards and backwards.
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/Long File Name.txt", OpenMode::Read, &f), Status::Ok);
    uint64_t n;
    CHECK_STATUS(f->read(500, ByteView(buf, 30), &n), Status::Ok); CHECK(n == 30 && !memcmp(buf, pattern + 500, 30));
    CHECK_STATUS(f->read(1500, ByteView(buf, 500), &n), Status::Ok); CHECK(n == 200 && !memcmp(buf, pattern + 1500, 200));
    CHECK_STATUS(f->read(10, ByteView(buf, 1100), &n), Status::Ok); CHECK(n == 1100 && !memcmp(buf, pattern + 10, 1100));
    CHECK_STATUS(f->read(1700, ByteView(buf, 8), &n), Status::Ok); CHECK(n == 0);
    CHECK_STATUS(f->read(0, ByteView(buf, 0), &n), Status::Ok); CHECK(n == 0);
    CHECK_STATUS(f->write(0, bytes_of("x"), &n), Status::ReadOnly);
    CHECK_STATUS(f->resize(0), Status::ReadOnly);
    f.reset();

    DirectoryEntry e;
    CHECK_STATUS(fs.stat("/hello.txt", &e), Status::Ok);
    CHECK(e.name.equals("HELLO.TXT") && e.size == 9 && e.type == NodeType::File);
    CHECK_STATUS(fs.stat("/sub/", &e), Status::Ok); CHECK(e.type == NodeType::Directory && e.size == 0);
    CHECK_STATUS(fs.stat("/", &e), Status::Ok); CHECK(e.type == NodeType::Directory && e.name.empty());
    CHECK_STATUS(fs.stat("/nothing", &e), Status::NotFound);
    CHECK_STATUS(fs.stat("/deleted.txt", &e), Status::NotFound);
    CHECK_STATUS(fs.stat("/thevolume", &e), Status::NotFound);
    CHECK_STATUS(fs.stat("/hello.txt/x", &e), Status::NotADirectory);
    CHECK_STATUS(fs.stat("/sub/../hello.txt", &e), Status::InvalidPath);
    CHECK_STATUS(fs.stat("/sub/.", &e), Status::Ok);
    CHECK_STATUS(fs.open_file("/sub", OpenMode::Read, &f), Status::IsADirectory);
    UniquePointer<IDirectory> d;
    CHECK_STATUS(fs.open_directory("/hello.txt", &d), Status::NotADirectory);

    CHECK_STATUS(fs.open_file("/hello.txt", OpenMode::ReadWrite, &f), Status::ReadOnly);
    CHECK_STATUS(fs.open_file("/new", OpenMode::CreateOrOpen, &f), Status::ReadOnly);
    CHECK_STATUS(fs.create_directory("/x"), Status::ReadOnly);
    CHECK_STATUS(fs.remove("/hello.txt"), Status::ReadOnly);
    CHECK_STATUS(fs.rename("/hello.txt", "/y"), Status::ReadOnly);

    CHECK_STATUS(fs.open_directory("/sub", &d), Status::Ok);
    int count = 0; while (d->next(&e) == Status::Ok) ++count;
    CHECK(count == 3);
    CHECK_STATUS(d->next(&e), Status::EndOfDirectory);
    d->rewind(); CHECK_STATUS(d->next(&e), Status::Ok); CHECK(e.name.equals("INNER.BIN") && e.size == 512);
    d.reset();
    fsp.reset();
    CHECK(a.live == 0);
  }
  CHECK(a.live == 0);
}

static void test_fat_bad() {
  TestAllocator a;
  FatImage im(FatImage::F16);
  UniquePointer<IFileSystem> fs;
  { MemDisk disk(im.img, im.total * 512ull, 512);
    im.img[510] = 0;
    MemDisk bad(im.img, im.total * 512ull, 512);
    CHECK_STATUS(FatFileSystem::mount(a, bad, 0, &fs), Status::Corrupt);
    im.img[510] = 0x55;
    CHECK_STATUS(FatFileSystem::mount(a, disk, 5000, &fs), Status::Corrupt);       // not at a boot sector
    CHECK_STATUS(FatFileSystem::mount(a, disk, 1u << 30, &fs), Status::InvalidArgument);
    a.fail = true;
    CHECK_STATUS(FatFileSystem::mount(a, disk, 0, &fs), Status::OutOfMemory);
    a.fail = false;
    CHECK(!fs);

    // A cluster chain that loops, and one that points at nonsense, must end in an error, not a hang.
    FatImage::Dir sub;
    FatImage::Dir& root = im.root;
    uint8_t data[2000] = {1};
    im.file(root, "LOOP    BIN", nullptr, data, 2000);   // clusters 2..5
    im.dir(root, sub, "SUB        ", nullptr);
    im.set_fat(5, 3);                                    // loops back inside the file (a file of 4 clusters is fine, the link is not)
    MemDisk looped(im.img, im.total * 512ull, 512);
    CHECK_STATUS(FatFileSystem::mount(a, looped, 0, &fs), Status::Ok);
    UniquePointer<IFile> f; uint64_t n; uint8_t buf[2000];
    CHECK_STATUS(fs->open_file("/loop.bin", OpenMode::Read, &f), Status::Ok);
    CHECK_STATUS(f->read(0, ByteView(buf), &n), Status::Ok);  // 2000 bytes fit in the 4 clusters; looping is only reachable past them
    im.set_fat(3, 0xFFF0);                                    // a reserved/bad link
    MemDisk badlink(im.img, im.total * 512ull, 512);
    UniquePointer<IFileSystem> fs2;
    CHECK_STATUS(FatFileSystem::mount(a, badlink, 0, &fs2), Status::Ok);
    f.reset();
    CHECK_STATUS(fs2->open_file("/loop.bin", OpenMode::Read, &f), Status::Ok);
    CHECK_STATUS(f->read(0, ByteView(buf), &n), Status::Corrupt);
    f.reset(); fs.reset(); fs2.reset();
  }
  CHECK(a.live == 0);
}

// A volume that does not start at block 0: directly via first_block, and through a partition window.
static void test_fat_offset() {
  TestAllocator a;
  FatImage im(FatImage::F16);
  im.file(im.root, "HELLO   TXT", nullptr, reinterpret_cast<const uint8_t*>("hello fat"), 9);
  const uint64_t prefix = 100;
  const uint64_t image_bytes = im.total * 512ull;
  uint8_t* shifted = static_cast<uint8_t*>(calloc(prefix + im.total, 512));
  memcpy(shifted + prefix * 512, im.img, image_bytes);
  MemDisk disk(shifted, (prefix + im.total) * 512ull, 512);
  free(shifted);
  uint8_t buf[16]; uint64_t len = 0;
  {
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(FatFileSystem::mount(a, disk, 0, &fs), Status::Corrupt);  // block 0 is not a boot sector
    CHECK_STATUS(FatFileSystem::mount(a, disk, prefix, &fs), Status::Ok);
    CHECK(fs && read_all(*fs, "/hello.txt", buf, sizeof buf, &len) && len == 9 && !memcmp(buf, "hello fat", 9));
  }
  {
    hardware::PartitionStorageDevice part(disk, prefix, im.total, 1);
    UniquePointer<IFileSystem> fs;
    CHECK_STATUS(FatFileSystem::mount(a, part, 0, &fs), Status::Ok);
    CHECK(fs && read_all(*fs, "/hello.txt", buf, sizeof buf, &len) && len == 9 && !memcmp(buf, "hello fat", 9));
  }
  CHECK(a.live == 0);
}


// Writing: create, grow, shrink, rename, remove, long names, directories that outgrow a cluster, a full disk, and a remount that finds it all.
static void test_fat_write(FatImage::Kind kind, uint32_t block, const char* label) {
  TestAllocator a;
  {
    FatImage im(kind);
    static const uint16_t existing_long[] = {'Q','u','a','r','t','e','r','l','y',' ','R','e','p','o','r','t',' ','2','0','2','6','.','t','x','t', 0};
    im.file(im.root, "HELLO   TXT", nullptr, reinterpret_cast<const uint8_t*>("hello fat"), 9);
    im.file(im.root, "QUARTE~1TXT", existing_long, reinterpret_cast<const uint8_t*>("old"), 3);
    MemDisk disk(im.img, im.total * 512ull, block, true);
    UniquePointer<IFileSystem> fsp;
    CHECK_STATUS(FatFileSystem::mount(a, disk, 0, &fsp), Status::Ok);
    if (!fsp) { printf("  (%s: mount failed, skipping)\n", label); return; }
    IFileSystem& fs = *fsp;
    CHECK(!fs.info().read_only);
    const uint64_t free_at_start = fs.info().free_bytes;
    CHECK(free_at_start > 0);

    uint8_t pattern[6000];
    for (uint32_t i = 0; i < sizeof pattern; ++i) pattern[i] = static_cast<uint8_t>(i * 13 + 5);
    uint8_t buf[8192]; uint64_t n = 0, len = 0;
    char l[2048];

    // Create and write; a short lower-case name keeps its case, the listing shows it as given.
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/new.txt", OpenMode::Read, &f), Status::NotFound);
    CHECK_STATUS(fs.open_file("/new.txt", OpenMode::CreateOrOpen, &f), Status::Ok);
    CHECK(f && f->size() == 0);
    CHECK_STATUS(f->write(0, bytes_of("hello world"), &n), Status::Ok); CHECK(n == 11 && f->size() == 11);
    CHECK_STATUS(f->write(6, bytes_of("WORLD!!"), &n), Status::Ok); CHECK(n == 7 && f->size() == 13);
    f.reset();
    CHECK(read_all(fs, "/NEW.TXT", buf, sizeof buf, &len) && len == 13 && !memcmp(buf, "hello WORLD!!", 13));
    DirectoryEntry e;
    CHECK_STATUS(fs.stat("/new.txt", &e), Status::Ok); CHECK(e.name.equals("new.txt") && e.size == 13);

    // A write that starts past the end leaves zeros in the gap; a multi-cluster file reads back whole.
    CHECK_STATUS(fs.open_file("/big.bin", OpenMode::CreateTruncate, &f), Status::Ok);
    CHECK_STATUS(f->write(1000, ConstByteView(pattern, sizeof pattern), &n), Status::Ok); CHECK(n == sizeof pattern && f->size() == 1000 + sizeof pattern);
    CHECK_STATUS(f->read(0, ByteView(buf, 1000), &n), Status::Ok); { bool zeros = n == 1000; for (int i = 0; i < 1000; ++i) zeros &= buf[i] == 0; CHECK(zeros); }
    CHECK_STATUS(f->read(1000, ByteView(buf, sizeof pattern), &n), Status::Ok); CHECK(n == sizeof pattern && !memcmp(buf, pattern, n));
    // Shrinking and growing again must not bring back the old bytes.
    CHECK_STATUS(f->resize(2000), Status::Ok); CHECK(f->size() == 2000);
    CHECK_STATUS(f->resize(5000), Status::Ok); CHECK(f->size() == 5000);
    CHECK_STATUS(f->read(1990, ByteView(buf, 20), &n), Status::Ok);
    { bool ok = n == 20; for (int i = 0; i < 10; ++i) ok &= buf[i] == pattern[990 + i]; for (int i = 10; i < 20; ++i) ok &= buf[i] == 0; CHECK(ok); }
    CHECK_STATUS(f->resize(0), Status::Ok); CHECK(f->size() == 0);
    CHECK_STATUS(f->write(0, bytes_of("again"), &n), Status::Ok);
    f.reset();
    CHECK(read_all(fs, "/big.bin", buf, sizeof buf, &len) && len == 5 && !memcmp(buf, "again", 5));
    // CreateTruncate empties an existing file; opening read-only refuses to write.
    CHECK_STATUS(fs.open_file("/big.bin", OpenMode::CreateTruncate, &f), Status::Ok); CHECK(f->size() == 0); f.reset();
    CHECK_STATUS(fs.open_file("/hello.txt", OpenMode::Read, &f), Status::Ok);
    CHECK_STATUS(f->write(0, bytes_of("x"), &n), Status::ReadOnly); CHECK_STATUS(f->resize(0), Status::ReadOnly);
    f.reset();
    CHECK_STATUS(fs.open_file("/hello.txt", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(f->write(0, bytes_of("J"), &n), Status::Ok); f.reset();
    CHECK(read_all(fs, "/hello.txt", buf, sizeof buf, &len) && len == 9 && !memcmp(buf, "Jello fat", 9));
    CHECK_STATUS(fs.open_file("/missing", OpenMode::ReadWrite, &f), Status::NotFound);
    CHECK_STATUS(fs.open_file("/nodir/file", OpenMode::CreateOrOpen, &f), Status::NotFound);
    CHECK_STATUS(fs.open_file("/new.txt/x", OpenMode::CreateOrOpen, &f), Status::NotADirectory);

    // Long names: mixed case, spaces, many files with the same start (each needs its own ~N alias), non-ASCII.
    const char* names[] = {"Quarterly Report 2027.txt", "Quarterly Report 2028.txt", "Mixed Case.TXT", "caf\xC3\xA9 \xE2\x82\xAC.txt", "UPPER.TXT", "a.b.c", ".hidden", "Data.Bin"};
    for (const char* nm : names) {
      char path[64]; snprintf(path, sizeof path, "/%s", nm);
      CHECK_STATUS(fs.open_file(path, OpenMode::CreateOrOpen, &f), Status::Ok);
      CHECK_STATUS(f->write(0, bytes_of(nm), &n), Status::Ok);
      f.reset();
    }
    for (const char* nm : names) {
      char path[64]; snprintf(path, sizeof path, "/%s", nm);
      CHECK(read_all(fs, path, buf, sizeof buf, &len) && len == strlen(nm) && !memcmp(buf, nm, len));
    }
    CHECK(read_all(fs, "/QUARTERLY REPORT 2026.TXT", buf, sizeof buf, &len) && len == 3);   // the one that was there before
    list(fs, "/", l, sizeof l);
    for (const char* nm : names) if (!strstr(l, nm)) { printf("FAILED listing misses %s (%s): %s\n", nm, label, l); ++g_failed; }
    CHECK_STATUS(fs.open_file("/mixed case.txt", OpenMode::CreateOrOpen, &f), Status::Ok);   // the same name in other case is the same file
    CHECK(f->size() == strlen("Mixed Case.TXT")); f.reset();
    CHECK_STATUS(fs.open_file("/bad:name", OpenMode::CreateOrOpen, &f), Status::InvalidPath);
    CHECK_STATUS(fs.open_file("/trailing.", OpenMode::CreateOrOpen, &f), Status::InvalidPath);
    CHECK_STATUS(fs.open_file("/../b", OpenMode::CreateOrOpen, &f), Status::InvalidPath);

    // Directories, also one that needs more than a cluster, and files inside them.
    CHECK_STATUS(fs.create_directory("/docs"), Status::Ok);
    CHECK_STATUS(fs.create_directory("/docs"), Status::AlreadyExists);
    CHECK_STATUS(fs.create_directory("/hello.txt"), Status::AlreadyExists);
    CHECK_STATUS(fs.create_directory("/nope/x"), Status::NotFound);
    CHECK_STATUS(fs.create_directory("/docs/sub dir"), Status::Ok);
    list(fs, "/docs", l, sizeof l); CHECK(!strcmp(l, "sub dir/"));
    list(fs, "/docs/sub dir", l, sizeof l); CHECK(!strcmp(l, ""));
    for (int i = 0; i < 60; ++i) {
      char path[64]; snprintf(path, sizeof path, "/docs/sub dir/file number %d.dat", i);
      CHECK_STATUS(fs.open_file(path, OpenMode::CreateOrOpen, &f), Status::Ok);
      CHECK_STATUS(f->write(0, ConstByteView(pattern, 100 + i), &n), Status::Ok);
      f.reset();
    }
    { UniquePointer<IDirectory> d; CHECK_STATUS(fs.open_directory("/docs/sub dir", &d), Status::Ok);
      DirectoryEntry de; int count = 0; while (d->next(&de) == Status::Ok) ++count; CHECK(count == 60); }
    CHECK(read_all(fs, "/docs/sub dir/file number 59.dat", buf, sizeof buf, &len) && len == 159 && !memcmp(buf, pattern, 159));

    // Remove: not while it has contents or is open; free space comes back.
    CHECK_STATUS(fs.remove("/docs/sub dir"), Status::NotEmpty);
    CHECK_STATUS(fs.remove("/nothing"), Status::NotFound);
    CHECK_STATUS(fs.remove("/"), Status::InvalidPath);
    CHECK_STATUS(fs.open_file("/new.txt", OpenMode::Read, &f), Status::Ok);
    CHECK_STATUS(fs.remove("/new.txt"), Status::Busy);
    CHECK_STATUS(fs.rename("/new.txt", "/renamed.txt"), Status::Busy);
    f.reset();
    CHECK_STATUS(fs.remove("/new.txt"), Status::Ok);
    CHECK_STATUS(fs.stat("/new.txt", &e), Status::NotFound);

    // Rename: in place, across directories, a directory with its contents, onto a taken name, into itself.
    CHECK_STATUS(fs.rename("/hello.txt", "/Greeting Text.txt"), Status::Ok);
    CHECK_STATUS(fs.stat("/hello.txt", &e), Status::NotFound);
    CHECK(read_all(fs, "/greeting text.txt", buf, sizeof buf, &len) && len == 9 && !memcmp(buf, "Jello fat", 9));
    CHECK_STATUS(fs.rename("/Greeting Text.txt", "/docs/greet.txt"), Status::Ok);
    CHECK(read_all(fs, "/docs/greet.txt", buf, sizeof buf, &len) && len == 9);
    CHECK_STATUS(fs.rename("/docs/greet.txt", "/docs/GREET.txt"), Status::Ok);        // only the case changes
    list(fs, "/docs", l, sizeof l); CHECK(strstr(l, "GREET.txt") && !strstr(l, "greet.txt"));
    CHECK_STATUS(fs.rename("/docs/greet.txt", "/docs/sub dir/file number 1.dat"), Status::AlreadyExists);
    CHECK_STATUS(fs.rename("/docs", "/docs/sub dir/inner"), Status::InvalidArgument);
    CHECK_STATUS(fs.rename("/docs/sub dir", "/moved"), Status::Ok);
    CHECK_STATUS(fs.stat("/docs/sub dir", &e), Status::NotFound);
    CHECK(read_all(fs, "/moved/file number 7.dat", buf, sizeof buf, &len) && len == 107);
    CHECK_STATUS(fs.rename("/nothing", "/x"), Status::NotFound);
    CHECK_STATUS(fs.rename("/moved", "/nodir/x"), Status::NotFound);
    for (int i = 0; i < 60; ++i) {
      char path[64]; snprintf(path, sizeof path, "/moved/file number %d.dat", i);
      CHECK_STATUS(fs.remove(path), Status::Ok);
    }
    CHECK_STATUS(fs.remove("/moved"), Status::Ok);

    // A remount sees everything (the changes are on the device), and both copies of the FAT agree.
    f.reset();
    fsp.reset();
    {
      const uint32_t fat_bytes = im.fat_sectors * 512;
      CHECK(!memcmp(disk.data + im.reserved * 512, disk.data + im.reserved * 512 + fat_bytes, fat_bytes));
      UniquePointer<IFileSystem> again;
      CHECK_STATUS(FatFileSystem::mount(a, disk, 0, &again), Status::Ok);
      if (again) {
        CHECK(read_all(*again, "/Mixed Case.TXT", buf, sizeof buf, &len) && len == strlen("Mixed Case.TXT"));
        CHECK(read_all(*again, "/docs/greet.txt", buf, sizeof buf, &len) && len == 9 && !memcmp(buf, "Jello fat", 9));
        CHECK_STATUS(again->stat("/moved", &e), Status::NotFound);
        list(*again, "/docs", l, sizeof l); CHECK(!strcmp(l, "GREET.txt"));
        // Clean up what this test made and the free space is what it was (nothing leaked).
        CHECK_STATUS(again->remove("/docs/GREET.txt"), Status::Ok);
        CHECK_STATUS(again->remove("/docs"), Status::Ok);
        for (const char* nm : names) { char path[64]; snprintf(path, sizeof path, "/%s", nm); CHECK_STATUS(again->remove(path), Status::Ok); }
        CHECK_STATUS(again->remove("/big.bin"), Status::Ok);
        CHECK_STATUS(again->remove("/Quarterly Report 2026.txt"), Status::Ok);
        CHECK_STATUS(again->remove("/hello.txt"), Status::NotFound);
        CHECK(again->info().free_bytes > free_at_start - 3 * 4096);   // (the three files the image started with, and what they take)
        // A full disk: the write fails and the clusters it took come back.
        const uint64_t before = again->info().free_bytes;
        const uint64_t too_big = before + 4096;
        uint8_t* huge = static_cast<uint8_t*>(calloc(1, too_big));
        UniquePointer<IFile> w;
        CHECK_STATUS(again->open_file("/huge.bin", OpenMode::CreateTruncate, &w), Status::Ok);
        CHECK_STATUS(w->write(0, ConstByteView(huge, too_big), &n), Status::NoSpace);
        CHECK(w->size() == 0 && again->info().free_bytes == before);
        w.reset();
        free(huge);
        CHECK_STATUS(again->remove("/huge.bin"), Status::Ok);
        CHECK(again->info().free_bytes == before);
        CHECK_STATUS(again->flush(), Status::Ok);
      }
    }
    CHECK(a.live == 0);
  }
  CHECK(a.live == 0);
}

// Time stamps on a RAM filesystem and on FAT (12, 16, 32), also with a local time that differs from UTC.
static void test_times() {
  {
    TestAllocator a;
    RamFileSystem fs(a);
    filetimes::check_stamps(fs, "ram");
  }
  const FatImage::Kind kinds[3] = {FatImage::F12, FatImage::F16, FatImage::F32};
  for (const FatImage::Kind kind : kinds) {
    for (int zone = 0; zone < 2; ++zone) {
      TestAllocator a;
      FatImage im(kind);
      MemDisk disk(im.img, im.total * 512ull, 512, true);
      UniquePointer<IFileSystem> fsp;
      CHECK_STATUS(FatFileSystem::mount(a, disk, 0, &fsp), Status::Ok);
      if (!fsp) continue;
      set_local_offset_seconds(zone ? 5 * 3600 + 1800 : 0);   // (local time five and a half hours ahead of UTC)
      filetimes::check_stamps(*fsp, zone ? "fat zone" : "fat");
      if (zone) {  // the entry of /e/h holds the local time of its last change: 2023-11-15 04:50:00 (UTC 23:20:00 plus 5:30)
        UniquePointer<IFile> f;
        CHECK_STATUS(fsp->flush(), Status::Ok);
        const uint8_t name[11] = {'H', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        bool found = false;
        for (uint64_t at = 0; at + 32 <= im.total * 512ull && !found; at += 32) {
          if (memcmp(disk.data + at, name, 11) != 0) continue;
          found = true;
          const uint32_t time = disk.data[at + 22] | (disk.data[at + 23] << 8), date = disk.data[at + 24] | (disk.data[at + 25] << 8);
          CHECK(date == (((2023 - 1980) << 9) | (11 << 5) | 15));
          CHECK((time >> 11) == 4 && ((time >> 5) & 63) == 50 && (time & 31) == 0);
        }
        (void)found;
      }
      set_local_offset_seconds(0);
    }
  }
}

int main() {
  test_times();
  test_paths();
  test_ram();
  test_fat(FatImage::F12, 512, "FAT12");
  test_fat(FatImage::F16, 512, "FAT16");
  test_fat(FatImage::F32, 512, "FAT32");
  test_fat(FatImage::F12, 2048, "FAT12/2048");
  test_fat(FatImage::F16, 2048, "FAT16/2048");
  test_fat(FatImage::F32, 2048, "FAT32/2048");
  test_fat_write(FatImage::F12, 512, "FAT12 write");
  test_fat_write(FatImage::F16, 512, "FAT16 write");
  test_fat_write(FatImage::F32, 512, "FAT32 write");
  test_fat_write(FatImage::F12, 2048, "FAT12/2048 write");
  test_fat_write(FatImage::F16, 2048, "FAT16/2048 write");
  test_fat_write(FatImage::F32, 2048, "FAT32/2048 write");
  test_fat_bad();
  test_fat_offset();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("filesystem tests passed\n");
  return 0;
}
