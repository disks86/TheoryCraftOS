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

#include "Hardware/StorageDevice/ScheduledStorageDevice.hpp"
#include "Kernel/Delay.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {

namespace {

iosched::MediaKind media_kind(const StorageInfo& info) {
  if (info.kind == StorageKind::Optical) return iosched::MediaKind::Rotational;
  switch (info.media) {
    case StorageMedia::Nvme: return iosched::MediaKind::Nvme;
    case StorageMedia::Rotational: return iosched::MediaKind::Rotational;
    case StorageMedia::Virtual: return iosched::MediaKind::Virtual;
    case StorageMedia::Removable: return iosched::MediaKind::Removable;
    case StorageMedia::SolidState:
    case StorageMedia::Unknown: break;
  }
  return iosched::MediaKind::SolidState;
}

// Who is asking, and at what priority: the calling thread's group (or the thread itself) and its I/O priority. Before threads run, and for
// code outside any thread, everything is one BestEffort issuer.
void caller(uint64_t* issuer, iosched::IoPriority* priority) {
  *issuer = 0;
  *priority = iosched::IoPriority{};
  kernel::Thread* t = kernel::current_thread();
  if (!t) return;
  const uint64_t group = __atomic_load_n(&t->io_group, __ATOMIC_ACQUIRE);
  *issuer = group ? group | (uint64_t{1} << 63) : t->id;   // (groups and thread ids cannot collide)
  const uint8_t io_class = __atomic_load_n(&t->io_class, __ATOMIC_ACQUIRE);
  if (io_class == kernel::kIoClassFromPriority) {
    *priority = iosched::priority_from_cpu_priority(__atomic_load_n(&t->priority, __ATOMIC_ACQUIRE));
  } else {
    const uint16_t weight = __atomic_load_n(&t->io_weight, __ATOMIC_RELAXED);
    *priority = iosched::clamp_priority(iosched::IoPriority{static_cast<iosched::IoClass>(io_class), weight ? weight : iosched::kDefaultWeight});
  }
}

// A thread waiting for its turn. grant() comes from the thread that finished before it (or submitted after it); Event::set holds the event's
// lock to its last access, and wait() takes the same lock, so the waiter cannot leave (and its event go away) while set() is still in it.
struct EventWaiter final : iosched::IoWaiter {
  kernel::Event event;
  void grant() override { event.set(); }
};

constexpr uint64_t kFullRetryUs = 100;

// The scheduler's clock: the time stamp counter in microseconds (0 until it is calibrated, which only stops deadlines and windows from moving).
uint64_t now_us() {
  const uint64_t per_us = tsc_ticks_per_us();
  return per_us ? read_tsc() / per_us : 0;
}   // every slot of the queue taken: try again this much later
}  // namespace

bool ScheduledStorageDevice::attach(UniquePointer<IStorageDevice>& device) {
  if (!device) return false;
  const StorageInfo info = device->info();
  iosched::IoSchedulerConfig config;
  config.depth = info.queue_depth ? info.queue_depth : 1;
  config.capacity = 256 + 4 * config.depth;
  config.fair.cost = iosched::IoCostModel::for_media(media_kind(info));
  if (!scheduler_.initialize(config, now_us())) return false;
  inner_ = util::move(device);
  return true;
}

template <class Transfer>
bool ScheduledStorageDevice::run(iosched::IoOp op, uint64_t bytes, Transfer&& transfer) {
  uint64_t issuer;
  iosched::IoPriority priority;
  caller(&issuer, &priority);
  EventWaiter waiter;
  iosched::IoTicket ticket;
  iosched::SubmitResult result;
  while ((result = scheduler_.submit(issuer, priority, op, bytes, waiter, &ticket, now_us())) == iosched::SubmitResult::Full)
    kernel::sleep_us(kFullRetryUs);
  if (result == iosched::SubmitResult::Queued) {
    // (A timed-out wait means the Idle class's grace time is over, or is the safety net: look again ourselves.)
    while (!waiter.event.wait(scheduler_.wait_hint_us(now_us()))) scheduler_.kick(now_us());
  }
  const bool ok = transfer();
  scheduler_.complete(ticket, now_us());
  return ok;
}

void ScheduledStorageDevice::describe(IPropertySink& sink) const {
  inner_->describe(sink);
  const iosched::IoSchedulerStatistics s = scheduler_.statistics();
  sink.add_number("I/O scheduler depth", scheduler_.depth());
  sink.add_number("Requests queued", s.fair.enqueued);
  sink.add_number("Requests straight to the device", s.bypassed);
  sink.add_number("Peak in flight", s.peak_in_flight);
  sink.add_number("Read slots", s.depth.limit[0]);
  sink.add_number("Write slots", s.depth.limit[1]);
}

bool ScheduledStorageDevice::read(uint64_t lba, ByteView buffer) {
  return run(iosched::IoOp::Read, buffer.size(), [&] { return inner_->read(lba, buffer); });
}

bool ScheduledStorageDevice::write(uint64_t lba, ConstByteView buffer) {
  return run(iosched::IoOp::Write, buffer.size(), [&] { return inner_->write(lba, buffer); });
}

bool ScheduledStorageDevice::flush() {
  return run(iosched::IoOp::Flush, 0, [&] { return inner_->flush(); });
}

}  // namespace hardware
