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
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Program/AppRunScript.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

// Installs, updates and removes AppImages the way AppMan does, into the Linux world (`files` is the OS file system; the paths below are in /Linux):
//
//   install(name):  1. fetch AppMan's install script for it (AppManCatalog.hpp: kScriptsUrl + name);
//                   2. work out what the script would download: its variable assignments up to and including the `version=$(curl ... | sed ... | grep ...
//                      | head -1)` line run in the built-in shell (evaluate_script_variable), whose curl fetches through `network` - the script itself
//                      is never run;
//                   3. download that into /opt/<name>/<name> (through a .part file, so an interrupted download leaves no half program) and check that it
//                      is an AppImage (type 1 or 2; a packed download, .zip or .tar, is refused);
//                   4. write /opt/<name>/version (the link it came from, which is what updates compare) and /opt/<name>/remove (AppMan's uninstall
//                      script), take the desktop entry and the icon out of the AppImage's image, and write
//                      /usr/local/share/applications/<name>-AM.desktop (Exec= the AppImage, Icon= /opt/<name>/icons/<name>) - which is what puts the
//                      program in the Start menu - and /usr/local/bin/<name>, a link to it where the file system has links.
//   update(name):   steps 1-2 again; if the link differs from /opt/<name>/version, steps 3-4.
//   remove(name):   everything above, gone.
//
// The layout is AppMan's own, so a real AppMan (or a person) finds the programs where it expects them. Calls block: run them on a thread of their own.
namespace desktop::appman {

// What the installer reaches the network through.
class INetwork : public IScriptNetwork {
 public:
  // Where downloaded bytes go, as they come. Returning false stops the download.
  using Sink = bool (*)(void* context, ConstByteView data);
  // Streams the body of a GET of `url` (redirects followed) into `sink`. *total gets the size if the server says it (else 0) before the first bytes.
  virtual bool download(StringView url, Sink sink, void* context, uint64_t* total) = 0;

 protected:
  ~INetwork() = default;
};

enum class Result : uint8_t {
  Ok,
  InvalidName,
  NoScript,          // AppMan has no install script of that name (or it could not be fetched)
  NoDownload,        // the script's link could not be worked out
  DownloadFailed,
  NotAnAppImage,     // what the script downloads is something else (a packed archive, a plain program)
  NotInstalled,
  UpToDate,
  Cancelled,
  FileError,         // the Linux world could not be written (full, read only)
  OutOfMemory,
};
StringView result_name(Result result);

// Progress of a download, readable from another thread while it runs; `cancel` stops it.
struct Progress {
  uint64_t done = 0, total = 0;      // bytes (total 0: unknown)
  bool cancel = false;
  // Called on the downloading thread as the bytes come (about every 256 KiB, and at the end).
  void (*changed)(void* context, const Progress& progress) = nullptr;
  void* context = nullptr;
};

struct InstalledApp {
  explicit InstalledApp(allocators::IAllocator& allocator) : name(allocator), version(allocator) {}
  String name, version;      // version: the link it was installed from
};

class Installer {
 public:
  Installer(allocators::IAllocator& allocator, filesystem::IFileSystem& files, INetwork& network) : allocator_(allocator), files_(files), network_(network) {}

  [[nodiscard]] Result install(StringView name, Progress* progress, String* detail);
  [[nodiscard]] Result update(StringView name, Progress* progress, String* detail);
  [[nodiscard]] Result remove(StringView name, String* detail);
  // The link the install script would download now (steps 1-2 of install), without downloading it.
  [[nodiscard]] Result resolve(StringView name, String* url, String* detail);

  bool installed(StringView name) const;
  // The programs AppMan's layout holds (every /opt/<name> with a version and a remove file), by name.
  [[nodiscard]] bool list_installed(List<InstalledApp>* apps) const;

  // What step 2 reads of an install script: its assignments up to the first `version=` (a line ending in a backslash goes on in the next one). False if
  // there is no version line.
  [[nodiscard]] static bool download_lines(StringView script, String* out);

 private:
  Result fetch_and_install(StringView name, StringView url, Progress* progress, String* detail);
  Result install_launcher(StringView name, String* detail);
  bool remove_tree(StringView path, uint32_t depth);
  bool write_file(StringView path, StringView text);
  bool read_file(StringView path, uint64_t limit, String* text) const;

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem& files_;
  INetwork& network_;
};

}  // namespace desktop::appman
