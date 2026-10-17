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

#include "Kernel/Clipboard.hpp"
#include "Utility/AllocatedView.hpp"

namespace kernel {

bool Clipboard::replace(ClipboardFormat format, ArrayView<uint8_t> data, uint32_t width, uint32_t height) {
  ArrayView<uint8_t> old;
  {
    ScopedMutex guard(mutex_);
    old = data_;
    data_ = data;
    format_ = format;
    width_ = width;
    height_ = height;
    ++sequence_;
  }
  release_view(allocator_, old);
  return true;
}

bool Clipboard::set_text(StringView text) {
  if (text.empty() || text.size() > kMaxBytes) return false;
  ArrayView<uint8_t> copy = allocate_view<uint8_t>(allocator_, text.size());
  if (copy.empty()) return false;
  copy.copy_from(ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  return replace(ClipboardFormat::Text, copy, 0, 0);
}

bool Clipboard::set_image(const uint32_t* pixels, uint32_t width, uint32_t height) {
  if (!pixels || width == 0 || height == 0) return false;
  const uint64_t bytes = uint64_t{width} * height * sizeof(uint32_t);
  if (bytes > kMaxBytes) return false;
  ArrayView<uint8_t> copy = allocate_view<uint8_t>(allocator_, bytes);
  if (copy.empty()) return false;
  copy.copy_from(ConstByteView(reinterpret_cast<const uint8_t*>(pixels), bytes));
  return replace(ClipboardFormat::Image, copy, width, height);
}

void Clipboard::clear() {
  ArrayView<uint8_t> old;
  {
    ScopedMutex guard(mutex_);
    if (format_ == ClipboardFormat::Empty) return;
    old = data_;
    data_ = ArrayView<uint8_t>();
    format_ = ClipboardFormat::Empty;
    width_ = height_ = 0;
    ++sequence_;
  }
  release_view(allocator_, old);
}

ClipboardFormat Clipboard::format() const {
  ScopedMutex guard(mutex_);
  return format_;
}

uint64_t Clipboard::sequence() const {
  ScopedMutex guard(mutex_);
  return sequence_;
}

bool Clipboard::get_text(String* out) const {
  ScopedMutex guard(mutex_);
  if (format_ != ClipboardFormat::Text) return false;
  return out->assign(StringView(reinterpret_cast<const char*>(data_.data()), data_.size()));
}

bool Clipboard::image_size(uint32_t* width, uint32_t* height) const {
  ScopedMutex guard(mutex_);
  if (format_ != ClipboardFormat::Image) return false;
  *width = width_;
  *height = height_;
  return true;
}

bool Clipboard::get_image(uint32_t* pixels, uint64_t capacity) const {
  ScopedMutex guard(mutex_);
  if (format_ != ClipboardFormat::Image || capacity < uint64_t{width_} * height_) return false;
  __builtin_memcpy(pixels, data_.data(), data_.size());
  return true;
}

}  // namespace kernel
