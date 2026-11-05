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

#include "CPUScheduler/IoAffinity.hpp"

namespace scheduling {

uint32_t placement_score(const PlacementHints& hints, uint32_t core, ArrayView<const CorePlace> cores) {
  CorePlace place;
  if (!cores.try_get(core, &place)) return 0;
  uint32_t score = 0;
  if (hints.preferred_kind >= 0 && hints.preferred_kind == place.kind) score += kScoreKind;
  if (hints.io_core == static_cast<int32_t>(core)) score += kScoreIoCore;
  if (hints.last_core == static_cast<int32_t>(core)) score += kScoreLastCore;
  // A shared last-level cache still has what the interrupt handler (or the thread itself, before) read, so it is worth something too.
  const int32_t anchor = hints.io_core != kNoCore ? hints.io_core : hints.last_core;
  CorePlace anchor_place;
  if (anchor >= 0 && cores.try_get(static_cast<uint64_t>(anchor), &anchor_place) && anchor_place.cache_domain == place.cache_domain)
    score += kScoreCacheDomain;
  return score;
}

int32_t choose_wake_core(const PlacementHints& hints, const CoreSet& allowed, ArrayView<const CorePlace> cores) {
  int32_t best = kNoCore;
  uint32_t best_score = 0;
  for (uint32_t i = 0; i < cores.size() && i < kMaxPlacementCores; ++i) {
    const CorePlace& place = cores[i];
    if (!place.online || !place.idle || !allowed.has(i)) continue;
    const uint32_t score = placement_score(hints, i, cores);
    if (best == kNoCore || score > best_score) {
      best = static_cast<int32_t>(i);
      best_score = score;
    }
  }
  return best;
}

int32_t choose_interrupt_core(ArrayView<const CorePlace> cores, ArrayView<const uint32_t> devices_per_core, const CoreSet& allowed, int8_t kind,
                              int32_t avoid) {
  // Passes, most wanted first: the kind and not `avoid`, the kind, any kind and not `avoid`, anything online.
  for (uint32_t pass = 0; pass < 4; ++pass) {
    const bool match_kind = pass < 2 && kind >= 0;
    const bool skip_avoid = pass % 2 == 0;
    int32_t best = kNoCore;
    uint32_t best_load = 0;
    for (uint32_t i = 0; i < cores.size() && i < kMaxPlacementCores; ++i) {
      const CorePlace& place = cores[i];
      if (!place.online || !allowed.has(i)) continue;
      if (match_kind && place.kind != kind) continue;
      if (skip_avoid && static_cast<int32_t>(i) == avoid) continue;
      uint32_t load = 0;
      devices_per_core.try_get(i, &load);
      if (best == kNoCore || load < best_load) {
        best = static_cast<int32_t>(i);
        best_load = load;
      }
    }
    if (best != kNoCore) return best;
  }
  return kNoCore;
}

uint32_t cache_domain_from_apic_id(uint32_t apic_id, uint32_t sharing) {
  uint32_t shift = 0;
  while (shift < 31 && (uint32_t{1} << shift) < sharing) ++shift;
  return apic_id >> shift;
}

// ---- IoDeviceTable

IoDeviceTable::Slot* IoDeviceTable::live_slot(int32_t id) {
  if (id < 0 || static_cast<uint32_t>(id) >= kCapacity) return nullptr;
  Slot& slot = slots_[id];
  return __atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) == kLive ? &slot : nullptr;
}

const IoDeviceTable::Slot* IoDeviceTable::live_slot(int32_t id) const {
  if (id < 0 || static_cast<uint32_t>(id) >= kCapacity) return nullptr;
  const Slot& slot = slots_[id];
  return __atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) == kLive ? &slot : nullptr;
}

bool IoDeviceTable::live(int32_t id) const { return live_slot(id) != nullptr; }

int32_t IoDeviceTable::add(StringView name, int32_t interrupt_core) {
  for (uint32_t i = 0; i < kCapacity; ++i) {
    Slot& slot = slots_[i];
    uint32_t expected = kFree;
    if (!__atomic_compare_exchange_n(&slot.state, &expected, kBusy, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
    const uint32_t sequence = __atomic_load_n(&slot.sequence, __ATOMIC_RELAXED);
    // Sequence lock without fences (which ThreadSanitizer does not model): the fields are stored with release, so a reader that sees a new
    // value also sees the odd count stored before it.
    __atomic_store_n(&slot.sequence, sequence + 1, __ATOMIC_RELAXED);
    uint64_t words[kNameWords] = {};
    const uint64_t length = name.size() < kNameWords * 8 - 1 ? name.size() : kNameWords * 8 - 1;
    for (uint64_t c = 0; c < length; ++c) words[c / 8] |= uint64_t{static_cast<uint8_t>(name[c])} << (c % 8 * 8);
    for (uint32_t w = 0; w < kNameWords; ++w) __atomic_store_n(&slot.name[w], words[w], __ATOMIC_RELEASE);
    __atomic_store_n(&slot.core, interrupt_core, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.interrupts, uint64_t{0}, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.wakes, uint64_t{0}, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.sequence, sequence + 2, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.state, static_cast<uint32_t>(kLive), __ATOMIC_RELEASE);
    return static_cast<int32_t>(i);
  }
  return kNoIoDevice;
}

bool IoDeviceTable::remove(int32_t id) {
  if (id < 0 || static_cast<uint32_t>(id) >= kCapacity) return false;
  Slot& slot = slots_[id];
  uint32_t expected = kLive;
  if (!__atomic_compare_exchange_n(&slot.state, &expected, kBusy, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return false;
  const uint32_t sequence = __atomic_load_n(&slot.sequence, __ATOMIC_RELAXED);
  __atomic_store_n(&slot.sequence, sequence + 1, __ATOMIC_RELAXED);
  for (uint64_t& word : slot.name) __atomic_store_n(&word, uint64_t{0}, __ATOMIC_RELEASE);
  __atomic_store_n(&slot.core, kNoCore, __ATOMIC_RELEASE);
  __atomic_store_n(&slot.sequence, sequence + 2, __ATOMIC_RELEASE);
  __atomic_store_n(&slot.state, static_cast<uint32_t>(kFree), __ATOMIC_RELEASE);
  return true;
}

bool IoDeviceTable::set_interrupt_core(int32_t id, int32_t core) {
  Slot* slot = live_slot(id);
  if (!slot) return false;
  __atomic_store_n(&slot->core, core, __ATOMIC_RELEASE);
  return true;
}

int32_t IoDeviceTable::interrupt_core(int32_t id) const {
  const Slot* slot = live_slot(id);
  return slot ? __atomic_load_n(&slot->core, __ATOMIC_ACQUIRE) : kNoCore;
}

// (A count that lands just as the slot is removed and taken again is credited to the new device: the counters are statistics.)
void IoDeviceTable::note_interrupt(int32_t id) {
  if (Slot* slot = live_slot(id)) __atomic_fetch_add(&slot->interrupts, 1, __ATOMIC_RELAXED);
}

void IoDeviceTable::note_wake(int32_t id) {
  if (Slot* slot = live_slot(id)) __atomic_fetch_add(&slot->wakes, 1, __ATOMIC_RELAXED);
}

bool IoDeviceTable::info(int32_t id, IoDeviceInfo* out) const {
  if (id < 0 || static_cast<uint32_t>(id) >= kCapacity) return false;
  const Slot& slot = slots_[id];
  // A writer that was interrupted half way would keep the count odd; give up after a while instead of waiting for it (this may run on its core).
  for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
    const uint32_t before = __atomic_load_n(&slot.sequence, __ATOMIC_ACQUIRE);
    if (before & 1) continue;
    if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) != kLive) return false;
    uint64_t words[kNameWords];
    // (acquire: the second read of the count cannot move above these)
    for (uint32_t w = 0; w < kNameWords; ++w) words[w] = __atomic_load_n(&slot.name[w], __ATOMIC_ACQUIRE);
    const int32_t core = __atomic_load_n(&slot.core, __ATOMIC_ACQUIRE);
    const uint64_t interrupts = __atomic_load_n(&slot.interrupts, __ATOMIC_ACQUIRE);
    const uint64_t wakes = __atomic_load_n(&slot.wakes, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&slot.sequence, __ATOMIC_RELAXED) != before) continue;
    for (uint32_t c = 0; c < sizeof out->name; ++c) out->name[c] = c / 8 < kNameWords ? static_cast<char>(words[c / 8] >> (c % 8 * 8)) : 0;
    out->name[sizeof out->name - 1] = 0;
    out->interrupt_core = core;
    out->interrupts = interrupts;
    out->wakes = wakes;
    return true;
  }
  return false;
}

void IoDeviceTable::devices_per_core(ArrayView<uint32_t> out) const {
  for (uint32_t& count : out) count = 0;
  for (const Slot& slot : slots_) {
    if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) != kLive) continue;
    const int32_t core = __atomic_load_n(&slot.core, __ATOMIC_ACQUIRE);
    if (core >= 0 && static_cast<uint64_t>(core) < out.size()) ++out[static_cast<uint64_t>(core)];
  }
}

}  // namespace scheduling
