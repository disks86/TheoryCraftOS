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

// Discard of freed blocks (FileSystem/DiscardBatch), the same check for every file system's test: a memory disk whose discard() overwrites
// the range with a poison byte, and a workload of files created, rewritten, shrunk, grown and removed with flushes in between. Every file
// that is still there must read back exactly after each flush and after a remount - a block discarded while (or after) it is in use again
// shows as poison - and discards must actually have happened. Header only: each test has its own MemDisk; this is the part they share.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "FileSystem/IFileSystem.hpp"

namespace discard_test {

constexpr uint8_t kPoison = 0xDD;

// What a test's MemDisk adds: the discard itself (bounds-checked, poisoning) and counters.
struct DiscardCounters {
  bool enabled = false;
  uint64_t calls = 0, blocks = 0, refused = 0;
  // write_zeroes: offered when `zeroes`; `refuse_zeroes` turns every call down (the caller must write zeros itself then).
  bool zeroes = false, refuse_zeroes = false;
  uint64_t zero_calls = 0, zeroed_blocks = 0;
  bool write_zeroes(uint8_t* data, uint64_t total, uint32_t block_size, uint64_t lba, uint64_t count) {
    if (!zeroes || refuse_zeroes || count == 0 || lba >= total || count > total - lba) return false;
    memset(data + lba * block_size, 0, count * block_size);
    ++zero_calls;
    zeroed_blocks += count;
    return true;
  }
  // Poisons blocks [lba, lba + count) of `data` (a disk of `total` blocks of `block_size`); false when out of range.
  bool discard(uint8_t* data, uint64_t total, uint32_t block_size, uint64_t lba, uint64_t count) {
    if (!enabled || count == 0 || lba >= total || count > total - lba) {
      ++refused;
      return false;
    }
    memset(data + lba * block_size, kPoison, count * block_size);
    ++calls;
    blocks += count;
    return true;
  }
};

struct Model {
  bool exists = false;
  uint64_t size = 0;
  uint32_t version = 0;
};

inline uint8_t pattern(uint32_t file, uint32_t version, uint64_t offset) {
  return static_cast<uint8_t>(file * 131 + version * 31 + offset * 7 + (offset >> 9));
}

inline void name_of(uint32_t file, char* out) { snprintf(out, 32, "/discard%02u.bin", file); }

// Writes `size` bytes of the file's pattern (a fresh file).
inline bool write_file(filesystem::IFileSystem& fs, uint32_t file, uint32_t version, uint64_t size) {
  char name[32];
  name_of(file, name);
  UniquePointer<filesystem::IFile> f;
  if (fs.open_file(name, filesystem::OpenMode::CreateTruncate, &f) != filesystem::Status::Ok || !f) return false;
  std::vector<uint8_t> data(size);
  for (uint64_t i = 0; i < size; ++i) data[i] = pattern(file, version, i);
  uint64_t done = 0;
  return f->write(0, ConstByteView(data.data(), size), &done) == filesystem::Status::Ok && done == size;
}

inline bool check_all(filesystem::IFileSystem& fs, const std::vector<Model>& files, const char* when) {
  bool ok = true;
  for (uint32_t i = 0; i < files.size(); ++i) {
    char name[32];
    name_of(i, name);
    UniquePointer<filesystem::IFile> f;
    const filesystem::Status s = fs.open_file(name, filesystem::OpenMode::Read, &f);
    if (!files[i].exists) {
      if (s == filesystem::Status::Ok) { printf("  discard workload (%s): %s should be gone\n", when, name); ok = false; }
      continue;
    }
    if (s != filesystem::Status::Ok || !f || f->size() != files[i].size) {
      printf("  discard workload (%s): %s missing or wrong size\n", when, name);
      ok = false;
      continue;
    }
    std::vector<uint8_t> data(files[i].size);
    uint64_t got = 0;
    if (files[i].size && (f->read(0, ByteView(data.data(), data.size()), &got) != filesystem::Status::Ok || got != data.size())) {
      printf("  discard workload (%s): %s unreadable\n", when, name);
      ok = false;
      continue;
    }
    for (uint64_t k = 0; k < data.size(); ++k) {
      if (data[k] != pattern(i, files[i].version, k)) {
        printf("  discard workload (%s): %s differs at byte %llu (0x%02x%s)\n", when, name, (unsigned long long)k, data[k],
               data[k] == kPoison ? ", poison: discarded while in use" : "");
        ok = false;
        break;
      }
    }
  }
  return ok;
}

// Runs the workload on a mounted, writable file system. `files` keeps the expected state (for a check after a remount).
inline bool run(filesystem::IFileSystem& fs, uint32_t seed, std::vector<Model>* files, uint32_t max_kib = 96) {
  files->assign(10, Model{});
  uint32_t state = seed | 1;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  bool ok = true;
  for (uint32_t round = 0; round < 80 && ok; ++round) {
    const uint32_t i = next() % files->size();
    Model& m = (*files)[i];
    const uint32_t op = next() % 4;
    char name[32];
    name_of(i, name);
    if (!m.exists || op == 0) {   // (re)write it whole: the old blocks are freed, new ones taken
      m.version = round + 1;
      m.size = 1 + next() % (max_kib * 1024);
      ok = write_file(fs, i, m.version, m.size);
      m.exists = ok;
    } else if (op == 1) {   // remove
      ok = fs.remove(name) == filesystem::Status::Ok;
      m.exists = false;
    } else if (op == 2) {   // shrink: the tail is freed
      UniquePointer<filesystem::IFile> f;
      const uint64_t size = m.size / 3;
      ok = fs.open_file(name, filesystem::OpenMode::ReadWrite, &f) == filesystem::Status::Ok && f && f->resize(size) == filesystem::Status::Ok;
      m.size = size;
    } else {   // grow by appending more of its pattern: blocks freed earlier may be taken again
      UniquePointer<filesystem::IFile> f;
      const uint64_t more = 1 + next() % (32 * 1024);
      std::vector<uint8_t> data(more);
      for (uint64_t k = 0; k < more; ++k) data[k] = pattern(i, m.version, m.size + k);
      uint64_t done = 0;
      ok = fs.open_file(name, filesystem::OpenMode::ReadWrite, &f) == filesystem::Status::Ok && f &&
           f->write(m.size, ConstByteView(data.data(), more), &done) == filesystem::Status::Ok && done == more;
      m.size += more;
    }
    if (!ok) printf("  discard workload: operation %u on %s failed in round %u\n", op, name, round);
    if (ok && next() % 3 == 0) {
      ok = fs.flush() == filesystem::Status::Ok;
      ok = check_all(fs, *files, "after a flush") && ok;
    }
  }
  ok = ok && fs.flush() == filesystem::Status::Ok;
  return check_all(fs, *files, "at the end") && ok;
}

// fstrim on the mounted volume: it must discard something (the workload left free space) and every file must still read back - the disk
// poisoned all that was discarded, so a block in use among them shows.
inline bool trim_and_check(filesystem::IFileSystem& fs, const std::vector<Model>& files, const DiscardCounters& counters) {
  const uint64_t before = counters.blocks;
  uint64_t bytes = 0;
  const filesystem::Status s = fs.trim("/", &bytes);
  if (s != filesystem::Status::Ok || bytes == 0 || counters.blocks == before) {
    printf("  trim: status %d, %llu bytes\n", static_cast<int>(s), (unsigned long long)bytes);
    return false;
  }
  return check_all(fs, files, "after a trim");
}

// send_discards without a flush: a file removed is discarded by it (and nothing else in use).
inline bool send_without_flush(filesystem::IFileSystem& fs, const DiscardCounters& counters) {
  if (!write_file(fs, 99, 1, 200 * 1024) || fs.flush() != filesystem::Status::Ok) return false;
  char name[32];
  name_of(99, name);
  if (fs.remove(name) != filesystem::Status::Ok) return false;
  const uint64_t before = counters.blocks;
  if (fs.send_discards() != filesystem::Status::Ok || counters.blocks == before) {
    printf("  send_discards sent nothing\n");
    return false;
  }
  return true;
}

}  // namespace discard_test
