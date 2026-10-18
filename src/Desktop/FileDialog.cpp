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

#include "Desktop/FileDialog.hpp"
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/Format.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::NodeType;
using filesystem::Status;

namespace {

// "/a/b" from "a//b/" and the like ("/" for nothing); leading and trailing blanks go.
bool normalize(StringView in, String* out) {
  out->clear();
  uint64_t from = 0, to = in.size();
  while (from < to && in[from] == ' ') ++from;
  while (to > from && in[to - 1] == ' ') --to;
  StringView rest = in.substr(from, to - from);
  bool any = false;
  while (!rest.empty()) {
    const uint64_t slash = rest.find('/');
    const uint64_t length = slash == StringView::npos ? rest.size() : slash;
    if (length > 0) {
      if (!out->push_back('/') || !out->append(rest.first(length))) return false;
      any = true;
    }
    rest = slash == StringView::npos ? StringView() : rest.without_prefix(slash + 1);
  }
  return any || out->assign("/");
}

}  // namespace

FileDialog::FileDialog(allocators::IAllocator& allocator)
    : Frame(allocator),
      address_row_(allocator),
      name_row_(allocator),
      type_row_(allocator),
      button_row_(allocator),
      gap_(allocator),
      bar_(allocator),
      list_(allocator),
      name_label_(allocator),
      type_label_(allocator),
      message_(allocator),
      name_(allocator),
      type_(allocator),
      ok_(allocator),
      cancel_(allocator),
      replace_(allocator),
      folder_(allocator),
      selected_(allocator),
      pending_(allocator) {
  set_minimize_enabled(false);
  set_escape_closes(true);
  set_padding(Insets::all(8));

  address_row_.set_style(PanelStyle::Transparent);
  address_row_.set_dock(DockStyle::Top);
  address_row_.set_size(0, 30);
  address_row_.set_padding(Insets{0, 0, 0, 4});
  bar_.set_dock(DockStyle::Fill);
  bar_.on_navigate({&FileDialog::on_navigate, this});
  address_row_.add(&bar_);

  // Along the bottom, from the lowest row up: the buttons (with the message beside them), the type, the name.
  button_row_.set_style(PanelStyle::Transparent);
  button_row_.set_dock(DockStyle::Bottom);
  button_row_.set_size(0, 32);
  button_row_.set_padding(Insets{0, 4, 0, 0});
  cancel_.set_text("Cancel");
  cancel_.set_dock(DockStyle::Right);
  cancel_.set_size(92, 0);
  cancel_.on_click({&FileDialog::on_cancel, this});
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_dock(DockStyle::Right);
  gap_.set_size(8, 0);
  ok_.set_dock(DockStyle::Right);
  ok_.set_size(92, 0);
  ok_.on_click({&FileDialog::on_ok, this});
  message_.set_dock(DockStyle::Fill);
  button_row_.add(&cancel_);
  button_row_.add(&gap_);
  button_row_.add(&ok_);
  button_row_.add(&message_);

  type_row_.set_style(PanelStyle::Transparent);
  type_row_.set_dock(DockStyle::Bottom);
  type_row_.set_size(0, 30);
  type_row_.set_padding(Insets{0, 2, 0, 2});
  type_label_.set_text("Files of type:");
  type_label_.set_dock(DockStyle::Left);
  type_label_.set_size(120, 0);
  type_.set_dock(DockStyle::Fill);
  type_.on_selection_changed({&FileDialog::on_type, this});
  type_row_.add(&type_label_);
  type_row_.add(&type_);

  name_row_.set_style(PanelStyle::Transparent);
  name_row_.set_dock(DockStyle::Bottom);
  name_row_.set_size(0, 30);
  name_row_.set_padding(Insets{0, 2, 0, 2});
  name_label_.set_text("File name:");
  name_label_.set_dock(DockStyle::Left);
  name_label_.set_size(120, 0);
  name_.set_dock(DockStyle::Fill);
  name_row_.add(&name_label_);
  name_row_.add(&name_);

  list_.add_column("Name", 280);
  list_.add_column("Size", 80, Align::Right);
  list_.add_column("Type", 100);
  list_.set_dock(DockStyle::Fill);
  list_.set_copy_enabled(false);
  list_.on_selection_changed({&FileDialog::on_selection, this});
  list_.on_item_activated({&FileDialog::on_activated, this});

  replace_.on_result({&FileDialog::on_replace_answer, this});

  add(&address_row_);
  add(&button_row_);
  add(&type_row_);
  add(&name_row_);
  add(&list_);  // Fill goes last
}

// --- Showing ---------------------------------------------------------------------------

bool FileDialog::show(WindowManager& manager, Mode mode, StringView title, const Filter* filters, uint32_t filter_count, StringView start_path,
                      Handler result) {
  UiGuard lock(&manager);
  if (showing() || !fs_) return false;
  mode_ = mode;
  accepted_ = false;
  selected_.clear();
  set_title(title);
  ok_.set_text(mode == Mode::Open ? "Open" : "Save");
  filter_count_ = filter_count < kMaxFilters ? filter_count : kMaxFilters;
  type_.clear_items();
  for (uint32_t i = 0; i < filter_count_; ++i) {
    filters_[i] = filters[i];
    type_.add_item(filters[i].name);
  }
  if (filter_count_ == 0) {
    filters_[0] = {"All files", "*"};
    filter_count_ = 1;
    type_.add_item("All files");
  }
  // A file path starts in its folder with its name filled in; a folder path just starts there.
  String start(allocator());
  normalize(start_path, &start);
  DirectoryEntry entry;
  String folder(allocator()), name(allocator());
  if (start.view().equals("/") || (fs_->stat(start.view(), &entry) == Status::Ok && entry.type == NodeType::Directory)) {
    folder.assign(start.view());
  } else {
    const uint64_t slash = start.view().rfind('/');
    folder.assign(slash == StringView::npos || slash == 0 ? StringView("/") : start.view().first(slash));
    name.assign(slash == StringView::npos ? start.view() : start.view().substr(slash + 1));
  }
  // Pick the filter that fits the name, if the name has an extension of one of them.
  int32_t chosen = 0;
  for (uint32_t i = 0; i < filter_count_ && !name.empty(); ++i)
    if (!StringView(filters_[i].extensions).equals("*") && name_has_extension(name.view(), filters_[i].extensions)) {
      chosen = static_cast<int32_t>(i);
      break;
    }
  type_.set_selected_index(chosen);
  if (!open_folder(folder.view()) && !open_folder("/")) return false;
  name_.set_text(name.view());
  say(StringView());
  on_closed(result);
  const int32_t area_w = static_cast<int32_t>(manager.dialog_area().width), area_h = static_cast<int32_t>(manager.dialog_area().height);
  const int32_t w = kWidth < area_w ? kWidth : area_w, h = kHeight < area_h ? kHeight : area_h;
  set_bounds({manager.dialog_area().x + ((area_w - w) / 2 > 0 ? (area_w - w) / 2 : 0), manager.dialog_area().y + ((area_h - h) / 3 > 0 ? (area_h - h) / 3 : 0), static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
  if (!manager.add_window(this, /*modal=*/true)) return false;
  if (Window* win = window()) {
    win->layout();
    win->set_focus(&name_);
  }
  return true;
}

StringView FileDialog::current_extensions() const {
  const int32_t index = type_.selected_index();
  return index >= 0 && static_cast<uint32_t>(index) < filter_count_ ? StringView(filters_[index].extensions) : StringView("*");
}

bool FileDialog::open_folder(StringView path) {
  if (!fs_) return false;
  List<DirectoryEntry> entries(allocator());
  String next(allocator());
  if (!normalize(path, &next) || read_sorted_directory(*fs_, next.view(), &entries) != Status::Ok) return false;
  folder_ = util::move(next);
  list_.clear_items();
  const StringView extensions = current_extensions();
  String size(allocator()), type(allocator());
  for (const DirectoryEntry& e : entries) {
    const bool folder = e.type == NodeType::Directory;
    if (!folder && !name_has_extension(e.name.view(), extensions)) continue;
    if (folder) size.clear();
    else format_size(e.size, &size);
    describe_entry_type(e, &type);
    const StringView cells[3] = {e.name.view(), size.view(), type.view()};
    list_.add_item(cells, 3, folder ? ItemIcon::Folder : ItemIcon::File);
  }
  bar_.set_path(folder_.view());
  return true;
}

// The full path for a name typed in the box: absolute names stand, others are inside the folder being shown.
bool FileDialog::join(StringView name, String* out) const {
  String text(allocator());
  uint64_t from = 0;
  while (from < name.size() && name[from] == ' ') ++from;
  if (from < name.size() && name[from] == '/') return normalize(name, out);
  if (!text.assign(folder_.view()) || (folder_.size() > 1 && !text.push_back('/')) || !text.append(name.substr(from))) return false;
  return normalize(text.view(), out);
}

// --- Choosing ---------------------------------------------------------------------------

void FileDialog::finish(StringView path) {
  selected_.assign(path);
  accepted_ = true;
  close();
}

void FileDialog::accept_name() {
  String name(allocator());
  name.assign(name_.text());
  if (name.view().find('/') == StringView::npos) {  // no folders in it: trim blanks only
    uint64_t from = 0, to = name.size();
    while (from < to && name[from] == ' ') ++from;
    while (to > from && name[to - 1] == ' ') --to;
    String trimmed(allocator());
    trimmed.assign(name.view().substr(from, to - from));
    name = util::move(trimmed);
  }
  if (name.empty()) {
    say("Enter a file name.");
    return;
  }
  String full(allocator());
  if (!join(name.view(), &full)) {
    say("That name cannot be used.");
    return;
  }
  DirectoryEntry entry;
  Status status = fs_->stat(full.view(), &entry);
  if (status == Status::Ok && entry.type == NodeType::Directory) {  // a folder name goes into the folder
    if (open_folder(full.view())) {
      name_.set_text(StringView());
      say(StringView());
    } else {
      say("That folder cannot be opened.");
    }
    return;
  }
  if (mode_ == Mode::Open) {
    if (status == Status::Ok) {
      finish(full.view());
    } else {
      String message(allocator());
      message.append(status == Status::NotFound ? "Not found: " : "Cannot open: ");
      message.append(name.view());
      say(message.view());
    }
    return;
  }
  // Save: a name without an extension gets the type's first one.
  const StringView extension = first_extension(current_extensions());
  const StringView last = full.view().substr(full.view().rfind('/') + 1);
  if (status == Status::NotFound && !extension.empty() && last.rfind('.') == StringView::npos) {
    if (!full.push_back('.') || !full.append(extension)) return;
    status = fs_->stat(full.view(), &entry);
  }
  if (status == Status::Ok) {  // it exists: ask before replacing it
    pending_ = util::move(full);
    String line(allocator());
    const StringView shown = pending_.view().substr(pending_.view().rfind('/') + 1);
    line.assign(shown);
    line.append(" already exists.");
    replace_.set_texts("Confirm Save", line.view(), "Do you want to replace it?", "Replace", "Cancel");
    if (WindowManager* m = manager()) replace_.show(*m);
    return;
  }
  const uint64_t slash = full.view().rfind('/');
  if (slash != StringView::npos && slash > 0) {  // the folder has to exist
    DirectoryEntry parent;
    if (fs_->stat(full.view().first(slash), &parent) != Status::Ok || parent.type != NodeType::Directory) {
      say("The folder does not exist.");
      return;
    }
  }
  finish(full.view());
}

void FileDialog::on_replace_answer(void* self, Control&) {
  auto* d = static_cast<FileDialog*>(self);
  if (d->replace_.accepted()) d->finish(d->pending_.view());
  else if (Window* w = d->window()) w->set_focus(&d->name_);
}

void FileDialog::on_navigate(void* self, Control&) {
  auto* d = static_cast<FileDialog*>(self);
  String target(d->allocator());
  target.assign(d->bar_.requested_path());
  if (d->open_folder(target.view())) d->say(StringView());
  else d->say("That folder cannot be opened.");
}

void FileDialog::on_selection(void* self, Control&) {  // a click on a file puts its name in the box
  auto* d = static_cast<FileDialog*>(self);
  const int32_t item = d->list_.selected_item();
  if (item < 0 || d->list_.item_icon(static_cast<uint32_t>(item)) == ItemIcon::Folder) return;
  String name(d->allocator());
  d->list_.item_text(static_cast<uint32_t>(item), 0, &name);
  d->name_.set_text(name.view());
  d->say(StringView());
}

void FileDialog::on_activated(void* self, Control&) {  // Enter or a double click: into a folder, or take the file
  auto* d = static_cast<FileDialog*>(self);
  const int32_t item = d->list_.selected_item();
  if (item < 0) return;
  String name(d->allocator());
  d->list_.item_text(static_cast<uint32_t>(item), 0, &name);
  if (d->list_.item_icon(static_cast<uint32_t>(item)) == ItemIcon::Folder) {
    String path(d->allocator());
    if (!path.assign(d->folder_.view()) || (d->folder_.size() > 1 && !path.push_back('/')) || !path.append(name.view())) return;
    if (d->open_folder(path.view())) d->say(StringView());
    else d->say("That folder cannot be opened.");
    return;
  }
  d->name_.set_text(name.view());
  d->accept_name();
}

void FileDialog::on_type(void* self, Control&) {  // another type: show its files
  auto* d = static_cast<FileDialog*>(self);
  String folder(d->allocator());
  folder.assign(d->folder_.view());
  d->open_folder(folder.view());
}

void FileDialog::on_ok(void* self, Control&) { static_cast<FileDialog*>(self)->accept_name(); }
void FileDialog::on_cancel(void* self, Control&) { static_cast<FileDialog*>(self)->close(); }

bool FileDialog::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    accept_name();
    return true;
  }
  if (args.usage == hardware::key::kBackspace && window() && window()->focus() != &name_ && window()->focus() != &bar_.edit_box()) {
    String parent(allocator());  // Backspace in the list goes up a folder
    const uint64_t slash = folder_.view().rfind('/');
    if (folder_.size() > 1) {
      parent.assign(slash == 0 || slash == StringView::npos ? StringView("/") : folder_.view().first(slash));
      open_folder(parent.view());
    }
    return true;
  }
  return Frame::on_key(args);
}

}  // namespace desktop
