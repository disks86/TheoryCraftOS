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

// The Windows and Linux applications installed on the system, found the way those systems' own menus find them, for the desktop's Start
// menu ("Google Chrome (Windows)", "Firefox (Linux)"):
//
//  - Windows: the shortcuts (.lnk, [MS-SHLLINK]) under C:\ProgramData\Microsoft\Windows\Start Menu\Programs and every user's
//    AppData\Roaming\...\Start Menu\Programs, sub-folders included. The entry is named after the shortcut file; the target comes from the
//    LinkInfo's local base path, else the item id list (this system's shell32 and Windows' own both), else the relative path. Uninstallers
//    are left out.
//  - Linux: the desktop entries (.desktop, freedesktop.org Desktop Entry Specification) under /usr/share/applications,
//    /usr/local/share/applications and root's ~/.local/share/applications. Type=Application entries with an Exec line that are not
//    NoDisplay or Hidden; the Name is the unlocalised one, the field codes (%f, %U, ...) are dropped from Exec.
//
// Only entries whose program is an ELF or PE file on the disk are listed. Paths are translated into the system tree (C:\ is /Windows, a
// Linux / is /Linux); a path that would leave its root (".." or another drive) is not used. The parsers take untrusted bytes and never read
// outside them.
#pragma once
#include <stdint.h>
#include "FileSystem/IFileSystem.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace desktop {

enum class AppPlatform : uint8_t { Windows, Linux };

// "Windows" or "Linux": what the Start menu puts in brackets after the name.
StringView app_platform_name(AppPlatform platform);

struct InstalledApp {
  explicit InstalledApp(allocators::IAllocator& allocator)
      : name(allocator), label(allocator), program(allocator), arguments(allocator), working_directory(allocator) {}
  InstalledApp(InstalledApp&&) = default;
  InstalledApp& operator=(InstalledApp&&) = default;

  AppPlatform platform = AppPlatform::Windows;
  String name;                // "Google Chrome"
  String label;               // what the Start menu shows: "Google Chrome (Windows)"
  String program;             // the executable in the system tree: "/Windows/Program Files/Google/Chrome/Application/chrome.exe"
  String arguments;           // the rest of the command line, quoted for the terminal's shell; may be empty
  String working_directory;   // where it starts, in the system tree
};

// The command line that starts `app` in the terminal's shell: the program (quoted when it needs to be), then the arguments. False if memory
// ran out.
[[nodiscard]] bool installed_app_command_line(const InstalledApp& app, String* out);
// Do the two describe the same application, started the same way?
bool same_installed_app(const InstalledApp& a, const InstalledApp& b);

// What a shortcut file says. Paths are Windows paths as stored ("C:\Program Files\..."); empty when absent.
struct ShellLinkInfo {
  explicit ShellLinkInfo(allocators::IAllocator& allocator) : target(allocator), relative_path(allocator), working_directory(allocator), arguments(allocator) {}
  String target;
  String relative_path;
  String working_directory;
  String arguments;
};
// False if `data` is not a well-formed shortcut. A shortcut with no target (one to a virtual folder) parses with `target` empty.
[[nodiscard]] bool parse_shell_link(ConstByteView data, ShellLinkInfo* out);

// What a desktop entry says, from its [Desktop Entry] group.
struct DesktopEntryInfo {
  explicit DesktopEntryInfo(allocators::IAllocator& allocator) : name(allocator), program(allocator), arguments(allocator), try_exec(allocator), path(allocator) {}
  String name;           // Name= (unlocalised)
  String program;        // the first word of Exec= (a Linux path or a bare name to look up)
  String arguments;      // the other words of Exec=, field codes removed, quoted for the terminal's shell
  String try_exec;       // TryExec=
  String path;           // Path= (the working directory)
  bool application = false;   // Type=Application
  bool hidden = false;        // NoDisplay=true or Hidden=true
};
// False if `text` has no [Desktop Entry] group, or its Exec line cannot be split (an unterminated quote).
[[nodiscard]] bool parse_desktop_entry(StringView text, DesktopEntryInfo* out);

// "C:\Program Files\App\app.exe" (or with forward slashes, or %ProgramFiles%\...) -> "/Windows/Program Files/App/app.exe". False for another
// drive, a UNC path, a relative path or one with ".." in it.
[[nodiscard]] bool windows_path_to_system(StringView windows_path, String* out);
// "/usr/bin/app" -> "/Linux/usr/bin/app". False for a relative path or one with ".." in it.
[[nodiscard]] bool linux_path_to_system(StringView linux_path, String* out);

// Finds the installed applications on `fs` and appends them to `out`, sorted by label (ignoring case), each label once. False only when
// memory ran out (what was found until then stays in `out`).
[[nodiscard]] bool find_installed_apps(filesystem::IFileSystem& fs, allocators::IAllocator& allocator, List<InstalledApp>* out);

}  // namespace desktop
