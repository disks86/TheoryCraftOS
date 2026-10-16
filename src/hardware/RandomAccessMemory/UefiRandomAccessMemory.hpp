#pragma once
#include "hardware/RandomAccessMemory/IRandomAccessMemory.hpp"
#include "uefi.hpp"

namespace hardware {

// System memory discovered through UEFI boot services. initialize() reads the
// firmware memory map for the total/usable figures and reserves one contiguous
// pool (up to kPoolBytes, from the largest free region) for the OS allocator.
class UefiRandomAccessMemory final : public IRandomAccessMemory {
 public:
  static constexpr uint64_t kPoolBytes = 64ull * 1024 * 1024;

  explicit constexpr UefiRandomAccessMemory(efi::SystemTable* st) : st_(st) {}

  const char* name() const override { return "UEFI RAM"; }
  bool initialize() override;
  void shutdown() override;

  uint64_t total_bytes() const override { return total_bytes_; }
  uint64_t usable_bytes() const override { return pool_bytes_; }
  void* base() const override { return reinterpret_cast<void*>(pool_base_); }

  // Free (conventional) memory the firmware reported, claimed or not.
  uint64_t free_bytes() const { return free_bytes_; }

 private:
  static constexpr uint64_t kPageSize = 4096;

  efi::SystemTable* st_;
  uint64_t total_bytes_ = 0;
  uint64_t free_bytes_ = 0;
  uint64_t pool_base_ = 0;
  uint64_t pool_bytes_ = 0;
};

}  // namespace hardware
