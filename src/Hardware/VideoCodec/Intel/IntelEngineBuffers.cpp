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
#include "Hardware/VideoCodec/Intel/IntelEngineBuffers.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {

namespace {
constexpr uint64_t kPage = intel_gt::kPageSize;
constexpr uint64_t kAddressAlignment = 0x10000;
constexpr uint64_t kAddressLimit = 0xFFFF0000ull;       // buffers stay below 4 GiB with a margin
constexpr uint64_t kBitstreamMinimum = 1u << 20;
constexpr uint64_t kBatchWords = intel_gt::VcsDriver::kBatchBytes / 4;

uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }
}  // namespace

IntelEngineBuffers::~IntelEngineBuffers() {
  release_all();
  if (leased_) {
    release_view(allocator_, batch_words_);
    driver_.release_lease();
  }
}

bool IntelEngineBuffers::lease() {
  if (leased_) return true;
  if (!driver_.try_lease()) return false;
  batch_words_ = allocate_view<uint32_t>(allocator_, kBatchWords);
  if (batch_words_.empty()) {
    driver_.release_lease();
    return false;
  }
  leased_ = true;
  return true;
}

void IntelEngineBuffers::release_lease() {
  if (!leased_) return;
  release_all();
  release_view(allocator_, batch_words_);
  batch_words_ = ArrayView<uint32_t>();
  driver_.release_lease();
  leased_ = false;
}

bool IntelEngineBuffers::place(Block* block, uint64_t bytes) {
  bytes = round_up(bytes, kPage);
  const uint64_t at = round_up(next_address_, kAddressAlignment);
  if (bytes == 0 || at > kAddressLimit || bytes > kAddressLimit - at) return false;
  if (!driver_.memory().allocate(bytes, &block->buffer)) return false;
  if (!driver_.map_buffer(block->buffer, static_cast<uint32_t>(at))) {
    driver_.memory().free(block->buffer.cpu);
    block->buffer = intel_gt::GpuBuffer();
    return false;
  }
  block->address = static_cast<uint32_t>(at);
  next_address_ = at + bytes + kAddressAlignment;   // an unmapped gap behind every buffer
  return true;
}

void IntelEngineBuffers::drop(Block* block) {
  if (!block->buffer.valid()) return;
  // Only an engine that is idle and ours can have its buffers freed; a wedged one may still be writing to them.
  if (engine_ready()) {
    driver_.unmap_buffer(block->buffer, block->address);
    driver_.memory().free(block->buffer.cpu);
  }
  block->buffer = intel_gt::GpuBuffer();
}

void IntelEngineBuffers::release_all() {
  for (Block& b : surfaces_) drop(&b);
  for (Block& b : aux_) drop(&b);
  for (Block& b : scratch_) drop(&b);
  drop(&bitstream_);
  next_address_ = intel_gt::VcsDriver::kFirstBufferGpuAddress;
  open_ = false;
}

bool IntelEngineBuffers::open(uint32_t width, uint32_t height) {
  release_all();
  if (width < 16 || height < 16 || width > kMaxMbsPerSide * 16 || height > kMaxMbsPerSide * 16 || !intel_mfx::nv12_tiled_layout(width, height, &layout_)) return false;
  width_ = width;
  height_ = height;
  if (!place(&bitstream_, kBitstreamMinimum)) return false;
  open_ = true;
  return true;
}

bool IntelEngineBuffers::surface(int slot, uint32_t* address) {
  if (!open_ || slot < 0 || slot >= static_cast<int>(kMaxSurfaces)) return false;
  Block& b = surfaces_[slot];
  if (!b.buffer.valid() && !place(&b, layout_.size)) return false;
  *address = b.address;
  return true;
}

bool IntelEngineBuffers::upload_scratch(uint32_t index, ConstByteView bytes) {
  if (!open_ || index >= kMaxScratch || !scratch_[index].buffer.valid()) return false;
  return scratch_[index].buffer.cpu.first(bytes.size()).copy_from(bytes);
}

bool IntelEngineBuffers::clear_scratch(uint32_t index) {
  if (!open_ || index >= kMaxScratch || !scratch_[index].buffer.valid()) return false;
  scratch_[index].buffer.cpu.fill(0);
  return true;
}

bool IntelEngineBuffers::read_scratch(uint32_t index, ByteView out) const {
  if (!open_ || index >= kMaxScratch || !scratch_[index].buffer.valid()) return false;
  return out.copy_from(scratch_[index].buffer.cpu.first(out.size()));
}

bool IntelEngineBuffers::picture_buffer(int slot, uint64_t bytes, uint32_t* address) {
  if (!open_ || slot < 0 || slot >= static_cast<int>(kMaxSurfaces)) return false;
  Block& b = aux_[slot];
  if (b.buffer.valid() && b.buffer.cpu.size() < round_up(bytes, kPage)) drop(&b);
  if (!b.buffer.valid() && !place(&b, bytes)) return false;
  *address = b.address;
  return true;
}

bool IntelEngineBuffers::copy_surface(int from, int to) {
  if (!open_ || !engine_ready() || from < 0 || to < 0 || from >= static_cast<int>(kMaxSurfaces) || to >= static_cast<int>(kMaxSurfaces)) return false;
  const intel_gt::GpuBuffer& a = surfaces_[from].buffer;
  intel_gt::GpuBuffer& b = surfaces_[to].buffer;
  return a.valid() && b.valid() && b.cpu.first(layout_.size).copy_from(a.cpu.first(layout_.size));
}

bool IntelEngineBuffers::scratch(uint32_t index, uint64_t bytes, uint32_t* address) {
  if (!open_ || index >= kMaxScratch) return false;
  Block& b = scratch_[index];
  if (b.buffer.valid() && b.buffer.cpu.size() < round_up(bytes, kPage)) drop(&b);   // (the engine is idle between jobs)
  if (!b.buffer.valid() && !place(&b, bytes)) return false;
  *address = b.address;
  return true;
}

bool IntelEngineBuffers::upload_bitstream(ConstByteView bits, uint32_t* address) {
  if (!open_) return false;
  if (!bitstream_.buffer.valid() || bitstream_.buffer.cpu.size() < bits.size()) {
    uint64_t wanted = bitstream_.buffer.valid() ? bitstream_.buffer.cpu.size() * 2 : kBitstreamMinimum;
    if (wanted < bits.size()) wanted = bits.size();
    drop(&bitstream_);
    if (!place(&bitstream_, wanted)) return false;
  }
  if (!bitstream_.buffer.cpu.first(bits.size()).copy_from(bits)) return false;
  *address = bitstream_.address;
  return true;
}

bool IntelEngineBuffers::run(ArrayView<const uint32_t> words) { return driver_.run_batch(words) == intel_gt::VcsDriver::Result::Done; }

bool IntelEngineBuffers::read_back(int slot, ByteView y, uint32_t y_stride, ByteView u, uint32_t u_stride, ByteView v, uint32_t v_stride, uint32_t first_line, uint32_t line_step) {
  if (!open_ || slot < 0 || slot >= static_cast<int>(kMaxSurfaces) || !surfaces_[slot].buffer.valid()) return false;
  return intel_mfx::detile_nv12_to_planes(surfaces_[slot].buffer.cpu, layout_, width_, height_, y, y_stride, u, u_stride, v, v_stride, first_line, line_step);
}

void IntelEngineBuffers::grey(int slot) {
  if (!engine_ready() || slot < 0 || slot >= static_cast<int>(kMaxSurfaces)) return;
  for (uint8_t& b : surfaces_[slot].buffer.cpu) b = 128;
}

}  // namespace hardware
