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

#include "IoScheduler/IoTypes.hpp"

namespace iosched {

IoPriority priority_from_cpu_priority(uint8_t cpu_priority) {
  IoPriority p;
  if (cpu_priority <= 2) {
    p.io_class = IoClass::Idle;
  } else if (cpu_priority >= 28) {
    p.io_class = IoClass::Realtime;
  } else {
    p.weight = static_cast<uint16_t>(uint32_t{kDefaultWeight} * (cpu_priority + 4u) / 12u);   // 3 -> 58, 8 -> 100, 27 -> 258
  }
  return p;
}

IoPriority clamp_priority(IoPriority priority) {
  if (static_cast<uint8_t>(priority.io_class) >= kClassCount) priority.io_class = IoClass::BestEffort;
  if (priority.weight < kMinWeight) priority.weight = kMinWeight;
  if (priority.weight > kMaxWeight) priority.weight = kMaxWeight;
  return priority;
}

// The numbers are typical device-occupancy times, not latencies: an NVMe drive that does 500k random reads a second at depth spends about 2 us
// of its capacity on each. Only their ratios matter to the fair queue; the targets and deadlines are what the user feels.
IoCostModel IoCostModel::for_media(MediaKind kind) {
  IoCostModel m;
  auto set = [&m](uint32_t read_req, uint32_t write_req, uint32_t other_req, uint32_t read_page, uint32_t write_page,
                  uint32_t read_target, uint32_t write_target, uint32_t read_deadline, uint32_t write_deadline) {
    m.request_ns[0] = read_req;
    m.request_ns[1] = write_req;
    m.request_ns[2] = other_req;
    m.page_ns[0] = read_page;
    m.page_ns[1] = write_page;
    m.page_ns[2] = 0;
    m.target_us[0] = read_target;
    m.target_us[1] = write_target;
    m.target_us[2] = write_target * 4;     // a cache flush takes as long as the writes it waits for
    m.deadline_us[0] = read_deadline;
    m.deadline_us[1] = write_deadline;
    m.deadline_us[2] = write_deadline;
  };
  switch (kind) {
    case MediaKind::Nvme:       set(2000, 4000, 100000, 1300, 2000, 1000, 5000, 10000, 100000); break;
    case MediaKind::SolidState: set(10000, 15000, 1000000, 8000, 9000, 2000, 10000, 20000, 200000); break;
    case MediaKind::Rotational: set(8000000, 8000000, 20000000, 27000, 27000, 50000, 250000, 250000, 2500000); break;
    case MediaKind::Virtual:    set(10000, 15000, 1000000, 8000, 9000, 4000, 20000, 20000, 200000); break;
    case MediaKind::Removable:  set(200000, 1000000, 5000000, 100000, 400000, 20000, 200000, 100000, 1000000); break;
  }
  return m;
}

uint64_t IoCostModel::cost(IoOp op, uint64_t bytes) const {
  const uint32_t d = domain_index(domain_of(op));
  uint64_t pages = bytes / 4096 + (bytes % 4096 != 0);
  if (pages > 0xFFFFFFFFull) pages = 0xFFFFFFFFull;     // (so pages * page_ns cannot wrap)
  return uint64_t{request_ns[d]} + pages * page_ns[d];
}

}  // namespace iosched
