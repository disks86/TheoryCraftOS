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
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace process {

// Synchronization objects shared by every process of the system: events, mutexes (mutants) and semaphores, with names, so that two programs that open the same name get the
// same object. Handles in a process's table refer to an object by its id; the object lives while a handle (or a name lookup in progress) refers to it.

// The one lock and wake-up all the waiting on these objects (and on threads) is built on: whoever runs the system (the kernel) supplies it. A waiter takes the lock, looks, and
// waits; whoever changes something takes the lock, changes it, and wakes everybody (the waiters look again).
class ISystemSync {
 public:
  virtual void lock() = 0;
  virtual void unlock() = 0;
  // With the lock held: releases it and sleeps until notify_all() or the timeout (nanoseconds, ~0 = none); the lock is held again on return. False on timeout.
  virtual bool wait(uint64_t timeout_ns) = 0;
  virtual void notify_all() = 0;

 protected:
  ISystemSync() = default;
  ~ISystemSync() = default;
};

enum class ObjectKind : uint8_t { None, Event, Mutex, Semaphore, Process };

class ObjectManager {
 public:
  static constexpr uint32_t kCapacity = 1024;
  static constexpr uint32_t kNameBytes = 128;

  explicit ObjectManager(ISystemSync& sync) : sync_(sync) {}
  ObjectManager(const ObjectManager&) = delete;
  ObjectManager& operator=(const ObjectManager&) = delete;

  ISystemSync& sync() { return sync_; }
  // Wakes every waiter (takes the lock itself): to be called after anything a waiter looks at changed outside the manager (a thread ended, a process was told to stop).
  void notify();

  // --- making and finding objects (these take the lock themselves). `status` is an NTSTATUS: 0, 0x40000000 (the name existed: the existing object is returned), or an error.
  // `owned` / `signaled`: a mutex created owned by `owner`; an event created signaled. A semaphore starts at `initial` and can reach `maximum`.
  struct Created {
    uint32_t id = 0;           // 0 on failure
    uint32_t status = 0;
  };
  Created create(ObjectKind kind, StringView name, bool manual_reset, bool initial_state, int32_t initial, int32_t maximum, uint64_t owner);
  Created open(ObjectKind kind, StringView name);
  // A process (waited for by its parent): signalled when it has ended. `pid` finds it again when the process ends.
  Created create_process(uint64_t pid);
  void process_exited(uint64_t pid, uint32_t code);
  bool query_process(uint32_t id, bool* exited, uint32_t* code, uint64_t* pid);
  void add_ref(uint32_t id);
  void release(uint32_t id);
  ObjectKind kind(uint32_t id);

  // --- operations on one object (they take the lock and wake the waiters). They return an NTSTATUS.
  uint32_t set_event(uint32_t id, int32_t* previous);
  uint32_t reset_event(uint32_t id, int32_t* previous);
  uint32_t pulse_event(uint32_t id, int32_t* previous);
  uint32_t release_mutex(uint32_t id, uint64_t owner, int32_t* previous_count);
  uint32_t release_semaphore(uint32_t id, int32_t count, int32_t* previous);
  // A thread (or all the threads of a process) ended: the mutexes it owns become abandoned and free.
  void abandon_thread(uint64_t thread);
  void abandon_process(uint64_t process);

  // --- waiting (the caller holds the lock: sync().lock())
  bool ready_locked(uint32_t id, uint64_t waiter);
  // Takes what a finished wait takes (the mutex, a unit of the semaphore, the auto-reset event's signal). *abandoned: the mutex was abandoned by its last owner.
  void acquire_locked(uint32_t id, uint64_t waiter, bool* abandoned);
  void waiting_locked(uint32_t id, int32_t delta);      // a waiter starts (+1) or stops (-1) waiting for the object

 private:
  struct Object {
    ObjectKind kind = ObjectKind::None;
    uint32_t references = 0;
    char name[kNameBytes] = {};
    uint32_t name_size = 0;
    bool manual_reset = false, signaled = false, abandoned = false;
    int32_t count = 0, maximum = 0;        // semaphore
    uint64_t owner = 0;                    // mutex: the thread that owns it, 0 if none
    int32_t recursion = 0;
    int32_t waiters = 0, pulses = 0;       // PulseEvent lets through the threads that are waiting now
  };
  Object* at(uint32_t id) { return id >= 1 && id <= kCapacity && objects_[id - 1].kind != ObjectKind::None ? &objects_[id - 1] : nullptr; }
  Object* find_locked(StringView name);

  ISystemSync& sync_;
  Object objects_[kCapacity];
};

// The objects of the system (null until whoever runs it has made them: the kernel, at start-up; host tests set their own).
ObjectManager* system_objects();
void set_system_objects(ObjectManager* manager);

}  // namespace process
