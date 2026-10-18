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

// The paint application: a window with a menu bar (File: New, Open, Save, Save As, Print, Close; Edit: Undo, Redo, Cut, Copy, Paste,
// Select All; View: Zoom In, Zoom Out, Actual Size, Pixel Grid; Options: Filled Shapes, Primary Color, Secondary Color), the
// brush size, a strip of tools at the left, the DrawingCanvas, and at the bottom a
// color palette with a color picker for any other color. Files are opened and saved as images; the format follows the
// extension (png, jpg, bmp, gif, ppm, qoi, tga, tif, webp; png when there is none); Open and Save As pick the file in
// the shared FileDialog, Save writes to the file the picture came from (or asks like Save As when there is none). Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Z, Ctrl+Y and Ctrl+A
// do what their menu items do. Cut, Copy and Paste (Ctrl+X, Ctrl+C, Ctrl+V) use the system clipboard: the selection made with the
// Select tool (the whole picture when nothing is selected) goes onto it as a picture, and a picture on it is pasted as a floating
// selection to drag into place. Warnings and errors appear in the shared NoticeDialog. A command that would throw away unsaved changes does nothing the first time (a
// warning says so) and goes ahead when it is repeated.
#pragma once
#include "Desktop/ColorDialog.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/PrintDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/ColorPalette.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/ColorPicker.hpp"
#include "Gui/Controls/DrawingCanvas.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/Slider.hpp"
#include "Gui/Controls/ToolStrip.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class Paint : public gui::Frame {
 public:
  static constexpr uint32_t kNewWidth = 640, kNewHeight = 480;
  static constexpr uint64_t kMaxFileBytes = 64u << 20;

  explicit Paint(allocators::IAllocator& allocator);

  // The filesystem pictures are opened from and saved to (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  // The dialog that Open and Save As use (nullptr: none, they then say so). It must outlive the window.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  // The dialog that Print uses, and the one that Options > Primary / Secondary Color use (nullptr: none, they then say so). They must outlive the window.
  void set_print_dialog(PrintDialog* dialog) { print_dialog_ = dialog; }
  void set_color_dialog(ColorDialog* dialog) { color_dialog_ = dialog; }
  // The dialog that warnings, information and errors are shown in (nullptr: the status line says them instead). It must outlive the window.
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }

  // Starts a blank white picture. False (nothing changes) while there are unsaved changes the user has not agreed to lose.
  bool new_picture(uint32_t width = kNewWidth, uint32_t height = kNewHeight);
  // Loads the picture at `path`; false if it cannot be read or decoded (an error dialog says why) or unsaved changes are in the way.
  bool open_file(StringView path);
  // Saves to `path` (the toolbar's path when empty). False if that fails.
  bool save_file(StringView path = StringView());
  // The commands behind File > Open and File > Save As: they show the dialog and carry on when a file is picked.
  void ask_open();
  void ask_save_as();
  // File > Print: the print dialog, then the picture goes to the chosen printer.
  void ask_print();
  // Options > Primary Color / Secondary Color: the color dialog, then the palette takes the choice.
  void ask_color(bool secondary);
  // True if a file name ends in an extension of a picture format (case does not matter).
  static bool is_picture_name(StringView name);

  StringView path() const { return path_.view(); }
  gui::DrawingCanvas& canvas() { return canvas_; }
  gui::ToolStrip& tools() { return tools_; }
  gui::ColorPalette& palette() { return palette_; }
  gui::ColorPicker& color_picker() { return picker_; }
  gui::Slider& size_slider() { return size_; }
  gui::MenuBar& menu_bar() { return menu_; }
  // The canvas's context menu (while the Select tool is in use): Undo, Redo, Cut, Copy, Paste, Delete, Select All.
  gui::ContextMenu& canvas_menu() { return canvas_menu_; }
  gui::Label& status_label() { return status_; }
  gui::Label& position_label() { return position_; }
  gui::Label& zoom_label() { return zoom_label_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_save_as(void* self, gui::Control&);
  static void on_print(void* self, gui::Control&);
  static void on_print_picked(void* self, gui::Control&);
  static void on_primary_color(void* self, gui::Control&);
  static void on_secondary_color(void* self, gui::Control&);
  static void on_color_picked_by_dialog(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_save_picked(void* self, gui::Control&);
  static void on_cut(void* self, gui::Control&);
  static void on_copy(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_tool_changed(void* self, gui::Control&);
  void copy_to_clipboard(bool cut);
  void paste_from_clipboard();
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
  static void on_toggle_filled(void* self, gui::Control&);
  static void on_toggle_grid(void* self, gui::Control&);
  static void on_actual_size(void* self, gui::Control&);
  static void on_select_all(void* self, gui::Control&);
  static void on_delete_selection(void* self, gui::Control&);
  static void on_canvas_menu_opening(void* self, gui::Control&);
  static void on_close_window(void* self, gui::Control&);
  static void on_canvas_changed(void* self, gui::Control&);
  static void on_pointer(void* self, gui::Control&);
  static void on_zoom_changed(void* self, gui::Control&);
  bool may_discard();
  bool read_picture(StringView path);
  bool write_picture(StringView path);
  void say(StringView text) { status_.set_text(text); }
  // Warnings, information and errors go to the notice dialog (or the status line when there is none to show it in).
  void notify(gui::NoticeKind kind, StringView text) {
    if (!gui::NoticeDialog::post(notices_, manager(), kind, text)) say(text);
  }
  void update_state();
  void update_position();

  gui::MenuBar menu_;
  gui::ContextMenu canvas_menu_;
  gui::MenuItem *ctx_undo_ = nullptr, *ctx_redo_ = nullptr, *ctx_cut_ = nullptr, *ctx_copy_ = nullptr, *ctx_paste_ = nullptr, *ctx_delete_ = nullptr;
  gui::Panel options_bar_, palette_bar_, status_bar_;
  gui::Label size_label_, size_value_, zoom_label_, status_, position_;
  gui::Slider size_;
  gui::MenuItem *undo_item_ = nullptr, *redo_item_ = nullptr, *filled_item_ = nullptr, *grid_item_ = nullptr;
  gui::ToolStrip tools_;
  gui::ColorPalette palette_;
  gui::ColorPicker picker_;
  gui::DrawingCanvas canvas_;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  PrintDialog* print_dialog_ = nullptr;
  gui::NoticeDialog* notices_ = nullptr;
  ColorDialog* color_dialog_ = nullptr;
  bool choosing_secondary_ = false;
  graphics::Surface print_surface_;   // the picture the print dialog previews (it borrows it)
  String path_;  // the file the picture came from or was saved to (empty: none yet)
  String name_;
  bool discard_armed_ = false;
};

}  // namespace desktop
