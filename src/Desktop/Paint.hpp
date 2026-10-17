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

// The paint application: a window with a toolbar (New, Open, Save, the file's path, Undo, Redo), the drawing options
// (brush size, filled shapes, pixel grid, zoom), a strip of tools at the left, the DrawingCanvas, and at the bottom a
// color palette with a color picker for any other color. Files are opened and saved as images; the format follows the
// extension (png, jpg, bmp, gif, ppm, qoi, tga, tif, webp; png when there is none). Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Z and Ctrl+Y
// do what their buttons do. A command that would throw away unsaved changes does nothing the first time (the status
// line says so) and goes ahead when it is repeated.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/CheckBox.hpp"
#include "Gui/Controls/ColorPalette.hpp"
#include "Gui/Controls/ColorPicker.hpp"
#include "Gui/Controls/DrawingCanvas.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/Slider.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Controls/ToolStrip.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class Paint : public gui::Frame {
 public:
  static constexpr uint32_t kNewWidth = 640, kNewHeight = 480;
  static constexpr uint64_t kMaxFileBytes = 64u << 20;

  explicit Paint(allocators::IAllocator& allocator);

  // The filesystem pictures are opened from and saved to (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }

  // Starts a blank white picture. False (nothing changes) while there are unsaved changes the user has not agreed to lose.
  bool new_picture(uint32_t width = kNewWidth, uint32_t height = kNewHeight);
  // Loads the picture at `path`; false if it cannot be read or decoded (the status line says why) or unsaved changes are in the way.
  bool open_file(StringView path);
  // Saves to `path` (the toolbar's path when empty). False if that fails.
  bool save_file(StringView path = StringView());
  // True if a file name ends in an extension of a picture format (case does not matter).
  static bool is_picture_name(StringView name);

  StringView path() const { return path_.text(); }
  gui::DrawingCanvas& canvas() { return canvas_; }
  gui::ToolStrip& tools() { return tools_; }
  gui::ColorPalette& palette() { return palette_; }
  gui::ColorPicker& color_picker() { return picker_; }
  gui::Slider& size_slider() { return size_; }
  gui::CheckBox& filled_box() { return filled_; }
  gui::CheckBox& grid_box() { return grid_; }
  gui::TextBox& path_box() { return path_; }
  gui::Button& new_button() { return new_; }
  gui::Button& open_button() { return open_; }
  gui::Button& save_button() { return save_; }
  gui::Button& undo_button() { return undo_; }
  gui::Button& redo_button() { return redo_; }
  gui::Button& zoom_in_button() { return zoom_in_; }
  gui::Button& zoom_out_button() { return zoom_out_; }
  gui::Label& status_label() { return status_; }
  gui::Label& position_label() { return position_; }
  gui::Label& zoom_label() { return zoom_label_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_undo(void* self, gui::Control&);
  static void on_redo(void* self, gui::Control&);
  static void on_zoom_in(void* self, gui::Control&);
  static void on_zoom_out(void* self, gui::Control&);
  static void on_tool(void* self, gui::Control&);
  static void on_tool_hover(void* self, gui::Control&);
  static void on_palette(void* self, gui::Control&);
  static void on_picker(void* self, gui::Control&);
  static void on_picked(void* self, gui::Control&);
  static void on_size(void* self, gui::Control&);
  static void on_options(void* self, gui::Control&);
  static void on_canvas_changed(void* self, gui::Control&);
  static void on_pointer(void* self, gui::Control&);
  static void on_zoom_changed(void* self, gui::Control&);
  bool may_discard();
  void say(StringView text) { status_.set_text(text); }
  void update_state();
  void update_position();

  gui::Panel file_bar_, options_bar_, palette_bar_, status_bar_, gap_a_, gap_b_;
  gui::Button new_, open_, save_, undo_, redo_, zoom_in_, zoom_out_;
  gui::TextBox path_;
  gui::Label size_label_, size_value_, zoom_label_, status_, position_;
  gui::Slider size_;
  gui::CheckBox filled_, grid_;
  gui::ToolStrip tools_;
  gui::ColorPalette palette_;
  gui::ColorPicker picker_;
  gui::DrawingCanvas canvas_;
  filesystem::IFileSystem* fs_ = nullptr;
  String name_;
  bool discard_armed_ = false;
};

}  // namespace desktop
