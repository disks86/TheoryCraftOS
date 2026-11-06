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

// The words the I/O scheduler (IoScheduler.hpp, docs/IoScheduling.md) is described in: the I/O classes and weights a thread asks for, the
// three token domains requests are counted in, the kind of medium a device is, and the cost model that turns a request into the device time
// it takes. Nothing here touches hardware or takes a lock.
#pragma once
#include <stdint.h>

namespace iosched {

// Strict order between classes: a Realtime request goes before any BestEffort one, and an Idle request only when nothing else has used the
// device for a moment (FairQueueConfig::idle_grace_us). Deadlines keep a lower class from starving for good. Within a class the device time
// is shared out by weight.
enum class IoClass : uint8_t { Realtime = 0, BestEffort = 1, Idle = 2 };
constexpr uint32_t kClassCount = 3;

enum class IoOp : uint8_t { Read, Write, Flush };

// The depth controller hands out device slots per domain, so writes cannot fill the device's queue while reads wait behind them.
enum class IoDomain : uint8_t { Read = 0, Write = 1, Other = 2 };
constexpr uint32_t kDomainCount = 3;
constexpr uint32_t kAllDomains = (1u << kDomainCount) - 1;
constexpr uint32_t domain_index(IoDomain d) { return static_cast<uint32_t>(d); }
constexpr uint32_t domain_bit(IoDomain d) { return 1u << domain_index(d); }
constexpr IoDomain domain_of(IoOp op) {
  return op == IoOp::Read ? IoDomain::Read : op == IoOp::Write ? IoDomain::Write : IoDomain::Other;
}

constexpr uint16_t kMinWeight = 1, kMaxWeight = 1000, kDefaultWeight = 100;

struct IoPriority {
  IoClass io_class = IoClass::BestEffort;
  uint16_t weight = kDefaultWeight;   // kMinWeight..kMaxWeight; a queue with twice the weight gets twice the device time
};

// The I/O priority of a thread that set none, from its CPU priority (0..31, default 8), the way Linux derives one from the nice value:
// 0-2 Idle, 3-27 BestEffort with the weight growing with the priority (8 gives kDefaultWeight), 28-31 Realtime.
IoPriority priority_from_cpu_priority(uint8_t cpu_priority);
// Clamps the weight into kMinWeight..kMaxWeight and an unknown class to BestEffort.
IoPriority clamp_priority(IoPriority priority);

enum class MediaKind : uint8_t {
  Nvme,        // flash behind NVMe: deep queues, tens of microseconds
  SolidState,  // a SATA/SAS SSD
  Rotational,  // a spinning disk or an optical drive: seeks dominate
  Virtual,     // a hypervisor's disk (virtio, pvscsi): what is behind it is unknown, latency is jittery
  Removable,   // a USB stick or SD card: slow writes, small caches
};

// What a request costs, in nanoseconds of device time (how long it keeps the device from doing something else), plus the latency targets the
// depth controller keeps each domain under and how long a request may wait in the queue. Costs, not sectors, are what the fair queue shares
// out: on flash a 4 KiB random read and a 4 KiB sequential one cost the same, and a write costs more than a read.
struct IoCostModel {
  uint32_t request_ns[kDomainCount] = {};  // per command, whatever its size
  uint32_t page_ns[kDomainCount] = {};     // per 4 KiB moved
  uint32_t target_us[kDomainCount] = {};   // the completion latency (90th percentile) each domain should stay under
  uint32_t deadline_us[kDomainCount] = {}; // the longest a BestEffort request waits in the queue (Realtime a quarter of it, Idle ten times)

  static IoCostModel for_media(MediaKind kind);
  // request_ns + page_ns per started 4 KiB page; saturates instead of wrapping for absurd sizes.
  uint64_t cost(IoOp op, uint64_t bytes) const;
};

// Wrap-safe comparisons of virtual times (they are allowed to wrap around 2^64).
constexpr bool vtime_before(uint64_t a, uint64_t b) { return static_cast<int64_t>(a - b) < 0; }
constexpr uint64_t vtime_max(uint64_t a, uint64_t b) { return vtime_before(a, b) ? b : a; }

}  // namespace iosched
