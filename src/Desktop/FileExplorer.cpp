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
#include "Desktop/DirectoryListing.hpp"
#include "Desktop/Format.hpp"
#include "Kernel/Clipboard.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::IDirectory;
using filesystem::NodeType;
using filesystem::Status;


FileExplorer::FileExplorer(allocators::IAllocator& allocator)
    : Frame(allocator),
      menu_(allocator),
      list_menu_(allocator),
      address_menu_(allocator),
      toolbar_(allocator),
      status_bar_(allocator),
      address_(allocator),
      status_(allocator),
      view_(allocator),
      path_(allocator),
      scratch_(allocator),
      opened_file_(allocator) {
  set_title("File Explorer");
  set_bounds({60, 40, 540, 360});
  set_padding(Insets::all(4));
  set_minimize_enabled(true);

  toolbar_.set_dock(DockStyle::Top);
  toolbar_.set_size(0, 28);
  toolbar_.set_padding(Insets{0, 2, 0, 2});

  menu_.set_dock(DockStyle::Top);
  menu_.set_size(0, 22);
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("Open", {&FileExplorer::on_open_item, this}, "Enter");
    file->add_item("Copy Path", {&FileExplorer::on_copy_path, this}, "Ctrl+C");
    file->add_item("Eject", {&FileExplorer::on_eject_menu, this});
    file->add_separator();
    file->add_item("Close", {&FileExplorer::on_close_window, this});
  }
  if (Menu* view = menu_.add_menu("View")) {
    list_item_ = view->add_item("List", {&FileExplorer::on_view_list, this});
    icons_item_ = view->add_item("Icons", {&FileExplorer::on_view_icons, this});
    details_item_ = view->add_item("Details", {&FileExplorer::on_view_details, this});
    view->add_separator();
    view->add_item("Refresh", {&FileExplorer::on_refresh_menu, this}, "F5");
  }
  if (Menu* go = menu_.add_menu("Go")) {
    go->add_item("Up One Level", {&FileExplorer::on_up_menu, this}, "Backspace");
    go->add_item("Address", {&FileExplorer::on_address_menu, this}, "Ctrl+L");
  }

  // The context menus.
  ctx_open_ = list_menu_.add_item("Open", {&FileExplorer::on_open_item, this}, "Enter");
  ctx_copy_ = list_menu_.add_item("Copy Path", {&FileExplorer::on_copy_path, this}, "Ctrl+C");
  list_menu_.add_separator();
  ctx_list_ = list_menu_.add_item("List", {&FileExplorer::on_view_list, this});
  ctx_icons_ = list_menu_.add_item("Icons", {&FileExplorer::on_view_icons, this});
  ctx_details_ = list_menu_.add_item("Details", {&FileExplorer::on_view_details, this});
  list_menu_.add_separator();
  ctx_up_ = list_menu_.add_item("Up One Level", {&FileExplorer::on_up_menu, this}, "Backspace");
  list_menu_.add_item("Refresh", {&FileExplorer::on_refresh_menu, this}, "F5");
  list_menu_.on_opening(Handler::immediate(&FileExplorer::on_list_menu_opening, this));
  address_menu_.add_item("Copy Address", {&FileExplorer::on_copy_address, this});
  address_menu_.add_item("Edit Address", {&FileExplorer::on_address_menu, this}, "Ctrl+L");
  address_.set_context_menu(&address_menu_);
  view_.set_context_menu(&list_menu_);

  address_.set_dock(DockStyle::Fill);
  address_.on_navigate({&FileExplorer::on_navigate, this});
  toolbar_.add(&address_);  // Fill goes last

  status_bar_.set_style(PanelStyle::Sunken);
  status_bar_.set_dock(DockStyle::Bottom);
  status_bar_.set_size(0, 22);
  status_.set_dock(DockStyle::Fill);
  status_bar_.add(&status_);

  view_.add_column("Name", 240);
  view_.add_column("Size", 80, Align::Right);
  view_.add_column("Type", 100);
  view_.set_dock(DockStyle::Fill);
  view_.set_copy_enabled(false);  // Ctrl+C copies the item's path instead (see on_key)
  view_.on_item_activated({&FileExplorer::on_activated, this});
  view_.on_selection_changed({&FileExplorer::on_selection, this});

  add(&menu_);
  add(&toolbar_);
  add(&status_bar_);
  add(&view_);  // Fill goes last
  path_.assign("/");
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
  if (list_item_) list_item_->set_checked(mode == ViewMode::List);
  if (icons_item_) icons_item_->set_checked(mode == ViewMode::Icons);
  if (details_item_) details_item_->set_checked(mode == ViewMode::Details);
}

bool FileExplorer::open_folder(StringView path) {
  kernel::ScopedMutex guard(navigation_);
  return load(path);
}

bool FileExplorer::refresh() {
  kernel::ScopedMutex guard(navigation_);
  if (!fs_) return false;
  String path(allocator());
  if (!path.assign(path_.view())) return false;
  for (;;) {
    if (load(path.view())) return true;
    if (path.size() <= 1) break;
    // The folder is gone (its volume was unplugged, say): try the one above it.
    const StringView p = path.view();
    uint64_t end = p.size();
    while (end > 1 && p[end - 1] == '/') --end;
    const uint64_t slash = p.first(end).rfind('/');
    String parent(allocator());
    if (!parent.assign(slash == StringView::npos || slash == 0 ? StringView("/") : p.first(slash))) break;
    path = util::move(parent);
  }
  view_.clear_items();
  status_.set_text("The folder could not be read");
  return false;
}

bool FileExplorer::load(StringView path) {
  if (!fs_) return false;
  List<DirectoryEntry> entries(allocator());
  if (read_sorted_directory(*fs_, path, &entries) != Status::Ok) return false;

  String next(allocator());
  if (!next.assign(path.empty() ? StringView("/") : path)) return false;
  path_ = util::move(next);
  view_.clear_items();
  String size(allocator()), type(allocator());
  for (const DirectoryEntry& e : entries) {
    const bool folder = e.type == NodeType::Directory;
    if (folder) size.clear();
    else format_size(e.size, &size);
    describe_entry_type(e, &type);
    const StringView cells[3] = {e.name.view(), size.view(), type.view()};
    view_.add_item(cells, 3, folder ? ItemIcon::Folder : ItemIcon::File);
  }
  address_.set_path(path_.view());
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

bool FileExplorer::eject_selected() {
  if (!fs_ || !eject_) {
    notify(gui::NoticeKind::Information, "Nothing here can be ejected");
    return false;
  }
  String volume(allocator());
  {
    kernel::ScopedMutex guard(navigation_);
    if (path_.size() <= 1) {   // the root lists the drives: the selected one
      const int32_t item = view_.selected_item();
      if (item < 0 || view_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::Folder) {
        notify(gui::NoticeKind::Warning, "Select a drive to eject");
        return false;
      }
      view_.item_text(static_cast<uint32_t>(item), 0, &volume);
    } else {                   // inside a drive: the first folder of the path
      const StringView p = path_.view();
      uint64_t end = p.find('/', 1);
      if (end == StringView::npos) end = p.size();
      volume.assign(StringView(p.data() + 1, end - 1));
    }
  }
  bool succeeded = true;
  const char* message = eject_(eject_context_, volume.view(), &succeeded);
  refresh();   // (the drive is gone from the root, and a folder on it from the view)
  if (message) notify(succeeded ? gui::NoticeKind::Information : gui::NoticeKind::Error, message);
  return true;
}

void FileExplorer::activate_selected() {
  const int32_t item = view_.selected_item();
  if (item < 0) return;
  String name(allocator());
  view_.item_text(static_cast<uint32_t>(item), 0, &name);
  String child(allocator());
  if (!child.assign(path_.view()) || (path_.size() > 1 && !child.push_back('/')) || !child.append(name.view())) return;
  if (view_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::Folder) {
    opened_file_ = util::move(child);
    fire(file_opened_);
    return;
  }
  if (!open_folder(child.view())) notify(gui::NoticeKind::Error, "The folder could not be opened");
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

// Puts the full path of the selected item on the clipboard. False if nothing is selected or there is no clipboard.
bool FileExplorer::copy_selected_path() {
  const int32_t item = view_.selected_item();
  if (item < 0 || !clipboard()) return false;
  String name(allocator()), full(allocator());
  view_.item_text(static_cast<uint32_t>(item), 0, &name);
  if (!full.assign(path_.view()) || (path_.size() > 1 && !full.push_back('/')) || !full.append(name.view())) return true;
  if (clipboard()->set_text(full.view())) {
    String message(allocator());
    message.append("Copied ");
    message.append(full.view());
    status_.set_text(message.view());
  }
  return true;
}

bool FileExplorer::on_key(const KeyArgs& args) {
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA + 2) return copy_selected_path();   // Ctrl+C
  if (args.usage == hardware::key::kF1 + 4) {  // F5
    refresh();
    return true;
  }
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA + 11) {  // Ctrl+L
    address_.begin_edit();
    return true;
  }
  if (args.usage == hardware::key::kBackspace) {
    go_up();
    return true;
  }
  return Frame::on_key(args);
}

void FileExplorer::on_navigate(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  String target(e->allocator());
  target.assign(e->address_.requested_path());
  if (!e->open_folder(target.view())) {
    String message(e->allocator());
    message.append("Cannot open ");
    message.append(target.view());
    e->notify(gui::NoticeKind::Error, message.view());
  }
}

// The list's menu is enabled for what is selected and shows the view in use.
void FileExplorer::on_list_menu_opening(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  const bool selected = e->view_.selected_item() >= 0;
  e->ctx_open_->set_enabled(selected);
  e->ctx_copy_->set_enabled(selected);
  e->ctx_up_->set_enabled(e->path_.size() > 1);
  e->ctx_list_->set_checked(e->view_.view_mode() == ViewMode::List);
  e->ctx_icons_->set_checked(e->view_.view_mode() == ViewMode::Icons);
  e->ctx_details_->set_checked(e->view_.view_mode() == ViewMode::Details);
}

void FileExplorer::on_copy_address(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  if (e->clipboard() && e->clipboard()->set_text(e->path_.view())) e->status_.set_text("Copied the address.");
}

void FileExplorer::on_view_list(void* self, Control&) { static_cast<FileExplorer*>(self)->set_view_mode(ViewMode::List); }
void FileExplorer::on_view_icons(void* self, Control&) { static_cast<FileExplorer*>(self)->set_view_mode(ViewMode::Icons); }
void FileExplorer::on_view_details(void* self, Control&) { static_cast<FileExplorer*>(self)->set_view_mode(ViewMode::Details); }
void FileExplorer::on_open_item(void* self, Control&) { static_cast<FileExplorer*>(self)->activate_selected(); }
void FileExplorer::on_copy_path(void* self, Control&) { static_cast<FileExplorer*>(self)->copy_selected_path(); }
void FileExplorer::on_eject_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->eject_selected(); }
void FileExplorer::on_close_window(void* self, Control&) { static_cast<FileExplorer*>(self)->close(); }
void FileExplorer::on_up_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->go_up(); }
void FileExplorer::on_refresh_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->refresh(); }
void FileExplorer::on_address_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->address_.begin_edit(); }

void FileExplorer::on_activated(void* self, Control&) { static_cast<FileExplorer*>(self)->activate_selected(); }
void FileExplorer::on_selection(void* self, Control&) { static_cast<FileExplorer*>(self)->update_status(); }

}  // namespace desktop
