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
#include "Desktop/FileStatus.hpp"
#include "Desktop/FileTransfer.hpp"
#include "Desktop/Format.hpp"
#include "Kernel/Clipboard.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

using namespace gui;
using filesystem::DirectoryEntry;
using filesystem::IDirectory;
using filesystem::IFile;
using filesystem::OpenMode;
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
      opened_file_(allocator),
      properties_path_(allocator),
      delete_dialog_(allocator),
      delete_path_(allocator),
      credentials_dialog_(allocator),
      pending_path_(allocator),
      last_user_(allocator), last_uid_(allocator), last_gid_(allocator) {
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
    file->add_item("Copy", {&FileExplorer::on_copy_menu, this}, "Ctrl+C");
    file->add_item("Paste", {&FileExplorer::on_paste_menu, this}, "Ctrl+V");
    file->add_item("Select All", {&FileExplorer::on_select_all_menu, this}, "Ctrl+A");
    file->add_item("Copy Path", {&FileExplorer::on_copy_path, this});
    file->add_item("Rename", {&FileExplorer::on_rename_menu, this}, "F2");
    file->add_item("Delete", {&FileExplorer::on_delete_menu, this}, "Del");
    file->add_item("New Folder", {&FileExplorer::on_new_folder, this});
    file->add_item("New File", {&FileExplorer::on_new_file, this});
    file->add_item("Properties", {&FileExplorer::on_properties_menu, this}, "Alt+Enter");
    file->add_separator();
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
    go->add_separator();
    go->add_item("Network", {&FileExplorer::on_network_menu, this});
    go->add_item("NFS", {&FileExplorer::on_nfs_menu, this});
    go->add_item("SFTP", {&FileExplorer::on_sftp_menu, this});
    go->add_item("FTPS", {&FileExplorer::on_ftps_menu, this});
    go->add_item("Connect to Server...", {&FileExplorer::on_connect_menu, this});
  }

  // The context menus.
  ctx_open_ = list_menu_.add_item("Open", {&FileExplorer::on_open_item, this}, "Enter");
  ctx_copy_files_ = list_menu_.add_item("Copy", {&FileExplorer::on_copy_menu, this}, "Ctrl+C");
  ctx_paste_ = list_menu_.add_item("Paste", {&FileExplorer::on_paste_menu, this}, "Ctrl+V");
  ctx_select_all_ = list_menu_.add_item("Select All", {&FileExplorer::on_select_all_menu, this}, "Ctrl+A");
  ctx_copy_ = list_menu_.add_item("Copy Path", {&FileExplorer::on_copy_path, this});
  list_menu_.add_separator();
  ctx_rename_ = list_menu_.add_item("Rename", {&FileExplorer::on_rename_menu, this}, "F2");
  ctx_delete_ = list_menu_.add_item("Delete", {&FileExplorer::on_delete_menu, this}, "Del");
  list_menu_.add_item("New Folder", {&FileExplorer::on_new_folder, this});
  list_menu_.add_item("New File", {&FileExplorer::on_new_file, this});
  list_menu_.add_separator();
  ctx_list_ = list_menu_.add_item("List", {&FileExplorer::on_view_list, this});
  ctx_icons_ = list_menu_.add_item("Icons", {&FileExplorer::on_view_icons, this});
  ctx_details_ = list_menu_.add_item("Details", {&FileExplorer::on_view_details, this});
  list_menu_.add_separator();
  ctx_up_ = list_menu_.add_item("Up One Level", {&FileExplorer::on_up_menu, this}, "Backspace");
  list_menu_.add_item("Refresh", {&FileExplorer::on_refresh_menu, this}, "F5");
  list_menu_.add_separator();
  list_menu_.add_item("Properties", {&FileExplorer::on_properties_menu, this}, "Alt+Enter");
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
  delete_dialog_.on_result({&FileExplorer::on_delete_answer, this});
  view_.on_item_activated({&FileExplorer::on_activated, this});
  view_.on_item_renamed({&FileExplorer::on_renamed, this});
  view_.on_selection_changed({&FileExplorer::on_selection, this});
  view_.set_drag_source(&FileExplorer::on_drag_item, this);

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
  last_status_ = read_sorted_directory(*fs_, path, &entries);
  if (last_status_ != Status::Ok) return false;

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
  if (!open_or_ask(child.view())) {
    if (last_status_ == Status::Ok || last_status_ == Status::NotFound) notify(gui::NoticeKind::Error, "The folder could not be opened");
  }
}

void FileExplorer::update_status() {
  scratch_.clear();
  scratch_.append_uint(view_.item_count());
  scratch_.append(view_.item_count() == 1 ? " item" : " items");
  const int32_t item = view_.selected_item();
  if (view_.selection_count() > 1) {
    scratch_.append("  |  ");
    scratch_.append_uint(view_.selection_count());
    scratch_.append(" selected");
  } else if (item >= 0) {
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

// The full paths of the selected items go on the clipboard, one per line.
bool FileExplorer::copy_selected() {
  if (view_.selection_count() <= 1) return copy_selected_path();
  if (!clipboard()) return false;
  String text(allocator()), name(allocator());
  uint32_t count = 0;
  for (uint32_t i = 0; i < view_.item_count(); ++i) {
    if (!view_.is_selected(i)) continue;
    view_.item_text(i, 0, &name);
    if ((count > 0 && !text.push_back('\n')) || !text.append(path_.view()) || (path_.size() > 1 && !text.push_back('/')) || !text.append(name.view())) return true;
    ++count;
  }
  if (clipboard()->set_text(text.view())) {
    String message(allocator());
    message.append("Copied ");
    message.append_uint(count);
    message.append(" items");
    status_.set_text(message.view());
  }
  return true;
}

void FileExplorer::select_all() {
  view_.select_all();
  update_status();
}

// Pastes the files and folders named on the clipboard into the folder shown.
bool FileExplorer::paste() {
  if (!fs_ || !clipboard()) return false;
  String text(allocator());
  if (!clipboard()->get_text(&text)) {
    status_.set_text("Nothing to paste.");
    return false;
  }
  if (path_.size() <= 1) {
    notify(NoticeKind::Error, "Files cannot be pasted into the list of drives.");
    return false;
  }
  uint32_t pasted = 0;
  Status failure = Status::Ok;
  String source(allocator()), name(allocator()), target(allocator()), message(allocator());
  const StringView all = text.view();
  for (uint64_t start = 0; start < all.size();) {
    uint64_t end = all.find('\n', start);
    if (end == StringView::npos) end = all.size();
    StringView line = all.substr(start, end - start);
    start = end + 1;
    if (line.size() > 0 && line[line.size() - 1] == '\r') line = line.substr(0, line.size() - 1);
    DirectoryEntry entry;
    if (line.size() < 2 || line[0] != '/' || fs_->stat(line, &entry) != Status::Ok) continue;
    const uint64_t slash = line.rfind('/');
    if (slash + 1 >= line.size()) continue;
    source.assign(line);
    name.assign(line.substr(slash + 1));
    // A folder cannot be pasted into itself.
    const StringView here = path_.view();
    if (here.size() >= source.size() && here.substr(0, source.size()).equals(source.view()) && (here.size() == source.size() || here[source.size()] == '/')) {
      message.assign("Cannot paste ");
      message.append(name.view());
      message.append(": a folder cannot be copied into itself.");
      notify(NoticeKind::Error, message.view());
      continue;
    }
    // The name in the folder: the same, or "Copy of ..." if it is taken.
    bool free_name = false;
    for (uint32_t attempt = 0; attempt < 100 && !free_name; ++attempt) {
      target.assign(here);
      target.push_back('/');
      if (attempt > 0) target.append("Copy of ");
      target.append(name.view());
      if (attempt > 1) {
        target.append(" (");
        target.append_uint(attempt);
        target.push_back(')');
      }
      DirectoryEntry taken;
      free_name = fs_->stat(target.view(), &taken) == Status::NotFound;
    }
    if (!free_name) continue;
    FileTransfer transfer(allocator(), *fs_);
    Status status = transfer.measure(source.view());
    if (status == Status::Ok) status = transfer.copy(source.view(), target.view());
    if (status != Status::Ok) {
      failure = status;
      continue;
    }
    ++pasted;
  }
  fs_->flush();
  fire(files_changed_);
  refresh();
  if (failure != Status::Ok) {
    message.assign("Cannot paste: ");
    message.append(describe_status(failure));
    notify(NoticeKind::Error, message.view());
  }
  if (pasted == 0) {
    if (failure == Status::Ok) status_.set_text("Nothing to paste.");
    return false;
  }
  message.assign("Pasted ");
  message.append_uint(pasted);
  message.append(pasted == 1 ? " item" : " items");
  status_.set_text(message.view());
  return true;
}

// The selected item's properties, or the folder's when nothing is selected.
bool FileExplorer::show_properties() {
  const int32_t item = view_.selected_item();
  if (item < 0) {
    properties_path_.assign(path_.view());
  } else {
    String name(allocator());
    view_.item_text(static_cast<uint32_t>(item), 0, &name);
    if (!properties_path_.assign(path_.view()) || (path_.size() > 1 && !properties_path_.push_back('/')) || !properties_path_.append(name.view())) return false;
  }
  fire(properties_);
  return true;
}

bool FileExplorer::on_key(const KeyArgs& args) {
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA + 2) return copy_selected();   // Ctrl+C
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA + 21) {   // Ctrl+V
    paste();
    return true;
  }
  if ((args.modifiers & hardware::modifier::kControl) && args.usage == hardware::key::kA) {   // Ctrl+A
    select_all();
    return true;
  }
  if ((args.modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) && args.usage == hardware::key::kEnter) {   // Alt+Enter
    show_properties();
    return true;
  }
  if (args.usage == hardware::key::kDelete) {
    delete_selected();
    return true;
  }
  if (args.usage == hardware::key::kF1 + 1) {  // F2
    rename_selected();
    return true;
  }
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
  target.assign(e->address_.typed_text().empty() ? e->address_.requested_path() : e->address_.typed_text());
  if (e->go_to(target.view())) return;
  if (e->last_status_ == Status::LoginRequired || e->last_status_ == Status::LoginFailed || e->last_status_ == Status::AccessDenied) return;   // (the dialog is up)
  String message(e->allocator());
  message.append("Cannot open ");
  message.append(target.view());
  if (e->last_status_ != Status::Ok && e->last_status_ != Status::NotFound) {
    message.append(": ");
    message.append(describe_status(e->last_status_));
  }
  e->notify(gui::NoticeKind::Error, message.view());
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool percent_decode(StringView in, String* out) {
  for (uint64_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%') {
      const int hi = i + 1 < in.size() ? hex_digit(in[i + 1]) : -1, lo = i + 2 < in.size() ? hex_digit(in[i + 2]) : -1;
      if (hi >= 0 && lo >= 0) {
        if (!out->push_back(static_cast<char>(hi * 16 + lo))) return false;
        i += 2;
        continue;
      }
    }
    if (!out->push_back(in[i])) return false;
  }
  return true;
}

bool FileExplorer::parse_network_address(allocators::IAllocator& allocator, StringView address, String* path, String* user, String* password) {
  address = StringView(address).trimmed();
  StringView rest;
  bool backslashes = false;
  const char* folder = kNetworkFolder;
  if (address.size() >= 6 && address.first(6).equals_ignore_case("smb://")) {
    rest = address.substr(6);
  } else if (address.size() >= 6 && address.first(6).equals_ignore_case("nfs://")) {
    rest = address.substr(6);
    folder = kNfsFolder;
  } else if (address.size() >= 7 && address.first(7).equals_ignore_case("sftp://")) {
    rest = address.substr(7);
    folder = kSftpFolder;
  } else if (address.size() >= 7 && address.first(7).equals_ignore_case("ftps://")) {
    rest = address.substr(7);
    folder = kFtpsFolder;
  } else if (address.size() >= 2 && address[0] == '\\' && address[1] == '\\') {   // \\server\share
    rest = address.substr(2);
    backslashes = true;
  } else {
    return false;
  }
  String text(allocator);
  for (char c : rest) {
    if (!text.push_back(backslashes && c == '\\' ? '/' : c)) return false;
  }
  StringView t = text.view();
  // user[:password]@ in front of the server (the last '@' before the first '/')
  const uint64_t slash = t.find('/');
  const StringView authority = slash == StringView::npos ? t : t.first(slash);
  const uint64_t at = authority.rfind('@');
  if (at != StringView::npos) {
    const StringView login = authority.first(at);
    const uint64_t colon = login.find(':');
    user->clear();
    password->clear();
    if (!percent_decode(colon == StringView::npos ? login : login.first(colon), user)) return false;
    if (colon != StringView::npos && !percent_decode(login.substr(colon + 1), password)) return false;
    t = t.substr(at + 1);
  }
  // Drop a trailing slash, collapse the rest into the tree's path.
  String tree(allocator);
  if (!tree.push_back('/') || !tree.append(folder)) return false;
  String decoded(allocator);
  if (!percent_decode(t, &decoded)) return false;
  uint64_t i = 0;
  const StringView d = decoded.view();
  while (i < d.size()) {
    while (i < d.size() && d[i] == '/') ++i;
    uint64_t j = i;
    while (j < d.size() && d[j] != '/') ++j;
    if (j > i && (!tree.push_back('/') || !tree.append(d.substr(i, j - i)))) return false;
    i = j;
  }
  return path->assign(tree.view());
}

// Which network folder a path is in: 1 for /Network (SMB), 2 for /NFS, 3 for /SFTP, 4 for /FTPS; *prefix gets the length of "/Network/" and the like
// (0 for none).
static int network_kind(StringView path, uint64_t* prefix) {
  static const char* const folders[] = {"/Network/", "/NFS/", "/SFTP/", "/FTPS/"};
  *prefix = 0;
  for (int i = 0; i < 4; ++i) {
    const StringView folder(folders[i]);
    if (path.size() > folder.size() && path.first(folder.size()).equals_ignore_case(folder)) {
      *prefix = folder.size();
      return i + 1;
    }
  }
  return 0;
}

FileExplorer::LoginFunction FileExplorer::login_of(int kind, void** context) const {
  switch (kind) {
    case 1: *context = login_context_; return login_;
    case 2: *context = nfs_login_context_; return nfs_login_;
    case 3: *context = sftp_login_context_; return sftp_login_;
    case 4: *context = ftps_login_context_; return ftps_login_;
    default: *context = nullptr; return nullptr;
  }
}

static StringView first_part(StringView rest) {
  const uint64_t slash = rest.find('/');
  return slash == StringView::npos ? rest : rest.first(slash);
}

// Opens a folder; a network server's request for a login is answered by asking the user. False if the folder was not opened.
bool FileExplorer::open_or_ask(StringView path) {
  uint64_t prefix = 0;
  const int kind = network_kind(path, &prefix);
  const bool network = kind != 0;
  if (network) status_.set_text("Connecting...");
  if (open_folder(path)) return true;
  void* unused = nullptr;
  if (network && (last_status_ == Status::LoginRequired || last_status_ == Status::LoginFailed || last_status_ == Status::AccessDenied) && login_of(kind, &unused)) {
    update_status();
    ask_login(path, last_status_);
  } else if (network) {
    update_status();
  }
  return false;
}

bool FileExplorer::go_to(StringView address) {
  String path(allocator()), user(allocator()), password(allocator());
  if (parse_network_address(allocator(), address, &path, &user, &password)) {
    uint64_t prefix = 0;
    const int kind = network_kind(path.view(), &prefix);
    void* context = nullptr;
    LoginFunction login = login_of(kind, &context);
    if ((!user.empty() || !password.empty()) && login) {   // a login in the address itself
      const StringView server = first_part(path.view().substr(prefix));
      if (!server.empty()) login(context, server, user.view(), password.view());
    }
    return open_or_ask(path.view());
  }
  String plain(allocator());
  if (!plain.assign(address)) return false;
  return open_or_ask(plain.view());
}

void FileExplorer::ask_login(StringView path, Status why) {
  if (!manager()) return;
  uint64_t prefix = 0;
  const int kind = network_kind(path, &prefix);
  if (kind == 0) return;
  const StringView server = first_part(path.substr(prefix));
  if (server.empty()) return;
  pending_path_.assign(path);
  const char* note = kind == 2 ? (why == Status::AccessDenied ? "Access was denied. Try other user and group ids." : "")
                               : why == Status::LoginFailed ? "The user name or password was not accepted." : why == Status::AccessDenied ? "Access was denied. Try another account." : "";
  credentials_dialog_.show(*manager(), server, note, kind == 2 ? last_uid_.view() : last_user_.view(), {&FileExplorer::on_login_answer, this},
                           kind == 2 ? CredentialsDialog::Style::UnixIds : CredentialsDialog::Style::Login, kind == 2 ? last_gid_.view() : StringView());
}

void FileExplorer::on_login_answer(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  CredentialsDialog& d = e->credentials_dialog_;
  uint64_t prefix = 0;
  const int kind = network_kind(e->pending_path_.view(), &prefix);
  void* context = nullptr;
  LoginFunction login = e->login_of(kind, &context);
  if (!d.accepted() || !login) {
    d.clear();
    return;
  }
  if (kind == 2) {
    e->last_uid_.assign(d.user());
    e->last_gid_.assign(d.password());
  } else {
    e->last_user_.assign(d.user());
  }
  login(context, d.server(), d.user(), d.password());
  d.clear();
  String path(e->allocator());
  path.assign(e->pending_path_.view());
  e->open_or_ask(path.view());
}

void FileExplorer::on_connect_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->address_.begin_edit("smb://"); }
void FileExplorer::on_network_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->open_or_ask("/Network"); }
void FileExplorer::on_nfs_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->open_or_ask("/NFS"); }
void FileExplorer::on_sftp_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->open_or_ask("/SFTP"); }
void FileExplorer::on_ftps_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->open_or_ask("/FTPS"); }

// The list's menu is enabled for what is selected and shows the view in use.
void FileExplorer::on_list_menu_opening(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  const bool selected = e->view_.selected_item() >= 0;
  e->ctx_open_->set_enabled(selected);
  e->ctx_copy_->set_enabled(selected);
  e->ctx_copy_files_->set_enabled(selected);
  e->ctx_select_all_->set_enabled(e->view_.item_count() > 0);
  e->ctx_paste_->set_enabled(e->path_.size() > 1 && e->clipboard() && e->clipboard()->has_text());
  e->ctx_rename_->set_enabled(selected);
  e->ctx_delete_->set_enabled(selected);
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
// --- Drag and drop

bool FileExplorer::on_drag_item(void* self, int32_t item, String* text) {
  auto* e = static_cast<FileExplorer*>(self);
  const ItemIcon icon = e->view_.item_icon(static_cast<uint32_t>(item));
  if (icon == ItemIcon::Drive || (e->path_.size() <= 1 && icon != ItemIcon::File)) return false;   // the volumes and mounted folders of the root stay
  String name(e->allocator());
  e->view_.item_text(static_cast<uint32_t>(item), 0, &name);
  return text->assign(e->path_.view()) && (e->path_.size() <= 1 || text->push_back('/')) && text->append(name.view());
}

bool FileExplorer::accepts_drop(StringView text, int32_t, int32_t, const Control*) const {
  return fs_ && !text.empty() && text[0] == '/';
}

void FileExplorer::on_drop(StringView text, int32_t x, int32_t y, const Control*) {
  // Dropped on a folder of the list: into that folder. Otherwise into the folder shown.
  String folder(allocator());
  folder.assign(path_.view());
  const Point mine = window_origin(), theirs = view_.window_origin();
  const int32_t item = view_.item_at_point(x + mine.x - theirs.x, y + mine.y - theirs.y);
  if (item >= 0 && view_.item_icon(static_cast<uint32_t>(item)) == ItemIcon::Folder) {
    String name(allocator());
    view_.item_text(static_cast<uint32_t>(item), 0, &name);
    if (path_.size() > 1) folder.push_back('/');
    folder.append(name.view());
  }
  move_into(text, folder.view());
}

bool FileExplorer::move_into(StringView source, StringView folder) {
  if (!fs_) {
    notify(NoticeKind::Error, "No storage available.");
    return false;
  }
  const uint64_t slash = source.rfind('/');
  if (slash == StringView::npos || slash + 1 >= source.size()) return false;
  const StringView name = source.substr(slash + 1);
  const StringView source_parent = slash == 0 ? StringView("/") : source.substr(0, slash);
  if (source_parent.equals(folder)) return true;   // already there
  String target(allocator());
  target.assign(folder);
  if (folder.size() > 1) target.push_back('/');
  target.append(name);
  String message(allocator());
  auto fail = [&](StringView why) {
    message.clear();
    message.append("Cannot move ");
    message.append(name);
    message.append(": ");
    message.append(why);
    notify(NoticeKind::Error, message.view());
    return false;
  };
  if (folder.size() >= source.size() && folder.substr(0, source.size()).equals(source) &&
      (folder.size() == source.size() || folder[source.size()] == '/'))
    return fail("a folder cannot be moved into itself.");
  if (folder.size() <= 1) return fail("the list of drives cannot be changed.");
  DirectoryEntry taken;
  if (fs_->stat(target.view(), &taken) != Status::NotFound) return fail("an item of that name is already in the folder.");
  Status status = fs_->rename(source, target.view());
  if (status == Status::Unsupported) {   // another volume: copy, then remove the original
    if (transfers_ && manager()) {   // in the background, with a window that shows how far it is
      if (!transfers_->start(*manager(), *fs_, source, target.view(), /*remove_source=*/true))
        return fail("another copy is still running.");
      return true;   // the dialog refreshes the explorers when it is done
    }
    FileTransfer transfer(allocator(), *fs_);
    status = transfer.measure(source);
    if (status == Status::Ok) status = transfer.copy(source, target.view());
    if (status == Status::Ok) status = remove_tree(source, 0);
  }
  fs_->flush();
  fire(files_changed_);
  refresh();
  if (status != Status::Ok) return fail(describe_status(status));
  return true;
}

void FileExplorer::copy_settings_from(const FileExplorer& o) {
  login_ = o.login_;
  login_context_ = o.login_context_;
  nfs_login_ = o.nfs_login_;
  nfs_login_context_ = o.nfs_login_context_;
  sftp_login_ = o.sftp_login_;
  sftp_login_context_ = o.sftp_login_context_;
  ftps_login_ = o.ftps_login_;
  ftps_login_context_ = o.ftps_login_context_;
  eject_ = o.eject_;
  eject_context_ = o.eject_context_;
  transfers_ = o.transfers_;
}

void FileExplorer::on_open_item(void* self, Control&) { static_cast<FileExplorer*>(self)->activate_selected(); }
void FileExplorer::on_copy_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->copy_selected(); }
void FileExplorer::on_paste_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->paste(); }
void FileExplorer::on_select_all_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->select_all(); }
void FileExplorer::on_properties_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->show_properties(); }
void FileExplorer::on_copy_path(void* self, Control&) { static_cast<FileExplorer*>(self)->copy_selected_path(); }
bool FileExplorer::new_folder() { return create_new(true); }
bool FileExplorer::new_file() { return create_new(false); }

bool FileExplorer::create_new(bool folder) {
  if (!fs_) {
    notify(gui::NoticeKind::Error, "No storage available.");
    return false;
  }
  const StringView base = folder ? StringView("New Folder") : StringView("New File");
  const StringView extension = folder ? StringView() : StringView(".txt");
  String name(allocator()), full(allocator());
  Status status = Status::Ok;
  for (uint32_t n = 1; n < 1000; ++n) {
    name.assign(base);
    if (n > 1) {
      name.push_back(' ');
      name.append_uint(n);
    }
    name.append(extension);
    full.assign(path_.view());
    if (path_.size() > 1) full.push_back('/');
    full.append(name.view());
    DirectoryEntry taken;
    if (fs_->stat(full.view(), &taken) == Status::NotFound) break;
    if (n == 999) {
      notify(gui::NoticeKind::Error, "There are too many items called New in this folder.");
      return false;
    }
  }
  if (folder) {
    status = fs_->create_directory(full.view());
  } else {
    UniquePointer<IFile> file;
    status = fs_->open_file(full.view(), OpenMode::CreateTruncate, &file);
  }
  if (status != Status::Ok) {
    String message(allocator());
    message.append(folder ? "Cannot create the folder here: " : "Cannot create the file here: ");
    message.append(describe_status(status));
    notify(gui::NoticeKind::Error, message.view());
    return false;
  }
  fs_->flush();
  refresh();
  for (uint32_t i = 0; i < view_.item_count(); ++i) {   // select what was made
    String text(allocator());
    view_.item_text(i, 0, &text);
    if (text.view().equals(name.view())) {
      view_.select_item(static_cast<int32_t>(i));
      view_.begin_rename();   // the name can be typed right away
      break;
    }
  }
  return true;
}

bool FileExplorer::rename_selected() { return view_.begin_rename(); }

bool FileExplorer::rename_item(StringView from, StringView to) {
  String target(allocator());
  target.assign(to);
  // Trim blanks around the new name.
  uint64_t begin = 0, end = target.size();
  while (begin < end && target[begin] == ' ') ++begin;
  while (end > begin && target[end - 1] == ' ') --end;
  String name(allocator());
  name.assign(target.view().substr(begin, end - begin));
  if (name.view().equals(from)) return true;   // unchanged
  if (name.empty()) {
    notify(gui::NoticeKind::Warning, "Enter a name.");
    return false;
  }
  if (name.view().find('/') != StringView::npos) {
    notify(gui::NoticeKind::Error, "A name cannot contain a slash.");
    return false;
  }
  if (!fs_) {
    notify(gui::NoticeKind::Error, "No storage available.");
    return false;
  }
  String old_path(allocator()), new_path(allocator());
  for (int which = 0; which < 2; ++which) {
    String& out = which == 0 ? old_path : new_path;
    out.assign(path_.view());
    if (path_.size() > 1) out.push_back('/');
    out.append(which == 0 ? from : name.view());
  }
  const Status status = fs_->rename(old_path.view(), new_path.view());
  if (status != Status::Ok) {
    String message(allocator());
    message.append("Cannot rename ");
    message.append(from);
    message.append(": ");
    message.append(describe_status(status));
    notify(gui::NoticeKind::Error, message.view());
    return false;
  }
  fs_->flush();
  refresh();
  for (uint32_t i = 0; i < view_.item_count(); ++i) {
    String text(allocator());
    view_.item_text(i, 0, &text);
    if (text.view().equals(name.view())) {
      view_.select_item(static_cast<int32_t>(i));
      break;
    }
  }
  return true;
}

bool FileExplorer::delete_selected() {
  const int32_t item = view_.selected_item();
  if (item < 0 || !manager() || view_.renaming()) return false;
  String name(allocator());
  view_.item_text(static_cast<uint32_t>(item), 0, &name);
  const bool folder = view_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::File;
  delete_path_.assign(path_.view());
  if (path_.size() > 1) delete_path_.push_back('/');
  delete_path_.append(name.view());
  String question(allocator());
  question.append("Delete \"");
  if (name.size() > 28) {
    question.append(name.view().first(25));
    question.append("...");
  } else {
    question.append(name.view());
  }
  question.append("\"?");
  delete_dialog_.set_texts(folder ? "Delete Folder" : "Delete File", question.view(),
                           folder ? "It and everything in it will be deleted." : "This cannot be undone.", "Delete", "Cancel");
  return delete_dialog_.show(*manager());
}

// Removes a file, or a folder after what is in it. Stops at the first failure.
Status FileExplorer::remove_tree(StringView path, uint32_t depth) { return FileTransfer::remove_tree(allocator(), *fs_, path, depth); }

void FileExplorer::on_delete_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->delete_selected(); }

void FileExplorer::on_delete_answer(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  if (!e->delete_dialog_.accepted() || !e->fs_) return;
  const Status status = e->remove_tree(e->delete_path_.view(), 0);
  e->fs_->flush();
  e->refresh();
  if (status != Status::Ok) {
    String message(e->allocator());
    message.append("Cannot delete ");
    message.append(e->delete_path_.view().substr(e->delete_path_.view().rfind('/') + 1));
    message.append(": ");
    message.append(describe_status(status));
    e->notify(gui::NoticeKind::Error, message.view());
  }
}

void FileExplorer::on_rename_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->rename_selected(); }

void FileExplorer::on_renamed(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  const int32_t item = e->view_.renamed_item();
  if (item < 0) return;
  String from(e->allocator()), to(e->allocator());
  e->view_.item_text(static_cast<uint32_t>(item), 0, &from);
  to.assign(e->view_.renamed_text());
  e->rename_item(from.view(), to.view());
}

void FileExplorer::on_new_folder(void* self, Control&) { static_cast<FileExplorer*>(self)->new_folder(); }
void FileExplorer::on_new_file(void* self, Control&) { static_cast<FileExplorer*>(self)->new_file(); }

void FileExplorer::on_eject_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->eject_selected(); }
void FileExplorer::on_close_window(void* self, Control&) { static_cast<FileExplorer*>(self)->close(); }
void FileExplorer::on_up_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->go_up(); }
void FileExplorer::on_refresh_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->refresh(); }
void FileExplorer::on_address_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->address_.begin_edit(); }

void FileExplorer::on_activated(void* self, Control&) { static_cast<FileExplorer*>(self)->activate_selected(); }
void FileExplorer::on_selection(void* self, Control&) { static_cast<FileExplorer*>(self)->update_status(); }

}  // namespace desktop
