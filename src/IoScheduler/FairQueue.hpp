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

// The fair queue of the I/O scheduler: which waiting request goes to the device next. Shaped after BFQ, changed for flash and deep queues:
//
// - Every issuer (a thread, or the threads of one process) has its own queue, and the device time is shared out between the queues of a class
//   by weight with B-WF2Q+ (each queue has a virtual start and finish time; the eligible queue that finishes first goes). Service is counted in
//   cost (IoCostModel: device nanoseconds), not sectors, and request by request rather than in budgets: there is no idling to wait for a
//   queue's next request, so several queues keep the device busy at once.
// - Classes are strict (Realtime, then BestEffort, then Idle, which waits for idle_grace_us of quiet), but every request has a deadline and one
//   that has passed goes first, earliest deadline first: nothing waits for ever.
// - Weight raising (BFQ's low-latency heuristic): a queue that has been idle for a while and comes back with a read gets raise_factor times its
//   weight for raise_duration_us or until it has had raise_budget of service. A queue whose busy periods are short keeps earning it (an
//   interactive program, a player reading ahead); one that streams for long does not.
// - Within a queue requests go in arrival order per domain; the caller says which domains have device slots free (the depth controller), and
//   only those are considered.
//
// Nothing here locks: the owner (IoScheduler) holds its lock around every call. Memory is allocated once in initialize() - a pool of request
// slots and a fixed set of issuer queues - so the I/O path never allocates.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "IoScheduler/IoTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace iosched {

struct FairQueueConfig {
  IoCostModel cost = IoCostModel::for_media(MediaKind::SolidState);
  uint32_t interactive_gap_us = 20000;   // idle at least this long, then a read: the queue is raised
  uint32_t raise_duration_us = 200000;
  uint32_t raise_factor = 10;
  uint64_t raise_budget = 0;             // service a raised queue may have before it loses the raise; 0: the cost of reading 16 MiB
  uint32_t idle_grace_us = 2000;         // the Idle class waits this long after the other classes last used the device
};

struct FairQueueStatistics {
  uint64_t enqueued = 0, dispatched = 0, completed = 0;
  uint64_t expired = 0;          // dispatched because their deadline had passed
  uint64_t raised = 0;           // times a queue was raised
  uint64_t overflowed = 0;       // requests that went to a shared queue because their issuer's set of queues was full
  uint64_t pool_full = 0;        // enqueue() calls refused
  uint64_t served_cost[kClassCount] = {};
};

// What a test (or a statistics view) can see of one issuer's queue.
struct IssuerState {
  IoPriority priority;
  uint32_t queued = 0, in_flight = 0;
  bool raised = false;
  uint32_t effective_weight = 0;
  uint64_t served_cost = 0;      // total, since the queue was given to this issuer
};

class FairQueue {
 public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  static constexpr uint32_t kSets = 64, kWays = 4;
  static constexpr uint32_t kIssuerQueues = kSets * kWays;
  static constexpr uint32_t kQueues = kIssuerQueues + kClassCount;   // plus one shared queue per class for issuers whose set is full
  static constexpr uint64_t kVirtualScale = 1024;                    // virtual time per unit of cost at weight 1, so small costs over big weights still count

  explicit FairQueue(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~FairQueue() { release(); }
  FairQueue(const FairQueue&) = delete;
  FairQueue& operator=(const FairQueue&) = delete;

  // Room for `capacity` requests queued or in flight at once. False if out of memory (or capacity is 0).
  bool initialize(uint32_t capacity, const FairQueueConfig& config);
  void release();
  bool ready() const { return !slots_.empty(); }

  // Queues a request of `issuer` (any number that tells issuers apart; 0 is fine). Returns its ticket, or kNone if every slot is in use. A
  // priority given while the issuer still has requests queued takes effect once they are gone.
  uint32_t enqueue(uint64_t issuer, IoPriority priority, IoOp op, uint64_t bytes, uint64_t now_us);
  // The request that should go next among the domains whose bit is set in `domains`, or kNone. Does not change anything: commit() does.
  uint32_t pick(uint32_t domains, uint64_t now_us) const;
  // Takes the picked request out of its queue and charges it; from now on it is in flight until complete().
  void commit(uint32_t ticket, uint64_t now_us);
  // The request in flight is done; its ticket is free again.
  void complete(uint32_t ticket, uint64_t now_us);

  uint32_t queued() const { return queued_; }
  uint32_t queued_in(IoDomain d) const { return queued_domain_[domain_index(d)]; }
  uint32_t in_flight() const { return in_flight_; }
  // When requests are queued but pick() gives none because only the Idle class waits for its grace time: when it may go. 0 otherwise.
  uint64_t idle_wait_until(uint64_t now_us) const;

  // A ticket's request (kNone/out of range: defaults).
  IoDomain domain(uint32_t ticket) const;
  uint64_t dispatched_us(uint32_t ticket) const;
  bool expired_dispatch(uint32_t ticket) const;

  const FairQueueStatistics& statistics() const { return stats_; }
  bool issuer_state(uint64_t issuer, IssuerState* out) const;

 private:
  enum class SlotState : uint8_t { Free, Queued, InFlight };
  struct Slot {
    uint64_t cost = 0;
    uint64_t arrival_us = 0;
    uint64_t deadline_us = 0;
    uint64_t dispatched_us = 0;
    uint32_t next = kNone;          // the next request in its queue's list for its domain, or the next free slot
    uint16_t queue = 0;
    IoDomain domain = IoDomain::Read;
    IoClass io_class = IoClass::BestEffort;
    SlotState state = SlotState::Free;
    bool expired = false;
  };
  struct Queue {
    uint64_t issuer = 0;
    bool assigned = false;
    bool active = false;            // has requests queued (and counts in its class's weight sum)
    bool raised = false;
    IoPriority priority;
    uint32_t head[kDomainCount] = {kNone, kNone, kNone};
    uint32_t tail[kDomainCount] = {kNone, kNone, kNone};
    uint32_t queued = 0, in_flight = 0;
    uint64_t start = 0, finish = 0; // virtual times
    uint64_t idle_since_us = 0;     // when it last had nothing queued or in flight
    uint64_t busy_cost = 0;         // service in the current busy period, and in the one before
    uint64_t last_busy_cost = 0;
    uint64_t raise_until_us = 0;
    uint64_t raise_cost = 0;
    uint64_t served_cost = 0;
  };
  struct Candidate {
    uint32_t ticket = kNone;
    uint32_t queue = kNone;
  };

  uint32_t find_queue(uint64_t issuer, IoClass io_class, uint64_t now_us, bool* fresh);
  uint32_t effective_weight(const Queue& q) const;
  uint32_t oldest_head(const Queue& q, uint32_t domains) const;
  void raise(Queue& q, uint64_t now_us);
  void unraise(Queue& q);
  void set_active(uint32_t index, bool active);
  bool is_active(uint32_t index) const;

  allocators::IAllocator& allocator_;
  FairQueueConfig config_;
  ArrayView<Slot> slots_;
  ArrayView<Queue> queues_;          // kQueues
  uint32_t free_head_ = kNone;
  uint64_t active_[(kQueues + 63) / 64] = {};
  uint64_t vtime_[kClassCount] = {};
  uint64_t weight_sum_[kClassCount] = {};
  uint32_t queued_class_[kClassCount] = {};
  uint32_t queued_domain_[kDomainCount] = {};
  uint32_t queued_ = 0, in_flight_ = 0;
  uint64_t last_busy_us_ = 0;        // the last dispatch or completion of a Realtime or BestEffort request
  bool seen_busy_ = false;
  FairQueueStatistics stats_;
};

}  // namespace iosched
