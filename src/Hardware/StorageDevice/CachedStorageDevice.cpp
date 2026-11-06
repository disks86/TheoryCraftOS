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

#include "Hardware/StorageDevice/CachedStorageDevice.hpp"
#include "IoScheduler/IoTypes.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kFlusherStackBytes = 64 * 1024;
constexpr uint64_t kFlusherPeriodUs = 100000;   // how often the writeback thread looks for expired data without being woken
}  // namespace

uint64_t KernelCacheEnv::now_us() {
  const uint64_t per_us = tsc_ticks_per_us();
  return per_us ? read_tsc() / per_us : 0;
}

CachedStorageDevice::~CachedStorageDevice() { stop_flusher(); }

blockcache::BlockCacheConfig CachedStorageDevice::config_for(const StorageInfo& info) {
  blockcache::BlockCacheConfig config;
  config.block_size = info.block_size;
  config.block_count = info.block_count;
  config.page_bytes = info.block_size > 4096 ? info.block_size : 4096;
  if (info.removable || info.media == StorageMedia::Removable) {
    // A stick or card may be pulled out without warning: keep little dirty data, and not for long.
    config.capacity_bytes = 8ull << 20;
    config.expire_us = 250000;
    config.background_percent = 5;
  } else {
    config.capacity_bytes = 32ull << 20;
    config.expire_us = 2000000;
  }
  return config;
}

bool CachedStorageDevice::attach(UniquePointer<IStorageDevice>& device) {
  if (!device) return false;
  return attach(device, config_for(device->info()));
}

bool CachedStorageDevice::attach(UniquePointer<IStorageDevice>& device, const blockcache::BlockCacheConfig& config) {
  if (!device) return false;
  const StorageInfo info = device->info();
  if (info.kind == StorageKind::Optical || info.block_count == 0) return false;
  inner_ = util::move(device);
  if (!cache_.initialize(backend_, config)) {
    device = util::move(inner_);
    return false;
  }
  info_ = info;
  cache_.set_dirty_notifier(&CachedStorageDevice::wake_flusher, this);
  start_flusher();
  return true;
}

void CachedStorageDevice::start_flusher() {
  if (!kernel::running()) return;
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&flusher_state_, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
  flusher_ = kernel::spawn(&CachedStorageDevice::flusher_main, this, "block cache writeback", -1, -1, kFlusherStackBytes);
  if (!flusher_) __atomic_store_n(&flusher_state_, 0u, __ATOMIC_RELEASE);   // (writers then write back themselves; the next write tries again)
}

void CachedStorageDevice::stop_flusher() {
  uint32_t expected = 1;
  if (!__atomic_compare_exchange_n(&flusher_state_, &expected, 2u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
  wake_.set();
  if (flusher_ && kernel::running()) {   // (after kernel::stop the thread is gone with the others)
    stopped_.wait();
    kernel::join(flusher_, 0);
  }
  flusher_ = nullptr;
}

void CachedStorageDevice::wake_flusher(void* self) { static_cast<CachedStorageDevice*>(self)->wake_.set(); }

void CachedStorageDevice::flusher_main(void* argument) {
  CachedStorageDevice* self = static_cast<CachedStorageDevice*>(argument);
  // Writeback competes with readers by the fair queue below, at BestEffort with half the default weight: it gets the device's spare time
  // first, and a share when the device is busy.
  kernel::set_thread_io_priority(kernel::current_thread(), static_cast<uint8_t>(iosched::IoClass::BestEffort), iosched::kDefaultWeight / 2);
  while (__atomic_load_n(&self->flusher_state_, __ATOMIC_ACQUIRE) == 1) {
    self->wake_.wait(kFlusherPeriodUs);
    if (__atomic_load_n(&self->flusher_state_, __ATOMIC_ACQUIRE) != 1) break;
    if (self->gone()) continue;
    const uint64_t now = KernelCacheEnv::now_us();
    if (self->cache_.writeback_due(now)) self->cache_.write_back_due(now);
  }
  self->stopped_.set();
}

bool CachedStorageDevice::gone() {
  if (inner_->present()) return false;
  if (!__atomic_exchange_n(&dropped_, true, __ATOMIC_ACQ_REL)) cache_.drop_all();
  return true;
}

void CachedStorageDevice::stop_interrupts() {
  stop_flusher();
  if (!gone()) cache_.write_back_all();
  inner_->stop_interrupts();
}

void CachedStorageDevice::shutdown() {
  if (!gone()) cache_.flush();
  stop_flusher();
  inner_->shutdown();
}

bool CachedStorageDevice::eject() {
  // What the cache holds goes to the device first; the device's own eject then flushes its cache (or lets off one that cannot).
  if (!gone() && !cache_.write_back_all()) return false;
  if (!inner_->eject()) return false;
  stop_flusher();
  cache_.drop_all();
  return true;
}

bool CachedStorageDevice::read(uint64_t lba, ByteView buffer) {
  if (gone()) return false;
  return cache_.read(lba, buffer);
}

bool CachedStorageDevice::write(uint64_t lba, ConstByteView buffer) {
  if (info_.read_only || gone()) return false;
  if (__atomic_load_n(&flusher_state_, __ATOMIC_ACQUIRE) == 0) start_flusher();
  return cache_.write(lba, buffer);
}

bool CachedStorageDevice::flush() {
  if (gone()) return false;
  return cache_.flush();
}

bool CachedStorageDevice::discard(uint64_t lba, uint64_t blocks) {
  if (!info_.discard || info_.read_only || gone()) return false;
  return cache_.discard(lba, blocks);
}

bool CachedStorageDevice::write_zeroes(uint64_t lba, uint64_t blocks) {
  if (!info_.write_zeroes || info_.read_only || gone()) return false;
  return cache_.write_zeroes(lba, blocks);
}

void CachedStorageDevice::describe(IPropertySink& sink) const {
  inner_->describe(sink);
  const blockcache::BlockCacheStatistics s = cache_.statistics();
  const uint64_t page_kib = cache_.page_bytes() / 1024;
  sink.add_number("Cache size", uint64_t{s.capacity_pages} * page_kib / 1024, "MiB");
  sink.add_number("Cache shards", cache_.shard_count());
  sink.add_number("Cached", uint64_t{s.resident_pages} * page_kib, "KiB");
  sink.add_number("Dirty", uint64_t{s.dirty_pages} * page_kib, "KiB");
  sink.add_number("Cache hits (pages)", s.read_hits);
  sink.add_number("Cache misses (pages)", s.read_misses);
  const uint64_t looked = s.read_hits + s.read_misses;
  sink.add_number("Cache hit rate", looked ? s.read_hits * 100 / looked : 0, "%");
  sink.add_number("Pages used again (ARC T2)", s.t2_pages);
  sink.add_number("Writebacks", s.writebacks);
  sink.add_number("Average writeback", s.writebacks ? s.writeback_bytes / s.writebacks / 1024 : 0, "KiB");
  sink.add_number("Writebacks by busy writers", s.throttled);
  if (s.write_errors || s.read_errors) sink.add_number("Cache I/O errors", s.write_errors + s.read_errors);
}

}  // namespace hardware
