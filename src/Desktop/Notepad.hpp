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

// The text editor application: a window with a menu bar (File: New, Open, Save, Save As, Print, Exit; Edit: Undo, Redo, Cut, Copy, Paste, Delete,
// Select All; Help: About Text Editor), a TextEditor and a status bar showing the caret position and what the last command did. Warnings, information
// and errors appear in the shared NoticeDialog. Open and Save As pick the file in the shared FileDialog; Save writes to the file the text came from
// (or asks like Save As when there is none). Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+Shift+S and Ctrl+P do the same as the menu items. A command that cannot
// work right now is disabled in the menu (nothing selected to cut, nothing to undo, nothing to print), and its key then does nothing.
// The title is "Document name - Text Editor"; a new document is "Document 1", "Document 2"... New, Open and closing the window ask
// Save / Don't Save / Cancel first when there are unsaved changes (see DocumentFrame). Files are read and written as plain bytes; a CR before
// a line break is dropped on opening, so saved files always use LF.
#pragma once
#include "Desktop/DocumentFrame.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/PrintDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextEditor.hpp"

namespace desktop {

class Notepad : public DocumentFrame {
 public:
  static constexpr uint64_t kMaxFileBytes = gui::TextEditor::kDefaultMaxLength;

  explicit Notepad(allocators::IAllocator& allocator);

  // The filesystem files are opened from and saved to (nullptr: none). It must outlive the editor.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }

  // The dialog that Open and Save As use (nullptr: none, the commands are then disabled). It must outlive the editor.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  // The dialog that Print uses (nullptr: none, Print is then disabled). It must outlive the editor.
  void set_print_dialog(PrintDialog* dialog) { print_dialog_ = dialog; }

  // Starts an empty, unnamed document. False (and nothing changes yet) while there are unsaved changes: the question about them is then on
  // the screen and the new document starts when the answer allows it.
  bool new_document();
  // Loads the file at `path` into the editor. False if it cannot be read (an error dialog says why) or unsaved changes are in the way (as above).
  bool open_file(StringView path);
  // Writes the text to `path` (the file it came from when empty; with neither, the Save As dialog opens and this returns false).
  // False if that fails.
  bool save_file(StringView path = StringView());
  // The commands behind File > Open and File > Save As: they show the dialog and carry on when a file is picked.
  void ask_open();
  void ask_save_as();
  // File > Print: the print dialog, then the text goes to the chosen printer.
  void ask_print();

  gui::TextEditor& editor() { return editor_; }
  gui::MenuBar& menu_bar() { return menu_; }
  gui::Label& status_label() { return status_; }
  gui::Label& position_label() { return position_; }

  // A file dragged here from a file explorer is opened (just like File > Open).
  bool accepts_drop(StringView text, int32_t, int32_t, const gui::Control*) const override { return fs_ && !text.empty() && text[0] == '/'; }
  void on_drop(StringView text, int32_t, int32_t, const gui::Control*) override { open_file(text); }
  bool on_key(const gui::KeyArgs& args) override;

 protected:
  bool document_modified() const override { return editor_.modified(); }
  bool save_document() override { return save_file(); }
  void carry_out(Next next, StringView path) override;
  void say(StringView text) override { status_.set_text(text); }

 private:
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_save_as(void* self, gui::Control&);
  static void on_print(void* self, gui::Control&);
  static void on_print_picked(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_save_picked(void* self, gui::Control&);
  static void on_exit(void* self, gui::Control&);
  static void on_undo(void* self, gui::Control&);
  static void on_redo(void* self, gui::Control&);
  static void on_cut(void* self, gui::Control&);
  static void on_copy(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_delete(void* self, gui::Control&);
  static void on_select_all(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_edited(void* self, gui::Control&);
  static void on_moved(void* self, gui::Control&);
  static void on_menu_opening(void* self, gui::Control&);
  void start_new();
  void show_open_dialog();
  bool read_into_editor(StringView path);
  bool write_from_editor(StringView path);
  void update_position();
  void update_menus();
  // What the commands need to be able to work (the menu items are enabled by the same rules).
  bool can_open() const { return dialog_ && fs_; }
  bool can_save() const { return fs_ && (!path().empty() || dialog_); }
  bool can_print() const { return print_dialog_ && !editor_.text().empty(); }

  gui::MenuBar menu_;
  gui::Panel status_bar_;
  gui::Label status_, position_;
  gui::MenuItem *open_item_ = nullptr, *save_item_ = nullptr, *save_as_item_ = nullptr, *print_item_ = nullptr;
  gui::MenuItem *undo_item_ = nullptr, *redo_item_ = nullptr, *cut_item_ = nullptr, *copy_item_ = nullptr, *paste_item_ = nullptr, *delete_item_ = nullptr;
  gui::TextEditor editor_;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  PrintDialog* print_dialog_ = nullptr;
};

}  // namespace desktop
