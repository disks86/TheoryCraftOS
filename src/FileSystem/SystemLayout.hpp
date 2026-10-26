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

// The layout of the system filesystem (the tree SystemFileSystem presents). Code that needs one of these places uses the names here
// instead of spelling the path out, so the layout can be followed and changed in one place. See docs/FileSystemLayout.md.
//
//   /                  the root: the primary partition (the first writable, built-in, non-boot volume)
//   /EFI               a mount of the EFI system partition (not a folder of the primary partition)
//   /Desktop           everything on the user's desktop
//   /Desktop/Downloads where the browser puts what it downloads
//   /Desktop/Documents spreadsheets, text files and the like
//   /Desktop/Pictures  image files
//   /Desktop/Video     video files
//   /Desktop/Audio     audio files such as music
//   /Windows           the root of Windows user-mode processes: they see its contents as the C: drive
//   /Linux             the root of Linux processes: they see its contents as /
//   /AppStore          one sub-folder per application for the files it keeps (the browser's cache is /AppStore/Browser/Cache)
//
// Volumes other than those above (USB sticks, network shares) are folders of "/" named by their label.
#pragma once
#include "FileSystem/IFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace filesystem::layout {

inline constexpr const char* kEfi = "/EFI";
inline constexpr const char* kDesktop = "/Desktop";
inline constexpr const char* kDownloads = "/Desktop/Downloads";
inline constexpr const char* kDocuments = "/Desktop/Documents";
inline constexpr const char* kPictures = "/Desktop/Pictures";
inline constexpr const char* kVideo = "/Desktop/Video";
inline constexpr const char* kAudio = "/Desktop/Audio";
inline constexpr const char* kWindows = "/Windows";
inline constexpr const char* kLinux = "/Linux";
inline constexpr const char* kAppStore = "/AppStore";

// The system database: where the OS and its programs keep their settings (the "System" application's folder in the AppStore).
inline constexpr const char* kSystemDatabase = "/AppStore/System/system.db";

// The name the EFI system partition is mounted under (the folder "/EFI").
inline constexpr const char* kEfiMountName = "EFI";

// Every folder of the layout that lives on the primary partition, parents before children. "/EFI" is not one: it is a mount.
inline constexpr const char* kFolders[] = {kDesktop, kDownloads, kDocuments, kPictures, kVideo, kAudio, kWindows, kLinux, kAppStore};

// Creates the folders above in `fs` (the primary partition's own filesystem, or one standing in for it) when they are missing. Folders that
// exist stay as they are, and so does everything else in them. False if one could not be made (read-only medium, no space).
inline bool create_folders(filesystem::IFileSystem& fs) {
  bool ok = true;
  for (const char* folder : kFolders) {
    const Status status = fs.create_directory(folder);
    if (status != Status::Ok && status != Status::AlreadyExists) ok = false;
  }
  return ok;
}

// "/AppStore/<app>" or "/AppStore/<app>/<sub>" in *path (replacing what it held). `app` is one folder name ("Browser"); `sub` may be a
// path ("Cache", "Cache/images") or empty. False if `app` is not a valid folder name, or on out-of-memory.
inline bool app_store_path(String* path, StringView app, StringView sub = StringView()) {
  if (!filesystem::valid_name(app)) return false;
  if (!path->assign(kAppStore) || !path->push_back('/') || !path->append(app)) return false;
  if (sub.empty()) return true;
  return path->push_back('/') && path->append(sub);
}

// Creates the folder "/AppStore/<app>" (and "/AppStore" itself if it is missing) in `fs`. Call once when an application first needs to
// store something; an existing folder is fine.
inline Status create_app_folder(filesystem::IFileSystem& fs, StringView app, allocators::IAllocator& allocator) {
  String path(allocator);
  if (!app_store_path(&path, app)) return Status::InvalidPath;
  const Status parent = fs.create_directory(kAppStore);
  if (parent != Status::Ok && parent != Status::AlreadyExists) return parent;
  const Status status = fs.create_directory(path.view());
  return status == Status::AlreadyExists ? Status::Ok : status;
}

}  // namespace filesystem::layout
