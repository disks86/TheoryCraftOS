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

#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Executable/AppImage.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "FileSystem/Mount/GraftFileSystem.hpp"
#include "FileSystem/Squash/SquashFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Program/AppRunScript.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

// Is the file at `path` an AppImage (which type)? None if it cannot be read or is not one.
exe::AppImageType appimage_type(filesystem::IFileSystem& files, StringView path);

// Running an AppImage the way its runtime would, without the runtime: the SquashFS image appended to the file is mounted read only at
// /tmp/.mount_<name><id> of the Linux world, for this program alone (a GraftFileSystem over the OS file system), the variables the runtime sets are
// set (APPIMAGE, APPDIR, ARGV0, OWD), and the AppRun at the image's root is worked out (AppRunScript.hpp) to the program it starts. If AppRun leads
// nowhere, the program of the image's desktop entry is started instead, with the environment the classic AppRun binary would give it.
// An Electron program (chrome-sandbox next to it) is given --no-sandbox: this OS has no user namespaces nor a setuid helper for Chromium's sandbox.
//
// The session owns the mount: keep it alive while the program runs (its files are read from the image as the program asks).
class AppImageSession {
 public:
  explicit AppImageSession(allocators::IAllocator& allocator)
      : allocator_(allocator), mount_(allocator), program_(allocator), launch_(allocator), argument_views_(allocator), environment_views_(allocator) {}
  AppImageSession(const AppImageSession&) = delete;
  AppImageSession& operator=(const AppImageSession&) = delete;

  // `files` is the OS file system, `path` the AppImage in it. `environment` is what the program would get without the AppImage ("NAME=value": the
  // standard variables and any extra ones). False with *message saying why if it cannot be started.
  [[nodiscard]] bool prepare(filesystem::IFileSystem& files, StringView path, StringView working_directory, ArrayView<const StringView> arguments,
                             ArrayView<const StringView> environment, String* message);

  // What to run, once prepared: in this file system (the OS's with the image grafted on), the program at this path, with these arguments and
  // variables (to be added to, and replace, the standard ones).
  filesystem::IFileSystem& files() { return *graft_; }
  StringView program_path() const { return program_.view(); }
  ArrayView<const StringView> arguments() const { return argument_views_.view(); }
  ArrayView<const StringView> environment() const { return environment_views_.view(); }
  // Where the image is mounted, as the program sees it ("/tmp/.mount_App1x2y3z").
  StringView mount_point() const { return mount_.view(); }

 private:
  bool desktop_entry_fallback(StringView appdir, ArrayView<const StringView> arguments, ArrayView<const StringView> environment);

  allocators::IAllocator& allocator_;
  UniquePointer<filesystem::SquashFileSystem> image_;
  UniquePointer<filesystem::GraftFileSystem> graft_;
  String mount_, program_;
  AppRunLaunch launch_;
  List<StringView> argument_views_, environment_views_;
};

}  // namespace desktop
