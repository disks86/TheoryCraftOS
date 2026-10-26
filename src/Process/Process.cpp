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
#include "Process/Process.hpp"
#include "Process/WindowsPipes.hpp"
#include "Process/Objects.hpp"

namespace process {

namespace {
constexpr uint64_t kChunk = 1024;
constexpr uint64_t kUnknownLinux = ~uint64_t{37};      // -ENOSYS
constexpr uint64_t kUnknownWindows = 0xC000001Cu;      // STATUS_INVALID_SYSTEM_SERVICE
}  // namespace

Process::Process(uint64_t id, const ProcessBinding& binding, IUserMemory& memory, IProcessHost& host)
    : id_(id), binding_(binding), memory_(memory), host_(host), files_(binding.abi == SyscallAbi::Windows ? PathStyle::Windows : PathStyle::Posix) {}

uint64_t Process::syscall(const SyscallRequest& request) {
  if (!binding_.interface || exited()) return binding_.abi == SyscallAbi::Windows ? kUnknownWindows : kUnknownLinux;
  return binding_.interface->dispatch(*this, request);
}

namespace {
bool is_object_kind(DescriptorKind k) { return k == DescriptorKind::Event || k == DescriptorKind::Mutex || k == DescriptorKind::Semaphore || k == DescriptorKind::Process || k == DescriptorKind::Timer || k == DescriptorKind::Section || k == DescriptorKind::Job; }
}  // namespace

Process::~Process() {
  if (ObjectManager* objects = system_objects()) {
    for (uint64_t slot = 0; slot < kMaxDescriptors; ++slot) {
      if (is_object_kind(files_.kind(slot))) objects->release(uint32_t(files_.token(slot)));
      else if (files_.kind(slot) == DescriptorKind::Pipe) windowsabi::pipe_token_release(files_.token(slot));
    }
  }
}

bool Process::close_descriptor(uint64_t slot) {
  const DescriptorKind kind = files_.kind(slot);
  const uint64_t token = files_.token(slot);
  if (!files_.close(slot)) return false;
  if (is_object_kind(kind)) if (ObjectManager* objects = system_objects()) objects->release(uint32_t(token));
  if (kind == DescriptorKind::Pipe) windowsabi::pipe_token_release(token);
  return true;
}

void Process::release_handles() {
  for (uint64_t slot = 0; slot < kMaxDescriptors; ++slot) if (files_.kind(slot) != DescriptorKind::Closed) close_descriptor(slot);
}

void Process::terminate(uint32_t code) {
  if (exited()) return;
  exit_code_ = code;
  __atomic_store_n(&exited_, true, __ATOMIC_RELEASE);
  host_.wake_blocked();
  if (runtime_) runtime_->notify();
  if (ObjectManager* objects = system_objects()) {      // what it owned is abandoned; whoever waits for it or for them looks again
    objects->abandon_process(id_);
    objects->process_exited(id_, code);
    objects->notify();
  }
}

void Process::register_main_thread(uint64_t id, uint64_t teb) {
  ThreadRecord* t = add_thread(id);
  if (!t) return;
  t->started = true;
  t->teb = teb;
}

Process::ThreadRecord* Process::find_thread(uint64_t id) {
  for (ThreadRecord& t : threads_) if (t.used && t.id == id) return &t;
  return nullptr;
}

Process::ThreadRecord* Process::add_thread(uint64_t id) {
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) {
      // The table is full: a thread that has ended and that no handle refers to any more is forgotten (the main thread's record stays).
      for (ThreadRecord& t : threads_) {
        if (!t.used || !t.finished || t.id == id_) continue;
        bool referenced = false;
        for (uint64_t slot = 0; slot < kMaxDescriptors && !referenced; ++slot) referenced = files_.kind(slot) == DescriptorKind::Thread && files_.token(slot) == t.id;
        if (!referenced) t.used = false;
      }
    }
    for (ThreadRecord& t : threads_) {
      if (t.used) continue;
      t = ThreadRecord();
      t.used = true;
      t.id = id;
      return &t;
    }
  }
  return nullptr;
}

uint32_t Process::thread_snapshot(uint64_t* ids, uint64_t* starts, int32_t* priorities, uint32_t capacity) const {
  uint32_t n = 0;
  for (const ThreadRecord& t : threads_) {
    if (!t.used || t.finished || n >= capacity) continue;
    ids[n] = t.id; starts[n] = t.start_address; priorities[n] = t.priority;
    ++n;
  }
  return n;
}

uint32_t Process::live_threads() const {
  uint32_t n = 0;
  for (const ThreadRecord& t : threads_) if (t.used && !t.finished) ++n;
  return n;
}

namespace {
// Gives the process lock back for the length of a blocking host call.
class Unlocked {
 public:
  explicit Unlocked(IProcessRuntime* runtime) : runtime_(runtime) { if (runtime_) runtime_->unlock(); }
  ~Unlocked() { if (runtime_) runtime_->lock(); }
  Unlocked(const Unlocked&) = delete;
  Unlocked& operator=(const Unlocked&) = delete;

 private:
  IProcessRuntime* runtime_;
};
}  // namespace

void Process::block_sleep(uint64_t ns) {
  if (!runtime_) {
    host_.sleep_ns(ns);
    return;
  }
  // With threads the sleep is a wait that the process ending (or a kill) cuts short; other changes in the process wake it too, so it keeps track of its deadline.
  const uint64_t now = host_.monotonic_ns();
  const uint64_t end = ns > ~uint64_t{0} - now ? ~uint64_t{0} : now + ns;
  while (!runtime_->interrupted() && !exited()) {
    const uint64_t t = host_.monotonic_ns();
    if (t >= end) break;
    runtime_->wait(end == ~uint64_t{0} ? ~uint64_t{0} : end - t);
  }
}

void Process::block_yield() {
  Unlocked u(runtime_);
  host_.yield();
}

bool Process::block_read_console(ByteView out, uint64_t* got) {
  Unlocked u(runtime_);
  return host_.read_console(out, got);
}


bool Process::read_user_u64(uint64_t address, uint64_t* out) {
  uint8_t raw[8];
  if (!read_user(address, ByteView(raw, 8))) return false;
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) value |= uint64_t{raw[i]} << (8 * i);
  *out = value;
  return true;
}

bool Process::write_user_u64(uint64_t address, uint64_t value) {
  uint8_t raw[8];
  for (uint32_t i = 0; i < 8; ++i) raw[i] = uint8_t(value >> (8 * i));
  return write_user(address, ConstByteView(raw, 8));
}

bool Process::write_user_u32(uint64_t address, uint32_t value) {
  uint8_t raw[4];
  for (uint32_t i = 0; i < 4; ++i) raw[i] = uint8_t(value >> (8 * i));
  return write_user(address, ConstByteView(raw, 4));
}

uint64_t copy_user_to_console(Process& process, uint32_t stream, uint64_t address, uint64_t count, bool* faulted) {
  *faulted = false;
  uint8_t buffer[kChunk];
  uint64_t done = 0;
  while (done < count) {
    const uint64_t n = count - done < kChunk ? count - done : kChunk;
    if (address > ~uint64_t{0} - done - n + 1) { *faulted = true; break; }      // the range would wrap past the end of the address space
    uint64_t ok = n;
    if (!process.read_user(address + done, ByteView(buffer, n))) {
      // The piece runs into unmapped memory: send the bytes before the hole, as a real kernel does, and report the fault.
      *faulted = true;
      ok = 0;
      while (ok < n && process.read_user(address + done + ok, ByteView(buffer + ok, 1))) ++ok;
    }
    if (ok && !process.host().write_console(stream, ConstByteView(buffer, ok))) { *faulted = false; break; }
    done += ok;
    if (*faulted) break;
  }
  return done;
}

uint64_t copy_console_to_user(Process& process, uint64_t address, uint64_t count, bool* faulted) {
  *faulted = false;
  uint8_t buffer[kChunk];
  const uint64_t n = count < kChunk ? count : kChunk;      // one read: a terminal read returns what is there, not what was asked for
  if (n == 0) return 0;
  uint64_t got = 0;
  if (!process.block_read_console(ByteView(buffer, n), &got) || got > n) return 0;
  if (!process.write_user(address, ConstByteView(buffer, got))) { *faulted = true; return 0; }
  return got;
}

}  // namespace process
