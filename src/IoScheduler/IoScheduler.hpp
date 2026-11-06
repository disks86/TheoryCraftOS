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

// The I/O scheduler of one device (docs/IoScheduling.md): the fair queue (FairQueue.hpp) decides which waiting request goes next, the depth
// controller (LatencyController.hpp) how many of each domain may be on the device at once. There is no dispatcher thread. The thread that
// submits a request, and the one that completes one, hand out the free device slots to the requests that should have them, and a request's
// own thread then runs it on the device. So a request that can go at once costs no context switch, and one that waits is woken by whoever
// finishes before it.
//
// Built for many cores and fast flash:
// - While nothing is queued, a request that finds a slot of its domain free goes straight to the device without the lock: one compare-and-swap
//   on the slot word (Idle requests excepted: they always queue, to wait for the quiet the class asks for). Only when the device is
//   saturated do requests queue and the fair queue decide.
// - The slots are one 64-bit word (16 bits for the total in flight, 16 per domain), taken and given back with atomics, so completions on any
//   core never block each other; the lock guards only the fair queue, held for a pick and a commit at a time.
// - Lost wake-ups cannot happen: a submitter counts itself queued before it looks for a slot under the lock, a completer gives its slot back
//   before it looks at the queued count (both sequentially consistent), so one of them always sees the other.
//
// `Lock` is anything with lock()/unlock() (kernel::RawSpinLock in the kernel, a std::mutex wrapper in the host tests). Waiters are woken
// through IoWaiter::grant() with no lock held, and a waiter whose wait times out calls kick() (wait_hint_us() says how long to wait).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "IoScheduler/FairQueue.hpp"
#include "IoScheduler/IoTypes.hpp"
#include "IoScheduler/LatencyController.hpp"
#include "Utility/AllocatedView.hpp"

namespace iosched {

// Whoever waits for a queued request. grant() is called once, from any thread, when the request may go to the device; the object must stay
// alive until then (and grant() must be the last thing that touches it, as the waiter may return right after).
class IoWaiter {
 public:
  virtual void grant() = 0;

 protected:
  ~IoWaiter() = default;
};

struct IoSchedulerConfig {
  uint32_t depth = 1;            // requests the device takes at once (what its driver can have in flight)
  uint32_t capacity = 512;       // requests queued or in flight at once; more get SubmitResult::Full
  uint32_t window_us = 100000;   // the depth controller's window
  bool bypass = true;            // the lock-free path while nothing is queued
  FairQueueConfig fair;
};

struct IoTicket {
  static constexpr uint32_t kBypass = 0xFFFFFFFEu;
  uint32_t index = FairQueue::kNone;
  IoDomain domain = IoDomain::Read;
  uint64_t dispatched_us = 0;    // (bypass tickets: the fair queue keeps the time for the others)
};

enum class SubmitResult : uint8_t {
  Granted,   // go to the device now
  Queued,    // wait for IoWaiter::grant()
  Full,      // no room: try again a little later
};

struct IoSchedulerStatistics {
  FairQueueStatistics fair;
  LatencyControllerStatistics depth;
  uint64_t bypassed = 0;
  uint32_t queued = 0, in_flight = 0;
  uint32_t peak_in_flight = 0;   // the most requests that were on the device at once
};

template <class Lock>
class IoScheduler {
 public:
  static constexpr uint64_t kSafetyKickUs = 10000;   // the longest a waiter waits before it looks again itself

  explicit IoScheduler(allocators::IAllocator& allocator) : allocator_(allocator), fair_(allocator) {}
  ~IoScheduler() { release_view(allocator_, waiters_); }
  IoScheduler(const IoScheduler&) = delete;
  IoScheduler& operator=(const IoScheduler&) = delete;

  bool initialize(const IoSchedulerConfig& config, uint64_t now_us) {
    config_ = config;
    if (config_.depth == 0) config_.depth = 1;
    if (config_.depth > 0xFFFF) config_.depth = 0xFFFF;
    release_view(allocator_, waiters_);
    if (!fair_.initialize(config_.capacity, config_.fair)) return false;
    waiters_ = allocate_view<IoWaiter*>(allocator_, config_.capacity);
    if (waiters_.empty()) return false;
    for (IoWaiter*& w : waiters_) w = nullptr;
    controller_.initialize(config_.depth, config_.fair.cost, config_.window_us, now_us);
    __atomic_store_n(&slots_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&queued_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&bypassed_, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&peak_, 0, __ATOMIC_RELAXED);
    return true;
  }

  // A request of `issuer` wants the device. Granted: run it now, then complete(). Queued: wait for waiter.grant() (it is not called when the
  // answer is Granted), then run it and complete(). Full: nothing was queued.
  SubmitResult submit(uint64_t issuer, IoPriority priority, IoOp op, uint64_t bytes, IoWaiter& waiter, IoTicket* ticket, uint64_t now_us) {
    const IoDomain d = domain_of(op);
    priority = clamp_priority(priority);
    ticket->domain = d;
    if (config_.bypass && priority.io_class != IoClass::Idle && __atomic_load_n(&queued_, __ATOMIC_SEQ_CST) == 0 && try_acquire(d)) {
      ticket->index = IoTicket::kBypass;
      ticket->dispatched_us = now_us;
      __atomic_fetch_add(&bypassed_, 1, __ATOMIC_RELAXED);
      return SubmitResult::Granted;
    }
    lock_.lock();
    const uint32_t t = fair_.enqueue(issuer, priority, op, bytes, now_us);
    if (t == FairQueue::kNone) {
      lock_.unlock();
      return SubmitResult::Full;
    }
    waiters_[t] = &waiter;
    __atomic_fetch_add(&queued_, 1, __ATOMIC_SEQ_CST);
    lock_.unlock();
    ticket->index = t;
    return dispatch(now_us, t) ? SubmitResult::Granted : SubmitResult::Queued;
  }

  // The request is off the device: its slot goes to whoever should have it next.
  void complete(const IoTicket& ticket, uint64_t now_us) {
    release(ticket.domain);
    uint64_t dispatched = ticket.dispatched_us;
    if (ticket.index != IoTicket::kBypass) {
      lock_.lock();
      dispatched = fair_.dispatched_us(ticket.index);
      fair_.complete(ticket.index, now_us);
      lock_.unlock();
    }
    controller_.record(ticket.domain, now_us >= dispatched ? now_us - dispatched : 0, now_us);
    if (__atomic_load_n(&queued_, __ATOMIC_SEQ_CST) != 0) dispatch(now_us, FairQueue::kNone);
  }

  // Hands out what may go now (a waiter whose wait timed out calls it: the Idle class's grace time has passed, or as a safety net).
  void kick(uint64_t now_us) { dispatch(now_us, FairQueue::kNone); }

  // How long a waiter should wait before it kicks.
  uint64_t wait_hint_us(uint64_t now_us) {
    lock_.lock();
    const uint64_t until = fair_.idle_wait_until(now_us);
    lock_.unlock();
    if (until == 0) return kSafetyKickUs;
    return until > now_us ? until - now_us : 1;
  }

  IoSchedulerStatistics statistics() {
    IoSchedulerStatistics s;
    lock_.lock();
    s.fair = fair_.statistics();
    s.queued = fair_.queued();
    lock_.unlock();
    s.depth = controller_.statistics();
    s.bypassed = __atomic_load_n(&bypassed_, __ATOMIC_RELAXED);
    s.in_flight = static_cast<uint32_t>(__atomic_load_n(&slots_, __ATOMIC_RELAXED) & 0xFFFF);
    s.peak_in_flight = __atomic_load_n(&peak_, __ATOMIC_RELAXED);
    return s;
  }

  // For tests: the issuer's queue (under the lock).
  bool issuer_state(uint64_t issuer, IssuerState* out) {
    lock_.lock();
    const bool found = fair_.issuer_state(issuer, out);
    lock_.unlock();
    return found;
  }
  LatencyController& controller() { return controller_; }
  uint32_t depth() const { return config_.depth; }

 private:
  static constexpr uint32_t kGrantBatch = 16;
  static constexpr uint64_t one_slot(IoDomain d) { return 1 + (uint64_t{1} << (16 * (domain_index(d) + 1))); }
  static uint32_t field(uint64_t word, uint32_t d) { return static_cast<uint32_t>(word >> (16 * (d + 1))) & 0xFFFF; }

  // The domains that have a slot free in `word`.
  uint32_t free_domains(uint64_t word) const {
    if ((word & 0xFFFF) >= config_.depth) return 0;
    uint32_t mask = 0;
    for (uint32_t d = 0; d < kDomainCount; ++d)
      if (field(word, d) < controller_.limit(static_cast<IoDomain>(d))) mask |= 1u << d;
    return mask;
  }

  bool try_acquire(IoDomain d) {
    uint64_t word = __atomic_load_n(&slots_, __ATOMIC_SEQ_CST);
    for (;;) {
      if (!(free_domains(word) & domain_bit(d))) return false;
      if (__atomic_compare_exchange_n(&slots_, &word, word + one_slot(d), true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        const uint32_t now_in = static_cast<uint32_t>((word + one_slot(d)) & 0xFFFF);
        uint32_t peak = __atomic_load_n(&peak_, __ATOMIC_RELAXED);
        while (now_in > peak && !__atomic_compare_exchange_n(&peak_, &peak, now_in, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        return true;
      }
    }
  }
  void release(IoDomain d) { __atomic_fetch_sub(&slots_, one_slot(d), __ATOMIC_SEQ_CST); }

  // Gives free slots to queued requests, in the fair queue's order, and wakes their waiters (outside the lock, a batch at a time). True if
  // `self` (the caller's own ticket) was among them: its waiter is not woken, the caller goes on itself.
  bool dispatch(uint64_t now_us, uint32_t self) {
    bool self_granted = false;
    for (;;) {
      IoWaiter* batch[kGrantBatch];
      uint32_t n = 0;
      lock_.lock();
      while (n < kGrantBatch && fair_.queued() != 0) {
        const uint32_t mask = free_domains(__atomic_load_n(&slots_, __ATOMIC_SEQ_CST));
        if (!mask) break;
        const uint32_t t = fair_.pick(mask, now_us);
        if (t == FairQueue::kNone) break;
        if (!try_acquire(fair_.domain(t))) continue;   // a request on the lock-free path took the slot: look again
        fair_.commit(t, now_us);
        __atomic_fetch_sub(&queued_, 1, __ATOMIC_SEQ_CST);
        if (t == self) {
          self_granted = true;
        } else {
          batch[n++] = waiters_[t];
        }
        waiters_[t] = nullptr;
      }
      // Requests left waiting for their domain's slots tell the controller (it grows such a domain when latencies allow).
      const uint32_t mask = free_domains(__atomic_load_n(&slots_, __ATOMIC_RELAXED));
      for (uint32_t d = 0; d < kDomainCount; ++d)
        if (fair_.queued_in(static_cast<IoDomain>(d)) != 0 && !(mask & (1u << d))) controller_.note_throttled(static_cast<IoDomain>(d));
      lock_.unlock();
      for (uint32_t i = 0; i < n; ++i) batch[i]->grant();
      if (n < kGrantBatch) return self_granted;
    }
  }

  allocators::IAllocator& allocator_;
  IoSchedulerConfig config_;
  Lock lock_;                       // guards fair_ and waiters_
  FairQueue fair_;
  ArrayView<IoWaiter*> waiters_;    // per ticket: who waits for it (non-owning)
  LatencyController controller_;
  uint64_t slots_ = 0;              // atomic: bits 0-15 in flight in all, then 16 bits per domain
  uint32_t queued_ = 0;             // atomic: requests in the fair queue
  uint64_t bypassed_ = 0;           // atomic
  uint32_t peak_ = 0;               // atomic
};

}  // namespace iosched
