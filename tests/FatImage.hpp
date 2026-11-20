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

// Host tests: small FAT12/16/32 images built in memory (FileSystemTest, FlushDurabilityTest).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
