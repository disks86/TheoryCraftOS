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
#include "Allocators/IAllocator.hpp"
#include "BlockCache/BlockCache.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "Kernel/Sync.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// The block cache's kernel side (BlockCache/BlockCache.hpp, docs/BlockCache.md): kernel::Mutex and kernel::ConditionVariable, the time stamp
// counter as the clock. Before threads run there is nobody to wait for, so a wait just lets the lock go for a moment.
struct KernelCacheEnv {
  using Mutex = kernel::Mutex;
  class Condition {
   public:
    bool wait(kernel::Mutex& mutex, uint64_t timeout_us) {
      if (!kernel::running()) {
        mutex.unlock();
        mutex.lock();
        return false;
      }
      return cv_.wait(mutex, timeout_us);
    }
    void notify_all() {
      if (kernel::running()) cv_.notify_all();
    }

   private:
    kernel::ConditionVariable cv_;
  };
  static uint64_t now_us();
};

// A storage device with the block cache in front: reads are served from memory where it can, writes go to memory and reach the device in
// coalesced runs, written back by a thread of its own (or by the writer itself when it writes a lot). SystemStorageDevices puts one in front
// of the I/O scheduler of every disk (not of optical drives, whose medium changes under the same device).
//
// flush() writes back what was written before it and flushes the device, so file systems keep their ordering guarantees. eject() writes
// everything back first. A device that is gone (present() false) loses what was not written back yet, as it would have without the cache.
class CachedStorageDevice final : public IStorageDevice {
 public:
  explicit CachedStorageDevice(allocators::IAllocator& allocator) : allocator_(allocator), cache_(allocator) {}
  ~CachedStorageDevice();

  // The cache's size and how long dirty data may stay, from the device's kind: removable media get a smaller cache and a short expiry.
  static blockcache::BlockCacheConfig config_for(const StorageInfo& info);
  // Sets up the cache for `device` (initialized already) and takes it over. False, with `device` left as it was, if there was no memory or
  // the geometry is one the cache does not take.
  bool attach(UniquePointer<IStorageDevice>& device);
  bool attach(UniquePointer<IStorageDevice>& device, const blockcache::BlockCacheConfig& config);

  const char* name() const override { return inner_->name(); }
  bool initialize() override { return true; }   // (attach() takes a device that is up)
  void shutdown() override;
  // The device's facts (and the scheduler's), then the cache's.
  void describe(IPropertySink& sink) const override;
  bool present() const override { return inner_->present(); }
  bool can_eject() const override { return inner_->can_eject(); }
  bool eject() override;
  bool ejected() const override { return inner_->ejected(); }
  StorageInfo info() const override { return info_; }
  StorageStatistics statistics() const override { return inner_->statistics(); }

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  bool discard(uint64_t lba, uint64_t blocks) override;
  bool write_zeroes(uint64_t lba, uint64_t blocks) override;
  // Before the kernel stops its threads: the writeback thread ends and what is dirty goes to the device while its interrupts still work.
  void stop_interrupts() override;

  blockcache::BlockCacheStatistics cache_statistics() const { return cache_.statistics(); }
  IStorageDevice& device() { return *inner_; }

 private:
  // The device as the cache sees it.
  class Backend final : public blockcache::IBlockBackend {
   public:
    explicit Backend(CachedStorageDevice& owner) : owner_(owner) {}
    bool read(uint64_t lba, ByteView buffer) override { return owner_.inner_->read(lba, buffer); }
    bool write(uint64_t lba, ConstByteView buffer) override { return owner_.inner_->write(lba, buffer); }
    bool flush() override { return owner_.inner_->flush(); }
    bool discard(uint64_t lba, uint64_t blocks) override { return owner_.inner_->discard(lba, blocks); }
    bool write_zeroes(uint64_t lba, uint64_t blocks) override { return owner_.inner_->write_zeroes(lba, blocks); }

   private:
    CachedStorageDevice& owner_;
  };

  // Starts the writeback thread once threads run (the first write after that does it, if attach() came too early).
  void start_flusher();
  void stop_flusher();
  static void flusher_main(void* self);
  static void wake_flusher(void* self);
  // A device that is gone: what the cache holds is of no use any more.
  bool gone();

  allocators::IAllocator& allocator_;
  UniquePointer<IStorageDevice> inner_;
  StorageInfo info_;
  Backend backend_{*this};
  blockcache::BlockCache<KernelCacheEnv> cache_;
  kernel::Event wake_{true};
  kernel::Event stopped_;
  kernel::Thread* flusher_ = nullptr;
  uint32_t flusher_state_ = 0;   // (atomic) 0 none, 1 starting or running, 2 stopping
  bool dropped_ = false;
};

}  // namespace hardware
