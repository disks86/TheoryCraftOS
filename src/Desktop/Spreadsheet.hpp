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

// The spreadsheet application: a window with a menu bar (File: New, Open, Save, Save As, Exit; Edit: Undo, Redo, Cut, Copy, Paste, Delete, Select All;
// Help: About Spreadsheet), a formula bar
// (the active cell's address and the text of the cell, which is where cells are edited), the grid of cells and a status bar with the sums of the selected
// numbers. Typing in the grid starts an edit in the formula bar (the text shows in the cell as well); Enter keeps it and moves down, Tab keeps it and moves
// right, Escape drops it; so does nothing but clicking another cell keep it. Cells hold numbers, text and formulas (see spreadsheet::Sheet). Sheets are
// saved and opened as CSV files. Ctrl+N, Ctrl+O, Ctrl+S and Ctrl+Shift+S do what the menu items do. The title is "Sheet name - Spreadsheet" (a new one is
// "Sheet 1", "Sheet 2"...), and New, Open and closing the window ask Save / Don't Save / Cancel first when there are unsaved changes (see DocumentFrame).
// Copy puts the selected cells on the clipboard as tab separated text; pasting that into the sheet again moves the cell references of copied formulas along.
// Every change to the cells (an entry, Delete, Cut, Paste) can be taken back with Undo (Ctrl+Z) and made again with Redo (Ctrl+Y): the sheet as it was is
// remembered before each one, up to kMaxUndoSteps of them within kMaxUndoBytes. A right click on a cell (Shift+F10, the Menu key) opens the cells' menu.
#pragma once
#include "Desktop/DocumentFrame.hpp"
#include "Desktop/FileDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/SpreadsheetGrid.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "ObjectContainer/List.hpp"
#include "Spreadsheet/Sheet.hpp"

namespace desktop {

class Spreadsheet : public DocumentFrame {
 public:
  static constexpr uint64_t kMaxFileBytes = 8u << 20;
  static constexpr uint32_t kMaxUndoSteps = 32;
  static constexpr uint64_t kMaxUndoBytes = 4u << 20;

  explicit Spreadsheet(allocators::IAllocator& allocator);

  // Does the file name say it is a sheet (a .csv)?
  static bool is_sheet_name(StringView name);

  // The filesystem files are opened from and saved to (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  // The dialog that Open and Save As use (nullptr: none, the commands are then disabled). It must outlive the window.
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }

  // Starts an empty, unnamed sheet. False (and nothing changes yet) while there are unsaved changes: the question about them is then on the screen
  // and the new sheet starts when the answer allows it.
  bool new_document();
  // Loads the CSV file at `path`. False if it cannot be read (an error dialog says why) or unsaved changes are in the way.
  bool open_file(StringView path);
  // Writes the sheet to `path` (the file it came from when empty; with neither, the Save As dialog opens and this returns false).
  bool save_file(StringView path = StringView());
  void ask_open();
  void ask_save_as();

  spreadsheet::Sheet& sheet() { return sheet_; }
  gui::SpreadsheetGrid& grid() { return grid_; }
  gui::TextBox& formula_bar() { return formula_; }
  gui::Label& name_label() { return name_box_; }
  gui::Label& status_label() { return status_; }
  gui::Label& summary_label() { return summary_; }
  gui::MenuBar& menu_bar() { return menu_; }
  // The menu of the cells: Cut, Copy, Paste, Delete, Select All.
  gui::ContextMenu& cell_menu() { return cell_menu_; }
  // True while text is being typed into a cell and not yet kept.
  bool editing() const { return editing_; }

  // A file dragged here from a file explorer is opened (just like File > Open).
  bool accepts_drop(StringView text, int32_t, int32_t, const gui::Control*) const override { return fs_ && !text.empty() && text[0] == '/'; }
  void on_drop(StringView text, int32_t, int32_t, const gui::Control*) override { open_file(text); }
  bool on_key(const gui::KeyArgs& args) override;

  // The edits as commands (what the menu does).
  void copy_selection();
  void cut_selection();
  void paste();
  void clear_selection();
  // Keeps the text being typed in the active cell. Returns false when nothing was being edited.
  bool commit_edit();
  // Drops the text being typed.
  void cancel_edit();
  // Takes back the last change to the cells / makes it again. False when there is none.
  bool undo();
  bool redo();
  bool can_undo() const { return undo_.count() > 0; }
  bool can_redo() const { return redo_.count() > 0; }

 protected:
  bool document_modified() const override { return sheet_.modified() || restored_; }
  bool save_document() override { return save_file(); }
  void carry_out(Next next, StringView path) override;
  void say(StringView text) override { status_.set_text(text); }

 private:
  class View final : public gui::ISheetView {
   public:
    explicit View(spreadsheet::Sheet& sheet) : sheet_(sheet) {}
    uint32_t column_count() const override { return spreadsheet::kColumns; }
    uint32_t row_count() const override { return spreadsheet::kRows; }
    bool cell_text(uint32_t column, uint32_t row, String* out, bool* right) override { return sheet_.display(column, row, out, right); }

   private:
    spreadsheet::Sheet& sheet_;
  };

  // The grid, with the one thing a context menu needs: a right click on a cell outside the selection selects that cell first.
  class Cells final : public gui::SpreadsheetGrid {
   public:
    using gui::SpreadsheetGrid::SpreadsheetGrid;
    void prepare_context_menu(int32_t x, int32_t y) override {
      uint32_t column, row;
      if (!cell_at(x, y, &column, &row)) return;
      if (column < selection_left() || column > selection_right() || row < selection_top() || row > selection_bottom()) select(column, row);
    }
  };
  // The sheet as it was before a change.
  struct Snapshot {
    explicit Snapshot(allocators::IAllocator& allocator) : csv(allocator) {}
    String csv;
    uint32_t column = 0, row = 0;
  };

  static void on_undo(void* self, gui::Control&);
  static void on_redo(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_menu_opening(void* self, gui::Control&);
  bool remember(List<Snapshot>* history);
  void remember_for_undo();
  bool swap_with(List<Snapshot>* from, List<Snapshot>* to);
  void forget_history();
  void start_new();
  void show_open_dialog();
  void update_menus();
  bool can_open() const { return dialog_ && fs_; }
  bool can_save() const { return fs_ && (!path().empty() || dialog_); }
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_save_as(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_cut(void* self, gui::Control&);
  static void on_copy(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_delete(void* self, gui::Control&);
  static void on_select_all(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_save_picked(void* self, gui::Control&);
  static void on_selection(void* self, gui::Control&);
  static void on_edit_requested(void* self, gui::Control&);
  static void on_formula_changed(void* self, gui::Control&);
  void begin_edit(char first);
  bool read_into_sheet(StringView path);
  bool write_from_sheet(StringView path);
  void changed();                 // the sheet's cells changed: redraw, title, summary
  void show_active();             // the address and the text of the active cell
  void update_summary();

  spreadsheet::Sheet sheet_;
  View view_;
  gui::MenuBar menu_;
  gui::Panel formula_panel_, status_bar_;
  gui::Label name_box_, status_, summary_;
  gui::TextBox formula_;
  Cells grid_;
  gui::ContextMenu cell_menu_;
  gui::MenuItem *open_item_ = nullptr, *save_item_ = nullptr, *save_as_item_ = nullptr, *undo_item_ = nullptr, *redo_item_ = nullptr, *paste_item_ = nullptr, *ctx_paste_ = nullptr;
  List<Snapshot> undo_, redo_;
  bool restored_ = false;   // an undo or redo put back cells that differ from the file (the sheet itself only knows about its own edits)
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  bool editing_ = false, updating_ = false;
  uint32_t edit_column_ = 0, edit_row_ = 0;
  // The cells last copied: where they came from, and the clipboard's count then (to know that what is on it is still ours).
  uint32_t copy_column_ = 0, copy_row_ = 0;
  uint64_t copy_sequence_ = 0;
};

}  // namespace desktop
