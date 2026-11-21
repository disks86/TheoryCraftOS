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
#include "Desktop/AppMan/SetupHost.hpp"
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
//                     3. choose the installer for this computer (x64, else neutral, else a 32-bit Inno Setup or Nullsoft setup program) among the kinds that need no
//                        setup program to be run:
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
// A package that comes as a setup program is installed by running that program, where there is a way to run one (an ISetupHost):
//
//   install:  steps 1 and 2 as above; then the installer of the kinds "inno" (Inno Setup), "nullsoft" (NSIS) or "exe" is downloaded into
//             C:\Windows\Temp\WinGet\<identifier>.<version> and checked (its SHA-256; that it is a program for this computer's processor), and run with
//             the switches that make it ask nothing: the manifest's InstallerSwitches (Silent, then Custom), else the usual ones of its kind. It has
//             succeeded if it ends with 0 or one of the manifest's InstallerSuccessCodes. The registry's Uninstall entries before and after tell what
//             it registered (the manifest's ProductCode or display name pick among several), and the record - a file of the App Manager's own,
//             /AppStore/AppMan/Winget/<identifier>.txt, since this system's registry does not outlast a restart - keeps that entry's uninstall command.
//   update:   the same with the new version's setup program, which replaces the old version itself. A package stays of the kind it was installed as.
//   remove:   runs the recorded uninstall command (with the quiet switches of its kind, unless it is the registry's QuietUninstallString), then
//             forgets the package.
//
// A package that is a Windows Installer package ("msi", "wix") is not a program: it is a description (tables of files, registry values, shortcuts and a
// list of actions) that this system installs by itself, with the engine of Msi/MsiInstaller.hpp, where there is an ISetupHost for the registry and the
// programs the package runs:
//
//   install:  steps 1 and 2; the .msi is downloaded into C:\Windows\Temp\WinGet\<identifier>.<version>, checked, and its execute sequence is run with the
//             properties the manifest's switches set (NAME=value). An older version that is another product is removed first. The package is then
//             kept as C:\Windows\Installer\<product code>.msi and the record names it; what the engine skipped (DLL custom actions, services, ...)
//             is in the record as notes. A failure takes away again what was put in place.
//   remove:   runs the kept package's sequence as a removal, then forgets the package.
//
// Packages of other kinds (MSIX, WiX Burn, ...), setup programs this system cannot run (a 32-bit Nullsoft or Inno Setup program is run; a 32-bit
// "exe" one is not: docs/Wow64.md), and "exe" installers without a silent switch are refused: Result::NotSupported, with the reason in the detail. Calls block:
// run them on a thread of their own.
namespace desktop::appman {

struct WingetNestedFile {
  explicit WingetNestedFile(allocators::IAllocator& allocator) : path(allocator), alias(allocator) {}
  String path;       // RelativeFilePath: where the program is in the archive
  String alias;      // PortableCommandAlias (may be empty)
};

struct WingetInstallerEntry {
  explicit WingetInstallerEntry(allocators::IAllocator& allocator)
      : architecture(allocator), type(allocator), nested_type(allocator), url(allocator), sha256(allocator), nested(allocator), silent(allocator), custom(allocator),
        product_code(allocator), display_name(allocator), success_codes(allocator) {}
  String architecture;      // "x64", "x86", "arm64", "neutral", ...
  String type;              // InstallerType, the manifest's own if the installer names none: "portable", "zip", "inno", "msi", ...
  String nested_type;       // NestedInstallerType (of a zip)
  String url;
  String sha256;            // hex
  List<UniquePointer<WingetNestedFile>> nested;
  // Of a setup program:
  String silent;            // InstallerSwitches.Silent (else SilentWithProgress): the switches that make it ask nothing ("" if the manifest has none)
  String custom;            // InstallerSwitches.Custom: more switches, always given
  String product_code;      // ProductCode (else the first AppsAndFeaturesEntries'): the name of its Uninstall key
  String display_name;      // the first AppsAndFeaturesEntries' DisplayName
  List<uint32_t> success_codes;      // InstallerSuccessCodes: exit codes other than 0 that mean it worked
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

  // Which kinds of installer may be chosen: the ones this package manager puts in place itself, the ones that are setup programs.
  enum Kinds : uint32_t { kNative = 1, kSetup = 2 };

  // `setup` (nullptr: packages that come as setup programs are refused) must outlive this.
  WingetInstaller(allocators::IAllocator& allocator, filesystem::IFileSystem& files, INetwork& network, ISetupHost* setup = nullptr)
      : allocator_(allocator), files_(files), network_(network), setup_(setup) {}

  // `ignore_checksum`: install the download even if its SHA-256 is not the manifest's (the user's choice after Result::BadChecksum, as winget's
  // --ignore-security-hash). Only a download over HTTPS is taken that way (may_ignore_checksum); everything else is checked as always.
  [[nodiscard]] Result install(const WingetPackage& package, Progress* progress, String* detail, bool ignore_checksum = false);
  [[nodiscard]] Result update(const WingetPackage& package, Progress* progress, String* detail, bool ignore_checksum = false);
  // Whether a download from `url` that failed its checksum may be installed anyway, if the user says so.
  static bool may_ignore_checksum(StringView url) { return url.starts_with("https://"); }
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
  // The installer of the manifest this computer can use and this package manager can install, among `kinds`, or nullptr; *why then says what the
  // package is instead. One that needs no setup program is taken before one that does.
  static const WingetInstallerEntry* choose_installer(const WingetManifest& manifest, uint32_t kinds, String* why);
  // A Windows command line as its program and its arguments ("C:\Program Files\App\unins000.exe" /SILENT; a program with spaces in its path and no
  // quotes around it ends at its ".exe"). With `program` null the whole text is arguments. False if memory ran out or there is no program.
  [[nodiscard]] static bool split_command_line(allocators::IAllocator& allocator, StringView text, String* program, List<String>* arguments);
  // An archive member's name (or a manifest's RelativeFilePath) as a path under the package's folder: '\' becomes '/'. False for one that would leave
  // the folder or that Windows could not hold (an absolute path, a drive, "..", a control character, one of : * ? " < > |, a name ending in a dot).
  [[nodiscard]] static bool safe_member_path(StringView raw, String* out);

 private:
  struct Program;
  Result place(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool was_installed, bool ignore_checksum, Progress* progress, String* detail);
  Result unpack(StringView archive, StringView folder, String* detail);
  struct SetupRecord;
  Result place_setup(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool ignore_checksum, Progress* progress, String* detail);
  Result remove_setup(StringView id, const SetupRecord& record, String* detail);
  Result place_msi(const WingetManifest& manifest, const WingetInstallerEntry& entry, bool ignore_checksum, Progress* progress, String* detail);
  Result run_msi_removal(StringView package, StringView upgrading, String* detail);
  bool read_setup_record(StringView id, WingetInstalled* app, SetupRecord* record) const;
  bool setup_record_path(StringView id, String* out) const;
  bool is_for_this_processor(StringView path) const;
  bool setup_program_runs(StringView path, StringView type) const;
  bool log_tail(StringView path, String* out) const;
  bool folder_of(StringView id, String* out) const;
  bool read_record(StringView id, WingetInstalled* app, List<String>* shortcuts) const;
  void remove_shortcuts(const List<String>& shortcuts);
  bool clear_folder(StringView folder, StringView keep);

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  INetwork& network_;
  ISetupHost* setup_;
};

}  // namespace desktop::appman
