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

#include "Browser/ResourcePrefetcher.hpp"
#include "Utility/Move.hpp"

namespace browser {

ResourcePrefetcher::Entry* ResourcePrefetcher::find_locked(StringView address) {
  for (UniquePointer<Entry>& e : entries_)
    if (e->generation == generation_ && e->address.view().equals(address)) return e.get();
  return nullptr;
}

void ResourcePrefetcher::remove_locked(const Entry* entry) {
  for (uint64_t i = 0; i < entries_.count(); ++i) {
    if (entries_[i].get() != entry) continue;
    if (entries_[i]->state == State::Done) cached_bytes_ -= entries_[i]->body.size();
    entries_.remove_at(i);
    return;
  }
}

void ResourcePrefetcher::request(StringView address) {
  if (address.empty() || !kernel::running()) return;
  bool start = false;
  {
    kernel::ScopedMutex lock(mutex_);
    if (closed_ || find_locked(address)) return;
    if (entries_.count() >= kMaxEntries) return;   // (too much asked for at once: the rest is fetched when the page wants it)
    auto entry = UniquePointer<Entry>::make(allocator_, allocator_);
    if (!entry || !entry->address.assign(address)) return;
    entry->generation = generation_;
    if (!entries_.push_back(util::move(entry))) return;
    if (__atomic_load_n(&workers_, __ATOMIC_ACQUIRE) < kMaxWorkers) {
      __atomic_fetch_add(&workers_, 1u, __ATOMIC_ACQ_REL);
      start = true;
    }
  }
  if (!start) return;
  kernel::Thread* thread = kernel::spawn(&ResourcePrefetcher::worker_main, this, "prefetch");
  if (thread) {
    kernel::detach(thread);
    return;
  }
  __atomic_fetch_sub(&workers_, 1u, __ATOMIC_ACQ_REL);   // (no thread to be had: the entry stays queued and is withdrawn by take(), or taken by a later worker)
}

// A worker takes the oldest queued entry of the current generation until there is none, then ends.
void ResourcePrefetcher::work() {
  for (;;) {
    Entry* entry = nullptr;
    String address(allocator_);
    {
      kernel::ScopedMutex lock(mutex_);
      if (!closed_)
        for (UniquePointer<Entry>& e : entries_)
          if (e->state == State::Queued && e->generation == generation_) {
            entry = e.get();
            break;
          }
      if (entry) {
        entry->state = State::Running;
        address.assign(entry->address.view());
      }
    }
    if (!entry) {
      __atomic_fetch_sub(&workers_, 1u, __ATOMIC_ACQ_REL);   // (after the unlock, and the last thing this thread does with `this`: the destructor may free it now)
      return;
    }
    ByteBuffer body(allocator_);
    const bool ok = fetch_(context_, address.view(), &body);   // (no lock held: this is the slow part)
    kernel::ScopedMutex lock(mutex_);
    if (entry->generation != generation_ || closed_) {   // a new page was opened meanwhile: nobody wants it
      remove_locked(entry);
      continue;
    }
    if (ok && cached_bytes_ <= kMaxCachedBytes && body.size() <= kMaxCachedBytes - cached_bytes_ && entry->body.append(body.view())) {
      cached_bytes_ += entry->body.size();
      entry->state = State::Done;
    } else if (ok) {
      remove_locked(entry);   // (over the budget: the caller fetches it itself if it wants it)
    } else {
      entry->state = State::Failed;
    }
  }
}

ResourcePrefetcher::Result ResourcePrefetcher::take(StringView address, uint32_t wait_ms, ByteBuffer* out) {
  if (!kernel::running()) return Result::Miss;
  for (uint64_t waited_us = 0;; waited_us += 1000) {
    {
      kernel::ScopedMutex lock(mutex_);
      Entry* entry = find_locked(address);
      if (!entry) return Result::Miss;
      switch (entry->state) {
        case State::Queued:
          remove_locked(entry);   // (not started: no point waiting behind other downloads)
          return Result::Miss;
        case State::Done: {
          const bool ok = out->append(entry->body.view());
          remove_locked(entry);
          return ok ? Result::Hit : Result::Miss;
        }
        case State::Failed:
          remove_locked(entry);
          return Result::Failed;
        case State::Running: break;
      }
    }
    if (waited_us >= uint64_t{wait_ms} * 1000) return Result::Failed;   // (it takes too long: the page goes on without it)
    kernel::sleep_us(1000);
  }
}

void ResourcePrefetcher::cancel_all() {
  kernel::ScopedMutex lock(mutex_);
  ++generation_;
  for (uint64_t i = 0; i < entries_.count();) {
    if (entries_[i]->state == State::Running) {
      ++i;   // (its worker removes it when it is done)
      continue;
    }
    if (entries_[i]->state == State::Done) cached_bytes_ -= entries_[i]->body.size();
    entries_.remove_at(i);
  }
}

void ResourcePrefetcher::shutdown() {
  {
    kernel::ScopedMutex lock(mutex_);
    closed_ = true;
  }
  cancel_all();
  while (__atomic_load_n(&workers_, __ATOMIC_ACQUIRE) != 0) kernel::sleep_us(1000);
}

}  // namespace browser
