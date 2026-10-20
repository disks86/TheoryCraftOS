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
#include "Desktop/FileStatus.hpp"
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

const FileDialog::Filter kSqlFilters[] = {{"SQL files", "sql;txt"}, {"All files", "*"}};
const FileDialog::Filter kCsvFilters[] = {{"CSV files", "csv"}, {"All files", "*"}};

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
    : Frame(allocator),
      menu_(allocator),
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
      path_(allocator),
      name_(allocator),
      server_(allocator) {
  set_bounds({110, 60, 680, 480});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);
  run_handler_ = {&DatabaseQuery::on_run, this};

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New Query", {&DatabaseQuery::on_new, this}, "Ctrl+N");
    file->add_item("Open SQL...", {&DatabaseQuery::on_open, this}, "Ctrl+O");
    file->add_item("Save SQL", {&DatabaseQuery::on_save, this}, "Ctrl+S");
    file->add_item("Save SQL As...", {&DatabaseQuery::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    file->add_item("Export Results as CSV...", {&DatabaseQuery::on_export, this});
    file->add_separator();
    file->add_item("Close", {&DatabaseQuery::on_close, this});
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    edit->add_item("Cut", {&DatabaseQuery::on_cut, this}, "Ctrl+X");
    edit->add_item("Copy", {&DatabaseQuery::on_copy, this}, "Ctrl+C");
    edit->add_item("Paste", {&DatabaseQuery::on_paste, this}, "Ctrl+V");
    edit->add_item("Select All", {&DatabaseQuery::on_select_all, this}, "Ctrl+A");
    edit->add_separator();
    edit->add_item("Copy Results", {&DatabaseQuery::on_copy_results, this});
  }
  if (Menu* query = menu_.add_menu("Query")) {
    run_item_ = query->add_item("Run", {&DatabaseQuery::on_run, this}, "F5 / Ctrl+R");
    query->add_separator();
    connect_item_ = query->add_item("Connect", {&DatabaseQuery::on_connect_clicked, this});
    disconnect_item_ = query->add_item("Disconnect", {&DatabaseQuery::on_connect_clicked, this});
  }

  connection_bar_.set_style(PanelStyle::Transparent);
  connection_bar_.set_dock(DockStyle::Top);
  connection_bar_.set_size(0, 28);
  connection_bar_.set_padding(Insets::all(1));
  url_label_.set_text("URL");
  url_label_.set_dock(DockStyle::Left);
  url_label_.set_size(36, 0);
  connect_button_.set_text("Connect");
  connect_button_.set_dock(DockStyle::Right);
  connect_button_.set_size(84, 0);
  connect_button_.on_click({&DatabaseQuery::on_connect_clicked, this});
  password_.set_password(true);
  password_.set_dock(DockStyle::Right);
  password_.set_size(130, 0);
  password_label_.set_text("Password");
  password_label_.set_dock(DockStyle::Right);
  password_label_.set_size(76, 0);
  url_.set_dock(DockStyle::Fill);
  connection_bar_.add(&url_label_);
  connection_bar_.add(&connect_button_);
  connection_bar_.add(&password_);
  connection_bar_.add(&password_label_);
  connection_bar_.add(&url_);

  editor_.set_dock(DockStyle::Top);
  editor_.set_size(0, 130);
  editor_.on_changed(Handler::immediate(&DatabaseQuery::on_edited, this));

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  info_.set_dock(DockStyle::Right);
  info_.set_size(200, 0);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&info_);
  status_bar_.add(&status_);

  grid_.set_dock(DockStyle::Fill);

  add(&menu_);
  add(&connection_bar_);
  add(&editor_);
  add(&status_bar_);
  add(&grid_);  // Fill goes last
  update_connection_state();
  say("Enter the server's URL, press Connect, type SQL and press F5.");
}

DatabaseQuery::~DatabaseQuery() { connection_.reset(); }

void DatabaseQuery::manager_changed() {
  if (!manager()) disconnect();   // the window was closed: the session ends with it
}

void DatabaseQuery::update_title() {
  String title(allocator());
  title.assign("Database - ");
  title.append(name_.empty() ? StringView("Untitled") : name_.view());
  if (editor_.modified()) title.append(" *");
  if (connected() && !server_.empty()) {
    title.append(" - ");
    title.append(server_.view());
  }
  set_title(title.view());
}

void DatabaseQuery::update_connection_state() {
  const bool on = connected();
  connect_button_.set_text(on ? "Disconnect" : "Connect");
  if (connect_item_) connect_item_->set_enabled(!on);
  if (disconnect_item_) disconnect_item_->set_enabled(on);
  if (run_item_) run_item_->set_enabled(on);
  if (!on) server_.clear();
  url_.set_read_only(on);
  update_title();
}

void DatabaseQuery::on_edited(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  q->discard_armed_ = false;
  q->update_title();
}

void DatabaseQuery::on_new(void* self, Control&) { static_cast<DatabaseQuery*>(self)->new_document(); }
void DatabaseQuery::on_open(void* self, Control&) { static_cast<DatabaseQuery*>(self)->ask_open(); }
void DatabaseQuery::on_save(void* self, Control&) { static_cast<DatabaseQuery*>(self)->save_file(); }
void DatabaseQuery::on_save_as(void* self, Control&) { static_cast<DatabaseQuery*>(self)->ask_save_as(); }
void DatabaseQuery::on_export(void* self, Control&) { static_cast<DatabaseQuery*>(self)->ask_export(); }
void DatabaseQuery::on_close(void* self, Control&) { static_cast<DatabaseQuery*>(self)->close(); }
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
  if (!databases_) {
    notify(NoticeKind::Error, "No databases are available on this system.");
    return false;
  }
  database::ConnectionOptions options(allocator());
  database::DbError error(allocator());
  String url(allocator());
  url.assign(url_.text());
  if (!database::parse_database_url(url.view(), &options, &error)) {
    String message(allocator());
    message.append("The URL is not valid: ");
    message.append(error.message.view());
    notify(NoticeKind::Warning, message.view());
    return false;
  }
  const bool url_has_password = !options.password.empty();
  if (options.password.empty() && !password_.text().empty()) options.password.assign(password_.text());
  disconnect();
  say("Connecting...");
  const DbStatus status = databases_->connect(options, &connection_, &error);
  if (status != DbStatus::Ok || !connection_) {
    String message(allocator());
    message.append("Cannot connect: ");
    message.append(error.message.empty() ? StringView(database::db_status_name(status)) : error.message.view());
    if (!error.sqlstate.empty()) {
      message.append(" (");
      message.append(error.sqlstate.view());
      message.append(")");
    }
    connection_.reset();
    update_connection_state();
    notify(NoticeKind::Error, message.view());
    return false;
  }
  server_.assign(options.host.view());
  if (!options.database.empty()) {
    server_.push_back('/');
    server_.append(options.database.view());
  }
  update_connection_state();
  if (settings_ && !url_has_password) settings_->set_text("database-query", "url", url.view());   // the next window starts here
  String message(allocator());
  message.append("Connected to ");
  message.append(server_.view());
  if (!connection_->server_version().empty()) {
    message.append(" (server ");
    message.append(connection_->server_version());
    message.append(connection_->encrypted() ? ", encrypted)" : ")");
  }
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
  if (!connected()) {
    update_connection_state();
    notify(NoticeKind::Warning, "Connect to a database first.");
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
    say("Nothing to run: type a statement first.");
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
    const database::DbError& error = connection_->last_error();
    String message(allocator());
    message.append(error.message.empty() ? StringView(database::db_status_name(status)) : error.message.view());
    if (!error.sqlstate.empty()) {
      message.append(" (");
      message.append(error.sqlstate.view());
      message.append(")");
    }
    if (!error.detail.empty()) {
      message.append(": ");
      message.append(error.detail.view());
    }
    update_connection_state();   // (the connection may have ended with it)
    info_.set_text(StringView());
    notify(NoticeKind::Error, message.view());
    return false;
  }
  {
    UiGuard lock(this);
    grid_.set_model(nullptr);
    result_ = util::move(fresh);
    show_result();
  }
  String text(allocator());
  if (result_ && result_->column_count()) {
    text.append_uint(result_->row_count());
    text.append(result_->row_count() == 1 ? " row" : " rows");
    say(result_->command());
  } else {
    if (result_ && !result_->command().empty()) say(result_->command());
    else say("Done.");
    if (result_) {
      text.append_uint(result_->rows_affected());
      text.append(result_->rows_affected() == 1 ? " row affected" : " rows affected");
    }
  }
  if (clock_) {
    text.append(", ");
    text.append_uint(elapsed_ms);
    text.append(" ms");
  }
  info_.set_text(text.view());
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
  if (!result_ || result_->column_count() == 0) {
    say("There is no table to copy.");
    return false;
  }
  if (!clip) {
    say("There is no clipboard.");
    return false;
  }
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
    say("The table is too large for the clipboard.");
    return false;
  }
  say("Copied the table.");
  return true;
}

bool DatabaseQuery::export_csv(StringView requested) {
  if (!result_ || result_->column_count() == 0) {
    notify(NoticeKind::Information, "There is no table to export: run a statement that returns rows.");
    return false;
  }
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) {
    notify(NoticeKind::Warning, "Enter a path to export to.");
    return false;
  }
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
  message.append("Exported ");
  message.append_uint(result_->row_count());
  message.append(" rows to ");
  message.append(path.view());
  say(message.view());
  return true;
}

// --- SQL files ------------------------------------------------------------------------------------------------------------------------------------------

bool DatabaseQuery::may_discard() {
  if (!editor_.modified() || discard_armed_) {
    discard_armed_ = false;
    return true;
  }
  discard_armed_ = true;
  notify(NoticeKind::Warning, "Unsaved changes - repeat the command to discard them, or press Save.");
  return false;
}

bool DatabaseQuery::new_document() {
  if (!may_discard()) return false;
  editor_.set_text(StringView());
  editor_.mark_saved();
  path_.clear();
  name_.clear();
  update_title();
  say("New query.");
  return true;
}

bool DatabaseQuery::read_text(StringView path, String* out) {
  if (!fs_) {
    notify(NoticeKind::Error, "No storage available.");
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::Read, &file);
  if (status != Status::Ok) {
    String message(allocator());
    message.append("Cannot open ");
    message.append(path);
    message.append(": ");
    message.append(describe_status(status));
    notify(NoticeKind::Error, message.view());
    return false;
  }
  const uint64_t size = file->size();
  if (size > kMaxFileBytes) {
    notify(NoticeKind::Error, "The file is too large to open (limit 1 MiB).");
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
  if (!ok) notify(NoticeKind::Error, status == Status::Ok ? "Out of memory." : "The file could not be read.");
  return ok;
}

bool DatabaseQuery::write_text(StringView path, StringView text) {
  if (!fs_) {
    notify(NoticeKind::Error, "No storage available.");
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  if (status == Status::Ok && !text.empty()) status = file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &written);
  if (status == Status::Ok) status = file->flush();
  file.reset();
  if (status != Status::Ok) {
    String message(allocator());
    message.append("Cannot save ");
    message.append(path);
    message.append(": ");
    message.append(describe_status(status));
    notify(NoticeKind::Error, message.view());
    return false;
  }
  fs_->flush();
  return true;
}

void DatabaseQuery::ask_open() {
  if (!dialog_ || !manager()) {
    notify(NoticeKind::Error, "No file dialog available.");
    return;
  }
  if (!may_discard()) return;
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kSqlFilters, 2, path_.view(), {&DatabaseQuery::on_open_picked, this}))
    notify(NoticeKind::Warning, "The file dialog is busy.");
}

void DatabaseQuery::ask_save_as() {
  if (!dialog_ || !manager()) {
    notify(NoticeKind::Error, "No file dialog available.");
    return;
  }
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kSqlFilters, 2, path_.view(), {&DatabaseQuery::on_save_picked, this}))
    notify(NoticeKind::Warning, "The file dialog is busy.");
}

void DatabaseQuery::ask_export() {
  if (!dialog_ || !manager()) {
    notify(NoticeKind::Error, "No file dialog available.");
    return;
  }
  if (!result_ || result_->column_count() == 0) {
    notify(NoticeKind::Information, "There is no table to export: run a statement that returns rows.");
    return;
  }
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Export Results", kCsvFilters, 2, StringView(), {&DatabaseQuery::on_export_picked, this}))
    notify(NoticeKind::Warning, "The file dialog is busy.");
}

void DatabaseQuery::on_open_picked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (!q->dialog_->accepted()) return;
  q->open_file(q->dialog_->selected_path());
}

void DatabaseQuery::on_save_picked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (q->dialog_->accepted()) q->save_file(q->dialog_->selected_path());
}

void DatabaseQuery::on_export_picked(void* self, Control&) {
  auto* q = static_cast<DatabaseQuery*>(self);
  if (q->dialog_->accepted()) q->export_csv(q->dialog_->selected_path());
}

bool DatabaseQuery::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) {
    notify(NoticeKind::Warning, "Enter the path of a file to open.");
    return false;
  }
  if (!may_discard()) return false;
  String text(allocator());
  if (!read_text(path.view(), &text)) return false;
  editor_.set_text(text.view());
  editor_.mark_saved();
  path_.assign(path.view());
  const uint64_t slash = path.view().rfind('/');
  name_.assign(slash == StringView::npos ? path.view() : path.view().substr(slash + 1));
  discard_armed_ = false;
  update_title();
  String message(allocator());
  message.append("Opened ");
  message.append(path.view());
  say(message.view());
  return true;
}

bool DatabaseQuery::save_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested.empty() ? path_.view() : requested, &path) || path.empty()) {
    if (dialog_ && manager()) ask_save_as();   // nowhere to save to yet: the same as Save As
    else notify(NoticeKind::Warning, "Enter a path to save to.");
    return false;
  }
  if (!write_text(path.view(), editor_.text())) return false;
  path_.assign(path.view());
  const uint64_t slash = path.view().rfind('/');
  name_.assign(slash == StringView::npos ? path.view() : path.view().substr(slash + 1));
  editor_.mark_saved();
  discard_armed_ = false;
  update_title();
  String message(allocator());
  message.append("Saved ");
  message.append(path.view());
  say(message.view());
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
        else save_file();
        return true;
      default: break;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
