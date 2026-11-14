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

#include "Process/LinuxExec.hpp"
#include "Executable/ExecutableKind.hpp"
#include "Process/LinuxFiles.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/Move.hpp"

namespace process::linuxabi {

namespace {

using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

constexpr int64_t kEnoent = 2, kE2big = 7, kEnoexec = 8, kEnomem = 12, kEacces = 13, kEfault = 14, kEinval = 22, kEnosys = 38, kEloop = 40;
constexpr uint64_t kAtEmptyPath = 0x1000;
// Linux's limits: a string of at most 32 pages (MAX_ARG_STRLEN), and here at most this many strings and bytes in all.
constexpr uint64_t kMaxString = 32 * 4096, kMaxStrings = 8192, kMaxTotal = uint64_t{2} << 20;
constexpr uint64_t kMaxImage = uint64_t{1} << 30;
constexpr uint32_t kMaxInterpreters = 4;
constexpr uint64_t kWaitNs = 20000000;      // how often the old program looks whether the new one has ended

uint64_t error(int64_t e) { return uint64_t(-e); }

// A NUL-terminated string of the program, at most `limit` bytes. 0, or -errno.
int64_t read_string(Process& p, uint64_t address, uint64_t limit, String* out) {
  out->clear();
  if (address == 0) return -kEfault;
  for (uint64_t at = address;;) {
    uint8_t byte = 0;
    uint8_t chunk[64];
    uint64_t n = sizeof(chunk);
    if (!p.read_user(at, ByteView(chunk, n))) {      // (a chunk that runs off the end of the mapping is read byte by byte)
      n = 0;
      while (n < sizeof(chunk) && p.read_user(at + n, ByteView(&byte, 1))) {
        chunk[n++] = byte;
        if (byte == 0) break;
      }
      if (n == 0) return -kEfault;
    }
    for (uint64_t i = 0; i < n; ++i) {
      if (chunk[i] == 0) return 0;
      if (out->size() >= limit) return -kE2big;
      if (!out->push_back(static_cast<char>(chunk[i]))) return -kEnomem;
    }
    if (n < sizeof(chunk)) return -kEfault;      // (the mapping ended before the NUL)
    if (at > ~uint64_t{0} - n) return -kEfault;
    at += n;
  }
}

// A NULL-terminated array of string pointers (argv, envp); a null array is empty. *total counts the bytes of every string read so far. 0, or -errno.
int64_t read_vector(Process& p, uint64_t address, List<String>* out, uint64_t* total) {
  if (address == 0) return 0;
  for (uint64_t i = 0;; ++i) {
    if (i >= kMaxStrings) return -kE2big;
    uint64_t pointer = 0;
    if (!p.read_user_u64(address + 8 * i, &pointer)) return -kEfault;
    if (pointer == 0) return 0;
    String text(p.files().allocator());
    if (const int64_t r = read_string(p, pointer, kMaxString, &text); r < 0) return r;
    *total += text.size() + 1;
    if (*total > kMaxTotal) return -kE2big;
    if (!out->push_back(util::move(text))) return -kEnomem;
  }
}

// The whole file at `virtual_path` (links followed) into *image. 0, or -errno.
int64_t read_program(Process& p, StringView virtual_path, ByteBuffer* image) {
  ProcessFiles& files = p.files();
  DirectoryEntry entry;
  const Status found = files.stat(virtual_path, &entry);
  if (found != Status::Ok) return found == Status::TooManyLinks ? -kEloop : -kEnoent;
  if (entry.type == NodeType::Directory || entry.special != filesystem::SpecialNode::None) return -kEacces;
  String host(files.allocator());
  if (!files.host_path_of(virtual_path, &host)) return -kEnoent;
  UniquePointer<filesystem::IFile> file;
  if (files.file_system()->open_file(host.view(), filesystem::OpenMode::Read, &file) != Status::Ok) return -kEacces;
  const uint64_t size = file->size();
  if (size > kMaxImage) return -kEnomem;
  image->clear();
  if (!image->put_zeros(size)) return -kEnomem;
  uint64_t done = 0;
  while (done < size) {
    uint64_t got = 0;
    if (file->read(done, image->writable_view().subview(done), &got) != Status::Ok || got == 0) return -kEacces;
    done += got;
  }
  return 0;
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

// The "#!interpreter [argument]" line of a script: true and the two parts if `image` starts with one (Linux reads at most 255 bytes of it).
bool shebang(ConstByteView image, StringView* interpreter, StringView* argument, String* line) {
  constexpr uint64_t kMaxLine = 255;
  if (image.size() < 2 || image[0] != '#' || image[1] != '!') return false;
  line->clear();
  for (uint64_t i = 2; i < image.size() && i < kMaxLine && image[i] != '\n'; ++i)
    if (!line->push_back(static_cast<char>(image[i]))) return false;
  StringView rest = line->view();
  while (!rest.empty() && is_blank(rest[0])) rest = rest.without_prefix(1);
  uint64_t end = 0;
  while (end < rest.size() && !is_blank(rest[end])) ++end;
  *interpreter = rest.first(end);
  rest = rest.without_prefix(end);
  while (!rest.empty() && is_blank(rest[0])) rest = rest.without_prefix(1);
  while (!rest.empty() && (is_blank(rest[rest.size() - 1]) || rest[rest.size() - 1] == '\r')) rest = rest.first(rest.size() - 1);
  *argument = rest;
  return !interpreter->empty();
}

}  // namespace

uint64_t sys_execve(Process& p, uint64_t dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags) {
  IProcessRuntime* runtime = p.runtime();
  ProcessFiles& files = p.files();
  if (!files.attached()) return error(kEnoent);
  if (flags & ~(kAtEmptyPath | 0x100 /* AT_SYMLINK_NOFOLLOW */)) return error(kEinval);
  allocators::IAllocator& allocator = files.allocator();

  // The file: a path, or (execveat with AT_EMPTY_PATH and an empty path) the open file dirfd.
  String target(allocator), given(allocator);
  uint8_t first = 1;
  if ((flags & kAtEmptyPath) && path != 0 && p.read_user(path, ByteView(&first, 1)) && first == 0) {
    const OpenObject* object = files.object(dirfd);
    if (!object || object->path.empty() || !target.assign(object->path.view())) return error(kEnoent);
  } else {
    if (const int64_t r = read_string(p, path, 4096, &given); r < 0) return error(-r);
    if (const int64_t r = resolve_user_path(p, dirfd, path, false, &target); r < 0) return error(-r);
  }
  if (given.empty() && !given.assign(target.view())) return error(kEnomem);

  List<String> arguments(allocator), environment(allocator);
  uint64_t total = 0;
  if (const int64_t r = read_vector(p, argv, &arguments, &total); r < 0) return error(-r);
  if (const int64_t r = read_vector(p, envp, &environment, &total); r < 0) return error(-r);

  // A script runs its interpreter: argv becomes the interpreter, its argument (if any), the script as it was named, then argv[1...].
  ByteBuffer image(allocator);
  for (uint32_t level = 0;; ++level) {
    if (const int64_t r = read_program(p, target.view(), &image); r < 0) return error(-r);
    StringView interpreter, argument;
    String line(allocator);
    if (!shebang(image.view(), &interpreter, &argument, &line)) break;
    if (level >= kMaxInterpreters) return error(kEloop);
    List<String> next(allocator);
    String a0(allocator), a1(allocator), a2(allocator);
    if (!a0.assign(interpreter) || !next.push_back(util::move(a0))) return error(kEnomem);
    if (!argument.empty() && (!a1.assign(argument) || !next.push_back(util::move(a1)))) return error(kEnomem);
    if (!a2.assign(given.view()) || !next.push_back(util::move(a2))) return error(kEnomem);
    for (uint64_t i = 1; i < arguments.count(); ++i) {
      String copy(allocator);
      if (!copy.assign(arguments[i].view()) || !next.push_back(util::move(copy))) return error(kEnomem);
    }
    arguments.clear();
    for (uint64_t i = 0; i < next.count(); ++i)
      if (!arguments.push_back(util::move(next[i]))) return error(kEnomem);
    String resolved(allocator);
    if (files.resolve(interpreter, &resolved) != PathResult::Ok) return error(kEnoent);
    if (!target.assign(resolved.view()) || !given.assign(interpreter)) return error(kEnomem);
  }
  const exe::ExecutableKind kind = exe::identify_executable(image.view().at_most(exe::kIdentifyBytes));
  if (abi_for_executable(kind) != SyscallAbi::Linux) return error(kEnoexec);
  if (!runtime) return error(kEnosys);

  // What the new program is told: its file (links resolved, as /proc/self/exe shows it), its arguments and its environment.
  String executable(allocator), command_line(allocator);
  if (files.canonical(target.view(), &executable) != Status::Ok && !executable.assign(target.view())) return error(kEnomem);
  List<StringView> argument_views(allocator), environment_views(allocator);
  for (const String& a : arguments.view()) {
    if (!argument_views.push_back(a.view())) return error(kEnomem);
    if ((!command_line.empty() && !command_line.push_back(' ')) || !command_line.append(a.view())) return error(kEnomem);
  }
  if (argument_views.count() == 0 && !argument_views.push_back(given.view())) return error(kEnomem);      // (Linux gives a program started with no argv an empty argv[0]; its path is kinder)
  for (const String& e : environment.view())
    if (!environment_views.push_back(e.view())) return error(kEnomem);

  ChildSpec spec;
  spec.pid = runtime->allocate_process_id();
  spec.image = image.view();
  spec.image_path = executable.view();
  spec.command_line = command_line.view();
  spec.working_directory = files.cwd();
  spec.environment = environment_views.view();
  spec.arguments = argument_views.view();
  spec.executable_path = executable.view();
  spec.linux_exec = true;
  if (spec.pid == 0) return error(kEnosys);
  const uint32_t started = runtime->spawn_process(spec);
  if (started == 0xC0000002u) return error(kEnosys);      // (STATUS_NOT_IMPLEMENTED: this runtime starts no processes)
  if (started == 0xC0000017u || started == 0xC000009Au) return error(kEnomem);
  if (started != 0) return error(kEnoexec);

  // From here on the old program is gone as far as anyone can tell: what it had open lives on in the new one, its other threads stop, and it ends
  // with the new program's code.
  for (uint64_t slot = 0; slot < ProcessFiles::kMaxDescriptors; ++slot)
    if (files.kind(slot) != DescriptorKind::Closed) p.close_descriptor(slot);
  const uint64_t self = runtime->current_thread_id();
  for (const Process::LinuxThreadState::Thread& t : p.linux_threads().threads)
    if (t.used && t.tid != self) runtime->request_terminate(t.tid, 0);
  bool exited = false, stopped = false;
  uint32_t code = 0;
  while (runtime->query_child(spec.pid, &exited, &code) && !exited) {
    if (!stopped && runtime->interrupted()) {      // (Ctrl+C: it goes to the program that runs now)
      runtime->terminate_process(spec.pid, 130);
      stopped = true;
    }
    runtime->wait(kWaitNs);
  }
  p.terminate(code & 0xFF);
  return 0;
}

}  // namespace process::linuxabi
