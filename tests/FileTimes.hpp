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

// The same time stamp scenario for every filesystem that keeps times: what a file and its folder report after being created, written,
// renamed, cut and removed, with a clock the test sets. Include after CHECK and CHECK_STATUS are defined. Times are even seconds (FAT keeps
// two second steps).
#pragma once
#include "FileSystem/IFileSystem.hpp"

namespace filetimes {

inline uint64_t g_now = 0;
inline uint64_t fake_clock(void*) { return g_now; }

inline bool times_of(filesystem::IFileSystem& fs, const char* path, uint64_t* modified, uint64_t* created) {
  filesystem::DirectoryEntry e;
  if (fs.stat(path, &e) != filesystem::Status::Ok) return false;
  *modified = e.modified;
  *created = e.created;
  return true;
}

// `fs` must be writable and hold nothing named /d, /e.
inline void check_stamps(filesystem::IFileSystem& fs, const char* label) {
  using namespace filesystem;
  const uint64_t t0 = 1700000000, t1 = t0 + 1000, t2 = t0 + 2000, t3 = t0 + 3000, t4 = t0 + 4000, t5 = t0 + 5000;
  uint64_t m = 0, c = 0, put = 0;
  auto expect = [&](const char* path, uint64_t modified, uint64_t created) {
    if (!times_of(fs, path, &m, &c)) { printf("FAILED [%s]: stat %s\n", label, path); ++g_failed; return; }
    if (m != modified || (created && c != created)) { printf("FAILED [%s]: %s modified %llu created %llu, want %llu / %llu\n", label, path, static_cast<unsigned long long>(m), static_cast<unsigned long long>(c), static_cast<unsigned long long>(modified), static_cast<unsigned long long>(created)); ++g_failed; }
  };
  set_unix_clock(fake_clock, nullptr);
  g_now = t0;
  CHECK_STATUS(fs.create_directory("/d"), Status::Ok);
  CHECK_STATUS(fs.create_directory("/e"), Status::Ok);
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/d/f", OpenMode::CreateOrOpen, &f), Status::Ok);
    const uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK_STATUS(f->write(0, ConstByteView(data, 10), &put), Status::Ok);
  }
  expect("/d/f", t0, t0);
  expect("/d", t0, t0);
  g_now = t1;  // writing changes the file, not its folder
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/d/f", OpenMode::ReadWrite, &f), Status::Ok);
    const uint8_t one = 9;
    CHECK_STATUS(f->write(20, ConstByteView(&one, 1), &put), Status::Ok);
  }
  expect("/d/f", t1, t0);
  expect("/d", t0, t0);
  g_now = t2;  // a new name changes the folder
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/d/g", OpenMode::CreateOrOpen, &f), Status::Ok);
  }
  expect("/d", t2, t0);
  expect("/d/f", t1, t0);
  expect("/d/g", t2, t2);
  g_now = t3;  // a rename changes both folders and leaves the file
  CHECK_STATUS(fs.rename("/d/f", "/e/h"), Status::Ok);
  expect("/d", t3, 0);
  expect("/e", t3, 0);
  expect("/e/h", t1, t0);
  g_now = t4;  // cutting changes the file
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/e/h", OpenMode::ReadWrite, &f), Status::Ok);
    CHECK_STATUS(f->resize(3), Status::Ok);
  }
  expect("/e/h", t4, t0);
  expect("/e", t3, 0);
  g_now = t5;  // removing a name changes the folder
  CHECK_STATUS(fs.remove("/d/g"), Status::Ok);
  expect("/d", t5, 0);
  // nothing is stamped without a clock
  set_unix_clock(nullptr, nullptr);
  {
    UniquePointer<IFile> f;
    CHECK_STATUS(fs.open_file("/e/h", OpenMode::ReadWrite, &f), Status::Ok);
    const uint8_t one = 7;
    CHECK_STATUS(f->write(0, ConstByteView(&one, 1), &put), Status::Ok);
  }
  expect("/e/h", t4, t0);
}

}  // namespace filetimes
