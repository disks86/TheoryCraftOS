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

const FileDialog::Filter kTextFilters[] = {{"Text Documents", "txt;md;log;cfg;ini;csv;json;xml;html"}, {"All Files", "*"}};

uint32_t g_documents = 0;   // numbers the new documents: "Document 1", "Document 2"...

}  // namespace

Notepad::Notepad(allocators::IAllocator& allocator)
    : DocumentFrame(allocator, "Text Editor", "Document", &g_documents),
      menu_(allocator),
      status_bar_(allocator),
      status_(allocator),
      position_(allocator),
      editor_(allocator) {
  set_bounds({120, 60, 560, 380});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  menu_.on_opening(Handler::immediate(&Notepad::on_menu_opening, this));
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New", {&Notepad::on_new, this}, "Ctrl+N");
    open_item_ = file->add_item("Open...", {&Notepad::on_open, this}, "Ctrl+O");
    save_item_ = file->add_item("Save", {&Notepad::on_save, this}, "Ctrl+S");
    save_as_item_ = file->add_item("Save As...", {&Notepad::on_save_as, this}, "Ctrl+Shift+S");
    file->add_separator();
    print_item_ = file->add_item("Print...", {&Notepad::on_print, this}, "Ctrl+P");
    file->add_separator();
    file->add_item("Exit", {&Notepad::on_exit, this});   // (the document goes with its window, so this is Exit and not Close)
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    undo_item_ = edit->add_item("Undo", {&Notepad::on_undo, this}, "Ctrl+Z");
    redo_item_ = edit->add_item("Redo", {&Notepad::on_redo, this}, "Ctrl+Y");
    edit->add_separator();
    cut_item_ = edit->add_item("Cut", {&Notepad::on_cut, this}, "Ctrl+X");
    copy_item_ = edit->add_item("Copy", {&Notepad::on_copy, this}, "Ctrl+C");
    paste_item_ = edit->add_item("Paste", {&Notepad::on_paste, this}, "Ctrl+V");
    delete_item_ = edit->add_item("Delete", {&Notepad::on_delete, this}, "Del");
    edit->add_separator();
    edit->add_item("Select All", {&Notepad::on_select_all, this}, "Ctrl+A");
  }
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About Text Editor", {&Notepad::on_about, this});

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
  update_position();
  say("Start typing, or choose Open from the File menu.");
}

// The items that cannot work right now are disabled (and say so by being grey) instead of answering with a message.
void Notepad::update_menus() {
  const kernel::Clipboard* clip = clipboard();
  if (open_item_) open_item_->set_enabled(can_open());
  if (save_item_) save_item_->set_enabled(can_save());
  if (save_as_item_) save_as_item_->set_enabled(can_open());
  if (print_item_) print_item_->set_enabled(can_print());
  if (undo_item_) undo_item_->set_enabled(editor_.can_undo());
  if (redo_item_) redo_item_->set_enabled(editor_.can_redo());
  if (cut_item_) cut_item_->set_enabled(editor_.has_selection());
  if (copy_item_) copy_item_->set_enabled(editor_.has_selection());
  if (paste_item_) paste_item_->set_enabled(clip && clip->has_text());
  if (delete_item_) delete_item_->set_enabled(editor_.has_selection());
}

void Notepad::update_position() {
  String text(allocator()), line(allocator()), column(allocator());
  line.append_uint(editor_.caret_line() + 1);
  column.append_uint(editor_.caret_column() + 1);
  format_message(&text, "Ln %1, Col %2", line.view(), column.view());
  position_.set_text(text.view());
  update_menus();
}

void Notepad::on_menu_opening(void* self, Control&) { static_cast<Notepad*>(self)->update_menus(); }
void Notepad::on_edited(void* self, Control&) { static_cast<Notepad*>(self)->update_menus(); }
void Notepad::on_moved(void* self, Control&) { static_cast<Notepad*>(self)->update_position(); }
void Notepad::on_new(void* self, Control&) { static_cast<Notepad*>(self)->new_document(); }
void Notepad::on_save(void* self, Control&) { static_cast<Notepad*>(self)->save_file(); }
void Notepad::on_exit(void* self, Control&) { static_cast<Notepad*>(self)->exit_requested(); }
void Notepad::on_undo(void* self, Control&) { static_cast<Notepad*>(self)->editor_.undo(); }
void Notepad::on_redo(void* self, Control&) { static_cast<Notepad*>(self)->editor_.redo(); }
void Notepad::on_cut(void* self, Control&) { static_cast<Notepad*>(self)->editor_.cut_selection(); }
void Notepad::on_copy(void* self, Control&) { static_cast<Notepad*>(self)->editor_.copy_selection(); }
void Notepad::on_paste(void* self, Control&) { static_cast<Notepad*>(self)->editor_.paste(); }
void Notepad::on_delete(void* self, Control&) { static_cast<Notepad*>(self)->editor_.delete_selection(); }
void Notepad::on_select_all(void* self, Control&) { static_cast<Notepad*>(self)->editor_.select_all(); }
void Notepad::on_open(void* self, Control&) { static_cast<Notepad*>(self)->ask_open(); }
void Notepad::on_save_as(void* self, Control&) { static_cast<Notepad*>(self)->ask_save_as(); }
void Notepad::on_print(void* self, Control&) { static_cast<Notepad*>(self)->ask_print(); }
void Notepad::on_about(void* self, Control&) {
  static_cast<Notepad*>(self)->show_about("Text Editor writes and changes plain text documents. It is part of TheoryCraftOS.");
}

void Notepad::ask_print() {
  if (!can_print() || !manager()) return;   // (the menu item is disabled then)
  print_dialog_->set_text(editor_.text());   // for the preview and the page count
  if (!print_dialog_->show(*manager(), PrintDialog::Kind::Text, "Print", {&Notepad::on_print_picked, this})) say("The Print dialog is already open for another window.");
}

void Notepad::on_print_picked(void* self, Control&) {
  auto* n = static_cast<Notepad*>(self);
  PrintDialog& d = *n->print_dialog_;
  if (!d.accepted() || !d.print_service()) return;
  String error(n->allocator());
  if (d.print_service()->print_text(d.printer_index(), n->editor_.text(), n->document_name(), d.options(), &error)) {
    n->say("Sent to the printer.");
  } else {
    n->notify(NoticeKind::Error, "\"%1\" could not be printed. %2", n->document_name(), error.view());
  }
}

void Notepad::carry_out(Next next, StringView path) {
  if (next == Next::New) start_new();
  else if (next == Next::Open) show_open_dialog();
  else if (next == Next::OpenFile) read_into_editor(path);
}

void Notepad::start_new() {
  editor_.set_text(StringView());
  name_new_document();
  update_position();
  say("New document.");
}

bool Notepad::new_document() {
  if (!ready_for(Next::New)) return false;
  start_new();
  return true;
}

void Notepad::ask_open() {
  if (!can_open() || !manager()) return;   // (the menu item is disabled then)
  if (ready_for(Next::Open)) show_open_dialog();
}

void Notepad::show_open_dialog() {
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kDocuments, ".txt", false, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Open, "Open", kTextFilters, 2, start.view(), {&Notepad::on_open_picked, this}))
    say("The Open dialog is already open for another window.");
}

void Notepad::ask_save_as() {
  if (!can_open() || !manager()) {
    save_abandoned();
    return;
  }
  String start(allocator());
  dialog_start(fs_, filesystem::layout::kDocuments, ".txt", true, &start);
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save As", kTextFilters, 2, start.view(), {&Notepad::on_save_picked, this})) {
    save_abandoned();
    say("The Save As dialog is already open for another window.");
  }
}

void Notepad::on_open_picked(void* self, Control&) {
  auto* n = static_cast<Notepad*>(self);
  if (n->dialog_->accepted()) n->read_into_editor(n->dialog_->selected_path());   // (the unsaved changes were dealt with before the dialog opened)
}

void Notepad::on_save_picked(void* self, Control&) {
  auto* n = static_cast<Notepad*>(self);
  if (n->dialog_->accepted()) n->write_from_editor(n->dialog_->selected_path());
  else n->save_abandoned();
}

bool Notepad::open_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested, &path) || path.empty()) return false;
  if (!fs_) {
    say("There is no disk to open files from.");
    return false;
  }
  if (!ready_for(Next::OpenFile, path.view())) return false;
  return read_into_editor(path.view());
}

bool Notepad::read_into_editor(StringView path) {
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
    notify(NoticeKind::Error, "\"%1\" is too large for Text Editor, which opens documents of up to 1 MB. Open it with another program.", name);
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
  if (!ok || !editor_.set_text(text.view())) {
    notify(NoticeKind::Error, "\"%1\" could not be opened. %2", name, file_advice(status == Status::Ok ? Status::OutOfMemory : status, false));
    return false;
  }
  name_document(path);
  update_position();
  String message(allocator());
  format_message(&message, "Opened \"%1\".", name);
  say(message.view());
  return true;
}

bool Notepad::save_file(StringView requested) {
  String path(allocator());
  if (!normalize(requested.empty() ? this->path() : requested, &path) || path.empty()) {
    ask_save_as();   // nowhere to save to yet: the same as Save As
    return false;
  }
  return write_from_editor(path.view());
}

bool Notepad::write_from_editor(StringView path) {
  if (!fs_) {
    save_abandoned();
    say("There is no disk to save files on.");
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
  if (status != Status::Ok) {
    save_abandoned();
    notify(NoticeKind::Error, "\"%1\" could not be saved. %2", name_of(path), file_advice(status, true));
    return false;
  }
  fs_->flush();
  String saved(allocator());
  saved.assign(path);   // (`path` may be the frame's own path, which name_document() replaces)
  name_document(saved.view());
  editor_.mark_saved();
  update_position();
  String message(allocator());
  format_message(&message, "Saved \"%1\".", document_name());
  say(message.view());
  document_saved();   // (what waited for the save - New, Open, closing - goes ahead)
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
        else if (can_save()) save_file();
        return true;
      default: break;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
