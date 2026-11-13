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

#include "Wayland/Media/WaylandClipboardPng.hpp"
#include "Media/Image.hpp"
#include "Media/ImageCodecs.hpp"

namespace wayland {

bool clipboard_to_png(allocators::IAllocator& allocator, kernel::Clipboard& clipboard, ByteBuffer* png) {
  uint32_t width = 0, height = 0;
  if (!clipboard.image_size(&width, &height)) return false;
  media::Image image(allocator);
  if (!image.create(width, height)) return false;
  ArrayView<uint32_t> pixels = image.frame(0);
  if (!clipboard.get_image(pixels.data(), pixels.size())) return false;      // (it may have changed meanwhile: a different size does not fit)
  bool alpha = false;
  for (const uint32_t p : pixels) alpha |= (p >> 24) != 0xFF;
  image.set_has_alpha(alpha);      // (RGBA only when some pixel is not opaque: else the smaller RGB file)
  return media::encode_png(image, 6, png);
}

bool png_to_clipboard(allocators::IAllocator& allocator, kernel::Clipboard& clipboard, ConstByteView png) {
  media::Image image(allocator);
  if (!media::decode_png(png, &image) || image.empty()) return false;
  const ArrayView<uint32_t> pixels = image.frame(0);
  return clipboard.set_image(pixels.data(), image.width(), image.height());
}

}  // namespace wayland
