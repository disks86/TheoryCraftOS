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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInstmem.hpp"

namespace hardware {
namespace nv {

// ---- PraminWindow ----

bool PraminWindow::place(uint64_t vram_address) {
  const uint64_t base = vram_address & ~(kWindowBytes - 1);
  if (base != base_) {
    regs_.write32(kWindowBase, static_cast<uint32_t>(base >> 16));
    base_ = base;
    ++moves_;
  }
  return true;
}

bool PraminWindow::read32(uint64_t vram_address, uint32_t* value) {
  if (!in_range(vram_address, 4)) return false;
  NvLockGuard guard(lock_);
  place(vram_address);
  *value = regs_.read32(kWindow + static_cast<uint32_t>(vram_address & (kWindowBytes - 1)));
  return true;
}

bool PraminWindow::write32(uint64_t vram_address, uint32_t value) {
  if (!in_range(vram_address, 4)) return false;
  NvLockGuard guard(lock_);
  place(vram_address);
  regs_.write32(kWindow + static_cast<uint32_t>(vram_address & (kWindowBytes - 1)), value);
  return true;
}

bool PraminWindow::fill32(uint64_t vram_address, uint64_t bytes, uint32_t value) {
  if (!in_range(vram_address, bytes)) return false;
  uint64_t at = vram_address;
  const uint64_t end = vram_address + bytes;
  while (at < end) {
    // One window position at a time, the lock held for at most a window's worth of words.
    const uint64_t window_end = (at & ~(kWindowBytes - 1)) + kWindowBytes;
    const uint64_t stop = window_end < end ? window_end : end;
    NvLockGuard guard(lock_);
    place(at);
    for (; at < stop; at += 4) regs_.write32(kWindow + static_cast<uint32_t>(at & (kWindowBytes - 1)), value);
  }
  return true;
}

void PraminWindow::forget() {
  NvLockGuard guard(lock_);
  base_ = ~uint64_t{0};
}

uint32_t PraminWindow::moves() {
  NvLockGuard guard(lock_);
  return moves_;
}

// ---- NvInstmem ----

bool NvInstmem::enable_bar2(NvBar& bar, INvAperture& aperture, uint32_t slots) {
  if (!bar.ready() || slots == 0 || slots > kMaxSlots) return false;
  const Optional<uint64_t> va = bar.bar2().allocate_va(slots * kSlotBytes, kSlotBytes);
  if (!va || *va > aperture.bytes() || slots * kSlotBytes > aperture.bytes() - *va) {
    if (va) (void)bar.bar2().free_va(*va);
    return false;
  }
  NvLockGuard guard(lock_);
  bar_ = &bar;
  aperture_ = &aperture;
  va_base_ = *va;
  slot_count_ = slots;
  next_victim_ = 0;
  for (Slot& s : slots_) s = Slot{};
  return true;
}

void NvInstmem::disable_bar2() {
  NvLockGuard guard(lock_);
  if (bar_) {
    (void)bar_->bar2().unmap(va_base_, slot_count_ * kSlotBytes);
    (void)bar_->bar2().free_va(va_base_);
  }
  bar_ = nullptr;
  aperture_ = nullptr;
  slot_count_ = 0;
}

Optional<uint64_t> NvInstmem::through_bar2(uint64_t vram_address) {
  if (!bar_ || !aperture_ || slot_count_ == 0) return {};
  const uint64_t chunk = vram_address & ~(kSlotBytes - 1);
  const ArrayView<Slot> slots = ArrayView<Slot>(slots_).first(slot_count_);
  for (uint32_t i = 0; i < slots.size(); ++i)
    if (slots[i].chunk == chunk) return va_base_ + i * kSlotBytes + (vram_address - chunk);
  const uint32_t victim = next_victim_;
  next_victim_ = (next_victim_ + 1) % slot_count_;
  GpuMemory piece;
  piece.gpu_address = chunk;
  piece.bytes = kSlotBytes;
  piece.target = MemoryTarget::Vram;
  slots[victim].chunk = ~uint64_t{0};
  const uint64_t va = va_base_ + victim * kSlotBytes;
  if (!bar_->bar2().map(va, piece, 0, kSlotBytes) || !bar_->bar2().flush()) return {};
  slots[victim].chunk = chunk;
  ++remaps_;
  return va + (vram_address - chunk);
}

bool NvInstmem::read32(uint64_t vram_address, uint32_t* value) {
  if (vram_address % 4 || vram_address > vram_bytes_ || vram_bytes_ - vram_address < 4) return false;
  {
    NvLockGuard guard(lock_);
    if (const Optional<uint64_t> at = through_bar2(vram_address)) {
      *value = aperture_->read32(*at);
      return true;
    }
  }
  return pramin_.read32(vram_address, value);
}

bool NvInstmem::write32(uint64_t vram_address, uint32_t value) {
  if (vram_address % 4 || vram_address > vram_bytes_ || vram_bytes_ - vram_address < 4) return false;
  {
    NvLockGuard guard(lock_);
    if (const Optional<uint64_t> at = through_bar2(vram_address)) {
      aperture_->write32(*at, value);
      return true;
    }
  }
  return pramin_.write32(vram_address, value);
}

bool NvInstmem::flush_writes() {
  NvLockGuard guard(lock_);
  return bar_ ? bar_->flush_writes() : true;
}

bool NvInstmem::bar2_active() {
  NvLockGuard guard(lock_);
  return bar_ != nullptr;
}

uint32_t NvInstmem::remaps() {
  NvLockGuard guard(lock_);
  return remaps_;
}

}  // namespace nv
}  // namespace hardware
