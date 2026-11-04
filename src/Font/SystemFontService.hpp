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
#include "Font/FontSet.hpp"
#include "Process/WindowsFonts.hpp"

namespace font {

// The system's fonts as a Windows program's DirectWrite sees them (NtTcosFont, src/Process/WindowsFonts.hpp): the bundled faces of the FontSet, then the fonts programs add. The
// caller serialises the calls (the service holds the system lock), as the FontSet itself has no lock.
class SystemFontService final : public process::windowsabi::IFontService {
 public:
  explicit SystemFontService(allocators::IAllocator& allocator);

  FontSet& fonts() { return fonts_; }
  allocators::IAllocator& allocator() override { return allocator_; }
  uint32_t face_count() override;
  bool face_info(uint32_t face, process::windowsabi::FontFaceInfo* out) override;
  bool face_string(uint32_t face, uint32_t which, String* out) override;
  bool map_codepoint(uint32_t face, uint32_t codepoint, uint32_t* glyph) override;
  bool glyph_box(uint32_t face, uint32_t glyph, process::windowsabi::FontGlyphBox* out) override;
  bool kerning(uint32_t face, uint32_t left, uint32_t right, int32_t* units) override;
  bool rasterize(uint32_t face, uint32_t glyph, int32_t size, uint32_t subpixel, process::windowsabi::FontBitmap* out) override;
  bool outline(uint32_t face, uint32_t glyph, ByteBuffer* contour_ends, ByteBuffer* points) override;
  bool font_data(uint32_t face, uint64_t offset, ByteView out, uint64_t* total, uint64_t* copied) override;
  bool add_font(ConstByteView data, uint32_t* face) override;

 private:
  allocators::IAllocator& allocator_;
  FontSet fonts_;
};

}  // namespace font
