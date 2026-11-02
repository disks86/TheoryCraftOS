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
#include "Jit/CodePageSource.hpp"
#include "Utility/AllocatedView.hpp"

namespace jit {

uint8_t* AllocatorPageSource::allocate_pages(uint64_t pages) {
  if (!allocator_ || pages == 0 || pages > UINT64_MAX / kCodePage) return nullptr;
  return static_cast<uint8_t*>(allocator_->allocate_aligned(pages * kCodePage, kCodePage));
}

CodePool::CodePool(allocators::IAllocator& allocator, uint32_t max_windows) : allocator_(allocator) {
  if (max_windows == 0) return;
  windows_ = allocate_view<Window>(allocator_, max_windows);
  for (Window& w : windows_) w = Window();
}

CodePool::~CodePool() {
  for (uint32_t i = 0; i < windows_used_; ++i) allocator_.deallocate(windows_[i].base);
  release_view(allocator_, windows_);
}

uint8_t* CodePool::take(Window& w, uint64_t pages) {
  // First fit: the first run of `pages` clear bits.
  uint64_t run = 0;
  for (uint64_t page = 0; page < kWindowPages; ++page) {
    run = test(w, page) ? 0 : run + 1;
    if (run == pages) {
      const uint64_t first = page + 1 - pages;
      for (uint64_t p = first; p <= page; ++p) w.used[p / 64] |= uint64_t{1} << (p % 64);
      return w.base + first * kCodePage;
    }
  }
  return nullptr;
}

uint8_t* CodePool::allocate_pages(uint64_t pages) {
  if (pages == 0 || pages > kWindowPages) return nullptr;
  SpinFlagGuard guard(lock_);
  for (uint32_t i = 0; i < windows_used_; ++i) {
    if (uint8_t* p = take(windows_[i], pages)) return p;
  }
  if (windows_used_ >= windows_.size()) return nullptr;
  auto* base = static_cast<uint8_t*>(allocator_.allocate_aligned(kWindowBytes, kWindowBytes));
  if (!base) return nullptr;
  Window& w = windows_[windows_used_];
  w = Window();
  w.base = base;
  ++windows_used_;
  return take(w, pages);
}

void CodePool::release_pages(uint8_t* first, uint64_t pages) {
  SpinFlagGuard guard(lock_);
  for (uint32_t i = 0; i < windows_used_; ++i) {
    Window& w = windows_[i];
    if (first < w.base || static_cast<uint64_t>(first - w.base) >= kWindowBytes) continue;
    const uint64_t start = static_cast<uint64_t>(first - w.base) / kCodePage;
    if (pages > kWindowPages - start) return;   // (not a run this pool handed out)
    for (uint64_t p = start; p < start + pages; ++p) w.used[p / 64] &= ~(uint64_t{1} << (p % 64));
    return;
  }
}

uint64_t CodePool::free_pages() const {
  SpinFlagGuard guard(lock_);
  uint64_t total = (windows_.size() - windows_used_) * kWindowPages;
  for (uint32_t i = 0; i < windows_used_; ++i) {
    for (uint64_t p = 0; p < kWindowPages; ++p) total += !test(windows_[i], p);
  }
  return total;
}

}  // namespace jit
