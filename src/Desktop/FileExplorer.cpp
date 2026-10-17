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

#include "Desktop/FileExplorer.hpp"
#include "Desktop/Format.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::IDirectory;
using filesystem::NodeType;
using filesystem::Status;

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

// Folders first, then by name without regard to case.
bool entry_less(const DirectoryEntry& a, const DirectoryEntry& b) {
  if (a.type != b.type) return a.type == NodeType::Directory;
  const StringView x = a.name.view(), y = b.name.view();
  const uint64_t n = x.size() < y.size() ? x.size() : y.size();
  for (uint64_t i = 0; i < n; ++i)
    if (lower(x[i]) != lower(y[i])) return lower(x[i]) < lower(y[i]);
  return x.size() < y.size();
}

// "TXT File" for "notes.txt", "File" without an extension.
bool format_type(const DirectoryEntry& entry, String* out) {
  out->clear();
  if (entry.type == NodeType::Directory) return out->assign("Folder");
  const StringView name = entry.name.view();
  const uint64_t dot = name.rfind('.');
  if (dot == StringView::npos || dot == 0 || dot + 1 == name.size()) return out->assign("File");
  for (uint64_t i = dot + 1; i < name.size() && i < dot + 6; ++i)
    if (!out->push_back(upper(name[i]))) return false;
  return out->append(" File");
}

}  // namespace

FileExplorer::FileExplorer(allocators::IAllocator& allocator)
    : Frame(allocator),
      toolbar_(allocator),
      status_bar_(allocator),
      up_(allocator),
      path_label_(allocator),
      status_(allocator),
      list_button_(allocator),
      icons_button_(allocator),
      details_button_(allocator),
      view_(allocator),
      path_(allocator),
      scratch_(allocator) {
  set_title("File Explorer");
  set_bounds({60, 40, 540, 360});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  toolbar_.set_dock(DockStyle::Top);
  toolbar_.set_size(0, 28);
  toolbar_.set_padding(Insets{0, 2, 0, 2});

  up_.set_text("Up");
  up_.set_dock(DockStyle::Left);
  up_.set_size(48, 0);
  up_.on_click({&FileExplorer::on_up, this});

  struct Mode {
    RadioButton& button;
    const char* text;
    int32_t width;
  };
  const Mode modes[3] = {{details_button_, "Details", 82}, {icons_button_, "Icons", 68}, {list_button_, "List", 58}};
  for (const Mode& m : modes) {  // docked right: the first added is the rightmost
    m.button.set_text(m.text);
    m.button.set_dock(DockStyle::Right);
    m.button.set_size(m.width, 0);
    m.button.set_group(&group_);
    m.button.on_changed({&FileExplorer::on_mode, this});
    toolbar_.add(&m.button);
  }
  path_label_.set_dock(DockStyle::Fill);
  path_label_.set_text("/");
  toolbar_.add(&up_);
  toolbar_.add(&path_label_);

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&status_);

  view_.add_column("Name", 240);
  view_.add_column("Size", 80, Align::Right);
  view_.add_column("Type", 100);
  view_.set_dock(DockStyle::Fill);
  view_.on_item_activated({&FileExplorer::on_activated, this});
  view_.on_selection_changed({&FileExplorer::on_selection, this});

  add(&toolbar_);
  add(&status_bar_);
  add(&view_);  // Fill goes last
  path_.assign("/");
  details_button_.set_checked(true);
  set_view_mode(ViewMode::Details);
  update_status();
}

void FileExplorer::set_file_system(filesystem::IFileSystem* fs) {
  fs_ = fs;
  if (!open_folder("/")) {
    view_.clear_items();
    status_.set_text(fs ? "The folder could not be read" : "No storage available");
  }
}

void FileExplorer::set_view_mode(ViewMode mode) {
  view_.set_view_mode(mode);
  RadioButton& button = mode == ViewMode::List ? list_button_ : mode == ViewMode::Icons ? icons_button_ : details_button_;
  button.set_checked(true);
}

bool FileExplorer::open_folder(StringView path) {
  if (!fs_) return false;
  UniquePointer<IDirectory> directory;
  if (fs_->open_directory(path, &directory) != Status::Ok) return false;
  // Read everything first, keeping the entries in display order.
  List<DirectoryEntry> entries(allocator());
  DirectoryEntry entry;
  while (directory->next(&entry) == Status::Ok) {
    uint64_t low = 0, high = entries.count();  // binary search for the insertion point
    while (low < high) {
      const uint64_t mid = (low + high) / 2;
      if (entry_less(entries[mid], entry)) low = mid + 1;
      else high = mid;
    }
    if (!entries.insert(low, entry)) break;
  }
  directory.reset();

  String next(allocator());
  if (!next.assign(path.empty() ? StringView("/") : path)) return false;
  path_ = util::move(next);
  view_.clear_items();
  String size(allocator()), type(allocator());
  for (const DirectoryEntry& e : entries) {
    const bool folder = e.type == NodeType::Directory;
    if (folder) size.clear();
    else format_size(e.size, &size);
    format_type(e, &type);
    const StringView cells[3] = {e.name.view(), size.view(), type.view()};
    view_.add_item(cells, 3, folder ? ItemIcon::Folder : ItemIcon::File);
  }
  path_label_.set_text(path_.view());
  up_.set_enabled(path_.size() > 1);
  update_status();
  return true;
}

bool FileExplorer::go_up() {
  if (path_.size() <= 1) return false;
  const StringView p = path_.view();
  uint64_t end = p.size();
  while (end > 1 && p[end - 1] == '/') --end;
  const uint64_t slash = p.first(end).rfind('/');
  String parent(allocator());
  if (!parent.assign(slash == StringView::npos || slash == 0 ? StringView("/") : p.first(slash))) return false;
  return open_folder(parent.view());
}

void FileExplorer::activate_selected() {
  const int32_t item = view_.selected_item();
  if (item < 0 || view_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::Folder) return;
  String name(allocator());
  view_.item_text(static_cast<uint32_t>(item), 0, &name);
  String child(allocator());
  if (!child.assign(path_.view()) || (path_.size() > 1 && !child.push_back('/')) || !child.append(name.view())) return;
  if (!open_folder(child.view())) status_.set_text("The folder could not be opened");
}

void FileExplorer::update_status() {
  scratch_.clear();
  scratch_.append_uint(view_.item_count());
  scratch_.append(view_.item_count() == 1 ? " item" : " items");
  const int32_t item = view_.selected_item();
  if (item >= 0) {
    String name(allocator()), size(allocator());
    view_.item_text(static_cast<uint32_t>(item), 0, &name);
    view_.item_text(static_cast<uint32_t>(item), 1, &size);
    scratch_.append("  |  ");
    scratch_.append(name.view());
    if (!size.empty()) {
      scratch_.append(", ");
      scratch_.append(size.view());
    }
  }
  status_.set_text(scratch_.view());
}

bool FileExplorer::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kBackspace) {
    go_up();
    return true;
  }
  return Frame::on_key(args);
}

void FileExplorer::on_up(void* self, Control&) { static_cast<FileExplorer*>(self)->go_up(); }

void FileExplorer::on_mode(void* self, Control& sender) {
  auto* e = static_cast<FileExplorer*>(self);
  auto& button = static_cast<RadioButton&>(sender);
  if (!button.checked()) return;
  e->view_.set_view_mode(&button == &e->list_button_ ? ViewMode::List : &button == &e->icons_button_ ? ViewMode::Icons : ViewMode::Details);
}

void FileExplorer::on_activated(void* self, Control&) { static_cast<FileExplorer*>(self)->activate_selected(); }
void FileExplorer::on_selection(void* self, Control&) { static_cast<FileExplorer*>(self)->update_status(); }

}  // namespace desktop
