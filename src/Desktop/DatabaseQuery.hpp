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

// The database query application: a window to connect to a database server, type SQL and see what comes back. A bar at the top takes the server's
// URL ("postgres://user@host/database", see database::parse_database_url) and the password; Connect (or Disconnect) opens (or ends) the session. Under it
// is the SQL editor and under that the answer of the last statement in a table that sorts by column and shows NULLs as NULL; the status bar says what the
// statement did and how long it took. F5 or Ctrl+R (and Query > Run) run the selected text, or all of it when nothing is selected; the statement
// runs on the handler thread, so the desktop stays alive while the server thinks. Parameters are not offered here: what is typed is sent as it is.
//
// File: New, Open, Save, Save As (the SQL as plain text), Export Results (the table as a spreadsheet file), Exit. Edit: Undo, Redo, Cut, Copy, Paste,
// Delete, Select All, and Copy Results (the table as tab separated text). Query: Run, Connect, Disconnect. Help: About Database. Commands that cannot
// work right now (Run without a connection, Export without a table) are disabled. The title is "Query name - Database" (a new one is "Query 1",
// "Query 2"...), and New, Open and closing the window ask Save / Don't Save / Cancel first when the SQL has unsaved changes (see DocumentFrame).
// Which databases can be reached depends on the drivers in the DatabaseManager it is given.
#pragma once
#include "Database/DatabaseManager.hpp"
#include "Database/Local/SystemSettings.hpp"
#include "Desktop/DocumentFrame.hpp"
#include "Desktop/FileDialog.hpp"
#include "Desktop/StackPanel.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/TextBox.hpp"
#include "Gui/Controls/TextEditor.hpp"

namespace desktop {

class DatabaseQuery : public DocumentFrame {
 public:
  static constexpr uint64_t kMaxFileBytes = gui::TextEditor::kDefaultMaxLength;

  explicit DatabaseQuery(allocators::IAllocator& allocator);
  ~DatabaseQuery();

  // The databases that can be connected to (nullptr: none, Connect is then disabled). It must outlive the window.
  void set_databases(database::DatabaseManager* databases) { databases_ = databases; update_connection_state(); }
  // The system settings (may be nullptr): the URL of the last database connected to (never with a password) is kept there and shown the next time the
  // window opens; with none kept yet the box starts with `default_url`.
  void set_settings(database::local::SystemSettings* settings, StringView default_url = StringView());
  // The filesystem SQL files are opened from and saved to (nullptr: none).
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  // Statements are timed with this clock (microseconds of uptime); without one the status line shows no time.
  void set_clock(const uint64_t* now_us) { clock_ = now_us; }

  // Does the file name say it is SQL (a .sql)?
  static bool is_sql_name(StringView name);

  // --- The connection
  // Connects to the URL in the URL box (the password box fills in a password the URL does not have). False (and the status line or a dialog says
  // why) when it fails. Any session that was open is ended first.
  bool connect();
  void disconnect();
  bool connected() const { return connection_ && connection_->is_open(); }
  database::IConnection* connection() { return connection_.get(); }

  // --- Statements
  // Runs the selected text, or all of the editor's text. False if there is nothing to run, no connection, or the statement failed.
  bool run();
  // The answer of the last statement (nullptr: none, or it failed).
  database::IResultSet* result() { return result_.get(); }

  // --- SQL files
  bool new_document();
  bool open_file(StringView path);
  bool save_file(StringView path = StringView());
  void ask_open();
  void ask_save_as();
  void ask_export();
  // Writes the table as CSV (the column names first) to `path`.
  bool export_csv(StringView path);
  // The table as tab separated text on the clipboard.
  bool copy_results();

  gui::TextEditor& editor() { return editor_; }
  gui::DataGrid& grid() { return grid_; }
  gui::TextBox& url_box() { return url_; }
  gui::TextBox& password_box() { return password_; }
  gui::Button& connect_button() { return connect_button_; }
  gui::Label& status_label() { return status_; }
  gui::Label& info_label() { return info_; }
  gui::MenuBar& menu_bar() { return menu_; }
  // The menu of the table of results: Copy Results, Export Results.
  gui::ContextMenu& results_menu() { return results_menu_; }

  bool accepts_drop(StringView text, int32_t, int32_t, const gui::Control*) const override { return fs_ && !text.empty() && text[0] == '/'; }
  void on_drop(StringView text, int32_t, int32_t, const gui::Control*) override { open_file(text); }
  bool on_key(const gui::KeyArgs& args) override;

 protected:
  void manager_changed() override;
  bool document_modified() const override { return editor_.modified(); }
  bool save_document() override { return save_file(); }
  void carry_out(Next next, StringView path) override;
  void say(StringView text) override { status_.set_text(text); }

 private:
  // The table's rows, read from the result when the grid asks.
  class ResultModel final : public gui::IGridModel {
   public:
    void set(const database::IResultSet* result) { result_ = result; }
    uint32_t row_count() const override;
    bool cell_text(uint32_t row, uint32_t column, String* out) const override;

   private:
    const database::IResultSet* result_ = nullptr;
  };

  static void on_undo(void* self, gui::Control&);
  static void on_redo(void* self, gui::Control&);
  static void on_delete(void* self, gui::Control&);
  static void on_about(void* self, gui::Control&);
  static void on_menu_opening(void* self, gui::Control&);
  void start_new();
  void show_open_dialog();
  bool load_file(StringView path);
  void update_menus();
  bool can_open() const { return dialog_ && fs_; }
  bool can_save() const { return fs_ && (!path().empty() || dialog_); }
  bool has_table() const { return result_ && result_->column_count() > 0; }
  static void on_new(void* self, gui::Control&);
  static void on_open(void* self, gui::Control&);
  static void on_save(void* self, gui::Control&);
  static void on_save_as(void* self, gui::Control&);
  static void on_export(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  static void on_cut(void* self, gui::Control&);
  static void on_copy(void* self, gui::Control&);
  static void on_paste(void* self, gui::Control&);
  static void on_select_all(void* self, gui::Control&);
  static void on_copy_results(void* self, gui::Control&);
  static void on_run(void* self, gui::Control&);
  static void on_connect_clicked(void* self, gui::Control&);
  static void on_open_picked(void* self, gui::Control&);
  static void on_save_picked(void* self, gui::Control&);
  static void on_export_picked(void* self, gui::Control&);
  static void on_edited(void* self, gui::Control&);
  bool read_text(StringView path, String* out);
  bool write_text(StringView path, StringView text);
  void show_result();
  void update_connection_state();
  uint64_t now() const { return clock_ ? *clock_ : 0; }

  database::DatabaseManager* databases_ = nullptr;
  database::local::SystemSettings* settings_ = nullptr;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  const uint64_t* clock_ = nullptr;
  UniquePointer<database::IConnection> connection_;
  UniquePointer<database::IResultSet> result_;
  ResultModel model_;
  gui::MenuBar menu_;
  gui::ContextMenu results_menu_;
  gui::MenuItem *connect_item_ = nullptr, *disconnect_item_ = nullptr, *run_item_ = nullptr;
  gui::MenuItem *open_item_ = nullptr, *save_item_ = nullptr, *save_as_item_ = nullptr, *export_item_ = nullptr, *undo_item_ = nullptr, *redo_item_ = nullptr;
  gui::MenuItem *cut_item_ = nullptr, *copy_item_ = nullptr, *paste_item_ = nullptr, *delete_item_ = nullptr, *copy_results_item_ = nullptr;
  gui::MenuItem *ctx_copy_results_ = nullptr, *ctx_export_ = nullptr;
  StackRow connection_bar_;   // (a row in reading order, so that Tab goes server, password, Connect)
  gui::Panel status_bar_;
  gui::Label url_label_, password_label_, status_, info_;
  gui::TextBox url_, password_;
  gui::Button connect_button_;
  gui::TextEditor editor_;
  gui::DataGrid grid_;
  String server_;   // "host/database" of the open session, for the messages
  gui::Handler run_handler_;
  bool running_ = false;
};

}  // namespace desktop
