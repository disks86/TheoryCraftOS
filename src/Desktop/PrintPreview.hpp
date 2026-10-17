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

// A print preview: one sheet of paper, drawn to scale inside the control, with what will be printed on it.
//  - A text (show_text) shows as it will fall on the page - the margins, the wrapping, where each page ends - with every line
//    drawn as bars where there are characters, since the characters themselves are too small to read at this size.
//  - A picture (show_picture) is placed and turned into dots the way the printer will, at the size of the preview.
// Page counts and layout come from the same code the print job uses (hardware::TextLayout, hardware::render_surface), so the preview
// shows what comes out. The control borrows the layout and the picture: they must stay valid while shown.
#pragma once
#include "Gui/Control.hpp"
#include "Hardware/Printer/Halftone.hpp"
#include "Hardware/Printer/TextLayout.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

class PrintPreview : public gui::Control {
 public:
  static constexpr int32_t kBorder = 8;

  explicit PrintPreview(allocators::IAllocator& allocator) : Control(allocator) {}
  ~PrintPreview() { release_view(allocator(), cache_); }

  // The paper (turned sideways for landscape) and the margin all round that the printer cannot mark.
  void set_sheet(hardware::PaperSize paper, bool landscape, uint32_t margin_mm10);
  // What is on the paper. nullptr (or show_nothing) leaves it blank.
  void show_text(const hardware::TextLayout* layout);
  void show_picture(const graphics::Surface* picture, hardware::Fit fit, hardware::Halftone method);
  void show_nothing();

  // The page shown, counted from 0; clamped to the pages there are.
  void set_page(uint32_t page);
  uint32_t page() const { return page_; }
  // The text's pages, 1 for a picture, 0 for nothing.
  uint32_t page_count() const;

  // Geometry (for painting and tests), in this control's coordinates: the sheet and the area inside its margins.
  gui::Rect2D sheet_rect() const;
  gui::Rect2D printable_rect() const;
  // How many bars of text page `page` draws (lines with something on them), for tests.
  uint32_t drawn_lines(uint32_t page) const;

  void layout() override;
  void paint(gui::Painter& painter) override;

 protected:
  gui::Size default_size() const override { return {220, 300}; }

 private:
  void rebuild_picture(const gui::Rect2D& area);

  hardware::PaperSize paper_ = hardware::PaperSize::A4;
  bool landscape_ = false;
  uint32_t margin_mm10_ = 42;
  const hardware::TextLayout* text_ = nullptr;
  const graphics::Surface* picture_ = nullptr;
  hardware::Fit fit_ = hardware::Fit::Contain;
  hardware::Halftone method_ = hardware::Halftone::ErrorDiffusion;
  uint32_t page_ = 0;
  ArrayView<uint32_t> cache_;  // the halftoned picture at the size it is drawn
  uint32_t cache_w_ = 0, cache_h_ = 0;
  bool cache_valid_ = false;
};

}  // namespace desktop
