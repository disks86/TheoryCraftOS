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

#include "IoScheduler/FairQueue.hpp"
#include "Utility/AllocatedView.hpp"

namespace iosched {

namespace {
constexpr uint64_t kMaxCost = uint64_t{1} << 50;   // (cost * kVirtualScale must not wrap)

uint32_t set_of(uint64_t issuer) { return static_cast<uint32_t>((issuer * 0x9E3779B97F4A7C15ull) >> 58); }   // 64 sets

uint64_t class_deadline(uint32_t base_us, IoClass c) {
  return c == IoClass::Realtime ? base_us / 4 : c == IoClass::Idle ? uint64_t{base_us} * 10 : base_us;
}
}  // namespace

bool FairQueue::initialize(uint32_t capacity, const FairQueueConfig& config) {
  release();
  if (capacity == 0 || capacity >= kNone) return false;
  config_ = config;
  if (config_.raise_factor == 0) config_.raise_factor = 1;
  if (config_.raise_budget == 0) config_.raise_budget = config_.cost.cost(IoOp::Read, 16ull << 20);
  slots_ = allocate_view<Slot>(allocator_, capacity);
  queues_ = allocate_view<Queue>(allocator_, kQueues);
  if (slots_.empty() || queues_.empty()) {
    release();
    return false;
  }
  for (Slot& s : slots_) s = Slot{};
  for (Queue& q : queues_) q = Queue{};
  for (uint32_t i = 0; i < capacity; ++i) slots_[i].next = i + 1 < capacity ? i + 1 : kNone;
  free_head_ = 0;
  for (uint32_t c = 0; c < kClassCount; ++c) {   // the shared queues, one per class
    Queue& q = queues_[kIssuerQueues + c];
    q.assigned = true;
    q.priority.io_class = static_cast<IoClass>(c);
  }
  for (uint64_t& w : active_) w = 0;
  for (uint32_t c = 0; c < kClassCount; ++c) vtime_[c] = weight_sum_[c] = queued_class_[c] = 0;
  for (uint32_t& n : queued_domain_) n = 0;
  queued_ = in_flight_ = 0;
  last_busy_us_ = 0;
  seen_busy_ = false;
  stats_ = FairQueueStatistics{};
  return true;
}

void FairQueue::release() {
  release_view(allocator_, slots_);
  release_view(allocator_, queues_);
  free_head_ = kNone;
}

uint32_t FairQueue::effective_weight(const Queue& q) const {
  return uint32_t{q.priority.weight} * (q.raised ? config_.raise_factor : 1u);
}

bool FairQueue::is_active(uint32_t index) const { return (active_[index / 64] >> (index % 64)) & 1; }

void FairQueue::set_active(uint32_t index, bool active) {
  Queue& q = queues_[index];
  const uint32_t c = static_cast<uint32_t>(q.priority.io_class);
  if (active == q.active) return;
  q.active = active;
  if (active) {
    active_[index / 64] |= uint64_t{1} << (index % 64);
    weight_sum_[c] += effective_weight(q);
  } else {
    active_[index / 64] &= ~(uint64_t{1} << (index % 64));
    weight_sum_[c] -= effective_weight(q);
  }
}

void FairQueue::raise(Queue& q, uint64_t now_us) {
  if (config_.raise_factor <= 1) return;   // raising is off
  if (q.active) weight_sum_[static_cast<uint32_t>(q.priority.io_class)] -= effective_weight(q);
  if (!q.raised) ++stats_.raised;
  q.raised = true;
  q.raise_until_us = now_us + config_.raise_duration_us;
  q.raise_cost = 0;
  if (q.active) weight_sum_[static_cast<uint32_t>(q.priority.io_class)] += effective_weight(q);
}

void FairQueue::unraise(Queue& q) {
  if (!q.raised) return;
  if (q.active) weight_sum_[static_cast<uint32_t>(q.priority.io_class)] -= effective_weight(q);
  q.raised = false;
  if (q.active) weight_sum_[static_cast<uint32_t>(q.priority.io_class)] += effective_weight(q);
}

// The issuer's queue: the one it already has in its set, else a way of the set that is unused or idle (the one idle longest), else the
// shared queue of its class.
uint32_t FairQueue::find_queue(uint64_t issuer, IoClass io_class, uint64_t now_us, bool* fresh) {
  *fresh = false;
  const uint32_t base = set_of(issuer) * kWays;
  uint32_t unused = kNone, idle = kNone;
  for (uint32_t w = 0; w < kWays; ++w) {
    const Queue& q = queues_[base + w];
    if (q.assigned && q.issuer == issuer) return base + w;
    if (!q.assigned) {
      if (unused == kNone) unused = base + w;
    } else if (q.queued == 0 && q.in_flight == 0 && (idle == kNone || q.idle_since_us < queues_[idle].idle_since_us)) {
      idle = base + w;
    }
  }
  const uint32_t victim = unused != kNone ? unused : idle;
  if (victim == kNone) {
    ++stats_.overflowed;
    return kIssuerQueues + static_cast<uint32_t>(io_class);
  }
  // A queue taken over starts afresh; its virtual finish is set when it becomes active (max with the class's virtual time).
  Queue& q = queues_[victim];
  q = Queue{};
  q.assigned = true;
  q.issuer = issuer;
  q.idle_since_us = now_us;
  q.finish = vtime_[static_cast<uint32_t>(io_class)];
  *fresh = true;
  return victim;
}

uint32_t FairQueue::enqueue(uint64_t issuer, IoPriority priority, IoOp op, uint64_t bytes, uint64_t now_us) {
  if (free_head_ == kNone) {
    ++stats_.pool_full;
    return kNone;
  }
  priority = clamp_priority(priority);
  bool fresh = false;
  const uint32_t qi = find_queue(issuer, priority.io_class, now_us, &fresh);
  Queue& q = queues_[qi];
  const IoDomain d = domain_of(op);
  uint64_t cost = config_.cost.cost(op, bytes);
  if (cost > kMaxCost) cost = kMaxCost;
  if (cost == 0) cost = 1;

  if (!q.active) {
    if (qi < kIssuerQueues) q.priority = priority;   // (a change of class or weight only while the queue is not in a weight sum)
    if (q.raised && now_us >= q.raise_until_us) unraise(q);
    if (q.in_flight == 0) {
      // A busy period ends when the queue has been idle for interactive_gap_us: a thread that reads, thinks briefly and reads again is
      // still in the same one. Coming back after a gap with a read and a short last busy period earns a raise.
      const bool long_idle = fresh || now_us - q.idle_since_us >= config_.interactive_gap_us;
      if (long_idle) {
        q.last_busy_cost = q.busy_cost;
        q.busy_cost = 0;
        if (op == IoOp::Read && q.priority.io_class == IoClass::BestEffort && q.last_busy_cost <= config_.raise_budget) raise(q, now_us);
      }
    }
    q.start = vtime_max(vtime_[static_cast<uint32_t>(q.priority.io_class)], q.finish);
    set_active(qi, true);
  }

  const uint32_t ticket = free_head_;
  Slot& s = slots_[ticket];
  free_head_ = s.next;
  s = Slot{};
  s.cost = cost;
  s.arrival_us = now_us;
  s.deadline_us = now_us + class_deadline(config_.cost.deadline_us[domain_index(d)], q.priority.io_class);
  s.queue = static_cast<uint16_t>(qi);
  s.domain = d;
  s.io_class = q.priority.io_class;
  s.state = SlotState::Queued;
  const uint32_t di = domain_index(d);
  if (q.tail[di] == kNone) {
    q.head[di] = ticket;
  } else {
    slots_[q.tail[di]].next = ticket;
  }
  q.tail[di] = ticket;
  ++q.queued;
  ++queued_class_[static_cast<uint32_t>(s.io_class)];
  ++queued_domain_[di];
  ++queued_;
  ++stats_.enqueued;
  return ticket;
}

uint32_t FairQueue::oldest_head(const Queue& q, uint32_t domains) const {
  uint32_t best = kNone;
  for (uint32_t d = 0; d < kDomainCount; ++d) {
    if (!(domains & (1u << d)) || q.head[d] == kNone) continue;
    if (best == kNone || slots_[q.head[d]].arrival_us < slots_[best].arrival_us) best = q.head[d];
  }
  return best;
}

uint32_t FairQueue::pick(uint32_t domains, uint64_t now_us) const {
  if (queued_ == 0 || slots_.empty()) return kNone;
  domains &= kAllDomains;

  // Deadlines first: the request whose deadline passed earliest, whatever its class.
  uint32_t expired = kNone;
  for (uint32_t word = 0; word < sizeof(active_) / sizeof(active_[0]); ++word) {
    for (uint64_t bits = active_[word]; bits; bits &= bits - 1) {
      const Queue& q = queues_[word * 64 + static_cast<uint32_t>(__builtin_ctzll(bits))];
      for (uint32_t d = 0; d < kDomainCount; ++d) {
        if (!(domains & (1u << d)) || q.head[d] == kNone) continue;
        const Slot& s = slots_[q.head[d]];
        if (s.deadline_us <= now_us && (expired == kNone || s.deadline_us < slots_[expired].deadline_us)) expired = q.head[d];
      }
    }
  }
  if (expired != kNone) return expired;

  for (uint32_t c = 0; c < kClassCount; ++c) {
    if (queued_class_[c] == 0) continue;
    if (c == static_cast<uint32_t>(IoClass::Idle)) {
      if (queued_class_[0] + queued_class_[1] > 0) break;              // strictly behind the others, even for other domains
      if (seen_busy_ && now_us - last_busy_us_ < config_.idle_grace_us) break;
    }
    // B-WF2Q+: of the queues whose virtual start has come (start <= V), the one whose finish is first; if none has, the one starting first.
    uint32_t eligible = kNone, earliest = kNone;
    uint64_t eligible_finish = 0, earliest_start = 0;
    for (uint32_t word = 0; word < sizeof(active_) / sizeof(active_[0]); ++word) {
      for (uint64_t bits = active_[word]; bits; bits &= bits - 1) {
        const Queue& q = queues_[word * 64 + static_cast<uint32_t>(__builtin_ctzll(bits))];
        if (static_cast<uint32_t>(q.priority.io_class) != c) continue;
        const uint32_t t = oldest_head(q, domains);
        if (t == kNone) continue;
        const uint64_t finish = q.start + slots_[t].cost * kVirtualScale / effective_weight(q);
        if (!vtime_before(vtime_[c], q.start)) {
          if (eligible == kNone || vtime_before(finish, eligible_finish)) {
            eligible = t;
            eligible_finish = finish;
          }
        } else if (earliest == kNone || vtime_before(q.start, earliest_start)) {
          earliest = t;
          earliest_start = q.start;
        }
      }
    }
    if (eligible != kNone) return eligible;
    if (earliest != kNone) return earliest;
    // Nothing of this class can go in the free domains: a lower class may use them (work conserving), except Idle (above).
  }
  return kNone;
}

void FairQueue::commit(uint32_t ticket, uint64_t now_us) {
  if (ticket >= slots_.size() || slots_[ticket].state != SlotState::Queued) return;
  Slot& s = slots_[ticket];
  const uint32_t qi = s.queue;
  Queue& q = queues_[qi];
  const uint32_t di = domain_index(s.domain);
  // Unlink it (pick() hands out heads, so this is normally the first step).
  uint32_t* link = &q.head[di];
  uint32_t previous = kNone;
  while (*link != ticket && *link != kNone) {
    previous = *link;
    link = &slots_[*link].next;
  }
  if (*link != ticket) return;
  *link = s.next;
  if (q.tail[di] == ticket) q.tail[di] = previous;
  s.next = kNone;

  const uint32_t c = static_cast<uint32_t>(q.priority.io_class);
  --q.queued;
  --queued_class_[c];
  --queued_domain_[di];
  --queued_;

  // Charge the queue and advance the class's virtual time by the service over the whole weight of the class.
  const uint64_t old_start = q.start;
  q.finish = q.start + s.cost * kVirtualScale / effective_weight(q);
  q.start = q.finish;
  if (weight_sum_[c]) vtime_[c] += s.cost * kVirtualScale / weight_sum_[c];
  vtime_[c] = vtime_max(vtime_[c], old_start);   // (a queue served before it was eligible: nothing else was; time moves on to it)
  q.busy_cost += s.cost;
  q.served_cost += s.cost;
  stats_.served_cost[c] += s.cost;
  if (q.raised) {
    q.raise_cost += s.cost;
    if (q.raise_cost > config_.raise_budget || now_us >= q.raise_until_us) unraise(q);
  }
  if (q.queued == 0) set_active(qi, false);

  s.expired = s.deadline_us <= now_us;
  s.state = SlotState::InFlight;
  s.dispatched_us = now_us;
  ++q.in_flight;
  ++in_flight_;
  if (c != static_cast<uint32_t>(IoClass::Idle)) {
    last_busy_us_ = now_us;
    seen_busy_ = true;
  }
  ++stats_.dispatched;
  if (s.expired) ++stats_.expired;
}

void FairQueue::complete(uint32_t ticket, uint64_t now_us) {
  if (ticket >= slots_.size() || slots_[ticket].state != SlotState::InFlight) return;
  Slot& s = slots_[ticket];
  Queue& q = queues_[s.queue];
  --q.in_flight;
  --in_flight_;
  if (q.in_flight == 0 && q.queued == 0) q.idle_since_us = now_us;
  if (s.io_class != IoClass::Idle) {
    last_busy_us_ = now_us;
    seen_busy_ = true;
  }
  s.state = SlotState::Free;
  s.next = free_head_;
  free_head_ = ticket;
  ++stats_.completed;
}

uint64_t FairQueue::idle_wait_until(uint64_t now_us) const {
  const uint32_t idle = static_cast<uint32_t>(IoClass::Idle);
  if (queued_class_[idle] == 0 || queued_class_[0] + queued_class_[1] > 0 || !seen_busy_) return 0;
  if (now_us - last_busy_us_ >= config_.idle_grace_us) return 0;
  return last_busy_us_ + config_.idle_grace_us;
}

IoDomain FairQueue::domain(uint32_t ticket) const { return ticket < slots_.size() ? slots_[ticket].domain : IoDomain::Read; }
uint64_t FairQueue::dispatched_us(uint32_t ticket) const { return ticket < slots_.size() ? slots_[ticket].dispatched_us : 0; }
bool FairQueue::expired_dispatch(uint32_t ticket) const { return ticket < slots_.size() && slots_[ticket].expired; }

bool FairQueue::issuer_state(uint64_t issuer, IssuerState* out) const {
  if (queues_.empty()) return false;
  const uint32_t base = set_of(issuer) * kWays;
  for (uint32_t w = 0; w < kWays; ++w) {
    const Queue& q = queues_[base + w];
    if (!q.assigned || q.issuer != issuer) continue;
    out->priority = q.priority;
    out->queued = q.queued;
    out->in_flight = q.in_flight;
    out->raised = q.raised;
    out->effective_weight = effective_weight(q);
    out->served_cost = q.served_cost;
    return true;
  }
  return false;
}

}  // namespace iosched
