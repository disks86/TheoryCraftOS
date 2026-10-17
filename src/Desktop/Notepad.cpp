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

#include "Desktop/Notepad.hpp"
#include "Desktop/FileStatus.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
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

}  // namespace

Notepad::Notepad(allocators::IAllocator& allocator)
    : Frame(allocator),
      menu_(allocator),
      status_bar_(allocator),
      status_(allocator),
      position_(allocator),
      editor_(allocator),
      path_(allocator),
      name_(allocator) {
  set_bounds({120, 60, 560, 380});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New", {&Notepad::on_new, this}, "Ctrl+N");
    file->add_item("Open...", {&Notepad::on_open, this}, "Ctrl+O");
    file->add_item("Save", {&Notepad::on_save, this}, "Ctrl+S");
    file->add_item("Save As...", {&Notepad::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    file->add_item("Print...", {&Notepad::on_print, this}, "Ctrl+P");
    file->add_separator();
    file->add_item("Close", {&Notepad::on_close, this});
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    cut_item_ = edit->add_item("Cut", {&Notepad::on_cut, this}, "Ctrl+X");
    copy_item_ = edit->add_item("Copy", {&Notepad::on_copy, this}, "Ctrl+C");
    edit->add_item("Paste", {&Notepad::on_paste, this}, "Ctrl+V");
    edit->add_separator();
    edit->add_item("Select All", {&Notepad::on_select_all, this}, "Ctrl+A");
  }

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  position_.set_dock(DockStyle::Right);
  position_.set_size(130, 0);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&position_);
  status_bar_.add(&status_);

  editor_.set_dock(DockStyle::Fill);
  editor_.on_changed(Handler::immediate(&Notepad::on_edited, this));
  editor_.on_caret_moved(Handler::immediate(&Notepad::on_moved, this));

  add(&menu_);
  add(&status_bar_);
  add(&editor_);  // Fill goes last
  update_title();
  update_position();
  say("Start typing, or choose File > Open.");
}

void Notepad::update_title() {
  String title(allocator());
  title.assign("Text Editor - ");
  title.append(name_.empty() ? StringView("Untitled") : name_.view());
  if (editor_.modified()) title.append(" *");
  set_title(title.view());
}

void Notepad::update_menus() {
  if (cut_item_) cut_item_->set_enabled(editor_.has_selection());
  if (copy_item_) copy_item_->set_enabled(editor_.has_selection());
}

void Notepad::update_position() {
  String text(allocator());
  text.append("Ln ");
  text.append_uint(editor_.caret_line() + 1);
  text.append(", Col ");
  text.append_uint(editor_.caret_column() + 1);
  position_.set_text(text.view());
  update_title();
  update_menus();
}

void Notepad::on_edited(void* self, Control&) { static_cast<Notepad*>(self)->discard_armed_ = false; }
void Notepad::on_moved(void* self, Control&) { static_cast<Notepad*>(self)->update_position(); }
void Notepad::on_new(void* self, Control&) { static_cast<Notepad*>(self)->new_document(); }
void Notepad::on_save(void* self, Control&) { static_cast<Notepad*>(self)->save_file(); }
void Notepad::on_close(void* self, Control&) { static_cast<Notepad*>(self)->close(); }
void Notepad::on_cut(void* self, Control&) { static_cast<Notepad*>(self)->editor_.cut_selection(); }
void Notepad::on_copy(void* self, Control&) { static_cast<Notepad*>(self)->editor_.copy_selection(); }
void Notepad::on_paste(void* self, Control&) { static_cast<Notepad*>(self)->editor_.paste(); }
void Notepad::on_select_all(void* self, Control&) { static_cast<Notepad*>(self)->editor_.select_all(); }
void Notepad::on_open(void* self, Control&) { static_cast<Notepad*>(self)->ask_open(); }
void Notepad::on_save_as(void* self, Control&) { static_cast<Notepad*>(self)->ask_save_as(); }
void Notepad::on_print(void* self, Control&) { static_cast<Notepad*>(self)->ask_print(); }

void Notepad::ask_print() {
  if (!print_dialog_ || !manager()) {
    say("No print dialog available.");
    return;
  }
  if (editor_.text().empty()) {
    say("There is nothing to print.");
    return;
  }
  print_dialog_->set_text(editor_.text());   // for the preview and the page count
  if (!print_dialog_->show(*manager(), PrintDialog::Kind::Text, "Print", {&Notepad::on_print_picked, this})) say("The print dialog is busy.");
}

void Notepad::on_print_picked(void* self, Control&) {
  auto* n = static_cast<Notepad*>(self);
  PrintDialog& d = *n->print_dialog_;
  if (!d.accepted() || !d.print_service()) return;
  String error(n->allocator());
  if (d.print_service()->print_text(d.printer_index(), n->editor_.text(), n->name_.empty() ? StringView("Untitled") : n->name_.view(), d.options(), &error)) {
    n->say("Sent to the printer.");
  } else {
    String message(n->allocator());
    message.append("Cannot print: ");
    message.append(error.view());
    n->say(message.view());
  }
}

bool Notepad::may_discard() {
  if (!editor_.modified() || discard_armed_) {
    discard_armed_ = false;
    return true;
  }
  discard_armed_ = true;
  say("Unsaved changes - repeat the command to discard them, or press Save.");
  return false;
}

bool Notepad::new_document() {
  if (!may_discard()) return false;
  editor_.set_text(StringView());
  path_.clear();
  name_.clear();
  update_position();
  say("New document.");
  return true;
}

namespace {
const FileDialog::Filter kTextFilters[] = {{"Text files", "txt;md;log;cfg;ini;csv;json;xml;html"}, {"All files", "*"}};
}

void Notepad::ask_open() {
  if (!dialog_ || !manager()) {
    say("No file dialog available.");
    return;
  }
  if (!may_discard()) return;
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kTextFilters, 2, path_.view(), {&Notepad::on_open_picked, this}))
    say("The file dialog is busy.");
}

void Notepad::ask_save_as() {
  if (!dialog_ || !manager()) {
    say("No file dialog available.");
    return;
  }
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kTextFilters, 2, path_.view(), {&Notepad::on_save_picked, this}))
    say("The file dialog is busy.");
}

void Notepad::on_open_picked(void* self, Control&) {
  auto* n = static_cast<Notepad*>(self);
  if (n->dialog_->accepted()) n->read_into_editor(n->dialog_->selected_path());   // the unsaved changes were agreed to before the dialog opened
}

void Notepad::on_save_picked(void* self, Control&) {
  auto* n = static_cast<Notepad*>(self);
  if (n->dialog_->accepted()) n->write_from_editor(n->dialog_->selected_path());
}

bool Notepad::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) {
    say("Enter the path of a file to open.");
    return false;
  }
  if (!fs_) {
    say("No storage available.");
    return false;
  }
  if (!may_discard()) return false;
  return read_into_editor(path.view());
}

bool Notepad::read_into_editor(StringView path) {
  if (!fs_) {
    say("No storage available.");
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
    say(message.view());
    return false;
  }
  const uint64_t size = file->size();
  if (size > kMaxFileBytes) {
    say("The file is too large to edit (limit 1 MiB).");
    return false;
  }
  ArrayView<uint8_t> bytes = allocate_view<uint8_t>(allocator(), size ? size : 1);
  uint64_t got = 0;
  status = bytes.empty() ? Status::OutOfMemory : size ? file->read(0, bytes.first(size), &got) : Status::Ok;
  file.reset();
  String text(allocator());
  bool ok = status == Status::Ok && text.reserve(got);
  for (uint64_t i = 0; ok && i < got; ++i)
    if (bytes[i] != '\r') ok = text.push_back(static_cast<char>(bytes[i]));
  release_view(allocator(), bytes);
  if (!ok) {
    say(status == Status::Ok ? "Out of memory." : "The file could not be read.");
    return false;
  }
  editor_.set_text(text.view());
  path_.assign(path);
  const uint64_t slash = path.rfind('/');
  name_.assign(slash == StringView::npos ? path : path.substr(slash + 1));
  discard_armed_ = false;
  update_position();
  String message(allocator());
  message.append("Opened ");
  message.append(path);
  say(message.view());
  return true;
}

bool Notepad::save_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested.empty() ? path_.view() : requested, &path) || path.empty()) {
    if (dialog_ && manager()) ask_save_as();   // nowhere to save to yet: the same as Save As
    else say("Enter a path to save to.");
    return false;
  }
  return write_from_editor(path.view());
}

bool Notepad::write_from_editor(StringView path) {
  if (!fs_) {
    say("No storage available.");
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  const StringView text = editor_.text();
  if (status == Status::Ok && !text.empty())
    status = file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &written);
  if (status == Status::Ok) status = file->flush();
  file.reset();
  String message(allocator());
  if (status != Status::Ok) {
    message.append("Cannot save ");
    message.append(path);
    message.append(": ");
    message.append(describe_status(status));
    say(message.view());
    return false;
  }
  fs_->flush();
  path_.assign(path);
  const uint64_t slash = path.rfind('/');
  name_.assign(slash == StringView::npos ? path : path.substr(slash + 1));
  editor_.mark_saved();
  discard_armed_ = false;
  update_position();
  message.append("Saved ");
  message.append(path);
  say(message.view());
  return true;
}

bool Notepad::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  if (args.modifiers & hardware::modifier::kControl) {
    switch (args.usage) {
      case key::kA + 13: new_document(); return true;  // N
      case key::kA + 14: ask_open(); return true;      // O
      case key::kA + 15: ask_print(); return true;     // P
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
