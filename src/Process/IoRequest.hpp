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
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/ArrayView.hpp"

namespace process::io {

// Asynchronous I/O of the Windows ABI, the part that does not depend on what the I/O is on.
//
// An IoRequest is what NT calls an IRP that has not finished: a call of NtReadFile, NtWriteFile or NtFsControlFile on a handle opened for overlapped use that could not be answered
// at once. It is queued on the object the call is for (a pipe end; later files and mailslots) and says where the answer goes when the object completes it: the IO_STATUS_BLOCK, the
// event, the APC of the thread that made the call, the completion port the handle is tied to. The object completes it (the peer wrote, the writer closed, a listen was satisfied,
// room was made, the call was cancelled); the requester's process then applies the completion to its own memory (the completer may be another process, whose lock it cannot
// take): the table keeps the finished requests in the order they finished until the owner takes them.
//
// A completion port is a queue of packets with the key of the handle and the context of the call; whoever removes a packet is a thread that waits on the port (the port
// object's waiting is done by the caller, with the one lock of the system).
//
// Neither class locks: the caller holds the system lock (ObjectManager::sync()) around every call. The host test runs them with a mutex in its place and several threads.

struct IoRequest {
  uint32_t id = 0;                 // 0: none; set by IoRequestTable::add
  uint64_t owner = 0;              // the process that made the call
  uint64_t thread = 0;             // the thread that made it (the APC is for it)
  uint64_t status_block = 0;       // the user address of the IO_STATUS_BLOCK (0: none)
  uint64_t apc_routine = 0, apc_context = 0;
  uint64_t buffer = 0;             // a read: where its data go (user address)
  uint32_t buffer_length = 0;
  uint32_t event_object = 0;       // a referenced event object that is set when the call is over (0: none)
  uint32_t port_object = 0;        // a referenced completion port object that gets a packet (0: none)
  uint64_t port_key = 0, port_context = 0;      // the packet: the key the handle was tied with, and the call's own context (the OVERLAPPED)
  uint32_t source = 0;             // what the call is queued on (an object id) ...
  uint32_t source_end = 0;         // ... which end of it ...
  uint32_t source_tag = 0;         // ... and the object's own number for the operation
  bool completed = false;
  bool cancel_requested = false;
  bool on_close = false;           // cancelled because the owner's last handle to the file object was closed: no event, no APC (a port hears of it only if there was no event)
  uint32_t status = 0;             // the outcome, once completed
  uint64_t information = 0;
  uint64_t sequence = 0;           // the order of completion
  ByteView data;                   // a read's data, owned by the table's allocator until taken (release_data)
};

class IoRequestTable {
 public:
  static constexpr uint32_t kMaxRequests = 4096;

  explicit IoRequestTable(allocators::IAllocator& allocator) : allocator_(allocator), requests_(allocator), done_(allocator) {}
  ~IoRequestTable();
  IoRequestTable(const IoRequestTable&) = delete;
  IoRequestTable& operator=(const IoRequestTable&) = delete;

  // Queues a request (its id is assigned). nullptr when the table is full or memory is short.
  IoRequest* add(const IoRequest& request);
  IoRequest* find(uint32_t id);
  // A pending request ends with an outcome and, for a read, its data (owned from now on by the table). False if there is no such pending request (data is then not taken).
  bool complete(uint32_t id, uint32_t status, uint64_t information, ByteView data = ByteView());
  // Completed requests that are not taken yet, in all: a cheap look (no lock) for whoever would otherwise scan for its own.
  uint32_t ready() const { return __atomic_load_n(&ready_, __ATOMIC_ACQUIRE); }
  // Moves the finished requests of a process, oldest first, into `out` (at most `capacity`) and removes them from the table; the data of each is now the caller's.
  uint32_t take_completed(uint64_t owner, IoRequest* out, uint32_t capacity);
  // Removes the requests of a process, finished or not, without applying them (at most `capacity` of them, returned in `out` without their data, which is released); call
  // again until it returns 0. A request that is still pending has been taken away from the object it waited on by the caller before.
  uint32_t remove_owner(uint64_t owner, IoRequest* out, uint32_t capacity);
  // Removes a request (a finished one the caller took by other means, or one that never started).
  void erase(uint32_t id);
  uint64_t count() const { return requests_.count(); }
  uint64_t pending_count() const { return pending_; }
  template <class F>
  void for_each(F&& f) { for (IoRequest& r : requests_) f(r); }
  void release_data(ByteView& data) { release(data); }

 private:
  void release(ByteView& data);
  allocators::IAllocator& allocator_;
  Hive<IoRequest> requests_;
  Queue<uint32_t> done_;         // ids of finished requests, oldest first (may name one that was removed since)
  uint32_t next_id_ = 1;
  uint32_t ready_ = 0;
  uint64_t pending_ = 0, sequence_ = 0;
};

// What a handle tied to a completion port reports: IO_STATUS_BLOCK Status and Information, plus the key and the context of the call.
struct Packet {
  uint64_t key = 0, context = 0, information = 0;
  uint32_t status = 0;
  uint64_t status_block = 0, owner = 0;      // a call cancelled by the close of a handle: the IO_STATUS_BLOCK of the process that made it is written when the packet is taken (0: none)
};

class CompletionQueue {
 public:
  static constexpr uint64_t kMaxPackets = 1u << 20;

  explicit CompletionQueue(allocators::IAllocator& allocator) : packets_(allocator) {}
  // False when memory is short or the queue is at its limit.
  bool post(const Packet& packet);
  // Takes the oldest packet; false if there is none.
  bool remove(Packet* out) { return packets_.dequeue(out); }
  uint32_t remove_many(Packet* out, uint32_t capacity);
  uint64_t depth() const { return packets_.count(); }
  void clear() { packets_.clear(); }

 private:
  Queue<Packet> packets_;
};

}  // namespace process::io
