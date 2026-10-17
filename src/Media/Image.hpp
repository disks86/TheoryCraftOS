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
#include "Graphics/GraphicsTypes.hpp"
#include "Utility/AllocatedView.hpp"

namespace media {

enum class ImageFormat : uint8_t { Unknown, Png, Jpeg, Bmp, Gif, Pnm, Qoi, Tga };
const char* image_format_name(ImageFormat format);
constexpr uint32_t image_format_bit(ImageFormat format) { return 1u << static_cast<uint8_t>(format); }
// Which format a file is, from its first bytes (Tga has no signature; it is recognised by a plausible header).
ImageFormat detect_image_format(ConstByteView data);

// A decoded picture, or an animation: one or more frames of the same size in 0xAARRGGBB (straight, not
// premultiplied alpha), the pixels of each frame one row after the other without padding. Still pictures have one
// frame. Animations carry how long each frame shows and how often the sequence repeats.
class Image {
 public:
  static constexpr uint64_t kMaxPixels = 1u << 27;  // across all the frames: a guard against absurd headers

  explicit Image(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Image() { clear(); }
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  // Room for `frames` frames of width x height, every pixel transparent black and every delay 100 ms. False if the size
  // is zero or too big, or memory ran out; the image is then empty.
  bool create(uint32_t width, uint32_t height, uint32_t frames = 1);
  void clear();

  bool empty() const { return pixels_.empty(); }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t frame_count() const { return frames_; }
  // Keeps only the first `frames` frames (the memory is kept until clear()); no-op if there are fewer.
  void limit_frames(uint32_t frames) { if (frames < frames_) frames_ = frames; }
  bool animated() const { return frames_ > 1; }
  // The pixels of one frame (an empty view for a frame that does not exist).
  ArrayView<uint32_t> frame(uint32_t index) const {
    return index < frames_ ? pixels_.subview(uint64_t{index} * width_ * height_, uint64_t{width_} * height_) : ArrayView<uint32_t>();
  }
  // A Surface over a frame, for drawing (it points into the image: valid while the image is).
  graphics::Surface surface(uint32_t index = 0) const;

  uint32_t delay_ms(uint32_t index) const { return index < frames_ ? delays_[index] : 0; }
  void set_delay_ms(uint32_t index, uint32_t ms) { if (index < frames_) delays_[index] = ms; }
  // How many times an animation plays; 0 means for ever.
  uint32_t loop_count() const { return loops_; }
  void set_loop_count(uint32_t loops) { loops_ = loops; }
  // True if any pixel is not fully opaque (decoders set this).
  bool has_alpha() const { return has_alpha_; }
  void set_has_alpha(bool v) { has_alpha_ = v; }
  uint64_t total_duration_ms() const;
  allocators::IAllocator& allocator() const { return allocator_; }

 private:
  allocators::IAllocator& allocator_;
  ArrayView<uint32_t> pixels_;
  ArrayView<uint32_t> delays_;
  uint32_t width_ = 0, height_ = 0, frames_ = 0, loops_ = 0;
  bool has_alpha_ = false;
};

}  // namespace media
