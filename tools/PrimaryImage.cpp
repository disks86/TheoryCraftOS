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

// Fills the primary disk image of the OS with the files that belong on it (the Windows libraries in /Windows/Windows/System32, say), using the OS's own FAT driver on
// the image file. The image must already be a FAT volume (mkfs.fat); files that exist are replaced, folders that exist are kept.
// Usage: primary_image <image> [dir:/folder]... [file:/path/on/image=host/file]...   (folders are made on the way, so dir: is only needed for empty ones)
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "FileSystem/Fat/FatFileSystem.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"

using namespace filesystem;

struct HostAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t align) override { return aligned_alloc(align, (size + align - 1) / align * align); }
  void deallocate(void* p) override { free(p); }
};

// An image file as a disk of 512-byte blocks.
class FileDisk final : public hardware::IStorageDevice {
 public:
  explicit FileDisk(int fd, uint64_t bytes) : fd_(fd), blocks_(bytes / 512) {}
  const char* name() const override { return "image file"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  hardware::StorageInfo info() const override {
    hardware::StorageInfo i;
    i.block_count = blocks_;
    i.block_size = 512;
    return i;
  }
  hardware::StorageStatistics statistics() const override { return {}; }
  bool read(uint64_t lba, ByteView buffer) override {
    if (buffer.size() % 512 || lba + buffer.size() / 512 > blocks_) return false;
    return pread(fd_, buffer.data(), buffer.size(), off_t(lba * 512)) == ssize_t(buffer.size());
  }
  bool write(uint64_t lba, ConstByteView buffer) override {
    if (buffer.size() % 512 || lba + buffer.size() / 512 > blocks_) return false;
    return pwrite(fd_, buffer.data(), buffer.size(), off_t(lba * 512)) == ssize_t(buffer.size());
  }
  bool flush() override { return fsync(fd_) == 0; }

 private:
  int fd_;
  uint64_t blocks_;
};

static bool make_folders(IFileSystem& fs, const std::string& folder) {
  for (size_t at = 1; at <= folder.size(); ++at) {
    if (at != folder.size() && folder[at] != '/') continue;
    const std::string part = folder.substr(0, at);
    DirectoryEntry entry;
    if (fs.stat(StringView(part.data(), part.size()), &entry) == Status::Ok) {
      if (entry.type != NodeType::Directory) return false;
      continue;
    }
    const Status status = fs.create_directory(StringView(part.data(), part.size()));
    if (status != Status::Ok && status != Status::AlreadyExists) return false;
  }
  return true;
}

static bool put_file(IFileSystem& fs, const std::string& path, const std::string& host_file) {
  FILE* in = fopen(host_file.c_str(), "rb");
  if (!in) { fprintf(stderr, "primary_image: cannot read %s\n", host_file.c_str()); return false; }
  std::vector<uint8_t> bytes;
  uint8_t chunk[65536];
  for (size_t n; (n = fread(chunk, 1, sizeof chunk, in)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(in);
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0 ? false : !make_folders(fs, path.substr(0, slash))) { fprintf(stderr, "primary_image: cannot make the folder of %s\n", path.c_str()); return false; }
  UniquePointer<IFile> file;
  if (fs.open_file(StringView(path.data(), path.size()), OpenMode::CreateTruncate, &file) != Status::Ok) { fprintf(stderr, "primary_image: cannot create %s\n", path.c_str()); return false; }
  for (uint64_t done = 0; done < bytes.size();) {
    uint64_t wrote = 0;
    const uint64_t want = bytes.size() - done < (1u << 20) ? bytes.size() - done : (1u << 20);
    if (file->write(done, ConstByteView(bytes.data() + done, want), &wrote) != Status::Ok || wrote == 0) { fprintf(stderr, "primary_image: cannot write %s (the image is full?)\n", path.c_str()); return false; }
    done += wrote;
  }
  return file->flush() == Status::Ok;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: primary_image <image> [dir:/folder] [file:/path=host-file]...\n"); return 2; }
  const int fd = open(argv[1], O_RDWR);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) { fprintf(stderr, "primary_image: cannot open %s\n", argv[1]); return 1; }
  HostAllocator allocator;
  FileDisk disk(fd, uint64_t(st.st_size));
  UniquePointer<IFileSystem> fs;
  if (FatFileSystem::mount(allocator, disk, 0, &fs) != Status::Ok || !fs) { fprintf(stderr, "primary_image: %s is not a FAT volume the OS can mount\n", argv[1]); return 1; }
  bool ok = true;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("dir:", 0) == 0) {
      ok = make_folders(*fs, arg.substr(4)) && ok;
    } else if (arg.rfind("file:", 0) == 0) {
      const size_t eq = arg.find('=');
      if (eq == std::string::npos) { fprintf(stderr, "primary_image: %s needs =host-file\n", arg.c_str()); ok = false; continue; }
      ok = put_file(*fs, arg.substr(5, eq - 5), arg.substr(eq + 1)) && ok;
    } else {
      fprintf(stderr, "primary_image: unknown argument %s\n", arg.c_str());
      ok = false;
    }
  }
  fs.reset();
  disk.flush();
  close(fd);
  return ok ? 0 : 1;
}
