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

// Downloads the scripts, style sheets and pictures a page names in the background, so they arrive in parallel while the page is read and its scripts run. The
// page itself still asks for each resource at the moment it needs it, in its own order (a script runs when the ones before it have run); what changes is that
// the answer is often already there. A resource nobody asked for in advance is fetched by the caller, as before.
//
// It cannot hold the system up:
//  - at most kMaxWorkers threads run at once, and they exist only while there is something to fetch (nothing idles);
//  - at most kMaxEntries addresses are remembered and at most kMaxCachedBytes of answers are kept; more is dropped, not queued without end;
//  - every fetch is made by the caller's FetchFunction, which has its own timeout, and take() waits no longer than the time it is given;
//  - a resource that has not been started yet is never waited for: take() hands it back to the caller (a Miss), who fetches it at once;
//  - cancel_all() (a new page is opened) forgets what is queued and throws away what is running when it finishes;
//  - the destructor waits for the running fetches (at most one fetch timeout) and never leaves a thread behind that points at freed memory.
// Without threads (kernel::running() is false: the tests) request() does nothing and every take() is a Miss: the page fetches serially, as it always did.
#pragma once
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace browser {

class ResourcePrefetcher {
 public:
  static constexpr uint32_t kMaxWorkers = 4, kMaxEntries = 96;
  static constexpr uint64_t kMaxCachedBytes = 12u << 20;

  // Fetches `address` and puts the body in `body`. True only for a usable answer (a status below 400). Called on worker threads, several at a time.
  using FetchFunction = bool (*)(void* context, StringView address, ByteBuffer* body);

  enum class Result : uint8_t {
    Hit,     // the answer is in `out`
    Failed,  // it was fetched (or was being fetched for too long) and failed: do not ask again
    Miss,    // nothing is known about it: fetch it yourself
  };

  ResourcePrefetcher(allocators::IAllocator& allocator, FetchFunction fetch, void* context)
      : allocator_(allocator), fetch_(fetch), context_(context), entries_(allocator) {}
  ~ResourcePrefetcher() { shutdown(); }
  ResourcePrefetcher(const ResourcePrefetcher&) = delete;
  ResourcePrefetcher& operator=(const ResourcePrefetcher&) = delete;

  // Queues a download unless the address is already known. Returns at once. Safe from any thread.
  void request(StringView address);
  // Takes the answer for `address`. A download that is running is waited for, up to `wait_ms`; one that is only queued is withdrawn (Miss). `out` is appended to.
  Result take(StringView address, uint32_t wait_ms, ByteBuffer* out);
  // Forgets everything that is not running; what is running is thrown away when it is done.
  void cancel_all();
  // Stops taking requests and waits for the running downloads. Called by the destructor.
  void shutdown();

  uint32_t workers() const { return __atomic_load_n(&workers_, __ATOMIC_ACQUIRE); }

 private:
  enum class State : uint8_t { Queued, Running, Done, Failed };
  struct Entry {
    explicit Entry(allocators::IAllocator& a) : address(a), body(a) {}
    String address;
    ByteBuffer body;
    State state = State::Queued;
    uint32_t generation = 0;
  };

  static void worker_main(void* self) { static_cast<ResourcePrefetcher*>(self)->work(); }
  void work();
  Entry* find_locked(StringView address);   // (of the current generation)
  void remove_locked(const Entry* entry);

  allocators::IAllocator& allocator_;
  FetchFunction fetch_;
  void* context_;
  kernel::Mutex mutex_;
  List<UniquePointer<Entry>> entries_;
  uint64_t cached_bytes_ = 0;
  uint32_t generation_ = 0;
  uint32_t workers_ = 0;   // threads started and not yet finished (atomic: the destructor reads it without the lock)
  bool closed_ = false;
};

}  // namespace browser
