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
#include "Process/LinuxFiles.hpp"
#include "Process/AddressSpace.hpp"

namespace process::linuxabi {

namespace {

using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

constexpr int64_t kEperm = 1, kEnoent = 2, kEio = 5, kEbadf = 9, kEnomem = 12, kEacces = 13, kEfault = 14, kEbusy = 16, kEexist = 17, kEnodev = 19, kEnotdir = 20,
                  kEisdir = 21, kEinval = 22, kEmfile = 24, kEnotty = 25, kEfbig = 27, kEnospc = 28, kEspipe = 29, kErofs = 30, kErange = 34, kEnametoolong = 36,
                  kEnosys = 38, kEnotempty = 39, kEopnotsupp = 95;
constexpr uint64_t kAtFdCwd = ~uint64_t{99};      // -100
constexpr uint64_t kAtRemoveDir = 0x200, kAtEmptyPath = 0x1000;
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u, kIovMax = 1024, kChunk = 4096;
// open(2) flags
constexpr uint64_t kOAccMode = 3, kOWrite = 1, kORdWr = 2, kOCreat = 0x40, kOExcl = 0x80, kOTrunc = 0x200, kOAppend = 0x400, kOCloexec = 0x80000;

uint64_t error(int64_t value) { return uint64_t(-value); }


uint64_t errno_of(Status status) {
  switch (status) {
    case Status::NotFound: return error(kEnoent);
    case Status::AlreadyExists: return error(kEexist);
    case Status::NotADirectory: return error(kEnotdir);
    case Status::IsADirectory: return error(kEisdir);
    case Status::NotEmpty: return error(kEnotempty);
    case Status::Busy: return error(kEbusy);
    case Status::ReadOnly: return error(kErofs);
    case Status::NoSpace: return error(kEnospc);
    case Status::InvalidPath: return error(kEnametoolong);
    case Status::InvalidArgument: return error(kEinval);
    case Status::Unsupported: return error(kEopnotsupp);
    case Status::OutOfMemory: return error(kEnomem);
    case Status::NoMedia: return error(kEnodev);
    case Status::AccessDenied:
    case Status::LoginRequired:
    case Status::LoginFailed: return error(kEacces);
    default: return error(kEio);
  }
}

uint64_t errno_of_or_zero(Status status);

// Reads a NUL-terminated string from the program. 0, or -errno.
int64_t read_path(Process& p, uint64_t address, String* out) {
  out->clear();
  if (address == 0) return -kEfault;
  for (uint64_t at = address;;) {
    uint8_t chunk[64];
    uint64_t n = sizeof(chunk);
    // A chunk that runs off the end of the mapping is read byte by byte.
    if (!p.read_user(at, ByteView(chunk, n))) {
      n = 0;
      while (n < sizeof(chunk) && p.read_user(at + n, ByteView(chunk + n, 1))) {
        ++n;
        if (chunk[n - 1] == 0) break;
      }
      if (n == 0 || (n < sizeof(chunk) && chunk[n - 1] != 0)) return -kEfault;
    }
    for (uint64_t i = 0; i < n; ++i) {
      if (chunk[i] == 0) return 0;
      if (out->size() >= kMaxPath || !out->push_back(static_cast<char>(chunk[i]))) return -kEnametoolong;
    }
    if (at > ~uint64_t{0} - n) return -kEfault;
    at += n;
  }
}

uint64_t errno_of_or_zero(Status status) { return status == Status::Ok ? 0 : errno_of(status); }

int64_t path_error(PathResult r) {
  switch (r) {
    case PathResult::Empty:
    case PathResult::Invalid: return -kEnoent;
    default: return -kEnametoolong;
  }
}

// The cleaned virtual path of (dirfd, path). `allow_empty`: an empty path names dirfd itself (AT_EMPTY_PATH). 0, or -errno.
int64_t resolve_at(Process& p, uint64_t dirfd, uint64_t path_address, bool allow_empty, String* virtual_path) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return -kEnoent;
  String text(files.allocator());
  int64_t result = read_path(p, path_address, &text);
  if (result < 0) return result;
  StringView base = files.cwd();
  if (text.empty() || text.view()[0] != '/') {
    if (dirfd != kAtFdCwd) {
      OpenObject* dir = files.object(dirfd);
      if (!dir) return files.kind(dirfd) == DescriptorKind::Closed ? -kEbadf : -kEnotdir;
      if (!dir->directory) return -kEnotdir;
      base = dir->path.view();
    }
  }
  if (text.empty()) {
    if (!allow_empty) return -kEnoent;
    return virtual_path->assign(base) ? 0 : -kEnomem;
  }
  const PathResult r = files.resolve_from(base, text.view(), virtual_path);
  return r == PathResult::Ok ? 0 : path_error(r);
}

DescriptorKind kind_of(Process& p, uint64_t fd) { return p.files().kind(fd); }

uint64_t fnv(StringView text) {
  uint64_t h = 1469598103934665603ull;
  for (char c : text) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
  return h | 1;
}

void put(ByteView out, uint64_t at, uint64_t value, uint32_t bytes) {
  for (uint32_t i = 0; i < bytes; ++i) out[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

constexpr uint32_t kSIfDir = 040000, kSIfReg = 0100000, kSIfChr = 020000;

struct Meta {
  uint32_t mode = 0;
  uint64_t size = 0, ino = 0, modified = 0, created = 0;
};

Meta meta_of(const DirectoryEntry& e, StringView path) {
  Meta m;
  const bool dir = e.type == NodeType::Directory;
  m.mode = dir ? (kSIfDir | (e.read_only ? 0555 : 0755)) : (kSIfReg | (e.read_only ? 0444 : 0644));
  m.size = dir ? 4096 : e.size;
  m.ino = fnv(path);
  m.modified = e.modified;
  m.created = e.created ? e.created : e.modified;
  return m;
}
Meta device_meta(uint32_t mode, uint64_t ino) {
  Meta m;
  m.mode = mode;
  m.ino = ino;
  return m;
}

// struct stat as the kernel of each CPU lays it out (144 bytes on x86-64, 128 on the others).
uint64_t write_stat(Process& p, UserArch arch, const Meta& m, uint64_t address) {
  uint8_t buffer[144] = {};
  ByteView out(buffer, sizeof(buffer));
  const bool x86 = arch == UserArch::X86_64;
  put(out, 8, m.ino, 8);
  put(out, x86 ? 16 : 20, 1, x86 ? 8 : 4);                  // st_nlink
  put(out, x86 ? 24 : 16, m.mode, 4);
  put(out, 48, m.size, 8);
  put(out, 56, 4096, x86 ? 8 : 4);                          // st_blksize
  put(out, 64, (m.size + 511) / 512, 8);                    // st_blocks
  put(out, 72, m.modified, 8);                              // st_atime
  put(out, 88, m.modified, 8);                              // st_mtime
  put(out, 104, m.created, 8);                              // st_ctime
  return p.write_user(address, ConstByteView(buffer, x86 ? 144 : 128)) ? 0 : error(kEfault);
}

// What a descriptor looks like to fstat. 0 and the metadata, or -errno.
int64_t meta_of_fd(Process& p, uint64_t fd, Meta* m) {
  switch (kind_of(p, fd)) {
    case DescriptorKind::Closed: return -kEbadf;
    case DescriptorKind::ConsoleInput:
    case DescriptorKind::ConsoleOutput:
    case DescriptorKind::ConsoleError: *m = device_meta(kSIfChr | 0620, 1); return 0;
    case DescriptorKind::Null: *m = device_meta(kSIfChr | 0666, 3); return 0;
    case DescriptorKind::Zero: *m = device_meta(kSIfChr | 0666, 5); return 0;
    case DescriptorKind::Random: *m = device_meta(kSIfChr | 0666, 9); return 0;
    default: break;
  }
  OpenObject* object = p.files().object(fd);
  DirectoryEntry entry;
  const Status status = p.files().stat_object(*object, &entry);
  if (status != Status::Ok) return static_cast<int64_t>(errno_of(status));
  *m = meta_of(entry, object->path.view());
  return 0;
}

}  // namespace

uint64_t sys_openat(Process& p, UserArch arch, uint64_t dirfd, uint64_t path, uint64_t flags) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  const bool cloexec = flags & kOCloexec;
  const DescriptorKind device = files.device_at(virtual_path.view());
  if (device != DescriptorKind::Closed) {
    const int64_t fd = device == DescriptorKind::ConsoleOutput ? files.duplicate(1, 0) : files.open_device(device);
    if (fd < 0) return error(kEmfile);
    files.set_close_on_exec(static_cast<uint64_t>(fd), cloexec);
    return static_cast<uint64_t>(fd);
  }
  OpenFlags f;
  const uint64_t access = flags & kOAccMode;
  f.read = access != kOWrite;
  f.write = access == kOWrite || access == kORdWr;
  f.create = flags & kOCreat;
  f.exclusive = flags & kOExcl;
  f.truncate = (flags & kOTrunc) && f.write;
  f.append = flags & kOAppend;
  f.must_be_directory = flags & (arch == UserArch::X86_64 ? 0x10000u : 0x4000u);      // O_DIRECTORY
  uint32_t slot = 0;
  const Status status = files.open(virtual_path.view(), f, &slot);
  if (status == Status::OutOfMemory && files.free_slot(0) < 0) return error(kEmfile);
  if (status != Status::Ok) return errno_of(status);
  files.set_close_on_exec(slot, cloexec);
  return slot;
}

namespace {

// Moves bytes from a file (or device) to the program, in pieces.
uint64_t read_from(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, bool positional, uint64_t offset) {
  if (count > kMaxIoBytes) count = kMaxIoBytes;
  ProcessFiles& files = p.files();
  switch (files.kind(fd)) {
    case DescriptorKind::Closed: return error(kEbadf);
    case DescriptorKind::ConsoleInput: {
      if (positional) return error(kEspipe);
      bool faulted = false;
      const uint64_t got = copy_console_to_user(p, buffer, count, &faulted);
      return faulted ? error(kEfault) : got;
    }
    case DescriptorKind::ConsoleOutput:
    case DescriptorKind::ConsoleError: return error(kEbadf);
    case DescriptorKind::Directory: return error(kEisdir);
    case DescriptorKind::Null: return 0;
    case DescriptorKind::Zero:
    case DescriptorKind::Random: {
      uint8_t chunk[kChunk];
      uint64_t done = 0;
      while (done < count) {
        const uint64_t n = count - done < sizeof(chunk) ? count - done : sizeof(chunk);
        if (files.kind(fd) == DescriptorKind::Random) {
          if (!p.host().random_bytes(ByteView(chunk, n))) return done ? done : error(kEio);
        } else {
          ByteView(chunk, n).fill(0);
        }
        if (!p.write_user(buffer + done, ConstByteView(chunk, n))) return done ? done : error(kEfault);
        done += n;
      }
      return done;
    }
    default: break;
  }
  OpenObject& object = *files.object(fd);
  uint64_t at = positional ? offset : object.position, done = 0;
  uint8_t chunk[kChunk];
  while (done < count) {
    const uint64_t n = count - done < sizeof(chunk) ? count - done : sizeof(chunk);
    uint64_t got = 0;
    const Status status = files.read(object, at, ByteView(chunk, n), &got);
    if (status != Status::Ok) return done ? done : (status == Status::AccessDenied ? error(kEbadf) : errno_of(status));      // not opened for reading: EBADF
    if (got == 0) break;
    if (!p.write_user(buffer + done, ConstByteView(chunk, got))) return done ? done : error(kEfault);
    done += got;
    at += got;
    if (got < n) break;
  }
  if (!positional) object.position = at;
  return done;
}

uint64_t write_to(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, bool positional, uint64_t offset) {
  if (count > kMaxIoBytes) count = kMaxIoBytes;
  ProcessFiles& files = p.files();
  switch (files.kind(fd)) {
    case DescriptorKind::Closed:
    case DescriptorKind::ConsoleInput: return error(kEbadf);
    case DescriptorKind::ConsoleOutput:
    case DescriptorKind::ConsoleError: {
      if (positional) return error(kEspipe);
      bool faulted = false;
      const uint64_t done = copy_user_to_console(p, files.kind(fd) == DescriptorKind::ConsoleError ? 2 : 1, buffer, count, &faulted);
      return faulted && done == 0 ? error(kEfault) : done;
    }
    case DescriptorKind::Directory: return error(kEisdir);
    case DescriptorKind::Null:
    case DescriptorKind::Zero:
    case DescriptorKind::Random: return count;
    default: break;
  }
  OpenObject& object = *files.object(fd);
  if (!object.writable) return error(kEbadf);
  uint64_t at = positional ? offset : object.position, done = 0;
  uint8_t chunk[kChunk];
  while (done < count) {
    const uint64_t n = count - done < sizeof(chunk) ? count - done : sizeof(chunk);
    if (!p.read_user(buffer + done, ByteView(chunk, n))) return done ? done : error(kEfault);
    uint64_t wrote = 0;
    const Status status = files.write(object, at, ConstByteView(chunk, n), &wrote);
    done += wrote;
    at += wrote;
    if (status != Status::Ok) {
      if (!positional) object.position = at;
      return done ? done : errno_of(status);
    }
    if (object.append && !positional) at = files.size_of(object);
  }
  if (!positional) object.position = object.append ? files.size_of(object) : at;
  return done;
}

uint64_t vector_io(Process& p, uint64_t fd, uint64_t iov, uint64_t count, bool write) {
  if (count > kIovMax) return error(kEinval);
  if (p.descriptor(fd) == DescriptorKind::Closed) return error(kEbadf);
  uint64_t total = 0;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t base = 0, length = 0;
    if (iov > ~uint64_t{0} - i * 16 - 15) return total ? total : error(kEfault);
    if (!p.read_user_u64(iov + i * 16, &base) || !p.read_user_u64(iov + i * 16 + 8, &length)) return total ? total : error(kEfault);
    if (length > kMaxIoBytes - total) return total ? total : error(kEinval);
    const uint64_t result = write ? write_to(p, fd, base, length, false, 0) : read_from(p, fd, base, length, false, 0);
    if (int64_t(result) < 0) return total ? total : result;
    total += result;
    if (result < length) break;      // a short transfer ends the call
  }
  return total;
}

}  // namespace

uint64_t sys_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) { return read_from(p, fd, buffer, count, false, 0); }
uint64_t sys_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) { return write_to(p, fd, buffer, count, false, 0); }
uint64_t sys_pread(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset) {
  if (int64_t(offset) < 0) return error(kEinval);
  return read_from(p, fd, buffer, count, true, offset);
}
uint64_t sys_pwrite(Process& p, uint64_t fd, uint64_t buffer, uint64_t count, uint64_t offset) {
  if (int64_t(offset) < 0) return error(kEinval);
  return write_to(p, fd, buffer, count, true, offset);
}
uint64_t sys_readv(Process& p, uint64_t fd, uint64_t iov, uint64_t count) { return vector_io(p, fd, iov, count, false); }
uint64_t sys_writev(Process& p, uint64_t fd, uint64_t iov, uint64_t count) { return vector_io(p, fd, iov, count, true); }

uint64_t sys_lseek(Process& p, uint64_t fd, uint64_t offset, uint64_t whence) {
  ProcessFiles& files = p.files();
  const DescriptorKind kind = files.kind(fd);
  if (kind == DescriptorKind::Closed) return error(kEbadf);
  if (kind == DescriptorKind::Null || kind == DescriptorKind::Zero || kind == DescriptorKind::Random) return 0;
  if (kind != DescriptorKind::File && kind != DescriptorKind::Directory) return error(kEspipe);
  OpenObject& object = *files.object(fd);
  if (kind == DescriptorKind::Directory) {
    if (whence > 1 || offset != 0) return error(kEinval);
    files.rewind(object);
    return 0;
  }
  int64_t base = 0;
  if (whence == 1) base = static_cast<int64_t>(object.position);
  else if (whence == 2) base = static_cast<int64_t>(files.size_of(object));
  else if (whence != 0) return error(kEinval);
  const int64_t delta = static_cast<int64_t>(offset);
  if ((delta > 0 && base > INT64_MAX - delta) || (delta < 0 && base < -delta)) return error(delta < 0 ? kEinval : kEfbig);
  object.position = static_cast<uint64_t>(base + delta);
  return object.position;
}

uint64_t sys_fstat(Process& p, UserArch arch, uint64_t fd, uint64_t buffer) {
  Meta m;
  const int64_t r = meta_of_fd(p, fd, &m);
  return r < 0 ? error(-r) : write_stat(p, arch, m, buffer);
}

uint64_t sys_statat(Process& p, UserArch arch, uint64_t dirfd, uint64_t path, uint64_t buffer, uint64_t flags) {
  Meta m;
  if (flags & kAtEmptyPath) {
    uint8_t first = 1;
    if (path == 0 || (p.read_user(path, ByteView(&first, 1)) && first == 0)) return sys_fstat(p, arch, dirfd, buffer);
  }
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  const DescriptorKind device = files.device_at(virtual_path.view());
  if (device != DescriptorKind::Closed) return write_stat(p, arch, device_meta(kSIfChr | 0666, 3), buffer);
  DirectoryEntry entry;
  const Status status = files.stat(virtual_path.view(), &entry);
  if (status != Status::Ok) return errno_of(status);
  return write_stat(p, arch, meta_of(entry, virtual_path.view()), buffer);
}

uint64_t sys_statx(Process& p, uint64_t dirfd, uint64_t path, uint64_t flags, uint64_t mask, uint64_t buffer) {
  (void)mask;
  Meta m;
  ProcessFiles& files = p.files();
  uint8_t first = 1;
  if ((flags & kAtEmptyPath) && (path == 0 || (p.read_user(path, ByteView(&first, 1)) && first == 0))) {
    const int64_t r = meta_of_fd(p, dirfd, &m);
    if (r < 0) return error(-r);
  } else {
    if (!files.attached()) return error(kEnoent);
    String virtual_path(files.allocator());
    const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
    if (r < 0) return error(-r);
    DirectoryEntry entry;
    const DescriptorKind device = files.device_at(virtual_path.view());
    if (device != DescriptorKind::Closed) {
      m = device_meta(kSIfChr | 0666, 3);
    } else {
      const Status status = files.stat(virtual_path.view(), &entry);
      if (status != Status::Ok) return errno_of(status);
      m = meta_of(entry, virtual_path.view());
    }
  }
  uint8_t out[256] = {};
  ByteView v(out, sizeof(out));
  put(v, 0, 0x7FF, 4);              // stx_mask: the basic statistics
  put(v, 4, 4096, 4);               // stx_blksize
  put(v, 16, 1, 4);                 // stx_nlink
  put(v, 28, m.mode, 2);
  put(v, 32, m.ino, 8);
  put(v, 40, m.size, 8);
  put(v, 48, (m.size + 511) / 512, 8);
  put(v, 64, m.modified, 8);        // stx_atime.tv_sec
  put(v, 96, m.created, 8);         // stx_ctime
  put(v, 112, m.modified, 8);       // stx_mtime
  return p.write_user(buffer, ConstByteView(out, sizeof(out))) ? 0 : error(kEfault);
}

uint64_t sys_getdents64(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) {
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  if (files.kind(fd) != DescriptorKind::Directory) return error(kEnotdir);
  OpenObject& object = *files.object(fd);
  uint64_t used = 0;
  for (;;) {
    DirectoryEntry entry;
    const Status status = files.next_entry(object, &entry);
    if (status == Status::EndOfDirectory) break;
    if (status != Status::Ok) return used ? used : errno_of(status);
    const StringView name = entry.name.view();
    const uint64_t length = (19 + name.size() + 1 + 7) & ~uint64_t{7};
    if (length > count - used) {
      object.pending = entry;
      object.has_pending = true;
      if (used == 0) return error(kEinval);      // not even one entry fits
      break;
    }
    uint8_t record[512] = {};
    ByteView v(record, sizeof(record));
    put(v, 0, fnv(name), 8);                       // d_ino
    put(v, 8, ++object.directory_index, 8);        // d_off: where the next call resumes
    put(v, 16, length, 2);                         // d_reclen
    v[18] = entry.type == NodeType::Directory ? 4 : 8;      // DT_DIR / DT_REG
    for (uint64_t i = 0; i < name.size(); ++i) v[19 + i] = static_cast<uint8_t>(name[i]);
    if (!p.write_user(buffer + used, ConstByteView(record, length))) return used ? used : error(kEfault);
    used += length;
  }
  return used;
}

uint64_t sys_mkdirat(Process& p, uint64_t dirfd, uint64_t path) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  if (virtual_path.view() == StringView("/") || files.device_at(virtual_path.view()) != DescriptorKind::Closed) return error(kEexist);
  return errno_of_or_zero(files.make_directory(virtual_path.view()));
}

uint64_t sys_unlinkat(Process& p, uint64_t dirfd, uint64_t path, uint64_t flags) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  DirectoryEntry entry;
  const Status found = files.stat(virtual_path.view(), &entry);
  if (found != Status::Ok) return errno_of(found);
  const bool directory = entry.type == NodeType::Directory;
  if ((flags & kAtRemoveDir) && !directory) return error(kEnotdir);
  if (!(flags & kAtRemoveDir) && directory) return error(kEisdir);
  if (virtual_path.view() == StringView("/")) return error(kEbusy);
  return errno_of_or_zero(files.remove(virtual_path.view()));
}

uint64_t sys_renameat(Process& p, uint64_t old_dirfd, uint64_t old_path, uint64_t new_dirfd, uint64_t new_path) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String from(files.allocator()), to(files.allocator());
  int64_t r = resolve_at(p, old_dirfd, old_path, false, &from);
  if (r >= 0) r = resolve_at(p, new_dirfd, new_path, false, &to);
  if (r < 0) return error(-r);
  DirectoryEntry source, target;
  const Status found = files.stat(from.view(), &source);
  if (found != Status::Ok) return errno_of(found);
  if (from.view() == to.view()) return 0;
  // POSIX replaces an existing target (a file by a file, an empty directory by a directory).
  if (files.stat(to.view(), &target) == Status::Ok) {
    if (source.type == NodeType::Directory && target.type != NodeType::Directory) return error(kEnotdir);
    if (source.type != NodeType::Directory && target.type == NodeType::Directory) return error(kEisdir);
    const Status removed = files.remove(to.view());
    if (removed != Status::Ok) return errno_of(removed);
  }
  return errno_of_or_zero(files.rename(from.view(), to.view()));
}

uint64_t sys_getcwd(Process& p, uint64_t buffer, uint64_t size) {
  const StringView cwd = p.files().cwd();
  if (size < cwd.size() + 1) return error(kErange);
  uint8_t text[kMaxPath + 1];
  for (uint64_t i = 0; i < cwd.size(); ++i) text[i] = static_cast<uint8_t>(cwd[i]);
  text[cwd.size()] = 0;
  return p.write_user(buffer, ConstByteView(text, cwd.size() + 1)) ? cwd.size() + 1 : error(kEfault);
}

uint64_t sys_chdir(Process& p, uint64_t path) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, kAtFdCwd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  return errno_of_or_zero(files.change_directory(virtual_path.view()));
}

uint64_t sys_fchdir(Process& p, uint64_t fd) {
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  if (files.kind(fd) != DescriptorKind::Directory) return error(kEnotdir);
  String path(files.allocator());
  path.assign(files.object(fd)->path.view());
  return errno_of_or_zero(files.change_directory(path.view()));
}

uint64_t sys_faccessat(Process& p, uint64_t dirfd, uint64_t path, uint64_t mode) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  if (files.device_at(virtual_path.view()) != DescriptorKind::Closed) return 0;
  DirectoryEntry entry;
  const Status status = files.stat(virtual_path.view(), &entry);
  if (status != Status::Ok) return errno_of(status);
  if ((mode & 2) && entry.read_only) return error(kEacces);
  return 0;
}

uint64_t sys_truncate(Process& p, uint64_t path, uint64_t length) {
  if (int64_t(length) < 0) return error(kEinval);
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, kAtFdCwd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  OpenFlags f;
  f.write = true;
  uint32_t slot = 0;
  const Status opened = files.open(virtual_path.view(), f, &slot);
  if (opened != Status::Ok) return errno_of(opened);
  const Status status = files.resize(*files.object(slot), length);
  files.close(slot);
  return errno_of_or_zero(status);
}

uint64_t sys_ftruncate(Process& p, uint64_t fd, uint64_t length) {
  if (int64_t(length) < 0) return error(kEinval);
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  if (files.kind(fd) != DescriptorKind::File) return error(kEinval);
  OpenObject& object = *files.object(fd);
  if (!object.writable) return error(kEinval);
  return errno_of_or_zero(files.resize(object, length));
}

uint64_t sys_fsync(Process& p, uint64_t fd) {
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  if (files.kind(fd) != DescriptorKind::File) return 0;
  return errno_of_or_zero(files.flush(*files.object(fd)));
}

uint64_t sys_dup(Process& p, uint64_t fd) {
  if (p.descriptor(fd) == DescriptorKind::Closed) return error(kEbadf);
  const int64_t target = p.files().duplicate(fd, 0);
  return target < 0 ? error(kEmfile) : static_cast<uint64_t>(target);
}

uint64_t sys_dup3(Process& p, uint64_t old_fd, uint64_t new_fd, uint64_t flags) {
  if (p.descriptor(old_fd) == DescriptorKind::Closed) return error(kEbadf);
  if (new_fd >= ProcessFiles::kMaxDescriptors) return error(kEbadf);
  if (flags == ~uint64_t{0}) {      // dup2: the same descriptor is simply returned
    if (old_fd == new_fd) return new_fd;
    flags = 0;
  } else if (old_fd == new_fd || (flags & ~kOCloexec)) {
    return error(kEinval);
  }
  p.files().duplicate_to(old_fd, new_fd);
  p.files().set_close_on_exec(new_fd, flags & kOCloexec);
  return new_fd;
}

uint64_t sys_fcntl(Process& p, uint64_t fd, uint64_t command, uint64_t argument) {
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  switch (command) {
    case 0:        // F_DUPFD
    case 1030: {   // F_DUPFD_CLOEXEC
      if (argument >= ProcessFiles::kMaxDescriptors) return error(kEinval);
      const int64_t target = files.duplicate(fd, argument);
      if (target < 0) return error(kEmfile);
      files.set_close_on_exec(static_cast<uint64_t>(target), command == 1030);
      return static_cast<uint64_t>(target);
    }
    case 1: return files.close_on_exec(fd) ? 1 : 0;      // F_GETFD: FD_CLOEXEC
    case 2: files.set_close_on_exec(fd, argument & 1); return 0;
    case 3: {      // F_GETFL
      OpenObject* object = files.object(fd);
      if (!object) return files.kind(fd) == DescriptorKind::ConsoleInput ? 0 : kORdWr;
      return (object->readable && object->writable ? kORdWr : object->writable ? kOWrite : 0) | (object->append ? kOAppend : 0);
    }
    case 4: return 0;      // F_SETFL: nothing here can change (no non-blocking mode)
    default: return error(kEinval);
  }
}

uint64_t sys_readlinkat(Process& p, uint64_t dirfd, uint64_t path) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  DirectoryEntry entry;
  const Status status = files.stat(virtual_path.view(), &entry);
  return status == Status::Ok ? error(kEinval) : errno_of(status);      // there are no symbolic links: what exists is not one
}

uint64_t sys_ioctl(Process& p, uint64_t fd) { return p.descriptor(fd) == DescriptorKind::Closed ? error(kEbadf) : error(kEnotty); }

}  // namespace process::linuxabi
