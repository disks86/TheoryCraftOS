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

// The text editor application: a window with a menu bar (File: New, Open, Save, Save As, Print, Close; Edit: Cut, Copy, Paste,
// Select All), a TextEditor and a status bar showing the caret position and what the last command did. Warnings, information and
// errors appear in the shared NoticeDialog rather than in the status bar. Open and Save As
// pick the file in the shared FileDialog; Save writes to the file the text came from (or asks like Save As when there is
// none). Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S and Ctrl+P do the same as the menu items. A
// command that would throw away unsaved changes does nothing the first time (a warning says so) and
// goes ahead when it is repeated. Files are read and written as plain bytes; a CR before a line break is dropped
// on opening, so saved files always use LF.
#pragma once
#include "Desktop/FileDialog.hpp"
#include "Desktop/PrintDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextEditor.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class Notepad : public gui::Frame {
 public:
  static constexpr uint64_t kMaxFileBytes = gui::TextEditor::kDefaultMaxLength;

  explicit Notepad(allocators::IAllocator& allocator);

  // The filesystem files are opened from and saved to (nullptr: none). It must outlive the editor.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }

  // The dialog that Open and Save As use (nullptr: none, they then say so). It must outlive the editor.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  // The dialog that Print uses (nullptr: none, Print then says so). It must outlive the editor.
  void set_print_dialog(PrintDialog* dialog) { print_dialog_ = dialog; }
  // The dialog that warnings, information and errors are shown in (nullptr: the status line says them instead). It must outlive the editor.
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }

  // Starts an empty, unnamed document. False (and nothing changes) while there are unsaved changes the user has not
  // agreed to lose.
  bool new_document();
  // Loads the file at `path` into the editor. False if it cannot be read (an error dialog says why) or unsaved changes
  // are in the way.
  bool open_file(StringView path);
  // Writes the text to `path` (the file it came from when empty; with neither, the Save As dialog opens and this returns false).
  // False if that fails.
  bool save_file(StringView path = StringView());
  // The commands behind File > Open and File > Save As: they show the dialog and carry on when a file is picked.
  void ask_open();
  void ask_save_as();
  // File > Print: the print dialog, then the text goes to the chosen printer.
  void ask_print();

  StringView path() const { return path_.view(); }
  gui::TextEditor& editor() { return editor_; }
  gui::MenuBar& menu_bar() { return menu_; }
  gui::Label& status_label() { return status_; }
  gui::Label& position_label() { return position_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_save_as(void* self, gui::Control&);
  static void on_print(void* self, gui::Control&);
  static void on_print_picked(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_save_picked(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_cut(void* self, gui::Control&);
  static void on_copy(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_select_all(void* self, gui::Control&);
  static void on_edited(void* self, gui::Control&);
  static void on_moved(void* self, gui::Control&);
  bool may_discard();
  bool read_into_editor(StringView path);
  bool write_from_editor(StringView path);
  void say(StringView text) { status_.set_text(text); }
  // Warnings, information and errors go to the notice dialog (or the status line when there is none to show it in).
  void notify(gui::NoticeKind kind, StringView text) {
    if (!gui::NoticeDialog::post(notices_, manager(), kind, text)) say(text);
  }
  void update_title();
  void update_position();
  void update_menus();

  gui::MenuBar menu_;
  gui::Panel status_bar_;
  gui::Label status_, position_;
  gui::MenuItem *cut_item_ = nullptr, *copy_item_ = nullptr;
  gui::TextEditor editor_;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  PrintDialog* print_dialog_ = nullptr;
  gui::NoticeDialog* notices_ = nullptr;
  String path_;  // the file the text came from or was saved to (empty: none yet)
  String name_;  // its name as shown in the title
  bool discard_armed_ = false;
};

}  // namespace desktop
