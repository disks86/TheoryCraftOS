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
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Jit/SpinFlag.hpp"
#include "Utility/ArrayView.hpp"

namespace jit {

constexpr uint64_t kCodePage = 4096;

// Where KernelCodeMemory gets the pages its regions are made of.
class ICodePageSource {
 public:
  // `pages` contiguous 4 KiB pages, 4 KiB aligned and mapped 1:1; null if there are none.
  virtual uint8_t* allocate_pages(uint64_t pages) = 0;
  virtual void release_pages(uint8_t* first, uint64_t pages) = 0;
  // True if the pages are ordinary heap memory that others will use after release (so they must go back to a writable, executable mapping); false if they
  // are reserved for code and rest as writable, not executable.
  virtual bool pages_are_ordinary_memory() const = 0;

 protected:
  constexpr ICodePageSource() = default;
  ~ICodePageSource() = default;
};

// Pages straight from the allocator, one allocation per region.
class AllocatorPageSource final : public ICodePageSource {
 public:
  AllocatorPageSource() = default;   // (a source with nothing behind it: allocate_pages() returns null)
  explicit AllocatorPageSource(allocators::IAllocator& allocator) : allocator_(&allocator) {}
  uint8_t* allocate_pages(uint64_t pages) override;
  void release_pages(uint8_t* first, uint64_t) override { if (allocator_) allocator_->deallocate(first); }
  bool pages_are_ordinary_memory() const override { return true; }

 private:
  allocators::IAllocator* allocator_ = nullptr;
};

// Code pages carved out of a few 2 MiB windows. A window is taken from the allocator the first time it is needed and stays reserved for code, so the
// page tables split exactly those 2 MiB into 4 KiB pages once, rather than every region splitting whatever firmware page it happens to land in, and
// the permission changes touch memory nothing else uses. Regions larger than a window are refused. Grows window by window up to `max_windows`.
class CodePool final : public ICodePageSource {
 public:
  static constexpr uint64_t kWindowBytes = 2ull << 20;
  static constexpr uint64_t kWindowPages = kWindowBytes / kCodePage;

  CodePool(allocators::IAllocator& allocator, uint32_t max_windows);
  ~CodePool();
  CodePool(const CodePool&) = delete;
  CodePool& operator=(const CodePool&) = delete;

  uint8_t* allocate_pages(uint64_t pages) override;
  void release_pages(uint8_t* first, uint64_t pages) override;
  bool pages_are_ordinary_memory() const override { return false; }

  uint32_t window_count() const { return windows_used_; }
  uint64_t free_pages() const;

 private:
  struct Window {
    uint8_t* base = nullptr;
    uint64_t used[kWindowPages / 64] = {};   // one bit per page
  };
  static bool test(const Window& w, uint64_t page) { return w.used[page / 64] >> (page % 64) & 1; }
  uint8_t* take(Window& w, uint64_t pages);

  allocators::IAllocator& allocator_;
  ArrayView<Window> windows_;
  uint32_t windows_used_ = 0;
  mutable SpinFlag lock_;
};

}  // namespace jit
