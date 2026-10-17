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
// toolbar switches between list, icon and details views. Double-click (or Enter on) a folder opens it, the
// Up button (or Backspace) goes to the parent. The window starts at the root of the filesystem it is given.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/ListView.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/RadioButton.hpp"
#include "Gui/Windows/Frame.hpp"

namespace desktop {

class FileExplorer : public gui::Frame {
 public:
  explicit FileExplorer(allocators::IAllocator& allocator);

  // The filesystem to browse (nullptr: none, the window says so). It must outlive the explorer. Goes to its root.
  void set_file_system(filesystem::IFileSystem* fs);
  // Shows the folder at `path` ("/" or "/a/b"). False (and the view stays where it was) if it cannot be read.
  bool open_folder(StringView path);
  // Opens the parent folder; false at the root.
  bool go_up();
  void set_view_mode(gui::ViewMode mode);
  // Double clicks are measured with this clock (see ListView::set_clock).
  void set_clock(const uint64_t* now_us) { view_.set_clock(now_us); }

  StringView path() const { return path_.view(); }
  gui::ListView& view() { return view_; }
  gui::Button& up_button() { return up_; }
  gui::RadioButton& list_button() { return list_button_; }
  gui::RadioButton& icons_button() { return icons_button_; }
  gui::RadioButton& details_button() { return details_button_; }
  gui::Label& status_label() { return status_; }
  gui::Label& path_label() { return path_label_; }

  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_up(void* self, gui::Control&);
  static void on_mode(void* self, gui::Control& sender);
  static void on_activated(void* self, gui::Control&);
  static void on_selection(void* self, gui::Control&);
  void activate_selected();
  void update_status();

  gui::RadioGroup group_;
  gui::Panel toolbar_, status_bar_;
  gui::Button up_;
  gui::Label path_label_, status_;
  gui::RadioButton list_button_, icons_button_, details_button_;
  gui::ListView view_;
  filesystem::IFileSystem* fs_ = nullptr;
  String path_;
  String scratch_;
};

}  // namespace desktop
