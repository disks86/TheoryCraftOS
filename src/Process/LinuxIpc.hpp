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
#include "Allocators/IAllocator.hpp"
#include "Process/MemoryFile.hpp"
#include "Process/Objects.hpp"
#include "Process/Syscall.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

// System V interprocess communication for Linux programs: shared memory segments (shmget, shmat, shmdt, shmctl), semaphore sets (semget, semop, semtimedop,
// semctl) and message queues (msgget, msgsnd, msgrcv, msgctl). The three kinds each have a table for the whole system, found by key or by id, that outlives
// the processes using it until it is removed (IPC_RMID), as on Linux.
//
// Locking: the tables are guarded by the system's one lock and wake-up (ISystemSync, the one the sockets and pipes use), because a semaphore operation or a
// message call that cannot go on must look and go to sleep without a wake-up falling in between. Every change a waiter could be waiting for wakes everybody.
// Nothing outside LinuxIpc.cpp touches the tables.
namespace process {

class Process;

class LinuxIpcManager {
 public:
  static constexpr uint32_t kSegments = 128, kSemSets = 128, kSemsPerSet = 128, kQueues = 64, kUndos = 256;
  static constexpr uint64_t kSegmentMax = 64ull << 20;      // one segment (SHMMAX): what one block of shared memory may be
  static constexpr uint32_t kSemaphoreMax = 32767;          // SEMVMX
  static constexpr uint32_t kOperationsMax = 64;            // sembuf entries in one semop (SEMOPM)
  static constexpr uint32_t kQueueBytes = 16384, kMessageMax = 8192;      // MSGMNB, MSGMAX

  LinuxIpcManager(ISystemSync& sync, allocators::IAllocator& allocator) : sync_(sync), allocator_(allocator) {}
  ~LinuxIpcManager();
  LinuxIpcManager(const LinuxIpcManager&) = delete;
  LinuxIpcManager& operator=(const LinuxIpcManager&) = delete;
  ISystemSync& sync() { return sync_; }
  allocators::IAllocator& allocator() { return allocator_; }

  // Who made a thing and who may use it (struct ipc64_perm). Nothing is checked against it: every program runs as root.
  struct Permission {
    uint32_t key = 0, uid = 0, gid = 0, creator_uid = 0, creator_gid = 0, mode = 0;
    uint16_t sequence = 0;      // goes up each time the slot is used again, so that an old id does not name the new thing
  };
  struct Segment {
    bool used = false;
    bool removed = false;       // IPC_RMID was called: it goes when the last process has detached it, and no key finds it meanwhile
    Permission permission;
    uint64_t size = 0;          // as asked for (the block is whole pages)
    MemoryBlock* block = nullptr;      // one use is this table's; each mapping has its own
    uint64_t attach_time = 0, detach_time = 0, change_time = 0;
    uint32_t creator = 0, last = 0;    // process ids
  };
  struct SemaphoreSet {
    bool used = false;
    Permission permission;
    uint32_t count = 0;
    uint64_t operation_time = 0, change_time = 0;
    uint16_t value[kSemsPerSet] = {};
    uint32_t last[kSemsPerSet] = {};                  // the process that last changed each
    uint16_t waiting_for_more[kSemsPerSet] = {};      // threads waiting for the value to rise (GETNCNT), and for it to be zero (GETZCNT)
    uint16_t waiting_for_zero[kSemsPerSet] = {};
  };
  // What a process's semaphore operations with SEM_UNDO come to: taken back when the process ends.
  struct Undo {
    bool used = false;
    uint64_t process = 0;
    uint32_t id = 0;
    uint32_t semaphore = 0;
    int32_t adjust = 0;
  };
  // A queue's messages lie one after another in `data`: the type (8 bytes), the size (4 bytes), the text.
  struct Queue {
    bool used = false;
    Permission permission;
    UniquePointer<ByteBuffer> data;
    uint64_t messages = 0, bytes = 0, limit = kQueueBytes;      // bytes: of text, which is what the limit counts
    uint64_t send_time = 0, receive_time = 0, change_time = 0;
    uint32_t last_sender = 0, last_receiver = 0;
  };

  Segment segments[kSegments];
  SemaphoreSet sets[kSemSets];
  Undo undos[kUndos];
  Queue queues[kQueues];

 private:
  ISystemSync& sync_;
  allocators::IAllocator& allocator_;
};

// The system's one manager, made on first use beside the object manager (null before there is one).
LinuxIpcManager* system_linux_ipc();
// Tests: use this one instead (null: back to the made-on-first-use one).
void set_system_linux_ipc(LinuxIpcManager* manager);

namespace linuxabi {

uint64_t sys_shmget(Process& p, uint64_t key, uint64_t size, uint64_t flags);
uint64_t sys_shmat(Process& p, uint64_t id, uint64_t address, uint64_t flags);
uint64_t sys_shmdt(Process& p, uint64_t address);
uint64_t sys_shmctl(Process& p, uint64_t id, uint64_t command, uint64_t buffer);
uint64_t sys_semget(Process& p, uint64_t key, uint64_t count, uint64_t flags);
// semop and semtimedop (`timeout`: a struct timespec to wait at most, or 0)
uint64_t sys_semtimedop(Process& p, uint64_t id, uint64_t operations, uint64_t count, uint64_t timeout);
uint64_t sys_semctl(Process& p, UserArch arch, uint64_t id, uint64_t semaphore, uint64_t command, uint64_t argument);
uint64_t sys_msgget(Process& p, uint64_t key, uint64_t flags);
uint64_t sys_msgsnd(Process& p, uint64_t id, uint64_t message, uint64_t size, uint64_t flags);
uint64_t sys_msgrcv(Process& p, uint64_t id, uint64_t message, uint64_t size, uint64_t type, uint64_t flags);
uint64_t sys_msgctl(Process& p, uint64_t id, uint64_t command, uint64_t buffer);
// A Linux process ended: its semaphore operations made with SEM_UNDO are taken back.
void ipc_process_gone(uint64_t pid);

}  // namespace linuxabi
}  // namespace process
