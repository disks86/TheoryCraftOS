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

#include "Hardware/GraphicsProcessingUnit/Intel/IntelScanout.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace intel_display {

namespace {
constexpr uint32_t kPitchAlignment = 64;
uint32_t pitch_for(uint32_t width) { return (width * 4 + kPitchAlignment - 1) / kPitchAlignment * kPitchAlignment; }
}  // namespace

bool Scanout::initialize() {
  shutdown();
  if (!display_.probe()) return false;
  const uint64_t bytes = uint64_t{pitch_for(display_.native_width())} * display_.native_height();
  const uint64_t pages = (bytes + intel_gt::kPageSize - 1) / intel_gt::kPageSize;
  uint64_t found = 0;
  if (!intel_gt::find_unused_ggtt_range(ggtt_table_, pages + kSurfaceAlignmentPages, kGuardPages, &found)) return false;
  first_page_ = (found + kSurfaceAlignmentPages - 1) / kSurfaceAlignmentPages * kSurfaceAlignmentPages;
  pages_ = pages;
  // The surface address has to fit the 32 bits of the plane's surface register.
  if ((first_page_ + pages_) * intel_gt::kPageSize > (uint64_t{1} << 32)) return false;
  const intel_gt::Ggtt ggtt(ggtt_table_, first_page_, pages_);
  if (!ggtt.valid()) return false;

  uint8_t* memory = static_cast<uint8_t*>(allocator_.allocate_aligned(pages * intel_gt::kPageSize, intel_gt::kPageSize));
  saved_entries_ = allocate_view<uint64_t>(allocator_, pages);
  if (!memory || saved_entries_.empty()) {
    allocator_.deallocate(memory);
    release_view(allocator_, saved_entries_);
    return false;
  }
  memory_ = ByteView(memory, pages * intel_gt::kPageSize);
  for (uint8_t& b : memory_) b = 0;
  flush(memory_, kPitchAlignment, 0, 0, 16, static_cast<uint32_t>(memory_.size() / kPitchAlignment));  // (the zeros must reach memory too)
  // The memory is identity mapped: its address is the physical one the GTT entries need.
  const uint64_t base = reinterpret_cast<uint64_t>(memory);
  for (uint64_t i = 0; i < pages_; ++i) {
    ggtt.read(i, &saved_entries_[i]);
    ggtt.map(i, base + i * intel_gt::kPageSize);
  }
  intel_gt::ggtt_invalidate(registers_);
  ready_ = true;
  return true;
}

bool Scanout::set_mode(uint32_t width, uint32_t height, ByteView* memory, uint32_t* pitch_bytes) {
  if (!ready_ || !display_.can_show(width, height)) return false;
  const uint32_t pitch = pitch_for(width);
  const uint64_t needed = uint64_t{pitch} * height;
  if (needed > memory_.size()) return false;
  // The new mode starts black: clear the part it shows, and push that out of the caches before the engine reads it.
  ByteView shown = memory_.first(needed);
  for (uint8_t& b : shown) b = 0;
  flush(shown, pitch, 0, 0, width, height);
  if (!display_.set_mode(width, height, intel_gt::Ggtt(ggtt_table_, first_page_, pages_).address_of(0), pitch)) return false;
  *memory = shown;
  *pitch_bytes = pitch;
  return true;
}

void Scanout::flush(ByteView memory, uint32_t pitch_bytes, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || pitch_bytes == 0) return;
  constexpr uint64_t kLine = 64;
  for (uint32_t row = y; row < y + height; ++row) {
    const uint64_t start = uint64_t{row} * pitch_bytes + uint64_t{x} * 4;
    const uint64_t end = start + uint64_t{width} * 4;
    if (end > memory.size()) break;
    for (uint64_t at = start & ~(kLine - 1); at < end; at += kLine) __builtin_ia32_clflush(memory.data() + at);
  }
  __builtin_ia32_sfence();
}

void Scanout::shutdown() {
  if (display_.active() && display_.original().valid && ready_) display_.restore(display_.original());
  if (ready_) {
    const intel_gt::Ggtt ggtt(ggtt_table_, first_page_, pages_);
    for (uint64_t i = 0; i < pages_; ++i) ggtt.write_raw(i, saved_entries_[i]);
    intel_gt::ggtt_invalidate(registers_);
  }
  ready_ = false;
  allocator_.deallocate(memory_.data());
  memory_ = ByteView();
  release_view(allocator_, saved_entries_);
}

}  // namespace intel_display
}  // namespace hardware
