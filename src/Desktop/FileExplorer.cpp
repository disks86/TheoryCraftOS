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
#include "Gui/Window.hpp"
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

namespace {

// Cut and Paste between explorers. The clipboard holds text only - the paths of the items, one per line - so "these were cut, not copied" is
// kept here: the hash of the text that Cut put on the clipboard (0: nothing is cut). Paste moves the items when the clipboard still holds
// exactly that text and copies them otherwise; Copy, and a paste that used it, clear it. One word, read and written atomically, shared by
// every explorer window.
uint64_t g_cut_text_hash = 0;

uint64_t text_hash(StringView text) {   // FNV-1a, never 0
  uint64_t h = 1469598103934665603ull;
  for (char c : text) h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
  return h ? h : 1;
}

StringView name_of(StringView path) { return path.substr(path.rfind('/') + 1); }

// A name as a message shows it: long ones are cut short so that the line still fits the dialog.
void shown_name(StringView name, String* out) {
  out->clear();
  if (name.size() > 28) {
    out->append(name.first(25));
    out->append("...");
  } else {
    out->append(name);
  }
}

// Takes the next line off `rest` (without its line break).
StringView next_line(StringView* rest) {
  const uint64_t end = rest->find('\n');
  StringView line = end == StringView::npos ? *rest : rest->first(end);
  *rest = end == StringView::npos ? StringView() : rest->without_prefix(end + 1);
  if (!line.empty() && line[line.size() - 1] == '\r') line = line.first(line.size() - 1);
  return line;
}

bool is_inside(StringView folder, StringView of) {   // `folder` is `of` or somewhere below it
  return folder.size() >= of.size() && folder.first(of.size()).equals(of) && (folder.size() == of.size() || folder[of.size()] == '/');
}

}  // namespace


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
      delete_paths_(allocator),
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
  // File, Edit, View, Go, Help (docs/UserInterfaceGuidelines.md, 8.1). Items that do not apply to the selection are disabled when a menu opens.
  if (Menu* file = menu_.add_menu("File")) {
    file->add_item("New Folder", {&FileExplorer::on_new_folder, this});
    file->add_item("New Text Document", {&FileExplorer::on_new_file, this});
    open_item_ = file->add_item("Open", {&FileExplorer::on_open_item, this}, "Enter");
    file->add_separator();
    rename_item_ = file->add_item("Rename", {&FileExplorer::on_rename_menu, this}, "F2");
    properties_item_ = file->add_item("Properties", {&FileExplorer::on_properties_menu, this}, "Alt+Enter");
    file->add_separator();
    eject_item_ = file->add_item("Eject", {&FileExplorer::on_eject_menu, this});
    file->add_separator();
    file->add_item("Close", {&FileExplorer::on_close_window, this});   // (closing the window ends nothing: the files stay)
  }
  if (Menu* edit = menu_.add_menu("Edit")) {
    cut_item_ = edit->add_item("Cut", {&FileExplorer::on_cut_menu, this}, "Ctrl+X");
    copy_item_ = edit->add_item("Copy", {&FileExplorer::on_copy_menu, this}, "Ctrl+C");
    paste_item_ = edit->add_item("Paste", {&FileExplorer::on_paste_menu, this}, "Ctrl+V");
    edit->add_separator();
    delete_item_ = edit->add_item("Delete", {&FileExplorer::on_delete_menu, this}, "Del");
    select_all_item_ = edit->add_item("Select All", {&FileExplorer::on_select_all_menu, this}, "Ctrl+A");
    edit->add_separator();
    copy_path_item_ = edit->add_item("Copy Path", {&FileExplorer::on_copy_path, this});
  }
  if (Menu* view = menu_.add_menu("View")) {
    list_item_ = view->add_item("List", {&FileExplorer::on_view_list, this});
    icons_item_ = view->add_item("Icons", {&FileExplorer::on_view_icons, this});
    details_item_ = view->add_item("Details", {&FileExplorer::on_view_details, this});
    view->add_separator();
    view->add_item("Refresh", {&FileExplorer::on_refresh_menu, this}, "F5");
  }
  if (Menu* go = menu_.add_menu("Go")) {
    up_item_ = go->add_item("Up One Level", {&FileExplorer::on_up_menu, this}, "Backspace");
    go->add_item("Go To Address...", {&FileExplorer::on_address_menu, this}, "Ctrl+L");
    go->add_separator();
    go->add_item("Network", {&FileExplorer::on_network_menu, this});
    go->add_item("NFS", {&FileExplorer::on_nfs_menu, this});
    go->add_item("SFTP", {&FileExplorer::on_sftp_menu, this});
    go->add_item("FTPS", {&FileExplorer::on_ftps_menu, this});
    go->add_item("Connect to Server...", {&FileExplorer::on_connect_menu, this});
  }
  if (Menu* help = menu_.add_menu("Help")) help->add_item("About File Explorer", {&FileExplorer::on_about, this});
  menu_.on_opening(Handler::immediate(&FileExplorer::on_menu_opening, this));

  // The context menus. The list's menu is filled when it opens: for the selection, or for the folder when the click was on empty space.
  build_list_menu(false);
  list_menu_.on_opening(Handler::immediate(&FileExplorer::on_list_menu_opening, this));
  address_menu_.add_item("Copy Address", {&FileExplorer::on_copy_address, this});
  address_menu_.add_item("Edit Address", {&FileExplorer::on_address_menu, this});
  address_.set_context_menu(&address_menu_);
  view_.set_context_menu(&list_menu_);
  view_.set_extended_selection(true);   // Shift+click, Ctrl+click, Shift+arrows, Ctrl+arrows and Space (guidelines, section 5)

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
  view_.add_column("Modified", 130);
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
  update_commands();
}

// The list's context menu (guidelines, 8.2). On the selection: the primary command first, then the transfer commands, then the others, and
// Properties last. On empty space: the commands of the folder. Every command is also in the menu bar.
void FileExplorer::build_list_menu(bool on_items) {
  Menu& m = list_menu_.menu();
  while (m.item_count() > 0)
    if (!m.remove_item(m.item(0))) break;
  ctx_open_ = ctx_cut_ = ctx_copy_files_ = ctx_paste_ = ctx_select_all_ = ctx_rename_ = ctx_delete_ = ctx_copy_ = ctx_up_ = ctx_list_ = ctx_icons_ = ctx_details_ = ctx_properties_ = nullptr;
  if (on_items) {
    ctx_open_ = list_menu_.add_item("Open", {&FileExplorer::on_open_item, this});
    list_menu_.add_separator();
    ctx_cut_ = list_menu_.add_item("Cut", {&FileExplorer::on_cut_menu, this});
    ctx_copy_files_ = list_menu_.add_item("Copy", {&FileExplorer::on_copy_menu, this});
    list_menu_.add_separator();
    ctx_rename_ = list_menu_.add_item("Rename", {&FileExplorer::on_rename_menu, this});
    ctx_delete_ = list_menu_.add_item("Delete", {&FileExplorer::on_delete_menu, this});
    ctx_copy_ = list_menu_.add_item("Copy Path", {&FileExplorer::on_copy_path, this});
  } else {
    ctx_paste_ = list_menu_.add_item("Paste", {&FileExplorer::on_paste_menu, this});
    list_menu_.add_separator();
    list_menu_.add_item("New Folder", {&FileExplorer::on_new_folder, this});
    list_menu_.add_item("New Text Document", {&FileExplorer::on_new_file, this});
    list_menu_.add_separator();
    ctx_select_all_ = list_menu_.add_item("Select All", {&FileExplorer::on_select_all_menu, this});
    list_menu_.add_separator();
    ctx_list_ = list_menu_.add_item("List", {&FileExplorer::on_view_list, this});
    ctx_icons_ = list_menu_.add_item("Icons", {&FileExplorer::on_view_icons, this});
    ctx_details_ = list_menu_.add_item("Details", {&FileExplorer::on_view_details, this});
    list_menu_.add_separator();
    ctx_up_ = list_menu_.add_item("Up One Level", {&FileExplorer::on_up_menu, this});
    list_menu_.add_item("Refresh", {&FileExplorer::on_refresh_menu, this});
  }
  list_menu_.add_separator();
  ctx_properties_ = list_menu_.add_item("Properties", {&FileExplorer::on_properties_menu, this});
  list_menu_on_items_ = on_items;
}

// --- The selection --------------------------------------------------------------------

// The one selected item, or -1 when none or several are.
int32_t FileExplorer::single_item() const {
  if (view_.selection_count() != 1) return -1;
  for (uint32_t i = 0; i < view_.item_count(); ++i)
    if (view_.is_selected(i)) return static_cast<int32_t>(i);
  return -1;
}

// The full paths of the selected items, one per line; how many there are.
uint32_t FileExplorer::selected_paths(String* out) const {
  out->clear();
  uint32_t count = 0;
  String name(allocator());
  for (uint32_t i = 0; i < view_.item_count(); ++i) {
    if (!view_.is_selected(i)) continue;
    view_.item_text(i, 0, &name);
    if ((count > 0 && !out->push_back('\n')) || !out->append(path_.view()) || (path_.size() > 1 && !out->push_back('/')) || !out->append(name.view())) return 0;
    ++count;
  }
  return count;
}

// Whether the selection can be cut, renamed or deleted: something is selected, and no drive is among it.
bool FileExplorer::selection_can_change() const {
  if (view_.selection_count() == 0) return false;
  for (uint32_t i = 0; i < view_.item_count(); ++i)
    if (view_.is_selected(i) && view_.item_icon(i) == ItemIcon::Drive) return false;
  return true;
}

bool FileExplorer::can_paste() const { return fs_ && path_.size() > 1 && clipboard() && clipboard()->has_text(); }

bool FileExplorer::can_eject() const {
  if (!fs_ || !eject_) return false;
  if (path_.size() > 1) return true;
  const int32_t item = single_item();
  return item >= 0 && view_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::File;
}

// A command that does not apply is disabled, not left to fail with a message (guidelines, 8.4). Run before a menu opens.
void FileExplorer::update_commands() {
  const uint32_t count = view_.selection_count();
  const bool one = count == 1, changeable = selection_can_change();
  auto enable = [](MenuItem* item, bool on) { if (item) item->set_enabled(on); };
  enable(open_item_, one);
  enable(ctx_open_, one);
  enable(rename_item_, one && changeable);
  enable(ctx_rename_, one && changeable);
  enable(properties_item_, count <= 1);   // (of the item, or of the folder shown)
  enable(ctx_properties_, count <= 1);
  enable(eject_item_, can_eject());
  enable(cut_item_, changeable);
  enable(ctx_cut_, changeable);
  enable(copy_item_, count > 0);
  enable(ctx_copy_files_, count > 0);
  enable(paste_item_, can_paste());
  enable(ctx_paste_, can_paste());
  enable(delete_item_, changeable);
  enable(ctx_delete_, changeable);
  enable(select_all_item_, view_.item_count() > 0);
  enable(ctx_select_all_, view_.item_count() > 0);
  enable(copy_path_item_, count > 0);
  enable(ctx_copy_, count > 0);
  enable(up_item_, path_.size() > 1);
  enable(ctx_up_, path_.size() > 1);
  if (ctx_list_) ctx_list_->set_checked(view_.view_mode() == ViewMode::List);
  if (ctx_icons_) ctx_icons_->set_checked(view_.view_mode() == ViewMode::Icons);
  if (ctx_details_) ctx_details_->set_checked(view_.view_mode() == ViewMode::Details);
}

// The title names the folder shown, as the list of its parent shows it; the list of drives has no name of its own.
void FileExplorer::update_title() {
  if (path_.size() <= 1) {
    set_title("File Explorer");
    return;
  }
  String title(allocator());
  if (format_message(&title, "%1 - File Explorer", name_of(path_.view()))) set_title(title.view());
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
  String size(allocator()), type(allocator()), modified(allocator());
  for (const DirectoryEntry& e : entries) {
    const bool folder = e.type == NodeType::Directory;
    if (folder) size.clear();
    else format_size(e.size, &size);
    describe_entry_type(e, &type);
    format_unix_time(e.modified, &modified);
    const StringView cells[4] = {e.name.view(), size.view(), type.view(), modified.view()};
    view_.add_item(cells, 4, folder ? ItemIcon::Folder : ItemIcon::File);
  }
  address_.set_path(path_.view());
  update_title();
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
  if (!can_eject()) return false;   // (the menu item is disabled then)
  String volume(allocator());
  {
    kernel::ScopedMutex guard(navigation_);
    if (path_.size() <= 1) {   // the root lists the drives: the selected one
      view_.item_text(static_cast<uint32_t>(single_item()), 0, &volume);
    } else {                   // inside a drive: the first folder of the path
      const StringView p = path_.view();
      uint64_t end = p.find('/', 1);
      if (end == StringView::npos) end = p.size();
      volume.assign(p.substr(1, end - 1));
    }
  }
  bool succeeded = true;
  const char* message = eject_(eject_context_, volume.view(), &succeeded);
  refresh();   // (the drive is gone from the root, and a folder on it from the view)
  if (message) notify(succeeded ? gui::NoticeKind::Information : gui::NoticeKind::Error, message, volume.view());
  return true;
}

void FileExplorer::activate_selected() {
  const int32_t item = single_item();
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
    if (last_status_ == Status::LoginRequired || last_status_ == Status::LoginFailed) return;   // (the sign-in dialog is up, or there is nobody to ask)
    String message(allocator());
    describe_failure(&message, "\"%1\" could not be opened because %2.", name.view(), last_status_ == Status::Ok ? Status::NotFound : last_status_);
    notify(gui::NoticeKind::Error, message.view());
  }
}

void FileExplorer::update_status() {
  String count(allocator()), number(allocator());
  number.append_uint(view_.item_count());
  format_message(&count, view_.item_count() == 1 ? "%1 item" : "%1 items", number.view());
  const int32_t item = single_item();
  if (view_.selection_count() > 1) {
    number.clear();
    number.append_uint(view_.selection_count());
    format_message(&scratch_, "%1  |  %2 selected", count.view(), number.view());
  } else if (item >= 0) {
    String name(allocator()), size(allocator());
    view_.item_text(static_cast<uint32_t>(item), 0, &name);
    view_.item_text(static_cast<uint32_t>(item), 1, &size);
    format_message(&scratch_, size.empty() ? "%1  |  %2" : "%1  |  %2, %3", count.view(), name.view(), size.view());
  } else {
    scratch_.assign(count.view());
  }
  status_.set_text(scratch_.view());
}

// What the status bar says after Cut, Copy or Copy Path of `count` items (`paths` is what went on the clipboard).
void FileExplorer::say_transferred(const char* one, const char* several, StringView paths, uint32_t count) {
  String message(allocator()), number(allocator());
  number.append_uint(count);
  if (count == 1) format_message(&message, one, name_of(paths));
  else format_message(&message, several, number.view());
  status_.set_text(message.view());
}

// Puts the full paths of the selected items on the clipboard as text, one per line. False if nothing is selected or there is no clipboard.
bool FileExplorer::copy_selected_path() {
  String text(allocator());
  const uint32_t count = selected_paths(&text);
  if (count == 0 || !clipboard()) return false;
  if (clipboard()->set_text(text.view())) {
    __atomic_store_n(&g_cut_text_hash, uint64_t{0}, __ATOMIC_RELEASE);
    say_transferred("Copied the path of \"%1\".", "Copied the paths of %1 items.", text.view(), count);
  }
  return true;
}

// Copy: the selected items go on the clipboard (as their paths), to be pasted into another folder.
bool FileExplorer::copy_selected() {
  String text(allocator());
  const uint32_t count = selected_paths(&text);
  if (count == 0 || !clipboard()) return false;
  if (clipboard()->set_text(text.view())) {
    __atomic_store_n(&g_cut_text_hash, uint64_t{0}, __ATOMIC_RELEASE);
    say_transferred("Copied \"%1\". Paste it where the copy should go.", "Copied %1 items. Paste them where the copies should go.", text.view(), count);
  }
  return true;
}

// Cut: like Copy, but Paste then moves the items. They stay where they are until they are pasted.
bool FileExplorer::cut_selected() {
  if (!selection_can_change() || !clipboard()) return false;
  String text(allocator());
  const uint32_t count = selected_paths(&text);
  if (count == 0) return false;
  if (clipboard()->set_text(text.view())) {
    __atomic_store_n(&g_cut_text_hash, text_hash(text.view()), __ATOMIC_RELEASE);
    say_transferred("Cut \"%1\". It moves when you paste it.", "Cut %1 items. They move when you paste them.", text.view(), count);
  }
  return true;
}

void FileExplorer::select_all() {
  view_.select_all();
  update_status();
}

// Pastes the files and folders named on the clipboard into the folder shown: copies of them, or the items themselves after Cut.
bool FileExplorer::paste() {
  if (!can_paste()) return false;   // (the command is disabled then)
  String text(allocator());
  if (!clipboard()->get_text(&text)) return false;
  const uint64_t hash = text_hash(text.view());
  const bool move = __atomic_load_n(&g_cut_text_hash, __ATOMIC_ACQUIRE) == hash;
  const bool done = transfer_into(text.view(), path_.view(), move, /*from_paste=*/true);
  if (done && move) __atomic_store_n(&g_cut_text_hash, uint64_t{0}, __ATOMIC_RELEASE);   // (moved: there is nothing left to paste again)
  return done;
}

// Copies or moves the items whose full paths are the lines of `sources` into `folder`. What can be done at once (a move within one volume)
// is; the rest runs in the progress window when there is one (so that it can be watched and cancelled), else here. A copy next to an item
// of the same name is called "Copy of <name>", then "Copy 2 of <name>"; a move never replaces anything. At most one message says what went
// wrong. False if nothing was transferred or started.
bool FileExplorer::transfer_into(StringView sources, StringView folder, bool move, bool from_paste) {
  if (!fs_) return false;
  String message(allocator()), shown(allocator());
  auto refuse = [&](StringView pattern, StringView name) {   // the first problem is the one reported
    if (!message.empty()) return;
    shown_name(name, &shown);
    format_message(&message, pattern, shown.view());
  };
  String batch_sources(allocator()), batch_targets(allocator()), target(allocator()), number(allocator());
  uint32_t done = 0, batched = 0, lines = 0;
  Status failure = Status::Ok;
  String failed_name(allocator());
  StringView rest = sources;
  while (!rest.empty()) {
    const StringView source = next_line(&rest);
    DirectoryEntry entry;
    if (source.size() < 2 || source[0] != '/') continue;
    ++lines;
    const uint64_t slash = source.rfind('/');
    if (slash + 1 >= source.size() || fs_->stat(source, &entry) != Status::Ok) continue;
    const StringView name = source.substr(slash + 1);
    const StringView parent = slash == 0 ? StringView("/") : source.first(slash);
    if (move && parent.equals(folder)) {   // already there
      ++done;
      continue;
    }
    if (is_inside(folder, source)) {
      refuse(move ? "\"%1\" cannot be moved into itself. Choose another folder." : "\"%1\" cannot be copied into itself. Choose another folder.", name);
      continue;
    }
    if (folder.size() <= 1) {
      refuse("\"%1\" cannot be put in the list of drives. Open a drive first.", name);
      continue;
    }
    // The name in the folder: the same, or for a copy "Copy of ...", "Copy 2 of ..." if it is taken.
    bool free_name = false;
    for (uint32_t attempt = 0; attempt < 100 && !free_name; ++attempt) {
      number.clear();
      number.append_uint(attempt);
      String copy_name(allocator());
      format_message(&copy_name, attempt == 0 ? "%1" : attempt == 1 ? "Copy of %1" : "Copy %2 of %1", name, number.view());
      target.assign(folder);
      target.push_back('/');
      target.append(copy_name.view());
      DirectoryEntry taken;
      free_name = fs_->stat(target.view(), &taken) == Status::NotFound;
      if (move) break;
    }
    if (!free_name) {
      refuse(move ? "\"%1\" was not moved because the folder already has an item with that name. Rename one of them and try again."
                  : "\"%1\" was not copied because the folder already has too many copies of it.", name);
      continue;
    }
    if (move) {
      const Status status = fs_->rename(source, target.view());
      if (status == Status::Ok) {
        ++done;
        continue;
      }
      if (status != Status::Unsupported) {   // (Unsupported: another volume - copy, then remove the original)
        if (failure == Status::Ok) {
          failure = status;
          failed_name.assign(name);
        }
        continue;
      }
    }
    if ((batched > 0 && (!batch_sources.push_back('\n') || !batch_targets.push_back('\n'))) || !batch_sources.append(source) || !batch_targets.append(target.view())) return false;
    ++batched;
  }
  bool started = false;
  if (batched > 0 && transfers_ && manager()) {   // in the background, with a window that shows how far it is and can cancel it
    started = transfers_->start_batch(*manager(), *fs_, batch_sources.view(), batch_targets.view(), move);
    if (!started && message.empty()) message.assign("Another copy is still running. Wait for it to finish, then try again.");
  } else if (batched > 0) {
    FileTransfer transfer(allocator(), *fs_);
    StringView from = batch_sources.view(), to = batch_targets.view();
    while (!from.empty()) {
      const StringView source = next_line(&from), destination = next_line(&to);
      Status status = transfer.measure(source);
      if (status == Status::Ok) status = transfer.copy(source, destination);
      if (status == Status::Ok && move) status = remove_tree(source, 0);
      if (status == Status::Ok) {
        ++done;
      } else if (failure == Status::Ok) {
        failure = status;
        failed_name.assign(name_of(source));
      }
    }
  }
  fs_->flush();
  if (done > 0) fire(files_changed_);
  refresh();   // (the progress window's owner refreshes again when a background transfer ends)
  if (message.empty() && failure != Status::Ok) {
    shown_name(failed_name.view(), &shown);
    describe_failure(&message, move ? "\"%1\" could not be moved because %2." : "\"%1\" could not be copied because %2.", shown.view(), failure);
  }
  if (message.empty() && done == 0 && !started && from_paste)   // (text that names no file: nothing an explorer can paste)
    message.assign(lines == 0 ? "The clipboard does not hold files or folders, so there is nothing to paste here."
                              : "The items on the clipboard are no longer where they were, so they cannot be pasted.");
  if (!message.empty()) notify(NoticeKind::Error, message.view());
  if (done == 0 && !started) return false;
  if (from_paste && done > 0) {
    number.clear();
    number.append_uint(done);
    String status(allocator());
    format_message(&status, done == 1 ? "Pasted %1 item" : "Pasted %1 items", number.view());
    status_.set_text(status.view());
  }
  return true;
}

// The selected item's properties, or the folder's when nothing is selected.
bool FileExplorer::show_properties() {
  if (view_.selection_count() > 1) return false;   // (one sheet shows one object; the command is disabled for several)
  const int32_t item = single_item();
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
  const bool control = (args.modifiers & hardware::modifier::kControl) != 0;
  if (control && args.usage == hardware::key::kA + 23) {   // Ctrl+X
    cut_selected();
    return true;
  }
  if (control && args.usage == hardware::key::kA + 2) {   // Ctrl+C
    copy_selected();
    return true;
  }
  if (control && args.usage == hardware::key::kA + 21) {   // Ctrl+V
    paste();
    return true;
  }
  if (control && args.usage == hardware::key::kA) {   // Ctrl+A
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
  if (control && args.usage == hardware::key::kA + 11) {  // Ctrl+L
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
  describe_failure(&message, "\"%1\" could not be opened because %2.", target.view(), e->last_status_ == Status::Ok ? Status::NotFound : e->last_status_);
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

// The list's menu is for the selection - or, opened on empty space, for the folder - and has only what applies enabled.
void FileExplorer::on_list_menu_opening(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  const bool on_items = e->view_.selection_count() > 0;
  if (on_items != e->list_menu_on_items_) e->build_list_menu(on_items);
  e->update_commands();
}

void FileExplorer::on_menu_opening(void* self, Control&) { static_cast<FileExplorer*>(self)->update_commands(); }

void FileExplorer::on_about(void* self, Control&) {
  static_cast<FileExplorer*>(self)->notify(NoticeKind::Information, "File Explorer shows the files and folders on this computer and on the network, and lets you open, copy, move, rename and delete them.",
                                           "About File Explorer");
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
  auto stays = [&](uint32_t i) {   // the volumes and mounted folders of the root stay
    const ItemIcon icon = e->view_.item_icon(i);
    return icon == ItemIcon::Drive || (e->path_.size() <= 1 && icon != ItemIcon::File);
  };
  if (stays(static_cast<uint32_t>(item))) return false;
  // A drag that starts inside the selection carries all of it.
  if (e->view_.selection_count() > 1 && e->view_.is_selected(static_cast<uint32_t>(item))) {
    for (uint32_t i = 0; i < e->view_.item_count(); ++i)
      if (e->view_.is_selected(i) && stays(i)) return false;
    return e->selected_paths(text) > 0;
  }
  String name(e->allocator());
  e->view_.item_text(static_cast<uint32_t>(item), 0, &name);
  return text->assign(e->path_.view()) && (e->path_.size() <= 1 || text->push_back('/')) && text->append(name.view());
}

// Where a drop at (x, y) of the frame goes: into the folder of the list it is on, otherwise into the folder shown.
bool FileExplorer::drop_folder(int32_t x, int32_t y, String* folder) const {
  if (!folder->assign(path_.view())) return false;
  const Point mine = window_origin(), theirs = view_.window_origin();
  const int32_t item = view_.item_at_point(x + mine.x - theirs.x, y + mine.y - theirs.y);
  if (item >= 0 && view_.item_icon(static_cast<uint32_t>(item)) == ItemIcon::Folder) {
    String name(allocator());
    view_.item_text(static_cast<uint32_t>(item), 0, &name);
    if ((path_.size() > 1 && !folder->push_back('/')) || !folder->append(name.view())) return false;
  }
  return true;
}

bool FileExplorer::accepts_drop(StringView text, int32_t, int32_t, const Control*) const {
  return fs_ && !text.empty() && text[0] == '/';
}

// A drop moves, as Cut and Paste do; with Ctrl held it copies, as Copy and Paste do.
void FileExplorer::on_drop(StringView text, int32_t x, int32_t y, const Control*) {
  String folder(allocator());
  if (!drop_folder(x, y, &folder)) return;
  const bool copy = window() && (window()->held_modifiers() & hardware::modifier::kControl);
  transfer_into(text, folder.view(), !copy, /*from_paste=*/false);
}

bool FileExplorer::move_into(StringView source, StringView folder) {
  if (!fs_) {
    notify(NoticeKind::Error, "There is no storage to move files on.");
    return false;
  }
  return transfer_into(source, folder, /*move=*/true, /*from_paste=*/false);
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
void FileExplorer::on_cut_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->cut_selected(); }
void FileExplorer::on_paste_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->paste(); }
void FileExplorer::on_select_all_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->select_all(); }
void FileExplorer::on_properties_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->show_properties(); }
void FileExplorer::on_copy_path(void* self, Control&) { static_cast<FileExplorer*>(self)->copy_selected_path(); }
bool FileExplorer::new_folder() { return create_new(true); }
bool FileExplorer::new_file() { return create_new(false); }

bool FileExplorer::create_new(bool folder) {
  if (!fs_) {
    notify(gui::NoticeKind::Error, "There is no storage to make it on.");
    return false;
  }
  const StringView base = folder ? StringView("New Folder") : StringView("New Text Document");
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
      notify(gui::NoticeKind::Error, folder ? "This folder already has too many folders called New Folder. Rename or delete some of them and try again."
                                            : "This folder already has too many documents called New Text Document. Rename or delete some of them and try again.");
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
    describe_failure(&message, folder ? "A folder could not be made in \"%1\" because %2." : "A document could not be made in \"%1\" because %2.",
                     path_.size() > 1 ? name_of(path_.view()) : StringView("the list of drives"), status);
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

bool FileExplorer::rename_selected() {
  const int32_t item = single_item();
  if (item < 0 || !selection_can_change()) return false;
  view_.select_item(item);   // (the item with the focus is the one whose name is edited)
  return view_.begin_rename();
}

bool FileExplorer::rename_item(StringView from, StringView to) {
  const StringView name = to.trimmed();
  if (name.empty() || name.equals(from)) return true;   // unchanged, or nothing typed: the name stays
  if (name.find('/') != StringView::npos) {
    String message(allocator());
    format_message(&message, "\"%1\" was not renamed because a name cannot contain a slash. Choose another name.", from);
    notify(gui::NoticeKind::Error, message.view());
    return false;
  }
  if (!fs_) return false;
  String old_path(allocator()), new_path(allocator());
  for (int which = 0; which < 2; ++which) {
    String& out = which == 0 ? old_path : new_path;
    out.assign(path_.view());
    if (path_.size() > 1) out.push_back('/');
    out.append(which == 0 ? from : name);
  }
  const Status status = fs_->rename(old_path.view(), new_path.view());
  if (status != Status::Ok) {
    String message(allocator());
    describe_failure(&message, "\"%1\" could not be renamed because %2.", from, status);
    notify(gui::NoticeKind::Error, message.view());
    return false;
  }
  fs_->flush();
  fire(files_changed_);
  refresh();
  for (uint32_t i = 0; i < view_.item_count(); ++i) {
    String text(allocator());
    view_.item_text(i, 0, &text);
    if (text.view().equals(name)) {
      view_.select_item(static_cast<int32_t>(i));
      break;
    }
  }
  return true;
}

// Delete asks first, because a deleted file cannot be brought back, and the button that leaves everything alone is the default one.
bool FileExplorer::delete_selected() {
  if (!selection_can_change() || !manager() || view_.renaming()) return false;
  const uint32_t count = selected_paths(&delete_paths_);
  if (count == 0) return false;
  String question(allocator()), shown(allocator());
  if (count == 1) {
    const int32_t item = single_item();
    const bool folder = item >= 0 && view_.item_icon(static_cast<uint32_t>(item)) != ItemIcon::File;
    shown_name(name_of(delete_paths_.view()), &shown);
    format_message(&question, "Delete \"%1\"?", shown.view());
    delete_dialog_.set_texts(title(), question.view(), folder ? "Everything in it will be deleted as well." : "It cannot be brought back afterwards.", "Delete", "Cancel");
  } else {
    shown.append_uint(count);
    format_message(&question, "Delete these %1 items?", shown.view());
    delete_dialog_.set_texts(title(), question.view(), "They cannot be brought back afterwards.", "Delete", "Cancel");
  }
  delete_dialog_.set_default_choice(gui::MessageDialog::Choice::Decline);
  return delete_dialog_.show(*manager());
}

// Removes a file, or a folder after what is in it. Stops at the first failure.
Status FileExplorer::remove_tree(StringView path, uint32_t depth) { return FileTransfer::remove_tree(allocator(), *fs_, path, depth); }

void FileExplorer::on_delete_menu(void* self, Control&) { static_cast<FileExplorer*>(self)->delete_selected(); }

void FileExplorer::on_delete_answer(void* self, Control&) {
  auto* e = static_cast<FileExplorer*>(self);
  if (!e->delete_dialog_.accepted() || !e->fs_) return;
  Status status = Status::Ok;
  StringView rest = e->delete_paths_.view(), failed;
  while (!rest.empty() && status == Status::Ok) {   // stops at the first that cannot be deleted
    failed = next_line(&rest);
    status = e->remove_tree(failed, 0);
  }
  e->fs_->flush();
  e->fire(e->files_changed_);
  e->refresh();
  if (status != Status::Ok) {
    String message(e->allocator());
    describe_failure(&message, "\"%1\" could not be deleted because %2.", name_of(failed), status);
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
