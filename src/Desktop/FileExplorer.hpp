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
#include "Gui/Controls/Menu.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Kernel/Sync.hpp"

namespace desktop {

class FileExplorer : public gui::Frame {
 public:
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

  StringView path() const { return path_.view(); }
  gui::ListView& view() { return view_; }
  gui::BreadcrumbBar& address_bar() { return address_; }
  gui::MenuBar& menu_bar() { return menu_; }
  // The View menu's entries; the one for the view in use is checked.
  gui::MenuItem* list_item() { return list_item_; }
  gui::MenuItem* icons_item() { return icons_item_; }
  gui::MenuItem* details_item() { return details_item_; }
  gui::Label& status_label() { return status_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_navigate(void* self, gui::Control&);
  static void on_view_list(void* self, gui::Control&);
  static void on_view_icons(void* self, gui::Control&);
  static void on_view_details(void* self, gui::Control&);
  static void on_open_item(void* self, gui::Control&);
  static void on_copy_path(void* self, gui::Control&);
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

  gui::MenuBar menu_;
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
};

}  // namespace desktop
