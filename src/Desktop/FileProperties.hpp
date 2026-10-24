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

// The properties of a file or folder: a window with a table of what is known about it - name, location, kind, size, what a folder holds, whether
// it can be changed - and, for pictures, videos and sounds, what the format says: the size in pixels, how many frames, the length, the frame rate,
// the sample rate and the number of channels. The format is read from the file's contents (not its name); a file that is too big to read whole is
// still looked at where the format allows it (videos are read through a byte source, only their headers). The window is reached from the file
// explorer's context menu.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Gui/Controls/DataGrid.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Windows/Frame.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace desktop {

class FileProperties : public gui::Frame {
 public:
  // Files above this size are not read whole to find out their picture size or sound length.
  static constexpr uint64_t kMaxInspectBytes = 32ull * 1024 * 1024;

  explicit FileProperties(allocators::IAllocator& allocator);

  // The filesystem the files are on (nullptr: none). It must outlive the window.
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  // Looks at the file or folder at `path` and fills the table. False (with the table saying why) if it does not exist.
  bool show(StringView path);
  StringView path() const { return path_.view(); }

  uint32_t row_count() const { return static_cast<uint32_t>(names_.count()); }
  StringView property(uint32_t row) const { return row < names_.count() ? names_[row].view() : StringView(); }
  StringView value(uint32_t row) const { return row < values_.count() ? values_[row].view() : StringView(); }
  // The value of the row called `name` (empty if there is none).
  StringView value_of(StringView name) const;
  gui::DataGrid& grid() { return grid_; }
  gui::Button& copy_button() { return copy_button_; }

 private:
  static void on_copy(void* self, gui::Control&);
  static void on_close(void* self, gui::Control&);
  void add_row(StringView name, StringView value);
  void clear();
  void add_folder_contents(StringView path);
  void add_media(StringView path, uint64_t size);
  bool read_whole(StringView path, uint64_t size, ByteBuffer* out);
  void add_image(ConstByteView data);
  void add_video(StringView path, ConstByteView head, uint64_t size);
  void add_audio(ConstByteView data);

  filesystem::IFileSystem* fs_ = nullptr;
  String path_;
  gui::DataGrid grid_;
  gui::Panel buttons_;
  gui::Button copy_button_, close_button_;
  List<String> names_, values_;
};

}  // namespace desktop
