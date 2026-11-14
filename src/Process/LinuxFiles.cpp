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
#include "Process/LinuxSockets.hpp"
#include "Process/LinuxFiles.hpp"
#include "Utility/ByteText.hpp"
#include "Process/AddressSpace.hpp"
#include "Process/DxgDevice.h"
#include "Process/Fifo.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Objects.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

namespace process::linuxabi {

namespace {

using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

constexpr int64_t kEperm = 1, kEnoent = 2, kEio = 5, kEbadf = 9, kEnomem = 12, kEacces = 13, kEfault = 14, kEbusy = 16, kEexist = 17, kEnodev = 19, kEnotdir = 20,
                  kEisdir = 21, kEinval = 22, kEmfile = 24, kEnotty = 25, kEfbig = 27, kEnospc = 28, kEspipe = 29, kErofs = 30, kErange = 34, kEnametoolong = 36,
                  kEnosys = 38, kEnotempty = 39, kEopnotsupp = 95, kEloop = 40;
constexpr uint64_t kAtFdCwd = ~uint64_t{99};      // -100
constexpr uint64_t kAtRemoveDir = 0x200, kAtEmptyPath = 0x1000;
constexpr uint64_t kMaxIoBytes = 0x7FFFF000u, kIovMax = 1024, kChunk = 4096;
// open(2) flags
constexpr uint64_t kOAccMode = 3, kOWrite = 1, kORdWr = 2, kOCreat = 0x40, kOExcl = 0x80, kOTrunc = 0x200, kOAppend = 0x400, kOCloexec = 0x80000, kONonBlock = 0x800;

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
    case Status::TooManyLinks: return error(kEloop);
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

}  // namespace

uint64_t errno_of_status(Status status) { return errno_of(status); }

namespace {

int64_t path_error(PathResult r) {
  switch (r) {
    case PathResult::Empty:
    case PathResult::Invalid: case PathResult::UnknownDrive: return -kEnoent;
    default: return -kEnametoolong;
  }
}

// The cleaned virtual path of (dirfd, path), as it is spelled (/proc/self/exe stays that). `allow_empty`: an empty path names dirfd itself (AT_EMPTY_PATH).
// 0, or -errno.
int64_t resolve_raw(Process& p, uint64_t dirfd, uint64_t path_address, bool allow_empty, String* virtual_path) {
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

// --- /proc/self: the links Linux keeps there for a process (its program file, its current folder, its descriptors). There is no /proc file system; these
// few names are answered here, which is what programs look at to find themselves (Rust's current_exe, glibc's $ORIGIN, musl's realpath, AppImage
// launchers such as sharun).

// Does `path` name one of the process's own /proc entries? *what is what follows "/proc/<self>/" ("exe", "fd/3", "cwd/x"). "self", "thread-self" and the
// process's own number name the process.
bool proc_self_entry(Process& p, StringView path, StringView* what) {
  const StringView prefix("/proc/");
  if (!path.starts_with(prefix)) return false;
  StringView rest = path.without_prefix(prefix.size());
  const uint64_t slash = rest.find('/');
  if (slash == StringView::npos) return false;
  const StringView who = rest.first(slash);
  bool self = who.equals("self") || who.equals("thread-self");
  if (!self && !who.empty()) {
    uint64_t number = 0;
    bool digits = true;
    for (char c : who) {
      if (c < '0' || c > '9' || number > (~uint64_t{0} - 9) / 10) { digits = false; break; }
      number = number * 10 + uint64_t(c - '0');
    }
    self = digits && number == p.id();
  }
  if (!self) return false;
  *what = rest.without_prefix(slash + 1);
  return true;
}

// What /proc/self/fd/<n> points at: the file's path, or Linux's names for what has none. 0, or -errno.
int64_t describe_descriptor(Process& p, uint64_t fd, String* out) {
  ProcessFiles& files = p.files();
  auto named = [&](const char* kind, uint64_t number) {
    if (!out->assign(kind) || !out->append(":[") || !out->append_uint(number) || !out->push_back(']')) return -kEnomem;
    return int64_t(0);
  };
  switch (files.kind(fd)) {
    case DescriptorKind::Closed: return -kEnoent;
    case DescriptorKind::ConsoleInput:
    case DescriptorKind::ConsoleOutput:
    case DescriptorKind::ConsoleError: return out->assign("/dev/pts/0") ? 0 : -kEnomem;
    case DescriptorKind::Null: return out->assign("/dev/null") ? 0 : -kEnomem;
    case DescriptorKind::Zero: return out->assign("/dev/zero") ? 0 : -kEnomem;
    case DescriptorKind::Random: return out->assign("/dev/urandom") ? 0 : -kEnomem;
    case DescriptorKind::Dxg: return out->assign("/dev/dxg") ? 0 : -kEnomem;
    case DescriptorKind::UnixSocket: return named(is_pipe_descriptor(p, fd) ? "pipe" : "socket", files.token(fd));
    default: break;
  }
  const OpenObject* object = files.object(fd);
  if (object && !object->memory && !object->gpu_buffer && !object->gpu_sync && !object->path.empty()) return out->assign(object->path.view()) ? 0 : -kEnomem;
  return out->assign("anon_inode:[unknown]") ? 0 : -kEnomem;
}

// The link at a /proc/self path: 1 and *target if `path` is one, 0 if `path` is no such entry, -errno if it is one that cannot be answered.
int64_t proc_self_link(Process& p, StringView path, String* target) {
  StringView what;
  if (!proc_self_entry(p, path, &what)) return 0;
  ProcessFiles& files = p.files();
  if (what.equals("exe")) {
    if (files.executable().empty()) return -kEnoent;
    const Status s = files.canonical(files.executable(), target);
    return s == Status::Ok ? 1 : int64_t(errno_of(s));
  }
  if (what.equals("cwd")) return target->assign(files.cwd()) ? 1 : -kEnomem;
  if (what.equals("root")) return target->assign("/") ? 1 : -kEnomem;
  if (what.starts_with("fd/")) {
    const StringView digits = what.without_prefix(3);
    uint64_t fd = 0;
    if (digits.empty() || digits.size() > 9) return -kEnoent;
    for (char c : digits) {
      if (c < '0' || c > '9') return -kEnoent;
      fd = fd * 10 + uint64_t(c - '0');
    }
    const int64_t r = describe_descriptor(p, fd, target);
    return r < 0 ? r : 1;
  }
  return 0;
}

// Paths through /proc/self that lead into the file tree (the program file, the current folder, the root) become the paths they lead to. 0, or -errno.
int64_t follow_proc_self(Process& p, String* virtual_path) {
  StringView what;
  if (!proc_self_entry(p, virtual_path->view(), &what)) return 0;
  ProcessFiles& files = p.files();
  String base(files.allocator());
  StringView rest;
  if (what.equals("exe")) {
    if (files.executable().empty()) return -kEnoent;
    const Status s = files.canonical(files.executable(), &base);
    if (s != Status::Ok) return int64_t(errno_of(s));
  } else if (what.equals("cwd") || what.starts_with("cwd/")) {
    if (!base.assign(files.cwd())) return -kEnomem;
    rest = what.without_prefix(3);
  } else if (what.equals("root") || what.starts_with("root/")) {
    if (!base.assign("/")) return -kEnomem;
    rest = what.without_prefix(4);
  } else {
    return 0;
  }
  String joined(files.allocator());
  if (!joined.assign(base.view()) || !joined.push_back('/') || !joined.append(rest)) return -kEnomem;
  const PathResult r = files.resolve_from("/", joined.view(), virtual_path);
  return r == PathResult::Ok ? 0 : path_error(r);
}

// The cleaned virtual path of (dirfd, path), /proc/self/exe and the like replaced by what they lead to. 0, or -errno.
int64_t resolve_at(Process& p, uint64_t dirfd, uint64_t path_address, bool allow_empty, String* virtual_path) {
  const int64_t r = resolve_raw(p, dirfd, path_address, allow_empty, virtual_path);
  return r < 0 ? r : follow_proc_self(p, virtual_path);
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

constexpr uint32_t kSIfDir = 040000, kSIfReg = 0100000, kSIfChr = 020000, kSIfSock = 0140000, kSIfIfo = 010000, kSIfBlk = 060000, kSIfMt = 0170000;

struct Meta {
  uint32_t mode = 0;
  uint64_t size = 0, ino = 0, modified = 0, created = 0;
  uint64_t rdev = 0;      // a device node's number (st_rdev)
};

Meta meta_of(const DirectoryEntry& e, StringView path) {
  Meta m;
  const bool dir = e.type == NodeType::Directory;
  m.mode = dir ? (kSIfDir | (e.read_only ? 0555 : 0755)) : (kSIfReg | (e.read_only ? 0444 : 0644));
  m.size = dir ? 4096 : e.size;
  m.ino = fnv(path);
  m.modified = e.modified;
  m.created = e.created ? e.created : e.modified;
  switch (e.special) {      // (a node of a file system that keeps Unix node types: ext)
    case filesystem::SpecialNode::Fifo: m.mode = kSIfIfo | 0644; m.size = 0; break;
    case filesystem::SpecialNode::Socket: m.mode = kSIfSock | 0755; m.size = 0; break;
    case filesystem::SpecialNode::CharDevice: m.mode = kSIfChr | 0644; m.size = 0; break;
    case filesystem::SpecialNode::BlockDevice: m.mode = kSIfBlk | 0644; m.size = 0; break;
    case filesystem::SpecialNode::None: break;
  }
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
  put(out, x86 ? 40 : 32, m.rdev, 8);                      // st_rdev
  put(out, 48, m.size, 8);
  put(out, 56, 4096, x86 ? 8 : 4);                          // st_blksize
  put(out, 64, (m.size + 511) / 512, 8);                    // st_blocks
  put(out, 72, m.modified, 8);                              // st_atime
  put(out, 88, m.modified, 8);                              // st_mtime
  put(out, 104, m.created, 8);                              // st_ctime
  return p.write_user(address, ConstByteView(buffer, x86 ? 144 : 128)) ? 0 : error(kEfault);
}

// A file bound to a unix socket is a socket to stat(), one remembered as a FIFO (Fifo.hpp: a file system without FIFO nodes) a FIFO.
void mark_socket(Process& p, StringView virtual_path, const DirectoryEntry& entry, Meta* m) {
  if ((m->mode & kSIfMt) != kSIfReg) return;
  String host(p.files().allocator());
  if (!p.files().host_path_of(virtual_path, &host)) return;
  if (is_socket_file(host.view())) { m->mode = kSIfSock | 0755; m->size = 0; }
  else if (is_fifo_node(host.view(), entry)) { m->mode = kSIfIfo | 0644; m->size = 0; }
}

constexpr uint32_t kSIfLnk = 0120000;
constexpr uint64_t kAtSymlinkNofollow = 0x100;

// lstat of `virtual_path` when it is a symbolic link (one of the file tree's, or a /proc/self link): true and *m. Its size is the length of its target.
bool link_meta(Process& p, StringView virtual_path, Meta* m) {
  String target(p.files().allocator());
  if (proc_self_link(p, virtual_path, &target) <= 0 && p.files().read_link(virtual_path, &target) != Status::Ok) return false;
  *m = Meta();
  m->mode = kSIfLnk | 0777;
  m->size = target.size();
  m->ino = fnv(virtual_path);
  return true;
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
    case DescriptorKind::Dxg:      // (a character device without a file object, like the others; its number is the one Wayland's dmabuf feedback names)
      *m = device_meta(kSIfChr | 0666, 10);
      m->rdev = TCOS_DXG_DEVICE_NUMBER;
      return 0;
    case DescriptorKind::UnixSocket: *m = device_meta((is_pipe_descriptor(p, fd) ? kSIfIfo | 0600 : kSIfSock | 0777), p.files().token(fd)); return 0;
    default: break;
  }
  OpenObject* object = p.files().object(fd);
  DirectoryEntry entry;
  if (object->memory || object->gpu_buffer || object->gpu_sync) {      // (a file in memory, a GPU image or a fence has no path to look at: a regular file of its size)
    entry.size = object->file->size();
    *m = meta_of(entry, object->path.view());
    return 0;
  }
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
  const uint64_t access = flags & kOAccMode;
  {
    // A named pipe: an end of its pipe (Fifo.hpp), not a file.
    DirectoryEntry entry;
    String host(files.allocator());
    if (files.stat(virtual_path.view(), &entry) == Status::Ok && entry.type == NodeType::File && files.host_path_of(virtual_path.view(), &host) &&
        is_fifo_node(host.view(), entry)) {
      if ((flags & kOCreat) && (flags & kOExcl)) return error(kEexist);
      if (flags & (arch == UserArch::AArch64 ? 0x4000u : 0x10000u)) return error(kEnotdir);
      if (access == kOAccMode) return error(kEinval);
      return fifo_open(p, host.view(), access != kOWrite, access != 0, (flags & kONonBlock) != 0, cloexec);
    }
  }
  OpenFlags f;
  f.read = access != kOWrite;
  f.write = access == kOWrite || access == kORdWr;
  f.create = flags & kOCreat;
  f.exclusive = flags & kOExcl;
  f.truncate = (flags & kOTrunc) && f.write;
  f.append = flags & kOAppend;
  f.must_be_directory = flags & (arch == UserArch::AArch64 ? 0x4000u : 0x10000u);      // O_DIRECTORY (AArch64 has its own value; the generic ABIs of x86-64, RISC-V and LoongArch share one)
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
    case DescriptorKind::Dxg: return error(kEinval);      // (only ioctl)
    case DescriptorKind::UnixSocket: return positional ? error(kEspipe) : socket_read(p, fd, buffer, count);
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
    case DescriptorKind::Dxg: return error(kEinval);      // (only ioctl)
    case DescriptorKind::UnixSocket: return positional ? error(kEspipe) : socket_write(p, fd, buffer, count);
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
  if (p.descriptor(fd) == DescriptorKind::UnixSocket) return socket_vector_io(p, fd, iov, count, write);      // (one message is one readv(), not one per buffer)
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
  const bool no_follow = flags & kAtSymlinkNofollow;
  const int64_t r = no_follow ? resolve_raw(p, dirfd, path, false, &virtual_path) : resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  if (no_follow && link_meta(p, virtual_path.view(), &m)) return write_stat(p, arch, m, buffer);
  if (no_follow && follow_proc_self(p, &virtual_path) < 0) return error(kEnoent);
  const DescriptorKind device = files.device_at(virtual_path.view());
  if (device != DescriptorKind::Closed) return write_stat(p, arch, device_meta(kSIfChr | 0666, 3), buffer);
  DirectoryEntry entry;
  const Status status = files.stat(virtual_path.view(), &entry);
  if (status != Status::Ok) return errno_of(status);
  Meta meta = meta_of(entry, virtual_path.view());
  mark_socket(p, virtual_path.view(), entry, &meta);
  return write_stat(p, arch, meta, buffer);
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
    const bool no_follow = flags & kAtSymlinkNofollow;
    const int64_t r = no_follow ? resolve_raw(p, dirfd, path, false, &virtual_path) : resolve_at(p, dirfd, path, false, &virtual_path);
    if (r < 0) return error(-r);
    DirectoryEntry entry;
    const DescriptorKind device = files.device_at(virtual_path.view());
    if (no_follow && link_meta(p, virtual_path.view(), &m)) {
      // (a link: m is set)
    } else if (no_follow && follow_proc_self(p, &virtual_path) < 0) {
      return error(kEnoent);
    } else if (device != DescriptorKind::Closed) {
      m = device_meta(kSIfChr | 0666, 3);
    } else {
      const Status status = files.stat(virtual_path.view(), &entry);
      if (status != Status::Ok) return errno_of(status);
      m = meta_of(entry, virtual_path.view());
      mark_socket(p, virtual_path.view(), entry, &m);
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

namespace {
// A listed name the manager remembers as a FIFO (a file system without FIFO nodes)?
bool fifo_listed(Process& p, const OpenObject& directory, const DirectoryEntry& entry) {
  if (entry.type != NodeType::File) return false;
  UnixSocketManager* m = system_unix_sockets();
  if (!m || !m->has_fifo_marks()) return false;
  String path(p.files().allocator()), host(p.files().allocator());
  if (!path.assign(directory.path.view()) || (!path.view().ends_with('/') && !path.append("/")) || !path.append(entry.name.view())) return false;
  return p.files().host_path_of(path.view(), &host) && is_fifo_node(host.view(), entry);
}
}  // namespace

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
    switch (entry.special) {
      case filesystem::SpecialNode::Fifo: v[18] = 1; break;           // DT_FIFO
      case filesystem::SpecialNode::Socket: v[18] = 12; break;        // DT_SOCK
      case filesystem::SpecialNode::CharDevice: v[18] = 2; break;     // DT_CHR
      case filesystem::SpecialNode::BlockDevice: v[18] = 6; break;    // DT_BLK
      case filesystem::SpecialNode::None: if (fifo_listed(p, object, entry)) v[18] = 1; break;
    }
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

// mknodat(dirfd, path, mode, dev) / mknod(path, mode, dev); mkfifo() is mknodat with S_IFIFO. A FIFO is a real node where the file system has them, else an
// empty file remembered as one (Fifo.hpp); a regular file (type 0 or S_IFREG) is made empty; a socket's node is made where the file system has those (else
// EPERM, as are devices: no device numbers here). The permission bits are not kept (no file modes yet).
uint64_t sys_mknodat(Process& p, uint64_t dirfd, uint64_t path, uint64_t mode, uint64_t device) {
  (void)device;
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  String virtual_path(files.allocator());
  const int64_t r = resolve_at(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  if (virtual_path.view() == StringView("/") || files.device_at(virtual_path.view()) != DescriptorKind::Closed) return error(kEexist);
  const uint32_t type = uint32_t(mode) & kSIfMt;
  if (type != 0 && type != kSIfReg && type != kSIfIfo && type != kSIfSock && type != kSIfChr && type != kSIfBlk) return error(kEinval);
  if (type == kSIfChr || type == kSIfBlk) return error(kEperm);
  DirectoryEntry entry;
  if (files.stat(virtual_path.view(), &entry) == Status::Ok) return error(kEexist);
  String host(files.allocator());
  filesystem::IFileSystem* fs = files.file_system();
  if (!fs || !files.host_path_of(virtual_path.view(), &host)) return error(kEnomem);
  if (type == kSIfIfo) return errno_of_or_zero(make_fifo_node(*fs, host.view()));
  if (type == kSIfSock) {
    const Status made = fs->create_special(host.view(), filesystem::SpecialNode::Socket);
    return made == Status::Unsupported ? error(kEperm) : errno_of_or_zero(made);
  }
  UniquePointer<filesystem::IFile> file;      // (a regular file: made empty)
  return errno_of_or_zero(fs->open_file(host.view(), filesystem::OpenMode::CreateOrOpen, &file));
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
  const Status status = files.resize(object, length);
  if (object.memory && status == Status::AccessDenied) return error(kEperm);      // (a seal forbids it)
  return errno_of_or_zero(status);
}

// memfd_create(name, flags): a file in memory (MemoryFile.hpp), named "memfd:<name>" in /proc's style; MFD_CLOEXEC 1, MFD_ALLOW_SEALING 2 (hugetlb and the
// exec flags are refused).
uint64_t sys_memfd_create(Process& p, uint64_t name, uint64_t flags) {
  constexpr uint64_t kCloexec = 1, kAllowSealing = 2, kNoexecSeal = 8, kExec = 0x10;
  if (flags & ~(kCloexec | kAllowSealing | kNoexecSeal | kExec)) return error(kEinval);
  char text[250] = {};
  uint64_t length = 0;
  for (; length < sizeof(text) - 1; ++length) {      // (the name is up to 249 bytes, as on Linux)
    uint8_t c = 0;
    if (!p.read_user(name + length, ByteView(&c, 1))) return error(kEfault);
    if (c == 0) break;
    text[length] = static_cast<char>(c);
  }
  if (length == sizeof(text) - 1) return error(kEinval);
  ProcessFiles& files = p.files();
  // the bytes outlive the process when they are passed on (SCM_RIGHTS): they come from the system's allocator when there is one
  ObjectManager* objects = system_objects();
  allocators::IAllocator& allocator = objects && objects->allocator() ? *objects->allocator() : files.allocator();
  SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(allocator, allocator);
  if (!object) return error(kEnomem);
  UniquePointer<MemoryFile> file = UniquePointer<MemoryFile>::make(allocator, allocator, (flags & kAllowSealing) != 0);
  if (!file || !object->path.append("/memfd:") || !object->path.append(StringView(text, length))) return error(kEnomem);
  object->memory = file.get();
  object->file = util::move(file);
  object->readable = object->writable = true;
  const int64_t fd = files.install(DescriptorKind::File, object, 0);
  if (fd < 0) return error(kEmfile);
  files.set_close_on_exec(uint64_t(fd), (flags & kCloexec) != 0);
  return uint64_t(fd);
}

// fallocate(fd, mode, offset, length): makes room in a file in memory (mode 0); other files and modes are not supported (glibc's posix_fallocate then
// writes the zeros itself).
uint64_t sys_fallocate(Process& p, uint64_t fd, uint64_t mode, uint64_t offset, uint64_t length) {
  ProcessFiles& files = p.files();
  if (files.kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  OpenObject* object = files.object(fd);
  if (int64_t(offset) < 0 || int64_t(length) <= 0) return error(kEinval);
  if (!object || !object->memory || mode != 0) return error(kEopnotsupp);
  if (!object->writable) return error(kEbadf);
  const Status status = object->memory->allocate(offset, length);
  return status == Status::AccessDenied ? error(kEperm) : errno_of_or_zero(status);      // (a seal forbids growing)
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
  if (target < 0) return error(kEmfile);
  descriptor_token_add(p.files().kind(fd), p.files().token(fd));      // (the copy holds a use of a shared object, like the original)
  return static_cast<uint64_t>(target);
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
  if (p.descriptor(new_fd) != DescriptorKind::Closed) p.close_descriptor(new_fd);      // (what was there goes, with its use of a shared object)
  p.files().duplicate_to(old_fd, new_fd);
  descriptor_token_add(p.files().kind(old_fd), p.files().token(old_fd));
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
      descriptor_token_add(files.kind(fd), files.token(fd));
      files.set_close_on_exec(static_cast<uint64_t>(target), command == 1030);
      return static_cast<uint64_t>(target);
    }
    case 1: return files.close_on_exec(fd) ? 1 : 0;      // F_GETFD: FD_CLOEXEC
    case 2: files.set_close_on_exec(fd, argument & 1); return 0;
    case 3: {      // F_GETFL
      if (files.kind(fd) == DescriptorKind::UnixSocket) {
        const int fifo = fifo_access_mode(p, fd);
        return (fifo < 0 ? kORdWr : uint64_t(fifo)) | (socket_non_blocking(p, fd) ? kONonBlock : 0);
      }
      OpenObject* object = files.object(fd);
      if (!object) return files.kind(fd) == DescriptorKind::ConsoleInput ? 0 : kORdWr;
      return (object->readable && object->writable ? kORdWr : object->writable ? kOWrite : 0) | (object->append ? kOAppend : 0);
    }
    case 4:        // F_SETFL: O_NONBLOCK of a socket; nothing else here can change
      if (files.kind(fd) == DescriptorKind::UnixSocket) set_socket_non_blocking(p, fd, (argument & kONonBlock) != 0);
      return 0;
    case 1031:     // F_SETPIPE_SZ / F_GETPIPE_SZ: a pipe's buffer is always kFifoBytes
    case 1032:
      if (!is_pipe_descriptor(p, fd)) return error(kEbadf);
      if (command == 1031 && argument > UnixSocketManager::kFifoBytes) return error(kEperm);
      return UnixSocketManager::kFifoBytes;
    case 1033:     // F_ADD_SEALS / F_GET_SEALS: files in memory (memfd_create) only
    case 1034: {
      OpenObject* object = files.object(fd);
      if (!object || !object->memory) return error(kEinval);
      if (command == 1034) return object->memory->seals();
      if (!object->writable) return error(kEperm);
      return object->memory->add_seals(uint32_t(argument)) ? 0 : error(kEperm);
    }
    default: return error(kEinval);
  }
}

int64_t resolve_user_path(Process& p, uint64_t dirfd, uint64_t path, bool allow_empty, String* virtual_path) {
  if (!p.files().attached()) return -kEnoent;
  return resolve_at(p, dirfd, path, allow_empty, virtual_path);
}

uint64_t sys_readlinkat(Process& p, uint64_t dirfd, uint64_t path, uint64_t buffer, uint64_t size) {
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  if (int64_t(size) <= 0) return error(kEinval);
  String virtual_path(files.allocator()), target(files.allocator());
  const int64_t r = resolve_raw(p, dirfd, path, false, &virtual_path);
  if (r < 0) return error(-r);
  const int64_t proc = proc_self_link(p, virtual_path.view(), &target);
  if (proc < 0) return error(-proc);
  if (proc == 0) {
    if (follow_proc_self(p, &virtual_path) < 0) return error(kEnoent);      // (a link inside /proc/self/cwd/...)
    const Status status = files.read_link(virtual_path.view(), &target);
    if (status != Status::Ok) {
      DirectoryEntry entry;
      const Status exists = files.stat(virtual_path.view(), &entry);
      return exists == Status::Ok ? error(kEinval) : errno_of(exists);      // (what exists but is no link: EINVAL)
    }
  }
  // The target, cut to the buffer, without a terminating NUL (as readlink does).
  const uint64_t n = target.size() < size ? target.size() : size;
  return p.write_user(buffer, text_as_bytes(target.view().first(n))) ? n : error(kEfault);
}

namespace {
DxgServiceHandler g_dxg_handler = nullptr;      // set once at boot, before any process runs (install_dxg)
DxgExportHandler g_dxg_export = nullptr;
DxgImportHandler g_dxg_import = nullptr;
DxgExportSyncHandler g_dxg_export_sync = nullptr;
DxgImportSyncHandler g_dxg_import_sync = nullptr;
GlRequestHandler g_gl_handler = nullptr;

void put_le32(uint8_t* at, uint32_t v) {
  for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (8 * i));
}

// TCOS_DXG_IOCTL_EXPORT: a descriptor (a File-kind object whose file is a DxgBufferFile) for an allocation's export.
uint64_t dxg_export(Process& p, uint64_t argument) {
  uint8_t raw[16];
  if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  const uint32_t allocation = reader.le32(), flags = reader.le32();
  if (flags & ~uint32_t(TCOS_DXG_EXPORT_CLOEXEC)) return error(kEinval);
  if (!g_dxg_export) return error(kEnodev);
  SharedPointer<dxgk::ExportedAllocation> exported;
  uint64_t bytes = 0;
  uint32_t status = 0;
  if (!g_dxg_export(p, allocation, &exported, &bytes, &status)) return error(kEnodev);
  int64_t fd = -1;
  if (exported) {
    // (the object outlives the process when it is passed on: it comes from the system's allocator when there is one, as a memfd's does)
    ProcessFiles& files = p.files();
    ObjectManager* objects = system_objects();
    allocators::IAllocator& allocator = objects && objects->allocator() ? *objects->allocator() : files.allocator();
    SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(allocator, allocator);
    UniquePointer<DxgBufferFile> file = UniquePointer<DxgBufferFile>::make(allocator, allocator, util::move(exported), bytes);
    if (!object || !file || !object->path.append("/dmabuf:dxg")) return error(kEnomem);
    object->gpu_buffer = file.get();
    object->file = util::move(file);
    object->readable = true;
    fd = files.install(DescriptorKind::File, object, 0);
    if (fd < 0) return error(kEmfile);
    files.set_close_on_exec(uint64_t(fd), (flags & TCOS_DXG_EXPORT_CLOEXEC) != 0);
  }
  uint8_t out[8];
  put_le32(out, uint32_t(int32_t(fd)));
  put_le32(out + 4, status);
  if (p.write_user(argument + 8, ConstByteView(out, sizeof(out)))) return 0;
  if (fd >= 0) p.files().close(uint64_t(fd));
  return error(kEfault);
}

// TCOS_DXG_IOCTL_IMPORT: an allocation handle of the process for the image of a descriptor.
uint64_t dxg_import(Process& p, uint64_t argument) {
  uint8_t raw[32];
  if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  const uint32_t fd = reader.le32(), device = reader.le32();
  if (p.descriptor(fd) != DescriptorKind::File) return error(kEbadf);
  SharedPointer<OpenObject> object = p.files().shared_object(fd);      // (held: another thread may close the descriptor meanwhile)
  if (!object || !object->gpu_buffer || !object->gpu_buffer->exported()) return error(kEbadf);
  if (!g_dxg_import) return error(kEnodev);
  DxgImported imported;
  uint32_t status = 0;
  if (!g_dxg_import(p, device, *object->gpu_buffer->exported(), &imported, &status)) return error(kEnodev);
  uint8_t out[24];
  put_le32(out, imported.allocation);
  put_le32(out + 4, status);
  put_le32(out + 8, imported.width);
  put_le32(out + 12, imported.height);
  put_le32(out + 16, imported.format);
  put_le32(out + 20, imported.pitch);
  return p.write_user(argument + 8, ConstByteView(out, sizeof(out))) ? 0 : error(kEfault);
}
// TCOS_DXG_IOCTL_EXPORT_SYNC: a descriptor (a File-kind object whose file is a DxgSyncFile) for a fence's export.
uint64_t dxg_export_sync(Process& p, uint64_t argument) {
  uint8_t raw[16];
  if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  const uint32_t sync = reader.le32(), flags = reader.le32();
  if (flags & ~uint32_t(TCOS_DXG_EXPORT_CLOEXEC)) return error(kEinval);
  if (!g_dxg_export_sync) return error(kEnodev);
  SharedPointer<dxgk::ExportedSync> exported;
  uint32_t status = 0;
  if (!g_dxg_export_sync(p, sync, &exported, &status)) return error(kEnodev);
  int64_t fd = -1;
  if (exported) {
    ProcessFiles& files = p.files();
    ObjectManager* objects = system_objects();
    allocators::IAllocator& allocator = objects && objects->allocator() ? *objects->allocator() : files.allocator();
    SharedPointer<OpenObject> object = SharedPointer<OpenObject>::make(allocator, allocator);
    UniquePointer<DxgSyncFile> file = UniquePointer<DxgSyncFile>::make(allocator, util::move(exported));
    if (!object || !file || !object->path.append("/syncobj:dxg")) return error(kEnomem);
    object->gpu_sync = file.get();
    object->file = util::move(file);
    object->readable = true;
    fd = files.install(DescriptorKind::File, object, 0);
    if (fd < 0) return error(kEmfile);
    files.set_close_on_exec(uint64_t(fd), (flags & TCOS_DXG_EXPORT_CLOEXEC) != 0);
  }
  uint8_t out[8];
  put_le32(out, uint32_t(int32_t(fd)));
  put_le32(out + 4, status);
  if (p.write_user(argument + 8, ConstByteView(out, sizeof(out)))) return 0;
  if (fd >= 0) p.files().close(uint64_t(fd));
  return error(kEfault);
}

// TCOS_DXG_IOCTL_IMPORT_SYNC: a sync object handle of the process for a fence's descriptor.
uint64_t dxg_import_sync(Process& p, uint64_t argument) {
  uint8_t raw[16];
  if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  const uint32_t fd = reader.le32(), device = reader.le32();
  if (p.descriptor(fd) != DescriptorKind::File) return error(kEbadf);
  SharedPointer<OpenObject> object = p.files().shared_object(fd);
  if (!object || !object->gpu_sync || !object->gpu_sync->exported()) return error(kEbadf);
  if (!g_dxg_import_sync) return error(kEnodev);
  uint32_t sync = 0, status = 0;
  if (!g_dxg_import_sync(p, device, *object->gpu_sync->exported(), &sync, &status)) return error(kEnodev);
  uint8_t out[8];
  put_le32(out, sync);
  put_le32(out + 4, status);
  return p.write_user(argument + 8, ConstByteView(out, sizeof(out))) ? 0 : error(kEfault);
}

// DMA_BUF_IOCTL_SYNC (_IOW('b', 0, struct dma_buf_sync { u64 flags })) on a GPU image's descriptor: moves the picture between the image and the CPU's copy that
// mmap maps (DxgBufferFile.hpp). EAGAIN while the GPU still works on the image. Anything else on a file is ENOTTY.
uint64_t dmabuf_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument) {
  constexpr uint32_t kDmaBufIoctlSync = 0x40086200u;
  constexpr int64_t kEagain = 11;
  SharedPointer<OpenObject> object = p.files().shared_object(fd);
  if (!object || !object->gpu_buffer || uint32_t(request) != kDmaBufIoctlSync) return error(kEnotty);
  uint8_t raw[8];
  if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  switch (object->gpu_buffer->sync(reader.le64())) {
    case DxgBufferFile::SyncResult::Ok: return 0;
    case DxgBufferFile::SyncResult::Busy: return error(kEagain);
    case DxgBufferFile::SyncResult::NoCopier: return error(kEnodev);
    default: return error(kEinval);
  }
}
}  // namespace

void set_dxg_service_handler(DxgServiceHandler handler) { g_dxg_handler = handler; }
void set_dxg_sync_handlers(DxgExportSyncHandler export_handler, DxgImportSyncHandler import_handler) {
  g_dxg_export_sync = export_handler;
  g_dxg_import_sync = import_handler;
}
void set_gl_request_handler(GlRequestHandler handler) { g_gl_handler = handler; }
void set_dxg_buffer_handlers(DxgExportHandler export_handler, DxgImportHandler import_handler) {
  g_dxg_export = export_handler;
  g_dxg_import = import_handler;
}

// ioctl on /dev/dxg: TCOS_DXG_IOCTL_CALL runs one D3DKMT service (DxgDevice.h); TCOS_DXG_IOCTL_EXPORT / IMPORT make and open GPU images as descriptors; TCOS_DXG_IOCTL_GL
// runs a request of the kernel's OpenGL. Everything else,
// and ioctl on other descriptors, is ENOTTY.
uint64_t sys_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument) {
  const DescriptorKind kind = p.descriptor(fd);
  if (kind == DescriptorKind::Closed) return error(kEbadf);
  if (kind == DescriptorKind::UnixSocket) return socket_ioctl(p, fd, request, argument);      // (FIONREAD, FIONBIO: LinuxSockets.cpp)
  if (kind == DescriptorKind::File) return dmabuf_ioctl(p, fd, request, argument);
  if (kind != DescriptorKind::Dxg) return error(kEnotty);
  const uint32_t code = uint32_t(request);      // (the request is an unsigned int: the upper half is not part of it)
  if (code == TCOS_DXG_IOCTL_EXPORT) return dxg_export(p, argument);
  if (code == TCOS_DXG_IOCTL_IMPORT) return dxg_import(p, argument);
  if (code == TCOS_DXG_IOCTL_EXPORT_SYNC) return dxg_export_sync(p, argument);
  if (code == TCOS_DXG_IOCTL_IMPORT_SYNC) return dxg_import_sync(p, argument);
  if (code == TCOS_DXG_IOCTL_GL) {
    // TcosDxgGl: request (8), status (4), reserved (4)
    uint8_t raw[16];
    if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
    ByteReader reader{ConstByteView(raw, sizeof(raw))};
    const uint64_t request = reader.le64();
    if (!g_gl_handler) return error(kEnodev);
    uint8_t out[4];
    put_le32(out, g_gl_handler(p, request));
    return p.write_user(argument + 8, ConstByteView(out, sizeof(out))) ? 0 : error(kEfault);
  }
  if (code != TCOS_DXG_IOCTL_CALL) return error(kEnotty);
  // TcosDxgCall: service (4), status (4), params (8)
  uint8_t raw[16];
  if (!p.read_user(argument, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader reader{ConstByteView(raw, sizeof(raw))};
  const uint32_t service = reader.le32();
  reader.skip(4);      // (status: written below)
  const uint64_t params = reader.le64();
  if (!g_dxg_handler) return error(kEnodev);
  uint32_t status = 0;
  if (!g_dxg_handler(p, service, params, &status)) return error(kEinval);
  uint8_t out[4] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)};
  return p.write_user(argument + 4, ConstByteView(out, sizeof(out))) ? 0 : error(kEfault);
}

}  // namespace process::linuxabi
