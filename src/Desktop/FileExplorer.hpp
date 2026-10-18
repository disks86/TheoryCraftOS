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

// The file explorer: a window showing the folders and files of a filesystem in a ListView, which the
// View menu switches between list, icon and details views. Double-click (or Enter on) a folder opens it, a file is passed on to on_file_opened, the
// Up button (or Backspace) goes to the parent. The window starts at the root of the filesystem it is given.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/BreadcrumbBar.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Kernel/Sync.hpp"

namespace desktop {

class FileExplorer : public gui::Frame {
 public:
  using Status = filesystem::Status;
  explicit FileExplorer(allocators::IAllocator& allocator);

  // The filesystem to browse (nullptr: none, the window says so). It must outlive the explorer. Goes to its root.
  void set_file_system(filesystem::IFileSystem* fs);
  // Shows the folder at `path` ("/" or "/a/b"). False (and the view stays where it was) if it cannot be read.
  bool open_folder(StringView path);
  // Reads the current folder again (volumes come and go, files change). A folder that is gone is replaced by the nearest one above it
  // that can still be read. Safe to call from any thread.
  bool refresh();
  // Opens the parent folder; false at the root.
  bool go_up();
  void set_view_mode(gui::ViewMode mode);
  // Double clicks are measured with this clock (see ListView::set_clock).
  void set_clock(const uint64_t* now_us) { view_.set_clock(now_us); }

  // Fired (see Gui::Handler) when a file is opened with Enter or a double click; opened_file() is its full path.
  void on_file_opened(gui::Handler handler) { file_opened_ = handler; }
  StringView opened_file() const { return opened_file_.view(); }

  // Ejecting a drive (File > Eject): the volume called `volume` - the selected folder when the explorer shows the root, else the volume the
  // current folder is on. The function does it, clears *succeeded if it failed, and returns what a notice should say (a static string or one that stays valid until the
  // next call; null for nothing); the explorer refreshes afterwards. Runs on the thread that fired the menu item.
  using EjectFunction = const char* (*)(void* context, StringView volume, bool* succeeded);   // *succeeded is true on entry
  void set_eject_handler(EjectFunction function, void* context) { eject_ = function; eject_context_ = context; }
  bool eject_selected();
  // File > New Folder / New File (also in the list's context menu): makes "New Folder" / "New File.txt" in the current folder - or "New Folder 2",
  // "New Folder 3", ... if the name is taken - and selects it. False (with a notice) if it could not be made, e.g. in the read-only root.
  bool new_folder();
  bool new_file();
  // File > Rename (F2, or Rename in the list's context menu): the selected item's name becomes a text box; Enter renames it. False if
  // nothing is selected. rename_item() does the renaming itself: `from` and `to` are names in the current folder; false (with a notice)
  // if it fails, e.g. because the name is taken or this place cannot be changed.
  bool rename_selected();
  bool rename_item(StringView from, StringView to);
  // File > Delete (the Delete key, or Delete in the list's context menu): asks "Delete <name>?" in delete_dialog(); on "Delete" the selected file
  // - or folder with everything in it - is removed. False if nothing is selected or the dialog cannot be shown.
  bool delete_selected();
  gui::MessageDialog& delete_dialog() { return delete_dialog_; }
  // The dialog that warnings, information and errors are shown in (nullptr: the status bar says them instead). It must outlive the explorer.
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }

  StringView path() const { return path_.view(); }
  gui::ListView& view() { return view_; }
  gui::BreadcrumbBar& address_bar() { return address_; }
  gui::MenuBar& menu_bar() { return menu_; }
  // The context menus: on the list (Open, Copy Path, the views, Refresh, Up) and on the address bar (Copy Address, Edit Address).
  gui::ContextMenu& list_menu() { return list_menu_; }
  gui::ContextMenu& address_menu() { return address_menu_; }
  // The View menu's entries; the one for the view in use is checked.
  gui::MenuItem* list_item() { return list_item_; }
  gui::MenuItem* icons_item() { return icons_item_; }
  gui::MenuItem* details_item() { return details_item_; }
  gui::Label& status_label() { return status_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_navigate(void* self, gui::Control&);
  static void on_list_menu_opening(void* self, gui::Control&);
  static void on_copy_address(void* self, gui::Control&);
  static void on_view_list(void* self, gui::Control&);
  static void on_view_icons(void* self, gui::Control&);
  static void on_view_details(void* self, gui::Control&);
  static void on_open_item(void* self, gui::Control&);
  static void on_copy_path(void* self, gui::Control&);
  static void on_eject_menu(void* self, gui::Control&);
  static void on_new_folder(void* self, gui::Control&);
  static void on_new_file(void* self, gui::Control&);
  static void on_rename_menu(void* self, gui::Control&);
  static void on_delete_menu(void* self, gui::Control&);
  static void on_delete_answer(void* self, gui::Control&);
  Status remove_tree(StringView path, uint32_t depth);
  static void on_renamed(void* self, gui::Control&);
  bool create_new(bool folder);
  static void on_close_window(void* self, gui::Control&);
  static void on_up_menu(void* self, gui::Control&);
  static void on_refresh_menu(void* self, gui::Control&);
  static void on_address_menu(void* self, gui::Control&);
  bool copy_selected_path();
  static void on_activated(void* self, gui::Control&);
  static void on_selection(void* self, gui::Control&);
  bool load(StringView path);   // (with navigation_ held)
  void activate_selected();
  void update_status();
  // Warnings, information and errors go to the notice dialog (or the status bar when there is none to show it in).
  void notify(gui::NoticeKind kind, StringView text) {
    if (!gui::NoticeDialog::post(notices_, manager(), kind, text)) status_.set_text(text);
  }

  gui::MenuBar menu_;
  gui::ContextMenu list_menu_, address_menu_;
  gui::MenuItem *ctx_open_ = nullptr, *ctx_rename_ = nullptr, *ctx_delete_ = nullptr, *ctx_copy_ = nullptr, *ctx_up_ = nullptr, *ctx_list_ = nullptr, *ctx_icons_ = nullptr, *ctx_details_ = nullptr;
  gui::Panel toolbar_, status_bar_;
  gui::BreadcrumbBar address_;
  gui::Label status_;
  gui::MenuItem *list_item_ = nullptr, *icons_item_ = nullptr, *details_item_ = nullptr;
  gui::ListView view_;
  kernel::Mutex navigation_;    // one folder is loaded at a time (a refresh from another thread must not interleave with a click)
  filesystem::IFileSystem* fs_ = nullptr;
  String path_;
  String scratch_;
  String opened_file_;
  gui::Handler file_opened_;
  gui::NoticeDialog* notices_ = nullptr;
  gui::MessageDialog delete_dialog_;
  String delete_path_;   // what the dialog asks about
  EjectFunction eject_ = nullptr;
  void* eject_context_ = nullptr;
};

}  // namespace desktop
