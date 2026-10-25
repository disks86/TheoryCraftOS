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

#include "Desktop/Terminal.hpp"
#include "Desktop/FileStatus.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::IFile;
using filesystem::NodeType;
using filesystem::OpenMode;
using filesystem::Status;

Terminal::Terminal(allocators::IAllocator& allocator) : Frame(allocator), menu_(allocator), view_(allocator), shell_(allocator), opened_path_(allocator) {
  set_title("Terminal");
  set_bounds({100, 80, 600, 380});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New Terminal", {&Terminal::on_new, this}, "Ctrl+N");
    file->add_item("Save Output As...", {&Terminal::on_save, this}, "Ctrl+S");
    file->add_separator();
    file->add_item("Close", {&Terminal::on_close, this});
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    edit->add_item("Copy All", {&Terminal::on_copy_all, this}, "Ctrl+Shift+C");
    edit->add_item("Paste", {&Terminal::on_paste, this}, "Ctrl+V");
    edit->add_separator();
    edit->add_item("Clear", {&Terminal::on_clear, this}, "Ctrl+L");
  }

  view_.set_dock(DockStyle::Fill);
  view_.on_submit({&Terminal::on_submitted, this});
  view_.set_link_checker(&Terminal::is_link, this);
  view_.on_link_clicked({&Terminal::on_link, this});
  add(&menu_);
  add(&view_);   // Fill goes last

  Shell::Host host;
  host.write = &Terminal::host_write;
  host.clear = &Terminal::host_clear;
  host.exit = &Terminal::host_exit;
  host.unsupported_executable = &Terminal::host_unsupported_executable;
  host.context = this;
  shell_.set_host(host);
  view_.write("TheoryCraftOS terminal. Type help for the commands;\ndrop a folder here to go to it.\n\n");
  refresh_prompt();
}

void Terminal::set_file_system(filesystem::IFileSystem* fs) {
  fs_ = fs;
  shell_.set_file_system(fs);
  refresh_prompt();
}

void Terminal::refresh_prompt() {
  String prompt(allocator());
  shell_.prompt(&prompt);
  view_.set_prompt(prompt.view());
  String title(allocator());
  title.append("Terminal - ");
  title.append(shell_.cwd());
  set_title(title.view());
}

void Terminal::host_write(void* self, StringView text) { static_cast<Terminal*>(self)->view_.write(text); }
void Terminal::host_clear(void* self) { static_cast<Terminal*>(self)->view_.clear(); }
void Terminal::host_exit(void* self) { static_cast<Terminal*>(self)->close(); }

void Terminal::on_submitted(void* self, Control&) {
  auto* t = static_cast<Terminal*>(self);
  String line(t->allocator());
  line.assign(t->view_.submitted());   // (already echoed by the view)
  t->shell_.execute(line.view());
  t->refresh_prompt();
}

// A word is a link if it looks like a path (it has a slash or a dot) and names something that exists.
bool Terminal::is_link(void* self, StringView word) {
  auto* t = static_cast<Terminal*>(self);
  if (!t->fs_ || (word.find('/') == StringView::npos && word.find('.') == StringView::npos)) return false;
  String path(t->allocator());
  if (!t->shell_.resolve(word, &path)) return false;
  if (path.view().equals("/")) return true;
  DirectoryEntry entry;
  return t->fs_->stat(path.view(), &entry) == Status::Ok;
}

void Terminal::on_link(void* self, Control&) {
  auto* t = static_cast<Terminal*>(self);
  if (!t->shell_.resolve(t->view_.clicked_link(), &t->opened_path_)) return;
  t->fire(t->open_path_);
}

void Terminal::run(StringView line) {
  String text(allocator());
  text.append(view_.prompt());
  text.append(line);
  text.push_back('\n');
  view_.write(text.view());
  shell_.execute(line);
  refresh_prompt();
}

bool Terminal::change_directory(StringView path) {
  const StringView before = shell_.cwd();
  String old(allocator());
  old.assign(before);
  String line(allocator());
  line.append("cd \"");
  line.append(path);
  line.push_back('"');
  run(line.view());
  return !old.view().equals(shell_.cwd()) || path.equals(old.view());
}

void Terminal::on_drop(StringView text, int32_t, int32_t, const Control*) {
  if (!fs_) return;
  activate();
  DirectoryEntry entry;
  const bool root = text.equals("/");
  if (root || (fs_->stat(text, &entry) == Status::Ok && entry.type == NodeType::Directory)) {
    change_directory(text);
    return;
  }
  // A file: its path goes on the command line, quoted if it has blanks.
  String path(allocator());
  if (text.find(' ') != StringView::npos) {
    path.push_back('"');
    path.append(text);
    path.push_back('"');
  } else {
    path.assign(text);
  }
  path.push_back(' ');
  view_.insert_input(path.view());
}

namespace {
const FileDialog::Filter kOutputFilters[] = {{"Text files", "txt;log"}, {"All files", "*"}};
}

void Terminal::ask_save_output() {
  if (!dialog_ || !manager()) return notify(NoticeKind::Error, "No file dialog available.");
  String start(allocator());
  start.assign(shell_.cwd());
  if (!dialog_->show(*manager(), FileDialog::Mode::Save, "Save Output As", kOutputFilters, 2, start.view(), {&Terminal::on_save_picked, this}))
    notify(NoticeKind::Warning, "The file dialog is busy.");
}

void Terminal::on_save_picked(void* self, Control&) {
  auto* t = static_cast<Terminal*>(self);
  if (t->dialog_->accepted()) t->save_output(t->dialog_->selected_path());
}

bool Terminal::save_output(StringView path) {
  if (!fs_) {
    notify(NoticeKind::Error, "No storage available.");
    return false;
  }
  String text(allocator());
  if (!view_.text(&text)) {
    notify(NoticeKind::Error, "Out of memory.");
    return false;
  }
  UniquePointer<IFile> file;
  Status status = fs_->open_file(path, OpenMode::CreateTruncate, &file);
  uint64_t written = 0;
  if (status == Status::Ok) status = file->write(0, ConstByteView(reinterpret_cast<const uint8_t*>(text.c_str()), text.size()), &written);
  if (status == Status::Ok) status = file->flush();
  if (status != Status::Ok) {
    String message(allocator());
    message.append("Cannot save the output: ");
    message.append(describe_status(status));
    notify(NoticeKind::Error, message.view());
    return false;
  }
  fs_->flush();
  return true;
}

void Terminal::on_new(void* self, Control&) { auto* t = static_cast<Terminal*>(self); t->fire(t->new_terminal_); }
void Terminal::on_save(void* self, Control&) { static_cast<Terminal*>(self)->ask_save_output(); }
void Terminal::on_close(void* self, Control&) { static_cast<Terminal*>(self)->close(); }
void Terminal::on_copy_all(void* self, Control&) { static_cast<Terminal*>(self)->view_.copy_all(); }
void Terminal::on_paste(void* self, Control&) { static_cast<Terminal*>(self)->view_.paste(); }
void Terminal::host_unsupported_executable(void* self, StringView path, exe::ExecutableKind kind) {
  auto* t = static_cast<Terminal*>(self);
  String message(t->allocator());
  if (exe::unsupported_executable_message(path, kind, &message)) t->notify(NoticeKind::Warning, message.view());
}

void Terminal::on_clear(void* self, Control&) { static_cast<Terminal*>(self)->view_.clear(); }

bool Terminal::on_key(const KeyArgs& args) {
  namespace key = hardware::key;
  if ((args.modifiers & hardware::modifier::kControl) && !(args.modifiers & hardware::modifier::kShift)) {
    if (args.usage == key::kA + 13) {   // N
      fire(new_terminal_);
      return true;
    }
    if (args.usage == key::kA + 18) {   // S
      ask_save_output();
      return true;
    }
  }
  return Frame::on_key(args);
}

}  // namespace desktop
