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
#include <stddef.h>
#include <stdint.h>
#include <type_traits>
#include "Kernel/Interrupts.hpp"
#include "Kernel/Thread.hpp"

// Read-copy-update, shaped after C++26 <rcu> (P2545): readers run inside a read-side critical section and
// never block or write shared data; a writer publishes a new version of an object, retires the old one, and
// the old one is reclaimed only after a grace period, i.e. once every reader that could still see it is done.
//
//   std (C++26)                          here
//   std::rcu_domain                      kernel::RcuDomain
//   std::rcu_default_domain()            kernel::rcu_default_domain()
//   dom.lock() / dom.unlock()            dom.lock() returns a token that unlock(token) takes; RcuReader does both
//   std::scoped_lock<rcu_domain>         kernel::RcuReader
//   std::rcu_obj_base<T, D>              kernel::RcuObjectBase<T, D>   (retire(d, dom))
//   std::rcu_retire(p, d, dom)           kernel::rcu_retire(p, d, dom) (p must derive from RcuObjectBase<T, D>)
//   std::rcu_synchronize(dom)            kernel::rcu_synchronize(dom)
//   std::rcu_barrier(dom)                kernel::rcu_barrier(dom)
// plus rcu_dereference / rcu_assign_pointer for the pointer being published (the memory-ordering half of RCU).
//
// Differences from the standard, all because this is kernel code:
//  - Retiring never allocates (it cannot fail), so the retired object carries its own node: derive from
//    RcuObjectBase. The deleter is stored in the object, so it is copied at retire time.
//  - The kernel has no thread-local storage, so lock() hands back the counter it joined and unlock() needs it back.
//    RcuReader keeps it; use that unless you must split lock and unlock.
//  - Reader counters are sharded by core (kernel::current_core(), one load from the core's GS-relative record), so
//    readers on different cores touch different cache lines. A reader that migrates to another core inside its section
//    is fine: the token names the shard it joined, and unlock() leaves that one.
//
// Kernel properties:
//  - A read-side section is two atomic increments/decrements: no lock, no interrupt masking, no allocation.
//    Usable from threads, interrupt handlers and early boot, may nest, and may be left on another core than it
//    began on. A reader that sleeps or is preempted holds up reclamation (not other readers) until it finishes:
//    keep sections short and never block inside one.
//  - retire() is lock-free and does not wait for anything. Every so often it also polls for grace period
//    completion with a try-lock, so it too is usable from interrupt handlers; the deleters then run in that context.
//    poll() can be called from a timer tick or idle loop to make progress without retiring.
//  - rcu_synchronize() and rcu_barrier() wait (spin) in the caller: thread context only, and never inside a
//    read-side section or a deleter (the grace period would wait for the caller itself).
//
// How the grace period works: per core shard, two reader counters, selected by the parity of a domain epoch. A reader
// joins the counter of its core's shard and the current parity (re-checking the epoch so it cannot join one a writer has
// already left). A grace period flips the epoch and waits for the counters of the old parity, in every shard, to reach zero, then does that a second time
// so that readers on either counter are covered (a single flip would miss a reader that started two flips ago and
// has been running ever since). Readers that start after a flip began after the retired objects were unlinked and
// are not waited for. Objects retired during a grace period go into the next one.
//
// Usage:
//   struct Config : RcuObjectBase<Config, FreeConfig> { int value; };
//   Config* volatile current;
//   // reader
//   { RcuReader reader;  const Config* c = rcu_dereference(&current);  use(c->value); }
//   // writer (serialised among writers by the caller)
//   Config* old = current;  rcu_assign_pointer(&current, fresh);  old->retire(FreeConfig{});
namespace kernel {

// Link and callback of one retired object. Embedded through RcuObjectBase, or used directly with RcuDomain::retire_node().
struct RcuNode {
  RcuNode* next = nullptr;
  void (*callback)(RcuNode* node) = nullptr;   // runs once after the grace period; may free the object holding `node`
};

class RcuDomain {
 public:
  constexpr RcuDomain() = default;
  RcuDomain(const RcuDomain&) = delete;
  RcuDomain& operator=(const RcuDomain&) = delete;
  // (Trivial destructor, so a global domain needs no atexit registration in the freestanding kernel: call barrier()
  // before a domain that is not global goes away, or whatever is still retired never runs.)

  // Reader counters per parity; cores beyond this share (core % kShards), which only costs some cache line sharing.
  static constexpr uint32_t kShards = 16;

  // Enters a read-side section; the returned token (shard and parity) goes to unlock().
  uint32_t lock() {
    const uint32_t shard = current_core() % kShards;
    for (;;) {
      const uint32_t parity = __atomic_load_n(&epoch_, __ATOMIC_SEQ_CST) & 1;
      __atomic_fetch_add(&counters_[shard][parity].readers, 1, __ATOMIC_SEQ_CST);
      // Sequentially consistent against the flip in poll(): either the flip comes after this increment (and the
      // writer waits for it), or the epoch has moved on, we undo and join the new counter.
      if ((__atomic_load_n(&epoch_, __ATOMIC_SEQ_CST) & 1) == parity) return shard * 2 + parity;
      __atomic_fetch_sub(&counters_[shard][parity].readers, 1, __ATOMIC_RELEASE);
    }
  }
  void unlock(uint32_t token) { __atomic_fetch_sub(&counters_[(token >> 1) % kShards][token & 1].readers, 1, __ATOMIC_RELEASE); }

  // Queues `node` (callback already set) to run after a grace period. Lock-free and allocation-free; `node` must stay
  // valid until the callback has run.
  void retire_node(RcuNode& node) {
    enqueue(node);
    if (__atomic_load_n(&outstanding_, __ATOMIC_RELAXED) >= kAutoPollThreshold) poll();
  }

  // Advances grace periods as far as the readers allow without waiting, and runs the callbacks that became due.
  // Returns how many ran. Does nothing if another core is already polling. Safe from any context.
  uint32_t poll() {
    if (__atomic_exchange_n(&poll_lock_, 1, __ATOMIC_ACQUIRE)) return 0;
    uint32_t ran = 0;
    for (;;) {
      if (phase_ == Phase::Idle) {
        RcuNode* stack = __atomic_exchange_n(&retired_head_, static_cast<RcuNode*>(nullptr), __ATOMIC_ACQUIRE);
        if (!stack) break;
        batch_ = reverse(stack);   // (oldest first, so callbacks run in retire order)
        old_parity_ = __atomic_load_n(&epoch_, __ATOMIC_SEQ_CST) & 1;
        __atomic_fetch_add(&epoch_, 1, __ATOMIC_SEQ_CST);
        phase_ = Phase::FirstFlip;
        continue;
      }
      if (!drained(old_parity_)) break;   // a reader of that era is still running
      if (phase_ == Phase::FirstFlip) {
        old_parity_ ^= 1;   // the parity the first flip made current; flip away from it, too
        __atomic_fetch_add(&epoch_, 1, __ATOMIC_SEQ_CST);
        phase_ = Phase::SecondFlip;
        continue;
      }
      phase_ = Phase::Idle;   // second flip drained: every reader that could see the batch has finished
      RcuNode* node = batch_;
      batch_ = nullptr;
      while (node) {
        RcuNode* next = node->next;   // (read first: the callback may free the node)
        node->callback(node);
        __atomic_fetch_sub(&outstanding_, 1, __ATOMIC_RELEASE);
        ++ran;
        node = next;
      }
    }
    __atomic_store_n(&poll_lock_, 0, __ATOMIC_RELEASE);
    return ran;
  }

  // Blocks until every read-side section that began before the call has ended, and every callback retired before
  // the call has run (this is rcu_barrier; waiting for the callbacks implies the grace period).
  void synchronize() { barrier(); }
  void barrier() {
    struct Waiter : RcuNode {
      uint32_t done = 0;
    } waiter;
    waiter.callback = [](RcuNode* node) { __atomic_store_n(&static_cast<Waiter*>(node)->done, 1, __ATOMIC_RELEASE); };
    enqueue(waiter);
    // (callbacks run in retire order, one batch after the other, so everything queued before the waiter has run by then)
    while (!__atomic_load_n(&waiter.done, __ATOMIC_ACQUIRE)) {
      poll();
      cpu_relax();
    }
  }

  // Retired objects whose callback has not finished yet (approximate while other cores run).
  uint32_t pending() const { return __atomic_load_n(&outstanding_, __ATOMIC_RELAXED); }

 private:
  static constexpr uint32_t kAutoPollThreshold = 32;
  enum class Phase : uint8_t { Idle, FirstFlip, SecondFlip };

  struct alignas(64) Counter {   // (a line of its own: readers and the polling core must not false-share with the other counter)
    uint32_t readers = 0;
  };

  // True once no shard has a reader on the counter of `parity`. The shards are read one after the other, which is enough:
  // after the flip nothing joins the old parity for good (a transient join is undone at once), so a shard seen at zero stays so.
  bool drained(uint32_t parity) const {
    for (const Counter (&shard)[2] : counters_) {
      if (__atomic_load_n(&shard[parity].readers, __ATOMIC_SEQ_CST) != 0) return false;
    }
    return true;
  }
  void enqueue(RcuNode& node) {
    __atomic_fetch_add(&outstanding_, 1, __ATOMIC_RELAXED);
    RcuNode* old = __atomic_load_n(&retired_head_, __ATOMIC_RELAXED);
    do {
      node.next = old;
    } while (!__atomic_compare_exchange_n(&retired_head_, &old, &node, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  }
  static RcuNode* reverse(RcuNode* list) {
    RcuNode* out = nullptr;
    while (list) {
      RcuNode* next = list->next;
      list->next = out;
      out = list;
      list = next;
    }
    return out;
  }

  Counter counters_[kShards][2];
  alignas(64) uint32_t epoch_ = 0;   // (read by every reader, written only by a grace period: its own line)
  uint32_t outstanding_ = 0;
  RcuNode* retired_head_ = nullptr;   // lock-free stack of what was retired since the last batch was taken
  // Owned by whoever holds poll_lock_:
  uint32_t poll_lock_ = 0;
  Phase phase_ = Phase::Idle;
  uint32_t old_parity_ = 0;
  RcuNode* batch_ = nullptr;          // the batch the grace period in flight covers, oldest first
};

// The domain used when none is given.
inline constinit RcuDomain g_rcu_default_domain;
inline RcuDomain& rcu_default_domain() { return g_rcu_default_domain; }

// A read-side critical section for its lifetime.
class RcuReader {
 public:
  explicit RcuReader(RcuDomain& domain = rcu_default_domain()) : domain_(domain), token_(domain.lock()) {}
  ~RcuReader() { domain_.unlock(token_); }
  RcuReader(const RcuReader&) = delete;
  RcuReader& operator=(const RcuReader&) = delete;

 private:
  RcuDomain& domain_;
  uint32_t token_;
};

// Base of an object that is retired through RCU. T is the derived class, D a callable taking T* that destroys it
// (it is copied into the object at retire(), so it may carry state). Retire an object once, after it is unreachable
// for new readers; the object, and so D, must stay valid until D runs.
template <typename T, typename D>
class RcuObjectBase : private RcuNode {
 public:
  void retire(D deleter = D(), RcuDomain& domain = rcu_default_domain()) {
    deleter_ = static_cast<D&&>(deleter);
    callback = &run;
    domain.retire_node(*this);
  }

 protected:
  RcuObjectBase() = default;
  RcuObjectBase(const RcuObjectBase&) = default;
  RcuObjectBase& operator=(const RcuObjectBase&) { return *this; }   // (the retire state belongs to the object, not to its value)
  ~RcuObjectBase() = default;

 private:
  static void run(RcuNode* node) {
    auto* base = static_cast<RcuObjectBase*>(node);
    D deleter = static_cast<D&&>(base->deleter_);   // (the deleter may free the storage it lives in)
    deleter(static_cast<T*>(base));
  }

  D deleter_{};
};

template <typename T, typename D>
void rcu_retire(T* object, D deleter, RcuDomain& domain = rcu_default_domain()) {
  object->retire(static_cast<D&&>(deleter), domain);
}

inline void rcu_synchronize(RcuDomain& domain = rcu_default_domain()) { domain.synchronize(); }
inline void rcu_barrier(RcuDomain& domain = rcu_default_domain()) { domain.barrier(); }

// Reads a published pointer inside a read-side section (acquire: what the writer initialised before publishing is visible).
template <typename T>
inline T* rcu_dereference(T* const* location) { return __atomic_load_n(location, __ATOMIC_ACQUIRE); }
// Publishes `pointer` (release: the pointee's initialisation is visible to every reader that sees the pointer).
template <typename T>
inline void rcu_assign_pointer(T** location, T* pointer) { __atomic_store_n(location, pointer, __ATOMIC_RELEASE); }

}  // namespace kernel
