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
#include "Program/EmulatedTask.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Delay.hpp"
#include "Process/Objects.hpp"
#include <new>

namespace desktop {

namespace {

// The process lock and the waiting between its threads, on the kernel's primitives.
class KernelSync final : public process::ISystemSync {
 public:
  explicit KernelSync(process::IProcessHost& host) : host_(host) {}
  void lock() override { mutex_.lock(); }
  void unlock() override { mutex_.unlock(); }
  bool wait(uint64_t timeout_ns) override {
    const uint64_t us = timeout_ns == ~uint64_t{0} ? 0 : (timeout_ns / 1000 + 1);      // (0 means forever; a short wait is at least one tick)
    return cv_.wait(mutex_, us);
  }
  void notify_all() override { cv_.notify_all(); }
  uint64_t now_ns() override { return host_.monotonic_ns(); }

 private:
  process::IProcessHost& host_;
  kernel::Mutex mutex_;
  kernel::ConditionVariable cv_;
};

// Guest threads are kernel threads with a stack big enough for the deepest system call.
class KernelThreads final : public emulation::IThreadSpawner {
 public:
  void* spawn(void (*fn)(void*), void* arg, const char* name) override { return kernel::spawn(fn, arg, name, -1, -1, 256 * 1024); }
  void join(void* handle) override { kernel::join(static_cast<kernel::Thread*>(handle), 0); }
  uint64_t self() override { return reinterpret_cast<uint64_t>(kernel::current_thread()); }
};

}  // namespace

namespace {

// The lock and wake-up of the waiting on objects shared between processes.
class ObjectSync final : public process::ISystemSync {
 public:
  void lock() override { mutex_.lock(); }
  void unlock() override { mutex_.unlock(); }
  bool wait(uint64_t timeout_ns) override { return changed_.wait(mutex_, timeout_ns == ~uint64_t{0} ? 0 : timeout_ns / 1000 + 1); }
  void notify_all() override { changed_.notify_all(); }
  uint64_t now_ns() override {
#if defined(__x86_64__)
    const uint64_t per_us = hardware::tsc_ticks_per_us() ? hardware::tsc_ticks_per_us() : 1;
    const uint64_t t = hardware::read_tsc();
    return t / per_us * 1000 + (t % per_us) * 1000 / per_us;
#else
    return 0;
#endif
  }

 private:
  kernel::Mutex mutex_;
  kernel::ConditionVariable changed_;
};

kernel::Mutex g_objects_mutex;

}  // namespace

bool ensure_system_objects(allocators::IAllocator& allocator) {
  kernel::ScopedMutex hold(g_objects_mutex);
  if (process::system_objects()) return true;
  void* sync_memory = allocator.allocate(sizeof(ObjectSync));
  void* objects_memory = sync_memory ? allocator.allocate(sizeof(process::ObjectManager)) : nullptr;
  if (!objects_memory) {
    if (sync_memory) allocator.deallocate(sync_memory);
    return false;
  }
  ObjectSync* sync = new (sync_memory) ObjectSync();
  process::ObjectManager* objects = new (objects_memory) process::ObjectManager(*sync);
  objects->set_allocator(&allocator);
  process::set_system_objects(objects);      // (lives as long as the system does)
  return true;
}

void EmulatedTask::thread_main(void* self) {
  EmulatedTask& task = *static_cast<EmulatedTask*>(self);
  KernelSync sync(task.program_.host());
  KernelThreads threads;
  task.outcome_ = emulation::run_emulated_threads(task.program_, *task.cpu_.get(), task, sync, threads, task.allocator_);
}

bool EmulatedTask::start(const char* name) {
  thread_ = kernel::spawn(&EmulatedTask::thread_main, this, name);
  return thread_ != nullptr;
}

bool EmulatedTask::wait(uint64_t timeout_us) {
  if (joined_) return true;
  if (!thread_ || kernel::join(thread_, timeout_us)) {
    joined_ = true;
    thread_ = nullptr;
    return true;
  }
  return false;
}

}  // namespace desktop
