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

// The desktop's text drawn at the device's resolution (HiDPI): each character of the 8x16 font's grid is drawn with a bundled TrueType face
// ("TCOS Mono", then the wide-coverage "TCOS Fallback"), sized to fill the cell on the device, instead of the 8x16 font's pixels enlarged. The cell
// layout is unchanged: a character still takes 8 x 16 units. ScaledRenderer2D::set_text_face takes it.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Font/GlyphCache.hpp"
#include "Font/TrueType.hpp"
#include "Graphics/Graphics2D/ScaledRenderer2D.hpp"
#include "Utility/ByteBuffer.hpp"

namespace graphics {

class CellTextFace final : public ICellTextFace {
 public:
  explicit CellTextFace(allocators::IAllocator& allocator);
  ~CellTextFace() = default;
  CellTextFace(const CellTextFace&) = delete;
  CellTextFace& operator=(const CellTextFace&) = delete;

  bool ready() const { return mono_.valid(); }      // false: the font could not be loaded (draw_cell then always declines)
  bool draw_cell(IRenderer2D& device, char32_t c, const Rect2D& cell, uint32_t color) override;

 private:
  struct Face {
    explicit Face(allocators::IAllocator& a) : sfnt(a), font(a) {}
    ByteBuffer sfnt;           // (the font reads from it: declared first, gone last)
    font::TrueTypeFont font;
  };
  // The size (26.6 px) at which the face fills a cell of w x h device pixels: its advance the width, its ascent and descent within the height.
  font::Fixed size_for(const font::TrueTypeFont& f, uint32_t w, uint32_t h) const;

  allocators::IAllocator& allocator_;
  Face mono_face_, fallback_face_;
  font::TrueTypeFont& mono_;
  font::GlyphCache cache_;
};

}  // namespace graphics
