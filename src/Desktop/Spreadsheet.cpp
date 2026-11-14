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
#include "FileSystem/SystemLayout.hpp"
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

const FileDialog::Filter kFilters[] = {{"Spreadsheets", "csv;txt;tsv"}, {"All Files", "*"}};

uint32_t g_sheets = 0;   // numbers the new sheets: "Sheet 1", "Sheet 2"...

}  // namespace

bool Spreadsheet::is_sheet_name(StringView name) {
  if (name.size() < 5) return false;
  const StringView ext = name.substr(name.size() - 4);
  return ext[0] == '.' && (ext[1] | 0x20) == 'c' && (ext[2] | 0x20) == 's' && (ext[3] | 0x20) == 'v';
}

Spreadsheet::Spreadsheet(allocators::IAllocator& allocator)
    : DocumentFrame(allocator, "Spreadsheet", "Sheet", &g_sheets),
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
      cell_menu_(allocator),
      undo_(allocator),
      redo_(allocator) {
  set_bounds({100, 50, 640, 420});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  menu_.on_opening(Handler::immediate(&Spreadsheet::on_menu_opening, this));
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New", {&Spreadsheet::on_new, this}, "Ctrl+N");
    open_item_ = file->add_item("Open...", {&Spreadsheet::on_open, this}, "Ctrl+O");
    save_item_ = file->add_item("Save", {&Spreadsheet::on_save, this}, "Ctrl+S");
    save_as_item_ = file->add_item("Save As...", {&Spreadsheet::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    file->add_item("Exit", {&Spreadsheet::on_close, this});   // (the sheet goes with its window, so this is Exit and not Close)
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    undo_item_ = edit->add_item("Undo", {&Spreadsheet::on_undo, this}, "Ctrl+Z");
    redo_item_ = edit->add_item("Redo", {&Spreadsheet::on_redo, this}, "Ctrl+Y");
    edit->add_separator();
    edit->add_item("Cut", {&Spreadsheet::on_cut, this}, "Ctrl+X");
    edit->add_item("Copy", {&Spreadsheet::on_copy, this}, "Ctrl+C");
    paste_item_ = edit->add_item("Paste", {&Spreadsheet::on_paste, this}, "Ctrl+V");
    edit->add_item("Delete", {&Spreadsheet::on_delete, this}, "Del");
    edit->add_separator();
    edit->add_item("Select All", {&Spreadsheet::on_select_all, this}, "Ctrl+A");
  }
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About Spreadsheet", {&Spreadsheet::on_about, this});

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
  summary_.set_size(330, 0);
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

  // The cells' own menu: the transfer commands, then the others (the same commands as in the Edit menu).
  cell_menu_.add_item("Cut", {&Spreadsheet::on_cut, this});
  cell_menu_.add_item("Copy", {&Spreadsheet::on_copy, this});
  ctx_paste_ = cell_menu_.add_item("Paste", {&Spreadsheet::on_paste, this});
  cell_menu_.add_item("Delete", {&Spreadsheet::on_delete, this});
  cell_menu_.add_separator();
  cell_menu_.add_item("Select All", {&Spreadsheet::on_select_all, this});
  cell_menu_.on_opening(Handler::immediate(&Spreadsheet::on_menu_opening, this));
  grid_.set_context_menu(&cell_menu_);

  add(&menu_);
  add(&formula_panel_);
  add(&status_bar_);
  add(&grid_);  // Fill goes last
  show_active();
  update_summary();
  update_menus();
  say("Type to fill a cell. A formula starts with an equals sign.");
}

// The items that cannot work right now are disabled instead of answering with a message.
void Spreadsheet::update_menus() {
  const kernel::Clipboard* clip = clipboard();
  const bool can_paste = clip && clip->has_text();
  if (open_item_) open_item_->set_enabled(can_open());
  if (save_item_) save_item_->set_enabled(can_save());
  if (save_as_item_) save_as_item_->set_enabled(can_open());
  if (undo_item_) undo_item_->set_enabled(editing_ || can_undo());   // (while a cell is being typed in, Undo drops what was typed)
  if (redo_item_) redo_item_->set_enabled(!editing_ && can_redo());
  if (paste_item_) paste_item_->set_enabled(can_paste);
  if (ctx_paste_) ctx_paste_->set_enabled(can_paste);
}

void Spreadsheet::on_menu_opening(void* self, Control&) { static_cast<Spreadsheet*>(self)->update_menus(); }
void Spreadsheet::on_undo(void* self, Control&) { static_cast<Spreadsheet*>(self)->undo(); }
void Spreadsheet::on_redo(void* self, Control&) { static_cast<Spreadsheet*>(self)->redo(); }
void Spreadsheet::on_about(void* self, Control&) {
  static_cast<Spreadsheet*>(self)->show_about("Spreadsheet works with tables of numbers, text and formulas. It is part of TheoryCraftOS.");
}

// --- Undo -----------------------------------------------------------------------------------------------------------------------------------------------

// The sheet as it is now, onto the end of `history`. The oldest states are forgotten while there are too many or they hold too much; one is always kept.
bool Spreadsheet::remember(List<Snapshot>* history) {
  Snapshot now(allocator());
  if (!sheet_.save_csv(&now.csv)) return false;
  now.column = grid_.active_column();
  now.row = grid_.active_row();
  if (!history->push_back(util::move(now))) return false;
  uint64_t bytes = 0;
  for (const Snapshot& s : history->view()) bytes += s.csv.size();
  while (history->count() > 1 && (history->count() > kMaxUndoSteps || bytes > kMaxUndoBytes)) {
    bytes -= (*history)[0].csv.size();
    history->remove_at(0);
  }
  return true;
}

// Called before every change to the cells: the change can then be taken back, and what was undone before it can no longer be made again.
void Spreadsheet::remember_for_undo() {
  remember(&undo_);
  redo_.clear();
}

void Spreadsheet::forget_history() {
  undo_.clear();
  redo_.clear();
  restored_ = false;
}

// Puts back the newest state of `from`, after remembering the present one in `to`.
bool Spreadsheet::swap_with(List<Snapshot>* from, List<Snapshot>* to) {
  if (from->count() == 0 || !remember(to)) return false;
  const Snapshot& back = (*from)[from->count() - 1];
  const uint32_t column = back.column, row = back.row;
  const bool ok = sheet_.load_csv(back.csv.view());
  from->remove_at(from->count() - 1);
  restored_ = true;
  grid_.select(column, row);
  changed();
  return ok;
}

bool Spreadsheet::undo() {
  if (editing_) {   // text being typed is not in the sheet yet: taking it back is dropping it
    cancel_edit();
    return true;
  }
  if (!swap_with(&undo_, &redo_)) return false;
  say("Took back the last change.");
  return true;
}

bool Spreadsheet::redo() {
  if (editing_ || !swap_with(&redo_, &undo_)) return false;
  say("Made the change again.");
  return true;
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
  String text(allocator()), counted(allocator()), total(allocator()), average(allocator());
  counted.append_uint(count);
  if (count) {
    spreadsheet::Sheet::format_number(sum, &total);
    spreadsheet::Sheet::format_number(sum / count, &average);
    format_message(&text, "Count: %1   Sum: %2   Average: %3", counted.view(), total.view(), average.view());
  } else {
    format_message(&text, "Count: %1", counted.view());
  }
  summary_.set_text(text.view());
}

// The cells changed: everything that shows them is brought up to date.
void Spreadsheet::changed() {
  grid_.model_changed();
  show_active();
  update_summary();
  update_menus();
}

void Spreadsheet::on_selection(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  // Moving on to another cell while typing keeps what was typed (in the cell it was typed in).
  if (s->editing_ && (s->grid_.active_column() != s->edit_column_ || s->grid_.active_row() != s->edit_row_)) {
    s->editing_ = false;
    s->grid_.stop_editing();
    if (!s->sheet_.text(s->edit_column_, s->edit_row_).equals(s->formula_.text())) s->remember_for_undo();
    s->sheet_.set_text(s->edit_column_, s->edit_row_, s->formula_.text());
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
  if (!sheet_.text(edit_column_, edit_row_).equals(formula_.text())) remember_for_undo();
  sheet_.set_text(edit_column_, edit_row_, formula_.text());
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
void Spreadsheet::on_close(void* self, Control&) { static_cast<Spreadsheet*>(self)->exit_requested(); }
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
  if (!clip) return;
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
    say("The cell is empty, so nothing was copied.");
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
  remember_for_undo();
  sheet_.clear_range(grid_.selection_left(), grid_.selection_top(), grid_.selection_right(), grid_.selection_bottom());
  changed();
}

void Spreadsheet::paste() {
  if (editing_) {
    formula_.paste();
    return;
  }
  kernel::Clipboard* clip = clipboard();
  String text(allocator());
  if (!clip || !clip->get_text(&text) || text.empty()) return;   // (the menu item is disabled then)
  remember_for_undo();
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
  changed();
  if (any) {
    grid_.select(grid_.active_column(), grid_.active_row(), false);
    grid_.select(max_column, last_row, true);
  }
  say("Pasted.");
}

// --- Files ----------------------------------------------------------------------------------------------------------------------------------------------

void Spreadsheet::carry_out(Next next, StringView path) {
  if (next == Next::New) start_new();
  else if (next == Next::Open) show_open_dialog();
  else if (next == Next::OpenFile) read_into_sheet(path);
}

void Spreadsheet::start_new() {
  cancel_edit();
  sheet_.clear();
  sheet_.mark_saved();
  forget_history();
  name_new_document();
  grid_.select(0, 0);
  changed();
  say("New sheet.");
}

bool Spreadsheet::new_document() {
  commit_edit();
  if (!ready_for(Next::New)) return false;
  start_new();
  return true;
}

void Spreadsheet::ask_open() {
  if (!can_open() || !manager()) return;   // (the menu item is disabled then)
  commit_edit();
  if (ready_for(Next::Open)) show_open_dialog();
}

void Spreadsheet::show_open_dialog() {
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kDocuments, ".csv", false, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kFilters, 2, start.view(), {&Spreadsheet::on_open_picked, this}))
    say("The Open dialog is already open for another window.");
}

void Spreadsheet::ask_save_as() {
  if (!can_open() || !manager()) {
    save_abandoned();
    return;
  }
  commit_edit();
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kDocuments, ".csv", true, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kFilters, 2, start.view(), {&Spreadsheet::on_save_picked, this})) {
    save_abandoned();
    say("The Save As dialog is already open for another window.");
  }
}

void Spreadsheet::on_open_picked(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  if (s->dialog_->accepted()) s->read_into_sheet(s->dialog_->selected_path());   // (the unsaved changes were dealt with before the dialog opened)
}

void Spreadsheet::on_save_picked(void* self, Control&) {
  auto* s = static_cast<Spreadsheet*>(self);
  if (s->dialog_->accepted()) s->write_from_sheet(s->dialog_->selected_path());
  else s->save_abandoned();
}

bool Spreadsheet::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) return false;
  if (!fs_) {
    say("There is no disk to open files from.");
    return false;
  }
  commit_edit();
  if (!ready_for(Next::OpenFile, path.view())) return false;
  return read_into_sheet(path.view());
}

bool Spreadsheet::read_into_sheet(StringView path) {
  if (!fs_) return false;
  const StringView name = name_of(path);
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::Read, &file);
  if (status != Status::Ok) {
    notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status, false));
    return false;
  }
  const uint64_t size = file->size();
  if (size > kMaxFileBytes) {
    notify(NoticeKind::Error, "\"%1\" is too large for Spreadsheet, which opens files of up to 8 MB. Open it with another program.", name);
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
  uint32_t dropped = 0;
  if (!ok || !sheet_.load_csv(text.view(), &dropped)) {
    notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status == Status::Ok ? Status::OutOfMemory : status, false));
    return false;
  }
  cancel_edit();
  forget_history();
  name_document(path);
  grid_.select(0, 0);
  changed();
  String message(allocator());
  format_message(&message, "Opened \"%1\".", name);
  say(message.view());
  if (dropped) {
    String count(allocator());
    count.append_uint(dropped);
    notify(NoticeKind::Warning, "\"%1\" is larger than a sheet: %2 cells beyond column AZ or row 1000 were left out. Saving it under the same name would lose them.", name, count.view());
  }
  return true;
}

bool Spreadsheet::save_file(StringView requested) {
  commit_edit();
  String path(allocator());
  if (!normalize(requested.empty() ? this->path() : requested, &path) || path.empty()) {
    ask_save_as();   // nowhere to save to yet: the same as Save As
    return false;
  }
  return write_from_sheet(path.view());
}

bool Spreadsheet::write_from_sheet(StringView path) {
  if (!fs_) {
    save_abandoned();
    say("There is no disk to save files on.");
    return false;
  }
  String csv(allocator());
  if (!sheet_.save_csv(&csv)) {
    save_abandoned();
    notify(NoticeKind::Error, "\"%1\" could not be saved. %2", name_of(path), file_advice(Status::OutOfMemory, true));
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  if (status == Status::Ok && !csv.empty()) status = file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(csv.data()), csv.size()), &written);
  if (status == Status::Ok) status = file->flush();
  file.reset();
  if (status != Status::Ok) {
    save_abandoned();
    notify(NoticeKind::Error, "\"%1\" could not be saved. %2", name_of(path), file_advice(status, true));
    return false;
  }
  fs_->flush();
  String saved(allocator());
  saved.assign(path);   // (`path` may be the frame's own path, which name_document() replaces)
  name_document(saved.view());
  sheet_.mark_saved();
  restored_ = false;
  String message(allocator());
  format_message(&message, "Saved \"%1\".", document_name());
  say(message.view());
  document_saved();   // (what waited for the save - New, Open, closing - goes ahead)
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
        else if (can_save()) save_file();
        return true;
      case key::kA + 25:                               // Z (with Shift: the same as Y); while a cell is typed in the keys are the text box's
        if (editing_) break;
        if (args.modifiers & hardware::modifier::kShift) redo();
        else undo();
        return true;
      case key::kA + 24:                               // Y
        if (editing_) break;
        redo();
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
