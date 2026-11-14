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

#include "Desktop/DatabaseQuery.hpp"
#include "Database/ConnectionOptions.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

using namespace gui;
using database::DbStatus;
using database::DbType;
using filesystem::IFile;
using filesystem::OpenMode;
using filesystem::Status;

namespace {

// The text with a leading '/' and no surrounding blanks.
bool normalize(StringView in, String* out) {
  uint64_t from = 0, to = in.size();
  while (from < to && in[from] == ' ') ++from;
  while (to > from && in[to - 1] == ' ') --to;
  out->clear();
  if (from == to) return true;
  if (in[from] != '/' && !out->push_back('/')) return false;
  return out->append(in.substr(from, to - from));
}

const FileDialog::Filter kSqlFilters[] = {{"Queries", "sql;txt"}, {"All Files", "*"}};
const FileDialog::Filter kCsvFilters[] = {{"Spreadsheets", "csv"}, {"All Files", "*"}};

uint32_t g_queries = 0;   // numbers the new queries: "Query 1", "Query 2"...

bool is_number_type(DbType t) { return t == DbType::Integer || t == DbType::Real || t == DbType::Decimal; }

int32_t width_for(DbType t) {
  switch (t) {
    case DbType::Boolean: return 60;
    case DbType::Integer: return 80;
    case DbType::Real:
    case DbType::Decimal:
    case DbType::Date:
    case DbType::Time: return 110;
    case DbType::Timestamp:
    case DbType::Uuid: return 190;
    default: return 140;
  }
}
}  // namespace

bool DatabaseQuery::is_sql_name(StringView name) {
  if (name.size() < 5) return false;
  const StringView ext = name.substr(name.size() - 4);
  return ext[0] == '.' && (ext[1] | 0x20) == 's' && (ext[2] | 0x20) == 'q' && (ext[3] | 0x20) == 'l';
}

uint32_t DatabaseQuery::ResultModel::row_count() const {
  if (!result_) return 0;
  const uint64_t n = result_->row_count();
  return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(n);
}

bool DatabaseQuery::ResultModel::cell_text(uint32_t row, uint32_t column, String* out) const {
  out->clear();
  if (!result_) return true;
  if (result_->is_null(row, column)) return out->assign("NULL");
  return out->assign(result_->text(row, column));
}

DatabaseQuery::DatabaseQuery(allocators::IAllocator& allocator)
    : DocumentFrame(allocator, "Database", "Query", &g_queries),
      menu_(allocator),
      results_menu_(allocator),
      connection_bar_(allocator),
      status_bar_(allocator),
      url_label_(allocator),
      password_label_(allocator),
      status_(allocator),
      info_(allocator),
      url_(allocator),
      password_(allocator),
      connect_button_(allocator),
      editor_(allocator),
      grid_(allocator),
      server_(allocator) {
  set_bounds({110, 60, 680, 480});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);
  run_handler_ = {&DatabaseQuery::on_run, this};

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  menu_.on_opening(Handler::immediate(&DatabaseQuery::on_menu_opening, this));
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New", {&DatabaseQuery::on_new, this}, "Ctrl+N");
    open_item_ = file->add_item("Open...", {&DatabaseQuery::on_open, this}, "Ctrl+O");
    save_item_ = file->add_item("Save", {&DatabaseQuery::on_save, this}, "Ctrl+S");
    save_as_item_ = file->add_item("Save As...", {&DatabaseQuery::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    export_item_ = file->add_item("Export Results...", {&DatabaseQuery::on_export, this});
    file->add_separator();
    file->add_item("Exit", {&DatabaseQuery::on_close, this});   // (the query and the session go with the window, so this is Exit and not Close)
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    undo_item_ = edit->add_item("Undo", {&DatabaseQuery::on_undo, this}, "Ctrl+Z");
    redo_item_ = edit->add_item("Redo", {&DatabaseQuery::on_redo, this}, "Ctrl+Y");
    edit->add_separator();
    cut_item_ = edit->add_item("Cut", {&DatabaseQuery::on_cut, this}, "Ctrl+X");
    copy_item_ = edit->add_item("Copy", {&DatabaseQuery::on_copy, this}, "Ctrl+C");
    paste_item_ = edit->add_item("Paste", {&DatabaseQuery::on_paste, this}, "Ctrl+V");
    delete_item_ = edit->add_item("Delete", {&DatabaseQuery::on_delete, this}, "Del");
    edit->add_separator();
    edit->add_item("Select All", {&DatabaseQuery::on_select_all, this}, "Ctrl+A");
    edit->add_separator();
    copy_results_item_ = edit->add_item("Copy Results", {&DatabaseQuery::on_copy_results, this});
  }
  if (Menu* query = menu_.add_menu("Query")) {
    run_item_ = query->add_item("Run", {&DatabaseQuery::on_run, this}, "F5");
    query->add_separator();
    connect_item_ = query->add_item("Connect", {&DatabaseQuery::on_connect_clicked, this});
    disconnect_item_ = query->add_item("Disconnect", {&DatabaseQuery::on_connect_clicked, this});
  }
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About Database", {&DatabaseQuery::on_about, this});

  // The connection bar, in reading order (which is also the order of Tab): the server, the password, Connect. Each label ends in a colon and comes
  // directly before its box, so its access key moves the focus there.
  connection_bar_.set_dock(DockStyle::Top);
  connection_bar_.set_size(0, 28);
  connection_bar_.set_padding(Insets::all(1));
  url_label_.set_text("Server:");
  url_label_.set_size(60, 0);
  url_.set_dock(DockStyle::Fill);
  password_label_.set_text("Password:");
  password_label_.set_size(84, 0);
  password_.set_password(true);
  password_.set_size(130, 0);
  connect_button_.set_text("Connect");
  connect_button_.set_size(92, 0);
  connect_button_.on_click({&DatabaseQuery::on_connect_clicked, this});
  connection_bar_.add(&url_label_);
  connection_bar_.add(&url_);
  connection_bar_.add(&password_label_);
  connection_bar_.add(&password_);
  connection_bar_.add(&connect_button_);

  editor_.set_dock(DockStyle::Top);
  editor_.set_size(0, 130);
  editor_.on_changed(Handler::immediate(&DatabaseQuery::on_edited, this));

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  info_.set_dock(DockStyle::Right);
  info_.set_size(260, 0);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&info_);
  status_bar_.add(&status_);

  grid_.set_dock(DockStyle::Fill);
  // The table's own menu (the same commands as in the Edit and File menus).
  ctx_copy_results_ = results_menu_.add_item("Copy Results", {&DatabaseQuery::on_copy_results, this});
  ctx_export_ = results_menu_.add_item("Export Results...", {&DatabaseQuery::on_export, this});
  results_menu_.on_opening(Handler::immediate(&DatabaseQuery::on_menu_opening, this));
  grid_.set_context_menu(&results_menu_);

  add(&menu_);
  add(&connection_bar_);
  add(&editor_);
  add(&status_bar_);
  add(&grid_);  // Fill goes last
  update_connection_state();
  say("Enter the server, choose Connect, type a statement and press F5.");
}

DatabaseQuery::~DatabaseQuery() { connection_.reset(); }

void DatabaseQuery::manager_changed() {
  if (!manager()) disconnect();   // the window was closed: the session ends with it
}

// The items that cannot work right now are disabled instead of answering with a message.
void DatabaseQuery::update_menus() {
  const kernel::Clipboard* clip = clipboard();
  const bool on = connected();
  if (connect_item_) connect_item_->set_enabled(!on && databases_);
  if (disconnect_item_) disconnect_item_->set_enabled(on);
  if (run_item_) run_item_->set_enabled(on && !running_);
  if (open_item_) open_item_->set_enabled(can_open());
  if (save_item_) save_item_->set_enabled(can_save());
  if (save_as_item_) save_as_item_->set_enabled(can_open());
  if (export_item_) export_item_->set_enabled(can_open() && has_table());
  if (ctx_export_) ctx_export_->set_enabled(can_open() && has_table());
  if (copy_results_item_) copy_results_item_->set_enabled(has_table() && clip);
  if (ctx_copy_results_) ctx_copy_results_->set_enabled(has_table() && clip);
  if (undo_item_) undo_item_->set_enabled(editor_.can_undo());
  if (redo_item_) redo_item_->set_enabled(editor_.can_redo());
  if (cut_item_) cut_item_->set_enabled(editor_.has_selection());
  if (copy_item_) copy_item_->set_enabled(editor_.has_selection());
  if (delete_item_) delete_item_->set_enabled(editor_.has_selection());
  if (paste_item_) paste_item_->set_enabled(clip && clip->has_text());
}

void DatabaseQuery::update_connection_state() {
  const bool on = connected();
  connect_button_.set_text(on ? "Disconnect" : "Connect");
  connect_button_.set_enabled(on || databases_);
  if (!on) server_.clear();
  url_.set_read_only(on);   // (the box shows where the session is connected to for as long as it lasts)
  update_menus();
}

void DatabaseQuery::on_edited(void* self, Control&) { static_cast<DatabaseQuery*>(self)->update_menus(); }
void DatabaseQuery::on_menu_opening(void* self, Control&) { static_cast<DatabaseQuery*>(self)->update_menus(); }
void DatabaseQuery::on_undo(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.undo(); }
void DatabaseQuery::on_redo(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.redo(); }
void DatabaseQuery::on_delete(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.delete_selection(); }
void DatabaseQuery::on_about(void* self, Control&) {
  static_cast<DatabaseQuery*>(self)->show_about("Database connects to a database server, runs statements and shows what comes back. It is part of TheoryCraftOS.");
}

void DatabaseQuery::on_new(void* self, Control&) { static_cast<DatabaseQuery*>(self)->new_document(); }
void DatabaseQuery::on_open(void* self, Control&) { static_cast<DatabaseQuery*>(self)->ask_open(); }
void DatabaseQuery::on_save(void* self, Control&) { static_cast<DatabaseQuery*>(self)->save_file(); }
void DatabaseQuery::on_save_as(void* self, Control&) { static_cast<DatabaseQuery*>(self)->ask_save_as(); }
void DatabaseQuery::on_export(void* self, Control&) { static_cast<DatabaseQuery*>(self)->ask_export(); }
void DatabaseQuery::on_close(void* self, Control&) { static_cast<DatabaseQuery*>(self)->exit_requested(); }
void DatabaseQuery::on_cut(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.cut_selection(); }
void DatabaseQuery::on_copy(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.copy_selection(); }
void DatabaseQuery::on_paste(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.paste(); }
void DatabaseQuery::on_select_all(void* self, Control&) { static_cast<DatabaseQuery*>(self)->editor_.select_all(); }
void DatabaseQuery::on_copy_results(void* self, Control&) { static_cast<DatabaseQuery*>(self)->copy_results(); }
void DatabaseQuery::on_run(void* self, Control&) { static_cast<DatabaseQuery*>(self)->run(); }

void DatabaseQuery::on_connect_clicked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (q->connected()) q->disconnect();
  else q->connect();
}

void DatabaseQuery::set_settings(database::local::SystemSettings* settings, StringView default_url) {
  settings_ = settings;
  if (!url_.text().empty()) return;
  String url(allocator());
  if (!settings || !settings->get_text("database-query", "url", &url)) url.assign(default_url);
  if (!url.empty()) url_.set_text(url.view());
}

// --- Connection -----------------------------------------------------------------------------------------------------------------------------------------

bool DatabaseQuery::connect() {
  if (!databases_) {   // (Connect is disabled then)
    say("No database drivers are installed, so no database can be connected.");
    return false;
  }
  database::ConnectionOptions options(allocator());
  database::DbError error(allocator());
  String url(allocator());
  url.assign(url_.text());
  if (!database::parse_database_url(url.view(), &options, &error)) {
    // The entry is at fault: the message says what is wrong with it and the focus goes to the box.
    notify(NoticeKind::Error, "The server's address cannot be used: %1. An address looks like \"postgres://user@host/database\".", error.message.view());
    if (Window* w = window()) w->set_focus(&url_);
    return false;
  }
  const bool url_has_password = !options.password.empty();
  if (options.password.empty() && !password_.text().empty()) options.password.assign(password_.text());
  disconnect();
  say("Connecting...");
  const DbStatus status = databases_->connect(options, &connection_, &error);
  String where(allocator());
  where.assign(options.host.view());
  if (!options.database.empty()) {
    where.push_back('/');
    where.append(options.database.view());
  }
  if (status != DbStatus::Ok || !connection_) {
    // What the server (or the driver) gave as the reason, with its reference code at the end.
    String reason(allocator());
    const StringView said = error.message.empty() ? StringView(database::db_status_name(status)) : error.message.view();
    if (error.sqlstate.empty()) reason.assign(said);
    else format_message(&reason, "%1 (%2)", said, error.sqlstate.view());
    connection_.reset();
    update_connection_state();
    say("Not connected.");
    notify(NoticeKind::Error, "\"%1\" could not be connected. Check the address, the user name and the password. The answer was: %2", where.view(), reason.view());
    return false;
  }
  server_.assign(where.view());
  update_connection_state();
  if (settings_ && !url_has_password) settings_->set_text("database-query", "url", url.view());   // the next window starts here
  String message(allocator());
  if (connection_->server_version().empty()) format_message(&message, "Connected to %1.", server_.view());
  else format_message(&message, connection_->encrypted() ? "Connected to %1 (server %2, encrypted)." : "Connected to %1 (server %2).", server_.view(), connection_->server_version());
  say(message.view());
  return true;
}

void DatabaseQuery::disconnect() {
  if (!connection_) return;
  connection_.reset();
  update_connection_state();
  say("Disconnected.");
}

// --- Statements -----------------------------------------------------------------------------------------------------------------------------------------

void DatabaseQuery::show_result() {
  UiGuard lock(this);
  grid_.set_model(nullptr);   // (the old table's text is still read until this)
  grid_.clear_columns();
  model_.set(result_.get());
  if (result_ && result_->column_count()) {
    for (uint32_t c = 0; c < result_->column_count(); ++c)
      grid_.add_column(result_->column_name(c), width_for(result_->column_type(c)), is_number_type(result_->column_type(c)) ? Align::Right : Align::Left);
    grid_.set_model(&model_);
  }
}

bool DatabaseQuery::run() {
  if (running_) {
    say("A statement is still running.");
    return false;
  }
  if (!connected()) {   // (Run is disabled in the menu; this answers F5)
    update_connection_state();
    notify(NoticeKind::Information, "No database is connected, so the statement was not run. Enter the server, choose Connect, and run it again.");
    return false;
  }
  String sql(allocator());
  {
    UiGuard lock(this);
    const StringView all = editor_.text();
    sql.assign(editor_.has_selection() ? all.substr(editor_.selection_start(), editor_.selection_end() - editor_.selection_start()) : all);
  }
  uint64_t from = 0, to = sql.size();
  while (from < to && (sql[from] == ' ' || sql[from] == '\t' || sql[from] == '\n' || sql[from] == '\r')) ++from;
  while (to > from && (sql[to - 1] == ' ' || sql[to - 1] == '\t' || sql[to - 1] == '\n' || sql[to - 1] == '\r')) --to;
  if (from == to) {
    say("There is no statement to run. Type one first.");
    return false;
  }
  running_ = true;
  say("Running...");
  UniquePointer<database::IResultSet> fresh;
  const uint64_t started = now();
  const DbStatus status = connection_->execute(sql.view().substr(from, to - from), &fresh);
  const uint64_t elapsed_ms = (now() - started) / 1000;
  running_ = false;
  if (status != DbStatus::Ok) {
    {
      UiGuard lock(this);
      grid_.set_model(nullptr);
      result_.reset();
      show_result();
    }
    // What the server gave as the reason: its message, its reference code, and the detail when it sent one.
    const database::DbError& error = connection_->last_error();
    const StringView said = error.message.empty() ? StringView(database::db_status_name(status)) : error.message.view();
    String reason(allocator());
    if (error.sqlstate.empty()) reason.assign(said);
    else format_message(&reason, "%1 (%2)", said, error.sqlstate.view());
    update_connection_state();   // (the connection may have ended with it)
    info_.set_text(StringView());
    say("The statement was not run.");
    if (error.detail.empty()) notify(NoticeKind::Error, "The statement could not be run. Correct it and run it again. The server answered: %1", reason.view());
    else {
      String message(allocator());
      format_message(&message, "The statement could not be run. Correct it and run it again. The server answered: %1: %2", reason.view(), error.detail.view());
      notify(NoticeKind::Error, message.view());
    }
    return false;
  }
  {
    UiGuard lock(this);
    grid_.set_model(nullptr);
    result_ = util::move(fresh);
    show_result();
  }
  // The status bar: what the statement did, and beside it how many rows and how long (as "name: value" pairs, which read the same for one row and many).
  String text(allocator()), count(allocator()), time(allocator());
  time.append_uint(elapsed_ms);
  if (result_ && result_->column_count()) {
    count.append_uint(result_->row_count());
    format_message(&text, clock_ ? "Rows: %1   Time: %2 ms" : "Rows: %1", count.view(), time.view());
    say(result_->command());
  } else {
    if (result_ && !result_->command().empty()) say(result_->command());
    else say("Done.");
    if (result_) {
      count.append_uint(result_->rows_affected());
      format_message(&text, clock_ ? "Rows changed: %1   Time: %2 ms" : "Rows changed: %1", count.view(), time.view());
    } else if (clock_) {
      format_message(&text, "Time: %1 ms", time.view());
    }
  }
  info_.set_text(text.view());
  update_menus();
  return true;
}

namespace {
void append_csv_field(String* out, StringView v) {
  bool quote = false;
  for (uint64_t i = 0; i < v.size(); ++i)
    if (v[i] == ',' || v[i] == '"' || v[i] == '\n' || v[i] == '\r') quote = true;
  if (!quote) {
    out->append(v);
    return;
  }
  out->push_back('"');
  for (uint64_t i = 0; i < v.size(); ++i) {
    if (v[i] == '"') out->push_back('"');
    out->push_back(v[i]);
  }
  out->push_back('"');
}
}  // namespace

bool DatabaseQuery::copy_results() {
  kernel::Clipboard* clip = clipboard();
  if (!has_table() || !clip) return false;   // (the menu items are disabled then)
  String text(allocator());
  for (uint32_t c = 0; c < result_->column_count(); ++c) {
    if (c) text.push_back('\t');
    text.append(result_->column_name(c));
  }
  text.push_back('\n');
  for (uint64_t r = 0; r < result_->row_count(); ++r) {
    for (uint32_t c = 0; c < result_->column_count(); ++c) {
      if (c) text.push_back('\t');
      const StringView v = result_->text(r, c);
      for (uint64_t i = 0; i < v.size(); ++i) text.push_back(v[i] == '\t' || v[i] == '\n' || v[i] == '\r' ? ' ' : v[i]);
    }
    text.push_back('\n');
  }
  if (!clip->set_text(text.view())) {
    notify(NoticeKind::Error, "The results are too large for the clipboard, so nothing was copied. Use Export Results in the File menu to save them in a file.");
    return false;
  }
  say("Copied the results.");
  return true;
}

bool DatabaseQuery::export_csv(StringView requested) {
  String path(allocator());
  if (!has_table() || !normalize(requested, &path) || path.empty()) return false;   // (Export Results is disabled without a table)
  String csv(allocator());
  for (uint32_t c = 0; c < result_->column_count(); ++c) {
    if (c) csv.push_back(',');
    append_csv_field(&csv, result_->column_name(c));
  }
  csv.push_back('\n');
  for (uint64_t r = 0; r < result_->row_count(); ++r) {
    for (uint32_t c = 0; c < result_->column_count(); ++c) {
      if (c) csv.push_back(',');
      append_csv_field(&csv, result_->text(r, c));   // (NULL is an empty field)
    }
    csv.push_back('\n');
  }
  if (!write_text(path.view(), csv.view())) return false;
  String message(allocator());
  format_message(&message, "Exported the results to \"%1\".", name_of(path.view()));
  say(message.view());
  return true;
}

// --- SQL files ------------------------------------------------------------------------------------------------------------------------------------------

void DatabaseQuery::carry_out(Next next, StringView path) {
  if (next == Next::New) start_new();
  else if (next == Next::Open) show_open_dialog();
  else if (next == Next::OpenFile) load_file(path);
}

void DatabaseQuery::start_new() {
  editor_.set_text(StringView());
  editor_.mark_saved();
  name_new_document();
  update_menus();
  say("New query.");
}

bool DatabaseQuery::new_document() {
  if (!ready_for(Next::New)) return false;
  start_new();
  return true;
}

bool DatabaseQuery::read_text(StringView path, String* out) {
  if (!fs_) {
    say("There is no disk to open files from.");
    return false;
  }
  const StringView name = name_of(path);
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::Read, &file);
  if (status != Status::Ok) {
    notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status, false));
    return false;
  }
  const uint64_t size = file->size();
  if (size > kMaxFileBytes) {
    notify(NoticeKind::Error, "\"%1\" is too large for Database, which opens queries of up to 1 MB. Open it with another program.", name);
    return false;
  }
  ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator(), size ? size : 1);
  uint64_t got = 0;
  status = bytes.empty() ? Status::OutOfMemory : size ? file->read(0, bytes.first(size), &got) : Status::Ok;
  file.reset();
  out->clear();
  bool ok = status == Status::Ok && out->reserve(got);
  for (uint64_t i = 0; ok && i < got; ++i)
    if (bytes[i] != '\r') ok = out->push_back(static_cast<char>(bytes[i]));
  release_view(allocator(), bytes);
  if (!ok) notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status == Status::Ok ? Status::OutOfMemory : status, false));
  return ok;
}

bool DatabaseQuery::write_text(StringView path, StringView text) {
  if (!fs_) {
    say("There is no disk to save files on.");
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  if (status == Status::Ok && !text.empty()) status = file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &written);
  if (status == Status::Ok) status = file->flush();
  file.reset();
  if (status != Status::Ok) {
    notify(NoticeKind::Error, "\"%1\" could not be saved. %2", name_of(path), file_advice(status, true));
    return false;
  }
  fs_->flush();
  return true;
}

void DatabaseQuery::ask_open() {
  if (!can_open() || !manager()) return;   // (the menu item is disabled then)
  if (ready_for(Next::Open)) show_open_dialog();
}

void DatabaseQuery::show_open_dialog() {
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kDocuments, ".sql", false, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kSqlFilters, 2, start.view(), {&DatabaseQuery::on_open_picked, this}))
    say("The Open dialog is already open for another window.");
}

void DatabaseQuery::ask_save_as() {
  if (!can_open() || !manager()) {
    save_abandoned();
    return;
  }
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kDocuments, ".sql", true, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kSqlFilters, 2, start.view(), {&DatabaseQuery::on_save_picked, this})) {
    save_abandoned();
    say("The Save As dialog is already open for another window.");
  }
}

void DatabaseQuery::ask_export() {
  if (!can_open() || !manager() || !has_table()) return;   // (the menu item is disabled then)
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Export Results", kCsvFilters, 2, StringView(), {&DatabaseQuery::on_export_picked, this}))
    say("The Export Results dialog is already open for another window.");
}

void DatabaseQuery::on_open_picked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (q->dialog_->accepted()) q->load_file(q->dialog_->selected_path());   // (the unsaved changes were dealt with before the dialog opened)
}

void DatabaseQuery::on_save_picked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (q->dialog_->accepted()) q->save_file(q->dialog_->selected_path());
  else q->save_abandoned();
}

void DatabaseQuery::on_export_picked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (q->dialog_->accepted()) q->export_csv(q->dialog_->selected_path());
}

bool DatabaseQuery::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) return false;
  if (!ready_for(Next::OpenFile, path.view())) return false;
  return load_file(path.view());
}

bool DatabaseQuery::load_file(StringView path) {
  String text(allocator());
  if (!read_text(path, &text)) return false;
  editor_.set_text(text.view());
  editor_.mark_saved();
  name_document(path);
  update_menus();
  String message(allocator());
  format_message(&message, "Opened \"%1\".", document_name());
  say(message.view());
  return true;
}

bool DatabaseQuery::save_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested.empty() ? this->path() : requested, &path) || path.empty()) {
    ask_save_as();   // nowhere to save to yet: the same as Save As
    return false;
  }
  if (!write_text(path.view(), editor_.text())) {
    save_abandoned();
    return false;
  }
  name_document(path.view());
  editor_.mark_saved();
  update_menus();
  String message(allocator());
  format_message(&message, "Saved \"%1\".", document_name());
  say(message.view());
  document_saved();   // (what waited for the save - New, Open, closing - goes ahead)
  return true;
}

// --- Keys -----------------------------------------------------------------------------------------------------------------------------------------------

bool DatabaseQuery::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  const bool control = args.modifiers & hardware::modifier::kControl;
  if (args.usage == key::kF1 + 4 || (control && args.usage == key::kA + 17)) {   // F5, Ctrl+R
    fire(run_handler_);   // (the statement waits for the server: that is not for the input thread)
    return true;
  }
  if (control) {
    switch (args.usage) {
      case key::kA + 13: new_document(); return true;  // N
      case key::kA + 14: ask_open(); return true;      // O
      case key::kA + 18:                               // S
        if (args.modifiers & hardware::modifier::kShift) ask_save_as();
        else if (can_save()) save_file();
        return true;
      default: break;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
