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

// The system clipboard: one piece of data that programs hand to each other, copied in by whoever puts it there and
// copied out again by whoever pastes it (so the owner may change or close its own data at once). It holds either text
// (bytes, normally UTF-8) or a picture (0xAARRGGBB pixels); putting in one kind replaces the other. Every change counts up
// sequence(), so a program can tell whether the clipboard changed since it last looked. All calls are safe from any
// thread. The data lives in memory from the allocator given and is freed when it is replaced, cleared or the clipboard
// goes away.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"

namespace kernel {

enum class ClipboardFormat : uint8_t { Empty, Text, Image };

class Clipboard {
 public:
  static constexpr uint64_t kMaxBytes = 64u << 20;  // the most it will hold

  explicit Clipboard(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Clipboard() { clear(); }
  Clipboard(const Clipboard&) = delete;
  Clipboard& operator=(const Clipboard&) = delete;

  // Replace the contents. False (and the old contents stay) if the data is empty or over kMaxBytes or memory ran out.
  bool set_text(StringView text);
  // `pixels` holds width * height values, row after row without padding.
  bool set_image(const uint32_t* pixels, uint32_t width, uint32_t height);
  void clear();

  ClipboardFormat format() const;
  bool has_text() const { return format() == ClipboardFormat::Text; }
  bool has_image() const { return format() == ClipboardFormat::Image; }
  uint64_t sequence() const;

  // Copies the text into `*out` (replacing what it held). False if the clipboard holds no text or memory ran out.
  bool get_text(String* out) const;
  // The size of the picture on the clipboard; false if it holds none.
  bool image_size(uint32_t* width, uint32_t* height) const;
  // Copies the picture into `pixels` (room for `capacity` values, at least width * height). False if there is none or it does not fit.
  bool get_image(uint32_t* pixels, uint64_t capacity) const;

 private:
  bool replace(ClipboardFormat format, ArrayView<uint8_t> data, uint32_t width, uint32_t height);

  allocators::IAllocator& allocator_;
  mutable Mutex mutex_;
  ClipboardFormat format_ = ClipboardFormat::Empty;
  ArrayView<uint8_t> data_;
  uint32_t width_ = 0, height_ = 0;
  uint64_t sequence_ = 0;
};

}  // namespace kernel
