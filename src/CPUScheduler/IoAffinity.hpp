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

// Device-aware placement (I/O affinity) for the thread scheduler (Kernel/ThreadScheduler.cpp): which core a thread should run on, given where
// the interrupts of the device it waits for arrive, where it ran last (its cache is warm there) and which kind of core it likes. The data the
// interrupt handler just read from the device is in that core's cache, so the thread that consumes it is cheapest to run there, or failing that
// on a core sharing that core's last-level cache. Also picks the core a new device's interrupt goes to (spread over the cores) and keeps the
// table of devices that interrupts are attributed to.
//
// Nothing here takes a lock or touches hardware, so the policy can be tested on the host: the scheduler passes in a snapshot of its cores
// (CorePlace) taken under its own lock. IoDeviceTable is the exception: it is shared with interrupt handlers on every core, so its slots are
// claimed and read with atomics (a sequence count per slot) and need no lock.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"
#include "Utility/StringView.hpp"

namespace scheduling {

constexpr uint32_t kMaxPlacementCores = 128;
constexpr int32_t kNoCore = -1;
constexpr int32_t kNoIoDevice = -1;

// A set of cores (by index, up to kMaxPlacementCores).
struct CoreSet {
  uint64_t words[2] = {};
  static CoreSet all() { return CoreSet{{~uint64_t{0}, ~uint64_t{0}}}; }
  void add(uint32_t core) { if (core < kMaxPlacementCores) words[core / 64] |= uint64_t{1} << (core % 64); }
  bool has(uint32_t core) const { return core < kMaxPlacementCores && (words[core / 64] >> (core % 64) & 1); }
  bool empty() const { return !words[0] && !words[1]; }
};

// One core as placement sees it.
struct CorePlace {
  bool online = false;
  bool idle = false;            // running only its idle thread
  int8_t kind = 0;              // hardware::CoreKind
  uint32_t cache_domain = 0;    // cores that share the last-level cache have the same number
};

// What a thread brings to placement.
struct PlacementHints {
  int32_t io_core = kNoCore;     // the core the interrupts that wake it arrive on
  int32_t last_core = kNoCore;   // the core it ran on last
  int8_t preferred_kind = -1;    // the hardware::CoreKind it asked for, or -1
};

// The core a thread has learnt to follow: the one whose device interrupts keep waking it. One interrupt wake-up from another core does not move
// it (a device can raise the odd interrupt elsewhere while its MSI is being moved): the confidence has to run down first.
struct LearnedIoCore {
  static constexpr uint8_t kMaxConfidence = 3;
  int32_t core = kNoCore;
  uint8_t confidence = 0;

  void woken_by_interrupt_on(uint32_t on) {
    if (core == static_cast<int32_t>(on)) {
      if (confidence < kMaxConfidence) ++confidence;
    } else if (confidence > 1) {
      --confidence;
    } else {
      core = static_cast<int32_t>(on);
      confidence = 1;
    }
  }
  void forget() { *this = LearnedIoCore(); }
  int32_t hint() const { return confidence ? core : kNoCore; }
};

// Whether a device's interrupt should move to where the threads it wakes run. Each wake-up says where the woken thread will run if that is not
// the interrupt's core (pinned elsewhere, or wanting another kind of core), else kNoCore. Once a window of wake-ups is in, decide() names a core
// when most of them (three in four) had to run elsewhere and one core has a clear majority of those (a majority vote over the window).
struct InterruptSteering {
  static constexpr uint32_t kWindow = 32;
  int32_t candidate = kNoCore;
  uint32_t votes = 0;
  uint32_t seen = 0;
  uint32_t remote = 0;

  void note(int32_t consumer_core) {
    ++seen;
    if (consumer_core < 0) return;
    ++remote;
    if (votes == 0) {
      candidate = consumer_core;
      votes = 1;
    } else if (candidate == consumer_core) {
      ++votes;
    } else {
      --votes;
    }
  }
  bool window_full() const { return seen >= kWindow; }
  // The core to move to, or kNoCore; starts a new window either way.
  int32_t decide() {
    const int32_t to = remote * 4 >= seen * 3 && remote && votes * 2 >= remote ? candidate : kNoCore;
    *this = InterruptSteering();
    return to;
  }
};

// How much each match is worth when cores compete for a thread (or threads for a core). The kind the thread asked for outweighs everything
// else (it is an explicit request; the rest are guesses), then the device's own core, then the cache it left behind, then a shared cache.
constexpr uint32_t kScoreKind = 16, kScoreIoCore = 8, kScoreLastCore = 4, kScoreCacheDomain = 2;

// How well `core` suits a thread with these hints. 0 for a core out of range.
uint32_t placement_score(const PlacementHints& hints, uint32_t core, ArrayView<const CorePlace> cores);

// The idle core a thread that has just become ready should be started on: online, idle, in `allowed`, with the best placement_score (the lowest
// index on a tie). kNoCore when no allowed core is idle (the thread then waits for a core to give up its current thread).
int32_t choose_wake_core(const PlacementHints& hints, const CoreSet& allowed, ArrayView<const CorePlace> cores);

// The core a new device's interrupt should be sent to: an online core in `allowed`, of `kind` if one is online (-1: any kind), carrying the fewest
// device interrupts already (`devices_per_core[i]`, missing entries count as 0), not `avoid` unless no other core qualifies, the lowest index on a
// tie. kNoCore if no core is online.
int32_t choose_interrupt_core(ArrayView<const CorePlace> cores, ArrayView<const uint32_t> devices_per_core, const CoreSet& allowed, int8_t kind,
                              int32_t avoid);

// The last-level cache domain of an x86 core: CPUID leaf 4 (or 0x8000001D on AMD) says how many logical processors share a cache (`sharing`, the
// field plus one); they have the same APIC id once the bits that number them are dropped.
uint32_t cache_domain_from_apic_id(uint32_t apic_id, uint32_t sharing);

struct IoDeviceInfo {
  char name[24] = {};
  int32_t interrupt_core = kNoCore;
  uint64_t interrupts = 0;   // interrupts the device raised (that went through a handler set with set_device_interrupt_handler)
  uint64_t wakes = 0;        // threads those interrupts made ready
};

// The devices interrupts are attributed to. Any core may call any function at any time, interrupt handlers included: a slot is claimed with a
// compare-and-swap, its fields are atomic words, and a sequence count lets info() read a consistent copy of a slot another core is changing.
class IoDeviceTable {
 public:
  static constexpr uint32_t kCapacity = 64;

  IoDeviceTable() = default;
  IoDeviceTable(const IoDeviceTable&) = delete;
  IoDeviceTable& operator=(const IoDeviceTable&) = delete;

  // A free slot for a device whose interrupt arrives on `interrupt_core` (kNoCore: not routed yet). The name is cut to 23 characters.
  // kNoIoDevice when every slot is taken.
  int32_t add(StringView name, int32_t interrupt_core);
  // Frees the slot. False if `id` is not a live device.
  bool remove(int32_t id);
  bool set_interrupt_core(int32_t id, int32_t core);
  // kNoCore for an id that is not a live device.
  int32_t interrupt_core(int32_t id) const;
  void note_interrupt(int32_t id);
  void note_wake(int32_t id);
  bool info(int32_t id, IoDeviceInfo* out) const;
  // How many live devices interrupt core i, into out[i] (the rest of `out` is zeroed).
  void devices_per_core(ArrayView<uint32_t> out) const;
  bool live(int32_t id) const;

 private:
  enum : uint32_t { kFree = 0, kBusy = 1, kLive = 2 };
  static constexpr uint32_t kNameWords = 3;
  struct Slot {
    uint32_t state = kFree;
    uint32_t sequence = 0;            // odd while the slot is being set up or torn down
    uint64_t name[kNameWords] = {};   // the name, 8 characters a word, little end first
    int32_t core = kNoCore;
    uint64_t interrupts = 0;
    uint64_t wakes = 0;
  };
  Slot* live_slot(int32_t id);
  const Slot* live_slot(int32_t id) const;

  Slot slots_[kCapacity];
};

}  // namespace scheduling
