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

// What a program that asks for a common dialog (Windows comdlg32: the Open and Save file dialogs, the colour chooser) gets from the screen it runs on. The kernel
// side of the request (WindowsDialogs.cpp) turns the program's structures into this, with paths of the OS file system, and the desktop shows the dialog.
#pragma once
#include <stdint.h>
#include "Utility/FixedString.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace process {

struct CommonDialogRequest {
  enum class Kind : uint8_t { OpenFile, SaveFile, Color };
  static constexpr uint32_t kMaxFilters = 8;
  struct Filter {
    FixedString<128> name;           // as shown in the "Files of type" list
    FixedString<128> extensions;     // without dots, separated by ';' ("txt;md"); "*" is every file (a list too long for this is "*")
  };

  explicit CommonDialogRequest(allocators::IAllocator& allocator) : start_path(allocator) {}

  Kind kind = Kind::OpenFile;
  FixedString<128> title;      // empty: the dialog's usual title
  String start_path;      // a folder or a file of the OS file system ("" : the root)
  Filter filters[kMaxFilters];
  uint32_t filter_count = 0;
  uint32_t filter_index = 0;      // the filter shown first (0-based)
  uint32_t color = 0;             // Color: the colour shown first, 0x00RRGGBB
};

struct CommonDialogReply {
  explicit CommonDialogReply(allocators::IAllocator& allocator) : path(allocator) {}

  bool accepted = false;
  String path;                // OpenFile, SaveFile: the OS file system path that was picked
  uint32_t filter_index = 0;      // the filter that was chosen (0-based)
  uint32_t color = 0;             // Color: the colour that was chosen, 0x00RRGGBB
};

// The screen's side. run() blocks the calling (program) thread until the user has answered; false means the dialog could not be shown (no screen, or another
// dialog is open), which is not the same as the user cancelling it.
class ICommonDialogs {
 public:
  virtual bool run(const CommonDialogRequest& request, CommonDialogReply* reply) = 0;

 protected:
  ICommonDialogs() = default;
  ~ICommonDialogs() = default;
};

}  // namespace process
