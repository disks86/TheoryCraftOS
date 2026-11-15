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
#include "Desktop/AppMan/AppManInstaller.hpp"
#include "Desktop/AppMan/WingetCatalog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

// Installs, updates and removes the Windows applications of winget's catalog (WingetCatalog.hpp) in the Windows world (`files` is the OS file system;
// C:\ is /Windows):
//
//   install(package): 1. fetch the package's version list (versionData.mszyml: YAML packed with MSZIP) and check it against the SHA-256 the index has
//                        for it; find the latest version's manifest in it;
//                     2. fetch that manifest and check it against the SHA-256 the version list has for it. It says what to download for which kind
//                        of computer, the SHA-256 of that, and how it is installed;
//                     3. choose the installer for this computer (x64, else neutral, else x86) among the kinds that need no setup program to be run:
//                        a "portable" program (one .exe) or a "zip" archive of portable programs;
//                     4. download it into the package's folder (through a .part file) and check its SHA-256 - so every byte that is installed is the
//                        one the signed-off catalog names;
//                     5. put the program (or unpack the archive) into C:\Program Files\WinGet\Packages\<identifier>_Microsoft.Winget.Source_8wekyb3d8bbwe,
//                        where winget itself puts portable packages installed for all users, write a shortcut for each program into the Start menu
//                        (C:\ProgramData\Microsoft\Windows\Start Menu\Programs) - which is what puts it in the desktop's Start menu as
//                        "Name (Windows)" - and a record of what was installed (kRecordName) into the folder.
//   update(package):  nothing if the installed version is the catalog's; else steps 1 to 5, the old files going only once the new download has been
//                     checked.
//   remove(id):       the shortcuts and the folder, gone.
//
// Packages that come as a setup program (Inno Setup, NSIS, MSI, MSIX, ...) are refused: Result::NotSupported, with the kind in the detail. Calls
// block: run them on a thread of their own.
namespace desktop::appman {

struct WingetNestedFile {
  explicit WingetNestedFile(allocators::IAllocator& allocator) : path(allocator), alias(allocator) {}
  String path;       // RelativeFilePath: where the program is in the archive
  String alias;      // PortableCommandAlias (may be empty)
};

struct WingetInstallerEntry {
  explicit WingetInstallerEntry(allocators::IAllocator& allocator) : architecture(allocator), type(allocator), nested_type(allocator), url(allocator), sha256(allocator), nested(allocator) {}
  String architecture;      // "x64", "x86", "arm64", "neutral", ...
  String type;              // InstallerType, the manifest's own if the installer names none: "portable", "zip", "inno", "msi", ...
  String nested_type;       // NestedInstallerType (of a zip)
  String url;
  String sha256;            // hex
  List<UniquePointer<WingetNestedFile>> nested;
};

struct WingetManifest {
  explicit WingetManifest(allocators::IAllocator& allocator) : id(allocator), name(allocator), version(allocator), command(allocator), installers(allocator) {}
  String id, name, version;
  String command;           // the first of Commands (may be empty)
  List<UniquePointer<WingetInstallerEntry>> installers;
};

struct WingetInstalled {
  explicit WingetInstalled(allocators::IAllocator& allocator) : id(allocator), name(allocator), version(allocator) {}
  WingetInstalled(WingetInstalled&&) = default;
  WingetInstalled& operator=(WingetInstalled&&) = default;
  String id, name, version;
};

class WingetInstaller {
 public:
  static constexpr StringView kRecordName = "winget-install.txt";
  static constexpr StringView kFolderSuffix = "_Microsoft.Winget.Source_8wekyb3d8bbwe";
  static constexpr uint64_t kMaxVersionDataBytes = 8ull << 20, kMaxVersionListBytes = 64ull << 20, kMaxManifestBytes = 4ull << 20;
  static constexpr uint64_t kMaxMemberBytes = 1ull << 30;       // one file of an archive (it is unpacked in memory)
  static constexpr uint64_t kMaxArchiveBytes = 16ull << 30;     // all of them together

  WingetInstaller(allocators::IAllocator& allocator, filesystem::IFileSystem& files, INetwork& network) : allocator_(allocator), files_(files), network_(network) {}

  [[nodiscard]] Result install(const WingetPackage& package, Progress* progress, String* detail);
  [[nodiscard]] Result update(const WingetPackage& package, Progress* progress, String* detail);
  [[nodiscard]] Result remove(StringView id, String* detail);
  // Steps 1 and 2: the manifest of the package's latest version, fetched and checked.
  [[nodiscard]] Result resolve(const WingetPackage& package, WingetManifest* manifest, String* detail);

  bool installed(StringView id) const;
  // The packages installed here (every folder of winget's with a record), by name.
  [[nodiscard]] bool list_installed(List<WingetInstalled>* apps) const;

  // The text of a .mszyml file: a header, then blocks of at most 32 KiB, each deflated with the ones before it as its dictionary. False if it is damaged
  // or would be longer than `max_bytes`.
  [[nodiscard]] static bool decode_mszyml(allocators::IAllocator& allocator, ConstByteView packed, uint64_t max_bytes, String* text);
  // Where the manifest of `version` (else of the first version listed) is, and its SHA-256, out of a version list.
  [[nodiscard]] static bool find_version(allocators::IAllocator& allocator, StringView version_list, StringView version, String* found_version, String* path, String* sha256);
  [[nodiscard]] static bool parse_manifest(allocators::IAllocator& allocator, StringView yaml, WingetManifest* manifest);
  // The installer of the manifest this computer can use and this package manager can install, or nullptr; *why then says what the package is instead.
  static const WingetInstallerEntry* choose_installer(const WingetManifest& manifest, String* why);
  // An archive member's name (or a manifest's RelativeFilePath) as a path under the package's folder: '\' becomes '/'. False for one that would leave
  // the folder or that Windows could not hold (an absolute path, a drive, "..", a control character, one of : * ? " < > |, a name ending in a dot).
  [[nodiscard]] static bool safe_member_path(StringView raw, String* out);

 private:
  struct Program;
  Result place(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool was_installed, Progress* progress, String* detail);
  Result unpack(StringView archive, StringView folder, String* detail);
  bool folder_of(StringView id, String* out) const;
  bool read_record(StringView id, WingetInstalled* app, List<String>* shortcuts) const;
  void remove_shortcuts(const List<String>& shortcuts);
  bool clear_folder(StringView folder, StringView keep);

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  INetwork& network_;
};

}  // namespace desktop::appman
