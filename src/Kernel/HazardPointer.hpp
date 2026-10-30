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
#include "Kernel/Interrupts.hpp"

// Hazard pointers (Michael, 2004): safe memory reclamation for lock-free data structures.
//
// A reader that is about to dereference a shared pointer first publishes it in a hazard slot
// (HazardGuard::protect). A writer that has unlinked an object does not free it right away but
// retires it; the object is only reclaimed once no slot holds its address, so a reader can never
// touch freed memory. Readers never block, and a stalled reader (preempted thread, or a core stuck
// in an interrupt handler) holds back only the objects it protects, not all reclamation.
//
// Kernel properties:
//  - No locks, no interrupt masking, no allocation, no thread-local storage: usable from threads,
//    interrupt handlers and early boot alike. A guard takes a slot with one atomic exchange, so an
//    interrupt handler that interrupts a thread mid-protect simply gets another slot.
//  - Retired objects are chained through a HazardNode that lives inside the object, so retiring
//    cannot fail for lack of memory.
//  - Slots are fixed in number. Keep guards short-lived; Slots must be at least the most guards
//    that can be live at the same time (threads or cores times guards each, plus handlers).
//    HazardGuard's constructor spins until a slot is free; try_acquire() fails instead.
//
// Usage:
//   struct Entry { HazardNode node; Entry* next; ... };
//   HazardDomain<> domain;                       // one per data structure (or shared)
//   Entry* volatile head;
//   // reader
//   HazardGuard guard(domain);
//   Entry* e = guard.protect(&head);             // safe to dereference until guard dies/reset()
//   // writer, after unlinking e so that no new reader can find it
//   domain.retire(e, e->node, [](void* p) { free_entry(static_cast<Entry*>(p)); });
//
// A pointer protected from one location stays protected only while it is reachable from there:
// to walk a list, protect the next node from the (still protected) current node's link, then
// re-validate as the structure requires. Use protect() on a link only inside an object a guard
// already covers (or a root). Retire an object only once, and only after it is unreachable.
namespace kernel {

// Embedded in every object that will be retired (anywhere in it: reclaim gets the object pointer).
struct HazardNode {
  void* object = nullptr;
  void (*reclaim)(void* object) = nullptr;
  HazardNode* next = nullptr;
};

template <uint32_t Slots = 32>
class HazardDomain {
  static_assert(Slots > 0 && Slots <= 4096, "hazard slot count out of range");

 public:
  HazardDomain() = default;
  HazardDomain(const HazardDomain&) = delete;
  HazardDomain& operator=(const HazardDomain&) = delete;
  // Reclaims what is left. No guard may be live and no other thread may use the domain.
  ~HazardDomain() { drain(); }

  static constexpr uint32_t slot_count() { return Slots; }

  // Takes a free slot, or returns kNoSlot.
  static constexpr uint32_t kNoSlot = ~uint32_t{0};
  uint32_t acquire_slot() {
    const uint32_t start = __atomic_fetch_add(&cursor_, 1, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < Slots; ++i) {
      Slot& slot = slots_[(start + i) % Slots];
      if (__atomic_load_n(&slot.in_use, __ATOMIC_RELAXED)) continue;
      if (!__atomic_exchange_n(&slot.in_use, 1, __ATOMIC_ACQUIRE)) return (start + i) % Slots;
    }
    return kNoSlot;
  }
  void release_slot(uint32_t index) {
    Slot& slot = slots_[index % Slots];
    __atomic_store_n(&slot.hazard, nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.in_use, 0, __ATOMIC_RELEASE);
  }
  // Publishes `pointer` (sequentially consistent: the store must be visible before the caller re-reads the source).
  void publish(uint32_t index, const void* pointer) { __atomic_store_n(&slots_[index % Slots].hazard, pointer, __ATOMIC_SEQ_CST); }

  // Hands `object` over for reclamation once nothing protects it. `node` must be storage inside (or that lives as long as) the
  // object and unused by anything else; reclaim(object) runs exactly once, possibly on another core or in an interrupt handler.
  void retire(void* object, HazardNode& node, void (*reclaim)(void*)) {
    node.object = object;
    node.reclaim = reclaim;
    push(&node, &node, 1);
    if (__atomic_load_n(&retired_, __ATOMIC_RELAXED) >= 2 * Slots && !__atomic_exchange_n(&auto_scanning_, 1, __ATOMIC_ACQUIRE)) {
      scan();
      __atomic_store_n(&auto_scanning_, 0, __ATOMIC_RELEASE);
    }
  }
  template <typename T>
  void retire(T* object, HazardNode& node, void (*reclaim)(void*)) { retire(static_cast<void*>(object), node, reclaim); }

  // Reclaims every retired object that no slot protects; the rest stay retired. Returns how many were reclaimed.
  // Safe to call from several cores at once (each takes the whole retired list while it works).
  uint32_t scan() {
    HazardNode* list = __atomic_exchange_n(&retired_head_, static_cast<HazardNode*>(nullptr), __ATOMIC_ACQUIRE);
    if (!list) return 0;
    // Snapshot the hazards after taking the list: an object on it was unlinked before it was retired, so a reader that protects it
    // later re-validates and fails, and a reader that protected it earlier is seen here (both sides are sequentially consistent).
    const void* snapshot[Slots];
    uint32_t held = 0;
    for (Slot& slot : slots_) {
      const void* hazard = __atomic_load_n(&slot.hazard, __ATOMIC_SEQ_CST);
      if (hazard) snapshot[held++] = hazard;
    }
    HazardNode* keep_head = nullptr;
    HazardNode* keep_tail = nullptr;
    uint32_t freed = 0;
    while (list) {
      HazardNode* node = list;
      list = list->next;                     // (read before reclaim: the node may live inside the object)
      bool protected_now = false;
      for (uint32_t i = 0; i < held; ++i) {
        if (snapshot[i] == node->object) { protected_now = true; break; }
      }
      if (protected_now) {
        node->next = keep_head;
        if (!keep_head) keep_tail = node;
        keep_head = node;
      } else {
        void (*reclaim)(void*) = node->reclaim;
        void* object = node->object;
        reclaim(object);
        ++freed;
      }
    }
    if (freed) __atomic_fetch_sub(&retired_, freed, __ATOMIC_RELAXED);
    if (keep_head) push_list(keep_head, keep_tail);   // (still counted in retired_)
    return freed;
  }

  // Reclaims everything, spinning while a slot still protects something. Only for teardown, when no guard can stay live.
  void drain() {
    while (__atomic_load_n(&retired_, __ATOMIC_ACQUIRE) != 0) {
      scan();
      cpu_relax();
    }
  }

  // Retired objects not reclaimed yet (approximate while other cores run).
  uint32_t pending() const { return __atomic_load_n(&retired_, __ATOMIC_RELAXED); }

 private:
  struct alignas(64) Slot {   // (a line of its own: readers on different cores must not share one)
    const void* hazard = nullptr;
    uint32_t in_use = 0;
  };

  void push(HazardNode* head, HazardNode* tail, uint32_t count) {
    __atomic_fetch_add(&retired_, count, __ATOMIC_RELAXED);
    push_list(head, tail);
  }
  // Lock-free push of the chain head..tail onto the retired stack.
  void push_list(HazardNode* head, HazardNode* tail) {
    HazardNode* old = __atomic_load_n(&retired_head_, __ATOMIC_RELAXED);
    do {
      tail->next = old;
    } while (!__atomic_compare_exchange_n(&retired_head_, &old, head, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  }

  Slot slots_[Slots];
  uint32_t cursor_ = 0;
  uint32_t auto_scanning_ = 0;
  uint32_t retired_ = 0;
  HazardNode* retired_head_ = nullptr;
};

// Owns one hazard slot of a domain for its lifetime.
template <typename Domain>
class BasicHazardGuard {
 public:
  // Waits for a slot (spins): see the note on Slots above.
  explicit BasicHazardGuard(Domain& domain) : domain_(domain) {
    while ((slot_ = domain_.acquire_slot()) == Domain::kNoSlot) cpu_relax();
  }
  struct NoWait {};
  // Does not wait: check valid().
  BasicHazardGuard(Domain& domain, NoWait) : domain_(domain), slot_(domain.acquire_slot()) {}
  ~BasicHazardGuard() {
    if (slot_ != Domain::kNoSlot) domain_.release_slot(slot_);
  }
  BasicHazardGuard(const BasicHazardGuard&) = delete;
  BasicHazardGuard& operator=(const BasicHazardGuard&) = delete;

  bool valid() const { return slot_ != Domain::kNoSlot; }

  // Loads *source and protects what it points to: when this returns, the object is either still what
  // *source holds or has been retired but cannot be reclaimed before reset()/destruction. Returns null
  // (and protects nothing) for a null pointer or an invalid guard.
  template <typename T>
  T* protect(T* const* source) {
    if (!valid()) return nullptr;
    T* pointer = __atomic_load_n(source, __ATOMIC_SEQ_CST);
    for (;;) {
      domain_.publish(slot_, pointer);
      T* again = __atomic_load_n(source, __ATOMIC_SEQ_CST);
      if (again == pointer) return pointer;
      pointer = again;
    }
  }
  // Protects a pointer the caller already holds safely (for example one it just read from an object another guard covers),
  // which the caller must re-validate afterwards.
  void set(const void* pointer) {
    if (valid()) domain_.publish(slot_, pointer);
  }
  // Stops protecting (keeps the slot for the next protect()).
  void reset() { set(nullptr); }

 private:
  Domain& domain_;
  uint32_t slot_ = Domain::kNoSlot;
};

template <uint32_t Slots>
using HazardGuardFor = BasicHazardGuard<HazardDomain<Slots>>;
using HazardGuard = BasicHazardGuard<HazardDomain<32>>;

}  // namespace kernel
