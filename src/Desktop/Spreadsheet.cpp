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

#include "Desktop/Spreadsheet.hpp"
#include "Desktop/FileStatus.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Kernel/Clipboard.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

using namespace gui;
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

const FileDialog::Filter kFilters[] = {{"CSV files", "csv;txt;tsv"}, {"All files", "*"}};

}  // namespace

bool Spreadsheet::is_sheet_name(StringView name) {
  if (name.size() < 5) return false;
  const StringView ext = name.substr(name.size() - 4);
  return ext[0] == '.' && (ext[1] | 0x20) == 'c' && (ext[2] | 0x20) == 's' && (ext[3] | 0x20) == 'v';
}

Spreadsheet::Spreadsheet(allocators::IAllocator& allocator)
    : Frame(allocator),
      sheet_(allocator),
      view_(sheet_),
      menu_(allocator),
      formula_panel_(allocator),
      status_bar_(allocator),
      name_box_(allocator),
      status_(allocator),
      summary_(allocator),
      formula_(allocator),
      grid_(allocator),
      path_(allocator),
      name_(allocator) {
  set_bounds({100, 50, 640, 420});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New", {&Spreadsheet::on_new, this}, "Ctrl+N");
    file->add_item("Open...", {&Spreadsheet::on_open, this}, "Ctrl+O");
    file->add_item("Save", {&Spreadsheet::on_save, this}, "Ctrl+S");
    file->add_item("Save As...", {&Spreadsheet::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    file->add_item("Close", {&Spreadsheet::on_close, this});
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    edit->add_item("Cut", {&Spreadsheet::on_cut, this}, "Ctrl+X");
    edit->add_item("Copy", {&Spreadsheet::on_copy, this}, "Ctrl+C");
    edit->add_item("Paste", {&Spreadsheet::on_paste, this}, "Ctrl+V");
    edit->add_item("Delete", {&Spreadsheet::on_delete, this}, "Del");
    edit->add_separator();
    edit->add_item("Select All", {&Spreadsheet::on_select_all, this}, "Ctrl+A");
  }

  formula_panel_.set_style(PanelStyle::Transparent);
  formula_panel_.set_dock(DockStyle::Top);
  formula_panel_.set_size(0, 26);
  formula_panel_.set_padding(Insets::all(1));
  name_box_.set_dock(DockStyle::Left);
  name_box_.set_size(64, 0);
  formula_.set_dock(DockStyle::Fill);
  formula_.on_changed(Handler::immediate(&Spreadsheet::on_formula_changed, this));
  formula_panel_.add(&name_box_);
  formula_panel_.add(&formula_);

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  summary_.set_dock(DockStyle::Right);
  summary_.set_size(260, 0);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&summary_);
  status_bar_.add(&status_);

  grid_.set_dock(DockStyle::Fill);
  grid_.set_model(&view_);
  grid_.on_selection_changed(Handler::immediate(&Spreadsheet::on_selection, this));
  grid_.on_edit_requested(Handler::immediate(&Spreadsheet::on_edit_requested, this));
  grid_.on_clear_requested(Handler::immediate(&Spreadsheet::on_delete, this));
  grid_.on_copy_requested(Handler::immediate(&Spreadsheet::on_copy, this));
  grid_.on_cut_requested(Handler::immediate(&Spreadsheet::on_cut, this));
  grid_.on_paste_requested(Handler::immediate(&Spreadsheet::on_paste, this));

  add(&menu_);
  add(&formula_panel_);
  add(&status_bar_);
  add(&grid_);  // Fill goes last
  show_active();
  update_title();
  update_summary();
  say("Type to fill a cell; formulas start with =");
}

void Spreadsheet::update_title() {
  String title(allocator());
  title.assign("Spreadsheet - ");
  title.append(name_.empty() ? StringView("Untitled") : name_.view());
  if (sheet_.modified()) title.append(" *");
  set_title(title.view());
}

void Spreadsheet::show_active() {
  String address(allocator());
  spreadsheet::Sheet::column_name(grid_.active_column(), &address);
  address.append_uint(grid_.active_row() + 1);
  name_box_.set_text(address.view());
  if (!editing_) {
    updating_ = true;
    formula_.set_text(sheet_.text(grid_.active_column(), grid_.active_row()));
    updating_ = false;
  }
}

// The sums of the numbers among the selected cells, when more than one cell is selected.
void Spreadsheet::update_summary() {
  if (grid_.single_cell_selected()) {
    summary_.set_text(StringView());
    return;
  }
  double sum = 0;
  uint32_t count = 0;
  for (uint32_t r = grid_.selection_top(); r <= grid_.selection_bottom(); ++r) {
    for (uint32_t c = grid_.selection_left(); c <= grid_.selection_right(); ++c) {
      double d;
      if (sheet_.number(c, r, &d)) {
        sum += d;
        ++count;
      }
    }
  }
  String text(allocator()), number(allocator());
  text.append("Count: ");
  text.append_uint(count);
  if (count) {
    spreadsheet::Sheet::format_number(sum, &number);
    text.append("   Sum: ");
    text.append(number.view());
    spreadsheet::Sheet::format_number(sum / count, &number);
    text.append("   Avg: ");
    text.append(number.view());
  }
  summary_.set_text(text.view());
}

// The cells changed: everything that shows them is brought up to date.
void Spreadsheet::changed() {
  grid_.model_changed();
  show_active();
  update_summary();
  update_title();
}

void Spreadsheet::on_selection(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  // Moving on to another cell while typing keeps what was typed (in the cell it was typed in).
  if (s->editing_ && (s->grid_.active_column() != s->edit_column_ || s->grid_.active_row() != s->edit_row_)) {
    s->editing_ = false;
    s->grid_.stop_editing();
    s->sheet_.set_text(s->edit_column_, s->edit_row_, s->formula_.text());
    s->discard_armed_ = false;
    s->changed();
    return;
  }
  s->show_active();
  s->update_summary();
}

void Spreadsheet::begin_edit(char first) {
  if (editing_) {   // an edit is already open (the focus came back to the grid): the character goes on the end of it
    if (first) {
      String text(allocator());
      text.assign(formula_.text());
      text.push_back(first);
      updating_ = true;
      formula_.set_text(text.view());
      updating_ = false;
      grid_.set_edit_text(formula_.text());
    }
    if (Window* w = window()) w->set_focus(&formula_);
    return;
  }
  edit_column_ = grid_.active_column();
  edit_row_ = grid_.active_row();
  editing_ = true;
  updating_ = true;
  if (first) formula_.set_text(StringView(&first, 1));
  else formula_.set_text(sheet_.text(edit_column_, edit_row_));
  updating_ = false;
  grid_.set_edit_text(formula_.text());
  if (Window* w = window()) w->set_focus(&formula_);
}

void Spreadsheet::on_edit_requested(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  s->begin_edit(s->grid_.edit_request_char());
}

// The formula bar changed: when the user did it, an edit has started.
void Spreadsheet::on_formula_changed(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  if (s->updating_) return;
  if (!s->editing_) {
    s->edit_column_ = s->grid_.active_column();
    s->edit_row_ = s->grid_.active_row();
    s->editing_ = true;
  }
  s->grid_.set_edit_text(s->formula_.text());
}

bool Spreadsheet::commit_edit() {
  if (!editing_) return false;
  editing_ = false;
  grid_.stop_editing();
  sheet_.set_text(edit_column_, edit_row_, formula_.text());
  discard_armed_ = false;
  changed();
  return true;
}

void Spreadsheet::cancel_edit() {
  if (!editing_) return;
  editing_ = false;
  grid_.stop_editing();
  show_active();
}

void Spreadsheet::on_new(void* self, Control&) { static_cast<Spreadsheet*>(self)->new_document(); }
void Spreadsheet::on_open(void* self, Control&) { static_cast<Spreadsheet*>(self)->ask_open(); }
void Spreadsheet::on_save(void* self, Control&) { static_cast<Spreadsheet*>(self)->save_file(); }
void Spreadsheet::on_save_as(void* self, Control&) { static_cast<Spreadsheet*>(self)->ask_save_as(); }
void Spreadsheet::on_close(void* self, Control&) { static_cast<Spreadsheet*>(self)->close(); }
void Spreadsheet::on_cut(void* self, Control&) { static_cast<Spreadsheet*>(self)->cut_selection(); }
void Spreadsheet::on_copy(void* self, Control&) { static_cast<Spreadsheet*>(self)->copy_selection(); }
void Spreadsheet::on_paste(void* self, Control&) { static_cast<Spreadsheet*>(self)->paste(); }
void Spreadsheet::on_delete(void* self, Control&) { static_cast<Spreadsheet*>(self)->clear_selection(); }
void Spreadsheet::on_select_all(void* self, Control&) { static_cast<Spreadsheet*>(self)->grid_.select_all(); }

// --- Clipboard ------------------------------------------------------------------------------------------------------------------------------------------

void Spreadsheet::copy_selection() {
  if (editing_) {   // text being typed: the formula bar's own copy
    formula_.copy_selection();
    return;
  }
  kernel::Clipboard* clip = clipboard();
  if (!clip) {
    say("There is no clipboard.");
    return;
  }
  String text(allocator());
  for (uint32_t r = grid_.selection_top(); r <= grid_.selection_bottom(); ++r) {
    for (uint32_t c = grid_.selection_left(); c <= grid_.selection_right(); ++c) {
      if (c > grid_.selection_left()) text.push_back('\t');
      const StringView cell = sheet_.text(c, r);
      for (uint64_t i = 0; i < cell.size(); ++i) text.push_back(cell[i] == '\t' || cell[i] == '\n' || cell[i] == '\r' ? ' ' : cell[i]);
    }
    text.push_back('\n');
  }
  if (grid_.single_cell_selected() && sheet_.text(grid_.active_column(), grid_.active_row()).empty()) {
    say("Nothing to copy: the cell is empty.");
    return;
  }
  if (clip->set_text(text.view())) {
    copy_column_ = grid_.selection_left();
    copy_row_ = grid_.selection_top();
    copy_sequence_ = clip->sequence();
    say("Copied.");
  }
}

void Spreadsheet::cut_selection() {
  if (editing_) {
    formula_.cut_selection();
    return;
  }
  copy_selection();
  clear_selection();
}

void Spreadsheet::clear_selection() {
  if (editing_) {
    cancel_edit();
    return;
  }
  sheet_.clear_range(grid_.selection_left(), grid_.selection_top(), grid_.selection_right(), grid_.selection_bottom());
  discard_armed_ = false;
  changed();
}

void Spreadsheet::paste() {
  if (editing_) {
    formula_.paste();
    return;
  }
  kernel::Clipboard* clip = clipboard();
  String text(allocator());
  if (!clip || !clip->get_text(&text) || text.empty()) {
    say("Nothing to paste.");
    return;
  }
  const bool ours = copy_sequence_ != 0 && clip->sequence() == copy_sequence_;   // still the cells copied here: formulas move along
  const int32_t dc = static_cast<int32_t>(grid_.active_column()) - static_cast<int32_t>(copy_column_);
  const int32_t dr = static_cast<int32_t>(grid_.active_row()) - static_cast<int32_t>(copy_row_);
  uint32_t row = grid_.active_row(), column = grid_.active_column(), max_column = column, last_row = row;
  String cell(allocator()), shifted(allocator());
  uint64_t i = 0;
  const uint64_t n = text.size();
  bool any = false;
  auto put = [&]() {
    if (column < spreadsheet::kColumns && row < spreadsheet::kRows) {
      StringView value = cell.view();
      if (ours && value.size() > 1 && value[0] == '=' && spreadsheet::Sheet::shift_references(value, dc, dr, &shifted)) value = shifted.view();
      sheet_.set_text(column, row, value);
      if (column > max_column) max_column = column;
      last_row = row;
      any = true;
    }
    cell.clear();
  };
  while (i < n) {
    const char ch = text[i];
    if (ch == '\t') {
      put();
      ++column;
    } else if (ch == '\n' || ch == '\r') {
      if (ch == '\r' && i + 1 < n && text[i + 1] == '\n') ++i;
      put();
      column = grid_.active_column();
      ++row;
      if (i + 1 >= n) {
        row = last_row + 1;   // (a final line break does not start another row)
        break;
      }
    } else {
      cell.push_back(ch);
    }
    ++i;
  }
  if (!cell.empty()) put();
  discard_armed_ = false;
  changed();
  if (any) {
    grid_.select(grid_.active_column(), grid_.active_row(), false);
    grid_.select(max_column, last_row, true);
  }
  say("Pasted.");
}

// --- Files ----------------------------------------------------------------------------------------------------------------------------------------------

bool Spreadsheet::may_discard() {
  if (!sheet_.modified() || discard_armed_) {
    discard_armed_ = false;
    return true;
  }
  discard_armed_ = true;
  notify(gui::NoticeKind::Warning, "Unsaved changes - repeat the command to discard them, or press Save.");
  return false;
}

bool Spreadsheet::new_document() {
  commit_edit();
  if (!may_discard()) return false;
  sheet_.clear();
  sheet_.mark_saved();
  path_.clear();
  name_.clear();
  grid_.select(0, 0);
  changed();
  say("New spreadsheet.");
  return true;
}

void Spreadsheet::ask_open() {
  if (!dialog_ || !manager()) {
    notify(gui::NoticeKind::Error, "No file dialog available.");
    return;
  }
  commit_edit();
  if (!may_discard()) return;
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kFilters, 2, path_.view(), {&Spreadsheet::on_open_picked, this}))
    notify(gui::NoticeKind::Warning, "The file dialog is busy.");
}

void Spreadsheet::ask_save_as() {
  if (!dialog_ || !manager()) {
    notify(gui::NoticeKind::Error, "No file dialog available.");
    return;
  }
  commit_edit();
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kFilters, 2, path_.view(), {&Spreadsheet::on_save_picked, this}))
    notify(gui::NoticeKind::Warning, "The file dialog is busy.");
}

void Spreadsheet::on_open_picked(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  if (s->dialog_->accepted()) s->read_into_sheet(s->dialog_->selected_path());   // the unsaved changes were agreed to before the dialog opened
}

void Spreadsheet::on_save_picked(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  if (s->dialog_->accepted()) s->write_from_sheet(s->dialog_->selected_path());
}

bool Spreadsheet::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) {
    notify(gui::NoticeKind::Warning, "Enter the path of a file to open.");
    return false;
  }
  if (!fs_) {
    notify(gui::NoticeKind::Error, "No storage available.");
    return false;
  }
  commit_edit();
  if (!may_discard()) return false;
  return read_into_sheet(path.view());
}

bool Spreadsheet::read_into_sheet(StringView path) {
  if (!fs_) {
    notify(gui::NoticeKind::Error, "No storage available.");
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
    notify(gui::NoticeKind::Error, message.view());
    return false;
  }
  const uint64_t size = file->size();
  if (size > kMaxFileBytes) {
    notify(gui::NoticeKind::Error, "The file is too large to open (limit 8 MiB).");
    return false;
  }
  ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator(), size ? size : 1);
  uint64_t got = 0;
  status = bytes.empty() ? Status::OutOfMemory : size ? file->read(0, bytes.first(size), &got) : Status::Ok;
  file.reset();
  String text(allocator());
  bool ok = status == Status::Ok && text.reserve(got);
  uint64_t start = 0;
  if (ok && got >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) start = 3;   // a byte order mark from another program
  for (uint64_t i = start; ok && i < got; ++i) ok = text.push_back(static_cast<char>(bytes[i]));
  release_view(allocator(), bytes);
  if (!ok) {
    notify(gui::NoticeKind::Error, status == Status::Ok ? "Out of memory." : "The file could not be read.");
    return false;
  }
  uint32_t dropped = 0;
  if (!sheet_.load_csv(text.view(), &dropped)) {
    notify(gui::NoticeKind::Error, "Out of memory.");
    return false;
  }
  path_.assign(path);
  const uint64_t slash = path.rfind('/');
  name_.assign(slash == StringView::npos ? path : path.substr(slash + 1));
  discard_armed_ = false;
  grid_.select(0, 0);
  changed();
  String message(allocator());
  message.append("Opened ");
  message.append(path);
  if (dropped) {
    message.append(" (");
    message.append_uint(dropped);
    message.append(" cells beyond column AZ or row 1000 were left out)");
    notify(gui::NoticeKind::Warning, message.view());
  }
  say(message.view());
  return true;
}

bool Spreadsheet::save_file(StringView requested) {
  commit_edit();
  String path(allocator());
  if (!normalize(requested.empty() ? path_.view() : requested, &path) || path.empty()) {
    if (dialog_ && manager()) ask_save_as();   // nowhere to save to yet: the same as Save As
    else notify(gui::NoticeKind::Warning, "Enter a path to save to.");
    return false;
  }
  return write_from_sheet(path.view());
}

bool Spreadsheet::write_from_sheet(StringView path) {
  if (!fs_) {
    notify(gui::NoticeKind::Error, "No storage available.");
    return false;
  }
  String csv(allocator());
  if (!sheet_.save_csv(&csv)) {
    notify(gui::NoticeKind::Error, "Out of memory.");
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  if (status == Status::Ok && !csv.empty()) status = file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(csv.data()), csv.size()), &written);
  if (status == Status::Ok) status = file->flush();
  file.reset();
  String message(allocator());
  if (status != Status::Ok) {
    message.append("Cannot save ");
    message.append(path);
    message.append(": ");
    message.append(describe_status(status));
    notify(gui::NoticeKind::Error, message.view());
    return false;
  }
  fs_->flush();
  path_.assign(path);
  const uint64_t slash = path.rfind('/');
  name_.assign(slash == StringView::npos ? path : path.substr(slash + 1));
  sheet_.mark_saved();
  discard_armed_ = false;
  update_title();
  message.append("Saved ");
  message.append(path);
  say(message.view());
  return true;
}

// --- Keys -----------------------------------------------------------------------------------------------------------------------------------------------

bool Spreadsheet::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  if (args.modifiers & hardware::modifier::kControl) {
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
  // Keys of the formula bar that the text box passes up: they end the edit.
  Window* w = window();
  if (w && w->focus() == &formula_) {
    if (args.usage == key::kEnter || args.usage == key::kTab) {
      const uint32_t column = editing_ ? edit_column_ : grid_.active_column(), row = editing_ ? edit_row_ : grid_.active_row();
      const bool shift = args.modifiers & hardware::modifier::kShift;
      commit_edit();
      w->set_focus(&grid_);
      if (args.usage == key::kEnter) grid_.select(column, shift && row > 0 ? row - 1 : row + 1);
      else grid_.select(shift && column > 0 ? column - 1 : column + 1, row);
      return true;
    }
    if (args.usage == key::kEscape) {
      cancel_edit();
      w->set_focus(&grid_);
      return true;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
