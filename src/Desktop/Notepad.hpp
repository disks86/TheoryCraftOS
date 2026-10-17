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

// The text editor application: a window with a toolbar (New, Open, Save and the path of the file), a TextEditor
// and a status bar showing the caret position and what the last command did. Open and Save use the path in the
// toolbar; Ctrl+N, Ctrl+O and Ctrl+S do the same as the buttons, and Enter in the path box opens the file. A
// command that would throw away unsaved changes does nothing the first time (the status line says so) and
// goes ahead when it is repeated. Files are read and written as plain bytes; a CR before a line break is dropped
// on opening, so saved files always use LF.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Controls/TextEditor.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class Notepad : public gui::Frame {
 public:
  static constexpr uint64_t kMaxFileBytes = gui::TextEditor::kDefaultMaxLength;

  explicit Notepad(allocators::IAllocator& allocator);

  // The filesystem files are opened from and saved to (nullptr: none). It must outlive the editor.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }

  // Starts an empty, unnamed document. False (and nothing changes) while there are unsaved changes the user has not
  // agreed to lose.
  bool new_document();
  // Loads the file at `path` into the editor. False if it cannot be read (the status line says why) or unsaved changes
  // are in the way.
  bool open_file(StringView path);
  // Writes the text to `path` (the toolbar's path when empty). False if that fails.
  bool save_file(StringView path = StringView());

  StringView path() const { return path_.text(); }
  gui::TextEditor& editor() { return editor_; }
  gui::TextBox& path_box() { return path_; }
  gui::Button& new_button() { return new_; }
  gui::Button& open_button() { return open_; }
  gui::Button& save_button() { return save_; }
  gui::Label& status_label() { return status_; }
  gui::Label& position_label() { return position_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_edited(void* self, gui::Control&);
  static void on_moved(void* self, gui::Control&);
  bool may_discard();
  void say(StringView text) { status_.set_text(text); }
  void update_title();
  void update_position();

  gui::Panel toolbar_, status_bar_, gap_;
  gui::Button new_, open_, save_;
  gui::TextBox path_;
  gui::Label status_, position_;
  gui::TextEditor editor_;
  filesystem::IFileSystem* fs_ = nullptr;
  String name_;  // the file's name as shown in the title
  bool discard_armed_ = false;
};

}  // namespace desktop
